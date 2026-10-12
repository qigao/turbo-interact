#include "turbo_interact/domain.h"
#ifdef NDEBUG
#undef NDEBUG /* Test invariants must run in Release too. */
#endif
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static ti_frame_ref_v1 valid_ref(void) {
    ti_frame_ref_v1 value = TI_FRAME_REF_V1_INIT;
    value.store_id = 7u;
    value.store_generation = 3u;
    value.slot = 0u;
    value.slot_generation = 1u;
    value.frame_id = 23u;
    value.session_generation = 8u;
    value.display_generation = 12u;
    return value;
}

static void test_frame_ref(void) {
    ti_frame_ref_v1 f = valid_ref();
    assert(ti_frame_ref_validate(&f) == TI_OK);
    f.slot = 0u; assert(ti_frame_ref_validate(&f) == TI_OK);
    f.slot_generation = 0u; assert(ti_frame_ref_validate(&f) == TI_EINVAL);
    f.slot_generation = 1u; f.size--;
    assert(ti_frame_ref_validate(&f) == TI_EINVAL);
    f.size++; f.version++;
    assert(ti_frame_ref_validate(&f) == TI_EVERSION);
    assert(ti_frame_ref_validate(NULL) == TI_EINVAL);
}

static void test_observation(void) {
    ti_observation_v1 o = TI_OBSERVATION_V1_INIT;
    o.frame = valid_ref();
    o.observation_id = 44u;
    o.capture_sequence = 2u;
    o.model_sha256[0] = 0x45;
    o.output_schema_id = 8u;
    o.target_class_id = 1u;
    o.confidence_ppm = 910000u;
    o.width_normalized = 100000u;
    o.height_normalized = 300000u;
    assert(ti_observation_validate(&o) == TI_OK);
    o.x_normalized = 950000u;
    assert(ti_observation_validate(&o) == TI_ERANGE);
    o.x_normalized = 0u; o.confidence_ppm = 1000001u;
    assert(ti_observation_validate(&o) == TI_ERANGE);
    o.confidence_ppm = 0u; o.frame.slot_generation = 0u;
    assert(ti_observation_validate(&o) == TI_EINVAL);
    o.frame.slot_generation = 1u; o.model_sha256[0] = 0u;
    assert(ti_observation_validate(&o) == TI_EINVAL);
}

static void test_event(void) {
    ti_event_header_v1 e = TI_EVENT_HEADER_V1_INIT;
    e.kind = TI_EVENT_OBSERVATION;
    e.event_id = 100u;
    e.plan_generation = 2u;
    e.session_generation = 3u;
    e.schema_id = 8u;
    assert(ti_event_header_validate(&e) == TI_OK);
    e.kind = TI_EVENT_ANALYSIS_RESULT;
    assert(ti_event_header_validate(&e) == TI_EINVAL);
    e.correlation_id = 99u;
    assert(ti_event_header_validate(&e) == TI_OK);
    e.payload_bytes = TI_MAX_EVENT_PAYLOAD_BYTES + 1u;
    assert(ti_event_header_validate(&e) == TI_ERANGE);
    e.payload_bytes = 128u;
    e.flags = 1u;
    assert(ti_event_header_validate(&e) == TI_EINVAL);
    e.flags = 0u; e.version = 4u;
    assert(ti_event_header_validate(&e) == TI_EVERSION);
}

static void test_action(void) {
    ti_action_intent_v1 a = TI_ACTION_INTENT_V1_INIT;
    ti_action_context_v1 c = TI_ACTION_CONTEXT_V1_INIT;
    ti_action_receipt_v1 r = TI_ACTION_RECEIPT_V1_INIT;
    a.kind = TI_ACTION_POINTER_DOWN;
    a.provider_id = 2u;
    a.action_id = 10u;
    a.attempt_id = 1u;
    a.evidence_id = 55u;
    a.plan_generation = 1u;
    a.session_generation = 2u;
    a.display_generation = 3u;
    a.deadline_ns = 2000u;
    a.pointer_id = 1u;
    a.x_normalized = 300000u;
    a.y_normalized = 700000u;
    c.provider_id = 2u;
    c.capabilities = TI_ACTION_CAP_POINTER;
    c.plan_generation = 1u;
    c.session_generation = 2u;
    c.display_generation = 3u;
    c.now_ns = 1000u;
    c.authorized = 1u;
    assert(ti_action_intent_preflight(&a, &c) == TI_OK);
    c.authorized = 0u;
    assert(ti_action_intent_preflight(&a, &c) == TI_EAUTH);
    c.authorized = 1u; c.session_generation++;
    assert(ti_action_intent_preflight(&a, &c) == TI_ESTALE);
    c.session_generation--; c.now_ns = 2000u;
    assert(ti_action_intent_preflight(&a, &c) == TI_EEXPIRED);
    c.now_ns = 1000u; a.pointer_id = 0u;
    assert(ti_action_intent_preflight(&a, &c) == TI_EINVAL);
    a.pointer_id = 1u; a.x_normalized = 1000001u;
    assert(ti_action_intent_preflight(&a, &c) == TI_ERANGE);
    a.x_normalized = 0u; a.kind = TI_ACTION_UI_NODE_CLICK;
    assert(ti_action_intent_preflight(&a, &c) == TI_EUNSUPPORTED);
    c.capabilities |= TI_ACTION_CAP_UI_NODE;
    assert(ti_action_intent_preflight(&a, &c) == TI_EINVAL); /* missing UI target */
    a.target_id = 42u;
    assert(ti_action_intent_preflight(&a, &c) == TI_OK);
    a.provider_id = 6u;
    assert(ti_action_intent_preflight(&a, &c) == TI_EUNSUPPORTED);

    r.outcome = TI_ACTION_COMPLETED;
    r.level = TI_RECEIPT_TRANSPORT;
    r.action_id = 10u; r.attempt_id = 1u;
    r.plan_generation = 1u; r.session_generation = 2u;
    assert(ti_action_receipt_validate(&r) == TI_EINVAL);
    r.level = TI_RECEIPT_BACKEND_TERMINAL;
    assert(ti_action_receipt_validate(&r) == TI_OK);
    r.outcome = TI_ACTION_UNKNOWN;
    assert(ti_action_receipt_validate(&r) == TI_OK);
}

int main(void) {
    test_frame_ref();
    test_observation();
    test_event();
    test_action();
    puts("domain contract tests passed");
    return 0;
}
