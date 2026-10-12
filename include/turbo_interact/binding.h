#ifndef TURBO_INTERACT_BINDING_H
#define TURBO_INTERACT_BINDING_H

#include "turbo_interact/application.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TI_MAX_BOUND_PORTS 128u
#define TI_MAX_BOUND_LINKS 128u
#define TI_MAX_BOUND_MODELS TI_MAX_TARGET_BINDINGS
#define TI_MAX_BOUND_PROVIDERS 32u

typedef enum ti_graph_role {
    TI_GRAPH_STREAM = 1,
    TI_GRAPH_DATA = 2,
    TI_GRAPH_STATECHART = 3
} ti_graph_role;

typedef enum ti_port_direction {
    TI_PORT_INPUT = 1,
    TI_PORT_OUTPUT = 2
} ti_port_direction;

typedef enum ti_port_kind {
    TI_PORT_OBSERVATION = 1,
    TI_PORT_DECISION = 2
} ti_port_kind;

typedef struct ti_graph_port_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t graph_role;
    uint32_t direction;
    uint32_t kind;
    uint32_t schema_id;
    char port_id[TI_NAME_CAPACITY];
    uint32_t reserved;
} ti_graph_port_v1;

#define TI_GRAPH_PORT_V1_INIT {sizeof(ti_graph_port_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, {0}, 0u}

typedef struct ti_graph_link_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t source_port;
    uint32_t target_port;
    uint32_t reserved;
} ti_graph_link_v1;

#define TI_GRAPH_LINK_V1_INIT {sizeof(ti_graph_link_v1), TI_ABI_VERSION, 0u, 0u, 0u}

/* Model bytes belong to the caller. The trusted callback MUST compute SHA-256
 * over all provided bytes; the pack may never supply its own hash callback.
 * A correct digest alone does not establish authenticity or execute a model. */
typedef struct ti_model_artifact_v1 {
    uint32_t size;
    uint32_t version;
    char model_id[TI_NAME_CAPACITY];
    uint8_t expected_sha256[TI_DIGEST_SHA256_BYTES];
    uint32_t output_schema_id;
    uint32_t reserved;
    const uint8_t *bytes;
    size_t byte_count;
} ti_model_artifact_v1;

#define TI_MODEL_ARTIFACT_V1_INIT {sizeof(ti_model_artifact_v1), TI_ABI_VERSION, {0}, {0}, 0u, 0u, NULL, 0u}

typedef int (*ti_sha256_callback)(void *user, const uint8_t *data, size_t size,
                                  uint8_t digest[TI_DIGEST_SHA256_BYTES]);

typedef struct ti_action_provider_desc_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t provider_id;
    uint32_t capabilities;
    uint32_t authorized; /* Snapshot only: ActionOwner rechecks live authority. */
    uint32_t reserved;
} ti_action_provider_desc_v1;

#define TI_ACTION_PROVIDER_DESC_V1_INIT {sizeof(ti_action_provider_desc_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u}

/* Borrowed catalog: no graph or model loading and no external device action. */
typedef struct ti_binding_catalog_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t port_count;
    uint32_t link_count;
    uint32_t model_count;
    uint32_t provider_count;
    const ti_graph_port_v1 *ports;
    const ti_graph_link_v1 *links;
    const ti_model_artifact_v1 *models;
    const ti_action_provider_desc_v1 *providers;
    ti_sha256_callback sha256;
    void *sha256_user;
    uint32_t required_action_capabilities; /* from trusted plan compiler */
    uint32_t reserved;
} ti_binding_catalog_v1;

#define TI_BINDING_CATALOG_V1_INIT {sizeof(ti_binding_catalog_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, NULL, NULL, NULL, NULL, NULL, NULL, 0u, 0u}

/* Provider callback rejection => TI_EUNSUPPORTED; mismatched model bytes or
 * schema => TI_ECONFLICT. Does not authenticate the caller/catalog. */
TI_API ti_status ti_application_bindings_validate(const ti_application_spec_v1 *spec,
                                                  const ti_binding_catalog_v1 *catalog);

/* Make independent pointer-free immutable snapshots of targets/ports/links. */
TI_API ti_status ti_application_plan_compile_bound(const ti_application_spec_v1 *spec,
                                                   const ti_binding_catalog_v1 *catalog,
                                                   ti_application_plan **out);

TI_API ti_status ti_application_plan_bound_port(const ti_application_plan *plan,
                                                uint32_t index, ti_graph_port_v1 *out);
TI_API ti_status ti_application_plan_bound_link(const ti_application_plan *plan,
                                                uint32_t index, ti_graph_link_v1 *out);
TI_API ti_status ti_application_plan_action_capabilities(const ti_application_plan *plan,
                                                      uint32_t *required, uint32_t *available);
TI_API ti_status ti_application_plan_binding_counts(const ti_application_plan *plan,
                                                    uint32_t *port_count,
                                                    uint32_t *link_count);
#ifdef __cplusplus
}
#endif
#endif
