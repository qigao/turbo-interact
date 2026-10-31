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
 * Must be instantiated with a DisplayManager from an authenticated shell
 * process. Constructing this object does not grant shell privileges.
 * The supplied IME callback must use the same authenticated shell identity.
 */
class AndroidVirtualDisplayOwner(
    private val displayManager: DisplayManager,
    private val registry: DisplayRegistry,
    private val setLocalImePolicy: (Int) -> Boolean,
) {
    private data class Owned(val display: VirtualDisplay, val origin: DisplayRef)
    private val monitor = Any()
    private val displays = mutableMapOf<Int, Owned>()

    /** On Android 14+ only, require own focus and a successful local IME request. */
    fun create(surface: Surface, geometry: DisplayGeometry): DisplaySnapshot = synchronized(monitor) {
        check(Build.VERSION.SDK_INT >= 34) { "background-focus isolation requires Android 14+" }
        check(geometry.rotationQuarterTurns == 0) { "rotation requires a separate geometry update" }
        val vd = displayManager.createVirtualDisplay(
            "turbo-interact-agent", geometry.width, geometry.height, geometry.densityDpi, surface, DISPLAY_FLAGS
        ) ?: error("VirtualDisplay creation returned null")
        val id = vd.display.displayId
        var registered: DisplaySnapshot? = null
        try {
            require(id > 0) { "refuse to own main/invalid display" }
            registered = registry.attach(id, geometry)
            check(setLocalImePolicy(id)) { "virtual display local IME policy unavailable" }
            check(id !in displays) { "duplicate owned display" }
            displays[id] = Owned(vd, registered.ref)
            registered
        } catch (failure: Throwable) {
            registered?.let { registry.detachInstance(it.ref) }
            try { vd.release() } catch (closeFailure: Throwable) { failure.addSuppressed(closeFailure) }
            throw failure
        }
    }

    /** Only current geometry and the original display incarnation can receive input. */
    fun current(ref: DisplayRef): Boolean = synchronized(monitor) {
        registry.current(ref) != null && displays[ref.displayId]?.origin?.displayGeneration == ref.displayGeneration
    }

    /** Encoder/ImageReader swap preserves the Android display and App task. */
    fun replaceSurface(ref: DisplayRef, surface: Surface): Boolean = synchronized(monitor) {
        if (!current(ref)) return@synchronized false
        val owned = displays[ref.displayId] ?: return@synchronized false
        owned.display.setSurface(surface)
        true
    }

    /**
     * Only the owner may dispose a matching display incarnation. Geometry version
     * does not decide resource lifetime: resizing must never make release impossible.
     */
    fun release(ref: DisplayRef): Boolean = synchronized(monitor) {
        val owned = displays[ref.displayId] ?: return@synchronized false
        if (owned.origin.displayGeneration != ref.displayGeneration || !registry.ownsInstance(ref)) return@synchronized false
        owned.display.release()
        displays.remove(ref.displayId)
        registry.detachInstance(ref)
    }

    /** Keep failed releases recorded rather than declaring a false clean shutdown. */
    fun releaseAll(): Int = synchronized(monitor) {
        var released = 0
        for ((id, owned) in displays.toMap()) {
            try {
                owned.display.release()
                displays.remove(id)
                registry.detachInstance(owned.origin)
                released++
            } catch (_: RuntimeException) {
                // Owning supervisor must see unresolved resources; no implicit retry.
            }
        }
        released
    }

    private companion object {
        // Hidden flags from Mika's Android 14+ shell-only implementation.
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
