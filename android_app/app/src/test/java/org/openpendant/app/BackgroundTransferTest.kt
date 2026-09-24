package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CancellationException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

class BackgroundTransferTest {
    @Test fun onlyTransfersSurviveBackgroundAndKeepVisibleScreenAwake() {
        for (work in DurableLibraryWork.entries) {
            val transfer = work == DurableLibraryWork.SYNC || work == DurableLibraryWork.EXPORT
            assertEquals(transfer, TransferActivityPolicy.isTransfer(work))
            assertEquals(!transfer, TransferActivityPolicy.cancelOnBackground(work))
            assertEquals(transfer, TransferActivityPolicy.keepScreenOn(true, work))
            assertFalse(TransferActivityPolicy.keepScreenOn(false, work))
        }
    }
    @Test fun backgroundLeaseCannotProtectOtherWork() {
        for (work in listOf(DurableLibraryWork.NONE, DurableLibraryWork.PLAY, DurableLibraryWork.DELETE, DurableLibraryWork.REFRESH)) {
            assertThrows(IllegalArgumentException::class.java) { BackgroundTransfer(work, { "test" }, {}) }
        }
    }
    @Test fun workerMustWaitUntilForegroundServiceIsReady() {
        val own = BackgroundTransfer(DurableLibraryWork.EXPORT, { "Sending to PC · 10%" }, {})
        val executor = Executors.newSingleThreadExecutor()
        try {
            val task = executor.submit { own.awaitForeground(2_000) }
            assertFalse(task.isDone)
            own.foregroundReady(); task.get(2, TimeUnit.SECONDS)
            assertEquals("Sending to PC · 10%", own.message())
        } finally { own.finish(); executor.shutdownNow() }
    }
    @Test fun deniedOrLateServiceNeverStartsUnprotectedWork() {
        val cancelled = AtomicInteger()
        val own = BackgroundTransfer(DurableLibraryWork.SYNC, { "test" }) { cancelled.incrementAndGet() }
        assertThrows(IllegalStateException::class.java) { own.awaitForeground(1) }
        own.foregroundReady()
        assertThrows(CancellationException::class.java) { own.awaitForeground(1) }
        assertEquals(1, cancelled.get())
    }
    @Test fun stoppingBeforeStartUnblocksWorkerAndCancelsOnlyOnce() {
        val cancelled = AtomicInteger()
        val own = BackgroundTransfer(DurableLibraryWork.EXPORT, { "test" }) { cancelled.incrementAndGet() }
        own.stop(); own.stop()
        assertThrows(CancellationException::class.java) { own.awaitForeground(1) }
        assertTrue(own.stopping); assertFalse(own.finished); assertEquals(1, cancelled.get())
        own.finish(); assertTrue(own.finished)
    }
    @Test fun completionAndOldStopsCannotCancelNextJob() {
        val firstCancelled = AtomicInteger(); val nextCancelled = AtomicInteger()
        val first = BackgroundTransfer(DurableLibraryWork.SYNC, { "test" }) { firstCancelled.incrementAndGet() }
        val next = BackgroundTransfer(DurableLibraryWork.EXPORT, { "test" }) { nextCancelled.incrementAndGet() }
        assertNotEquals(first.token, next.token)
        first.foregroundReady(); first.awaitForeground(1); first.finish(); first.stop()
        next.foregroundReady(); next.awaitForeground(1)
        assertEquals(0, firstCancelled.get()); assertEquals(0, nextCancelled.get())
        next.stop(); assertEquals(1, nextCancelled.get()); next.finish()
    }
    @Test fun completionBeforeServiceArrivesDoesNotRunWorkerOrCancelAnotherOperation() {
        val cancelled = AtomicInteger()
        val own = BackgroundTransfer(DurableLibraryWork.EXPORT, { "test" }) { cancelled.incrementAndGet() }
        own.finish(); own.foregroundReady(); own.stop()
        assertThrows(CancellationException::class.java) { own.awaitForeground(1) }
        assertEquals(0, cancelled.get())
    }
}
