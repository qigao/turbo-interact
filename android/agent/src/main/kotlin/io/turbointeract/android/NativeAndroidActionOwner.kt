package io.turbointeract.android

import android.os.SystemClock

/**
 * Typed host request. The JNI protocol is private to this Android library:
 * 17 signed 64-bit cells, versioned at index zero, never a public wire ABI.
 * C11 ActionOwner still validates every field and rechecks live authority.
 */
data class NativeInputAction(
    val display: DisplayRef,
    val origin: Int,
    val kind: Int,
    val actionId: Long,
    val attemptId: Long,
    val evidenceId: Long,
    val captureSequence: Long,
    val planGeneration: Long,
    val deadlineNs: Long,
    val xNormalized: Int = 0,
    val yNormalized: Int = 0,
) {
    init {
        require(origin in 1..3 && kind in listOf(TAP, BACK))
        require(actionId > 0 && attemptId > 0 && planGeneration > 0 && deadlineNs > 0)
        require(display.deviceSessionId > 0 && display.sessionGeneration > 0)
        require(display.displayId > 0 && display.displayGeneration > 0 && display.geometryGeneration > 0)
        require(xNormalized in 0..1_000_000 && yNormalized in 0..1_000_000)
        if (kind == TAP) require(evidenceId > 0 && captureSequence > 0)
    }

    fun values(nowNs: Long): LongArray = longArrayOf(
        1L, origin.toLong(), kind.toLong(), actionId, attemptId,
        evidenceId, captureSequence, display.deviceSessionId,
        display.sessionGeneration, display.displayId.toLong(),
        display.displayGeneration, display.geometryGeneration,
        planGeneration, deadlineNs, xNormalized.toLong(), yNormalized.toLong(), nowNs
    )

    companion object {
        const val MODEL = 1
        const val SANDBOX = 2
        const val SCRIPT = 3
        const val TAP = 4
        const val BACK = 6

        /** The only versioned request format accepted by the JNI adapter. */
        fun fromNative(values: LongArray): NativeInputAction? = try {
            if (values.size != 17 || values[0] != 1L ||
                values[9] <= 0L || values[9] > Int.MAX_VALUE ||
                values[14] !in 0..1_000_000L || values[15] !in 0..1_000_000L) null
            else NativeInputAction(
                DisplayRef(values[7], values[8], values[9].toInt(), values[10], values[11]),
                values[1].toInt(), values[2].toInt(), values[3], values[4],
                values[5], values[6], values[12], values[13],
                values[14].toInt(), values[15].toInt()
            )
        } catch (_: IllegalArgumentException) { null }
    }
}

/**
 * Device-side input dispatch is the only terminal claim this receipt makes.
 * A successful Android Binder call DOES NOT prove screen state or task success.
 */
data class NativeInputReceipt(val status: Int, val outcome: Int, val level: Int) {
    val admitted: Boolean get() = status == 0
    val unknown: Boolean get() = admitted && outcome == 4
}

/** Must consult the actual user/host grants for EACH native admission. */
fun interface LiveNativeInputPolicy {
    fun allow(action: NativeInputAction): Boolean
}

/**
 * Transport adapter; implementations here only call the authenticated shell
 * Binder returned by TurboDisplayHandoffHost, never fallback to display 0.
 * The shell itself calls the host's CanonicalHostGrants for a second fence
 * that matches a single native in-flight ticket (not a second action owner).
 */
internal class NativeActionPeer(
    private val shell: ITurboDisplayShell,
    private val policy: LiveNativeInputPolicy,
) {
    @Suppress("unused") // JNI entry: called from the single C11 ActionOwner
    fun grant(values: LongArray): Boolean = try {
        val request = NativeInputAction.fromNative(values) ?: return false
        policy.allow(request)
    } catch (_: Exception) { false }

    @Suppress("unused") // JNI validates the exact return shape and display identity
    fun geometry(values: LongArray): Long = try {
        if (values.size != 17 || values[0] != 1L || values[9] !in 1..Int.MAX_VALUE.toLong()) return 0L
        val b = shell.displayGeometry(values[9].toInt(), values[10], values[11])
        if (b.getInt("status", -1) != 0) return 0L
        val width = b.getInt("width")
        val height = b.getInt("height")
        if (width !in 1..4096 || height !in 1..4096 ||
            width.toLong() * height.toLong() > 8_388_608L) return 0L
        (width.toLong() shl 32) or (height.toLong() and 0xffffffffL)
    } catch (_: Exception) { 0L }

    @Suppress("unused") // JNI dispatch callback, no permission cache
    fun tap(values: LongArray): Int = try {
        if (values.size != 7 || values[0] !in 1..Int.MAX_VALUE.toLong() ||
            values[5] !in 0..Int.MAX_VALUE.toLong() ||
            values[6] !in 0..Int.MAX_VALUE.toLong()) return -2
        shell.tap(values[0].toInt(), values[1], values[2],
            values[3], values[4], values[5].toInt(), values[6].toInt())
    } catch (_: Exception) { -2 }

    @Suppress("unused")
    fun key(values: LongArray): Int = try {
        if (values.size != 6 || values[0] !in 1..Int.MAX_VALUE.toLong() || values[5] != 1L) return -2
        shell.key(values[0].toInt(), values[1], values[2], values[3], values[4], 1)
    } catch (_: Exception) { -2 }
}

/** Declared in the production AAR's NDK shared target, built from root C11. */
internal object NativeActionJni {
    init { System.loadLibrary("turbo_interact_android_jni") }

    external fun open(peer: NativeActionPeer, providerId: Int, capabilities: Int,
                      planGeneration: Long, deviceSessionId: Long, sessionGeneration: Long,
                      displayId: Int, displayGeneration: Long, geometryGeneration: Long,
                      attemptCapacity: Int): Long
    external fun submit(handle: Long, fields: LongArray): IntArray
    external fun confirm(handle: Long, kind: Int, deviceSessionId: Long,
                         sessionGeneration: Long, displayId: Int,
                         displayGeneration: Long, geometryGeneration: Long,
                         actionId: Long, attemptId: Long): Boolean
    external fun updateGeometry(handle: Long, geometryGeneration: Long): Int
    external fun revoke(handle: Long): Int
    external fun destroy(handle: Long): Int
}

/**
 * The single canonical host input owner. The embedding app creates one after
 * an authenticated shell is connected and the display has been created.
 * It must use this SAME object for model and sandbox/script ActionIntent.
 *
 * A pinned call never holds the Kotlin monitor across JNI/Binder, so Binder's
 * reverse authorization callback can borrow the same owner without deadlock.
 */
class NativeAndroidActionOwner private constructor(
    val display: DisplayRef,
    val providerId: Int,
    val planGeneration: Long,
    private val peer: NativeActionPeer,
    nativeHandle: Long,
) : AutoCloseable {
    private val monitor = Any()
    private var handle: Long = nativeHandle
    private var borrowers = 0
    private var closing = false

    private fun pin(allowClosing: Boolean = false): Long = synchronized(monitor) {
        if (handle == 0L || (closing && !allowClosing)) 0L
        else { borrowers++; handle }
    }

    private fun unpin() = synchronized(monitor) {
        borrowers--
        check(borrowers >= 0) { "native owner borrow underflow" }
    }

    fun submit(action: NativeInputAction,
               nowNs: Long = SystemClock.elapsedRealtimeNanos()): NativeInputReceipt {
        val h = pin()
        if (h == 0L) return NativeInputReceipt(-5, 0, 0)
        return try {
            val result = NativeActionJni.submit(h, action.values(nowNs))
            if (result.size != 3) NativeInputReceipt(-1, 0, 0)
            else NativeInputReceipt(result[0], result[1], result[2])
        } finally { unpin() }
    }

    /** Device's reverse Binder admission: exact native ticket, consumed once. */
    fun confirmInFlight(intent: InputIntent): Boolean {
        if (intent.kind != InputIntent.Kind.TAP && intent.kind != InputIntent.Kind.BACK) return false
        val h = pin()
        if (h == 0L) return false
        return try {
            NativeActionJni.confirm(h,
                if (intent.kind == InputIntent.Kind.TAP) NativeInputAction.TAP else NativeInputAction.BACK,
                intent.display.deviceSessionId, intent.display.sessionGeneration,
                intent.display.displayId, intent.display.displayGeneration,
                intent.display.geometryGeneration, intent.actionId, intent.attemptId)
        } finally { unpin() }
    }

    fun updateGeometry(nextGeneration: Long): Boolean {
        val h = pin()
        if (h == 0L) return false
        return try { NativeActionJni.updateGeometry(h, nextGeneration) == 0 }
        finally { unpin() }
    }

    /** May be called even during Binder dispatch; no hardware effect is replayed. */
    fun revoke(): Boolean {
        val h = pin(allowClosing = true)
        if (h == 0L) return false
        return try { NativeActionJni.revoke(h) == 0 }
        finally { unpin() }
    }

    /** False means BUSY: caller must retry after all in-flight callbacks settle. */
    fun tryClose(): Boolean = synchronized(monitor) {
        closing = true
        if (handle == 0L) return true
        if (borrowers != 0) return false
        val status = NativeActionJni.destroy(handle)
        if (status == 0) { handle = 0L; true } else false
    }

    override fun close() {
        if (!tryClose()) throw IllegalStateException("native ActionOwner still in-flight")
    }

    companion object {
        fun create(
            ref: DisplayRef,
            shell: ITurboDisplayShell,
            policy: LiveNativeInputPolicy,
            providerId: Int,
            capabilities: Int,
            planGeneration: Long,
            attemptCapacity: Int = 4096,
        ): NativeAndroidActionOwner {
            require(ref.deviceSessionId > 0 && ref.sessionGeneration > 0 &&
                ref.displayId > 0 && ref.displayGeneration > 0 && ref.geometryGeneration > 0)
            require(providerId > 0 && capabilities > 0 && planGeneration > 0)
            require(attemptCapacity in 1..4096)
            val peer = NativeActionPeer(shell, policy)
            val h = NativeActionJni.open(peer, providerId, capabilities, planGeneration,
                ref.deviceSessionId, ref.sessionGeneration, ref.displayId,
                ref.displayGeneration, ref.geometryGeneration, attemptCapacity)
            check(h != 0L) { "unable to create native C11 ActionOwner" }
            return NativeAndroidActionOwner(ref, providerId, planGeneration, peer, h)
        }
    }
}

/** Separate explicit session lifecycle policy; it is not an input executor. */
interface HostSessionControlPolicy {
    fun allowCreateDisplay(deviceSessionId: Long, sessionGeneration: Long): Boolean
    fun allowLaunch(ref: DisplayRef, packageName: String, actionId: Long): Boolean
}

/**
 * Pass this to TurboDisplayHandoffHost.begin BEFORE the helper is started.
 * bindNativeOwner() occurs after an authenticated shell and owned virtual
 * display exist. Until then input is denied, always; after death, revoked.
 *
 * Display creation and App launch remain explicit host session-control grants
 * because M0 C11 ActionIntent only defines input operations. Neither is
 * implicitly authorized by the native ActionOwner or sandbox.
 */
class CanonicalHostGrants(
    private val deviceSessionId: Long,
    private val sessionGeneration: Long,
    private val controls: HostSessionControlPolicy,
) : HostDisplayGrants {
    @Volatile private var owner: NativeAndroidActionOwner? = null
    @Volatile private var terminated = false

    @Synchronized
    fun bindNativeOwner(value: NativeAndroidActionOwner) {
        check(!terminated && owner == null) { "host session closed or already bound" }
        check(value.display.deviceSessionId == deviceSessionId &&
            value.display.sessionGeneration == sessionGeneration &&
            value.display.displayId > 0) { "ActionOwner belongs to another session" }
        owner = value
    }

    override fun allowCreateDisplay(deviceSessionId: Long, sessionGeneration: Long): Boolean =
        !terminated && deviceSessionId == this.deviceSessionId &&
            sessionGeneration == this.sessionGeneration &&
            runCatching { controls.allowCreateDisplay(deviceSessionId, sessionGeneration) }.getOrDefault(false)

    override fun allowLaunch(ref: DisplayRef, packageName: String, actionId: Long): Boolean =
        !terminated && ref.deviceSessionId == deviceSessionId &&
            ref.sessionGeneration == sessionGeneration && ref.displayId > 0 &&
            ref.displayGeneration > 0 && ref.geometryGeneration > 0 && actionId > 0 &&
            runCatching { controls.allowLaunch(ref, packageName, actionId) }.getOrDefault(false)

    override fun allowInput(intent: InputIntent): Boolean =
        !terminated && owner?.confirmInFlight(intent) == true

    @Synchronized
    override fun onShellTerminated() {
        terminated = true
        owner?.revoke()
        owner = null
    }
}
