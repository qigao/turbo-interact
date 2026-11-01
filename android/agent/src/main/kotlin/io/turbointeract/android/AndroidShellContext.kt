/*
 * Portions adapted from scrcpy (https://github.com/Genymobile/scrcpy):
 *   Copyright (C) 2018 Genymobile
 *   Copyright (C) 2018-2024 Romain Vimont
 * The shell context bootstrap is also adapted from Mika (https://github.com/wangzan101/Mika),
 *   Copyright 2026 wangzan101.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0.
 */
package io.turbointeract.android

import android.annotation.SuppressLint
import android.app.Application
import android.app.Instrumentation
import android.content.AttributionSource
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ApplicationInfo
import android.os.Build
import android.os.Process
import android.hardware.display.DisplayManager

/**
 * Android framework services validate the calling UID/package identity.
 * This is for an already authorized shell (uid 2000) app_process only,
 * NOT a way for an ordinary installed app to obtain shell privileges.
 */
internal class AndroidShellContext private constructor(base: Context) : ContextWrapper(base) {
    override fun getPackageName(): String = SHELL_PACKAGE
    override fun getOpPackageName(): String = SHELL_PACKAGE
    override fun getApplicationContext(): Context = this

    override fun getAttributionSource(): AttributionSource =
        AttributionSource.Builder(SHELL_UID).setPackageName(SHELL_PACKAGE).build()

    companion object {
        const val SHELL_PACKAGE = "com.android.shell"
        const val SHELL_UID = 2000

        @SuppressLint("PrivateApi", "DiscouragedPrivateApi")
        fun create(): Context {
            check(Process.myUid() == SHELL_UID) { "shell context requires uid 2000" }
            val at = Class.forName("android.app.ActivityThread")
            val thread = at.getMethod("currentActivityThread").invoke(null) ?: newActivityThread(at)
            val system = at.getDeclaredMethod("getSystemContext").invoke(thread) as Context
            val context = AndroidShellContext(system)
            if (at.getDeclaredField("mInitialApplication").apply { isAccessible = true }.get(thread) == null) {
                runCatching {
                    val app = Instrumentation.newApplication(Application::class.java, context)
                    at.getDeclaredField("mInitialApplication").apply { isAccessible = true }.set(thread, app)
                }
            }
            return context
        }

        @SuppressLint("PrivateApi", "DiscouragedPrivateApi")
        fun displayManager(context: Context): DisplayManager {
            check(Process.myUid() == SHELL_UID && context.packageName == SHELL_PACKAGE)
            return DisplayManager::class.java.getDeclaredConstructor(Context::class.java)
                .apply { isAccessible = true }.newInstance(context)
        }

        @SuppressLint("PrivateApi", "DiscouragedPrivateApi", "SoonBlockedPrivateApi")
        private fun newActivityThread(at: Class<*>): Any {
            val thread = at.getDeclaredConstructor().apply { isAccessible = true }.newInstance()
            at.getDeclaredField("sCurrentActivityThread").apply { isAccessible = true }.set(null, thread)
            at.getDeclaredField("mSystemThread").apply { isAccessible = true }.setBoolean(thread, true)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) runCatching {
                val configController = Class.forName("android.app.ConfigurationController")
                val internal = Class.forName("android.app.ActivityThreadInternal")
                val controller = configController.getDeclaredConstructor(internal)
                    .apply { isAccessible = true }.newInstance(thread)
                at.getDeclaredField("mConfigurationController").apply { isAccessible = true }.set(thread, controller)
            }
            runCatching {
                val bindData = Class.forName("android.app.ActivityThread\$AppBindData")
                val data = bindData.getDeclaredConstructor().apply { isAccessible = true }.newInstance()
                bindData.getDeclaredField("appInfo").apply { isAccessible = true }
                    .set(data, ApplicationInfo().apply { packageName = SHELL_PACKAGE })
                at.getDeclaredField("mBoundApplication").apply { isAccessible = true }.set(thread, data)
            }
            return thread
        }
    }
}

/**
 * The upper bootstrap still owns one-time nonce + certificate/UID verification
 * and Binder delivery. Without that separate handshake this factory is not used.
 */
object AndroidShellBootstrap {
    fun newService(
        allowedCompanionUid: Int,
        deviceSessionId: Long,
        sessionGeneration: Long,
        admission: ShellAdmission,
    ): AuthenticatedDisplayShell {
        val context = AndroidShellContext.create()
        val manager = AndroidShellContext.displayManager(context)
        return AuthenticatedDisplayShell(
            allowedCompanionUid, context, manager, deviceSessionId, sessionGeneration, admission
        )
    }
}
