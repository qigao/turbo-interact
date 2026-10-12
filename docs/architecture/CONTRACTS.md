# TurboInteract Contracts v1 — FrameRef, GraphBridge, Control and Action

**Status:** proposed C11 ABI sketches, not currently shipped. Public headers must be size/versioned and tested through installed C and C++ consumers. Only the owner can mutate its native state.

## 1. Domain primitives

| Type | Immutable/pointer-free? | Owner | Scope |
| --- | --- | --- | --- |
| `FrameRef` | yes | StreamGraph/FrameStore | one Stream/Display generation |
| `FramePinTicket` | yes (identity); **move-owned credit** | FrameStore + GraphBridge | one accepted async consumer |
| `FrameLease` | opaque, runtime-local | FrameStore | acquired operation |
| `ObservationEvent` | yes after serialization | StreamGraph model provider | source frame + model version |
| `AnalysisRequest` | yes; optional transferred pin | SCXML/GraphBridge | request/attempt |
| `AnalysisResult` | yes | StreamGraph executor | request/attempt |
| `ActionIntent` | yes; a *proposal* until admitted | Turbo-Flow/SCXML | target & plan generation |
| `ActionAttempt` | opaque owner-local | Action Owner | one attempted effect |
| `ActionReceipt` | yes | Action Owner | one attempt |
| `TaskState` | mutable, no raw provider ptr persistence | SCXML Session | Task generation |

## 2. Stable identities

```c
/* Conceptual ABI only */
typedef struct ti_frame_ref_v1 {
    uint32_t size, version;
    uint64_t store_id, store_generation;
    uint32_t slot, slot_generation;
    uint64_t frame_id;
    uint64_t session_generation;
    uint64_t display_generation;
} ti_frame_ref_v1;

typedef struct ti_request_ref_v1 {
    uint32_t size, version;
    uint64_t plan_generation;
    uint64_t request_id;             /* unique inside plan incarnation */
    uint64_t attempt_id;
    uint64_t source_session_generation;
} ti_request_ref_v1;
```

FrameRef references may be copied freely but **do not retain pixels, a graph instance, or an input right**. A slot may be reused only after all *admitted pins* and live *FrameLeases* are fully settled. Slot reuse increments `slot_generation`; store rebuild increments `store_generation`. Store ID must be unique within the authorized host namespace and guarded by the Store owner; it is not a secret authentication token.

Never persist FrameRef as a durable input or reconstitute it after process reboot. A persisted workflow stores only immutable plan/model IDs and trusted business progress, then requests fresh observations.

## 3. FrameStore state machine

```text
FREE --reserve_write--> WRITING --publish--> READY
                           |                         |
                           +---abort-----------------+
READY --reserve_pin--> READY [outstanding_pin++]
READY --acquire--> LEASED [outstanding_lease++]
LEASED --release--> READY [outstanding_lease--]
READY --retire--> RETIRING --credits=0,fences_done--> FREE
```

This is a conceptual state machine; implementation can track concurrent lease counts without a single LEASED enum state. Retirement blocks new pin/acquire, not valid existing usage. `retire` must not recycle a slot with outstanding credits.

### FrameStore operations (proposed)

```c
typedef struct ti_frame_store ti_frame_store;
typedef struct ti_frame_lease ti_frame_lease;
typedef struct ti_frame_pin_ticket ti_frame_pin_ticket;
typedef struct ti_frame_map_view ti_frame_map_view;

int ti_frame_store_reserve_pin(ti_frame_store*, const ti_frame_ref_v1*,
                               uint64_t deadline_ns, ti_frame_pin_ticket**);
int ti_frame_store_pin_transfer(ti_frame_pin_ticket** from,
                                ti_frame_pin_ticket** to);
int ti_frame_store_acquire_pinned(ti_frame_pin_ticket*,
                                  ti_frame_lease** out);
int ti_frame_store_try_acquire(ti_frame_store*, const ti_frame_ref_v1*,
                               ti_frame_lease** out); /* may return STALE */
int ti_frame_lease_map(ti_frame_lease*, uint32_t requested_domain,
                       ti_frame_map_view* out); /* may reject unsupported domain */
int ti_frame_lease_unmap(ti_frame_lease*);
void ti_frame_lease_release(ti_frame_lease*);
int ti_frame_store_release_pin(ti_frame_pin_ticket**);
```

**Must distinguish**:
- `stale`: referenced generation/slot no longer current;
- `busy`: resource still leased or blocked by GPU fence;
- `capacity`: no available credits;
- `unsupported`: map/memory-domain conversion is not supported;
- `device-lost`: GPU/device generation revoked.

These are not aliases for retryable `timeout`. API in actual code may use Salts statuses with typed extended diagnostics. Any output lease is owned only on success. Cross-process transfer requires a broker contract, not just serializing a pointer to `ti_frame_pin_ticket`.

## 4. GraphBridge admission and settlement

The bridge handles `small immutable bytes` plus owner-local retention metadata:

```text
sender:
  preflight(schema/version/capability/authority)
  reserve bridge record credit
  reserve FrameStore pin IF delayed frame access required
  copy pointer-free message into reserved row
  commit -> publish READY and transfer pin obligation

receiver:
  take READY -> resolve provider/request generation
  acquire frame if requested -> execute -> release frame
  publish terminal result -> release pin/record credit
```

**Prepare has no visible effect to receivers.** Commit must be a single linearizable owner transition from RESERVED to READY. Discard releases all reservations without publishing. A published ticket cannot be returned as if it had never been consumed; the receiver/owner must settle or the bridge must terminally cancel it.

A bridge event slot is separate from a FrameStore slot. A message with `FrameRef` but **without** retained pin may traverse Turbo-Flow as a weak reference and later fail `STALE_FRAME`. A message with a pin is move-only in admission bookkeeping. DataGraph fan-out duplicates immutable bytes but **does not duplicate** the pin. Fan-out consumers needing retained frames must independently reserve their own pins through the Bridge.

Bridge callbacks cannot synchronously call back into a foreign graph's run/poll, destroy the resource currently delivering that callback, or mutate an SCXML SerialExecutor. Return results via bounded ingress.

### GraphBridge minimal event schema

```c
typedef enum {
    TI_EVT_OBSERVATION = 1,
    TI_EVT_ANALYSIS_REQUEST,
    TI_EVT_ANALYSIS_RESULT,
    TI_EVT_ACTION_INTENT,
    TI_EVT_ACTION_RECEIPT,
    TI_EVT_OWNER_STATUS
} ti_event_kind;

typedef struct ti_event_header_v1 {
    uint32_t size, version;
    uint32_t kind, flags;
    uint64_t event_id, correlation_id;
    uint64_t plan_generation, session_generation;
    uint64_t producer_sequence;
    uint64_t expires_at_ns; /* only comparable on same host monotonic-clock domain */
    uint32_t schema_id, payload_bytes;
} ti_event_header_v1;
```

The serialized wire format must specify endian, field bounds, message integrity and authentication/authorization where remote; these C layouts are **host-memory sketches, not wire ABI**. Schema payloads use the installed DataBind/CMeta type contracts; no private `DataBindValue*` escapes.

### Backpressure

`Bridge publish` returns a capacity error at its true admission boundary. Observation latest/coalescing is allowed **only before** Turbo-Flow admission and only when expressly configured; committed ActionIntent, ActionReceipt and accepted AnalysisRequest are never silently dropped. Turbo-Flow's worker backpressure currently supports block/fail/drop-newest; it does **not** implement drop-oldest. No hidden unbounded spill queue.

## 5. Observation and temporal evidence

```c
typedef struct ti_observation_header_v1 {
    uint32_t size, version;
    ti_frame_ref_v1 frame;
    uint64_t observed_sequence;
    uint64_t capture_timestamp_ns;
    uint64_t display_generation;
    uint64_t model_digest_hi, model_digest_lo; /* illustrative; actual hash is full digest */
    uint32_t geometry_space;
    uint32_t confidence_encoding;
} ti_observation_header_v1;
```

A real model digest is a complete cryptographic content hash in the payload; no truncated digest should be used for model integrity.

**Causality:** An Action postcondition must be observed from a new capture sequence causally after the action's chosen receipt barrier. Do not directly compare unrelated Android and PC monotonic timestamps. Android capture and action agent must provide generation-scoped ordering evidence or an explicit verified capture-barrier handshake. If such evidence is unavailable, the Workflow must declare a weaker verification strategy and cannot claim strict post-action proof.

An Observation carries `geometry_space` and a versioned coordinate transform. Actions based on stale display generation/ROI/transform are rejected.

## 6. AnalysisRequest and model switching

An AnalysisRequest includes:
- immutable request identity and plan generation;
- `FrameRef`, optional pin ownership, model/profile ID and exact digest;
- ROI and coordinate space;
- absolute deadline in the host clock domain and output schema;
- authorized requester and result destination.

Only StreamGraph's selected Provider may map the FrameLease. If model rebind happens while a request is in flight, the existing model/provider generation stays leased until that request settles. Request and result refer to exactly one model generation; old results never become valid under the new plan.

Model activation sequence: validate candidate -> load model within quota -> confirm StreamGraph ready -> seal old admission -> switch active model generation -> drain old work -> unload old generation. Failed candidate preparation leaves the active generation unchanged.

## 7. Action authorization and effect semantics

`ActionIntent` is not automatically an executable command. The Action Owner checks:
1. live session/plan and display generation;
2. current explicit authorization and provider capability;
3. compatible action domain (Pointer/Gesture/Semantic/System);
4. input owner conflict/TouchLease credit;
5. evidence freshness and action deadline;
6. request id and replay status.

```text
PROPOSED -> ADMITTED -> DISPATCHED
                         +-> COMPLETED
                         +-> FAILED
                         +-> UNKNOWN
PROPOSED/ADMITTED --cancel before effect--> CANCELLED
```

A cancellation request after `DISPATCHED` does not imply the remote effect was undone. If the external outcome is not known, retain terminal `UNKNOWN` and do not automatically replay. ActionOwner may report input/report receipt levels; only fresh observations satisfy a Task's business postcondition.

One long-held pointer has exactly one owner, one provider and one generation. `Down` acquires a TouchLease; `Move` changes only that lease; `Up` releases it. Stop requests best-effort release; unresolved external state is surfaced, not silently cleared as verified.

## 8. SCXML Host event and resource adapter

The AVAR SCXML host adapter is proposed as an application-specific composition of current TurboSCXML Event I/O and Invoke surfaces:

- `graph.stream.ready/failed/stopped`
- `graph.data.ready/failed/stopped`
- `vision.analysis.completed/failed/stale`
- `action.completed/failed/unknown`
- `device.connected/disconnected/permission_changed`

SCXML event data is bounded and copied into Session-owned storage through the existing API. No FrameLease or mutable owner object is serialized in an Event. Host prepare/commit/discard allocates bounded tickets and must retain all borrowed Plugin/DSO generations until quiescence, matching current TurboSCXML Host adapter rules.

The Supervisor uses Resource **commands** and waits for authoritative OwnerStatus. Leaving a State does not forcibly abort native FFmpeg or GPU processing, and a successful SCXML cancel does not substitute for an Action/FrameStore drain.

## 9. Multi-process and failure fencing

**MVP:** all graphs in one host process. A process restart always invalidates FrameRefs and pins; no restart recovery may use previous runtime identity.

**Future split:** broker owns shared-memory file descriptors or OS GPU resource handles, maps per process with authorization, maintains independent per-process leases/heartbeats and crash cleanup. Windows handles, Linux FDs and GPU synchronization cannot be represented by identical raw integers across processes.

**Future remote:** explicitly transmit/decode media or integrate a separately qualified remote-memory transport. FrameRef remains *metadata identity*, not a global retrieval guarantee.

## 10. Required conformance proofs

- ABA: old ref rejected after same slot reused and even after store instance recycled.
- Linearizable pin-transfer: publish/commit failure, receiver crash and duplicate release cannot leak/double release.
- Fan-out: N interested consumers cost N independent pins or weaken to weak refs; message clone is never pin clone.
- Time: stale source frame, capture/display rotation, async pre-action buffered frame, disconnected clock domains.
- Device generation: old ActionIntent/Receipt/AnalysisResult are fenced on reconnect.
- Backpressure: full queues never silently retry/drop committed control effects; preview latest mode never reuses claimed slot.
- Stop: producer and consumer callback races plus GPU fence pending, explicit busy until truly drained.
- Permissions: Accessibility revoked/UHID unsupported => no fallback or stealth action.
- Model: hot replace without use-after-unload; exact model/plan digest in results.
- Two games: same reusable task semantics, different vision models, no shared mutable model/task state.
