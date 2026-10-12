# TurboInteract Roadmap and Qualification Ledger

**Status:** architecture approved for implementation planning, no TurboInteract runtime code or new CI verified yet. Each gate requires reproducible code/tests, not only an architecture document.

## Work items and hard dependency edges

| Phase | Tracking | Repository that owns implementation | Hard exit gate |
| --- | --- | --- | --- |
| M0: contracts / plan | [#2](https://github.com/qigao/turbo-interact/issues/2) | TurboInteract | C11/C++17 types, schema/authority/plan preflight tests |
| M1: FrameRef bridge | [#3](https://github.com/qigao/turbo-interact/issues/3) | TurboInteract fake bridge; TurboMedia real FrameStore | ABA/pin/fan-out/cancel/stop conformance |
| M2: StreamGraph v2 prerequisite | [#4](https://github.com/qigao/turbo-interact/issues/4) | **TurboMedia** | StreamGraph node registry, FFmpeg migration, frame lease, installed SDK |
| M3: DataGraph | [#5](https://github.com/qigao/turbo-interact/issues/5) | TurboInteract adapter; Turbo-Flow runtime already exists | real .flow typed observation source/filters; bridge correlated terminal outcomes |
| M4: Control | [#6](https://github.com/qigao/turbo-interact/issues/6) | TurboInteract SCXML host adapters | Supervisor/Task Statecharts, invoke cancel, generations and drain |
| M5: Android Actions | [#7](https://github.com/qigao/turbo-interact/issues/7) | TurboInteract | UHID, Accessibility, TouchLease and strict receipt semantics |
| M6: App Packs, two apps | [#8](https://github.com/qigao/turbo-interact/issues/8) | TurboInteract | one reusable Task with two different recognizers; complete closed loop |

Umbrella: [#1](https://github.com/qigao/turbo-interact/issues/1).

### Scheduling guidance

M0 and a pure-C deterministic M1 fake FrameStore test suite can start immediately without waiting for StreamGraph v2. M2 upstream work can proceed independently. M3 can start with owned synthetic ObservationEvent messages on current Turbo-Flow `.flow` APIs; M4 can start with synthetic Host events on current TurboSCXML APIs; neither may claim end-to-end qualification until joined to real StreamGraph and Action Owner. Start M5 fake-backend semantics before provisioning devices; native device backends are later conformance layers. M6 requires integrated M0..M5 plus Android capability checks.

## Component availability matrix

| Capability | Verified public source surface | Proposed / missing |
| --- | --- | --- |
| TurboMedia input/FFmpeg/RTP | `turbo_media_source_t`, `turbo_pipeline_create_from_yaml`, `turbo_pipeline_run` | General StreamGraph v2, typed processor registration |
| TurboMedia frame ownership | Encoded `turbo_media_frame_t`, FFmpeg internal AVFrame | Exported decoded `FrameStore`, CPU/GPU Lease, Tee/Sample/latest |
| Turbo-Flow typed graph | `turbo_flow_register_operation`, .flow compiler, typed operations, bounded async ingress | AVAR-specific Observation providers/Bridge |
| Turbo-Flow windows | keyed state, fixed event-time tumbling windows | specialized tracker/vision model scheduling; no drop-oldest worker ring |
| TurboSCXML | SCXML Program/Session, Host Event/Invoke, delayed send/cancel | AVAR graph control/Analysis/Action Host adapters |
| Salts | `mem_buffer_t` atomic refcount, CNet | StreamGraph FrameStore; cross-process shared-memory broker |
| Praktor | reviewed YAML Plan, `uses`, HostTool | optional integration of bounded device Task invocations |
| TurboInteract | only design README/docs/issues | all runtime modules and CI |

The repository must **not** report available features based only on the proposals. Every native SDK and CMake target must be explicitly identified and matched to the actual installed release when code starts.

## M0 architecture qualification tests

- Version and size of every public type; reject unknown version and truncated mandatory fields.
- Stable namespace / generation identities and semantic target schema checks.
- Illegal graph cross-edge (raw VideoFrame pointer into Turbo-Flow message) rejected.
- Unsupported control Provider, ambiguous model registry selection and unauthorized Action binding rejected.
- Immutable ApplicationPlan publication; failure leaves no constructed external live resources.
- Installed consumers link from SDK; C and C++ names match intended ABI.

## M1 deterministic frame/bridge tests

1. Frame slot 7 publishes generation 10, retires, then publishes generation 11; gen-10 FrameRef rejected even if frame ID repeats.
2. Producer reserves 1 pin and 1 bridge slot; prepare fails -> exactly both released.
3. Producer commits; receiver takes -> exactly one owner of pin obligation; duplicate release rejected.
4. Two consumers with 2 pins hold same Frame; releasing first does not recycle while second remains active.
5. Producer publishes weak FrameRef; consumer delays until eviction -> STALE_FRAME, not wrong pixels.
6. Cancel arrives simultaneously with completion; one terminal result, all credits returned.
7. Queue capacity exhausted -> explicit FULL before external effect; no unbounded spill.
8. Source stops while GPU fence outstanding -> BUSY with retained safe storage; destroy only after actual completion.
9. SCXML Host closes while Bridge callback is preparing -> callback quiescence protects its borrowed session.
10. Model plan generation switches; stale inference output rejected without reclassifying it as new output.

Conformance should use deterministic barriers/interleaving rather than relying solely on timing sleeps. Run ASan/UBSan and TSan where supported; test Windows IOCP/macOS Kqueue separately for relevant CNet adapters.

## Media integration gates

- FFmpeg input: local synthetic H264/YUV, file playback, RTSP and live HLS; record every decoded frame/PTS and compare before/after migration.
- RTP: H264/Opus depay/pay and bounded queue failures, no invalid output reordering.
- Format negotiation: NV12 CPU -> RGB CPU conversion explicit; GPU->CPU mapping explicit/failable; never silently label copy as zero-copy.
- Branch policies: record ordered/no drop; preview latest; analysis sample; overflow and sink failure attributed to configured failure domain.
- Stop/drain: explicit EOF flush/trailer on normal file completion, fast terminate policies for canceled realtime source, no late Frame published into destroyed Graph.
- Installed `TurboMedia::StreamGraph` consumer; old `TurboMedia::Pipeline` tests must pass before retiring special execution path.

## Temporal and Action verification gates

- Action request references source frame, source/dispay/session generation and evidence freshness.
- Capture-before-action buffered frame cannot satisfy postcondition; use agent sequence/barrier or report `UNPROVEN`.
- UHID touch Down/Move/Up resource is unique to Action Owner and cannot leak after successful stop.
- Transport ACK != backend completion != Task postcondition; UNKNOWN external action does not trigger automatic replay.
- Permission revoke immediately blocks new actions; no automatic fallback from Accessibility to UHID/scrcpy or vice versa.
- Recovery after device reconnect invalidates old pending analysis/intent/receipts.

## Quality matrix (targets measured, not asserted)

| Metric | What to record |
| --- | --- |
| Pipeline performance | p50/p95/p99 Source->decode->Frame available, Frame->Observation |
| Decision latency | Observation->Turbo-Flow->SCXML state advance |
| Effect latency | Action admission->backend receipt; separate effect->verified observation |
| Memory | peak frame slots, pin credits, CPU/GPU bytes and copies |
| Processing | latest/sample drop count, slow branch backpressure, capacity failures |
| Reliability | cancel/drain duration, leaked credits, stale callback rejections |
| Compatibility | Win/Linux/macOS installed consumers and Android device matrix |

Do not set synthetic performance targets as success claims; benchmark baseline must be measured on target devices and documented with workload, clock domain and hardware.

## Repository layout (target)

```text
include/turbo_interact/
  application.h          # immutable ApplicationPlan
  domain.h               # schemas and generated event types
  frame_ref.h            # pointer-free identity
  bridge.h               # bounded bridge provider interface
  action.h               # typed action domain
src/
  application/           # parser/resolver/preflight
  bridge/                # FrameStore / Turbo-Flow / SCXML adapters
  action/                # owner state and device providers
  device/                # ScreenAgent/AccessAgent transport adaptation
  providers/             # explicitly selected graph/model providers
android/
  screen-agent/
  access-agent/
packs/
  examples/
tests/
  contracts/
  race/
  integration/
docs/architecture/
```

The first PR should contain **M0 headers + fake FrameStore conformance only**, built with CMakePresets and minimal CI. Do not couple initial implementation to an unmerged TurboMedia StreamGraph v2 branch.

## Versioning and cross-repo policy

- Version native ABI independently for FrameRef/GraphBridge/Action/Event.
- Pin installed SDK versions or exact candidate artifacts for qualifications; do not mix header/library generations.
- When TurboMedia StreamGraph is unavailable, a declared StreamGraph-based product build is unsupported; no legacy runtime fallback.
- New upstream work must be implemented/tracked in the repository that owns its native code, linked to TurboInteract umbrella. Separate upstream PRs from TurboInteract product PRs.
- No unconditional video/vision backend in TurboInteract, no Qt core dependency, no global plugin registry duplicating existing CMeta/Salts plugin lifetimes.
