# TurboInteract Android interactive VirtualDisplay agent

**Draft implementation, not qualified on a physical Android device.** Based on [Mika pinned import](../third_party/mika/README.md) (Apache-2.0) and scrcpy's MediaCodec InputSurface model. This is a **production Android library**, not an installer or a consumer/probe test app.

## Code ownership

| Source | Owner and behavior | Status |
| --- | --- | --- |
| `DisplayRegistry` | Session-scoped DisplayRef, numeric ID reuse, geometry generation; display 0 always invalid | JVM test and actual Android library compile |
| `AndroidVirtualDisplayOwner` | Android 14+ shell VirtualDisplay + OWN_FOCUS + anti-focus-steal flags; local IME request and exact release; 2-display budget | SDK source compiled, no real-device proof |
| `AndroidRgbaCapture` | ImageReader RGBA latest-frame sampling with copied Bitmap and local capture sequence | SDK compiled; on-device rendering unverified |
| `AndroidH264Capture` | Hardware MediaCodec InputSurface encoder, copied H.264 buffers, bounded packet queue and explicit FAIL on full queue | Android AAR build passed; no transport/protocol or device test yet |
| `AndroidSdkInput` | Display-owner + admission guard, double replay prevention, touch and limited BACK/ENTER; no implicit main/ADB fallback | SDK compiled, no real device |
| `AndroidShellAppLauncher` | Activity launch on Display N, main-screen/other-display App conflict and attempt replay refusal | SDK compiled, actual task behavior pending |
| `AndroidShellContext` | UID 2000 shell package attribution/ActivityThread context | Android AAR build passed; no trusted bootstrap yet |
| `ITurboDisplayShell.aidl` + `AuthenticatedDisplayShell` | Narrow Binder surface, caller-UID checks and explicit `ShellAdmission` on external effects | SDK compiled; **handshake/Binder delivery not integrated** |

## Build and test

From `android/`, using the **released SimpleDSL 0.3.0** settings/Android plugins, JDK 21, Gradle 9.1.0 and Android SDK 36.

The single repository policy `android/dependencies.toml` specifies Android Java 21, compileSdk 36 and minSdk 30. SimpleDSL manages AGP 9.0.1 with built-in Kotlin (2.2.10); `:agent` uses `simpledsl { androidLibrary { namespace = 'io.turbointeract.android' } }` and explicitly sets `android.buildFeatures.aidl = true`. There is **no** `org.jetbrains.kotlin.android` plugin, duplicate AGP version or separate consumer project.



```sh
gradle --no-daemon :agent:assembleDebug :agent:testDebugUnitTest
```

CI: [Android device library](../.github/workflows/android-agent.yml). It compiles the actual product library, generated AIDL and runs JUnit tests for display generation, attempt replay, and launch policy. The first SimpleDSL migration at commit `575c0186eb2d` passed [Android build CI #41255377321](https://github.com/qigao/turbo-interact/actions/runs/41255377321) and [Mermaid CI #41255377375](https://github.com/qigao/turbo-interact/actions/runs/41255377375). Each newer HEAD must pass independently. CI does **not** install an APK, start an `app_process` helper or prove a real Android device works.

## Restricted shell handoff (implemented in source, device-unqualified)

The Android library now includes a **host App entry** and **shell app_process entry**; this is not a separate APK or a replacement for the C11 Action Owner:

1. An embedding App explicitly calls `TurboDisplayHandoffHost.begin(context, deviceSessionId, sessionGeneration, grants)`. The caller supplies the actual `HostDisplayGrants` policy; there is no allow-all fallback.
2. `launchSpec()` issues a one-time 256-bit nonce and returns `ShellLaunchSpec.command` and `nonceLine`. The embedding App's *already authorized* ADB exec transport must send `nonceLine` on stdin, not in argv, saved scripts or logs. The Android library does not silently enable wireless debugging, install a consumer App, or open an ADB connection.
3. `TurboDisplayShellMain` runs via Android `app_process` under shell uid 2000, checks the actual installed APK source/UID/signing certificates, and connects to the embedding APK's `${applicationId}.turbo_interact_helper` provider using Android's external ContentProvider service. The protected provider rejects unknown callers.
4. `TurboDisplayHandoffHost` verifies Binder calling UID/PID, supplied process UID/PID, current APK classpath, the shell service's separate `identity()` Binder call and a fresh 30-second one-shot nonce, then returns a live `ITurboDisplayAuthority` Binder and host death token.
5. Every device effect consults live host policy through reverse Binder; `Binder.clearCallingIdentity()` avoids accidentally forwarding the host caller's UID during nested callbacks. On host death, shell grants are dropped, owned displays are released and the helper exits. Explicit `revoke()` denies new host grants and requests remote session teardown.

The generated interface `ITurboDisplayShell` exposes only display create/release, targeted tap/limited key and App launch; there is **no** generic command, unlock, clipboard or audio AppOps operation. No implicit display-0 routing or effect retry is allowed.

**Remaining device/runtime blockers:** this library has no bundled ADB transport or installing App; an actual embedding App must provide an authenticated transport and live grant source. The hidden Android API invocation paths have not been exercised on an Android phone. Current APK signer lookup is first qualified for the primary Android user; multi-user/work-profile package discovery needs separate conformance. C11 Action Owner binding, main-screen focus/IME proof, cross-process grant-revocation linearizability, physical touch/surface races, and input→compositor-frame causality remain independent gates.

## Security and functional limits

1. `AuthenticatedDisplayShell` has no default allow-all authority and rejects unknown Binder caller UIDs. Its construction requires shell UID 2000, but the **Mika-derived nonce/certificate-pinned ContentProvider handoff is not yet ported**. The Binder is not available to a host app until that explicit bootstrap is implemented and tested.
2. No globally callable shell command executor, clipboard modifier, unlocking, app move/kill or audio AppOps are exposed. Resource teardown is allowed with a matching display incarnation even after grant revocation.
3. This release path is intentionally Android 14+ for strong focus isolation. Android 12–13 capabilities are separately pending, never silently promoted.
4. `DisplaySession` (actual Android display lifetime) and `CaptureSession` (ImageReader or MediaCodec Surface lifetime) are separate. `replaceSurface()` can rebind without destroying the App task; ownership and codec reset on real devices remain untested.
5. `SENT` means a local input backend reported successful dispatch, **not** the user-visible postcondition. Encoded PTS and local RGBA frame counters do not prove post-input capture causality. The capture barrier and C11 Action Owner binding remain pending.
6. Capture consumers may not retain a borrowed codec buffer. H.264 packets are owned byte copies, with bounded credit and explicit error on capacity exhaustion; no silent output drop.
7. Permission changes, OS/API incompatibility, shell-helper process death, input on display 0, resize/geometry changes, multiple App tasks and IME isolation all require physical-device conformance before merging.

## Next implementation gates

- Finish trusted app `app_process` bootstrap, nonce + pinned signature, Binder handoff, process death cleanup, scoped grants.
- Bind the existing C11 ActionIntent/ActionReceipt M0 PR #9 semantics and Action Owner #7 without another statechart.
- Device proof: virtual display N → Settings launch → ImageReader/MediaCodec captures → targeted click → **new** frame evidence → unchanged user main display.
- Verify 2 displays, rotation and geometry ABA, stop/drain, device lost, secure content, Android 14/15/16 OEMs.

Tracking [Issue #16](https://github.com/qigao/turbo-interact/issues/16) and [Draft PR #17](https://github.com/qigao/turbo-interact/pull/17).
