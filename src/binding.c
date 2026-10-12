#include "application_internal.h"

#include <stdlib.h>
#include <string.h>

static ti_status valid_abi(uint32_t size, uint32_t version, size_t expected) {
    if (version != TI_ABI_VERSION) return TI_EVERSION;
    return size == expected ? TI_OK : TI_EINVAL;
}

static int name_ok(const char *s) {
    size_t n;
    if (!s || s[0] == '\0') return 0;
    for (n = 0u; n < TI_NAME_CAPACITY; n++) {
        unsigned char c = (unsigned char)s[n];
        if (c == 0) return 1;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 0;
}

static int digest_present(const uint8_t *value) {
    size_t i;
    for (i = 0u; i < TI_DIGEST_SHA256_BYTES; i++) if (value[i]) return 1;
    return 0;
}

/* Constant-time digest comparison, independent of where the mismatch occurs. */
static int digest_equal(const uint8_t *a, const uint8_t *b) {
    size_t i;
    unsigned char diff = 0u;
    for (i = 0u; i < TI_DIGEST_SHA256_BYTES; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0u;
}

static int port_supported(const ti_graph_port_v1 *p) {
    if (p->graph_role == TI_GRAPH_STREAM)
        return p->direction == TI_PORT_OUTPUT && p->kind == TI_PORT_OBSERVATION;
    if (p->graph_role == TI_GRAPH_DATA)
        return (p->direction == TI_PORT_INPUT && p->kind == TI_PORT_OBSERVATION) ||
               (p->direction == TI_PORT_OUTPUT && p->kind == TI_PORT_DECISION);
    if (p->graph_role == TI_GRAPH_STATECHART)
        return p->direction == TI_PORT_INPUT &&
               (p->kind == TI_PORT_OBSERVATION || p->kind == TI_PORT_DECISION);
    return 0;
}

static int bridge_link_supported(const ti_graph_port_v1 *from,
                                 const ti_graph_port_v1 *to,
                                 int has_data_graph) {
    if (from->direction != TI_PORT_OUTPUT || to->direction != TI_PORT_INPUT ||
        from->kind != to->kind || from->schema_id != to->schema_id) return 0;
    if (has_data_graph)
        return (from->graph_role == TI_GRAPH_STREAM &&
                to->graph_role == TI_GRAPH_DATA && from->kind == TI_PORT_OBSERVATION) ||
               (from->graph_role == TI_GRAPH_DATA &&
                to->graph_role == TI_GRAPH_STATECHART && from->kind == TI_PORT_DECISION);
    return from->graph_role == TI_GRAPH_STREAM &&
           to->graph_role == TI_GRAPH_STATECHART && from->kind == TI_PORT_OBSERVATION;
}

static ti_status validate_ports_and_links(const ti_application_spec_v1 *spec,
                                          const ti_binding_catalog_v1 *cat) {
    uint32_t i, j;
    int has_data_graph = (spec->flags & TI_PLAN_WITH_DATA_GRAPH) != 0;
    int decision_output_connected = 0;
    for (i = 0u; i < cat->port_count; i++) {
        const ti_graph_port_v1 *p = &cat->ports[i];
        ti_status rc = valid_abi(p->size, p->version, sizeof(*p));
        if (rc != TI_OK) return rc;
        if (!p->schema_id || p->reserved || !name_ok(p->port_id)) return TI_EINVAL;
        if (!port_supported(p) || (!has_data_graph && p->graph_role == TI_GRAPH_DATA))
            return TI_EUNSUPPORTED;
        for (j = 0u; j < i; j++) {
            const ti_graph_port_v1 *other = &cat->ports[j];
            if (p->graph_role == other->graph_role &&
                strcmp(p->port_id, other->port_id) == 0) return TI_ECONFLICT;
        }
    }
    for (i = 0u; i < cat->link_count; i++) {
        const ti_graph_link_v1 *link = &cat->links[i];
        ti_status rc = valid_abi(link->size, link->version, sizeof(*link));
        const ti_graph_port_v1 *from, *to;
        if (rc != TI_OK) return rc;
        if (link->reserved || link->source_port >= cat->port_count ||
            link->target_port >= cat->port_count ||
            link->source_port == link->target_port) return TI_EINVAL;
        from = &cat->ports[link->source_port];
        to = &cat->ports[link->target_port];
        if (!bridge_link_supported(from, to, has_data_graph)) return TI_ECONFLICT;
        if (from->kind == TI_PORT_DECISION) decision_output_connected = 1;
        for (j = 0u; j < i; j++) {
            const ti_graph_link_v1 *prior = &cat->links[j];
            if (prior->target_port == link->target_port) return TI_ECONFLICT;
        }
    }
    if (has_data_graph && !decision_output_connected) return TI_EUNSUPPORTED;
    /* Every semantic target has a StreamGraph observation producer. */
    for (i = 0u; i < spec->target_count; i++) {
        uint32_t schema = spec->targets[i].output_schema_id;
        int matched = 0;
        for (j = 0u; j < cat->link_count; j++) {
            const ti_graph_link_v1 *link = &cat->links[j];
            const ti_graph_port_v1 *from = &cat->ports[link->source_port];
            if (from->graph_role == TI_GRAPH_STREAM && from->schema_id == schema &&
                from->kind == TI_PORT_OBSERVATION) { matched = 1; break; }
        }
        if (!matched) return TI_EUNSUPPORTED;
    }
    for (i = 0u; i < cat->port_count; i++) {
        int connected = 0;
        for (j = 0u; j < cat->link_count; j++) {
            if (cat->links[j].source_port == i || cat->links[j].target_port == i) {
                connected = 1; break;
            }
        }
        if (!connected) return TI_EUNSUPPORTED;
    }
    return TI_OK;
}

static ti_status validate_models(const ti_application_spec_v1 *spec,
                                 const ti_binding_catalog_v1 *cat) {
    uint32_t i, j;
    for (i = 0u; i < cat->model_count; i++) {
        const ti_model_artifact_v1 *m = &cat->models[i];
        uint8_t computed[TI_DIGEST_SHA256_BYTES] = {0};
        int used = 0;
        ti_status rc = valid_abi(m->size, m->version, sizeof(*m));
        if (rc != TI_OK) return rc;
        if (!name_ok(m->model_id) || !m->output_schema_id || m->reserved ||
            !digest_present(m->expected_sha256) || !m->bytes || m->byte_count == 0u)
            return TI_EINVAL;
        for (j = 0u; j < i; j++) {
            if (strcmp(m->model_id, cat->models[j].model_id) == 0) return TI_ECONFLICT;
        }
        for (j = 0u; j < spec->target_count; j++) {
            const ti_target_binding_v1 *b = &spec->targets[j];
            if (strcmp(b->model_id, m->model_id) != 0) continue;
            used = 1;
            if (b->output_schema_id != m->output_schema_id ||
                !digest_equal(b->model_sha256, m->expected_sha256)) return TI_ECONFLICT;
        }
        if (!used) return TI_EUNSUPPORTED;
        if (!cat->sha256 || cat->sha256(cat->sha256_user, m->bytes,
                                         m->byte_count, computed) != 0)
            return TI_EUNSUPPORTED;
        if (!digest_equal(computed, m->expected_sha256)) return TI_ECONFLICT;
    }
    for (i = 0u; i < spec->target_count; i++) {
        int found = 0;
        for (j = 0u; j < cat->model_count; j++) {
            if (strcmp(spec->targets[i].model_id, cat->models[j].model_id) == 0) {
                found = 1; break;
            }
        }
        if (!found) return TI_EUNSUPPORTED;
    }
    return TI_OK;
}

static ti_status validate_providers(const ti_application_spec_v1 *spec,
                                    const ti_binding_catalog_v1 *cat) {
    uint32_t i, j;
    int found = 0;
    const uint32_t valid_mask = TI_ACTION_CAP_POINTER | TI_ACTION_CAP_GESTURE |
                                TI_ACTION_CAP_UI_NODE | TI_ACTION_CAP_SYSTEM;
    if (!cat->required_action_capabilities ||
        (cat->required_action_capabilities & ~valid_mask)) return TI_EINVAL;
    for (i = 0u; i < cat->provider_count; i++) {
        const ti_action_provider_desc_v1 *p = &cat->providers[i];
        ti_status rc = valid_abi(p->size, p->version, sizeof(*p));
        if (rc != TI_OK) return rc;
        if (!p->provider_id || p->reserved || p->authorized > 1u ||
            (p->capabilities & ~(TI_ACTION_CAP_POINTER | TI_ACTION_CAP_GESTURE |
                                 TI_ACTION_CAP_UI_NODE | TI_ACTION_CAP_SYSTEM)))
            return TI_EINVAL;
        for (j = 0u; j < i; j++)
            if (p->provider_id == cat->providers[j].provider_id) return TI_ECONFLICT;
        if (p->provider_id == spec->action_provider_id) {
            found = 1;
            if (!p->authorized) return TI_EAUTH;
            if ((p->capabilities & cat->required_action_capabilities) !=
                cat->required_action_capabilities) return TI_EUNSUPPORTED;
        }
    }
    return found ? TI_OK : TI_EUNSUPPORTED;
}

ti_status ti_application_bindings_validate(const ti_application_spec_v1 *spec,
                                           const ti_binding_catalog_v1 *cat) {
    ti_status rc = ti_application_spec_validate(spec);
    if (rc != TI_OK) return rc;
    if (!cat) return TI_EINVAL;
    rc = valid_abi(cat->size, cat->version, sizeof(*cat));
    if (rc != TI_OK) return rc;
    if (cat->reserved) return TI_EINVAL;
    if (!cat->port_count || cat->port_count > TI_MAX_BOUND_PORTS ||
        !cat->link_count || cat->link_count > TI_MAX_BOUND_LINKS ||
        !cat->model_count || cat->model_count > TI_MAX_BOUND_MODELS ||
        !cat->provider_count || cat->provider_count > TI_MAX_BOUND_PROVIDERS)
        return TI_ERANGE;
    if (!cat->ports || !cat->links || !cat->models || !cat->providers || !cat->sha256)
        return TI_EINVAL;
    rc = validate_ports_and_links(spec, cat);
    if (rc != TI_OK) return rc;
    rc = validate_providers(spec, cat);
    if (rc != TI_OK) return rc;
    return validate_models(spec, cat);
}

ti_status ti_application_plan_compile_bound(const ti_application_spec_v1 *spec,
                                            const ti_binding_catalog_v1 *cat,
                                            ti_application_plan **out) {
    ti_application_plan *plan = NULL;
    ti_status rc;
    if (!out || *out != NULL) return TI_EINVAL;
    rc = ti_application_bindings_validate(spec, cat);
    if (rc != TI_OK) return rc;
    rc = ti_application_plan_compile(spec, &plan);
    if (rc != TI_OK) return rc;
    plan->ports = (ti_graph_port_v1 *)calloc(cat->port_count, sizeof(*plan->ports));
    plan->links = (ti_graph_link_v1 *)calloc(cat->link_count, sizeof(*plan->links));
    if (!plan->ports || !plan->links) {
        ti_application_plan_destroy(plan);
        return TI_ENOMEM;
    }
    memcpy(plan->ports, cat->ports, cat->port_count * sizeof(*plan->ports));
    memcpy(plan->links, cat->links, cat->link_count * sizeof(*plan->links));
    plan->port_count = cat->port_count;
    plan->link_count = cat->link_count;
    plan->required_action_capabilities = cat->required_action_capabilities;
    for (uint32_t i = 0; i < cat->provider_count; i++) {
        if (cat->providers[i].provider_id == spec->action_provider_id) {
            plan->provider_action_capabilities = cat->providers[i].capabilities;
            break;
        }
    }
    plan->bound = 1;
    *out = plan;
    return TI_OK;
}

ti_status ti_application_plan_action_capabilities(const ti_application_plan *plan,
                                                  uint32_t *required, uint32_t *available) {
    if (!plan || !required || !available) return TI_EINVAL;
    if (!plan->bound) return TI_EUNSUPPORTED;
    *required = plan->required_action_capabilities;
    *available = plan->provider_action_capabilities;
    return TI_OK;
}

ti_status ti_application_plan_binding_counts(const ti_application_plan *plan,
                                             uint32_t *port_count, uint32_t *link_count) {
    if (!plan || !port_count || !link_count) return TI_EINVAL;
    if (!plan->bound) return TI_EUNSUPPORTED;
    *port_count = plan->port_count;
    *link_count = plan->link_count;
    return TI_OK;
}

ti_status ti_application_plan_bound_port(const ti_application_plan *plan,
                                         uint32_t index, ti_graph_port_v1 *out) {
    if (!plan || !out) return TI_EINVAL;
    if (out->version != TI_ABI_VERSION) return TI_EVERSION;
    if (out->size != sizeof(*out)) return TI_EINVAL;
    if (!plan->bound) return TI_EUNSUPPORTED;
    if (index >= plan->port_count) return TI_ERANGE;
    *out = plan->ports[index];
    return TI_OK;
}

ti_status ti_application_plan_bound_link(const ti_application_plan *plan,
                                         uint32_t index, ti_graph_link_v1 *out) {
    if (!plan || !out) return TI_EINVAL;
    if (out->version != TI_ABI_VERSION) return TI_EVERSION;
    if (out->size != sizeof(*out)) return TI_EINVAL;
    if (!plan->bound) return TI_EUNSUPPORTED;
    if (index >= plan->link_count) return TI_ERANGE;
    *out = plan->links[index];
    return TI_OK;
}
