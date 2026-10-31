/*
 * Adapted from Mika (https://github.com/wangzan101/Mika), Copyright 2026 wangzan101.
 * SPDX-License-Identifier: Apache-2.0
 * Changes: deny-by-default display/attempt guard; no shell command fallback.
 */
package io.turbointeract.android

import android.os.SystemClock
import android.view.InputDevice
import android.view.InputEvent
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent

/** This shell-side approval is not a replacement for the C11 Action Owner. */
fun interface InputApproval {
    /** Called per attempt and must reject missing, revoked, or expired host authority. */
    fun approved(intent: InputIntent): Boolean
}

data class InputIntent(
    val display: DisplayRef,
    val actionId: Long,
    val attemptId: Long,
    val kind: Kind,
) {
    enum class Kind { TAP, BACK, ENTER }
}

enum class InputDispatch { REJECTED, SENT, UNKNOWN }

/**
 * Shell-only input path. No ADB command fallback; never inject on display 0.
 * SENT means Android accepted and processed the events at injection level,
 * not that the target app actually completed its intended action.
 * The host Action Owner must serialize conflicting pointer ownership.
 */
class AndroidSdkInput(private val registry: DisplayRegistry, private val approval: InputApproval) {
    private val inputLock = Any()

    fun tap(intent: InputIntent, x: Int, y: Int): InputDispatch = synchronized(inputLock) {
        if (intent.kind != InputIntent.Kind.TAP || !admit(intent)) return@synchronized InputDispatch.REJECTED
        val geometry = registry.current(intent.display)?.geometry ?: return@synchronized InputDispatch.REJECTED
        if (x !in 0 until geometry.width || y !in 0 until geometry.height) return@synchronized InputDispatch.REJECTED
        val now = SystemClock.uptimeMillis()
        val down = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, x.toFloat(), y.toFloat(), 0)
        val up = MotionEvent.obtain(now, now + 50L, MotionEvent.ACTION_UP, x.toFloat(), y.toFloat(), 0)
        try {
            down.source = InputDevice.SOURCE_TOUCHSCREEN
            up.source = InputDevice.SOURCE_TOUCHSCREEN
            if (!inject(down, intent.display.displayId)) return@synchronized InputDispatch.UNKNOWN
            if (inject(up, intent.display.displayId)) InputDispatch.SENT else InputDispatch.UNKNOWN
        } finally {
            down.recycle()
            up.recycle()
        }
    }

    fun key(intent: InputIntent): InputDispatch = synchronized(inputLock) {
        val keyCode = when (intent.kind) {
            InputIntent.Kind.BACK -> KeyEvent.KEYCODE_BACK
            InputIntent.Kind.ENTER -> KeyEvent.KEYCODE_ENTER
            else -> return@synchronized InputDispatch.REJECTED
        }
        if (!admit(intent)) return@synchronized InputDispatch.REJECTED
        val now = SystemClock.uptimeMillis()
        val down = KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD)
        val up = KeyEvent.changeAction(down, KeyEvent.ACTION_UP)
        if (!inject(down, intent.display.displayId)) InputDispatch.UNKNOWN
        else if (inject(up, intent.display.displayId)) InputDispatch.SENT
        else InputDispatch.UNKNOWN
    }

    private fun admit(intent: InputIntent): Boolean =
        intent.actionId > 0 && intent.attemptId > 0 && intent.display.displayId > 0 &&
            registry.current(intent.display) != null && approval.approved(intent) &&
            registry.current(intent.display) != null

    private fun inject(event: InputEvent, displayId: Int): Boolean = try {
        InputEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType)
            .invoke(event, displayId)
        val manager = runCatching {
            Class.forName("android.hardware.input.InputManagerGlobal")
                .getMethod("getInstance").invoke(null)
        }.getOrNull() ?: Class.forName("android.hardware.input.InputManager")
            .getMethod("getInstance").invoke(null)
        manager.javaClass.getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
            .invoke(manager, event, 2) as Boolean // WAIT_FOR_FINISH, not business-success proof.
    } catch (_: ReflectiveOperationException) {
        false
    } catch (_: SecurityException) {
        false
    }
}
