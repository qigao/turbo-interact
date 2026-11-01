/*
 * Inspired by Mika's HelperHandoff / HandoffNonces, Copyright 2026 wangzan101.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unlike Mika's persistent resume/USB script nonce slots, TurboInteract keeps
 * one launch challenge in host memory and never enables automatic helper restore.
 */
package io.turbointeract.android

import java.security.MessageDigest
import java.security.SecureRandom

/**
 * One 256-bit launch challenge, issued by the intended host and valid for 30s.
 * A rejected offer must not consume it; a successful offer consumes it once.
 * Time uses Android elapsedRealtime when constructed in the host app.
 */
class OneShotHandoff(
    private val clockMs: () -> Long,
    private val randomBytes: (ByteArray) -> Unit = SecureRandom()::nextBytes,
    private val ttlMs: Long = 30_000L,
) {
    init { require(ttlMs in 1L..120_000L) }

    private var expected: String? = null
    private var issuedAt: Long = 0L

    @Synchronized
    fun issue(): String {
        val bytes = ByteArray(32)
        randomBytes(bytes)
        return bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }.also {
            expected = it
            issuedAt = clockMs()
            bytes.fill(0)
        }
    }

    @Synchronized
    fun matches(supplied: String?): Boolean {
        val challenge = expected ?: return false
        if (supplied == null || supplied.length != 64 || supplied.any { it !in '0'..'9' && it !in 'a'..'f' }) return false
        val age = clockMs() - issuedAt
        return age in 0..ttlMs && MessageDigest.isEqual(
            challenge.toByteArray(Charsets.US_ASCII), supplied.toByteArray(Charsets.US_ASCII))
    }

    @Synchronized
    fun consume(supplied: String?): Boolean {
        if (!matches(supplied)) return false
        invalidate()
        return true
    }

    @Synchronized
    fun invalidate() {
        expected = null
        issuedAt = 0L
    }
}

/** Pure handoff admission; Binder/process and signature verification are external inputs. */
object ShellHandoffProof {
    const val SHELL_UID = 2000

    fun valid(
        callingUid: Int, callingPid: Int, reportedUid: Int, reportedPid: Int,
        helperClassPath: String?, installedApkPath: String,
        binderProcessMatches: Boolean,
    ): Boolean = callingUid == SHELL_UID && callingPid > 0 &&
        reportedUid == callingUid && reportedPid == callingPid &&
        installedApkPath.isNotBlank() && helperClassPath == installedApkPath &&
        binderProcessMatches
}

/**
 * Command contains no secret. The host sends nonceLine through its authenticated
 * ADB exec session's stdin, never argv or an installable shell script.
 * The caller owns ADB permission/pairing; this class never opens adbd.
 */
data class ShellLaunchSpec(val command: String, val nonceLine: String) {
    // nonceLine is a one-use secret. Data-class default toString would leak
    // it into exception/log output before the authenticated ADB handoff.
    override fun toString(): String = "ShellLaunchSpec(<redacted>)"
}

object ShellLaunchCommand {
    const val HELPER_CLASS = "io.turbointeract.android.TurboDisplayShellMain"

    fun build(
        apkPath: String, packageName: String, appUid: Int,
        certSha256: String, deviceSessionId: Long, sessionGeneration: Long,
        nonce: String,
    ): ShellLaunchSpec {
        require(apkPath.startsWith("/") && apkPath.endsWith(".apk") && '\u0000' !in apkPath && '\n' !in apkPath)
        require(packageName.matches(Regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+")))
        require(appUid > 0 && appUid != 2000 && deviceSessionId > 0 && sessionGeneration > 0)
        require(certSha256.matches(Regex("[0-9a-f]{64}(,[0-9a-f]{64})*")))
        require(nonce.matches(Regex("[0-9a-f]{64}")))
        val proc = "$packageName:ti-helper"
        val command = "read -r n; (printf '%s\\n' \"\$n\" | CLASSPATH=${quote(apkPath)} setsid app_process /system/bin " +
            "--nice-name=${quote(proc)} $HELPER_CLASS ${quote(packageName)} $appUid ${quote(certSha256)} " +
            "$deviceSessionId $sessionGeneration >/dev/null 2>&1 &); read -r _"
        return ShellLaunchSpec(command, "$nonce\n")
    }

    private fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
}
