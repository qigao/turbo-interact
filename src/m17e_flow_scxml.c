#include "turbo_interact/m17e_flow_scxml.h"
#include <string.h>

/* Called exclusively by a REAL TurboFlow ingress worker. It does not
 * manipulate the Bridge, FrameStore, SCXML session, ActionOwner or lifecycle.
 * It only completes the previously admitted existing Portal ticket. */
static void m17e_flow_completed(void *opaque,
                                 const turbo_flow_publish_result_t *result) {
    const ti_m17e_flow_completion_v1 *c=
        (const ti_m17e_flow_completion_v1 *)opaque;
    int32_t status=SALTS_EPROTO;
    if (!c || c->size!=sizeof(*c) || c->version!=TI_ABI_VERSION ||
        !c->portal || !c->ticket.incarnation ||
        !c->ticket.generation || !c->correlation) return;
    if (result && result->size==sizeof(*result)) status=result->status;
    /* A late callback after cancellation remains CANCELLED in the Portal.
     * This call never authorizes replay or frees the original FramePin. */
    (void)tm_pc_complete(c->portal,c->ticket,status);
}

static ti_status flow_rejection(int code) {
    if (code==SALTS_ENOSPC) return TI_EFULL;
    if (code==SALTS_ESHUTDOWN || code==SALTS_EBUSY)
        return TI_EBUSY;
    if (code==SALTS_ENOMEM) return TI_ENOMEM;
    return TI_EUNSUPPORTED;
}

ti_status ti_m17e_flow_publish_status(
    turbo_flow_t *flow,const char *source,
    const ti_m17e_terminal_fact_v1 *f,
    tm_pc_portal *portal,ti_m17e_flow_completion_v1 *ctx,
    int *flow_status) {
    turbo_flow_msg_t msg;
    uint8_t bytes[TI_M17E_TERMINAL_WIRE_BYTES]={0};
    tm_pc_ticket_v1 ticket={0};
    size_t n=0u;
    tm_pc_status pc;
    ti_status rc;
    int status=SALTS_EINVAL;
    if (flow_status) *flow_status=SALTS_EINVAL;
    if (!flow || !source || !source[0] || !portal || !ctx ||
        ctx->version!=TI_ABI_VERSION || ctx->size!=sizeof(*ctx) ||
        ctx->portal || ctx->ticket.incarnation || ctx->correlation)
        return TI_EINVAL;
    rc=ti_m17e_terminal_fact_encode(f,bytes,sizeof(bytes),&n);
    if (rc!=TI_OK) return rc;
    if (n!=TI_M17E_TERMINAL_WIRE_BYTES) return TI_ECONFLICT;
    pc=tm_pc_begin(portal,f->event_id,&ticket);
    if (pc==TM_PC_EFULL) return TI_EFULL;
    if (pc==TM_PC_ESHUTDOWN) return TI_EBUSY;
    if (pc!=TM_PC_OK) return TI_ECONFLICT;

    ctx->portal=portal;
    ctx->ticket=ticket;
    ctx->correlation=f->event_id;
    turbo_flow_msg_init(&msg);
    msg.id=f->event_id;
    msg.type=TI_M17E_TERMINAL_WIRE_KIND;
    msg.owned_payload=tstr_from_v(
        tstr_v_from_buf((const char *)bytes,sizeof(bytes)));
    if (!msg.owned_payload) {
        (void)tm_pc_fail_start(portal,ticket,SALTS_ENOMEM);
        return TI_ENOMEM; /* Portal's START_FAILED still needs owner settle. */
    }
    msg.payload=tstr_to_v(msg.owned_payload);
    /* Flow clones the owned binary message before returning ADMITTED.
     * Source, Flow runtime and callback_context are Host-owned and bounded.
     * Flow's completion may race this RETURN, never tm_pc_begin above. */
    status=turbo_flow_publish_async(flow,source,&msg,m17e_flow_completed,ctx);
    turbo_flow_msg_cleanup(&msg);
    if (flow_status) *flow_status=status;
    if (status!=SALTS_OK) {
        (void)tm_pc_fail_start(portal,ticket,status);
        return flow_rejection(status);
    }
    return TI_OK;
}

ti_status ti_m17e_scxml_admit_provider_status(
    scxml_session *session,uint64_t session_generation,
    uint64_t plan_generation,const ti_m17e_terminal_fact_v1 *f,
    cflow_mailbox_status *out_status) {
    scxml_event_metadata meta={0};
    cflow_mailbox_status status;
    const char *name;
    ti_status rc;
    if (!session || !out_status || !session_generation ||
        !plan_generation) return TI_EINVAL;
    *out_status=CFLOW_MAILBOX_INVALID_ARGUMENT;
    rc=ti_m17e_terminal_fact_validate(f);
    if (rc!=TI_OK) return rc;
    if (f->source_session_generation!=session_generation ||
        f->plan_generation!=plan_generation) return TI_ESTALE;

    switch(f->outcome) {
        case TM_PC_COMPLETED:
            name="vision.provider.completed";break;
        case TM_PC_CANCELLED:
            name="vision.provider.cancelled";break;
        case TM_PC_START_FAILED:
            name="vision.provider.start_failed";break;
        default:return TI_EINVAL;
    }
    meta.abi_version=SCXML_EVENT_METADATA_ABI;
    meta.struct_size=sizeof(meta);
    /* Only a copied scalar correlation is sent into SCXML. No pixels,
     * FrameRef, dynamic native pointer, AI action, or trust authority. */
    meta.data.kind=SCXML_CONTENT_SCALAR;
    meta.data.scalar.kind=SCXML_PAYLOAD_VALUE_UINT;
    meta.data.scalar.data.uint=f->correlation;
    status=scxml_session_try_send_named_with_metadata(
        session,name,strlen(name),&meta);
    *out_status=status;
    if (status==CFLOW_MAILBOX_OK) return TI_OK; /* ONLY admitted */
    if (status==CFLOW_MAILBOX_FULL) return TI_EFULL;
    if (status==CFLOW_MAILBOX_CLOSED) return TI_EBUSY;
    return TI_EUNSUPPORTED;
}
