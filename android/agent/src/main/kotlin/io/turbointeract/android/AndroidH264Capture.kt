/*
 * MediaCodec InputSurface model adapted from scrcpy:
 * https://github.com/Genymobile/scrcpy/blob/a60891aea193d92e7e5c3942700eca63f9d19a5f/server/src/main/java/com/genymobile/scrcpy/video/SurfaceEncoder.java
 * Copyright (C) 2018 Genymobile, Copyright (C) 2018-2024 Romain Vimont
 * SPDX-License-Identifier: Apache-2.0
 *
 * TurboInteract adds an independent, bounded encoded packet owner. This code
 * does not include the scrcpy wire protocol or claim action/frame causality.
 */
package io.turbointeract.android

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.view.Surface
import java.util.concurrent.ArrayBlockingQueue

/**
 * One hardware/software encoder instance, one Surface and bounded owned packets.
 * Present the returned Surface to a VirtualDisplay, or use
 * AndroidVirtualDisplayOwner.replaceSurface() to switch from RGBA capture.
 *
 * The PC host may route encoded packets into TurboMedia. This is a device-side
 * encoder, not a second PC-side media graph/scheduler.
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
        val bytes: ByteArray,
        val presentationTimeUs: Long,
        val codecConfig: Boolean,
        val keyFrame: Boolean,
        val endOfStream: Boolean,
    )

    enum class State { NEW, RUNNING, FAILED, CLOSED }

    init {
        require(width in 1..4096 && height in 1..4096 && width.toLong() * height <= 8_388_608L)
        require(bitrate in 64_000..30_000_000 && fps in 1..120)
        require(packetCapacity in 1..128 && maxPacketBytes in 1024..8_388_608)
    }

    private val queue = ArrayBlockingQueue<EncodedPacket>(packetCapacity)
    @Volatile private var state = State.NEW
    @Volatile private var running = false
    @Volatile private var failure: String? = null
    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    private var worker: Thread? = null

    @Synchronized
    fun start(): Surface {
        check(state == State.NEW) { "encoder already started/closed" }
        val encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        var ownedSurface: Surface? = null
        try {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            }
            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            ownedSurface = encoder.createInputSurface()
            encoder.start()
            codec = encoder
            surface = ownedSurface
            running = true
            state = State.RUNNING
            worker = Thread({ drain(encoder) }, "turbo-interact-h264").apply { isDaemon = true; start() }
            return ownedSurface
        } catch (e: Exception) {
            running = false
            state = State.FAILED
            runCatching { ownedSurface?.release() }
            runCatching { encoder.release() }
            failure = e.javaClass.simpleName
            throw e
        }
    }

    /** Never yields a borrowed codec buffer; the byte array is an owned copy. */
    fun pollPacket(): EncodedPacket? = queue.poll()

    fun currentState(): State = state
    fun failureCategory(): String? = failure

    private fun drain(encoder: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (running) {
                when (val index = encoder.dequeueOutputBuffer(info, 10_000L)) {
                    MediaCodec.INFO_TRY_AGAIN_LATER, MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> Unit
                    else -> if (index >= 0) {
                        try {
                            if (info.size > maxPacketBytes) {
                                fail("packet_capacity")
                                return
                            }
                            if (info.size > 0) {
                                val buffer = encoder.getOutputBuffer(index) ?: error("no output buffer")
                                buffer.position(info.offset)
                                buffer.limit(info.offset + info.size)
                                val bytes = ByteArray(info.size)
                                buffer.get(bytes)
                                val packet = EncodedPacket(
                                    bytes, info.presentationTimeUs,
                                    info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0,
                                    info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0,
                                    info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                                )
                                if (!queue.offer(packet)) {
                                    fail("output_queue_full")
                                    return // Never silently drop an accepted encoded packet.
                                }
                            }
                            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                                running = false
                            }
                        } finally {
                            encoder.releaseOutputBuffer(index, false)
                        }
                    }
                }
            }
        } catch (e: Exception) {
            if (running) fail(e.javaClass.simpleName)
        }
    }

    private fun fail(reason: String) {
        failure = reason
        state = State.FAILED
        running = false
    }

    /**
     * Caller must release/detach the VirtualDisplay before closing the Surface.
     * Returns false (BUSY) if the drain thread has not terminated; no premature
     * MediaCodec or Surface release while another thread still borrows them.
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
            // Failed encoders may already be stopped.
        } finally {
            codec?.release()
            surface?.release()
            codec = null
            surface = null
            queue.clear()
            state = State.CLOSED
        }
        return true
    }
}
