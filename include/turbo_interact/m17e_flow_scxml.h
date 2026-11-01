#ifndef TURBO_INTERACT_M17E_FLOW_SCXML_H
#define TURBO_INTERACT_M17E_FLOW_SCXML_H

/* OPTIONAL, real installed TurboFlow/SCXML SDK integration.
 * No fake Flow engine, alternative Statechart, async worker, or IPC.
 * This API is absent when TI_WITH_TURBOFLOW_SCXML=OFF; the regular
 * TurboInteract::Core and StreamGraphBridge products remain independent.
 */
#include "turbo_interact/m17e_fact.h"
#include <turbo_flow.h>
#include <scxml/scxml.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ti_m17e_flow_completion_v1 {
    uint32_t size,version;
    tm_pc_portal *portal; /* borrowed through Flow callback JOIN */
    tm_pc_ticket_v1 ticket; /* reserved BEFORE Flow admission */
    uint64_t correlation;
} ti_m17e_flow_completion_v1;
#define TI_M17E_FLOW_COMPLETION_V1_INIT {sizeof(ti_m17e_flow_completion_v1),TI_ABI_VERSION,NULL,{0},0u}

/* Owner-only: reserve a ticket in the EXISTING ProviderCompletion Portal,
 * then call the REAL TurboFlow `turbo_flow_publish_async` on a STARTED Flow.
 * The Flow owns cloned canonical TIFA bytes if admission succeeds. Callback
 * runs on Flow's own ingress worker and calls ONLY tm_pc_complete().
 * SALTS_OK/TI_OK here means ADMITTED, never completed or inferred success.
 *
 * On immediate Flow rejection the adapter calls tm_pc_fail_start; the Host
 * MUST still take/settle the terminal Portal credit on its existing Media
 * Owner. No auto replay, graph thread, new Portal or original Bridge Pin
 * settlement. The caller owns stable ctx through Flow drain/callback JOIN
 * and Portal terminal settlement; it must not be reused before then.
 * `out_flow_status` is the original Flow admission status, if attempted.
 */
TI_API ti_status ti_m17e_flow_publish_status(
    turbo_flow_t *flow,const char *source_name,
    const ti_m17e_terminal_fact_v1 *fact,
    tm_pc_portal *portal,
    ti_m17e_flow_completion_v1 *callback_context,
    int *out_flow_status);

/* Owner-only: offer an already Flow-qualified terminal status to the REAL
 * TurboSCXML bounded session ingress; this is NOT an SCXML transition receipt.
 * The Host must own/prove the live Session mapping for current_generation.
 * The event only carries a scalar correlation ID; downstream must use the
 * separately typed Flow fact for full provenance/model facts.
 * Never route COMPLETED to any `task.succeeded` or device ActionOwner path.
 * `out_mailbox_status` preserves exact cflow_mailbox_status if called.
 */
TI_API ti_status ti_m17e_scxml_admit_provider_status(
    scxml_session *session,
    uint64_t current_session_generation, uint64_t current_plan_generation,
    const ti_m17e_terminal_fact_v1 *fact,
    cflow_mailbox_status *out_mailbox_status);

#ifdef __cplusplus
}
#endif
#endif
