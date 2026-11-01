#ifndef TURBO_INTERACT_DOMAIN_H
#define TURBO_INTERACT_DOMAIN_H

#include <stdint.h>

#if defined(_WIN32) && defined(TI_SHARED)
#  if defined(TI_BUILDING_LIBRARY)
#    define TI_API __declspec(dllexport)
#  else
#    define TI_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(TI_SHARED)
#  define TI_API __attribute__((visibility("default")))
#else
#  define TI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TI_ABI_VERSION 1u
#define TI_NAME_CAPACITY 64u
#define TI_DIGEST_SHA256_BYTES 32u
#define TI_MAX_EVENT_PAYLOAD_BYTES 65536u

/* A status is an application-local result, not a native provider result. */
typedef enum ti_status {
    TI_OK = 0,
    TI_EINVAL = -1,
    TI_EVERSION = -2,
    TI_ERANGE = -3,
    TI_ECONFLICT = -4,
    TI_EAUTH = -5,
    TI_ESTALE = -6,
    TI_EEXPIRED = -7,
    TI_EUNSUPPORTED = -8,
    TI_ENOMEM = -9,
    TI_EBUSY = -10,
    TI_EFULL = -11
} ti_status;

typedef struct ti_frame_ref_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t store_id;
    uint64_t store_generation;
    uint32_t slot;
    uint32_t slot_generation;
    uint64_t frame_id;
    uint64_t session_generation;
    uint64_t display_generation;
} ti_frame_ref_v1;

#define TI_FRAME_REF_V1_INIT {sizeof(ti_frame_ref_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u}

/* FrameRef is a weak, pointer-free identity. Validation does NOT pin the frame. */
TI_API ti_status ti_frame_ref_validate(const ti_frame_ref_v1 *ref);

/* Normalized target geometry is a value, not a retained image view. */
typedef struct ti_observation_v1 {
    uint32_t size;
    uint32_t version;
    ti_frame_ref_v1 frame;
    uint64_t observation_id;
    uint64_t capture_sequence;
    uint64_t captured_at_ns; /* only comparable in the capture clock domain */
    uint8_t model_sha256[TI_DIGEST_SHA256_BYTES];
    uint32_t output_schema_id;
    uint32_t target_class_id;
    uint32_t confidence_ppm; /* 0..1,000,000 */
    uint32_t x_normalized;
    uint32_t y_normalized;
    uint32_t width_normalized;
    uint32_t height_normalized;
} ti_observation_v1;

#define TI_OBSERVATION_V1_INIT {sizeof(ti_observation_v1), TI_ABI_VERSION, TI_FRAME_REF_V1_INIT, 0u, 0u, 0u, {0}, 0u, 0u, 0u, 0u, 0u, 0u, 0u}
TI_API ti_status ti_observation_validate(const ti_observation_v1 *observation);

typedef enum ti_event_kind {
    TI_EVENT_OBSERVATION = 1,
    TI_EVENT_ANALYSIS_REQUEST = 2,
    TI_EVENT_ANALYSIS_RESULT = 3,
    TI_EVENT_ACTION_INTENT = 4,
    TI_EVENT_ACTION_RECEIPT = 5,
    TI_EVENT_OWNER_STATUS = 6
} ti_event_kind;

typedef struct ti_event_header_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t kind;
    uint32_t flags;
    uint64_t event_id;
    uint64_t correlation_id;
    uint64_t plan_generation;
    uint64_t session_generation;
    uint64_t producer_sequence;
    uint64_t expires_at_ns;
    uint32_t schema_id;
    uint32_t payload_bytes;
} ti_event_header_v1;

#define TI_EVENT_HEADER_V1_INIT {sizeof(ti_event_header_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}

/* Header fields are HOST MEMORY, not a serialized/wire ABI. */
TI_API ti_status ti_event_header_validate(const ti_event_header_v1 *event);

typedef enum ti_action_kind {
    TI_ACTION_POINTER_DOWN = 1,
    TI_ACTION_POINTER_MOVE = 2,
    TI_ACTION_POINTER_UP = 3,
    TI_ACTION_GESTURE_TAP = 4,
    TI_ACTION_UI_NODE_CLICK = 5,
    TI_ACTION_SYSTEM_BACK = 6
} ti_action_kind;

enum {
    TI_ACTION_CAP_POINTER = 1u << 0,
    TI_ACTION_CAP_GESTURE = 1u << 1,
    TI_ACTION_CAP_UI_NODE = 1u << 2,
    TI_ACTION_CAP_SYSTEM = 1u << 3
};

typedef struct ti_action_intent_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t kind;
    uint32_t provider_id;
    uint64_t action_id;
    uint64_t attempt_id;
    uint64_t evidence_id;
    uint64_t plan_generation;
    uint64_t session_generation;
    uint64_t display_generation;
    uint64_t deadline_ns;
    uint64_t target_id; /* UI node ID for UI action; unused otherwise. */
    uint32_t pointer_id;
    uint32_t x_normalized; /* 0..1,000,000 */
    uint32_t y_normalized;
} ti_action_intent_v1;

#define TI_ACTION_INTENT_V1_INIT {sizeof(ti_action_intent_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}

typedef struct ti_action_context_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t provider_id;
    uint32_t capabilities;
    uint64_t plan_generation;
    uint64_t session_generation;
    uint64_t display_generation;
    uint64_t now_ns; /* from SAME host monotonic-clock domain as intent deadline */
    uint32_t authorized; /* 0 or 1 */
    uint32_t reserved;
} ti_action_context_v1;

#define TI_ACTION_CONTEXT_V1_INIT {sizeof(ti_action_context_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}

/* Admission preflight only: no device effects, callbacks, or authority transfer. */
TI_API ti_status ti_action_intent_preflight(const ti_action_intent_v1 *intent,
                                            const ti_action_context_v1 *context);

typedef enum ti_action_outcome {
    TI_ACTION_COMPLETED = 1,
    TI_ACTION_FAILED = 2,
    TI_ACTION_CANCELLED = 3,
    TI_ACTION_UNKNOWN = 4
} ti_action_outcome;

typedef enum ti_receipt_level {
    TI_RECEIPT_TRANSPORT = 1,
    TI_RECEIPT_AGENT_ACCEPTED = 2,
    TI_RECEIPT_BACKEND_TERMINAL = 3
} ti_receipt_level;

typedef struct ti_action_receipt_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t outcome;
    uint32_t level;
    uint64_t action_id;
    uint64_t attempt_id;
    uint64_t plan_generation;
    uint64_t session_generation;
    uint64_t agent_sequence;
} ti_action_receipt_v1;

#define TI_ACTION_RECEIPT_V1_INIT {sizeof(ti_action_receipt_v1), TI_ABI_VERSION, 0u, 0u, 0u, 0u, 0u, 0u, 0u}
TI_API ti_status ti_action_receipt_validate(const ti_action_receipt_v1 *receipt);

#ifdef __cplusplus
}
#endif
#endif
