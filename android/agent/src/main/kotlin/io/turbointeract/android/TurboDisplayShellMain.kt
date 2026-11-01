/*
 * Based on Mika HelperProcess handoff via ActivityManager external ContentProvider.
 * Copyright 2026 wangzan101, Apache-2.0.
 * Only the display/task/capture helper is supported; no Mika app restoration,
 * auto-enable Accessibility, arbitrary runCommand, unlock or clipboard access.
 */
package io.turbointeract.android

import android.annotation.SuppressLint
import android.content.AttributionSource
import android.content.Context
import android.content.pm.PackageManager
import android.content.pm.ProviderInfo
import android.os.Binder
import android.os.Bundle
import android.os.IBinder
import android.os.Looper
import android.os.Process
import java.security.MessageDigest
import kotlin.system.exitProcess

/** Grants must go back to the live host authority for EVERY attempted effect. */
internal class RemoteDisplayAdmission(
    private val deviceSessionId: Long,
    private val sessionGeneration: Long,
) : ShellAdmission {
    @Volatile private var authority: ITurboDisplayAuthority? = null

    fun attach(remote: ITurboDisplayAuthority) { authority = remote }
    fun revoke() { authority = null }

    override fun allowCreateDisplay(): Boolean = permit {
        it.allowCreate(deviceSessionId, sessionGeneration)
    }

    override fun allowLaunch(ref: DisplayRef, packageName: String, actionId: Long): Boolean = permit {
        it.allowLaunch(ref.deviceSessionId, ref.sessionGeneration,
            ref.displayId, ref.displayGeneration, ref.geometryGeneration, packageName, actionId)
    }

    override fun allowInput(intent: InputIntent): Boolean = permit {
        val r = intent.display
        val kind = when (intent.kind) {
            InputIntent.Kind.TAP -> 1
            InputIntent.Kind.BACK -> 2
            InputIntent.Kind.ENTER -> 3
        }
        it.allowInput(r.deviceSessionId, r.sessionGeneration, r.displayId,
            r.displayGeneration, r.geometryGeneration, intent.actionId, intent.attemptId, kind)
    }

    private inline fun permit(block: (ITurboDisplayAuthority) -> Boolean): Boolean {
        val current = authority ?: return false
        return try { block(current) } catch (_: Exception) { false }
    }
}

/**
 * Main class launched under uid=2000 by a user-authorized ADB exec session.
 * Startup never copies secrets into process arguments, files or logcat.
 * The host App must have registered TurboDisplayHandoffHost and issued
 * ShellLaunchSpec before this process can be accepted.
 */
object TurboDisplayShellMain {
    private const val SHELL_UID = 2000

    @JvmStatic
    fun main(args: Array<String>) {
        if (Process.myUid() != SHELL_UID || args.size != 5) exitProcess(2)
        val packageName = args[0]
        val appUid = args[1].toIntOrNull() ?: exitProcess(2)
        val pinnedSigner = args[2]
        val deviceSessionId = args[3].toLongOrNull() ?: exitProcess(2)
        val sessionGeneration = args[4].toLongOrNull() ?: exitProcess(2)
        if (appUid <= 0 || appUid == SHELL_UID || deviceSessionId <= 0 || sessionGeneration <= 0 ||
            !packageName.matches(Regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+")) ||
            !pinnedSigner.matches(Regex("[0-9a-f]{64}(,[0-9a-f]{64})*"))) exitProcess(2)

        val nonce = try { System.`in`.bufferedReader().readLine() } catch (_: Exception) { null }
        if (nonce == null || !nonce.matches(Regex("[0-9a-f]{64}"))) exitProcess(2)

        try {
            Looper.prepareMainLooper()
            val context = AndroidShellContext.create()
            if (!signedInstalledPackage(context, packageName, appUid, pinnedSigner)) exitProcess(3)
            val bridge = RemoteDisplayAdmission(deviceSessionId, sessionGeneration)
            val service = AndroidShellBootstrap.newService(appUid, deviceSessionId, sessionGeneration, bridge)
            if (!handOff(context, packageName, appUid, nonce, service, bridge)) {
                service.terminateFromHostDeath()
                exitProcess(4)
            }
            Looper.loop() // Only serves authenticated host Binder calls.
        } catch (_: Exception) {
            exitProcess(5)
        }
    }

    private fun signedInstalledPackage(
        context: Context, packageName: String, appUid: Int, pinnedSigner: String
    ): Boolean {
        return try {
        val info = context.packageManager.getPackageInfo(packageName, PackageManager.GET_SIGNING_CERTIFICATES)
        val actual = info.signingInfo?.apkContentsSigners?.map { it.toByteArray() }
            ?.takeIf { it.isNotEmpty() } ?: return false
        val sourceDir = info.applicationInfo?.sourceDir ?: return false
        val classPath = System.getProperty("java.class.path") ?: return false
        val pin = PackageSigningPin.pin(actual)
        info.applicationInfo?.uid == appUid && sourceDir == classPath &&
            MessageDigest.isEqual(pin.toByteArray(Charsets.US_ASCII), pinnedSigner.toByteArray(Charsets.US_ASCII))
    } catch (_: Exception) {
        false
    }
    }

    /**
     * Package/UID checked by the external ContentProvider holder, while
     * the App provider independently verifies Binder caller UID/PID/APK,
     * service Binder identity and consumes a 30s one-time nonce.
     */
    @SuppressLint("PrivateApi", "DiscouragedPrivateApi")
    private fun handOff(
        context: Context, packageName: String, appUid: Int, nonce: String,
        service: AuthenticatedDisplayShell, bridge: RemoteDisplayAdmission,
    ): Boolean {
        val authority = "$packageName.turbo_interact_helper"
        val userId = appUid / 100_000
        val am = try {
            Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null)
        } catch (_: Exception) { null } ?: return false
        val lease = Binder()
        val holder = try {
            am.javaClass.getMethod(
                "getContentProviderExternal", String::class.java, Int::class.javaPrimitiveType,
                IBinder::class.java, String::class.java
            ).invoke(am, authority, userId, lease, "TurboInteractShell")
        } catch (_: Exception) { null } ?: return false
        try {
            val info = holder.javaClass.getField("info").get(holder) as? ProviderInfo ?: return false
            if (info.packageName != packageName || info.applicationInfo?.uid != appUid ||
                info.authority != authority) return false
            val remoteProvider = holder.javaClass.getField("provider").get(holder) ?: return false
            val extras = Bundle().apply {
                putString(TurboDisplayHandoffHost.KEY_NONCE, nonce)
                putBinder(TurboDisplayHandoffHost.KEY_BINDER, service)
                putInt(TurboDisplayHandoffHost.KEY_UID, Process.myUid())
                putInt(TurboDisplayHandoffHost.KEY_PID, Process.myPid())
                putString(TurboDisplayHandoffHost.KEY_APK, System.getProperty("java.class.path"))
            }
            val call = remoteProvider.javaClass.methods.firstOrNull {
                it.name == "call" && it.parameterTypes.size == 5 &&
                    it.parameterTypes[0] == AttributionSource::class.java
            } ?: return false
            val caller = AttributionSource.Builder(SHELL_UID).setPackageName("com.android.shell").build()
            val response = call.invoke(remoteProvider, caller, authority,
                TurboDisplayHandoffHost.METHOD_ATTACH, null, extras) as? Bundle ?: return false
            if (!response.getBoolean(TurboDisplayHandoffHost.KEY_ACCEPTED, false)) return false

            val authorityBinder = response.getBinder(TurboDisplayHandoffHost.KEY_AUTHORITY) ?: return false
            val hostToken = response.getBinder(TurboDisplayHandoffHost.KEY_HOST_TOKEN) ?: return false
            val hostAuthority = ITurboDisplayAuthority.Stub.asInterface(authorityBinder) ?: return false
            bridge.attach(hostAuthority)
            try {
                hostToken.linkToDeath({
                    bridge.revoke()
                    service.terminateFromHostDeath()
                    exitProcess(0) // No privileged helper survives host-process death.
                }, 0)
            } catch (_: Exception) {
                bridge.revoke()
                return false
            }
            if (!hostToken.isBinderAlive) {
                bridge.revoke()
                return false
            }
            return true
        } catch (_: Exception) {
            bridge.revoke()
            return false
        } finally {
            runCatching {
                am.javaClass.getMethod(
                    "removeContentProviderExternalAsUser", String::class.java,
                    IBinder::class.java, Int::class.javaPrimitiveType
                ).invoke(am, authority, lease, userId)
            }
        }
    }
}
