# TurboInteract Android VirtualDisplay agent

This is a **production Android library module in progress**, extracted from the pinned [Mika source import](../third_party/mika/README.md). It is not an installer, a standalone consumer project or a replacement application. The C11 host/Action Owner interface will be integrated after M0 binding is qualified.

## What's implemented in source on this branch

| Component | Purpose | Status |
| --- | --- | --- |
| `DisplayRegistry` | Explicit session identity, Android display ID, display/geometry generations; denies main display | Pure Kotlin compile + local smoke pass; JUnit source added |
| `AndroidVirtualDisplayOwner` | Android 14+ shell-only VirtualDisplay flags, local IME setup requirement, owner release | Source extracted; Android build/device test pending |
| `AndroidRgbaCapture` | ImageReader latest RGBA frame and bounded sampling | Source extracted; Android build/device test pending |
| `AndroidSdkInput` | Explicit display-bound MotionEvent/KeyEvent with an injected deny-by-default Action Owner approval hook | Source extracted; Android build/device test pending |

Reference: [Mika upstream pinned commit](https://github.com/wangzan101/Mika/commit/279a5cae405ac83106b47af9c2794ebfa3333a57), Apache-2.0; derivatives retain upstream attribution.

## Dependency and privilege contract

- `android/agent` is an Android library (AGP 8.7.3 / Kotlin 2.1.20 / compileSdk 36 / minSdk 30), not an APK. Its Android 14+ DisplayOwner requires an **externally authenticated shell-context DisplayManager**. No helper process, permission or ADB setup is implicit.
- `AndroidVirtualDisplayOwner` deliberately requires local IME policy to be confirmed by an injected shell-authorized callback. It fails if Android 14 focus isolation, display creation or IME isolation cannot be achieved; it never silently switches to main-display mirroring.
- Only `DisplayRegistry` is qualified with the local Kotlin compiler; **no Android SDK, real shell helper, Gradle build or device is available in this run**, so compilation/integration of the Android classes and their runtime behavior is unverified.
- `AndroidSdkInput` verifies `DisplayRef` and consults `InputApproval`. The actual authoritative Action Owner, Binder caller-UID checks, capabilities and revocation gates remain [#7](https://github.com/qigao/turbo-interact/issues/7) / [#13](https://github.com/qigao/turbo-interact/issues/13); this skeleton must not be deployed with a permissive verifier.
- `SENT` is an injection-level result, not an App postcondition. `UNKNOWN` must not be auto-replayed. Capture `sequence` is not yet an action-correlated display composition barrier.
- Captured `Bitmap` is owned by the caller after `latestFrame()`; release display first, then capture, then helper resources. No raw frame pointer crosses the device/host boundary.

## Required next code gates

1. Build the real Android library using an Android SDK/Gradle toolchain; fix actual platform/API issues. No CI green is claimed.
2. Extract authenticated shell bootstrap/Binder and a restricted display/launch API from Mika. Deny main screen and duplicate App usage; validate shell caller UID.
3. Bind DisplayRef + AccessAgent node identities to C11 ActionIntent and capture with one owner, no fallback input backend.
4. Validate physical device with Settings: create Display N, start Activity on N, fetch pixels, inject input into N, verify unchanged main-screen focus/task and terminal ownership.
5. Add optional MediaCodec CaptureSession/Surface rebind referencing scrcpy, then TurboMedia transport and capture-ordering proof; do not pretend ImageReader is a full video stream.

Tracking: [#16](https://github.com/qigao/turbo-interact/issues/16).
