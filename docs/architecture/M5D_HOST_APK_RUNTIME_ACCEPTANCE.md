# M5d — Embedding host APK and post-input evidence acceptance

**Status:** source contract only, not Android device-qualified. This is an
integration procedure for an existing trusted embedding App, NOT a new
installer/consumer/probe project and NOT a CI device/emulator requirement.

Preceding source gates: [PR #21](https://github.com/qigao/turbo-interact/pull/21),
[Issue #19](https://github.com/qigao/turbo-interact/issues/19),
[Issue #22](https://github.com/qigao/turbo-interact/issues/22).

## A. Host APK extraction is a distinct gate from AAR packaging

The AAR contains compiled `libturbo_interact_sandbox_worker.so` for the four
ABIs. That does **not** ensure the final host APK retains and extracts it.
Because Android 10+ disallows executing an App-written file in its own private
data directory, the worker is a reviewed, packaged PIE binary deliberately
named `lib*.so`, executed directly from the OS-managed
`ApplicationInfo.nativeLibraryDir`. Do not copy it into `filesDir`/`cacheDir`.

The **embedding host application** (not this Android library's Gradle project)
must configure its final APK native packaging, for example in AGP Groovy:

~~~groovy
android {
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}
~~~

This requests native extraction on installation. Check the *actual installed*
application; do not infer behavior from an AAR, Gradle default or an uninstalled
APK. The host's runtime preflight is:

~~~kotlin
val status = TrustedSandboxWorkerInstallation.inspect(applicationContext)
check(status == TrustedWorkerInstallStatus.READY_FOR_EXEC_ATTEMPT) {
    "Installed host APK worker preflight failed: " + status
}
~~~

The preflight checks `ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS`, the final
`nativeLibraryDir`, canonical ownership of the fixed worker name, and that
the extracted installed file is executable and not App-writable. Missing,
symlink-escaped, writable or nonexecutable workers fail closed. READY means
*it is permissible to attempt a real OS exec*, **not** that the binary has
executed successfully on this device. An embedding host may need to handle
ABI splits. No arbitrary worker path, shell command, App-writable executable,
new APK or ADB capability is introduced by this API.

**On-device product acceptance A1 (not CI):** inspect the installed App flags,
actual nativeLibraryDir, installed ABI and worker path; execute exactly the
packaged one-shot worker from the App process using
`TrustedSandboxCommandRunner`, while its canonical owner and sandbox IPC
socket are live. Record the process exit/timeout classification and actual
server accepted/rejected call count. No token/path should appear in argv,
logcat, artifacts, or process environment. One accepted IPC call alone does
not prove screen input or task success.

## B. Canonical owner / Shell / Binder input

The existing embedding host supplies an explicitly authorized ADB transport,
the real package name and authorized launch, and live host grants. There is
no library-provided implicit ADB pairing or shell escalation.

1. Create `CanonicalHostGrants` and `TurboDisplayHandoffHost.begin`;
   explicitly authorize display creation and launch. `launchSpec()` issues
   a fresh nonce; pass `command` only to the already trusted ADB exec path
   and `nonceLine` only through its stdin (never log/store them).
2. After the UID/PID/certificate-pinned one-shot `app_process` Binder
   handoff, require `connectedShell()`; create the display with a host-owned
   RGBA `Surface`. Reject display 0, missing generations and mismatched
   response. Keep the exact returned `DisplayRef`.
3. Construct one `NativeAndroidActionOwner` with an actual live
   `LiveNativeInputPolicy`. Bind this same instance to
   `CanonicalHostGrants`; all direct and trusted-child actions must use it.
4. Submit a uniquely identified, observation-bound TAP or BACK. Native C11
   admission, live geometry, JNI ticket, reverse Binder `allowInput` and
   shell-side attempt ledger all have to agree. Never re-submit an
   `UNKNOWN` attempt. Do not promote Shell `SENT` into task success.
5. Exercise both the direct submission and one genuine
   `TrustedSandboxCommandRunner` child using the reviewed fixed
   `libturbo_interact_sandbox_worker.so` command grammar. The child
   receives the path/token on stdin and is the **same App UID**, not a
   security boundary for untrusted scripts. A call to the real C11 socket
   and live Binder admission must be observed for full product qualification.

**On-device acceptance B1 (not CI):** record current session/generations,
ActionOwner action/attempt identity, shell process uid/pid, dispatch status,
single reverse-Binder grant consumption and the isolated display ID.
Deliberately reject wrong generation, duplicate attempt, missing permission,
expired nonce, shell death and display 0. Never log the handoff nonce or
sandbox token. Whether the target App actually reacts remains separate.

## C. ImageReader evidence and honest causality

The RGBA capture Surface is allocated before display creation. After
`createDisplay` returns a valid display incarnation, reconstruct the
`DisplaySnapshot` from its exact response and the supplied geometry, then
call `AndroidRgbaCapture.bindDisplay(snapshot)` **once**. Its copied frames
now carry a nullable `Frame.stamp` with exact `DisplayRef`, random
capture-session ID, capture sequence and host `elapsedRealtimeNanos` sampled
when the frame arrived. Frames obtained before binding have no usable stamp.

For a TAP, use a pre-action frame's stamp as the actual source of the action's
`captureSequence` and supply nonzero evidence ID, matching
`NativeInputAction.display`. Then call:

~~~kotlin
val before = capture.latestFrame() ?: error("No pre-action image")
try {
    val stamp = before.stamp ?: error("Capture not bound to a display")
    // input: NativeInputAction with display == stamp.display,
    // captureSequence == stamp.sequence and a real live evidenceId.
    val gate = owner.submitWithFrameBarrier(input, stamp)
    val after = capture.latestFrame()
    try {
        val state = gate.observe(after?.stamp)
        // FRAME_OBSERVED_CAUSALITY_UNPROVEN is NOT task VERIFIED.
        // Obtain independent, app-visible postcondition evidence.
    } finally {
        after?.bitmap?.recycle()
    }
} finally {
    before.bitmap.recycle()
}
~~~

This gate refuses mismatched device/session/display/geometry generation,
display-ID reuse, capture-session replacement, stale frame sequence, frames
sampled no later than the submission return time and any input receipt
without C11 `COMPLETED + BACKEND_TERMINAL` semantics. `UNKNOWN` is terminal and
never becomes success because a newer frame appeared.

**Important:** `Image.timestamp`, `elapsedRealtimeNanos`, and an incrementing
ImageReader sequence are not a compositor/input fence. A queued frame may
arrive after dispatch despite being rendered earlier. Therefore the strongest
result this API can currently report is
`FRAME_OBSERVED_CAUSALITY_UNPROVEN`. There is deliberately no `VERIFIED`
state. Rotations, display recreation and Surface replacement need a new
bound capture rather than silently reusing old frame ancestry.

**On-device acceptance C1 (not CI):** sample the actual newly rendered
VirtualDisplay stream after a successful targeted input and check a
task-specific postcondition (for example an expected Settings UI state).
Keep the observation, user main-display state, generation and input receipt
together. A strong compositor-causal claim needs an independently validated
render/input synchronization mechanism; visual postcondition alone cannot
establish exclusive causal attribution to the tap.

## D. Evidence and status separation

| Gate | Existing CI/source can prove | Actual host/device evidence still needed |
| --- | --- | --- |
| AAR artifact | JNI and worker present under four `jni/<abi>` paths | Final APK keeps ABI-specific worker, extracts it, `execve` succeeds |
| Sandbox | POSIX worker/IPC contracts, kernel UID/token checks | Installed Android App launches it, same-owner C11 call executes |
| Shell input | AIDL/JNI and live-ticket deterministic contracts | Real Android `app_process` handshake and reverse Binder input |
| Capture | Frame lineage and conservative post-action classification | Real compositor ordering and task-specific postcondition |
| Security | Allowlisted same-UID reviewed worker | OS-isolated untrusted-code provider per Issue #22 |

No new physical-device/emulator/USB/ADB CI, installer/consumer project or
probe-only CMake target. PR #21 stays Draft until its integration review
decision; source CI green is never described as on-device proof.
