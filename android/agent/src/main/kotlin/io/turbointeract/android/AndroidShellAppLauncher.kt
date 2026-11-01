/*
 * Adapted from Mika (https://github.com/wangzan101/Mika), Copyright 2026 wangzan101.
 * SPDX-License-Identifier: Apache-2.0
 * Changes: explicit display ownership, fail-closed task conflict, no shell command fallback.
 */
package io.turbointeract.android

import android.app.ActivityOptions
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.os.Process
import android.view.Display

fun interface LaunchApproval {
    /** Live host authorization for this exact display, package, and action attempt. */
    fun approved(display: DisplayRef, packageName: String, actionId: Long): Boolean
}

interface ShellTaskGateway {
    /** Null means unknown/unavailable: do not proceed to launch. */
    fun snapshot(): List<DisplayTaskInfo>?
    /** True only if Android ActivityTaskManager accepted the start request. */
    fun start(intent: Intent, displayId: Int): Boolean
}

enum class LaunchOutcome {
    DENIED, STALE_DISPLAY, TASK_STATUS_UNKNOWN, MAIN_SCREEN_BUSY, OTHER_SCREEN_BUSY,
    NOT_FOUND, SUBMITTED, UNKNOWN
}

/**
 * Only a shell-owned interactive Display is a legal destination. It does not
 * move or force-stop any existing task, and never falls back to main screen.
 * SUBMITTED is not an Activity-started or postcondition verified result.
 */
class AndroidShellAppLauncher(
    private val context: Context,
    private val registry: DisplayRegistry,
    private val owner: AndroidVirtualDisplayOwner,
    private val tasks: ShellTaskGateway,
    private val approval: LaunchApproval,
) {
    private val monitor = Any()
    private val attempts = InputAttemptLedger(2048)

    fun launch(ref: DisplayRef, packageName: String, actionId: Long): LaunchOutcome = synchronized(monitor) {
        if (actionId <= 0 || !owner.current(ref) || registry.current(ref) == null) {
            return@synchronized LaunchOutcome.STALE_DISPLAY
        }
        val permitted = try { approval.approved(ref, packageName, actionId) } catch (_: Exception) { false }
        if (!permitted) return@synchronized LaunchOutcome.DENIED
        val snapshot = try { tasks.snapshot() } catch (_: Exception) { null }
            ?: return@synchronized LaunchOutcome.TASK_STATUS_UNKNOWN
        when (DisplayTaskPolicy.decide(ref, packageName, snapshot)) {
            LaunchVerdict.INVALID_TARGET -> return@synchronized LaunchOutcome.DENIED
            LaunchVerdict.MAIN_SCREEN_BUSY -> return@synchronized LaunchOutcome.MAIN_SCREEN_BUSY
            LaunchVerdict.OTHER_SCREEN_BUSY -> return@synchronized LaunchOutcome.OTHER_SCREEN_BUSY
            LaunchVerdict.ALLOWED -> Unit
        }

        val intent = try { context.packageManager.getLaunchIntentForPackage(packageName) }
            catch (_: Exception) { null } ?: return@synchronized LaunchOutcome.NOT_FOUND
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        // The only external side effect is serialized against release and ID reuse.
        owner.withOwnedDisplay(ref) {
            if (!attempts.claim(InputAttemptKey.from(ref, actionId, actionId))) {
                return@withOwnedDisplay LaunchOutcome.DENIED
            }
            val submitted = try { tasks.start(intent, ref.displayId) } catch (_: Exception) { false }
            if (submitted) LaunchOutcome.SUBMITTED else LaunchOutcome.UNKNOWN
        } ?: LaunchOutcome.STALE_DISPLAY
    }
}

/**
 * Shell-only Android task API adapter. The 3-/4-arg getTasks and 12-arg
 * startActivityAsUser overloads are hidden/OS-dependent, so failure is explicit.
 */
class AndroidShellTaskGateway : ShellTaskGateway {
    private val taskManager: Any by lazy {
        Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null)
            ?: error("ActivityTaskManager unavailable")
    }

    override fun snapshot(): List<DisplayTaskInfo>? {
        if (Process.myUid() != SHELL_UID) return null
        return try {
        val listMethods = taskManager.javaClass.methods.filter { it.name == "getTasks" }
        val withDisplay = listMethods.firstOrNull { it.parameterTypes.size == 4 }
        val raw = (if (withDisplay != null) {
            withDisplay.invoke(taskManager, MAX_TASKS, false, false, Display.INVALID_DISPLAY)
        } else {
            val legacy = listMethods.firstOrNull { it.parameterTypes.size == 3 } ?: return null
            legacy.invoke(taskManager, MAX_TASKS, false, false)
        }) as? List<*> ?: return null
        if (raw.size >= MAX_TASKS) return null // incomplete task enumeration is not proof of isolation
        raw.filterNotNull().map { task ->
            val clazz = task.javaClass
            val displayId = clazz.getField("displayId").getInt(task)
            val visible = runCatching { clazz.getField("isVisible").getBoolean(task) }.getOrDefault(true)
            val top = clazz.getField("topActivity").get(task) as? ComponentName
            val base = clazz.getField("baseActivity").get(task) as? ComponentName
            DisplayTaskInfo(displayId, top?.packageName ?: base?.packageName, visible)
        }
    } catch (_: ReflectiveOperationException) {
        null
    } catch (_: SecurityException) {
        null
    }
    }

    override fun start(intent: Intent, displayId: Int): Boolean {
        if (Process.myUid() != SHELL_UID || displayId <= 0) return false
        return try {
        val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId).toBundle()
        val method = taskManager.javaClass.methods.firstOrNull {
            it.name == "startActivityAsUser" && it.parameterTypes.size == 12
        } ?: return false
        val result = method.invoke(
            taskManager, null, "com.android.shell", null, intent, null, null, null,
            0, 0, null, options, USER_CURRENT,
        ) as? Int ?: return false
        result >= 0
    } catch (_: ReflectiveOperationException) {
        false
    } catch (_: SecurityException) {
        false
    }
    }

    private companion object {
        const val SHELL_UID = 2000
        const val USER_CURRENT = -2
        const val MAX_TASKS = 100
    }
}
