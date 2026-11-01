package io.turbointeract.android

import android.content.Context
import android.os.SystemClock
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * A host-reviewed, packaged, read-only native executable. This is deliberately
 * NOT a generic model-supplied command, script interpreter or secure sandbox.
 * A child of an Android App keeps the same App UID and SELinux confinement.
 */
data class TrustedSandboxCommand(
    val nativeLibraryName: String,
    val arguments: List<String> = emptyList(),
    val taskId: Long,
    val sandboxRunId: Long,
    val allowedTools: Int = NativeAndroidActionOwner.SANDBOX_TAP or NativeAndroidActionOwner.SANDBOX_BACK,
    val maxDurationMs: Long = 30_000L,
    val maxRequests: Int = 64,
    val maxOutputBytes: Int = 65_536,
)

enum class SandboxProcessOutcome {
    EXITED, DEADLINE, CALL_BUDGET, OUTPUT_LIMIT, CANCELLED
}

/** Process exit != agent task success; each accepted RPC is a backend effect only. */
data class SandboxProcessReceipt(
    val outcome: SandboxProcessOutcome,
    val exitCode: Int?,
    val acceptedCalls: Int,
    val rejectedCalls: Int,
    val output: String,
)

/** Pure host policy, also covered by the Android AAR's JVM tests. */
internal object TrustedSandboxLaunchPolicy {
    fun validate(command: TrustedSandboxCommand) {
        val name = command.nativeLibraryName
        require(name.startsWith("lib") && name.endsWith(".so") && name.length in 7..64)
        require(name.substring(3, name.length - 3).all { ch ->
            ch in 'A'..'Z' || ch in 'a'..'z' || ch in '0'..'9' || ch == '_'
        })
        require(command.taskId > 0 && command.sandboxRunId > 0)
        require(command.allowedTools in 1..3)
        require(command.maxDurationMs in 100L..120_000L)
        require(command.maxRequests in 1..4096)
        require(command.maxOutputBytes in 1024..262_144)
        require(command.arguments.size <= 32)
        require(command.arguments.sumOf { it.toByteArray(Charsets.UTF_8).size } <= 4096)
        require(command.arguments.all { value ->
            value.length <= 4096 && value.none { ch -> ch.code == 0 || ch.code == 10 || ch.code == 13 }
        })
    }

    /** The child reads 3 ASCII/UTF-8 lines on stdin, never secret argv/env. */
    fun handoff(credentials: NativeSandboxCredentials): ByteArray {
        val path = credentials.socketPath
        require(path.all { it.code in 33..126 && it != '\\' } &&
            path.none { it == '\u0000' || it == '\n' || it == '\r' })
        return ("TURBOINTERACT-SANDBOX/1\n" + path + "\n" +
            credentials.tokenHex + "\n").toByteArray(Charsets.US_ASCII)
    }

    fun trustedExecutable(context: Context, command: TrustedSandboxCommand): File {
        validate(command)
        val root = File(context.applicationInfo.nativeLibraryDir).canonicalFile
        val target = File(root, command.nativeLibraryName).canonicalFile
        check(target.parentFile == root && target.isFile && target.canExecute() && !target.canWrite()) {
            "sandbox runner must be a packaged immutable executable native library"
        }
        return target
    }
}

private class SandboxOutputCollector(
    private val process: Process,
    private val limit: Int,
) {
    private val bytes = ByteArrayOutputStream()
    @Volatile var exceeded = false
        private set
    private val worker = thread(start = true, isDaemon = true, name = "ti-sandbox-output") {
        try {
            process.inputStream.use { stream ->
                val buffer = ByteArray(4096)
                while (true) {
                    val n = stream.read(buffer)
                    if (n < 0) break
                    synchronized(bytes) {
                        val remaining = limit - bytes.size()
                        if (n > remaining) {
                            if (remaining > 0) bytes.write(buffer, 0, remaining)
                            exceeded = true
                        } else bytes.write(buffer, 0, n)
                    }
                    if (exceeded) {
                        process.destroyForcibly()
                        break
                    }
                }
            }
        } catch (_: IOException) {
            // Stream closure on cancellation is an expected terminal state.
        }
    }

    fun finish(): String {
        worker.join(1_000L)
        return synchronized(bytes) { String(bytes.toByteArray(), Charsets.UTF_8) }
    }
}

/**
 * A single-use, synchronous host-controlled command lifecycle.
 *
 * Only the explicitly selected, immutable packaged executable is launched,
 * with a fixed argv array (no shell -c/eval, no model-selected executable).
 * The host must call run() from an IO worker, not the Android UI thread.
 * A qualified isolated-service/container provider is REQUIRED before treating
 * arbitrary model-supplied scripts as isolated untrusted code.
 *
 * The child inherits the App UID. Its only phone-tool capability is the
 * bounded socket + token + live ActionOwner approval; it has no shell Binder.
 */
class TrustedSandboxCommandRunner(
    private val context: Context,
    private val owner: NativeAndroidActionOwner,
) {
    private val started = AtomicBoolean(false)
    private val cancelled = AtomicBoolean(false)
    @Volatile private var current: Process? = null

    /** Safe concurrent cancellation; serveSandboxOnce has a <=250ms poll. */
    fun cancel() {
        cancelled.set(true)
        current?.destroyForcibly()
    }

    fun run(command: TrustedSandboxCommand): SandboxProcessReceipt {
        check(started.compareAndSet(false, true)) { "sandbox runner is single-use" }
        TrustedSandboxLaunchPolicy.validate(command)
        check(!cancelled.get()) { "sandbox task already cancelled" }
        val executable = TrustedSandboxLaunchPolicy.trustedExecutable(context, command)
        val credentials = owner.openSandbox(
            context.filesDir, command.taskId, command.sandboxRunId, command.allowedTools
        )
        var process: Process? = null
        var collector: SandboxOutputCollector? = null
        var accepted = 0
        var rejected = 0
        var outcome: SandboxProcessOutcome
        var exitCode: Int? = null
        try {
            val builder = ProcessBuilder(listOf(executable.absolutePath) + command.arguments)
            builder.directory(context.filesDir)
            builder.redirectErrorStream(true)
            // Never forward App credentials, ADB pairing details or other
            // ambient process environment to the sandbox worker.
            builder.environment().clear()
            builder.environment()["PATH"] = "/system/bin"
            builder.environment()["HOME"] = context.filesDir.absolutePath
            builder.environment()["TMPDIR"] = context.cacheDir.absolutePath
            builder.environment()["LANG"] = "C"
            process = builder.start()
            current = process
            collector = SandboxOutputCollector(process, command.maxOutputBytes)
            val payload = TrustedSandboxLaunchPolicy.handoff(credentials)
            try {
                process.outputStream.use { stream ->
                    stream.write(payload)
                    stream.flush()
                }
            } finally {
                payload.fill(0)
            }

            val begun = SystemClock.elapsedRealtime()
            outcome = SandboxProcessOutcome.DEADLINE
            while (!cancelled.get()) {
                if (collector.exceeded) { outcome = SandboxProcessOutcome.OUTPUT_LIMIT; break }
                if (!process.isAlive) { outcome = SandboxProcessOutcome.EXITED; break }
                val elapsed = SystemClock.elapsedRealtime() - begun
                if (elapsed >= command.maxDurationMs) break
                if (accepted + rejected >= command.maxRequests) {
                    outcome = SandboxProcessOutcome.CALL_BUDGET
                    break
                }
                val remaining = (command.maxDurationMs - elapsed).coerceAtMost(250L).toInt()
                val status = owner.serveSandboxOnce(remaining)
                if (status == 0) accepted++
                else if (status != -7) rejected++ // -7 = listener timeout / expired request
            }
            if (cancelled.get()) outcome = SandboxProcessOutcome.CANCELLED
            else if (collector.exceeded) outcome = SandboxProcessOutcome.OUTPUT_LIMIT
            else if (process.isAlive && accepted + rejected >= command.maxRequests) {
                outcome = SandboxProcessOutcome.CALL_BUDGET
            }
            if (outcome == SandboxProcessOutcome.EXITED) {
                exitCode = process.exitValue()
            }
        } finally {
            val running = process
            if (running != null) {
                if (running.isAlive) running.destroyForcibly()
                try { running.waitFor(2L, TimeUnit.SECONDS) }
                catch (interrupted: InterruptedException) {
                    Thread.currentThread().interrupt()
                }
            }
            current = null
            val closed = owner.closeSandbox()
            check(closed) { "sandbox socket still borrowed; native owner MUST NOT be destroyed" }
        }
        val output = (collector?.finish() ?: "").replace(credentials.tokenHex, "[redacted]")
        return SandboxProcessReceipt(outcome, exitCode, accepted, rejected, output)
    }
}
