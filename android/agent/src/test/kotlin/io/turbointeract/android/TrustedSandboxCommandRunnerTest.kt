package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** No fake device or consumer process: these are production launch-policy contracts. */
class TrustedSandboxCommandRunnerTest {
    private fun spec(name: String = TrustedSandboxLaunchPolicy.WORKER_LIBRARY_NAME) =
        TrustedSandboxCommand(name,
            arguments = listOf("back", "1", "2", "1", "0", "0", "3", "1", "2000000000"),
            taskId = 10L, sandboxRunId = 3L)

    @Test fun allowsOnlyExplicitPackagedNativeWorkerNames() {
        TrustedSandboxLaunchPolicy.validate(spec())
        for (name in listOf("sh", "/system/bin/sh", "lib../evil.so",
            "libworker.so;id", "libmika.sh", "lib工人.so", "liba/b.so")) {
            try {
                TrustedSandboxLaunchPolicy.validate(spec(name))
                throw AssertionError("accepted executable: $name")
            } catch (_: IllegalArgumentException) { }
        }
    }

    @Test fun refusesShellFramingAndUnboundedWork() {
        val good = spec()
        TrustedSandboxLaunchPolicy.validate(good)
        for (bad in listOf(
            good.copy(arguments = listOf("a\nexec")),
            good.copy(arguments = listOf("a\u0000b")),
            good.copy(maxDurationMs = 1L),
            good.copy(maxRequests = 10_000),
            good.copy(maxRequests = 2),
            good.copy(arguments = listOf("tap", "1", "2", "1", "0", "0", "3", "1",
                "2000000000", "100000", "100000")),
            good.copy(arguments = listOf("back", "1", "2", "1", "0", "0", "3",
                "1", "2000000000", "0")),
            good.copy(arguments = listOf("back", "1", "2", "1", "0", "0", "3",
                "1", "99999999999999999999999999")),
            good.copy(maxOutputBytes = 1_000_000),
            good.copy(taskId = 0L),
            good.copy(sandboxRunId = 0L),
            good.copy(allowedTools = 4)
        )) {
            try {
                TrustedSandboxLaunchPolicy.validate(bad)
                throw AssertionError("accepted unbounded/unsafe launch")
            } catch (_: IllegalArgumentException) { }
        }
    }

    @Test fun auditedOneShotTapSchema() {
        val command = spec().copy(arguments = listOf(
            "tap", "1", "22", "1", "10", "3", "5", "1",
            "2000000000", "100000", "900000"))
        TrustedSandboxLaunchPolicy.validate(command)
    }

    @Test fun tokenGoesOnlyIntoFramedStdinAndIsRedactedInCasualLogs() {
        val token = "abcde123".repeat(8)
        val credentials = NativeSandboxCredentials("/data/user/0/app/files/ti-worker.sock", token)
        val message = String(TrustedSandboxLaunchPolicy.handoff(credentials), Charsets.US_ASCII)
        assertEquals(
            "TURBOINTERACT-SANDBOX/1\n/data/user/0/app/files/ti-worker.sock\n$token\n",
            message
        )
        assertFalse(credentials.toString().contains(token))
        assertTrue(credentials.toString().contains("redacted"))
    }
}
