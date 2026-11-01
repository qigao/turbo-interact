package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SandboxCancellationGateTest {
    @Test fun cancelCallsExactlyOneRegisteredWorkerOutsideItsLock() {
        val gate = SandboxCancellationGate()
        var called = 0
        val callback: () -> Unit = {
            called++
            assertTrue(gate.terminated())
            assertFalse(gate.register { called += 100 })
        }
        assertTrue(gate.register(callback))
        assertFalse(gate.register { called += 100 })
        gate.cancel()
        gate.cancel()
        assertEquals(1, called)
        assertFalse(gate.register { called += 100 })
    }

    @Test fun removingFinishedWorkerPreventsItsCancellationCallback() {
        val gate = SandboxCancellationGate()
        var called = false
        val callback: () -> Unit = { called = true }
        assertTrue(gate.register(callback))
        gate.unregister(callback)
        gate.cancel()
        assertFalse(called)
        assertTrue(gate.terminated())
    }

    @Test fun revokedBeforeWorkerStartDeniesRegistration() {
        val gate = SandboxCancellationGate()
        gate.cancel()
        assertFalse(gate.register { throw AssertionError("should not run") })
    }
}
