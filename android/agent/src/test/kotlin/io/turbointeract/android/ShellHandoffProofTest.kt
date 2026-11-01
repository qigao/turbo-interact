package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ShellHandoffProofTest {
    @Test fun challengeIsOneShotAndExpires() {
        var time = 1_000L
        var number = 0
        val gate = OneShotHandoff({ time }, { data -> data.fill((++number).toByte()) }, 30_000L)
        val first = gate.issue()
        assertEquals(64, first.length)
        assertTrue(gate.matches(first))
        assertFalse(gate.matches("0".repeat(64)))
        assertTrue(gate.consume(first))
        assertFalse(gate.consume(first))
        val second = gate.issue()
        assertNotEquals(first, second)
        time += 30_001L
        assertFalse(gate.consume(second))
        time -= 31_000L // clock rolled backwards
        assertFalse(gate.matches(second))
        gate.invalidate()
        assertFalse(gate.matches(second))
    }

    @Test fun bindsUidPidApkAndServiceProcess() {
        fun ok(uid: Int, pid: Int, reportedUid: Int, reportedPid: Int, apk: String?, same: Boolean) =
            ShellHandoffProof.valid(uid, pid, reportedUid, reportedPid, apk, "/data/app/me/base.apk", same)
        assertTrue(ok(2000, 900, 2000, 900, "/data/app/me/base.apk", true))
        assertFalse(ok(10000, 900, 10000, 900, "/data/app/me/base.apk", true))
        assertFalse(ok(2000, 900, 2000, 901, "/data/app/me/base.apk", true))
        assertFalse(ok(2000, 900, 2000, 900, "/data/app/old/base.apk", true))
        assertFalse(ok(2000, 900, 2000, 900, "/data/app/me/base.apk", false))
    }

    @Test fun shellCommandNeverEmbedsNonceAndQuotesPaths() {
        val nonce = "a".repeat(64)
        val cmd = ShellLaunchCommand.build(
            "/data/app/a'quoted/base.apk", "io.turbointeract.demo", 10421,
            "b".repeat(64), 101, 7, nonce)
        assertFalse(cmd.command.contains(nonce))
        assertEquals(nonce + "\n", cmd.nonceLine)
        assertTrue(cmd.command.contains("setsid app_process"))
        assertTrue(cmd.command.contains("io.turbointeract.android.TurboDisplayShellMain"))
        assertTrue(cmd.command.contains("'\\''"))
    }

    @Test fun refusesMalformedPrivilegeParameters() {
        try {
            ShellLaunchCommand.build("/data/app/base.apk", "io.demo;reboot", 10001,
                "f".repeat(64), 1, 1, "0".repeat(64))
            throw AssertionError("malformed package accepted")
        } catch (_: IllegalArgumentException) { }
        try {
            ShellLaunchCommand.build("/data/app/base.apk", "io.demo.safe", 2000,
                "f".repeat(64), 1, 1, "0".repeat(64))
            throw AssertionError("shell UID accepted as host")
        } catch (_: IllegalArgumentException) { }
    }
}
