package org.openpendant.app

import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Per-operation identity: an old notification/service cannot stop a later job. */
internal class BackgroundTransfer(
    val work: DurableLibraryWork,
    val message: () -> String,
    private val cancel: () -> Unit
) {
    val token: String = UUID.randomUUID().toString()
    private val ready = CountDownLatch(1)
    private val stopped = AtomicBoolean()
    private val completed = AtomicBoolean()
    val finished get() = completed.get()
    val stopping get() = stopped.get()
    init { require(TransferActivityPolicy.isTransfer(work)) }
    fun foregroundReady() { ready.countDown() }
    fun awaitForeground(timeoutMillis: Long = 10_000) {
        if (!ready.await(timeoutMillis, TimeUnit.MILLISECONDS)) {
            stop(); throw IllegalStateException("Background transfer did not become ready. Try again with the app open.")
        }
        if (stopped.get() || finished) throw CancellationException("Transfer stopped")
    }
    fun stop() {
        if (!finished && stopped.compareAndSet(false, true)) cancel()
        ready.countDown()
    }
    fun finish() { completed.set(true); ready.countDown() }
}

internal object TransferActivityPolicy {
    fun isTransfer(work: DurableLibraryWork) = work == DurableLibraryWork.SYNC || work == DurableLibraryWork.EXPORT
    fun keepScreenOn(visible: Boolean, work: DurableLibraryWork) = visible && isTransfer(work)
    fun cancelOnBackground(work: DurableLibraryWork) = !isTransfer(work)
}
