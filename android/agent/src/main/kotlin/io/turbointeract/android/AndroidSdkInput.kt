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

/** The authoritative, live host Action Owner must approve every independent attempt. */
fun interface InputApproval {
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
 * Shell-only targeted input. Real ownership and host authority are mandatory.
 * UNKNOWN is never automatically retried, even when the first event returned false.
 * SENT is only a backend-input result, never a business postcondition.
 */
class AndroidSdkInput(
    private val registry: DisplayRegistry,
    private val owner: AndroidVirtualDisplayOwner,
    private val approval: InputApproval,
    private val ledger: InputAttemptLedger = InputAttemptLedger(),
) {
    private val inputLock = Any()

    fun tap(intent: InputIntent, x: Int, y: Int): InputDispatch = synchronized(inputLock) {
        // Do the remote host admission outside the DisplayOwner monitor.
        if (intent.kind != InputIntent.Kind.TAP || !admit(intent)) return@synchronized InputDispatch.REJECTED
        owner.withOwnedDisplay(intent.display) {
            val geometry = registry.current(intent.display)?.geometry
                ?: return@withOwnedDisplay InputDispatch.REJECTED
            if (x !in 0 until geometry.width || y !in 0 until geometry.height) {
                return@withOwnedDisplay InputDispatch.REJECTED
            }
            if (!claim(intent)) return@withOwnedDisplay InputDispatch.REJECTED

            val now = SystemClock.uptimeMillis()
            val down = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, x.toFloat(), y.toFloat(), 0)
            val up = MotionEvent.obtain(now, now + 50L, MotionEvent.ACTION_UP, x.toFloat(), y.toFloat(), 0)
            try {
                down.source = InputDevice.SOURCE_TOUCHSCREEN
                up.source = InputDevice.SOURCE_TOUCHSCREEN
                if (!inject(down, intent.display.displayId)) return@withOwnedDisplay InputDispatch.UNKNOWN
                SystemClock.sleep(50L)
                if (inject(up, intent.display.displayId)) InputDispatch.SENT else {
                    // One best-effort cancellation. This can NEVER make UNKNOWN a success.
                    val cancel = MotionEvent.obtain(now, SystemClock.uptimeMillis(),
                        MotionEvent.ACTION_CANCEL, x.toFloat(), y.toFloat(), 0)
                    try {
                        cancel.source = InputDevice.SOURCE_TOUCHSCREEN
                        inject(cancel, intent.display.displayId)
                    } finally {
                        cancel.recycle()
                    }
                    InputDispatch.UNKNOWN
                }
            } finally {
                down.recycle()
                up.recycle()
            }
        } ?: InputDispatch.REJECTED
    }

    fun key(intent: InputIntent): InputDispatch = synchronized(inputLock) {
        val keyCode = when (intent.kind) {
            InputIntent.Kind.BACK -> KeyEvent.KEYCODE_BACK
            InputIntent.Kind.ENTER -> KeyEvent.KEYCODE_ENTER
            else -> return@synchronized InputDispatch.REJECTED
        }
        if (!admit(intent)) return@synchronized InputDispatch.REJECTED
        owner.withOwnedDisplay(intent.display) {
            if (!claim(intent)) return@withOwnedDisplay InputDispatch.REJECTED
            val now = SystemClock.uptimeMillis()
            val down = KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD)
            val up = KeyEvent.changeAction(down, KeyEvent.ACTION_UP)
            if (!inject(down, intent.display.displayId)) InputDispatch.UNKNOWN
            else if (inject(up, intent.display.displayId)) InputDispatch.SENT
            else InputDispatch.UNKNOWN
        } ?: InputDispatch.REJECTED
    }

    private fun admit(intent: InputIntent): Boolean {
        if (intent.display.displayId <= 0 || registry.current(intent.display) == null || !owner.current(intent.display)) {
            return false
        }
        if (intent.actionId <= 0 || intent.attemptId <= 0) return false
        val authorized = try { approval.approved(intent) } catch (_: Exception) { false }
        return authorized && owner.current(intent.display)
    }

    private fun claim(intent: InputIntent): Boolean =
        ledger.claim(InputAttemptKey.from(intent.display, intent.actionId, intent.attemptId))

    private fun inject(event: InputEvent, displayId: Int): Boolean = try {
        // Never omit the display ID or fall back to ADB 'input' on the main screen.
        InputEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType).invoke(event, displayId)
        val manager = runCatching {
            Class.forName("android.hardware.input.InputManagerGlobal")
                .getMethod("getInstance").invoke(null)
        }.getOrNull() ?: Class.forName("android.hardware.input.InputManager")
            .getMethod("getInstance").invoke(null)
        manager.javaClass.getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
            .invoke(manager, event, 2) as Boolean // WAIT_FOR_FINISH, not App success.
    } catch (_: ReflectiveOperationException) {
        false
    } catch (_: SecurityException) {
        false
    }
}
