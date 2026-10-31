package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class InputAttemptLedgerTest {
    private fun key(n: Long) = InputAttemptKey(9, 2, 7, 1, 50, n)

    @Test fun neverReplaysSameAttempt() {
        val ledger = InputAttemptLedger(2)
        assertTrue(ledger.claim(key(1)))
        assertFalse(ledger.claim(key(1)))
        assertTrue(ledger.claim(key(2)))
        assertFalse(ledger.claim(key(3)))
        assertEquals(2, ledger.count())
    }

    @Test fun rejectsMainDisplayAndMissingIds() {
        val ledger = InputAttemptLedger()
        assertFalse(ledger.claim(key(1).copy(displayId = 0)))
        assertFalse(ledger.claim(key(1).copy(actionId = 0)))
        assertEquals(0, ledger.count())
    }

    @Test fun aNewSessionCannotReuseOldAttemptIdentity() {
        val ledger = InputAttemptLedger()
        val previous = key(1)
        assertTrue(ledger.claim(previous))
        assertFalse(ledger.claim(previous))
        assertTrue(ledger.claim(previous.copy(sessionGeneration = 3)))
    }
}
