package org.openpendant.app

import java.util.concurrent.CancellationException

/** Owns EMPTY output setup independently of the coordinator and output monitor.
 * A concurrent close sets its tombstone immediately: late setup/activation is
 * disposed without ever admitting a PCM operation. Disposal and platform setup
 * are not claimed to have a hard scheduling bound. Callbacks never reenter. */
internal class EmptyPlaybackOutput<T : Any>(private val dispose: (T) -> Unit) : AutoCloseable {
    private val lock = Any()
    private var output: T? = null
    private var attempted = false
    private var closed = false
    private var disposalFailed = false
    private var disposingActive = false

    fun start(createAndActivateEmpty: () -> T) {
        synchronized(lock) { check(!closed && !attempted); attempted = true }
        // The factory owns partial resources on failure and must dispose them.
        val made = createAndActivateEmpty()
        val accepted = synchronized(lock) {
            if (closed || disposalFailed) false else { output = made; true }
        }
        if (!accepted) {
            discard(made)
            throw CancellationException("Playback output closed during setup")
        }
    }
    fun <R> useOutput(action: (T) -> R): R = synchronized(lock) {
        check(!closed && !disposalFailed)
        action(output ?: error("Audio output is not started"))
    }
    fun markDisposalFailure() = synchronized(lock) { disposalFailed = true }
    private fun discard(value: T) {
        try { dispose(value) } catch (failure: Throwable) { markDisposalFailure(); throw failure }
    }
    override fun close() {
        val old = synchronized(lock) {
            closed = true
            // A second close cannot falsely acknowledge another thread's still
            // pending platform teardown. Fail closed instead of waiting/joining.
            if (disposalFailed || disposingActive) throw RecordingPlaybackException()
            output.also { output = null; disposingActive = it != null }
        }
        if (old != null) try { discard(old) } finally { synchronized(lock) { disposingActive = false } }
    }
}
