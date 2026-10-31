/*
 * Adapted from Mika (https://github.com/wangzan101/Mika), Copyright 2026 wangzan101.
 * SPDX-License-Identifier: Apache-2.0
 * Changes: independent capture lifetime, owned bitmap snapshots and frame sequence.
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

/**
 * Bounded latest-frame capture. This is a low-rate local observation path,
 * not MediaCodec/StreamGraph, nor a proven action→capture causal barrier.
 * The display must be released before this Surface owner is closed.
 */
class AndroidRgbaCapture(val width: Int, val height: Int, private val minIntervalMs: Long = 100L) {
    data class Frame(val bitmap: Bitmap, val sequence: Long, val capturedAtNs: Long)

    private val monitor = Any()
    private val thread = HandlerThread("turbo-interact-capture").apply { start() }
    private val handler = Handler(thread.looper)
    private val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
    private var closed = false
    private var pending = false
    private var lastProcessedMs = 0L
    private var sequence = 0L
    private var latest: Frame? = null

    val surface: Surface get() = reader.surface

    init {
        require(width > 0 && height > 0 && minIntervalMs > 0)
        reader.setOnImageAvailableListener({ schedule() }, handler)
    }

    private fun schedule() {
        if (closed || pending) return
        pending = true
        val delayMs = (lastProcessedMs + minIntervalMs - SystemClock.uptimeMillis()).coerceAtLeast(0)
        handler.postDelayed({ pending = false; sample() }, delayMs)
    }

    private fun sample() {
        if (closed) return
        val image = reader.acquireLatestImage() ?: return
        try {
            val plane = image.planes[0]
            if (plane.pixelStride < 4) return
            val strideWidth = plane.rowStride / plane.pixelStride
            if (strideWidth < width) return
            val padded = Bitmap.createBitmap(strideWidth, height, Bitmap.Config.ARGB_8888)
            try {
                padded.copyPixelsFromBuffer(plane.buffer.rewind())
                val crop = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                Canvas(crop).drawBitmap(padded, Rect(0, 0, width, height), Rect(0, 0, width, height), null)
                synchronized(monitor) {
                    if (closed) crop.recycle() else {
                        sequence++
                        latest?.bitmap?.recycle()
                        latest = Frame(crop, sequence, image.timestamp)
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

    /** Caller owns the returned bitmap and must recycle it. */
    fun latestFrame(): Frame? = synchronized(monitor) {
        latest?.let { Frame(it.bitmap.copy(Bitmap.Config.ARGB_8888, false), it.sequence, it.capturedAtNs) }
    }

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
