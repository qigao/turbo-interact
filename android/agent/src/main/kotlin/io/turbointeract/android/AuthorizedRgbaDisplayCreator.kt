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
/**
 * Only this bridge calls createDisplay and interprets its returned Binder
 * identity. An invalid/partial response revokes the whole Shell: never
 * release a possibly unrelated display based on untrusted numeric IDs.
 */
internal object AuthorizedDisplaySurfaceCreator {
    fun create(
        connection: AuthorizedShellConnection,
        surface: android.view.Surface,
        geometry: DisplayGeometry,
        deviceSessionId: Long,
        sessionGeneration: Long,
    ): DisplaySnapshot {
        require(deviceSessionId > 0 && sessionGeneration > 0)
        require(geometry.rotationQuarterTurns == 0 &&
            geometry.width in 1..4096 && geometry.height in 1..4096 &&
            geometry.width.toLong() * geometry.height <= 8_388_608L)
        val shell = connection.shell
        val reply = try {
            shell.createDisplay(surface, geometry.width, geometry.height, geometry.densityDpi)
        } catch (failure: Throwable) {
            try { connection.close() }
            catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
        val ref = try {
            OwnedDisplayReply.accept(
                deviceSessionId, sessionGeneration,
                reply.getInt("status", -2),
                reply.getLong("deviceSessionId", 0L),
                reply.getLong("sessionGeneration", 0L),
                reply.getInt("displayId", 0),
                reply.getLong("displayGeneration", 0L),
                reply.getLong("geometryGeneration", 0L),
            )
        } catch (_: Exception) { null }
        if (ref == null) {
            val failure = IllegalStateException(
                "authenticated Shell returned an invalid display incarnation")
            try { connection.close() }
            catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
        return DisplaySnapshot(ref, geometry)
    }

    fun releaseExactOrRevoke(
        connection: AuthorizedShellConnection,
        ref: DisplayRef,
    ): Boolean {
        val released = runCatching {
            connection.shell.releaseDisplay(ref.displayId, ref.displayGeneration)
        }.getOrDefault(false)
        if (!released) runCatching { connection.close() }
        return released
    }
}

/** Owns no display itself; the authenticated Shell keeps authoritative IDs. */
object AuthorizedRgbaDisplayCreator {
    fun create(
        connection: AuthorizedShellConnection,
        capture: AndroidRgbaCapture,
        geometry: DisplayGeometry,
        deviceSessionId: Long,
        sessionGeneration: Long,
    ): DisplaySnapshot {
        require(geometry.width == capture.width && geometry.height == capture.height)
        val snapshot = AuthorizedDisplaySurfaceCreator.create(
            connection, capture.surface, geometry, deviceSessionId, sessionGeneration)
        try {
            capture.bindDisplay(snapshot)
            return snapshot
        } catch (failure: Throwable) {
            AuthorizedDisplaySurfaceCreator.releaseExactOrRevoke(
                connection, snapshot.ref)
            throw failure
        }
    }
}
