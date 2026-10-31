/*
 * Adapted from Mika (https://github.com/wangzan101/Mika), Copyright 2026 wangzan101.
 * SPDX-License-Identifier: Apache-2.0
 */
package io.turbointeract.android

import android.os.Process

/** Requests that the IME belongs to the virtual display, not the user main display. */
object AndroidShellImePolicy {
    fun requestLocal(displayId: Int): Boolean = try {
        if (Process.myUid() != 2000 || displayId <= 0) return false
        val wm = Class.forName("android.view.WindowManagerGlobal")
            .getMethod("getWindowManagerService").invoke(null) ?: return false
        wm.javaClass.getMethod("setDisplayImePolicy", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
            .invoke(wm, displayId, 0) // DISPLAY_IME_POLICY_LOCAL
        true // Request accepted; real focus/IME isolation still needs a device test.
    } catch (_: ReflectiveOperationException) {
        false
    } catch (_: SecurityException) {
        false
    }
}
