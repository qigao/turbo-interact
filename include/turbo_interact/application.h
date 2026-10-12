#ifndef TURBO_INTERACT_APPLICATION_H
#define TURBO_INTERACT_APPLICATION_H

#include "turbo_interact/domain.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TI_MAX_TARGET_BINDINGS 64u
#define TI_MAX_FRAME_PIN_CAPACITY 4096u
#define TI_MAX_BRIDGE_CAPACITY 65536u
#define TI_MAX_ACTION_CAPACITY 4096u

enum {
    /* Explicit selection of a Turbo-Flow DataGraph; never silently enabled. */
    TI_PLAN_WITH_DATA_GRAPH = 1u << 0
};

typedef struct ti_target_binding_v1 {
    uint32_t size;
    uint32_t version;
    char semantic_target[TI_NAME_CAPACITY];
    char model_id[TI_NAME_CAPACITY];
    uint8_t model_sha256[TI_DIGEST_SHA256_BYTES];
    uint32_t output_schema_id;
    uint32_t reserved;
} ti_target_binding_v1;

#define TI_TARGET_BINDING_V1_INIT {sizeof(ti_target_binding_v1), TI_ABI_VERSION, {0}, {0}, {0}, 0u, 0u}

/* Caller-owned specification: the targets pointer is borrowed ONLY for compile(). */
typedef struct ti_application_spec_v1 {
    uint32_t size;
    uint32_t version;
    char application_id[TI_NAME_CAPACITY];
    char stream_graph_id[TI_NAME_CAPACITY];
    char data_graph_id[TI_NAME_CAPACITY]; /* empty only without TI_PLAN_WITH_DATA_GRAPH */
    char statechart_id[TI_NAME_CAPACITY];
    uint64_t plan_generation;
    uint64_t session_generation;
    uint32_t action_provider_id;
    uint32_t flags;
    uint32_t frame_pin_capacity;
    uint32_t bridge_capacity;
    uint32_t action_capacity;
    uint32_t target_count;
    const ti_target_binding_v1 *targets;
} ti_application_spec_v1;

#define TI_APPLICATION_SPEC_V1_INIT {sizeof(ti_application_spec_v1), TI_ABI_VERSION, {0}, {0}, {0}, {0}, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, NULL}

typedef struct ti_application_plan ti_application_plan;

/* Scalar snapshot: no borrowed pointer or mutable resource handle. */
typedef struct ti_application_info_v1 {
    uint32_t size;
    uint32_t version;
    char application_id[TI_NAME_CAPACITY];
    char stream_graph_id[TI_NAME_CAPACITY];
    char data_graph_id[TI_NAME_CAPACITY];
    char statechart_id[TI_NAME_CAPACITY];
    uint64_t plan_generation;
    uint64_t session_generation;
    uint32_t action_provider_id;
    uint32_t flags;
    uint32_t frame_pin_capacity;
    uint32_t bridge_capacity;
    uint32_t action_capacity;
    uint32_t target_count;
} ti_application_info_v1;

#define TI_APPLICATION_INFO_V1_INIT {sizeof(ti_application_info_v1), TI_ABI_VERSION, {0}, {0}, {0}, {0}, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}

/* Side-effect-free shape/identity preflight, with NO model-file or SDK probing. */
TI_API ti_status ti_application_spec_validate(const ti_application_spec_v1 *spec);

/* Deep-copy into immutable storage; on failure *out remains NULL. */
TI_API ti_status ti_application_plan_compile(const ti_application_spec_v1 *spec,
                                              ti_application_plan **out);
TI_API ti_status ti_application_plan_info(const ti_application_plan *plan,
                                           ti_application_info_v1 *out);
TI_API ti_status ti_application_plan_find_target(const ti_application_plan *plan,
                                                  const char *semantic_target,
                                                  ti_target_binding_v1 *out);
TI_API void ti_application_plan_destroy(ti_application_plan *plan);

#ifdef __cplusplus
}
#endif
#endif
