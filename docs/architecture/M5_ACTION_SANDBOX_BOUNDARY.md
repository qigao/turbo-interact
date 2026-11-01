# M5: Unified Action Owner and sandbox phone bridge

**Implementation:** [Draft PR #18](https://github.com/qigao/turbo-interact/pull/18), stacked on [M0 PR #9](https://github.com/qigao/turbo-interact/pull/9). C11 source/contract implementation; **not** an installed Android integration or a complete Linux sandbox or Agent loop.

## Mika source-derived architecture

Reviewed [Mika @ 279a5cae](https://github.com/wangzan101/Mika/commit/279a5cae405ac83106b47af9c2794ebfa3333a57):

- `core/agent/AgentLoop.kt` calls the sole `core/action/ActionExecutor.kt`: all side effects go through guards, approvals and run records.
- `app/cap/sandbox/SandboxBridge.kt` opens a per-command random Unix-domain socket and token, validates the peer UID and token, then routes allowed phone tools through the same ActionExecutor using `core/cap/sandbox/SandboxTools.kt`.
- `app/cap/sandbox/ProcRunner.kt` and `app/src/main/cpp/mikarun/mikarun.c` bound Alpine/PRoot command resources; the AgentLoop itself is Kotlin outside PRoot.
- PRoot and the loader are GPL-2.0-or-later, **not imported**. Mika's `mikarun.c` source header states Apache-2.0 while `tools/sandbox/README.md` lists MIT; verify attribution before reuse.

**Decision:** reuse the single-authority architecture but not import Mika's sandbox/agent runtime into TurboInteract Android AAR. Device code belongs to [Android Draft PR #17](https://github.com/qigao/turbo-interact/pull/17); C11 ActionOwner and the script-to-host gate are separate.

## Runtime trust flow

```text
Model ToolCall -----------------------+
                                      |
Sandbox Python/Shell -- Unix socket --|-- OS peer UID + random per-run token
                          (not yet    |   (host socket adapter: NOT IMPLEMENTED)
                           wired)     |
                                      v
                          ti_sandbox_bridge_execute
                             (sandbox/script only)
                                      |
                                      v
                            ti_action_owner_submit
                       (exact display/session/geometry)
                       (evidence, deadline, replay guard)
                       (LIVE host grant for every attempt)
                                      |
                                      v
                          Android device Binder (PENDING)
                                      |
                              backend receipt / UNKNOWN
                                      |
                       post-action frame evidence (PENDING)
```

The bridge never directly injects Android events, launches apps, or grants permission. Its `origin` field is **not authentication**; the upstream adapter must obtain the peer UID from socket credentials and bind a strong random token to the running task. The action callback must recheck live authority independently.

## Source and acceptance

| Component | Source | Contract |
| --- | --- | --- |
| Action Owner | `include/turbo_interact/action_owner.h`, `src/action_owner.c` | C11 immutable display ref, trusted plan/provider snapshot, live callback, bounded attempt replay defense |
| Sandbox phone gate | `include/turbo_interact/sandbox_bridge.h`, `src/sandbox_bridge.c` | exact token, OS-verified UID supplied by adapter, task/run/call IDs, tap/back allow-list; delegates to ActionOwner |
| Tests | `tests/test_action_owner.c`, `tests/test_sandbox_bridge.c` | no side effects on authorization failure; stale IDs, replay, UNKNOWN, revocation, host-model reuse after sandbox closure |

**Key invariants:** display 0 is rejected, display recreation and geometry changes invalidate old targets, and sandbox/script actions cannot bypass live host grants. A claimed `(action_id, attempt_id)` cannot be replayed even if dispatch outcome is `UNKNOWN` or origin changes. One effect is in flight per owner. Owner revocation prevents new admissions but cannot retract an already dispatched hardware effect; backend completion must not be mistaken for verified task success.

## Still missing

An actual authenticated Unix-socket transport, peer credential extraction, cryptographic token generation, bounded JSON parsing, C11-to-Android `ITurboDisplayAuthority` mapping, TouchLease / multi-touch, compositor-causal post-action frame barrier, and full Agent task/orchestration integration remain **not implemented**. This work is a source-level gate, not a claim of working Android screen operation.

## CI

Only the existing native workflow: Linux/macOS runner-native Clang; Windows pinned llvm-mingw; Linux Clang ASan/UBSan. Real product C11 build, CTest and production header/library install. **No MSVC, no installer/consumer projects, no probe-only CI/CMake, no emulator or physical-device CI.** Real-device compatibility is separate optional product validation and not a blanket PR merge blocker.
