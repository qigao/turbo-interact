package io.turbointeract.android

import android.content.Context
import android.graphics.Bitmap
import android.os.Looper

/** A local call-borrow fence, NOT an alternative C11 input authority. */
internal enum class HostCloseAdmission { ALREADY_TERMINATED, OTHER_CLOSE_ACTIVE, ATTEMPT }

/**
 * Makes first-close terminal for new calls while allowing a later close retry
 * after already-borrowed host/worker calls finish. No Binder/JNI or callbacks
 * may run under this monitor. Tests exercise this exact product state gate.
 */
internal class EmbeddedHostCallGate {
    private val monitor = Any()
    private var active = 0
    private var closing = false
    private var attempt = false
    private var terminated = false

    fun borrow(): Boolean = synchronized(monitor) {
        if (closing || terminated) false else { active++; true }
    }

    fun release() = synchronized(monitor) {
        check(active > 0) { "host lifecycle borrow underflow" }
        active--
    }

    fun startClose(): HostCloseAdmission = synchronized(monitor) {
        when {
            terminated -> HostCloseAdmission.ALREADY_TERMINATED
            attempt -> HostCloseAdmission.OTHER_CLOSE_ACTIVE
            else -> {
                closing = true
                attempt = true
                HostCloseAdmission.ATTEMPT
            }
        }
    }

    fun idle(): Boolean = synchronized(monitor) { active == 0 }

    fun finishClose(completed: Boolean) = synchronized(monitor) {
        check(attempt) { "no active host teardown" }
        if (completed) terminated = true
        attempt = false
        // closing deliberately remains true after BUSY/error, so the native
        // borrower can finish but no new host calls can sneak into a retry.
    }
}

/**
 * Local teardown observations, not Android system process/compositor proof.
 * A false acknowledgement means the remote Shell may already have died;
 * revocation still denies future input.
 */
data class EmbeddedHostTeardownReport(
    val exactDisplayReleaseAcknowledged: Boolean,
    val authorizedTransportCloseReturnedNormally: Boolean,
)

/**
 * One trusted host App session, one UID-2000 authenticated Shell, one owned
 * VirtualDisplay, and exactly one authoritative native C11 ActionOwner.
 *
 * This is a library facade for an EXISTING embedding APK, not a separate
 * consumer/installer project, an ADB pairing implementation or an isolated
 * sandbox for untrusted model scripts.
 */
class EmbeddedAndroidHostSession private constructor(
    private val application: Context,
    private val connection: AuthorizedShellConnection,
    val snapshot: DisplaySnapshot,
    val capture: AndroidRgbaCapture,
    val actionOwner: NativeAndroidActionOwner,
) : AutoCloseable {
    private val calls = EmbeddedHostCallGate()

    @Volatile
    var teardownReport: EmbeddedHostTeardownReport? = null
        private set

    private inline fun <T> borrowed(block: () -> T): T {
        check(calls.borrow()) { "embedded host session revoked or closing" }
        return try { block() } finally { calls.release() }
    }

    /** Shell launch status is NOT proof that the target App is visible. */
    fun launchTarget(packageName: String, launchActionId: Long): Int = borrowed {
        require(packageName.isNotBlank() && launchActionId > 0)
        val ref = snapshot.ref
        connection.shell.launchApp(ref.displayId, ref.displayGeneration,
            ref.geometryGeneration, packageName, launchActionId)
    }

    /**
     * A true visual predicate only means the expected later UI state exists.
     * It is NOT an input/compositor-causal fence or task success certificate.
     * 'before' must be a real caller-owned bitmap from this capture, and
     * must be recycled by the caller after this operation.
     */
    fun observeInput(
        action: NativeInputAction,
        before: AndroidRgbaCapture.Frame,
        waitForNewFrameMs: Long = 3_000L,
        visualPostcondition: (Bitmap) -> Boolean,
    ): CapturedInputObservation = borrowed {
        check(Looper.myLooper() != Looper.getMainLooper()) {
            "native Binder input and frame collection require a host IO thread"
        }
        val stamp = before.stamp ?: error("unbound source capture")
        require(stamp.display == snapshot.ref &&
            action.display == snapshot.ref &&
            action.captureSequence == stamp.sequence &&
            action.planGeneration == actionOwner.planGeneration) {
            "input must belong to this native owner and source capture"
        }
        CapturedInputObservationRunner.run(actionOwner, capture, action, stamp,
            waitForNewFrameMs, visualPostcondition)
    }

    /**
     * Executes only the reviewed packaged one-shot C11 worker under the same
     * host App UID. SANDBOX is the fixed OS-Socket origin, not an OS-isolated
     * principal; a successful child/IPC receipt is not visual task success.
     */
    fun runTrustedSandboxAction(
        action: NativeInputAction,
        callId: Long,
        taskId: Long,
        sandboxRunId: Long,
    ): SandboxProcessReceipt = borrowed {
        check(Looper.myLooper() != Looper.getMainLooper()) {
            "native child and Socket IPC require a host IO thread"
        }
        require(action.display == snapshot.ref &&
            action.planGeneration == actionOwner.planGeneration) {
            "worker action belongs to a different session"
        }
        val command = TrustedSandboxCommand.forSandboxAction(
            action, callId, taskId, sandboxRunId)
        TrustedSandboxCommandRunner(application, actionOwner).run(command)
    }

    /**
     * Terminal intent begins at first tryClose: no new host calls thereafter.
     * Returns false while an already admitted host call or native borrower is
     * still active. Caller retries from an IO thread; there is no hidden
     * blocking teardown loop or unsafe release while JNI/Binder is in-flight.
     */
    fun tryClose(): Boolean {
        check(Looper.myLooper() != Looper.getMainLooper()) {
            "host session teardown requires a host IO thread"
        }
        when (calls.startClose()) {
            HostCloseAdmission.ALREADY_TERMINATED -> return true
            HostCloseAdmission.OTHER_CLOSE_ACTIVE -> return false
            HostCloseAdmission.ATTEMPT -> Unit
        }
        var completed = false
        try {
            actionOwner.revoke() // Cancels a direct worker without monitor held.
            if (!calls.idle()) return false
            if (!actionOwner.tryClose()) return false

            val ref = snapshot.ref
            val released = runCatching {
                connection.shell.releaseDisplay(ref.displayId, ref.displayGeneration)
            }.getOrDefault(false)
            // If exact release cannot be confirmed (e.g. Shell process death),
            // revoking the entire authenticated Shell is the safe fallback.
            val transportClosed = runCatching { connection.close() }.isSuccess
            runCatching { capture.close() }
            teardownReport = EmbeddedHostTeardownReport(released, transportClosed)
            completed = true
            return true
        } finally {
            calls.finishClose(completed)
        }
    }

    override fun close() {
        check(tryClose()) { "in-flight host/JNI call: retry tryClose after drain" }
    }

    companion object {
        /**
         * Product source composition. Only the caller's EXISTING authenticated
         * ADB transport can run the Shell command. There is no default
         * allow-all grant and no implicit target package.
         */
        fun open(
            context: Context,
            deviceSessionId: Long,
            sessionGeneration: Long,
            geometry: DisplayGeometry,
            transport: AuthorizedAdbShellTransport,
            controls: HostSessionControlPolicy,
            inputPolicy: LiveNativeInputPolicy,
            providerId: Int,
            capabilities: Int,
            planGeneration: Long,
            handoffTimeoutMs: Long = 15_000L,
            attemptCapacity: Int = 4096,
        ): EmbeddedAndroidHostSession {
            require(deviceSessionId > 0 && sessionGeneration > 0)
            require(providerId > 0 && capabilities > 0 && planGeneration > 0)
            require(attemptCapacity in 1..4096)
            require(handoffTimeoutMs in 100L..29_000L)
            check(Looper.myLooper() != Looper.getMainLooper()) {
                "host setup must not block the UI thread"
            }
            val app = context.applicationContext
            val grants = CanonicalHostGrants(deviceSessionId, sessionGeneration, controls)
            val handoff = TurboDisplayHandoffHost.begin(
                app, deviceSessionId, sessionGeneration, grants)
            var connection: AuthorizedShellConnection? = null
            var capture: AndroidRgbaCapture? = null
            var snapshot: DisplaySnapshot? = null
            var owner: NativeAndroidActionOwner? = null
            try {
                connection = AuthorizedShellHostConnector.connect(
                    handoff, transport, handoffTimeoutMs)
                capture = AndroidRgbaCapture(geometry.width, geometry.height)
                snapshot = AuthorizedRgbaDisplayCreator.create(
                    connection, capture, geometry, deviceSessionId, sessionGeneration)
                owner = NativeAndroidActionOwner.create(
                    snapshot.ref, connection.shell, inputPolicy, providerId,
                    capabilities, planGeneration, attemptCapacity)
                grants.bindNativeOwner(owner)
                return EmbeddedAndroidHostSession(app, connection, snapshot,
                    capture, owner)
            } catch (failure: Throwable) {
                // Fail closed, including a Binder failure after the actual
                // VirtualDisplay was created but before the host received it.
                runCatching { owner?.revoke() }
                runCatching { owner?.tryClose() }
                if (snapshot != null && connection != null) {
                    runCatching { connection.shell.releaseDisplay(
                        snapshot.ref.displayId, snapshot.ref.displayGeneration) }
                }
                try {
                    if (connection != null) connection.close() else handoff.revoke()
                } catch (closeFailure: Throwable) {
                    failure.addSuppressed(closeFailure)
                }
                runCatching { capture?.close() }
                throw failure
            }
        }
    }
}
