/*
 * Adapted from Mika (https://github.com/wangzan101/Mika), Copyright 2026 wangzan101.
 * SPDX-License-Identifier: Apache-2.0
 * Changes: capture lifetime, owned bitmap snapshots and explicit frame sequence.
 */
package io.turbointeract.android

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.PixelFormat
import android.graphics.Rect
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.view.Surface
import java.util.UUID

/**
 * Low-frequency ImageReader capture attached to a separate owned Display.
 * The frame sequence is local to this capture, NOT a proof of Android
 * input→display composition order. A replacement capture starts a new session.
 */
class AndroidRgbaCapture(val width: Int, val height: Int, private val minIntervalMs: Long = 100L) {
    init {
        require(width in 1..4096 && height in 1..4096 && minIntervalMs > 0)
        require(width.toLong() * height.toLong() <= 8_388_608L) { "capture budget exceeded" }
    }

    data class Frame(
        val bitmap: Bitmap,
        val sequence: Long,
        val capturedAtNs: Long,
        /** Non-authoritative lineage; null until host binds the created display. */
        val stamp: CaptureFrameStamp? = null,
    )

    private val monitor = Any()
    private val thread = HandlerThread("turbo-interact-capture").apply { start() }
    private val handler = Handler(thread.looper)
    private val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
    private val captureSessionId = UUID.randomUUID().toString()
    private var boundDisplay: DisplayRef? = null
    @Volatile private var closed = false
    private var pending = false
    private var lastProcessedMs = 0L
    private var sequence = 0L
    private var latest: Frame? = null

    val surface: Surface get() = reader.surface

    /**
     * The Surface is supplied before createDisplay; bind its exact returned
     * DisplaySnapshot AFTER successful creation and BEFORE collecting action
     * evidence. A rotation, Surface swap or display recreation needs a NEW
     * capture session rather than silently reusing the old lineage.
     */
    fun bindDisplay(snapshot: DisplaySnapshot) {
        require(snapshot.geometry.width == width && snapshot.geometry.height == height)
        val ref = snapshot.ref
        require(ref.deviceSessionId > 0 && ref.sessionGeneration > 0 &&
            ref.displayId > 0 && ref.displayGeneration > 0 && ref.geometryGeneration > 0)
        synchronized(monitor) {
            check(!closed && boundDisplay == null) { "capture already bound or closed" }
            boundDisplay = ref
            latest?.bitmap?.recycle()
            latest = null
            sequence = 0L
        }
    }

    init {
        reader.setOnImageAvailableListener({ schedule() }, handler)
    }

    private fun schedule() {
        if (closed || pending) return
        pending = true
        val waitMs = (lastProcessedMs + minIntervalMs - SystemClock.uptimeMillis()).coerceAtLeast(0)
        handler.postDelayed({ pending = false; sample() }, waitMs)
    }

    private fun sample() {
        if (closed) return
        val image = try { reader.acquireLatestImage() } catch (_: IllegalStateException) { null } ?: return
        try {
            val plane = image.planes[0]
            if (plane.pixelStride != 4) return
            val strideWidth = plane.rowStride / plane.pixelStride
            if (strideWidth < width) return
            val padded = Bitmap.createBitmap(strideWidth, height, Bitmap.Config.ARGB_8888)
            try {
                padded.copyPixelsFromBuffer(plane.buffer.rewind())
                val cropped = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                Canvas(cropped).drawBitmap(padded, Rect(0, 0, width, height), Rect(0, 0, width, height), null)
                synchronized(monitor) {
                    if (closed) cropped.recycle() else {
                        if (sequence == Long.MAX_VALUE) error("capture sequence exhausted")
                        sequence++
                        latest?.bitmap?.recycle()
                        latest = Frame(cropped, sequence, image.timestamp,
                            boundDisplay?.let { ref ->
                                CaptureFrameStamp(ref, captureSessionId, sequence,
                                    SystemClock.elapsedRealtimeNanos())
                            })
                    }
                }
            } finally {
                padded.recycle()
            }
            lastProcessedMs = SystemClock.uptimeMillis()
        } finally {
            image.close()
        }
    }

    /** Caller owns the returned bitmap and must recycle it. Returns null after close. */
    fun latestFrame(): Frame? = synchronized(monitor) {
        if (closed) null else latest?.let {
            Frame(it.bitmap.copy(Bitmap.Config.ARGB_8888, false), it.sequence, it.capturedAtNs, it.stamp)
        }
    }

    /** Release the VirtualDisplay first, then the Surface owner. */
    fun close() {
        synchronized(monitor) {
            if (closed) return
            closed = true
        }
        handler.removeCallbacksAndMessages(null)
        handler.post {
            reader.close()
            synchronized(monitor) { latest?.bitmap?.recycle(); latest = null }
        }
        thread.quitSafely()
    }
}
