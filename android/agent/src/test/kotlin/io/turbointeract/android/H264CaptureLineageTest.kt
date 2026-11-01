package io.turbointeract.android

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Pure source contract: no fake Android device, Codec, or consumer App. */
class H264CaptureLineageTest {
    private val display = DisplayRef(11L, 2L, 7, 3L, 4L)
    private val stamp = H264CaptureStamp(
        display, 0x1234L, 0x5678L, 1L, 5L, 33_333L, 5_000L)

    @Test fun requiresActualAnnexBPrefixNotAvccOrTruncation() {
        assertTrue(H264CaptureLineageGate.hasAnnexBPrefix(
            byteArrayOf(0, 0, 0, 1, 0x67)))
        assertTrue(H264CaptureLineageGate.hasAnnexBPrefix(
            byteArrayOf(0, 0, 1, 0x65)))
        assertFalse(H264CaptureLineageGate.hasAnnexBPrefix(
            byteArrayOf(0, 0, 0, 5, 0x67)))
        assertFalse(H264CaptureLineageGate.hasAnnexBPrefix(
            byteArrayOf(0, 0, 1)))
        assertFalse(H264CaptureLineageGate.hasAnnexBPrefix(byteArrayOf()))
    }

    @Test fun sourceIdentityCannotRebindToNewDisplayGeometryOrEpoch() {
        fun matches(candidate: H264CaptureStamp): Boolean =
            H264CaptureLineageGate.matches(display, 0x1234L, 0x5678L, 1L, 5L, candidate)
        assertTrue(matches(stamp))
        assertFalse(matches(stamp.copy(
            display=display.copy(deviceSessionId=12L))))
        assertFalse(matches(stamp.copy(
            display=display.copy(sessionGeneration=3L))))
        assertFalse(matches(stamp.copy(
            display=display.copy(displayId=8))))
        assertFalse(matches(stamp.copy(
            display=display.copy(displayGeneration=4L))))
        assertFalse(matches(stamp.copy(
            display=display.copy(geometryGeneration=5L))))
        assertFalse(matches(stamp.copy(captureSessionLo=0x5679L)))
        assertFalse(matches(stamp.copy(streamGeneration=2L)))
        assertFalse(matches(stamp.copy(packetSequence=6L)))
        assertFalse(H264CaptureLineageGate.matches(null,
            0x1234L, 0x5678L, 1L, 5L, stamp))
        assertFalse(H264CaptureLineageGate.matches(display,
            0x1234L, 0x5678L, 1L, 4L, stamp))
    }

    @Test fun packetSequenceAndTimestampsHaveExplicitConstraints() {
        for (bad in listOf(
            { stamp.copy(packetSequence=0L) },
            { stamp.copy(streamGeneration=0L) },
            { stamp.copy(presentationTimeUs=-1L) },
            { stamp.copy(receivedAtNs=0L) },
            { stamp.copy(captureSessionHi=0L,captureSessionLo=0L) }
        )) {
            try {
                bad()
                throw AssertionError("invalid media lineage accepted")
            } catch (_: IllegalArgumentException) { }
        }
    }
}
