package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Test

/** Deterministic source-contract tests; these are NOT emulator/compositor tests. */
class PostActionFrameEvidenceTest {
    private val ref = DisplayRef(41L, 3L, 7, 2L, 5L)
    private val before = CaptureFrameStamp(ref, "capture-session-a", 6L, 100L)
    private val action = NativeInputAction(
        ref, NativeInputAction.MODEL, NativeInputAction.TAP,
        actionId = 8L, attemptId = 1L, evidenceId = 9L, captureSequence = 6L,
        planGeneration = 2L, deadlineNs = 10_000L,
        xNormalized = 100_000, yNormalized = 400_000
    )

    private fun barrier(receipt: NativeInputReceipt = NativeInputReceipt(0, 1, 3)) =
        PostActionFrameBarrier(action, before, receipt, 200L)

    @Test fun newFrameIsOnlyUnprovenObservationNotTaskSuccess() {
        val observed = CaptureFrameStamp(ref, "capture-session-a", 7L, 201L)
        assertEquals(PostActionFrameState.FRAME_OBSERVED_CAUSALITY_UNPROVEN,
            barrier().observe(observed))
        assertEquals(PostActionFrameState.NO_NEW_FRAME_AFTER_DISPATCH,
            barrier().observe(null))
        assertEquals(PostActionFrameState.NO_NEW_FRAME_AFTER_DISPATCH,
            barrier().observe(before))
        assertEquals(PostActionFrameState.NO_NEW_FRAME_AFTER_DISPATCH,
            barrier().observe(observed.copy(receivedAtNs = 200L)))
        assertEquals(PostActionFrameState.NO_NEW_FRAME_AFTER_DISPATCH,
            barrier().observe(observed.copy(sequence = before.sequence)))
    }

    @Test fun neverPromotesTransportAckUnknownOrFailedInput() {
        val observed = CaptureFrameStamp(ref, "capture-session-a", 7L, 250L)
        for (receipt in listOf(
            NativeInputReceipt(-5, 0, 0),
            NativeInputReceipt(0, 1, 1), // transport ACK only
            NativeInputReceipt(0, 1, 2), // accepted by agent only
            NativeInputReceipt(0, 2, 3), // terminal failure
            NativeInputReceipt(0, 3, 3), // cancellation
            NativeInputReceipt(0, 4, 3)  // UNKNOWN; no replay
        )) {
            assertEquals(PostActionFrameState.INPUT_NOT_BACKEND_TERMINAL,
                barrier(receipt).observe(observed))
        }
    }

    @Test fun displayAbaGeometryAndCaptureReplacementAreNeverAccepted() {
        val observed = CaptureFrameStamp(ref, "capture-session-a", 7L, 250L)
        for (wrong in listOf(
            observed.copy(display = ref.copy(deviceSessionId = 42L)),
            observed.copy(display = ref.copy(sessionGeneration = 4L)),
            observed.copy(display = ref.copy(displayGeneration = 3L)),
            observed.copy(display = ref.copy(geometryGeneration = 6L)),
            observed.copy(display = ref.copy(displayId = 8)),
            observed.copy(captureSessionId = "capture-session-b")
        )) {
            assertEquals(PostActionFrameState.CAPTURE_SOURCE_CHANGED,
                barrier().observe(wrong))
        }
    }

    @Test fun inputMustReferenceTheExactPreActionFrameAndClock() {
        val wrongActions = listOf(
            action.copy(display = ref.copy(geometryGeneration = 6L)),
            action.copy(captureSequence = 5L),
        )
        for (wrong in wrongActions) {
            try {
                PostActionFrameBarrier(wrong, before, NativeInputReceipt(0, 1, 3), 200L)
                fail("accepted mismatched action/frame evidence")
            } catch (_: IllegalArgumentException) { }
        }
        // NativeInputAction validates TAP evidence at construction, before
        // the separate post-action observation gate even exists.
        try {
            action.copy(evidenceId = 0L)
            fail("accepted a TAP without source evidence")
        } catch (_: IllegalArgumentException) { }
        try {
            PostActionFrameBarrier(action, before, NativeInputReceipt(0, 1, 3), 99L)
            fail("accepted a dispatch timestamp preceding the source frame")
        } catch (_: IllegalArgumentException) { }
    }
}
