package io.turbointeract.android

/**
 * A source identity for an owned Android MediaCodec H264 access unit.
 * Raw UUID bits may be signed in Kotlin; they are transferred unchanged
 * to an unsigned native sidecar. This grants no frame lease or authority.
 *
 * Android exposes presentationTimeUs but not an authoritative DTS. A remote
 * no-B-frame Codec may set DTS=PTS ONLY after verifying the actual H264 SPS
 * and decoder profile. This stamp is not compositor/input causal evidence.
 */
data class H264CaptureStamp(
    val display: DisplayRef,
    val captureSessionHi: Long,
    val captureSessionLo: Long,
    val streamGeneration: Long,
    val packetSequence: Long,
    val presentationTimeUs: Long,
    val receivedAtNs: Long,
) {
    init {
        require(display.deviceSessionId > 0 && display.sessionGeneration > 0 &&
            display.displayId > 0 && display.displayGeneration > 0 &&
            display.geometryGeneration > 0)
        require(captureSessionHi != 0L || captureSessionLo != 0L)
        require(streamGeneration > 0 && packetSequence > 0)
        require(presentationTimeUs >= 0 && receivedAtNs > 0)
    }
}

/** The full H264 NAL grammar is validated by TurboMedia, not this AAR. */
internal object H264CaptureLineageGate {
    fun hasAnnexBPrefix(bytes: ByteArray): Boolean =
        bytes.size >= 4 && bytes[0] == 0.toByte() && bytes[1] == 0.toByte() &&
            (bytes[2] == 1.toByte() ||
                (bytes[2] == 0.toByte() && bytes[3] == 1.toByte()))

    /**
     * One terminal EOS marker is required even for a zero-byte Codec buffer.
     * A nonempty final access unit needs TWO queue credits: picture then EOS.
     */
    fun outputRows(bytes: Int, config: Boolean, key: Boolean, eos: Boolean): Int =
        when {
            bytes < 0 || (config && (key || eos)) -> 0
            bytes == 0 && !eos -> 0
            eos && bytes > 0 -> 2
            else -> 1
        }

    /** Preflight against the existing TurboMedia CPU FrameStore 8 MiB cap. */
    fun fitsI420FrameStore(width: Int, height: Int): Boolean =
        width in 16..4096 && height in 16..4096 &&
            width % 2 == 0 && height % 2 == 0 &&
            width.toLong() * height * 3L / 2L <= 8_388_608L

    fun matches(
        display: DisplayRef?,
        captureSessionHi: Long,
        captureSessionLo: Long,
        streamGeneration: Long,
        lastIssuedSequence: Long,
        stamp: H264CaptureStamp,
    ): Boolean =
        display == stamp.display &&
        captureSessionHi == stamp.captureSessionHi &&
        captureSessionLo == stamp.captureSessionLo &&
        streamGeneration == stamp.streamGeneration &&
        stamp.packetSequence > 0 &&
        stamp.packetSequence <= lastIssuedSequence
}
