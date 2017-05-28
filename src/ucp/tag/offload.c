/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2017.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "offload.h"
#include "eager.h"
#include "rndv.h"
#include <ucp/proto/proto.h>
#include <ucp/proto/proto_am.inl>
#include <ucp/core/ucp_context.h>
#include <ucp/core/ucp_request.inl>
#include <ucp/core/ucp_request.h>
#include <ucp/tag/tag_match.inl>
#include <ucs/datastruct/queue.h>
#include <ucs/sys/sys.h>

/* Tag consumed by the transport - need to remove it from expected queue */
void ucp_tag_offload_tag_consumed(uct_tag_context_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, recv.uct_ctx);
    ucp_context_t *ctx = req->recv.worker->context;
    ucs_queue_head_t *queue;

    queue = ucp_tag_exp_get_req_queue(&ctx->tm, req);
    ucs_queue_remove(queue, &req->recv.queue);
}

/* Message is scattered to user buffer by the transport, complete the request */
void ucp_tag_offload_completed(uct_tag_context_t *self, uct_tag_t stag,
                               uint64_t imm, size_t length, ucs_status_t status)
{
    ucp_request_t *req        = ucs_container_of(self, ucp_request_t, recv.uct_ctx);
    ucp_context_t *ctx        = req->recv.worker->context;
    ucp_worker_iface_t *iface = ucs_queue_head_elem_non_empty(&ctx->tm.offload_ifaces,
                                                              ucp_worker_iface_t, queue);

    req->recv.info.sender_tag = stag;
    req->recv.info.length     = length;

    ucp_request_memory_dereg(ctx, iface->rsc_index, req->recv.datatype,
                             &req->recv.state);
    ucp_request_complete_recv(req, status);
}

/* RNDV request matched by the transport. Need to proceed with AM based RNDV */
void ucp_tag_offload_rndv_cb(uct_tag_context_t *self, uct_tag_t stag,
                             const void *header, unsigned header_length,
                             ucs_status_t status)
{
    ucp_request_t *req        = ucs_container_of(self, ucp_request_t, recv.uct_ctx);
    ucp_context_t *ctx        = req->recv.worker->context;
    ucp_sw_rndv_hdr_t *sreq   = (ucp_sw_rndv_hdr_t*)header;
    ucp_worker_iface_t *iface = ucs_queue_head_elem_non_empty(&ctx->tm.offload_ifaces,
                                                              ucp_worker_iface_t, queue);
    ucp_rndv_rts_hdr_t rts;

    rts.sreq      = sreq->super;
    rts.super.tag = stag;
    rts.address   = 0; /* RNDV needs to be completed in SW */
    rts.size      = sreq->length;

    ucp_request_memory_dereg(ctx, iface->rsc_index, req->recv.datatype,
                             &req->recv.state);
    ucp_rndv_matched(req->recv.worker, req, &rts);
}

ucs_status_t ucp_tag_offload_unexp_rndv(void *arg, unsigned flags,
                                        uint64_t stag, const void *hdr,
                                        unsigned hdr_length,
                                        uint64_t remote_addr, size_t length,
                                        const void *rkey_buf)
{
    ucp_rndv_rts_hdr_t *rts = (ucp_rndv_rts_hdr_t*)(((ucp_tag_hdr_t*)hdr) - 1);
    ucp_worker_t *worker    = arg;
    void *rkey              = rts + 1;
    size_t len              = sizeof(*rts);
    ucp_ep_t *ep            = ucp_worker_get_reply_ep(worker, rts->sreq.sender_uuid);
    const uct_md_attr_t *md_attrs;
    size_t rkey_size;

    /* rts.req  should be alredy in place - it is sent by the sender.
     * Fill the rest of rts header and pass to common rts handler */
    if (rkey_buf) {
        /* Copy rkey before to fill rts, to avoid overriding rkey */
        md_attrs   = ucp_ep_md_attr(ep, ucp_ep_get_tag_lane(ep));
        rkey_size  = md_attrs->rkey_packed_size;
        memcpy(rkey, rkey_buf, rkey_size);
        len       += rkey_size;
        rts->flags = UCP_RNDV_RTS_FLAG_PACKED_RKEY | UCP_RNDV_RTS_FLAG_OFFLOAD;
        rts->size  = length;
    } else {
        ucs_assert(remote_addr == 0ul);
        /* This must be SW RNDV request. Take length from its header. */
        rts->size = ((ucp_sw_rndv_hdr_t*)hdr)->length;
    }

    rts->super.tag = stag;
    rts->address   = remote_addr;

    return ucp_rndv_rts_handler(arg, rts, len, flags, UCP_RECV_DESC_FLAG_OFFLOAD);
}

void ucp_tag_offload_cancel(ucp_context_t *ctx, ucp_request_t *req, int force)
{
    ucp_worker_iface_t *ucp_iface;
    ucs_status_t status;

    ucs_assert(req->flags & UCP_REQUEST_FLAG_OFFLOADED);

    ucp_iface = ucs_queue_head_elem_non_empty(&ctx->tm.offload_ifaces,
                                              ucp_worker_iface_t, queue);
    ucp_request_memory_dereg(ctx, ucp_iface->rsc_index, req->recv.datatype,
                             &req->recv.state);
    status = uct_iface_tag_recv_cancel(ucp_iface->iface, &req->recv.uct_ctx, force);
    if (status != UCS_OK) {
        ucs_error("Failed to cancel recv in the transport: %s",
                  ucs_status_string(status));
    }
}

int ucp_tag_offload_post(ucp_context_t *ctx, ucp_request_t *req)
{
    size_t length = req->recv.length;
    ucp_worker_iface_t *ucp_iface;
    ucs_status_t status;
    uct_iov_t iov;

    if (!UCP_DT_IS_CONTIG(req->recv.datatype)) {
        /* Non-contig buffers not supported yet. */
        return 0;
    }

    if ((ctx->config.tag_sender_mask & req->recv.tag_mask) !=
         ctx->config.tag_sender_mask) {
        /* Wildcard.
         * TODO add check that only offload capable iface present. In
         * this case can post tag as well. */
        return 0;
    }

    if (ctx->tm.sw_req_count) {
        /* There are some requests which must be completed in SW. Do not post
         * tags to HW until they are completed. */
        return 0;
    }

    ucp_iface = ucs_queue_head_elem_non_empty(&ctx->tm.offload_ifaces,
                                              ucp_worker_iface_t, queue);
    status = ucp_request_memory_reg(ctx, ucp_iface->rsc_index, req->recv.buffer,
                                    req->recv.length, req->recv.datatype,
                                    &req->recv.state);
    if (status != UCS_OK) {
        return 0;
    }

    req->recv.uct_ctx.tag_consumed_cb = ucp_tag_offload_tag_consumed;
    req->recv.uct_ctx.completed_cb    = ucp_tag_offload_completed;
    req->recv.uct_ctx.rndv_cb         = ucp_tag_offload_rndv_cb;

    iov.buffer = (void*)req->recv.buffer;
    iov.length = length;
    iov.memh   = req->recv.state.dt.contig.memh;
    iov.count  = 1;
    iov.stride = 0;
    status = uct_iface_tag_recv_zcopy(ucp_iface->iface, req->recv.tag,
                                      req->recv.tag_mask, &iov, 1,
                                      &req->recv.uct_ctx);
    if (status != UCS_OK) {
        /* No more matching entries in the transport. */
        return 0;
    }
    req->flags |= UCP_REQUEST_FLAG_OFFLOADED;
    ucs_trace_req("recv request %p (%p) was posted to transport (rsc %d)",
                  req, req + 1, ucp_iface->rsc_index);
    return 1;
}

static size_t ucp_tag_offload_pack_eager(void *dest, void *arg)
{
    ucp_request_t *req = arg;
    size_t length;

    length = ucp_dt_pack(req->send.datatype, dest, req->send.buffer,
                         &req->send.state, req->send.length);
    ucs_assert(length == req->send.length);
    return length;
}

static ucs_status_t ucp_tag_offload_eager_short(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_ep_t *ep       = req->send.ep;
    ucs_status_t status;

    req->send.lane = ucp_ep_get_tag_lane(ep);
    status         = uct_ep_tag_eager_short(ep->uct_eps[req->send.lane],
                                            req->send.tag, req->send.buffer,
                                            req->send.length);
    if (status == UCS_OK) {
        ucp_request_complete_send(req, UCS_OK);
    }
    return status;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_do_tag_offload_bcopy(uct_pending_req_t *self, uint64_t imm_data,
                         uct_pack_callback_t pack_cb)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_ep_t *ep       = req->send.ep;
    ssize_t packed_len;

    req->send.lane = ucp_ep_get_tag_lane(ep);
    packed_len     = uct_ep_tag_eager_bcopy(ep->uct_eps[req->send.lane],
                                            req->send.tag, imm_data,
                                            pack_cb, req);
    if (packed_len < 0) {
        return packed_len;
    }
    return UCS_OK;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_do_tag_offload_zcopy(uct_pending_req_t *self, uint64_t imm_data,
                         ucp_req_complete_func_t complete)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_ep_t *ep       = req->send.ep;
    size_t max_iov     = ucp_ep_config(ep)->tag.eager.max_iov;
    uct_iov_t *iov     = ucs_alloca(max_iov * sizeof(uct_iov_t));
    size_t iovcnt      = 0;
    ucs_status_t status;
    ucp_dt_state_t saved_state;

    saved_state    = req->send.state;
    req->send.lane = ucp_ep_get_tag_lane(ep);

    ucp_dt_iov_copy_uct(iov, &iovcnt, max_iov, &req->send.state, req->send.buffer,
                        req->send.datatype, req->send.length);

    status = uct_ep_tag_eager_zcopy(ep->uct_eps[req->send.lane], req->send.tag,
                                    imm_data, iov, iovcnt, &req->send.uct_comp);
    if (status == UCS_OK) {
        complete(req);
    } else if (status < 0) {
        req->send.state = saved_state; /* need to restore the offsets state */
        return status;
    } else {
        ucs_assert(status == UCS_INPROGRESS);
    }

    return UCS_OK;
}

static ucs_status_t ucp_tag_offload_eager_bcopy(uct_pending_req_t *self)
{
    ucs_status_t status = ucp_do_tag_offload_bcopy(self, 0ul,
                                                   ucp_tag_offload_pack_eager);

    if (status == UCS_OK) {
        ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
        ucp_request_send_generic_dt_finish(req);
        ucp_request_complete_send(req, UCS_OK);
    }
    return status;
}

static ucs_status_t ucp_tag_offload_eager_zcopy(uct_pending_req_t *self)
{
    return ucp_do_tag_offload_zcopy(self, 0ul, ucp_tag_eager_zcopy_req_complete);
}

ucs_status_t ucp_tag_offload_sw_rndv(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_ep_t *ep       = req->send.ep;
    ucp_sw_rndv_hdr_t rndv_hdr = {
        .super.sender_uuid = req->send.ep->worker->uuid,
        .super.reqptr      = (uintptr_t)req,
        .length            = req->send.length
    };

    return uct_ep_tag_rndv_request(ep->uct_eps[req->send.lane], req->send.tag,
                                   &rndv_hdr, sizeof(rndv_hdr));
}

ucs_status_t ucp_tag_offload_rndv_zcopy(uct_pending_req_t *self)
{
    void *rndv_op;
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_ep_t *ep       = req->send.ep;
    size_t max_iov     = ucp_ep_config(ep)->tag.eager.max_iov;
    uct_iov_t *iov     = ucs_alloca(max_iov * sizeof(uct_iov_t));
    size_t iovcnt      = 0;
    ucp_request_hdr_t rndv_hdr = {
        .sender_uuid = ep->worker->uuid,
        .reqptr      = (uintptr_t)req
    };

    req->send.uct_comp.count = 1;
    req->send.uct_comp.func  = ucp_tag_eager_zcopy_completion;

    ucs_assert_always(UCP_DT_IS_CONTIG(req->send.datatype));
    ucp_dt_iov_copy_uct(iov, &iovcnt, max_iov, &req->send.state, req->send.buffer,
                        req->send.datatype, req->send.length);

    rndv_op = uct_ep_tag_rndv_zcopy(ep->uct_eps[req->send.lane], req->send.tag,
                                    &rndv_hdr, sizeof(rndv_hdr), iov, iovcnt,
                                    &req->send.uct_comp);
    if (UCS_PTR_IS_ERR(rndv_op)) {
        return UCS_PTR_STATUS(rndv_op);
    }
    req->flags |= UCP_REQUEST_FLAG_OFFLOADED;
    req->send.tag_offload.rndv_op = rndv_op;
    return UCS_OK;
}

void ucp_tag_offload_cancel_rndv(ucp_request_t *req)
{
    ucp_ep_t *ep = req->send.ep;
    ucs_status_t status;

    status = uct_ep_tag_rndv_cancel(ep->uct_eps[ucp_ep_get_tag_lane(ep)],
                                    req->send.tag_offload.rndv_op);
    if (status != UCS_OK) {
        ucs_error("Failed to cancel tag rndv op %s", ucs_status_string(status));
    }
}

ucs_status_t ucp_tag_offload_start_rndv(ucp_request_t *sreq)
{
    ucs_status_t status;
    ucp_lane_index_t lane = ucp_ep_get_tag_lane(sreq->send.ep);

    sreq->send.lane = lane;
    if (UCP_DT_IS_CONTIG(sreq->send.datatype)) {
        status = ucp_request_send_buffer_reg(sreq, lane);
        if (status != UCS_OK) {
            return status;
        }
        sreq->send.uct.func = ucp_tag_offload_rndv_zcopy;
    } else {
        sreq->send.uct.func = ucp_tag_offload_sw_rndv;
    }
    return UCS_OK;
}

const ucp_proto_t ucp_tag_offload_proto = {
    .contig_short     = ucp_tag_offload_eager_short,
    .bcopy_single     = ucp_tag_offload_eager_bcopy,
    .bcopy_multi      = NULL,
    .zcopy_single     = ucp_tag_offload_eager_zcopy,
    .zcopy_multi      = NULL,
    .zcopy_completion = ucp_tag_eager_zcopy_completion,
    .only_hdr_size    = 0,
    .first_hdr_size   = 0,
    .mid_hdr_size     = 0
};

/* Eager sync */

const ucp_proto_t ucp_tag_offload_sync_proto = {
    .contig_short     = NULL,
    .bcopy_single     = ucs_empty_function_return_unsupported,
    .bcopy_multi      = NULL,
    .zcopy_single     = ucs_empty_function_return_unsupported,
    .zcopy_multi      = NULL,
    .zcopy_completion = NULL,
    .only_hdr_size    = 0,
    .first_hdr_size   = 0,
    .mid_hdr_size     = 0
};


