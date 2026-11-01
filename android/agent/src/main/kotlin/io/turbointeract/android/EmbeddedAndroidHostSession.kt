package io.turbointeract.android

import android.content.Context
import android.graphics.Bitmap
import android.os.Looper

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
    private val monitor = Any()
    private var activeCalls = 0
    private var closing = false
    private var terminated = false
    private var closeAttemptRunning = false

    @Volatile
    var teardownReport: EmbeddedHostTeardownReport? = null
        private set

    private inline fun <T> borrowed(block: () -> T): T {
        synchronized(monitor) {
            check(!closing && !terminated) { "embedded host session revoked or closing" }
            activeCalls++
        }
        return try { block() } finally {
            synchronized(monitor) {
                activeCalls--
                check(activeCalls >= 0) { "host session borrow underflow" }
            }
        }
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
        synchronized(monitor) {
            if (terminated) return true
            if (closeAttemptRunning) return false
            closing = true
            closeAttemptRunning = true
        }
        try {
            actionOwner.revoke() // Cancels a direct worker without monitor held.
            synchronized(monitor) { if (activeCalls != 0) return false }
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
            synchronized(monitor) { terminated = true }
            return true
        } finally {
            synchronized(monitor) { closeAttemptRunning = false }
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
