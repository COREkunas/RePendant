package org.openpendant.app

import android.os.Looper
import java.io.File
import java.util.concurrent.CancellationException
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock

/**
 * Offline, CPU-only inference. This blocking method belongs on a background
 * executor, never on the main thread. The caller verifies the pinned model's
 * size/SHA-256 before passing its app-private path and owns/scrubs the PCM array.
 * No model is loaded until transcribe is explicitly invoked.
 */
object WhisperEngine {
    const val ENGINE_VERSION = "whisper.cpp-v1.9.4-cpu"
    const val SAMPLE_RATE = 16_000
    const val MAX_SAMPLES = SAMPLE_RATE * 30

    interface Callbacks {
        /** Must be thread-safe and fast: a native compute thread may call it. */
        fun isCancelled(): Boolean
        /** Progress is approximate; dispatch UI updates onto the main thread. */
        fun onProgress(percent: Int)
    }

    private val inferenceLock = ReentrantLock()
    private val libraryLoaded: Unit by lazy { System.loadLibrary("openpendant_whisper") }

    /** Returns UTF-8 JSON: {"segments":[{"startMs":0,"endMs":1000,"text":"…"}]}. */
    fun transcribe(
        modelPath: String,
        pcm16kMono: FloatArray,
        language: String = "lt",
        callbacks: Callbacks
    ): String {
        check(Looper.myLooper() != Looper.getMainLooper()) {
            "Transcription must run on a background thread."
        }
        require(language == "lt") { "This build supports explicit Lithuanian transcription only." }
        require(pcm16kMono.size in 1..MAX_SAMPLES) { "Provide at most 30 seconds of 16 kHz mono audio." }
        require(pcm16kMono.all { it.isFinite() && it in -1.0f..1.0f }) { "PCM must be finite and normalized." }
        val model = File(modelPath)
        require(model.isFile && model.canRead()) { "The local speech model is unavailable." }
        try {
            while (!inferenceLock.tryLock(100, TimeUnit.MILLISECONDS)) {
                if (callbacks.isCancelled()) throw CancellationException("Transcription cancelled.")
            }
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            throw CancellationException("Transcription cancelled.")
        }
        try {
            if (callbacks.isCancelled()) throw CancellationException("Transcription cancelled.")
            libraryLoaded
            return nativeTranscribe(model.absolutePath, pcm16kMono, language, callbacks)
                .toString(Charsets.UTF_8)
        } finally {
            inferenceLock.unlock()
        }
    }

    private external fun nativeTranscribe(
        modelPath: String,
        pcm: FloatArray,
        language: String,
        callbacks: Callbacks
    ): ByteArray
}
