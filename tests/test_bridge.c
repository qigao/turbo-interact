#include "turbo_interact/bridge.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t store_generation;
    uint32_t generation;
    uint32_t pins;
    uint64_t next_ticket;
    uint64_t tickets[16];
    int live[16];
    int fail_next_release;
} fake_store;

static ti_status fake_reserve(void *ctx, const ti_frame_ref_v1 *ref, uint64_t *out) {
    fake_store *f = (fake_store *)ctx;
    uint32_t i;
    if (ref->store_generation != f->store_generation ||
        ref->slot != 0u || ref->slot_generation != f->generation) return TI_ESTALE;
    for (i = 0u; i < 16u; ++i) {
        if (!f->live[i]) {
            f->live[i] = 1;
            f->tickets[i] = ++f->next_ticket;
            f->pins++;
            *out = f->tickets[i];
            return TI_OK;
        }
    }
    return TI_EFULL;
}

static ti_status fake_release(void *ctx, uint64_t token) {
    fake_store *f = (fake_store *)ctx;
    uint32_t i;
    if (f->fail_next_release) {
        f->fail_next_release = 0;
        return TI_EBUSY;
    }
    for (i = 0u; i < 16u; ++i) {
        if (f->live[i] && f->tickets[i] == token) {
            f->live[i] = 0;
            f->pins--;
            return TI_OK;
        }
    }
    return TI_ESTALE;
}

static ti_frame_ref_v1 frame_ref(uint32_t gen) {
    ti_frame_ref_v1 f = TI_FRAME_REF_V1_INIT;
    f.store_id = 3u;
    f.store_generation = 2u;
    f.slot = 0u;
    f.slot_generation = gen;
    f.frame_id = 7u;
    f.session_generation = 12u;
    f.display_generation = 1u;
    return f;
}

static ti_bridge_event_v1 event(uint32_t gen, int pin) {
    ti_bridge_event_v1 e = TI_BRIDGE_EVENT_V1_INIT;
    e.header.kind = TI_EVENT_ANALYSIS_REQUEST;
    e.header.event_id = 5u;
    e.header.plan_generation = 4u;
    e.header.session_generation = 12u;
    e.header.schema_id = 66u;
    e.frame = frame_ref(gen);
    e.has_frame = 1u;
    e.pin_frame = (uint32_t)pin;
    return e;
}

static ti_bridge *create(fake_store *store, uint32_t capacity) {
    ti_bridge_config_v1 c = TI_BRIDGE_CONFIG_V1_INIT;
    ti_bridge *b = NULL;
    c.capacity = capacity;
    c.incarnation = 15u;
    c.pin_reserve = fake_reserve;
    c.pin_release = fake_release;
    c.pin_ctx = store;
    assert(ti_bridge_create(&c, &b) == TI_OK);
    return b;
}

static void test_pin_ownership_and_aba(void) {
    fake_store store = {0};
    ti_bridge *b;
    ti_bridge_event_v1 e = event(1u, 1);
    ti_bridge_event_v1 received = TI_BRIDGE_EVENT_V1_INIT;
    ti_bridge_ticket_v1 t1 = {0}, t2 = {0}, taking = {0};
    ti_bridge_snapshot_v1 snap = TI_BRIDGE_SNAPSHOT_V1_INIT;
    uint64_t pin_token = 0u;
    store.generation = 1u;
    store.store_generation = 2u;
    b = create(&store, 2u);
    assert(ti_bridge_prepare(b, &e, &t1) == TI_OK);
    assert(store.pins == 1u);
    assert(ti_bridge_commit(b, t1) == TI_OK);
    assert(ti_bridge_prepare(b, &e, &t2) == TI_OK);
    assert(store.pins == 2u);
    assert(ti_bridge_commit(b, t2) == TI_OK);
    assert(ti_bridge_take(b, &taking, &received) == TI_OK);
    assert(taking.slot == t1.slot && received.frame.slot_generation == 1u);
    assert(ti_bridge_borrow_pin_token(b, taking, &pin_token) == TI_OK && pin_token);
    assert(ti_bridge_borrow_pin_token(b, t2, &pin_token) == TI_ECONFLICT);
    assert(ti_bridge_settle(b, taking) == TI_OK);
    assert(ti_bridge_borrow_pin_token(b, taking, &pin_token) == TI_ESTALE);
    assert(store.pins == 1u);
    assert(ti_bridge_settle(b, taking) == TI_ESTALE);
    assert(ti_bridge_take(b, &taking, &received) == TI_OK);
    assert(taking.slot == t2.slot);
    assert(ti_bridge_settle(b, taking) == TI_OK);
    assert(store.pins == 0u);
    store.generation = 2u;
    assert(ti_bridge_prepare(b, &e, &taking) == TI_ESTALE);
    e.frame = frame_ref(2u);
    store.store_generation = 3u;
    assert(ti_bridge_prepare(b, &e, &taking) == TI_ESTALE);
    e.frame.store_generation = 3u;
    assert(ti_bridge_prepare(b, &e, &taking) == TI_OK);
    assert(ti_bridge_commit(b, t1) == TI_ESTALE);
    assert(ti_bridge_discard(b, taking) == TI_OK);
    assert(ti_bridge_seal(b) == TI_OK);
    assert(ti_bridge_snapshot(b, &snap) == TI_OK && snap.drained);
    assert(ti_bridge_destroy(b) == TI_OK);
}

static void test_capacity_cancel_and_failed_release(void) {
    fake_store store = {0};
    ti_bridge *b;
    ti_bridge_event_v1 e = event(1u, 1);
    ti_bridge_event_v1 received = TI_BRIDGE_EVENT_V1_INIT;
    ti_bridge_ticket_v1 a = {0}, taken = {0}, rejected = {0};
    store.generation = 1u;
    store.store_generation = 2u;
    b = create(&store, 1u);
    assert(ti_bridge_prepare(b, &e, &a) == TI_OK);
    assert(ti_bridge_prepare(b, &e, &rejected) == TI_EFULL);
    assert(rejected.incarnation == 0u);
    store.fail_next_release = 1;
    assert(ti_bridge_discard(b, a) == TI_EBUSY);
    assert(store.pins == 1u);
    assert(ti_bridge_discard(b, a) == TI_OK && store.pins == 0u);
    assert(ti_bridge_prepare(b, &e, &a) == TI_OK);
    assert(ti_bridge_commit(b, a) == TI_OK);
    assert(ti_bridge_seal(b) == TI_OK);
    assert(ti_bridge_prepare(b, &e, &rejected) == TI_EBUSY);
    assert(ti_bridge_destroy(b) == TI_EBUSY);
    assert(ti_bridge_take(b, &taken, &received) == TI_OK);
    assert(ti_bridge_discard(b, taken) == TI_EBUSY);
    assert(ti_bridge_destroy(b) == TI_EBUSY);
    assert(ti_bridge_settle(b, taken) == TI_OK);
    assert(ti_bridge_destroy(b) == TI_OK);
}

static void test_bad_identity_and_weak_ref(void) {
    fake_store store = {0};
    ti_bridge *b;
    ti_bridge_event_v1 e = event(1u, 0);
    ti_bridge_ticket_v1 t = {0};
    store.generation = 1u;
    store.store_generation = 2u;
    b = create(&store, 1u);
    assert(ti_bridge_prepare(b, &e, &t) == TI_OK);
    assert(store.pins == 0u);
    t.incarnation ^= 1u;
    assert(ti_bridge_commit(b, t) == TI_ESTALE);
    t.incarnation ^= 1u;
    assert(ti_bridge_discard(b, t) == TI_OK);
    assert(ti_bridge_discard(b, t) == TI_ESTALE);
    e.header.payload_bytes = 1u;
    assert(ti_bridge_prepare(b, &e, &t) == TI_EUNSUPPORTED);
    e.header.payload_bytes = 0u;
    e.pin_frame = 1u;
    e.has_frame = 0u;
    assert(ti_bridge_prepare(b, &e, &t) == TI_EINVAL);
    assert(ti_bridge_seal(b) == TI_OK);
    assert(ti_bridge_destroy(b) == TI_OK);
}

static void test_seal_prepared_not_publishable(void) {
    fake_store store = {0};
    ti_bridge_event_v1 e = event(1u, 1);
    ti_bridge_ticket_v1 t = {0};
    ti_bridge *b;
    store.generation = 1u;
    store.store_generation = 2u;
    b = create(&store, 1u);
    assert(ti_bridge_prepare(b, &e, &t) == TI_OK);
    assert(ti_bridge_seal(b) == TI_OK);
    assert(ti_bridge_commit(b, t) == TI_EBUSY);
    assert(store.pins == 1u);
    assert(ti_bridge_destroy(b) == TI_EBUSY);
    assert(ti_bridge_discard(b, t) == TI_OK);
    assert(store.pins == 0u);
    assert(ti_bridge_destroy(b) == TI_OK);
}

static void test_owned_observation_payload(void) {
    fake_store store = {0};
    ti_bridge_config_v1 cfg = TI_BRIDGE_CONFIG_V1_INIT;
    ti_bridge_payload_limits_v1 limits = TI_BRIDGE_PAYLOAD_LIMITS_V1_INIT;
    ti_bridge *b = NULL;
    ti_bridge_ticket_v1 t = {0}, taken = {0}, fail_ticket = {0};
    ti_bridge_event_v1 e = event(1u, 1);
    ti_bridge_event_v1 delivered = TI_BRIDGE_EVENT_V1_INIT;
    uint8_t bytes[4] = {2, 0, 7, 10}, out[4] = {0};
    size_t out_size = 0;
    store.store_generation = 2u;
    store.generation = 1u;
    cfg.capacity = 2u;
    cfg.incarnation = 52u;
    cfg.pin_reserve = fake_reserve;
    cfg.pin_release = fake_release;
    cfg.pin_ctx = &store;
    limits.max_message_bytes = 4u;
    limits.max_inflight_bytes = 6u;
    assert(ti_bridge_create_with_payload_limits(&cfg, &limits, &b) == TI_OK);
    e.header.payload_bytes = sizeof(bytes);
    assert(ti_bridge_prepare(b, &e, &t) == TI_EUNSUPPORTED);
    assert(ti_bridge_prepare_owned(b, &e, bytes, sizeof(bytes), &t) == TI_OK);
    bytes[0] = 99u;
    assert(ti_bridge_prepare_owned(b, &e, bytes, sizeof(bytes), &fail_ticket) == TI_EFULL);
    assert(store.pins == 1u && fail_ticket.incarnation == 0u);
    assert(ti_bridge_commit(b, t) == TI_OK);
    assert(ti_bridge_take(b, &taken, &delivered) == TI_OK);
    assert(ti_bridge_read_payload(b, taken, NULL, 0u, &out_size) == TI_ERANGE);
    assert(out_size == sizeof(out));
    assert(ti_bridge_read_payload(b, taken, out, sizeof(out), &out_size) == TI_OK);
    assert(out[0] == 2u && out[1] == 0u && out[2] == 7u && out[3] == 10u);
    store.fail_next_release = 1;
    assert(ti_bridge_settle(b, taken) == TI_EBUSY);
    assert(ti_bridge_read_payload(b, taken, out, sizeof(out), &out_size) == TI_OK);
    assert(ti_bridge_settle(b, taken) == TI_OK && store.pins == 0u);
    assert(ti_bridge_read_payload(b, taken, out, sizeof(out), &out_size) == TI_ESTALE);
    assert(ti_bridge_prepare_owned(b, &e, bytes, sizeof(bytes), &t) == TI_OK);
    assert(ti_bridge_discard(b, t) == TI_OK);
    assert(ti_bridge_seal(b) == TI_OK);
    assert(ti_bridge_destroy(b) == TI_OK);
}

int main(void) {
    test_pin_ownership_and_aba();
    test_capacity_cancel_and_failed_release();
    test_bad_identity_and_weak_ref();
    test_seal_prepared_not_publishable();
    test_owned_observation_payload();
    puts("bridge credit tests passed");
    return 0;
}
