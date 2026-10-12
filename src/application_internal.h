#ifndef TURBO_INTERACT_APPLICATION_INTERNAL_H
#define TURBO_INTERACT_APPLICATION_INTERNAL_H

#include "turbo_interact/binding.h"

struct ti_application_plan {
    ti_application_info_v1 info;
    ti_target_binding_v1 *targets;
    ti_graph_port_v1 *ports;
    ti_graph_link_v1 *links;
    uint32_t port_count;
    uint32_t link_count;
    uint32_t required_action_capabilities;
    uint32_t provider_action_capabilities;
    int bound;
};

#endif
