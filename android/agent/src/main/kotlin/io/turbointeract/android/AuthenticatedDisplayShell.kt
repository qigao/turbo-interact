/*
 * Inspired by Mika's UID-protected Binder and shell-only VirtualDisplay owner.
 * Mika Copyright 2026 wangzan101, Apache-2.0.
 * This replacement interface is intentionally much narrower than IMikaShell.
 */
package io.turbointeract.android

import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Binder
import android.os.Bundle
import android.os.IBinder
import android.os.Parcel
import android.os.Process
import android.view.Surface

/**
 * An immutable shell authorization boundary, supplied by a trusted bootstrap.
 * It must query live host grants; a cached "true" does not revoke in-flight work.
 */
interface ShellAdmission {
    fun allowCreateDisplay(): Boolean
    fun allowLaunch(ref: DisplayRef, packageName: String, actionId: Long): Boolean
    fun allowInput(intent: InputIntent): Boolean
}

/**
 * Construct ONLY after verified app_process bootstrap. That bootstrap must
 * verify package certificate, caller UID, and a one-time challenge before
 * handing this Binder to the authorized companion app. Not yet integrated.
 *
 * No default allow-all admission, no main-screen fallback, no arbitrary command.
 */
class AuthenticatedDisplayShell(
    private val allowedUid: Int,
    context: Context,
    displayManager: DisplayManager,
    deviceSessionId: Long,
    sessionGeneration: Long,
    private val admission: ShellAdmission,
    taskGateway: ShellTaskGateway = AndroidShellTaskGateway(),
) : ITurboDisplayShell.Stub() {
    init {
        check(Process.myUid() == 2000) { "must run under Android shell uid" }
        require(allowedUid > 0 && allowedUid != 2000) { "companion UID required" }
    }

    @Volatile private var closed = false

    private val registry = DisplayRegistry(deviceSessionId, sessionGeneration)
    private val displays = AndroidVirtualDisplayOwner(displayManager, registry, AndroidShellImePolicy::requestLocal)
    private val input = AndroidSdkInput(registry, displays, InputApproval { request ->
        safeAdmission { admission.allowInput(request) }
    })
    private val launcher = AndroidShellAppLauncher(context, registry, displays, taskGateway,
        LaunchApproval { ref, pkg, action -> safeAdmission { admission.allowLaunch(ref, pkg, action) } })

    override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
        if (code in IBinder.FIRST_CALL_TRANSACTION..IBinder.LAST_CALL_TRANSACTION &&
            Binder.getCallingUid() != allowedUid) {
            throw SecurityException("unauthorized Binder caller")
        }
        return super.onTransact(code, data, reply, flags)
    }

    override fun identity(): IntArray = intArrayOf(Process.myUid(), Process.myPid())

    /** App-controlled teardown, or local cleanup on host Binder death. */
    override fun closeSession() {
        terminateFromHostDeath()
    }

    fun terminateFromHostDeath(): Int {
        closed = true
        return displays.releaseAll()
    }

    override fun createDisplay(surface: Surface, width: Int, height: Int, densityDpi: Int): Bundle {
        val result = Bundle()
        if (closed || !safeAdmission { admission.allowCreateDisplay() }) {
            result.putInt("status", STATUS_DENIED)
            return result
        }
        val snapshot = try {
            displays.create(surface, DisplayGeometry(width, height, densityDpi))
        } catch (_: Exception) {
            result.putInt("status", STATUS_UNKNOWN)
            return result
        }
        result.putInt("status", STATUS_SUBMITTED)
        result.putLong("deviceSessionId", snapshot.ref.deviceSessionId)
        result.putLong("sessionGeneration", snapshot.ref.sessionGeneration)
        result.putInt("displayId", snapshot.ref.displayId)
        result.putLong("displayGeneration", snapshot.ref.displayGeneration)
        result.putLong("geometryGeneration", snapshot.ref.geometryGeneration)
        return result
    }

    override fun launchApp(
        displayId: Int, displayGeneration: Long, geometryGeneration: Long,
        packageName: String, actionId: Long
    ): Int {
        if (closed) return STATUS_DENIED
        val ref = ref(displayId, displayGeneration, geometryGeneration)
        return when (launcher.launch(ref, packageName, actionId)) {
            LaunchOutcome.SUBMITTED -> STATUS_SUBMITTED
            LaunchOutcome.UNKNOWN -> STATUS_UNKNOWN
            else -> STATUS_DENIED
        }
    }

    override fun tap(
        displayId: Int, displayGeneration: Long, geometryGeneration: Long,
        actionId: Long, attemptId: Long, x: Int, y: Int
    ): Int = if (closed) STATUS_DENIED else toStatus(input.tap(
        InputIntent(ref(displayId, displayGeneration, geometryGeneration), actionId, attemptId, InputIntent.Kind.TAP), x, y
    ))

    override fun key(
        displayId: Int, displayGeneration: Long, geometryGeneration: Long,
        actionId: Long, attemptId: Long, keyKind: Int
    ): Int {
        if (closed) return STATUS_DENIED
        val kind = when (keyKind) {
            1 -> InputIntent.Kind.BACK
            2 -> InputIntent.Kind.ENTER
            else -> return STATUS_DENIED
        }
        return toStatus(input.key(InputIntent(
            ref(displayId, displayGeneration, geometryGeneration), actionId, attemptId, kind
        )))
    }

    override fun releaseDisplay(displayId: Int, displayGeneration: Long): Boolean =
        displays.release(ref(displayId, displayGeneration, 0L))

    /** Call from the trusted bootstrap on Binder death/revocation, never via untrusted IPC. */
    fun releaseOwnedResources(): Int = terminateFromHostDeath()

    private fun ref(displayId: Int, displayGeneration: Long, geometryGeneration: Long) =
        DisplayRef(registry.deviceSessionId, registry.sessionGeneration,
            displayId, displayGeneration, geometryGeneration)

    private fun toStatus(dispatch: InputDispatch) = when (dispatch) {
        InputDispatch.REJECTED -> STATUS_DENIED
        InputDispatch.SENT -> STATUS_SENT
        InputDispatch.UNKNOWN -> STATUS_UNKNOWN
    }

    private inline fun safeAdmission(predicate: () -> Boolean): Boolean =
        try { predicate() } catch (_: Exception) { false }

    companion object {
        const val STATUS_SUBMITTED = 0
        const val STATUS_SENT = 1
        const val STATUS_DENIED = -1
        const val STATUS_UNKNOWN = -2
    }
}
