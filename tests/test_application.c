#include "turbo_interact/application.h"
#ifdef NDEBUG
#undef NDEBUG /* Test invariants must run in Release too. */
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ti_target_binding_v1 valid_target(const char *name, unsigned char digest) {
    ti_target_binding_v1 b = TI_TARGET_BINDING_V1_INIT;
    (void)snprintf(b.semantic_target, sizeof(b.semantic_target), "%s", name);
    (void)snprintf(b.model_id, sizeof(b.model_id), "%s", "opencv.start");
    b.model_sha256[0] = digest;
    b.output_schema_id = 9u;
    return b;
}

static ti_application_spec_v1 valid_spec(const ti_target_binding_v1 *targets, unsigned count) {
    ti_application_spec_v1 s = TI_APPLICATION_SPEC_V1_INIT;
    (void)snprintf(s.application_id, sizeof(s.application_id), "%s", "test.game");
    (void)snprintf(s.stream_graph_id, sizeof(s.stream_graph_id), "%s", "media.plan");
    (void)snprintf(s.data_graph_id, sizeof(s.data_graph_id), "%s", "facts.plan");
    (void)snprintf(s.statechart_id, sizeof(s.statechart_id), "%s", "game.task");
    s.plan_generation = 101u;
    s.session_generation = 7u;
    s.action_provider_id = 1u;
    s.flags = TI_PLAN_WITH_DATA_GRAPH;
    s.frame_pin_capacity = 4u;
    s.bridge_capacity = 8u;
    s.action_capacity = 3u;
    s.target_count = count;
    s.targets = targets;
    return s;
}

static void test_preflight_and_copy(void) {
    ti_target_binding_v1 targets[2] = { valid_target("ui.start", 0x73), valid_target("ui.done", 0xb4) };
    ti_application_spec_v1 s = valid_spec(targets, 2u);
    ti_application_plan *p = NULL;
    ti_application_info_v1 info = TI_APPLICATION_INFO_V1_INIT;
    ti_target_binding_v1 copied = TI_TARGET_BINDING_V1_INIT;
    assert(ti_application_spec_validate(&s) == TI_OK);
    assert(ti_application_plan_compile(&s, &p) == TI_OK);
    assert(p != NULL);
    memset(targets, 0, sizeof(targets)); /* lifetime independent of caller */
    assert(ti_application_plan_info(p, &info) == TI_OK);
    assert(info.target_count == 2u);
    assert(strcmp(info.application_id, "test.game") == 0);
    assert(info.plan_generation == 101u);
    assert(ti_application_plan_find_target(p, "ui.start", &copied) == TI_OK);
    assert(copied.model_sha256[0] == 0x73);
    assert(ti_application_plan_find_target(p, "missing", &copied) == TI_EUNSUPPORTED);
    ti_application_plan_destroy(p);
}

static void test_invalid_specs(void) {
    ti_target_binding_v1 targets[2] = { valid_target("ui.start", 0x01), valid_target("ui.done", 0x02) };
    ti_application_spec_v1 s = valid_spec(targets, 2u);
    ti_application_plan *p = NULL;
    s.flags = 0u;
    assert(ti_application_spec_validate(&s) == TI_EINVAL); /* undeclared DataGraph */
    s.data_graph_id[0] = '\0';
    assert(ti_application_spec_validate(&s) == TI_OK); /* explicit no-data-graph plan */
    s.flags = TI_PLAN_WITH_DATA_GRAPH;
    assert(ti_application_spec_validate(&s) == TI_EINVAL);
    (void)snprintf(s.data_graph_id, sizeof(s.data_graph_id), "facts.plan");
    targets[1] = targets[0];
    assert(ti_application_spec_validate(&s) == TI_ECONFLICT);
    assert(ti_application_plan_compile(&s, &p) == TI_ECONFLICT && p == NULL);
    targets[1] = valid_target("ui.done", 0x02);
    targets[0].model_sha256[0] = 0u;
    assert(ti_application_spec_validate(&s) == TI_EINVAL);
    targets[0].model_sha256[0] = 0x01;
    s.version = TI_ABI_VERSION + 1u;
    assert(ti_application_spec_validate(&s) == TI_EVERSION);
    s.version = TI_ABI_VERSION;
    s.target_count = TI_MAX_TARGET_BINDINGS + 1u;
    assert(ti_application_spec_validate(&s) == TI_ERANGE);
    s.target_count = 2u;
    s.bridge_capacity = 0u;
    assert(ti_application_spec_validate(&s) == TI_EINVAL);
    s.bridge_capacity = 8u;
    s.application_id[TI_NAME_CAPACITY - 1u] = 'x';
    memset(s.application_id, 'x', TI_NAME_CAPACITY);
    assert(ti_application_spec_validate(&s) == TI_EINVAL);
}

int main(void) {
    test_preflight_and_copy();
    test_invalid_specs();
    puts("application plan tests passed");
    return 0;
}
