package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

/** Source-only rule: new frame/predicate != compositor causal proof. */
class VisualPostconditionGateTest {
    @Test fun matchingVisualStateRemainsCausalityUnproven() {
        assertEquals(VisualPostconditionResult.MATCHED_CAUSALITY_UNPROVEN,
            VisualPostconditionGate.evaluate(
                PostActionFrameState.FRAME_OBSERVED_CAUSALITY_UNPROVEN
            ) { true })
        assertEquals(VisualPostconditionResult.NOT_MATCHED,
            VisualPostconditionGate.evaluate(
                PostActionFrameState.FRAME_OBSERVED_CAUSALITY_UNPROVEN
            ) { false })
        assertEquals(VisualPostconditionResult.EVALUATION_FAILED,
            VisualPostconditionGate.evaluate(
                PostActionFrameState.FRAME_OBSERVED_CAUSALITY_UNPROVEN
            ) { throw IllegalStateException("predicate error") })
    }

    @Test fun unknownFailedAndStaleFramesNeverExecutePostcondition() {
        var invoked = false
        for (state in PostActionFrameState.entries) {
            if (state == PostActionFrameState.FRAME_OBSERVED_CAUSALITY_UNPROVEN) continue
            assertEquals(VisualPostconditionResult.NOT_EVALUATED,
                VisualPostconditionGate.evaluate(state) { invoked = true; true })
        }
        assertFalse(invoked)
    }
}
