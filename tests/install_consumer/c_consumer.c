#include <turbo_interact/application.h>
#include <turbo_interact/binding.h>
#include <turbo_interact/bridge.h>
#include <turbo_interact/domain.h>
#include <turbo_interact/bridge.h>

int main(void) {
    ti_frame_ref_v1 f = TI_FRAME_REF_V1_INIT;
    f.store_id = f.store_generation = f.slot_generation = 1u;
    f.frame_id = f.session_generation = f.display_generation = 1u;
    ti_bridge *b = NULL;
    ti_bridge_config_v1 cfg = TI_BRIDGE_CONFIG_V1_INIT;
    ti_bridge_payload_limits_v1 limits = TI_BRIDGE_PAYLOAD_LIMITS_V1_INIT;
    cfg.capacity = 2u;
    cfg.incarnation = 4u;
    limits.max_message_bytes = 128u;
    limits.max_inflight_bytes = 256u;
    if (ti_frame_ref_validate(&f) != TI_OK) return 1;
    if (ti_bridge_create_with_payload_limits(&cfg, &limits, &b) != TI_OK) return 2;
    if (ti_bridge_seal(b) != TI_OK) return 3;
    return ti_bridge_destroy(b) == TI_OK ? 0 : 4;
}
