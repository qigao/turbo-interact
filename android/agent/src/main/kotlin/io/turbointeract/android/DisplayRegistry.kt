package io.turbointeract.android

/** Platform-independent identity contract. An Android display ID alone is never an authority. */
data class DisplayRef(
    val deviceSessionId: Long,
    val sessionGeneration: Long,
    val displayId: Int,
    val displayGeneration: Long,
    val geometryGeneration: Long,
)

/** Display-space geometry, not encoded-video geometry. */
data class DisplayGeometry(val width: Int, val height: Int, val densityDpi: Int, val rotationQuarterTurns: Int = 0) {
    init {
        require(width > 0 && height > 0 && densityDpi > 0) { "invalid display geometry" }
        require(rotationQuarterTurns in 0..3) { "invalid rotation" }
    }
}

data class DisplaySnapshot(val ref: DisplayRef, val geometry: DisplayGeometry)

/**
 * Serializes identity changes. It does not own VirtualDisplay or InputManager objects.
 * This registry is deliberately incapable of registering display 0 (the user's main screen).
 * A different shell/device connection must construct a new registry generation.
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

    fun current(ref: DisplayRef): DisplaySnapshot? = synchronized(monitor) {
        if (ref.deviceSessionId != deviceSessionId || ref.sessionGeneration != sessionGeneration || ref.displayId <= 0) null
        else live[ref.displayId]?.takeIf { it.ref == ref }
    }

    /** Any resize, rotation or density change invalidates previously observed coordinates. */
    fun updateGeometry(ref: DisplayRef, geometry: DisplayGeometry): DisplaySnapshot? = synchronized(monitor) {
        if (current(ref) == null) return@synchronized null
        check(ref.geometryGeneration < Long.MAX_VALUE) { "geometry generation exhausted" }
        DisplaySnapshot(ref.copy(geometryGeneration = ref.geometryGeneration + 1), geometry)
            .also { live[ref.displayId] = it }
    }

    fun detach(ref: DisplayRef): Boolean = synchronized(monitor) {
        if (current(ref) == null) false
        else { live.remove(ref.displayId); true }
    }

    fun drain(): List<DisplaySnapshot> = synchronized(monitor) {
        live.values.toList().also { live.clear() }
    }
}
