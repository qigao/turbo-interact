#include <turbo_interact/application.h>
#include <turbo_interact/domain.h>
#include <type_traits>

static_assert(std::is_standard_layout<ti_frame_ref_v1>::value, "C ABI layout");
static_assert(std::is_trivially_copyable<ti_event_header_v1>::value, "event header is a value");
int main() {
    ti_action_receipt_v1 receipt = TI_ACTION_RECEIPT_V1_INIT;
    receipt.level = TI_RECEIPT_BACKEND_TERMINAL;
    receipt.outcome = TI_ACTION_UNKNOWN;
    receipt.action_id = receipt.attempt_id = 1;
    receipt.plan_generation = receipt.session_generation = 1;
    return ti_action_receipt_validate(&receipt) == TI_OK ? 0 : 1;
}
