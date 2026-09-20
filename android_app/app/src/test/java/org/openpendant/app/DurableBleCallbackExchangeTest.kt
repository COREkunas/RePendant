package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

/** Callback scheduling/abandonment tests; actual Bluetooth objects are not
 * instantiated on JVM. Radio helper tests separately exercise both OP events. */
class DurableBleCallbackExchangeTest {
    private val connection = DurableSyncConnection(UUID(1, 2), RecordingVolume(UUID(3, 4), UUID(5, 6), 7), "12".repeat(32))
    private val request = DurableBleCodec.catalog(UUID(7, 8), 0)
    private inner class Channel : DurableBleCallbackChannel {
        val ownership = DurableTransportOwnership()
        var lease: DurableTransportLease? = null
        var now = 1000L
        var current = true
        var onMain = false
        var closed = 0
        var sent = 0
        var throwClose = false
        var rejectPosts = false
        var defer = false
        var afterPost: () -> Unit = {}
        val queue = mutableListOf<() -> Unit>()
        var savedSuccess: ((ByteArray) -> Unit)? = null
        var savedFailure: (() -> Unit)? = null
        var action: ((ByteArray) -> Unit, () -> Unit) -> Unit = { success, _ -> success(byteArrayOf(1, 2, 3)) }
        override fun capabilities(epoch: UUID) = DurableSyncCapabilities(true, true, true, true)
        override fun acquire(epoch: UUID) = ownership.acquire(epoch).also { lease = it }
        override fun isCurrent(epoch: UUID) = current && epoch == connection.epoch
        override fun onCallbackThread() = onMain
        override fun post(action: () -> Unit): Boolean {
            if (rejectPosts) return false
            if (defer) queue += action else action()
            afterPost(); return true
        }
        override fun submit(lease: DurableTransportLease, call: DurableSyncCall, request: DurableBleRequest,
            success: (ByteArray) -> Unit, failure: () -> Unit) {
            assertSame(this.lease, lease); call.checkActive(); sent++
            savedSuccess = success; savedFailure = failure
            action(success, failure)
        }
        override fun close(lease: DurableTransportLease) {
            if (this.lease !== lease) return
            closed++; current = false
            lease.revokeForTransportClose()
            if (throwClose) error("Injected local-close failure")
            lease.cancelAfterTransportClosed()
        }
        fun drain() { while (queue.isNotEmpty()) queue.removeAt(0)() }
        fun call(lease: DurableTransportLease, check: () -> Unit = {}) =
            DurableSyncCall(connection, 31000, lease.beginRequest()) { check(); check(current); check(now < 31000) }
    }
    @Test fun successReturnsOwnedCopyButDoesNotCompleteLogicalTicket() {
        val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
        val lease = bridge.acquire(connection.epoch); val call = channel.call(lease)
        val borrowed = byteArrayOf(1, 2, 3)
        channel.action = { success, _ -> success(borrowed); borrowed.fill(0) }
        val received = bridge.exchange(lease, call, request)
        assertArrayEquals(byteArrayOf(1, 2, 3), received); received.fill(0)
        call.checkActive(); assertTrue(call.transportCompleted()); call.validateReply()
        assertTrue(lease.retire()); assertEquals(0, channel.closed)
    }
    @Test fun queuedFrameTimeoutSkipsSubmissionAndClosesOnlyAfterMainDrains() {
        val channel = Channel(); channel.defer = true
        val bridge = DurableBleCallbackExchange(channel) { channel.now }
        val lease = bridge.acquire(connection.epoch); val call = channel.call(lease)
        channel.afterPost = { channel.now = 5000 }
        assertThrows(DurableSyncException::class.java) { bridge.exchange(lease, call, request) }
        assertEquals(0, channel.sent); assertFalse(lease.retire())
        assertThrows(DurableTransportBusyException::class.java) { channel.ownership.acquire(UUID(9, 9)) }
        channel.drain(); assertEquals(0, channel.sent); assertTrue(channel.ownership.isIdle())
    }
    @Test fun originalLogicalDeadlineCannotBeRenewedByPosting() {
        val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
        val lease = bridge.acquire(connection.epoch)
        val call = DurableSyncCall(connection, 1001, lease.beginRequest()) {}
        channel.afterPost = { channel.now = 1001 }
        assertThrows(DurableSyncException::class.java) { bridge.exchange(lease, call, request) }
        assertEquals(1, channel.sent); assertEquals(1, channel.closed)
        assertFalse(call.transportCompleted())
    }
    @Test fun staleEpochAndBackwardClockAfterSuccessCannotReturnBody() {
        for (backward in listOf(false, true)) {
            val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
            val lease = bridge.acquire(connection.epoch); val call = channel.call(lease)
            channel.afterPost = { if (backward) channel.now = 999 else channel.current = false }
            assertThrows(DurableSyncException::class.java) { bridge.exchange(lease, call, request) }
            assertFalse(call.transportCompleted()); assertEquals(1, channel.closed)
        }
    }
    @Test fun delayedSuccessAfterCancellationCannotReleaseNewOwner() {
        val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
        val old = bridge.acquire(connection.epoch); val call = channel.call(old)
        channel.action = { _, _ -> channel.now = 5000 }
        assertThrows(DurableSyncException::class.java) { bridge.exchange(old, call, request) }
        val newer = channel.ownership.acquire(UUID(9, 10)); channel.lease = newer
        channel.savedSuccess!!(byteArrayOf(9, 8, 7)); channel.savedFailure!!()
        assertTrue(newer.isActive()); assertFalse(call.transportCompleted()); assertTrue(newer.retire())
    }
    @Test fun throwingCloseOrRejectedPostLeavesPendingGateFenced() {
        for (reject in listOf(false, true)) {
            val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
            val lease = bridge.acquire(connection.epoch); val call = channel.call(lease)
            channel.throwClose = !reject; channel.rejectPosts = reject
            channel.action = { _, _ -> channel.now = 5000 }
            assertThrows(DurableSyncException::class.java) { bridge.exchange(lease, call, request) }
            assertFalse(lease.retire())
            assertThrows(DurableTransportBusyException::class.java) { channel.ownership.acquire(UUID(9, 9)) }
        }
    }
    @Test fun oversizedBodyAndSynchronousSubmissionFailureAreNeverDelivered() {
        for (mode in 0..2) {
            val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
            val lease = bridge.acquire(connection.epoch); val call = channel.call(lease)
            channel.action = { success, failure -> when (mode) { 0 -> success(ByteArray(73)); 1 -> throw AssertionError("Injected submit failure"); else -> failure() } }
            assertThrows(DurableSyncException::class.java) { bridge.exchange(lease, call, request) }
            assertFalse(call.transportCompleted()); assertTrue(channel.closed >= 1)
        }
    }
    @Test fun wrongLeaseAndMainThreadFailBeforePostingWithoutClosingOwner() {
        val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
        val lease = bridge.acquire(connection.epoch); val call = channel.call(lease)
        val other = DurableTransportOwnership().acquire(connection.epoch)
        assertThrows(IllegalArgumentException::class.java) { bridge.exchange(other, call, request) }
        channel.onMain = true
        assertThrows(IllegalStateException::class.java) { bridge.exchange(lease, call, request) }
        assertEquals(0, channel.sent); assertEquals(0, channel.closed)
        assertTrue(call.transportCancelled()); assertTrue(lease.retire()); other.retire()
    }
    @Test fun batchReceiptAllowsSlowStorageButNeverRenewsLogicalDeadline() {
        val receipt=DurableBleRequest(DurableBleCodec.FULL_RECEIVE_RANGE,ByteArray(72))
        for(elapsed in listOf(5000L,29999L,30000L)) {
            val channel=Channel();val bridge=DurableBleCallbackExchange(channel){channel.now}
            val lease=bridge.acquire(connection.epoch);val call=channel.call(lease)
            channel.action={success,_->channel.now+=elapsed;success(byteArrayOf(1))}
            if(elapsed<30000) {
                assertArrayEquals(byteArrayOf(1),bridge.exchange(lease,call,receipt))
                assertTrue(call.transportCompleted());call.validateReply();assertTrue(lease.retire())
            } else {
                assertThrows(DurableSyncException::class.java){bridge.exchange(lease,call,receipt)}
                assertFalse(call.transportCompleted());assertTrue(channel.closed>=1)
            }
        }
        val channel=Channel();val bridge=DurableBleCallbackExchange(channel){channel.now}
        val lease=bridge.acquire(connection.epoch)
        val call=DurableSyncCall(connection,2000,lease.beginRequest()){}
        channel.action={success,_->channel.now=2000;success(byteArrayOf(1))}
        assertThrows(DurableSyncException::class.java){bridge.exchange(lease,call,receipt)}
        assertFalse(call.transportCompleted());assertTrue(channel.ownership.isIdle())
    }
    @Test fun cancellationPredicateAfterSubmissionClosesAndCannotProduceReceipt() {
        val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
        val lease = bridge.acquire(connection.epoch); var cancelled = false
        val call = channel.call(lease) { check(!cancelled) }
        channel.action = { success, _ -> cancelled = true; success(byteArrayOf(1)) }
        assertThrows(DurableSyncException::class.java) { bridge.exchange(lease, call, request) }
        assertFalse(call.transportCompleted()); assertTrue(channel.ownership.isIdle())
    }
    @Test fun successfulIdleSessionCloseHoldsGateUntilExactMainClosure() {
        val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
        val lease = bridge.acquire(connection.epoch); val call = channel.call(lease)
        bridge.exchange(lease, call, request).fill(0)
        assertTrue(call.transportCompleted()); call.validateReply()
        channel.defer = true; bridge.cancel(lease)
        assertFalse(lease.isActive()); assertFalse(lease.retire()); assertEquals(0, channel.closed)
        assertThrows(DurableTransportBusyException::class.java) { channel.ownership.acquire(UUID(8, 9)) }
        channel.drain(); assertEquals(1, channel.closed); assertTrue(channel.ownership.isIdle())
        val newer = channel.ownership.acquire(UUID(8, 9)); channel.lease = newer
        bridge.cancel(lease); assertEquals(0, channel.queue.size); assertTrue(newer.isActive())
        newer.retire()
    }
    @Test fun successfulIdleSessionRejectedOrThrowingCloseCannotRetire() {
        for (reject in listOf(false, true)) {
            val channel = Channel(); val bridge = DurableBleCallbackExchange(channel) { channel.now }
            val lease = bridge.acquire(connection.epoch)
            channel.rejectPosts = reject; channel.throwClose = !reject
            bridge.cancel(lease)
            assertFalse(lease.retire()); assertFalse(channel.ownership.isIdle())
        }
    }
}
