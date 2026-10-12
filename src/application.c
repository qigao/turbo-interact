#include "turbo_interact/application.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct ti_application_plan {
    ti_application_info_v1 info;
    ti_target_binding_v1 *targets; /* plan-owned immutable array */
};

static int ti_name_valid(const char text[TI_NAME_CAPACITY]) {
    size_t i;
    for (i = 0u; i < TI_NAME_CAPACITY; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == 0u) return i > 0u;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 0;
}

static int ti_digest_present(const uint8_t digest[TI_DIGEST_SHA256_BYTES]) {
    size_t i;
    for (i = 0; i < TI_DIGEST_SHA256_BYTES; ++i)
        if (digest[i]) return 1;
    return 0;
}

ti_status ti_application_spec_validate(const ti_application_spec_v1 *spec) {
    uint32_t i, j;
    if (!spec) return TI_EINVAL;
    if (spec->version != TI_ABI_VERSION) return TI_EVERSION;
    if (spec->size != sizeof(*spec)) return TI_EINVAL;
    if (!ti_name_valid(spec->application_id) ||
        !ti_name_valid(spec->stream_graph_id) ||
        !ti_name_valid(spec->statechart_id) ||
        !spec->plan_generation || !spec->session_generation ||
        !spec->action_provider_id || !spec->frame_pin_capacity ||
        !spec->bridge_capacity || !spec->action_capacity) return TI_EINVAL;
    if (spec->flags & ~TI_PLAN_WITH_DATA_GRAPH) return TI_EUNSUPPORTED;
    if ((spec->flags & TI_PLAN_WITH_DATA_GRAPH) != 0u) {
        if (!ti_name_valid(spec->data_graph_id)) return TI_EINVAL;
    } else if (spec->data_graph_id[0] != '\0') {
        return TI_EINVAL;
    }
    if (spec->frame_pin_capacity > TI_MAX_FRAME_PIN_CAPACITY ||
        spec->bridge_capacity > TI_MAX_BRIDGE_CAPACITY ||
        spec->action_capacity > TI_MAX_ACTION_CAPACITY ||
        spec->target_count == 0u || spec->target_count > TI_MAX_TARGET_BINDINGS)
        return TI_ERANGE;
    if (!spec->targets) return TI_EINVAL;
    for (i = 0u; i < spec->target_count; ++i) {
        const ti_target_binding_v1 *entry = &spec->targets[i];
        if (entry->version != TI_ABI_VERSION) return TI_EVERSION;
        if (entry->size != sizeof(*entry) ||
            !ti_name_valid(entry->semantic_target) ||
            !ti_name_valid(entry->model_id) ||
            !ti_digest_present(entry->model_sha256) ||
            !entry->output_schema_id || entry->reserved) return TI_EINVAL;
        for (j = 0u; j < i; ++j) {
            if (strcmp(entry->semantic_target, spec->targets[j].semantic_target) == 0)
                return TI_ECONFLICT;
        }
    }
    return TI_OK;
}

ti_status ti_application_plan_compile(const ti_application_spec_v1 *spec,
                                       ti_application_plan **out) {
    ti_application_plan *plan;
    ti_status status;
    if (!out || *out != NULL) return TI_EINVAL;
    status = ti_application_spec_validate(spec);
    if (status != TI_OK) return status;
    plan = (ti_application_plan *)calloc(1u, sizeof(*plan));
    if (!plan) return TI_ENOMEM;
    plan->targets = (ti_target_binding_v1 *)calloc((size_t)spec->target_count,
                                                  sizeof(*plan->targets));
    if (!plan->targets) {
        free(plan);
        return TI_ENOMEM;
    }
    plan->info.size = sizeof(plan->info);
    plan->info.version = TI_ABI_VERSION;
    memcpy(plan->info.application_id, spec->application_id, TI_NAME_CAPACITY);
    memcpy(plan->info.stream_graph_id, spec->stream_graph_id, TI_NAME_CAPACITY);
    memcpy(plan->info.data_graph_id, spec->data_graph_id, TI_NAME_CAPACITY);
    memcpy(plan->info.statechart_id, spec->statechart_id, TI_NAME_CAPACITY);
    plan->info.plan_generation = spec->plan_generation;
    plan->info.session_generation = spec->session_generation;
    plan->info.action_provider_id = spec->action_provider_id;
    plan->info.flags = spec->flags;
    plan->info.frame_pin_capacity = spec->frame_pin_capacity;
    plan->info.bridge_capacity = spec->bridge_capacity;
    plan->info.action_capacity = spec->action_capacity;
    plan->info.target_count = spec->target_count;
    memcpy(plan->targets, spec->targets, (size_t)spec->target_count * sizeof(spec->targets[0]));
    *out = plan;
    return TI_OK;
}

ti_status ti_application_plan_info(const ti_application_plan *plan,
                                   ti_application_info_v1 *out) {
    if (!plan || !out) return TI_EINVAL;
    if (out->version != TI_ABI_VERSION) return TI_EVERSION;
    if (out->size != sizeof(*out)) return TI_EINVAL;
    *out = plan->info;
    return TI_OK;
}

ti_status ti_application_plan_find_target(const ti_application_plan *plan,
                                          const char *semantic_target,
                                          ti_target_binding_v1 *out) {
    size_t i;
    if (!plan || !semantic_target || !out) return TI_EINVAL;
    if (out->version != TI_ABI_VERSION) return TI_EVERSION;
    if (out->size != sizeof(*out)) return TI_EINVAL;
    size_t name_length = 0u;
    if (semantic_target[0] == '\0') return TI_EINVAL;
    while (name_length < TI_NAME_CAPACITY && semantic_target[name_length] != '\0')
        ++name_length;
    if (name_length == TI_NAME_CAPACITY) return TI_EINVAL;
    for (i = 0u; i < plan->info.target_count; ++i) {
        if (strcmp(plan->targets[i].semantic_target, semantic_target) == 0) {
            *out = plan->targets[i];
            return TI_OK;
        }
    }
    return TI_EUNSUPPORTED;
}

void ti_application_plan_destroy(ti_application_plan *plan) {
    if (!plan) return;
    free(plan->targets);
    free(plan);
}
