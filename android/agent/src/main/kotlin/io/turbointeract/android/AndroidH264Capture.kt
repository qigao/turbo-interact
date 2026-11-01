/*
 * MediaCodec InputSurface model adapted from scrcpy:
 * https://github.com/Genymobile/scrcpy/blob/a60891aea193d92e7e5c3942700eca63f9d19a5f/server/src/main/java/com/genymobile/scrcpy/video/SurfaceEncoder.java
 * Copyright (C) 2018 Genymobile, Copyright (C) 2018-2024 Romain Vimont
 * SPDX-License-Identifier: Apache-2.0
 *
 * The AAR owns bounded copies of H264 access units, never FrameStore pixels.
 * The SDK does not include a scrcpy transport or a second Codec/Graph engine.
 */
package io.turbointeract.android

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.SystemClock
import android.view.Surface
import java.util.ArrayDeque
import java.util.UUID
import java.util.concurrent.ArrayBlockingQueue

/**
 * A one-shot MediaCodec H264 Source for a caller-owned VirtualDisplay.
 * Construct/start() obtains an InputSurface BEFORE createDisplay; after the
 * authenticated Shell returns a live display identity, bindDisplay() binds
 * it ONCE. No pixel frame may be published before this exact binding.
 *
 * Only actual Annex-B encoded buffers are admitted. Baseline/no-B-frame
 * encoding is REQUESTED, not assumed: the remote TurboMedia H264 Provider
 * independently validates SPS/IDR and timestamp semantics. MediaCodec
 * exposes PTS but does not prove DTS, composition order or task success.
 */
class AndroidH264Capture(
    val width: Int,
    val height: Int,
    private val bitrate: Int = 4_000_000,
    private val fps: Int = 30,
    packetCapacity: Int = 32,
    private val maxPacketBytes: Int = 2 * 1024 * 1024,
) {
    data class EncodedPacket(
        val bytes: ByteArray, // OWNED copy; no borrowed MediaCodec ByteBuffer
        val presentationTimeUs: Long,
        val codecConfig: Boolean,
        val keyFrame: Boolean,
        val endOfStream: Boolean,
        /** Always present for packets returned by pollPacket(). */
        val stamp: H264CaptureStamp? = null,
        /** This v1 Source publishes Annex-B only; not AVCC or RTP fragments. */
        val framing: Int = FRAMING_ANNEX_B,
    )

    enum class State { NEW, RUNNING, ENDED, FAILED, CLOSED }

    init {
        require(width in 16..4096 && height in 16..4096 &&
            width % 2 == 0 && height % 2 == 0 &&
            width.toLong() * height <= 8_388_608L)
        require(bitrate in 64_000..30_000_000 && fps in 1..120)
        require(packetCapacity in 1..128 && maxPacketBytes in 1024..2_097_152)
    }

    private data class Pending(
        val bytes: ByteArray,
        val ptsUs: Long,
        val config: Boolean,
        val key: Boolean,
        val eos: Boolean,
    )

    private val monitor = Any()
    private val queue = ArrayBlockingQueue<EncodedPacket>(packetCapacity)
    private val preBindConfig = ArrayDeque<Pending>()
    private val captureSession = UUID.randomUUID()
    private val streamGeneration = 1L // New source object is required on reset/rotation.
    private var display: DisplayRef? = null
    private var nextSequence = 1L
    @Volatile private var running = false
    @Volatile private var state = State.NEW
    @Volatile private var failure: String? = null
    private var finishRequested = false
    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    private var worker: Thread? = null

    @Synchronized
    fun start(): Surface {
        check(state == State.NEW) { "H264 encoder already started/closed" }
        val encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        var ownedSurface: Surface? = null
        try {
            val format = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
                // A request, not proof; actual SPS is checked downstream.
                setInteger(MediaFormat.KEY_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
                setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }
            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            ownedSurface = encoder.createInputSurface()
            encoder.start()
            codec = encoder
            surface = ownedSurface
            running = true
            state = State.RUNNING
            worker = Thread({ drain(encoder) }, "turbo-interact-h264").apply {
                isDaemon = true
                start()
            }
            return ownedSurface
        } catch (e: Exception) {
            fail("encoder_start_${e.javaClass.simpleName}")
            runCatching { ownedSurface?.release() }
            runCatching { encoder.release() }
            throw e
        }
    }

    /**
     * The caller passes only the actual authenticated Shell createDisplay()
     * reply, never model-supplied display IDs or a main-display fallback.
     * Codec configuration may have arrived before this binding; it is held
     * in a finite local list and stamped when the display is known.
     */
    fun bindDisplay(snapshot: DisplaySnapshot) {
        require(snapshot.geometry.width == width && snapshot.geometry.height == height)
        val ref = snapshot.ref
        require(ref.deviceSessionId > 0 && ref.sessionGeneration > 0 &&
            ref.displayId > 0 && ref.displayGeneration > 0 &&
            ref.geometryGeneration > 0)
        synchronized(monitor) {
            check(state == State.RUNNING && display == null) {
                "H264 capture closed, failed or already bound"
            }
            if (preBindConfig.size > queue.remainingCapacity()) {
                fail("config_queue_full")
                error("bounded H264 config queue is full")
            }
            display = ref
            while (preBindConfig.isNotEmpty()) {
                check(emitBound(preBindConfig.removeFirst()))
            }
        }
    }

    /** The caller sees no packets before binding or after a failed session. */
    fun pollPacket(): EncodedPacket? = synchronized(monitor) {
        if (display == null || state == State.FAILED || state == State.CLOSED)
            null else queue.poll()
    }

    /** Explicit graceful EOS, only after the VirtualDisplay stops rendering. */
    @Synchronized
    fun signalEndOfInput(): Boolean {
        if (state != State.RUNNING || finishRequested) return false
        finishRequested = true
        return try {
            codec?.signalEndOfInputStream()
            true
        } catch (_: Exception) {
            fail("signal_eos_failed")
            false
        }
    }

    fun currentState(): State = state
    fun failureCategory(): String? = failure

    private fun fail(reason: String) = synchronized(monitor) {
        failure = reason
        state = State.FAILED
        running = false
        queue.clear()
        preBindConfig.clear()
    }

    private fun emitBound(pending: Pending): Boolean {
        val ref = display ?: return false
        if (nextSequence == Long.MAX_VALUE) { fail("sequence_exhausted"); return false }
        val stamp = H264CaptureStamp(ref, captureSession.mostSignificantBits,
            captureSession.leastSignificantBits, streamGeneration, nextSequence++,
            pending.ptsUs, SystemClock.elapsedRealtimeNanos())
        return queue.offer(EncodedPacket(pending.bytes, pending.ptsUs,
            pending.config, pending.key, pending.eos, stamp))
    }

    /**
     * Preserve a nonempty final access unit AND its distinct empty EOS row;
     * the upstream H264 Source requires EOS to be its own terminal packet.
     * Reserve bounded capacity for all resulting rows before publishing any.
     */
    private fun emit(bytes: ByteArray, ptsUs: Long, config: Boolean,
                     key: Boolean, eos: Boolean): Boolean = synchronized(monitor) {
        if (state != State.RUNNING || ptsUs < 0 || (config && key) ||
            (config && eos) || (bytes.isEmpty() && !eos)) {
            fail("invalid_encoder_output")
            return@synchronized false
        }
        if (bytes.isNotEmpty() && !H264CaptureLineageGate.hasAnnexBPrefix(bytes)) {
            fail("non_annexb")
            return@synchronized false
        }
        val pending = if (eos && bytes.isNotEmpty())
            listOf(Pending(bytes, ptsUs, false, key, false),
                Pending(ByteArray(0), ptsUs, false, false, true))
        else listOf(Pending(bytes, ptsUs, config, key, eos))
        if (display == null) {
            if (pending.any { !it.config } || preBindConfig.size + pending.size > 4) {
                fail("unbound_display_output")
                return@synchronized false
            }
            for (packet in pending) preBindConfig.addLast(packet)
            return@synchronized true
        }
        if (queue.remainingCapacity() < pending.size) {
            fail("output_queue_full")
            return@synchronized false
        }
        for (packet in pending) if (!emitBound(packet)) {
            fail("output_queue_full")
            return@synchronized false
        }
        if (eos) {
            state = State.ENDED
            running = false
        }
        true
    }

    /** Some Android encoders provide SPS/PPS through output-format csd. */
    private fun publishOutputFormat(format: MediaFormat): Boolean {
        for (field in listOf("csd-0", "csd-1")) {
            val csd = format.getByteBuffer(field) ?: continue
            val copy = csd.duplicate()
            val bytes = ByteArray(copy.remaining())
            if (bytes.isEmpty() || bytes.size > maxPacketBytes) {
                fail("invalid_codec_config")
                return false
            }
            copy.get(bytes)
            if (!emit(bytes, 0L, true, false, false)) return false
        }
        return true
    }

    private fun drain(encoder: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        var sawOutputFormat = false
        try {
            while (running) {
                when (val index = encoder.dequeueOutputBuffer(info, 10_000L)) {
                    MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        if (sawOutputFormat) {
                            fail("format_change_requires_new_stream")
                            return
                        }
                        sawOutputFormat = true
                        if (!publishOutputFormat(encoder.outputFormat)) return
                    }
                    else -> if (index >= 0) {
                        try {
                            if (info.size < 0 || info.size > maxPacketBytes) {
                                fail("packet_capacity")
                                return
                            }
                            val buffer = encoder.getOutputBuffer(index)
                            val data = if (info.size > 0) {
                                if (buffer == null) {
                                    fail("missing_output_buffer")
                                    return
                                }
                                buffer.position(info.offset)
                                buffer.limit(info.offset + info.size)
                                ByteArray(info.size).also { buffer.get(it) }
                            } else ByteArray(0)
                            val config = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                            val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                            val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                            if (data.isNotEmpty() || eos) {
                                if (!emit(data, info.presentationTimeUs, config, key, eos)) return
                            }
                        } finally {
                            encoder.releaseOutputBuffer(index, false)
                        }
                    }
                }
            }
        } catch (e: Exception) {
            if (running) fail("codec_drain_${e.javaClass.simpleName}")
        }
    }

    /**
     * Release VirtualDisplay first; only then stop/close this Surface owner.
     * Returns BUSY if the original codec worker has not joined. No native
     * buffer is returned after close, even if stale packets remained queued.
     */
    @Synchronized
    fun close(): Boolean {
        if (state == State.CLOSED) return true
        if (state == State.NEW) { state = State.CLOSED; return true }
        running = false
        val thread = worker
        if (thread != null && thread !== Thread.currentThread()) {
            thread.join(2_000L)
            if (thread.isAlive) return false
        }
        try {
            codec?.stop()
        } catch (_: IllegalStateException) {
            // A failed encoder might already be stopped.
        } finally {
            codec?.release()
            surface?.release()
            codec = null
            surface = null
            synchronized(monitor) {
                queue.clear()
                preBindConfig.clear()
                display = null
                state = State.CLOSED
            }
        }
        return true
    }

    companion object { const val FRAMING_ANNEX_B = 1 }
}
