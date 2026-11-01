# M5c — Android JNI binding to canonical C11 ActionOwner

**Status:** source integrated in [Draft PR #21](https://github.com/qigao/turbo-interact/pull/21), stacked on native [#20](https://github.com/qigao/turbo-interact/pull/20) and with a merge parent from Android [#17](https://github.com/qigao/turbo-interact/pull/17). Current code-qualified HEAD `74bca9ac3dc1bb90422faedc42da13b3c1be7d05`: [Android AAR/NDK/JVM #41266990739](https://github.com/qigao/turbo-interact/actions/runs/41266990739) SUCCESS; [native LLVM gates #41266990767](https://github.com/qigao/turbo-interact/actions/runs/41266990767) 4/4 SUCCESS; Mermaid #41266990740 SUCCESS.

## Dependency and owner graph

```text
Trusted host App / Agent ToolCaller
  |
  |  typed NativeInputAction; live host policy
  v
NativeAndroidActionOwner (Kotlin facade; per-owner close/revoke)
  |  JNI; same native in-process owner
  v
ti_action_owner_submit (C11, shared with model/sandbox)
  |---- validate plan/provider/deadline/evidence/display/geometry
  |---- live Kotlin policy via JNI; consume attempt exactly once
  |---- ti_android_shell_backend_dispatch
            |---- Binder displayGeometry(ref) => exact current dimensions
            |---- issue native one-use in-flight ticket
            |---- ITurboDisplayShell.tap / key(BACK)
                       |
                       v
               Shell UID 2000 / AndroidSdkInput
                       |---- reverse Binder HostDisplayGrants.allowInput
                                    |
                                    v
                         CanonicalHostGrants.confirmInFlight
                           (native ticket consumed once; no 2nd owner)
                       |---- injection targets only owned VirtualDisplay N
            |---- SENT only if a confirmed ticket was consumed
  |
  +---- ActionReceipt (backend accepted/failed/UNKNOWN != task success)
```

## Exact source contracts

- `NativeInputAction`: explicit typed request (origin MODEL/SANDBOX/SCRIPT, C11 action kind TAP/BACK, app/device session, display and geometry generations, action/attempt IDs, evidence/capture and deadline). Internal JNI 17-cell representation is private to the AAR; C11 preflight is still authoritative.
- `NativeActionJni.open`: creates exactly one `ti_action_owner` and uses C11 `ti_android_shell_backend_dispatch` as the dispatch callback; no other native action policy/ledger.
- Kotlin peer `grant` delegates to a user-supplied live `LiveNativeInputPolicy`, deny on exceptions. `geometry` must query `ITurboDisplayShell.displayGeometry` via the authenticated Binder and exact `DisplayRef`, not reuse stale capture dimensions.
- Native `tap/key` creates an in-flight ticket **after** the C11 owner accepted/consumed the attempt, and the authenticated Shell reverse Binder must consume it exactly once. A Binder `SENT` without that confirmation becomes `UNKNOWN`, not completed.
- Lifecycle control `CanonicalHostGrants` is deliberately distinct: display create/launch are explicitly approved by `HostSessionControlPolicy`. The current C11 `ti_action_kind` contains input actions only. No default allow, no main display input, no auto-replay, and revocation is terminal for the session.
- JNI owner reference is borrowed/pinned around calls, never held under the Kotlin monitor while crossing Binder. `tryClose` refuses when in-flight and C11 owner refuses destroy while busy. `onShellTerminated` revokes host input authorization.
- R8 JNI symbols and method IDs protected by product AAR `jni-keep.pro` metadata.

## CI and acceptance

SimpleDSL 0.3.0 / Gradle 9.1 / AGP 9.0.1 / Kotlin 2.2.10 / JDK 21 / SDK36 / NDK 27.2 / CMake 3.22.1. Existing Android workflow builds the real product AAR, JNI `.so` for arm64-v8a/armeabi-v7a/x86/x86_64 and existing JVM unit tests. Root C11 workflow still covers Linux/macOS runner Clang, Windows llvm-mingw, Linux ASan/UBSan. Android workflow paths include root CMake/include/src changes because AAR links those files, but exclude documentation-only edits.

**No real Android device/emulator, ADB/USB, installer/consumer project or probe CI/CMake.** Green source checks do not prove an actual helper/host Binder handshake. Absent real-device tests are not a blanket merge blocker, but Android runtime success cannot be asserted.

Remaining: embedding App's authorized ADB execution/nonce handoff, actual JNI/Binder callback device execution, authenticated Sandbox process launch, host-qualified action-to-post-frame compositor barrier, full TouchLease/multi-pointer and revoke races on device; track [Issue #19](https://github.com/qigao/turbo-interact/issues/19).
