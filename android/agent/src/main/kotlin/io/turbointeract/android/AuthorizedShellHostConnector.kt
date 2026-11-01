package io.turbointeract.android

import android.os.Looper
import android.os.SystemClock
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Only an existing embedding host may implement this capability after it has
 * separately authenticated and authorized its ADB shell transport.
 *
 * launchAuthorized MUST deliver the exact launch.command to the authorized
 * exec service and launch.nonceLine to that invocation's stdin (NOT argv,
 * a stored script, a log or another ADB session), then promptly return a
 * lease that keeps the transport alive until handoff or termination.
 *
 * This AAR does not discover/pair with ADB, issue arbitrary shell commands,
 * install an App, or provide an allow-all permission policy.
 */
fun interface AuthorizedAdbShellTransport {
    fun launchAuthorized(launch: ShellLaunchSpec): AutoCloseable
}

/**
 * A live Shell session obtained via an authenticated one-shot host Provider
 * handoff; it is NOT a grant to invoke input. The shell consults the canonical
 * ActionOwner's live one-use ticket before each external input side effect.
 *
 * Closing revokes host grants before stopping the supplied transport.
 * The embedding host must separately release display Surface/capture and
 * native ActionOwner resources as described in M5d.
 */
class AuthorizedShellConnection internal constructor(
    val shell: ITurboDisplayShell,
    private val host: TurboDisplayHandoffHost,
    private val lease: AutoCloseable,
) : AutoCloseable {
    private val closed = AtomicBoolean(false)

    override fun close() {
        if (closed.compareAndSet(false, true)) {
            try { host.revoke() } finally { lease.close() }
        }
    }
}

/**
 * Bounded, fail-closed connection orchestration for an EXISTING host APK.
 * Must run on its IO thread, not the UI thread or Binder callback thread.
 *
 * This does not assume that an exec command succeeded: only an actual,
 * identity-verified Binder offer at the existing TurboDisplayHandoffHost
 * can produce a connectedShell(). One launch, no retry, no automatic
 * installation, no fallback to display 0 or to a different shell authority.
 */
object AuthorizedShellHostConnector {
    fun connect(
        host: TurboDisplayHandoffHost,
        transport: AuthorizedAdbShellTransport,
        timeoutMs: Long = 15_000L,
    ): AuthorizedShellConnection {
        require(timeoutMs in 100L..29_000L) { "handoff must fit the one-use 30s nonce TTL" }
        check(Looper.myLooper() != Looper.getMainLooper()) {
            "launch and Binder handoff must run outside Android UI thread"
        }
        val beginMs = SystemClock.elapsedRealtime()
        var lease: AutoCloseable? = null
        var established = false
        try {
            val launch = host.launchSpec()
            lease = transport.launchAuthorized(launch)
            while (true) {
                check(host.activeForBootstrap()) { "authenticated host was revoked" }
                check(!Thread.currentThread().isInterrupted) { "handoff interrupted" }
                val elapsedMs = SystemClock.elapsedRealtime() - beginMs
                check(elapsedMs in 0 until timeoutMs) { "authorized Shell handoff expired" }
                // The host checks real OS Binder UID/PID, exact package/signature,
                // a one-time nonce, and the helper Binder identity before this.
                val shell = host.connectedShell()
                if (shell != null) {
                    val connection = AuthorizedShellConnection(shell, host, checkNotNull(lease))
                    established = true
                    return connection
                }
                val remainingMs = timeoutMs - elapsedMs
                try {
                    Thread.sleep(remainingMs.coerceAtMost(25L))
                } catch (e: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("authorized Shell handoff interrupted", e)
                }
            }
        } finally {
            if (!established) {
                // Revocation invalidates the one-shot nonce and any pending
                // callback before the user's transport lease is torn down.
                host.revoke()
                runCatching { lease?.close() }
            }
        }
    }
}
