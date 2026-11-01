package io.turbointeract.android

import android.graphics.Bitmap

/**
 * A host-supplied visual postcondition can establish that a desired UI state
 * was seen in a later frame, but cannot exclusively attribute that change to
 * the preceding input. No state here means compositor causal verification.
 */
enum class VisualPostconditionResult {
    NOT_EVALUATED,
    MATCHED_CAUSALITY_UNPROVEN,
    NOT_MATCHED,
    EVALUATION_FAILED,
}

/** No bitmap is retained; the callback's borrowed image is recycled. */
data class CapturedInputObservation(
    val inputReceipt: NativeInputReceipt,
    val frameState: PostActionFrameState,
    val visualPostcondition: VisualPostconditionResult,
    val afterCaptureSequence: Long?,
)

internal object VisualPostconditionGate {
    fun evaluate(
        frameState: PostActionFrameState,
        matcher: () -> Boolean,
    ): VisualPostconditionResult {
        if (frameState != PostActionFrameState.FRAME_OBSERVED_CAUSALITY_UNPROVEN) {
            return VisualPostconditionResult.NOT_EVALUATED
        }
        return try {
            if (matcher()) VisualPostconditionResult.MATCHED_CAUSALITY_UNPROVEN
            else VisualPostconditionResult.NOT_MATCHED
        } catch (_: Exception) {
            VisualPostconditionResult.EVALUATION_FAILED
        }
    }
}

/**
 * Production one-shot host integration on an already-created VirtualDisplay.
 *
 * Calls the SAME C11 ActionOwner as the sandbox IPC, dispatches through live
 * Binder grants, waits for a newer frame from the actual bound ImageReader,
 * then invokes the embedding host's task-specific predicate on an owned
 * Bitmap copy. Only the host knows the expected target-App UI postcondition.
 *
 * The predicate must return synchronously and must NOT retain the Bitmap;
 * caller runs this on a dedicated IO worker and owns the 'before' frame.
 * On UNKNOWN or rejected input, no frame can promote the attempt to success.
 */
object CapturedInputObservationRunner {
    fun run(
        owner: NativeAndroidActionOwner,
        capture: AndroidRgbaCapture,
        action: NativeInputAction,
        before: CaptureFrameStamp,
        waitForNewFrameMs: Long = 3_000L,
        visualPostcondition: (Bitmap) -> Boolean,
    ): CapturedInputObservation {
        require(waitForNewFrameMs in 1L..15_000L)
        require(capture.ownsSource(before)) {
            "input source frame belongs to a foreign or closed ImageReader"
        }
        val barrier = owner.submitWithFrameBarrier(action, before)
        val receipt = barrier.receipt
        // No waiting for input which was not conclusively backend-terminal.
        val canObserve = receipt.admitted && receipt.outcome == 1 && receipt.level == 3
        val later = if (canObserve) capture.awaitFrameAfter(
            before, barrier.submitReturnedAtNs, waitForNewFrameMs
        ) else null
        try {
            val state = barrier.observe(later?.stamp)
            val result = VisualPostconditionGate.evaluate(state) {
                visualPostcondition(checkNotNull(later).bitmap)
            }
            return CapturedInputObservation(receipt, state, result, later?.sequence)
        } finally {
            later?.bitmap?.recycle()
        }
    }
}
