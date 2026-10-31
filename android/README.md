# TurboInteract Android VirtualDisplay refactor

**State:** source import complete; runtime/device acceptance pending. Production Android interfaces under `android/agent/` shall be built only when Android toolchain and device tests are available.

## Boundaries

| Role | Owner | Imported reference |
| --- | --- | --- |
| `DisplaySession` | Android shell DisplayOwner | `ShellService.createDisplay`, `setLaunchDisplayId`, `displayFlags` |
| `CaptureSession` | Android frame/surface owner (later TurboMedia) | `AgentDisplay` |
| `InputBackend` | Android shell agent **after** Action Owner admission | `ShellService.inject*`, `ScreenAccessibility.gesture` |
| `AccessibilityProvider` | explicitly user-approved Accessibility service | `ScreenReader`, `AndroidScreen` |
| `DeviceSession` | C11 TurboInteract host | binding to DisplayRef and external grants |

## Rules

1. Android framework/private APIs stay on Android Java/Kotlin side; all outside effects are authorized through the one TurboInteract Action Owner. No second statechart/scheduler/media codec.
2. Distinguish physical main, mirror-only and independent interactive virtual display; only the last one is allowed under a virtual-only input grant.
3. Keep numeric `displayId`, `device_session_id`, generation and geometry version separate. Never inject an event to display 0 as an implicit fallback.
4. The capture `Surface` is a different lifetime from the `VirtualDisplay`; encoder or ImageReader restart must not silently terminate the App task.
5. Keep screen-local IME and focus isolation as requested capabilities. Android 12/13 and OEM behavior is explicitly not assumed.
6. Cached image frames and codec PTS alone are insufficient for a causal post-action observation. Return `UNKNOWN` when a stronger proof cannot be established.
7. Abort or revoke the input session safely, keeping pointer DOWN/UP ownership and uncertain terminal outcomes; actions are never auto replayed.
8. Verify on a real Android device **without** new installer/consumer projects or probe-only CI.

First gate: one display N, launch Settings, obtain frame, click on N and verify main-screen task/focus unaffected; then two displays and rotation.

Upstream: [Mika exact import](../../third_party/mika/README.md) · [#16](https://github.com/qigao/turbo-interact/issues/16).
