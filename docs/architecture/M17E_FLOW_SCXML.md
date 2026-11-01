# M17e — real TurboFlow/SCXML terminal fact binding

**Implementation stages:** [TurboInteract Draft #25](https://github.com/qigao/turbo-interact/pull/25), [Issue #24](https://github.com/qigao/turbo-interact/issues/24). Depends on the exact upstream M17d [Draft #23](https://github.com/qigao/turbo-interact/pull/23) and TurboMedia [Draft #276](https://github.com/qigao/turbomedia/pull/276). No new media Store, Agent scheduler or Statechart runtime.

## 1. What is already source-qualified

The existing native `ti_bridge_test` (Linux/macOS/Windows llvm-mingw and Linux ASan/UBSan) uses the real installed TurboMedia StreamGraph FrameStore; on Linux it also uses genuine OpenH264 decoded I420, original ProviderCompletion callback threads and Media Owner Dispatcher.

`ti_m17e_terminal_fact_from_taken()` reads the **actual** Bridge TAKEN `TI_EVENT_ANALYSIS_RESULT` and validates exact plan/session/provider/capture/display/geometry/stream lineage. `ti_m17e_terminal_fact_encode/decode()` build a stable **TIFA/v1 160-byte** network-order fact, never a native struct dump. A cross-process receiver cannot deserialize a FrameRef, borrow a FramePin or obtain Android input rights. The payload records only **Provider terminal status**; `TM_PC_COMPLETED` is **not** an inference/model correctness fact or task success.

TIFA header layout (offsets in bytes): magic 0, version 4, size 6, kind 8, outcome 12, completion_observed 16, provider_status 20, event_id 24, correlation 32, plan_generation 40, source_session 48, device_session 56, device session generation 64, display_id 72, reserved 76, geometry_generation 80, capture UUID 88/96, stream_generation 104, packet_sequence 112, provider_generation 120, PTS/DTS 128/136, timebase 144/148, schema_id 152, reserved 156. Exactly 160 bytes; zero reserved bytes; no trailing data accepted. This is **content encoding, not transport authentication**.

A `ti_m17e_fact_scope_v1` is supplied by the **trusted Host owner** for
every external Flow/SCXML admission. Every plan/provider/source/device/display/
geometry/capture/stream generation must match before an external API call.
The same pure C11 gate runs on Linux, macOS, Windows and sanitizers.
The Host must additionally prove its Flow/SCXML runtime pointer belongs
to this active scope; a byte array does not authenticate the runtime.

## 2. Actual SDK adapter — explicitly optional, NOT yet release-qualified

`TI_WITH_TURBOFLOW_SCXML=ON` requires `TI_WITH_TURBOMEDIA_STREAMGRAPH=ON` and the **real installed** `TurboFlow::Graph` and `TurboSCXML::SCXML` imported SDK targets. Missing dependencies cause a CMake failure. No faux SDK, generated consumer project, mock external Flow or duplicate Statechart is available in production. The regular `TurboInteract::Core` and `TurboInteract::StreamGraphBridge` remain independent when OFF.

`ti_m17e_flow_publish_status` reserves a ticket in the existing TurboMedia ProviderCompletion Portal **before** calling the actual `turbo_flow_publish_async`. It passes a real, Flow-owned clone of the canonical TIFA bytes as `turbo_flow_msg_t.owned_payload`; `TI_OK` means only **bounded Flow admission**. The genuine Flow ingress completion callback calls **only** `tm_pc_complete` on that pre-reserved ticket; the original Media Owner must take/reconcile and settle it. Rejecting admission calls `tm_pc_fail_start`, retaining a separately owned START_FAILED terminal. No fake completion, no synchronous Flow reentry from its worker callback, no auto-retry/replay, no release of original Bridge FramePin.

`ti_m17e_scxml_admit_provider_status` calls the actual `scxml_session_try_send_named_with_metadata` on a **Host-proven current Session generation**. Only three fact names are offered: `vision.provider.completed`, `vision.provider.cancelled`, and `vision.provider.start_failed`. The metadata copies the scalar correlation ID. `CFLOW_MAILBOX_OK` means only a bounded Event was **admitted**, not that the SCXML transition ran, model output was validated or an Android gesture executed. The Host must run the real SCXML Statechart and treat its separate terminal authority according to the already-owned session/plan contract.

### Qualification limitation

Public GitHub evidence presently has **no published TurboFlow/TurboSCXML release assets** for a pinned, independently installed whole SDK closure. Historical Salts→CMeta SDK tag provenance is separately unresolved ([TurboMedia #225](https://github.com/qigao/turbomedia/issues/225)). Therefore the external `TI_WITH_TURBOFLOW_SCXML` build profile has source changes but **has not passed real installed TurboFlow/SCXML CI**. This profile is explicitly not enabled in default CI, and must remain Draft/unshipped until exact installed SDK and runtime tests are available. Do not treat a default standalone four-host CTest pass as validation of an external Flow or SCXML library.

## 3. Independent remaining product gates

The actual Flow graph must be precompiled/STARTED, with a finite async-ingress capacity. A Flow admission receipt and a later Flow completion must never be conflated. The Flow completion Portal must be reconciled by the same Media Owner; if a Bridge or SCXML downstream rejects, preserve original credits and require Host reconciliation, no hidden retry.

A real SCXML session/program with matching compiled events, finite FIFO and model/plan/session generation fences must be installed and exercised. SCXML acceptance is still not a transition result. An actual trained Vision Provider must separately produce schema-validated confidence/ROI and immutable model hash. Only the canonical TurboInteract C11 ActionOwner may execute Android input. No physical/emulator/ADB device tests belong in CI.
