package io.turbointeract.android

/**
 * Frame lineage from one explicitly bound RGBA capture on one owned display
 * incarnation. receivedAtNs is the host monotonic sampling time, NOT a
 * compositor fence and NOT necessarily the ImageReader image timestamp.
 */
data class CaptureFrameStamp(
    val display: DisplayRef,
    val captureSessionId: String,
    val sequence: Long,
    val receivedAtNs: Long,
) {
    init {
        require(display.deviceSessionId > 0 && display.sessionGeneration > 0 &&
            display.displayId > 0 && display.displayGeneration > 0 &&
            display.geometryGeneration > 0)
        require(captureSessionId.isNotBlank() && sequence > 0 && receivedAtNs > 0)
    }
}

/**
 * Input-admission lineage check: source evidence must originate in the
 * current ImageReader. This is not a privilege credential or compositor fence.
 */
internal object CaptureSourceLineageGate {
    fun matches(currentDisplay: DisplayRef?, captureSessionId: String,
                latestSequence: Long, source: CaptureFrameStamp): Boolean =
        currentDisplay == source.display &&
        captureSessionId == source.captureSessionId &&
        latestSequence >= source.sequence && latestSequence > 0L
}

/**
 * No VERIFIED state intentionally exists: neither a successful input Binder
 * result nor a later ImageReader callback proves compositor causal order, or
 * that the target app achieved the requested UI postcondition.
 */
enum class PostActionFrameState {
    INPUT_NOT_BACKEND_TERMINAL,
    CAPTURE_SOURCE_CHANGED,
    NO_NEW_FRAME_AFTER_DISPATCH,
    FRAME_OBSERVED_CAUSALITY_UNPROVEN,
}

/**
 * Read-only observation gate. Construct from the same captured frame used as
 * the action's input evidence; evaluate a newly sampled frame only after the
 * canonical C11 ActionOwner returns. No action retry or busy waiting here.
 *
 * A frame arriving after dispatch may have been queued earlier by Surface/
 * ImageReader. This gate MUST NOT promote such a frame to task completion.
 */
class PostActionFrameBarrier(
    val action: NativeInputAction,
    val preAction: CaptureFrameStamp,
    val receipt: NativeInputReceipt,
    val submitReturnedAtNs: Long,
) {
    init {
        require(action.display == preAction.display) { "input and capture display incarnations differ" }
        require(action.evidenceId > 0 && action.captureSequence == preAction.sequence) {
            "input must reference the exact pre-action capture"
        }
        require(submitReturnedAtNs >= preAction.receivedAtNs && submitReturnedAtNs > 0) {
            "dispatch and frame must use the same monotonic host clock"
        }
    }

    fun observe(next: CaptureFrameStamp?): PostActionFrameState {
        // C11: COMPLETED=1, BACKEND_TERMINAL=3. A transport ACK, failed,
        // cancelled or UNKNOWN attempt cannot qualify as an input effect.
        if (!receipt.admitted || receipt.outcome != 1 || receipt.level != 3) {
            return PostActionFrameState.INPUT_NOT_BACKEND_TERMINAL
        }
        if (next == null) return PostActionFrameState.NO_NEW_FRAME_AFTER_DISPATCH
        if (next.display != preAction.display ||
            next.captureSessionId != preAction.captureSessionId) {
            return PostActionFrameState.CAPTURE_SOURCE_CHANGED
        }
        if (next.sequence <= preAction.sequence || next.receivedAtNs <= submitReturnedAtNs) {
            return PostActionFrameState.NO_NEW_FRAME_AFTER_DISPATCH
        }
        return PostActionFrameState.FRAME_OBSERVED_CAUSALITY_UNPROVEN
    }
}
