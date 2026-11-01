package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Pure JVM fixtures. Native ActionOwner is independently tested by real CTest. */
class NativeActionContractTest {
    private val ref = DisplayRef(9, 7, 13, 5, 2)

    @Test fun exactPrivateJniFieldsAndTypedReconstruction() {
        val action = NativeInputAction(
            ref, NativeInputAction.SANDBOX, NativeInputAction.TAP,
            100, 1, 500, 8, 3, 10_000L, 300_000, 700_000
        )
        val cells = action.values(100L)
        assertEquals(17, cells.size)
        assertEquals(1L, cells[0])
        assertEquals(9L, cells[7])
        assertEquals(7L, cells[8])
        assertEquals(13L, cells[9])
        assertEquals(5L, cells[10])
        assertEquals(2L, cells[11])
        assertEquals(3L, cells[12])
        assertEquals(10_000L, cells[13])
        assertEquals(100L, cells[16])
        assertEquals(action, NativeInputAction.fromNative(cells))
    }

    @Test fun cannotAdmitMainScreenOrMissingEvidence() {
        assertNull(NativeInputAction.fromNative(longArrayOf()))
        val sample = NativeInputAction(ref, 1, 4, 50, 1, 4, 2, 3, 10_000L).values(50)
        sample[9] = 0
        assertNull(NativeInputAction.fromNative(sample))
        sample[9] = 13
        sample[14] = 1000001
        assertNull(NativeInputAction.fromNative(sample))
        sample[14] = 0
        sample[5] = 0
        assertNull(NativeInputAction.fromNative(sample))
        try {
            NativeInputAction(ref.copy(displayId = 0), 1, 4, 10, 1, 2, 1, 3, 500)
            throw AssertionError("main display accepted")
        } catch (_: IllegalArgumentException) { }
    }

    @Test fun backDoesNotRequireScreenshotEvidence() {
        val back = NativeInputAction(ref, NativeInputAction.MODEL, NativeInputAction.BACK,
            81, 1, 0, 0, 3, 500)
        assertNotNull(NativeInputAction.fromNative(back.values(99)))
    }

    @Test fun hostInputDeniesUntilCanonicalNativeTicketIsBound() {
        var controls = 0
        val grants = CanonicalHostGrants(9, 7, object : HostSessionControlPolicy {
            override fun allowCreateDisplay(deviceSessionId: Long, sessionGeneration: Long): Boolean {
                controls++
                return true
            }
            override fun allowLaunch(ref: DisplayRef, packageName: String, actionId: Long): Boolean {
                controls++
                return true
            }
        })
        val tap = InputIntent(ref, 90, 1, InputIntent.Kind.TAP)
        assertFalse(grants.allowInput(tap))
        assertFalse(grants.allowCreateDisplay(9, 999))
        assertFalse(grants.allowLaunch(ref.copy(sessionGeneration = 8), "com.android.settings", 4))
        assertTrue(grants.allowCreateDisplay(9, 7))
        assertTrue(grants.allowLaunch(ref, "com.android.settings", 4))
        assertEquals(2, controls)
        grants.onShellTerminated()
        assertFalse(grants.allowInput(tap))
        assertFalse(grants.allowCreateDisplay(9, 7))
        assertFalse(grants.allowLaunch(ref, "com.android.settings", 4))
    }

    @Test fun sandboxCredentialsAreStrongAndNeverContainAnExternalUid() {
        val good = NativeSandboxCredentials("/data/user/0/app/files/ti-a.sock", "a".repeat(64))
        assertTrue(good.socketPath.startsWith("/"))
        assertEquals(64, good.tokenHex.length)
        for ((path, token) in listOf(
            Pair("relative.sock", "a".repeat(64)),
            Pair("/data/local/tmp/ti.sock", "A".repeat(64)),
            Pair("/data/user/0/app/files/sock", "x".repeat(63))
        )) {
            try {
                NativeSandboxCredentials(path, token)
                throw AssertionError("accepted malformed sandbox credentials")
            } catch (_: IllegalArgumentException) { }
        }
    }

    @Test fun sandboxMethodCapabilitiesStayNarrow() {
        assertEquals(1, NativeAndroidActionOwner.SANDBOX_TAP)
        assertEquals(2, NativeAndroidActionOwner.SANDBOX_BACK)
        assertEquals(3, NativeAndroidActionOwner.SANDBOX_TAP or NativeAndroidActionOwner.SANDBOX_BACK)
    }

    @Test fun receiptsNeverEquateDispatchWithTaskSuccess() {
        assertTrue(NativeInputReceipt(0, 4, 2).unknown)
        assertFalse(NativeInputReceipt(0, 1, 3).unknown)
        assertTrue(NativeInputReceipt(0, 1, 3).admitted)
        assertFalse(NativeInputReceipt(-5, 0, 0).admitted)
    }
}
