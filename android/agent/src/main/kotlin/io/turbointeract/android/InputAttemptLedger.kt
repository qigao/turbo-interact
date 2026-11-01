package io.turbointeract.android

/**
 * A bounded at-most-once *dispatch admission* ledger. A failed or unknown external
 * effect is deliberately NOT removed; a retry requires a new, explicitly authorized
 * attempt ID. No unbounded history or silent eviction of accepted attempts.
 */
data class InputAttemptKey(
    val deviceSessionId: Long,
    val sessionGeneration: Long,
    val displayId: Int,
    val displayGeneration: Long,
    val actionId: Long,
    val attemptId: Long,
) {
    companion object {
        fun from(ref: DisplayRef, actionId: Long, attemptId: Long): InputAttemptKey =
            InputAttemptKey(ref.deviceSessionId, ref.sessionGeneration,
                ref.displayId, ref.displayGeneration, actionId, attemptId)
    }

    fun valid() = deviceSessionId > 0 && sessionGeneration > 0 && displayId > 0 &&
        displayGeneration > 0 && actionId > 0 && attemptId > 0
}

class InputAttemptLedger(private val capacity: Int = 4096) {
    init { require(capacity > 0) }
    private val monitor = Any()
    private val seen = HashSet<InputAttemptKey>()

    /** True means admitted once, not that the input event was accepted or applied. */
    fun claim(key: InputAttemptKey): Boolean = synchronized(monitor) {
        key.valid() && seen.size < capacity && seen.add(key)
    }

    fun count(): Int = synchronized(monitor) { seen.size }
}
