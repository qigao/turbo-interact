# M5e — Existing-host Android runtime composition

**Status: source integrated; real installed App / Android hardware not qualified.**
This file documents library APIs to call **inside an existing embedding host**
that already has an authenticated, user-authorized ADB execution transport.
It is **not** a new App/installer/consumer project and not a device CI plan.

[PR #21](https://github.com/qigao/turbo-interact/pull/21)
· [Issue #19](https://github.com/qigao/turbo-interact/issues/19)
· [Issue #22](https://github.com/qigao/turbo-interact/issues/22)

## Exact runtime sequence

1. Existing host APK opts into AGP
   `android.packaging.jniLibs.useLegacyPackaging = true`. The production AAR
   contributes both JNI and an ABI-specific PIE
   `libturbo_interact_sandbox_worker.so`. Before launching a worker, call
   `TrustedSandboxWorkerInstallation.inspect(applicationContext)` on the
   **installed host**; `READY_FOR_EXEC_ATTEMPT` is a *preflight*, not an
   observed `execve` success. No App-writable native executable fallback.
2. Existing host constructs `CanonicalHostGrants` with its **real**
   `HostSessionControlPolicy`, and calls
   `TurboDisplayHandoffHost.begin(app, deviceSessionId, sessionGeneration,
   grants)`. Input starts **denied** until the one C11 owner is bound.
3. On a host **IO** thread call
   `AuthorizedShellHostConnector.connect(host, authorizedAdbTransport)`.
   The transport is an explicitly authorized caller-supplied implementation
   of `AuthorizedAdbShellTransport`. It must send `ShellLaunchSpec.command`
   to its already authorized ADB execution service and `nonceLine` to that
   invocation's **stdin**, promptly returning its process/session lease.
   No nonce in logs/argv/env/files. The connector waits up to 15 seconds by
   default for the **real authenticated Binder offer** and revokes the host
   and transport on expiry/cancel/failure. No implicit ADB pairing, shell
   restart, default permission, or display-0 fallback.
4. Allocate `AndroidRgbaCapture(width, height)` in the App host and on an
   IO thread call `AuthorizedRgbaDisplayCreator.create(connection, capture,
   geometry, deviceSessionId, sessionGeneration)`. This calls the actual
   `ITurboDisplayShell.createDisplay` and validates the returned exact
   device/session/display/geometry incarnation. It binds the ImageReader
   capture to that incarnation; wrong/partial reply fails closed.
5. Create **one** `NativeAndroidActionOwner` with the authenticated
   `connection.shell`, returned `snapshot.ref`, explicit live
   `LiveNativeInputPolicy`, provider/capability/plan identities. Bind it
   once using `grants.bindNativeOwner(owner)`. Never create a separate
   owner for worker script requests. Launch the **explicitly chosen** target
   package on `snapshot.ref` through the already authenticated
   `connection.shell.launchApp(displayId, displayGeneration,
   geometryGeneration, packageName, launchActionId)`; the separate
   `HostSessionControlPolicy.allowLaunch` must approve it. A `STATUS_SUBMITTED`
   result only means launch was requested, **not** that the App is visible or
   ready. Reject DENIED/UNKNOWN and do not silently launch Settings or fall
   back to the main screen.
6. Obtain a real `capture.latestFrame()` from the bound ImageReader **after
   checking the intended target App is visible in that frame**.
   A typed `NativeInputAction` for a TAP must reference that exact
   `stamp.display` and `stamp.sequence`, a valid evidenceId, new action
   ID/attempt ID, live plan generation and monotonic deadline. The App
   submits it through `CapturedInputObservationRunner.run(...)`: C11
   admission and one-use native ticket → Binder input → bounded later RGBA
   sample → host-supplied visual predicate. The predicate must not retain its
   temporary bitmap. `MATCHED_CAUSALITY_UNPROVEN` means an expected UI
   state was *seen*, not that the tap exclusively caused it. A failed,
   cancelled, transport-only or UNKNOWN input never passes this gate.
7. To qualify **the actual packaged worker**, build the reviewed typed
   command with `TrustedSandboxCommand.forSandboxAction(action, callId,
   taskId, sandboxRunId)` (origin **SANDBOX only**), then on an App IO
   thread call `TrustedSandboxCommandRunner(app, owner).run(command)`.
   The caller uses the **same** canonical owner. The runner launches only
   the packaged ABI-specific reviewed worker, delivers socket/token via
   stdin, and serves one Unix RPC through native kernel peer UID checks.
   Its `SandboxProcessReceipt` is an RPC/process result, *not* a visual task
   result. Never expose this same-UID worker as untrusted code isolation.
8. Teardown: revoke owner and host grants, terminate/reap the approved direct
   child, drain busy callbacks before `owner.tryClose()`, release exact
   `snapshot.ref` display with `connection.shell.releaseDisplay`, close
   the capture Surface, then close the authorized Shell transport lease.
   Shell death invalidates the session and all action attempts. No replay of
   UNKNOWN; do not silently rebind to reused display IDs.

## Single-owner session facade

The existing host may now use `EmbeddedAndroidHostSession.open(...)` on a
dedicated IO thread instead of manually composing steps 2–5. It creates
`CanonicalHostGrants`, performs the authorized ADB/nonce/Binder handoff,
creates the actual RGBA Surface and VirtualDisplay, creates **one** JNI/C11
ActionOwner and binds it to the same live grants. It does not launch ADB,
pair a device, install an APK, or accept arbitrary model shell commands.

The returned `EmbeddedAndroidHostSession` exposes `launchTarget`,
`observeInput` and `runTrustedSandboxAction` against that owner. The
embedding host must still explicitly choose the package, supply live
policy, and verify the input's source frame before invoking it. A live
ImageReader capture session and its own sequence must match **before**
a C11/Binder input is sent; numeric DisplayRef/sequence alone are not proof
of source-frame provenance.

Call `tryClose()` on an IO thread. It first revokes input and cancels
the worker, then refuses a premature native destroy while any host call
or JNI callback remains outstanding; retry `tryClose` after calls drain.
Only after native destruction does it release the exact owned display,
close the authenticated connection, and dispose of the ImageReader
Surface. `teardownReport` distinguishes whether `releaseDisplay` returned
true, and whether the transport close returned normally. Neither
proves a remote Android compositor released a display if the Binder died.

If `createDisplay` fails or returns a wrong session/generation, the
library closes the authenticated Shell session, which owns all displays.
It must **not** release the guessed ID/generation from a malformed reply,
which could otherwise destroy a different valid display.

## Source and product gates

| Gate | Product source/CI | Runtime host acceptance still needed |
| --- | --- | --- |
| R8/minification | AAR consumer rules preserve exact `TurboDisplayShellMain.main(String[])` and JNI peer. Existing Android workflow reads actual AAR `proguard.txt` | Install an **R8-minified** existing host APK and observe `app_process` class load |
| Native PIE | Four-ABI AAR contents, Android SDK preflight and real POSIX C11 process tests | Final host APK extracts ABI worker; device App can `execve` it |
| Shell identity | One-shot nonce, UID/PID/certificate Binder checks and source tests | Actual host-authorized ADB stdin handoff and live Shell Binder callback |
| Display | Exact `DisplayRef`/geometry reply, no display 0 | VirtualDisplay focus/IME/capture on real Android |
| Input + visual | Canonical owner, one-use ticket, post-input sample and predicate with conservative statuses | Observe real targeted tap/BACK and task-specific UI state |
| Causal compositor ordering | No compositor fence and no task-success claim | Independent render/input synchronization evidence before asserting causality |
| Untrusted scripts | Only reviewed same-UID one-shot C11 worker | OS-isolated separate principal, Issue #22 |

**Explicit exclusions:** no new emulator/physical-device/USB/ADB CI; no
consumer, installer or probe projects; no permissive host grants; no general
shell executor; no alternate ActionOwner or native engine.

The CI demonstrates buildable product source and contracts, not any real
Android installation, Binder effect, compositor causal ordering or OS-isolated
untrusted-code sandbox.
