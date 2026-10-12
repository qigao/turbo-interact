#include "turbo_interact/binding.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Known-answer SHA-256("abc"). Real artifact hashing is host-provided. */
static const uint8_t abc_sha256[TI_DIGEST_SHA256_BYTES] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
    0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
    0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
};

static int known_hash(void *user, const uint8_t *bytes, size_t size,
                      uint8_t out[TI_DIGEST_SHA256_BYTES]) {
    uint32_t *calls = (uint32_t *)user;
    (*calls)++;
    if (size != 3u || memcmp(bytes, "abc", 3u) != 0) return -1;
    memcpy(out, abc_sha256, TI_DIGEST_SHA256_BYTES);
    return 0;
}

static ti_graph_port_v1 port(uint32_t role, uint32_t direction,
                             uint32_t kind, uint32_t schema, const char *id) {
    ti_graph_port_v1 p = TI_GRAPH_PORT_V1_INIT;
    p.graph_role = role;
    p.direction = direction;
    p.kind = kind;
    p.schema_id = schema;
    (void)snprintf(p.port_id, sizeof(p.port_id), "%s", id);
    return p;
}

typedef struct fixture {
    ti_target_binding_v1 target;
    ti_application_spec_v1 spec;
    ti_graph_port_v1 ports[4];
    ti_graph_link_v1 links[2];
    ti_model_artifact_v1 model;
    ti_action_provider_desc_v1 provider;
    uint32_t calls;
    ti_binding_catalog_v1 catalog;
} fixture;

static void init_fixture(fixture *f) {
    ti_target_binding_v1 target = TI_TARGET_BINDING_V1_INIT;
    ti_application_spec_v1 spec = TI_APPLICATION_SPEC_V1_INIT;
    ti_graph_link_v1 link = TI_GRAPH_LINK_V1_INIT;
    ti_model_artifact_v1 model = TI_MODEL_ARTIFACT_V1_INIT;
    ti_action_provider_desc_v1 provider = TI_ACTION_PROVIDER_DESC_V1_INIT;
    ti_binding_catalog_v1 cat = TI_BINDING_CATALOG_V1_INIT;
    memset(f, 0, sizeof(*f));

    (void)snprintf(target.semantic_target, sizeof(target.semantic_target), "ui.start");
    (void)snprintf(target.model_id, sizeof(target.model_id), "game.ui");
    memcpy(target.model_sha256, abc_sha256, sizeof(abc_sha256));
    target.output_schema_id = 42u;
    f->target = target;

    (void)snprintf(spec.application_id, sizeof(spec.application_id), "game.example");
    (void)snprintf(spec.stream_graph_id, sizeof(spec.stream_graph_id), "media.graph");
    (void)snprintf(spec.data_graph_id, sizeof(spec.data_graph_id), "facts.graph");
    (void)snprintf(spec.statechart_id, sizeof(spec.statechart_id), "scene.control");
    spec.plan_generation = 1u;
    spec.session_generation = 2u;
    spec.action_provider_id = 17u;
    spec.flags = TI_PLAN_WITH_DATA_GRAPH;
    spec.frame_pin_capacity = 4u;
    spec.bridge_capacity = 8u;
    spec.action_capacity = 2u;
    spec.target_count = 1u;
    spec.targets = &f->target;
    f->spec = spec;

    f->ports[0] = port(TI_GRAPH_STREAM, TI_PORT_OUTPUT,
                       TI_PORT_OBSERVATION, 42u, "vision.out");
    f->ports[1] = port(TI_GRAPH_DATA, TI_PORT_INPUT,
                       TI_PORT_OBSERVATION, 42u, "facts.in");
    f->ports[2] = port(TI_GRAPH_DATA, TI_PORT_OUTPUT,
                       TI_PORT_DECISION, 99u, "facts.out");
    f->ports[3] = port(TI_GRAPH_STATECHART, TI_PORT_INPUT,
                       TI_PORT_DECISION, 99u, "control.in");
    f->links[0] = link;
    f->links[0].source_port = 0u;
    f->links[0].target_port = 1u;
    f->links[1] = link;
    f->links[1].source_port = 2u;
    f->links[1].target_port = 3u;

    (void)snprintf(model.model_id, sizeof(model.model_id), "game.ui");
    memcpy(model.expected_sha256, abc_sha256, sizeof(abc_sha256));
    model.output_schema_id = 42u;
    model.bytes = (const uint8_t *)"abc";
    model.byte_count = 3u;
    f->model = model;

    provider.provider_id = 17u;
    provider.authorized = 1u;
    provider.capabilities = TI_ACTION_CAP_GESTURE;
    f->provider = provider;

    cat.port_count = 4u;
    cat.link_count = 2u;
    cat.model_count = 1u;
    cat.provider_count = 1u;
    cat.ports = f->ports;
    cat.links = f->links;
    cat.models = &f->model;
    cat.providers = &f->provider;
    cat.sha256 = known_hash;
    cat.sha256_user = &f->calls;
    f->catalog = cat;
}

static void test_compile_and_snapshot(void) {
    fixture f;
    ti_application_plan *plan = NULL;
    ti_graph_port_v1 p = TI_GRAPH_PORT_V1_INIT;
    ti_graph_link_v1 link = TI_GRAPH_LINK_V1_INIT;
    uint32_t pc = 0u, lc = 0u;
    init_fixture(&f);
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_OK);
    assert(f.calls == 1u);
    assert(ti_application_plan_compile_bound(&f.spec, &f.catalog, &plan) == TI_OK);
    assert(f.calls == 2u && plan != NULL);
    memset(f.ports, 0, sizeof(f.ports));
    memset(f.links, 0, sizeof(f.links));
    memset(&f.model, 0, sizeof(f.model));
    assert(ti_application_plan_binding_counts(plan, &pc, &lc) == TI_OK);
    assert(pc == 4u && lc == 2u);
    assert(ti_application_plan_bound_port(plan, 0u, &p) == TI_OK);
    assert(strcmp(p.port_id, "vision.out") == 0);
    assert(ti_application_plan_bound_link(plan, 1u, &link) == TI_OK);
    assert(link.source_port == 2u && link.target_port == 3u);
    assert(ti_application_plan_bound_port(plan, 4u, &p) == TI_ERANGE);
    ti_application_plan_destroy(plan);
}

static void test_rejections(void) {
    fixture f;
    ti_application_plan *plan = NULL;
    init_fixture(&f);
    f.ports[1].schema_id = 41u;
    assert(ti_application_plan_compile_bound(&f.spec, &f.catalog, &plan) == TI_ECONFLICT);
    assert(plan == NULL && f.calls == 0u);
    f.ports[1].schema_id = 42u;

    f.provider.authorized = 0u;
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_EAUTH);
    assert(f.calls == 0u);
    f.provider.authorized = 1u;

    f.model.expected_sha256[0] ^= 1u;
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_ECONFLICT);
    f.model.expected_sha256[0] ^= 1u;

    f.model.bytes = (const uint8_t *)"xyz";
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_EUNSUPPORTED);
    f.model.bytes = (const uint8_t *)"abc";

    f.model.expected_sha256[10] ^= 1u;
    f.target.model_sha256[10] ^= 1u;
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_ECONFLICT);
    f.model.expected_sha256[10] ^= 1u;
    f.target.model_sha256[10] ^= 1u;

    f.links[1].source_port = 0u;
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_ECONFLICT);
    f.links[1].source_port = 2u;
    f.catalog.link_count = TI_MAX_BOUND_LINKS + 1u;
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_ERANGE);
    f.catalog.link_count = 2u;
    f.catalog.sha256 = NULL;
    assert(ti_application_bindings_validate(&f.spec, &f.catalog) == TI_EINVAL);
}

static void test_explicit_direct_observation(void) {
    fixture f;
    ti_application_plan *plan = NULL;
    init_fixture(&f);
    f.spec.flags = 0u;
    f.spec.data_graph_id[0] = '\0';
    f.ports[1] = port(TI_GRAPH_STATECHART, TI_PORT_INPUT,
                      TI_PORT_OBSERVATION, 42u, "control.in");
    f.catalog.port_count = 2u;
    f.catalog.link_count = 1u;
    assert(ti_application_plan_compile_bound(&f.spec, &f.catalog, &plan) == TI_OK);
    ti_application_plan_destroy(plan);
}

int main(void) {
    test_compile_and_snapshot();
    test_rejections();
    test_explicit_direct_observation();
    puts("binding preflight tests passed");
    return 0;
}
