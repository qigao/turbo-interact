/*
 * Adapted from Mika (https://github.com/wangzan101/Mika), Copyright 2026 wangzan101.
 * SPDX-License-Identifier: Apache-2.0
 * Changes: independent DisplaySession registry, explicit IME policy, Android 14+ focus gate.
 */
package io.turbointeract.android

import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.Build
import android.view.Surface

/**
 * The caller must supply a DisplayManager belonging to an authenticated shell
 * process; an ordinary app cannot claim TRUSTED/OWN_FOCUS privileges.
 * This class never installs, launches or authorizes the shell helper.
 */
class AndroidVirtualDisplayOwner(
    private val displayManager: DisplayManager,
    private val registry: DisplayRegistry,
    private val setLocalImePolicy: (Int) -> Boolean,
) {
    private data class Owned(val display: VirtualDisplay, val ref: DisplayRef)
    private val monitor = Any()
    private val displays = mutableMapOf<Int, Owned>()

    /** Fail closed if focus isolation or local IME cannot be established. */
    fun create(surface: Surface, geometry: DisplayGeometry): DisplaySnapshot = synchronized(monitor) {
        check(Build.VERSION.SDK_INT >= 34) { "background-focus isolation requires Android 14+" }
        check(geometry.rotationQuarterTurns == 0) { "rotate/resize requires an explicit geometry transition" }
        val vd = displayManager.createVirtualDisplay(
            "turbo-interact-agent", geometry.width, geometry.height, geometry.densityDpi,
            surface, DISPLAY_FLAGS,
        ) ?: error("VirtualDisplay creation returned null")
        val id = vd.display?.displayId ?: -1
        var registered: DisplaySnapshot? = null
        try {
            require(id > 0) { "refuse to own main/invalid display" }
            val snap = registry.attach(id, geometry)
            registered = snap
            check(setLocalImePolicy(id)) { "virtual display local IME policy unavailable" }
            check(id !in displays) { "duplicate owned display" }
            displays[id] = Owned(vd, snap.ref)
            snap
        } catch (failure: Throwable) {
            registered?.let { registry.detach(it.ref) }
            try { vd.release() } catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
    }

    fun current(ref: DisplayRef): Boolean = synchronized(monitor) {
        registry.current(ref) != null && displays[ref.displayId]?.ref == ref
    }

    /** Destroy only an exactly matching generation; never release by bare displayId. */
    fun release(ref: DisplayRef): Boolean = synchronized(monitor) {
        val owned = displays[ref.displayId] ?: return@synchronized false
        if (owned.ref != ref || registry.current(ref) == null) return@synchronized false
        owned.display.release()
        displays.remove(ref.displayId)
        registry.detach(ref)
    }

    /** Releases all displays; retains unresolved entries if platform release throws. */
    fun releaseAll(): Int = synchronized(monitor) {
        var released = 0
        for ((id, owned) in displays.toMap()) {
            try {
                owned.display.release()
                displays.remove(id)
                registry.detach(owned.ref)
                released++
            } catch (_: RuntimeException) {
                // Keep the entry and report it as unresolved to the owning shell supervisor.
            }
        }
        released
    }

    private companion object {
        // Mika's hidden Android 14+ flags; only usable by privileged/shell processes.
        const val OWN_CONTENT_ONLY = 1 shl 3
        const val SUPPORTS_TOUCH = 1 shl 6
        const val ROTATES_WITH_CONTENT = 1 shl 7
        const val DESTROY_CONTENT_ON_REMOVAL = 1 shl 8
        const val TRUSTED = 1 shl 10
        const val OWN_DISPLAY_GROUP = 1 shl 11
        const val ALWAYS_UNLOCKED = 1 shl 12
        const val TOUCH_FEEDBACK_DISABLED = 1 shl 13
        const val OWN_FOCUS = 1 shl 14
        const val STEAL_TOP_FOCUS_DISABLED = 1 shl 16
        const val DISPLAY_FLAGS = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC or OWN_CONTENT_ONLY or
            SUPPORTS_TOUCH or ROTATES_WITH_CONTENT or DESTROY_CONTENT_ON_REMOVAL or TRUSTED or
            OWN_DISPLAY_GROUP or ALWAYS_UNLOCKED or TOUCH_FEEDBACK_DISABLED or OWN_FOCUS or
            STEAL_TOP_FOCUS_DISABLED
    }
}
