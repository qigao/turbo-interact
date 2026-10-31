# Mika background-display source snapshot

A **verbatim, selective** code import from [wangzan101/Mika](https://github.com/wangzan101/Mika/tree/279a5cae405ac83106b47af9c2794ebfa3333a57) at `279a5cae405ac83106b47af9c2794ebfa3333a57`.

* License: Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
* Original scrcpy attribution and other upstream third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). `ShellContext.kt` carries its original scrcpy-derived header.
* Exact upstream paths, Git blob identities and matching status: [IMPORT_MANIFEST.json](IMPORT_MANIFEST.json). **All 36 imported blobs are byte-for-byte identical** to the pinned upstream.
* Original package names and symbols have not been changed. These files remain under `third_party/mika/` and are **excluded from builds**.

## What is included

Shell helper/Binder/AIDL, VirtualDisplay ownership and app launch, ImageReader frame capture, Accessibility window/action, OCR and Android instrumented tests. This intentionally **does not include all Mika app/core dependencies**, training agents, UI, installer, or runtime resources; the snapshot is not standalone compilable. This is an auditable migration baseline, **not evidence that TurboInteract supports Android today**.

## Migration policy

Refactor into a separate production Android device library (Android framework layer) and C11 TurboInteract provider/Action Owner (host layer); avoid porting Android hidden APIs to C11. Runtime code must never directly link these unmodified files. Preserve Apache-2.0 attribution on derivatives. Privileged `app_process` must be explicitly authorized and authenticated and must not expose Mika's broad command/clipboard/audio APIs. Runtime grants are deny-by-default.

Specifically, copied upstream `ShellService.injectTap()` / `injectKey()` do not perform the same owned-display check as `injectStroke()`: **the new adapter must reject display 0 and unknown/stale display generations at every input entry**. The vendor code is not itself an acceptable privileged input gateway.

Tracking: [TurboInteract Issue #16](https://github.com/qigao/turbo-interact/issues/16) and [Action Owner #7](https://github.com/qigao/turbo-interact/issues/7).
