package io.turbointeract.android

/**
 * Exact source-contract validation of a createDisplay reply from the already
 * authenticated Shell Binder. Unknown/mismatched session and display-0 replies
 * are never allowed to establish a C11 ActionOwner or capture provenance.
 */
internal object OwnedDisplayReply {
    fun accept(
        expectedDeviceSessionId: Long,
        expectedSessionGeneration: Long,
        status: Int,
        deviceSessionId: Long,
        sessionGeneration: Long,
        displayId: Int,
        displayGeneration: Long,
        geometryGeneration: Long,
    ): DisplayRef? {
        if (expectedDeviceSessionId <= 0 || expectedSessionGeneration <= 0 ||
            status != AuthenticatedDisplayShell.STATUS_SUBMITTED ||
            deviceSessionId != expectedDeviceSessionId ||
            sessionGeneration != expectedSessionGeneration ||
            displayId <= 0 || displayGeneration <= 0 || geometryGeneration <= 0
        ) return null
        return DisplayRef(deviceSessionId, sessionGeneration, displayId,
            displayGeneration, geometryGeneration)
    }
}

/**
 * Creates a real RGBA-backed VirtualDisplay on an authenticated Shell session.
 * Calls the actual ITurboDisplayShell.createDisplay Binder method; there is
 * no fake display and no implicit main-screen fallback.
 *
 * The capture is caller-owned. Caller MUST close the native ActionOwner,
 * release the owned display on the Shell, then close this capture on teardown.
 * Display creation is allowed only if the host's separate LIVE session-control
 * grant accepts it. An invalid Binder reply is fatal, not silently remapped.
 */
object AuthorizedRgbaDisplayCreator {
    fun create(
        connection: AuthorizedShellConnection,
        capture: AndroidRgbaCapture,
        geometry: DisplayGeometry,
        deviceSessionId: Long,
        sessionGeneration: Long,
    ): DisplaySnapshot {
        require(deviceSessionId > 0 && sessionGeneration > 0)
        require(geometry.width == capture.width && geometry.height == capture.height)
        require(geometry.rotationQuarterTurns == 0) {
            "rotated display requires a separately created capture incarnation"
        }
        val shell = connection.shell
        val reply = try {
            shell.createDisplay(capture.surface,
                geometry.width, geometry.height, geometry.densityDpi)
        } catch (failure: Throwable) {
            // The remote process might already have created a display before
            // its Binder reply failed. Revoke the WHOLE authenticated session
            // to release its owned resources; never guess a display ID.
            try { connection.close() }
            catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
        val displayId = reply.getInt("displayId", 0)
        val displayGeneration = reply.getLong("displayGeneration", 0L)
        val ref = OwnedDisplayReply.accept(
            deviceSessionId, sessionGeneration,
            reply.getInt("status", -2),
            reply.getLong("deviceSessionId", 0L),
            reply.getLong("sessionGeneration", 0L),
            displayId, displayGeneration, reply.getLong("geometryGeneration", 0L),
        )
        if (ref == null) {
            // Invalid/partial replies cannot establish ownership, including
            // a returned display ID which could refer to some OTHER live
            // display. Never invoke releaseDisplay on that untrusted pair.
            // Close the authenticated Shell session instead; it owns the
            // actual display registry and releases ALL of its resources.
            val failure = IllegalStateException(
                "authenticated Shell returned an invalid display incarnation"
            )
            try { connection.close() }
            catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
        val snapshot = DisplaySnapshot(ref, geometry)
        try {
            capture.bindDisplay(snapshot)
            return snapshot
        } catch (failure: Throwable) {
            // Avoid publishing a display whose capture source was not bound.
            try { shell.releaseDisplay(ref.displayId, ref.displayGeneration) }
            catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
    }
}
