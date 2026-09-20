package org.openpendant.app

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Foreground job ownership, not audio decoding. A cancellation task may outlive
 * run(), so the process job gate must not be released until finish() proves it
 * returned. A failed/timed-out close is sticky; late return cannot clear it.
 * finish runs on the playback worker, never the UI or a coordinator monitor.
 * The wait is bounded; synchronous platform disposal itself has no hard SLA.
 */
internal class DurablePlaybackLifetime(private val closeOutput: () -> Unit,
    private val dispatch: (() -> Unit) -> Unit,
    private val waitForClose: (CountDownLatch) -> Boolean = { it.await(5, TimeUnit.SECONDS) }) {
    private val lock = Any()
    private val completed = CountDownLatch(1)
    private var requested = false
    private var finished = false
    private var failed = false

    fun cancelAsync() {
        synchronized(lock) {
            if (finished || requested) return
            requested = true
        }
        try {
            dispatch {
                try { closeOutput() }
                catch (_: Throwable) { synchronized(lock) { failed = true } }
                finally { completed.countDown() }
            }
        } catch (_: Throwable) {
            synchronized(lock) { failed = true }
            completed.countDown()
        }
    }

    /** Exactly once, after run has returned/thrown or before it was ever called.
     * False means retain the process owner and fence playback/deletion. */
    fun finish(): Boolean {
        val pending = synchronized(lock) {
            check(!finished)
            finished = true
            requested
        }
        if (pending) {
            val done = try { waitForClose(completed) } catch (_: Throwable) { false }
            if (!done || completed.count != 0L) {
                synchronized(lock) { failed = true }
                return false
            }
        }
        if (synchronized(lock) { failed }) return false
        // Also covers denied focus and cancellation BEFORE run: constructing a
        // session is dormant, but the holder must still be explicitly closed.
        return try { closeOutput(); true }
        catch (_: Throwable) { synchronized(lock) { failed = true }; false }
    }
}

/** UI notification only; it grants no transfer/receipt authority. The final job
 * summary is emitted separately, even when no intermediate update is due. */
internal class DurableProgressThrottle(private val clockMillis: () -> Long = { System.nanoTime() / 1_000_000 }) {
    private var last: Long? = null
    fun due(): Boolean {
        val now = clockMillis()
        val previous = last
        if (previous != null && (now < previous || now - previous < 250)) return false
        last = now
        return true
    }
}
