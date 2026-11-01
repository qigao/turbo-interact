package io.turbointeract.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Pure metadata contract. No device, emulator, consumer or installer App. */
class OwnedDisplayReplyTest {
    private fun accept(
        status: Int = 0,
        device: Long = 41L,
        session: Long = 2L,
        display: Int = 7,
        displayGeneration: Long = 3L,
        geometryGeneration: Long = 1L
    ) = OwnedDisplayReply.accept(
        41L, 2L, status, device, session, display, displayGeneration,
        geometryGeneration)

    @Test fun acceptsOnlyTheExactAuthorizedReturnedIncarnation() {
        assertEquals(DisplayRef(41, 2, 7, 3, 1), accept())
        assertNull(accept(status = -1))
        assertNull(accept(status = -2))
        assertNull(accept(device = 42L))
        assertNull(accept(session = 3L))
        assertNull(accept(display = 0))
        assertNull(accept(display = -1))
        assertNull(accept(displayGeneration = 0L))
        assertNull(accept(geometryGeneration = 0L))
    }

    @Test fun missingSessionCannotEstablishCaptureOwnership() {
        assertNull(OwnedDisplayReply.accept(0, 2, 0, 0, 2, 4, 1, 1))
        assertNull(OwnedDisplayReply.accept(41, 0, 0, 41, 0, 4, 1, 1))
    }
}
