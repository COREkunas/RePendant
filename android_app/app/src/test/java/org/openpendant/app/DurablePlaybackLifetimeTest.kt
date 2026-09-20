package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class DurablePlaybackLifetimeTest {
    @Test fun deniedFocusOrCancelledBeforeRunStillClosesDormantSession() {
        var closes = 0
        val lifetime = DurablePlaybackLifetime({ closes++ }, { fail("No queued cancellation") })
        assertTrue(lifetime.finish()); assertEquals(1, closes)
        lifetime.cancelAsync(); assertEquals(1, closes)
        assertThrows(IllegalStateException::class.java) { lifetime.finish() }
    }
    @Test fun cancellationDispatchIsOnceAndCompletionPrecedesOwnerRelease() {
        var closes = 0; val tasks = mutableListOf<() -> Unit>()
        val lifetime = DurablePlaybackLifetime({ closes++ }, { tasks += it }, { latch ->
            assertEquals(0, closes); assertEquals(1L, latch.count)
            tasks.single()(); true
        })
        lifetime.cancelAsync(); lifetime.cancelAsync()
        assertEquals(1, tasks.size); assertEquals(0, closes)
        assertTrue(lifetime.finish()); assertEquals(2, closes)
        lifetime.cancelAsync(); assertEquals(1, tasks.size)
    }
    @Test fun timeoutRemainsUnconfirmedEvenIfCloseReturnsLater() {
        var closes = 0; var task: (() -> Unit)? = null
        val lifetime = DurablePlaybackLifetime({ closes++ }, { task = it }, { false })
        lifetime.cancelAsync(); assertFalse(lifetime.finish()); assertEquals(0, closes)
        task!!(); assertEquals(1, closes)
        lifetime.cancelAsync(); assertEquals(1, closes)
        assertThrows(IllegalStateException::class.java) { lifetime.finish() }
    }
    @Test fun throwingOutputOrDispatchOrWaitNeverAcknowledgesCleanup() {
        for (mode in 0..3) {
            var closes = 0
            val lifetime = DurablePlaybackLifetime({ closes++; if (mode == 0 || mode == 3) throw AssertionError("Injected close") },
                { if (mode == 1) throw IllegalStateException("Injected dispatch") else it() },
                { if (mode == 2) throw InterruptedException() else true })
            if (mode != 3) lifetime.cancelAsync()
            assertFalse(lifetime.finish())
            assertEquals(if (mode == 1) 0 else 1, closes)
        }
    }
    @Test fun awaitClaimWithoutActualCompletionCannotFreeOwner() {
        val lifetime = DurablePlaybackLifetime({ fail("Not dispatched") }, {}, { true })
        lifetime.cancelAsync(); assertFalse(lifetime.finish())
    }
    @Test fun progressIsThrottledWithoutChangingTransferCompletion() {
        var now = 1000L; val throttle = DurableProgressThrottle { now }
        assertTrue(throttle.due())
        repeat(1000) { assertFalse(throttle.due()) }
        now = 1249; assertFalse(throttle.due())
        now = 1250; assertTrue(throttle.due())
        now = 1200; assertFalse(throttle.due())
        now = 1500; assertTrue(throttle.due())
    }
}
