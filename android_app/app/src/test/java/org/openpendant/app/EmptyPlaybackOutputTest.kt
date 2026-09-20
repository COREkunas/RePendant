package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.CancellationException

class EmptyPlaybackOutputTest {
    @Test fun closeDoesNotWaitForBlockedEmptySetupAndLateResourceIsDiscarded() {
        val entered=CountDownLatch(1);val finish=CountDownLatch(1)
        val error=AtomicReference<Throwable?>();val disposed=AtomicReference<Any?>();val resource=Any()
        val owner=EmptyPlaybackOutput<Any>{disposed.set(it)}
        val thread=Thread {
            try { owner.start { entered.countDown();check(finish.await(2,TimeUnit.SECONDS));resource } }
            catch(t:Throwable){error.set(t)}
        }
        thread.start()
        try {
            assertTrue(entered.await(1,TimeUnit.SECONDS))
            owner.close() // would deadlock until fixture timeout if setup held monitor
            assertNull(disposed.get())
            assertThrows(IllegalStateException::class.java){owner.useOutput{fail("Closed")}}
        } finally { finish.countDown();thread.join(2500) }
        assertFalse(thread.isAlive);assertTrue(error.get() is CancellationException);assertSame(resource,disposed.get())
    }
    @Test fun activeOutputIsDiscardedExactlyOnceAndFailureIsSticky() {
        var disposals=0
        val owner=EmptyPlaybackOutput<Any>{disposals++;throw IllegalStateException("Synthetic release failure")}
        owner.start{Any()};assertEquals(42,owner.useOutput{42})
        assertThrows(IllegalStateException::class.java){owner.close()}
        assertThrows(RecordingPlaybackException::class.java){owner.close()}
        assertThrows(IllegalStateException::class.java){owner.useOutput{fail("Fenced")}}
        assertEquals(1,disposals)
    }
    @Test fun failedFactoryCannotBeRetriedAndNormalCloseIsIdempotent() {
        val failed=EmptyPlaybackOutput<Any>{fail("Factory owned its partial object")}
        assertThrows(IllegalStateException::class.java){failed.start{throw IllegalStateException("Synthetic setup failure")}}
        assertThrows(IllegalStateException::class.java){failed.start{Any()}};failed.close()
        var disposals=0;val good=EmptyPlaybackOutput<Any>{disposals++}
        good.start{Any()};good.close();good.close();assertEquals(1,disposals)
    }
    @Test fun concurrentCloseCannotAcknowledgeStillPendingActiveDisposal() {
        val entered=CountDownLatch(1);val finish=CountDownLatch(1);val error=AtomicReference<Throwable?>()
        val owner=EmptyPlaybackOutput<Any>{entered.countDown();check(finish.await(2,TimeUnit.SECONDS))}
        owner.start{Any()}
        val thread=Thread{try{owner.close()}catch(t:Throwable){error.set(t)}}
        thread.start()
        try {
            assertTrue(entered.await(1,TimeUnit.SECONDS))
            assertThrows(RecordingPlaybackException::class.java){owner.close()}
        } finally { finish.countDown();thread.join(2500) }
        assertFalse(thread.isAlive);assertNull(error.get());owner.close()
    }
}
