package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Real product host lifecycle gate, tested in the existing Android Library
 * JVM source set (not a consumer APK or simulated device/ADB).
 */
class EmbeddedHostCallGateTest {
    @Test fun firstCloseDeniesNewCallsAndRequiresInFlightDrain() {
        val gate = EmbeddedHostCallGate()
        assertTrue(gate.borrow())
        assertEquals(HostCloseAdmission.ATTEMPT, gate.startClose())
        assertFalse(gate.borrow())
        assertFalse(gate.idle())
        gate.finishClose(false)
        assertFalse(gate.borrow()) // No reopening across BUSY retry.
        gate.release()
        assertEquals(HostCloseAdmission.ATTEMPT, gate.startClose())
        assertTrue(gate.idle())
        gate.finishClose(true)
        assertEquals(HostCloseAdmission.ALREADY_TERMINATED, gate.startClose())
    }

    @Test fun concurrentCloseCannotReleaseResourcesTwice() {
        val gate = EmbeddedHostCallGate()
        assertEquals(HostCloseAdmission.ATTEMPT, gate.startClose())
        assertEquals(HostCloseAdmission.OTHER_CLOSE_ACTIVE, gate.startClose())
        assertTrue(gate.idle())
        gate.finishClose(true)
        assertEquals(HostCloseAdmission.ALREADY_TERMINATED, gate.startClose())
    }

    @Test fun releaseRequiresAnOwnedHostBorrow() {
        val gate = EmbeddedHostCallGate()
        try {
            gate.release()
            throw AssertionError("allowed foreign resource release")
        } catch (_: IllegalStateException) { }
        assertTrue(gate.borrow())
        gate.release()
        assertTrue(gate.idle())
    }
}
