package io.turbointeract.android

/** A numeric Android display ID is scoped to one connection, never an authorization token. */
data class DisplayRef(
    val deviceSessionId: Long,
    val sessionGeneration: Long,
    val displayId: Int,
    val displayGeneration: Long,
    val geometryGeneration: Long,
)

data class DisplayGeometry(val width: Int, val height: Int, val densityDpi: Int, val rotationQuarterTurns: Int = 0) {
    init {
        require(width > 0 && height > 0 && densityDpi > 0) { "invalid display geometry" }
        require(rotationQuarterTurns in 0..3) { "invalid rotation" }
    }
}

data class DisplaySnapshot(val ref: DisplayRef, val geometry: DisplayGeometry)

/**
 * One registry per authenticated device session. The owner, not an untrusted
 * request, is responsible for attach/detach. Display 0 is never registered.
 * All operations are synchronized, including generation checks.
 */
class DisplayRegistry(val deviceSessionId: Long, val sessionGeneration: Long) {
    init {
        require(deviceSessionId > 0 && sessionGeneration > 0)
    }

    private val monitor = Any()
    private val live = mutableMapOf<Int, DisplaySnapshot>()
    private var nextDisplayGeneration = 1L

    fun attach(displayId: Int, geometry: DisplayGeometry): DisplaySnapshot = synchronized(monitor) {
        require(displayId > 0) { "main/invalid display is forbidden" }
        check(displayId !in live) { "display already owned" }
        check(nextDisplayGeneration < Long.MAX_VALUE) { "display generation exhausted" }
        val ref = DisplayRef(deviceSessionId, sessionGeneration, displayId, nextDisplayGeneration++, 1L)
        DisplaySnapshot(ref, geometry).also { live[displayId] = it }
    }

    /** Strict validity for actions using coordinates from a particular screenshot. */
    fun current(ref: DisplayRef): DisplaySnapshot? = synchronized(monitor) {
        live[ref.displayId]?.takeIf { it.ref == ref && inSession(ref) }
    }

    /** Resource ownership survives geometry changes but never display-ID ABA/reconnection. */
    fun ownsInstance(ref: DisplayRef): Boolean = synchronized(monitor) {
        inSession(ref) && live[ref.displayId]?.ref?.displayGeneration == ref.displayGeneration
    }

    fun updateGeometry(ref: DisplayRef, geometry: DisplayGeometry): DisplaySnapshot? = synchronized(monitor) {
        if (current(ref) == null) return@synchronized null
        check(ref.geometryGeneration < Long.MAX_VALUE) { "geometry generation exhausted" }
        DisplaySnapshot(ref.copy(geometryGeneration = ref.geometryGeneration + 1), geometry)
            .also { live[ref.displayId] = it }
    }

    /** Strict release when the caller is also using the current geometry ref. */
    fun detach(ref: DisplayRef): Boolean = synchronized(monitor) {
        if (current(ref) == null) false else { live.remove(ref.displayId); true }
    }

    /**
     * Display owner's teardown: retain lifetime identity and ignore stale geometry.
     * A reused display ID with a different display generation can never be detached.
     */
    fun detachInstance(ref: DisplayRef): Boolean = synchronized(monitor) {
        if (!ownsInstance(ref)) false else { live.remove(ref.displayId); true }
    }

    fun drain(): List<DisplaySnapshot> = synchronized(monitor) {
        live.values.toList().also { live.clear() }
    }

    private fun inSession(ref: DisplayRef): Boolean =
        ref.displayId > 0 && ref.deviceSessionId == deviceSessionId &&
            ref.sessionGeneration == sessionGeneration && ref.displayGeneration > 0
}
