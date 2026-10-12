# M1 GraphBridge Credit Slice — executable boundary

**Status:** Draft stacked [PR #10](https://github.com/qigao/turbo-interact/pull/10), based on M0 [PR #9](https://github.com/qigao/turbo-interact/pull/9). This is a deliberately small **in-process, one-owner-lane** bridge, not yet a concurrent cross-process transport or TurboMedia FrameStore implementation.

## 1. Event ownership

The bridge accepts only copied, pointer-free \`ti_bridge_event_v1\` values containing the event header and optionally a \`FrameRef\`. Any event that claims nonzero \`payload_bytes\` returns \`TI_EUNSUPPORTED\`: arbitrary owned observation/event payload transfer is **not yet implemented**.

\`\`\`text
owner prepare(event, optional frame pin)
    -> PREPARED (bridge owns event row and optional pin)
    -> commit -> READY (visible FIFO)
    -> take -> TAKEN (one consumer owns the delivery identity)
    -> settle -> FREE (release pin and record credit)

PREPARED / READY -- discard --> FREE
TAKEN -- discard --> BUSY (must settle)
seal: no new prepare/commit; existing READY can drain
destroy: only when sealed + all rows FREE
\`\`\`

All operations and pin-provider callbacks are **caller-serialized**, including init/destroy; the bridge does not create worker threads, locks or a secondary scheduler. Pin providers must not reenter the same bridge. Concurrent producers and process crashes are later extensions requiring different qualification.

## 2. FrameRef / pinned credits

\`FrameRef\` has no ownership. A pinned request calls the selected FrameStore adapter **before** admission to obtain one store-issued pin token. This token remains internal to the bridge until a consumer takes the ready row; \`ti_bridge_borrow_pin_token()\` exposes a borrowed lookup token for the active delivery only. Borrowed tokens are not authorization to release or duplicate the pin. The bridge invokes the provider's release callback exactly once after successful settlement/discard; release failure leaves the original row/pin owned, allowing explicit retry.

Each consumer requesting a frame pin must prepare its **own** bridge row, even if multiple consumers share the same FrameRef. Weak-frame messages do not reserve any pin and must tolerate stale frames at later lookup.

The Store adapter is an injected pair of callbacks, \`pin_reserve\` and \`pin_release\`; only tests provide a fake. The actual MediaFrameStore and FrameLease mapping remain TurboMedia StreamGraph v2 dependencies. A borrowed pin must not be retained beyond \`settle\`.

## 3. Generation and shutdown rules

The ticket identity contains bridge incarnation, row slot and row generation. Reusing a row increments its generation. A ticket from another incarnation or a retired row returns \`TI_ESTALE\`; a ready row cannot be settled by a stale/duplicated ticket. A FrameStore itself validates \`store_generation\` and \`slot_generation\` during pin reservation.

\`seal\` makes the bridge unavailable for new preparation/commit. It does not imply existing rows or external resource callbacks have quiesced. \`destroy\` returns \`TI_EBUSY\` until all PREPARED, READY and TAKEN credits are returned. Failed \`pin_release\` is not hidden: it preserves the row and requires another explicit attempt.

This prototype does not solve the race between simultaneous calls from foreign threads (it requires host serialization). In addition, a terminal Delivery receipt is *not* the Android Action Receipt or Task verification result.

## 4. Deterministic qualification

The Fake FrameStore fixture checks:
- same source frame pinned independently by two consumers; releasing one does not retire the other;
- row slot reuse and FrameStore generation reuse reject stale identities;
- queue exhaustion returns \`TI_EFULL\` rather than growing or silently dropping;
- pinned release failure keeps credit on the row and explicit retry releases it;
- seal refuses late prepare and commit; retained prepared/ready/inflight work blocks destroy;
- duplicate settle/discard, wrong incarnation, malformed FrameRef/Event all reject;
- unpinned weak-reference messages never acquire store credit;
- nonempty Event payloads fail until a true owned-payload transport is implemented.

CI must qualify Windows/Linux/macOS C11 native+installed C11/C++17 consumers and Linux ASan/UBSan. **These tests are deterministic owner-lane interleavings, not TSan proof of concurrent publication.**

## 5. Next required work

1. Integrate a real TurboMedia FrameStore lease provider once StreamGraph v2 is published; do not copy FFmpeg buffers into TurboInteract.
2. Add real bounded owned event payload schemas and model/Action result correlation IDs.
3. Define MPSC admission/Host serial dispatcher if cross-thread producers are required; do not claim this single-owner facade is thread-safe.
4. Add true cancellation/commit and teardown concurrency tests (ASan/TSan) plus GPU fence drain.
5. Add GraphBridge integration with installed Turbo-Flow \`.flow\` and TurboSCXML Host Event APIs. No hidden fallback.

Track upstream prerequisites in [#4](https://github.com/qigao/turbo-interact/issues/4) and bridge runtime qualification in [#3](https://github.com/qigao/turbo-interact/issues/3).

## 6. Owned event payload admission (additive slice)

The new `ti_bridge_create_with_payload_limits()` explicitly enables bounded, bridge-owned copies of serialized event content. Producers use `ti_bridge_prepare_owned()`, then `commit`; consumers retrieve a copy through `ti_bridge_read_payload()` only after `take`. Producer storage can be released after prepare. Inflight bytes are charged until discard/settle succeeds, including when a pin-release failure delays settlement. The pre-existing `ti_bridge_create()` and `prepare()` still reject nonzero payloads. This is transport ownership only, not schema-specific decoding, a concurrent MPSC implementation, or a production TurboMedia FrameStore. See the executable owned-observation test and installed C11/C++17 consumers.
