#include <turbo_interact/application.h>
#include <turbo_interact/domain.h>

int main(void) {
    ti_frame_ref_v1 f = TI_FRAME_REF_V1_INIT;
    f.store_id = f.store_generation = f.slot_generation = 1u;
    f.frame_id = f.session_generation = f.display_generation = 1u;
    return ti_frame_ref_validate(&f) == TI_OK ? 0 : 1;
}
