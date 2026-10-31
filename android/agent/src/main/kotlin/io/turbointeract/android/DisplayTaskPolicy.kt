package io.turbointeract.android

/** One conservatively observed Android Task. A task can remain in a stopped App process. */
data class DisplayTaskInfo(val displayId: Int, val packageName: String?, val visible: Boolean)

enum class LaunchVerdict { ALLOWED, INVALID_TARGET, MAIN_SCREEN_BUSY, OTHER_SCREEN_BUSY }

/**
 * Policy is pure/testable. The shell backend must fail closed when task listing
 * is unavailable, and re-check the actual target before submitting an effect.
 */
object DisplayTaskPolicy {
    fun decide(target: DisplayRef, packageName: String, tasks: List<DisplayTaskInfo>): LaunchVerdict {
        if (target.displayId <= 0 || packageName.isBlank() ||
            !packageName.matches(Regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+"))) {
            return LaunchVerdict.INVALID_TARGET
        }
        if (tasks.any { it.displayId == 0 && it.packageName == packageName && it.visible }) {
            return LaunchVerdict.MAIN_SCREEN_BUSY
        }
        if (tasks.any { it.displayId > 0 && it.displayId != target.displayId && it.packageName == packageName }) {
            return LaunchVerdict.OTHER_SCREEN_BUSY
        }
        return LaunchVerdict.ALLOWED
    }
}
