/*
 * Inspired by Mika HelperHub / HelperHandoffProvider and HandoffNonces.
 * Copyright 2026 wangzan101. SPDX-License-Identifier: Apache-2.0
 *
 * This host-side gate never opens ADB, installs an App or auto-restores helpers.
 * The embedding Android App must explicitly authorize and launch the shell
 * command over its own authenticated adb transport.
 */
package io.turbointeract.android

import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Binder
import android.os.Bundle
import android.os.IBinder
import android.os.Parcel
import android.os.Process
import android.os.SystemClock
import java.security.MessageDigest

/** Host's authoritative, live, per-effect policy; no permissive default exists. */
interface HostDisplayGrants {
    fun allowCreateDisplay(deviceSessionId: Long, sessionGeneration: Long): Boolean
    fun allowLaunch(ref: DisplayRef, packageName: String, actionId: Long): Boolean
    fun allowInput(intent: InputIntent): Boolean
}

/**
 * Explicitly activated by the embedding App before requesting an adb launch.
 * The registered Context is an ordinary App context, not a shell Context.
 * A host-process restart loses the nonce and must re-authorize a new session.
 */
class TurboDisplayHandoffHost private constructor(
    private val app: Context,
    private val deviceSessionId: Long,
    private val sessionGeneration: Long,
    private val grants: HostDisplayGrants,
) {
    private val challenge = OneShotHandoff(SystemClock::elapsedRealtime)
    private val hostToken = Binder()
    private val certPin = PackageSigningPin.fromContext(app)
    private var authorizedPid = -1
    private var accepted: ITurboDisplayShell? = null
    @Volatile private var revoked = false

    /** Exposed only to the trusted shell process via the protected Provider. */
    private val authority = object : ITurboDisplayAuthority.Stub() {
        private fun shellIsBound(): Boolean =
            !revoked && Binder.getCallingUid() == ShellHandoffProof.SHELL_UID &&
                Binder.getCallingPid() == authorizedPid && authorizedPid > 0 &&
                active === this@TurboDisplayHandoffHost

        override fun allowCreate(deviceSessionId: Long, sessionGeneration: Long): Boolean =
            shellIsBound() && deviceSessionId == this@TurboDisplayHandoffHost.deviceSessionId &&
                sessionGeneration == this@TurboDisplayHandoffHost.sessionGeneration &&
                safeGrant { grants.allowCreateDisplay(deviceSessionId, sessionGeneration) }

        override fun allowLaunch(
            deviceSessionId: Long, sessionGeneration: Long,
            displayId: Int, displayGeneration: Long, geometryGeneration: Long,
            packageName: String, actionId: Long
        ): Boolean {
            if (!shellIsBound() || !matchesSession(deviceSessionId, sessionGeneration)) return false
            val ref = DisplayRef(deviceSessionId, sessionGeneration, displayId, displayGeneration, geometryGeneration)
            return displayId > 0 && displayGeneration > 0 && geometryGeneration > 0 &&
                packageName.isNotBlank() && actionId > 0 && safeGrant { grants.allowLaunch(ref, packageName, actionId) }
        }

        override fun allowInput(
            deviceSessionId: Long, sessionGeneration: Long,
            displayId: Int, displayGeneration: Long, geometryGeneration: Long,
            actionId: Long, attemptId: Long, actionKind: Int
        ): Boolean {
            if (!shellIsBound() || !matchesSession(deviceSessionId, sessionGeneration)) return false
            val kind = when (actionKind) {
                1 -> InputIntent.Kind.TAP
                2 -> InputIntent.Kind.BACK
                3 -> InputIntent.Kind.ENTER
                else -> return false
            }
            val ref = DisplayRef(deviceSessionId, sessionGeneration, displayId, displayGeneration, geometryGeneration)
            return displayId > 0 && displayGeneration > 0 && geometryGeneration > 0 &&
                actionId > 0 && attemptId > 0 &&
                safeGrant { grants.allowInput(InputIntent(ref, actionId, attemptId, kind)) }
        }

        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code in IBinder.FIRST_CALL_TRANSACTION..IBinder.LAST_CALL_TRANSACTION && !shellIsBound()) {
                throw SecurityException("unbound shell authority")
            }
            return super.onTransact(code, data, reply, flags)
        }
    }

    private fun matchesSession(id: Long, generation: Long) =
        id == deviceSessionId && generation == sessionGeneration

    @Synchronized
    fun launchSpec(): ShellLaunchSpec {
        check(!revoked && active === this) { "host session revoked" }
        check(accepted == null) { "helper already attached" }
        val nonce = challenge.issue()
        return ShellLaunchCommand.build(
            app.applicationInfo.sourceDir, app.packageName, Process.myUid(),
            certPin, deviceSessionId, sessionGeneration, nonce)
    }

    @Synchronized
    fun connectedShell(): ITurboDisplayShell? =
        if (revoked) null else accepted?.takeIf { it.asBinder().isBinderAlive }

    /** No retry or silent reconnection after process death. Reauthorization required. */
    private fun onShellDied(binder: IBinder) = synchronized(this) {
        if (accepted?.asBinder() === binder) {
            accepted = null
            authorizedPid = -1
            challenge.invalidate()
            revoked = true
            synchronized(Companion) { if (active === this) active = null }
        }
    }

    /** Revoke before remote teardown so outstanding host-grant requests deny. */
    fun revoke() {
        val old = synchronized(this) {
            if (revoked) return
            revoked = true
            challenge.invalidate()
            authorizedPid = -1
            accepted.also { accepted = null }
        }
        synchronized(Companion) { if (active === this) active = null }
        try { old?.closeSession() } catch (_: Exception) { /* remote may have died */ }
    }

    private fun offer(extras: Bundle?, callingUid: Int, callingPid: Int): Bundle = synchronized(this) {
        fun reject(): Bundle = Bundle().apply { putBoolean(KEY_ACCEPTED, false) }
        if (revoked || active !== this || accepted != null) return@synchronized reject()
        val binder = extras?.getBinder(KEY_BINDER) ?: return@synchronized reject()
        val claimedUid = extras.getInt(KEY_UID, -1)
        val claimedPid = extras.getInt(KEY_PID, -1)
        val clazzPath = extras.getString(KEY_APK)
        if (!ShellHandoffProof.valid(callingUid, callingPid, claimedUid, claimedPid,
                clazzPath, app.applicationInfo.sourceDir, true)) return@synchronized reject()
        val nonce = extras.getString(KEY_NONCE)
        if (!challenge.matches(nonce)) return@synchronized reject()
        // Clear incoming shell identity before making the outbound Binder call.
        val caller = Binder.clearCallingIdentity()
        val identities = try {
            ITurboDisplayShell.Stub.asInterface(binder)?.identity()
        } catch (_: Exception) { null } finally { Binder.restoreCallingIdentity(caller) }
        if (identities == null || identities.size != 2 || !ShellHandoffProof.valid(
                callingUid, callingPid, claimedUid, claimedPid, clazzPath,
                app.applicationInfo.sourceDir, identities[0] == callingUid && identities[1] == callingPid
            )) return@synchronized reject()
        val death = IBinder.DeathRecipient { onShellDied(binder) }
        try { binder.linkToDeath(death, 0) } catch (_: Exception) { return@synchronized reject() }
        if (!challenge.consume(nonce)) {
            binder.unlinkToDeath(death, 0)
            return@synchronized reject()
        }
        accepted = ITurboDisplayShell.Stub.asInterface(binder)
        authorizedPid = callingPid
        Bundle().apply {
            putBoolean(KEY_ACCEPTED, true)
            putBinder(KEY_HOST_TOKEN, hostToken)
            putBinder(KEY_AUTHORITY, authority.asBinder())
        }
    }

    companion object {
        const val METHOD_ATTACH = "attach"
        const val KEY_NONCE = "nonce"
        const val KEY_BINDER = "shell"
        const val KEY_UID = "uid"
        const val KEY_PID = "pid"
        const val KEY_APK = "apk"
        const val KEY_ACCEPTED = "accepted"
        const val KEY_HOST_TOKEN = "host_token"
        const val KEY_AUTHORITY = "host_authority"
        @Volatile private var active: TurboDisplayHandoffHost? = null

        /** The companion explicitly creates exactly one host session. */
        @Synchronized
        fun begin(context: Context, deviceSessionId: Long, sessionGeneration: Long,
                  grants: HostDisplayGrants): TurboDisplayHandoffHost {
            check(active == null) { "a helper handoff session is already active" }
            require(deviceSessionId > 0 && sessionGeneration > 0)
            check(Process.myUid() != 2000) { "host must be an App UID" }
            return TurboDisplayHandoffHost(context.applicationContext, deviceSessionId,
                sessionGeneration, grants).also { active = it }
        }

        internal fun receive(extras: Bundle?, uid: Int, pid: Int): Bundle =
            active?.offer(extras, uid, pid) ?: Bundle().apply { putBoolean(KEY_ACCEPTED, false) }

        private inline fun safeGrant(block: () -> Boolean): Boolean =
            try { block() } catch (_: Exception) { false }
    }
}

/** This merged AAR provider may be reached only by UID 2000 with the one-shot nonce. */
class TurboDisplayHandoffProvider : ContentProvider() {
    override fun onCreate(): Boolean = true

    override fun call(method: String, arg: String?, extras: Bundle?): Bundle? {
        if (method != TurboDisplayHandoffHost.METHOD_ATTACH) return null
        return TurboDisplayHandoffHost.receive(extras, Binder.getCallingUid(), Binder.getCallingPid())
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?,
                       selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null
    override fun getType(uri: Uri): String? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0
}

/** A no-fallback check of the actual installed package's signer certificates. */
internal object PackageSigningPin {
    fun fromContext(context: Context): String {
        val pkg = context.packageManager.getPackageInfo(context.packageName, PackageManager.GET_SIGNING_CERTIFICATES)
        val signatures = pkg.signingInfo?.apkContentsSigners?.map { it.toByteArray() }
            ?.takeIf { it.isNotEmpty() } ?: error("missing signing certificate")
        return pin(signatures)
    }

    fun pin(signatures: List<ByteArray>): String = signatures.map { der ->
        MessageDigest.getInstance("SHA-256").digest(der).joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }.sorted().joinToString(",")
}
