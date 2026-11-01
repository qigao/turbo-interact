package io.turbointeract.android

/**
 * A real Android MediaCodec input Surface handed to the SAME authenticated
 * Shell VirtualDisplay API as RGBA. No display 0, permission fallback,
 * second ActionOwner, consumer APK, or screen recorder process is created.
 *
 * The Surface is prepared BEFORE createDisplay, while packet lineage is
 * bound only AFTER the real Shell response. On a failed remote display
 * creation the full Shell session is revoked. On post-create bind failure
 * the exact owned display is released BEFORE stopping MediaCodec.
 */
object AuthorizedH264DisplayCreator {
    fun create(
        connection: AuthorizedShellConnection,
        capture: AndroidH264Capture,
        geometry: DisplayGeometry,
        deviceSessionId: Long,
        sessionGeneration: Long,
    ): DisplaySnapshot {
        require(geometry.width == capture.width && geometry.height == capture.height)
        require(geometry.rotationQuarterTurns == 0)
        require(H264CaptureLineageGate.fitsI420FrameStore(
            capture.width, capture.height)) {
            "selected TurboMedia CPU FrameStore cannot retain decoded I420 frame"
        }
        // Never restart a prior codec instance or reuse old capture UUID/
        // stream epoch after display geometry changes.
        val surface = capture.start()
        try {
            val snapshot = AuthorizedDisplaySurfaceCreator.create(
                connection, surface, geometry, deviceSessionId, sessionGeneration)
            try {
                capture.bindDisplay(snapshot)
                return snapshot
            } catch (failure: Throwable) {
                try {
                    AuthorizedDisplaySurfaceCreator.releaseExactOrRevoke(
                        connection, snapshot.ref)
                } catch (closeFailure: Throwable) {
                    failure.addSuppressed(closeFailure)
                }
                throw failure
            }
        } catch (failure: Throwable) {
            try { check(capture.close()) { "MediaCodec capture still in-flight" } }
            catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
    }
}
