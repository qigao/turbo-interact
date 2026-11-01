# M5b: Authenticated Sandbox RPC and Android input mapping

**Current source:** [Draft PR #20](https://github.com/qigao/turbo-interact/pull/20), stacked on [ActionOwner PR #18](https://github.com/qigao/turbo-interact/pull/18). [Issue #19](https://github.com/qigao/turbo-interact/issues/19). No real Android device behavior or Kotlin/JNI binding is claimed.

## Three distinct boundaries

1. **Unprivileged sandbox process:** receives only a randomly created Unix-domain socket pathname and fresh 256-bit token from its authorized host command launch. It cannot obtain C11 ActionOwner, Android Binder or display handles.
2. **Native host Sandbox IPC:** `ti_sandbox_ipc_open` opens a pathname AF_UNIX listener under an existing owned 0700 private directory, ensures socket 0600, uses an unpredictable CSPRNG suffix and reads kernel peer credentials (Android/Linux `SO_PEERCRED`, macOS `LOCAL_PEERCRED`). A strict one-line JSON parser rejects duplicates, unknown keys, nesting, oversized (>64KiB) or incomplete requests. A 30-second maximum deadline and single request per connection are enforced. The host decides which OS peer UID and which tap/back tools are allowed, never the JSON input.
3. **Canonical ActionOwner:** each authenticated request invokes the same `ti_sandbox_bridge_execute` and `ti_action_owner_submit` used for model actions. The request's provider, plan, session, display number and device identity are taken from the trusted host session. The native bridge is NOT an alternate executor or authority; live host grants and fresh frame provenance still need verification by the embedding host.

## Exact request protocol

One newline-terminated JSON object per accepted connection. The response contains an `ok` boolean, numeric `status`, numeric `outcome` and numeric `level`; `ok` means the C11 admission/dispatch path returned TI_OK, **not** that the Android App or user's task succeeded.

```json
{"token":"<64 lowercase hex characters>","tool":"tap","callId":1,"actionId":10,"attemptId":1,"observationId":23,"captureSequence":5,"displayGeneration":3,"geometryGeneration":1,"deadlineNs":2000000000,"x":100000,"y":900000}
```

Use `tool=back` without `x` or `y` and with `observationId=0`, `captureSequence=0` when there is no screen evidence. Coordinates are integer-normalized in `[0,1000000]`, not screenshot pixel coordinates. The caller cannot supply `uid`, `taskId`, `sandboxRunId`, `planGeneration`, `providerId`, `deviceSessionId`, or `displayId`; such keys are rejected. The host session owns these fields.

The host must generate the private directory and choose an OS peer UID before constructing `ti_sandbox_ipc_config_v1`. This library does not create directories, spawn Python/PRoot, install an APK or open wireless debugging. One caller invoking `ti_sandbox_ipc_serve_one` serves a bounded RPC; the host controls its scheduler/cancel policy. Closing/destroying the IPC listener erases the token, unlinks only the originally owned socket inode and **does not revoke the independent model-side ActionOwner**. Windows local IPC is explicitly `TI_EUNSUPPORTED`; do not accept fake credentials.

## Android AIDL mapping: one owner, not two

`ti_android_shell_backend_dispatch` is a callback for the **existing C11 ActionOwner's** `dispatch` slot, not a second owner. It maps only `TI_ACTION_GESTURE_TAP` and `TI_ACTION_SYSTEM_BACK` to Android PR #17's `ITurboDisplayShell.tap` and `key(..., kind=1)` method signatures.

- First the host ActionOwner performs exact display/session/geometry, deadline, evidence, authorized provider and replay checks, then its **live host grant callback** approves this particular attempt.
- Then the backend calls a trusted, **version-aware `geometry(ref)` callback**. On stale/unowned display, it refuses; no historical size or display 0 fallback. Normalized `[0,1000000]` coordinates are converted to `(width-1,height-1)` pixel extent with integer rounding.
- `SENT=1` => backend-terminal input accepted, `DENIED=-1` => backend-terminal input failed, `UNKNOWN=-2` or unrecognized result => `UNKNOWN`. This NEVER proves that the target UI changed or a business task succeeded.
- Kotlin's `AndroidSdkInput` and `AndroidVirtualDisplayOwner` remain a device-side safety fence; their optional additional attempt ledger is defensive, not an independent policy/ownership decision.

**Missing runtime integration:** actual JNI or host FFI adapter that implements geometry/tap/key callbacks with the embedding Android App and connects this C11 owner to Android `ITurboDisplayAuthority` and the authenticated Shell Helper. None of the current C11 CTests exercise live Binder or Android Hidden APIs. Do not report this as complete Android integration.

## CI-qualified boundaries

- Existing production C11 library and CTest (Linux/macOS runner Clang, Windows llvm-mingw, Linux Clang ASan/UBSan).
- Real AF_UNIX socket/client and OS peer-identity conformance on Linux/macOS, with wrong token, forged JSON UID, private-directory restrictions, duplicate/replayed calls, main screen and stale geometry refusal.
- Deterministic C11 adapter dispatch tests validate exact AIDL argument mapping, session IDs, coordinate rounding and strict receipt levels across model and sandbox origins.
- **No emulator, physical device, installer/consumer test projects, probe-only CI or CMake.** Android SDK/AAR CI remains exclusively in separate PR #17.

## Independent outstanding work

- Host wiring that owns the ADB/sandbox process and passes token/IPC endpoint safely to that process; Android/JNI Binder callbacks are not implemented.
- Turn `source_observation_id/capture_sequence` into a host-qualified, nonforgeable evidence grant. Numeric equality is not a compositor-causal barrier; `SENT` must never be used as Task Verified.
- Pointer leases, multi-pointer gestures, cancellation and cross-process revocation/close race proofs. Real-device compatibility is a separate optional product validation, not CI.
