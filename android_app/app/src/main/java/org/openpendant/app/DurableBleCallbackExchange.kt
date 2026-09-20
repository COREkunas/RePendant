package org.openpendant.app

import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Main-thread callback seam. submit receives an immutable request and borrows
 * response only during success. It must use the actual shared radio owner and
 * validate BOTH callback events/OP frame, not just a notification. */
internal interface DurableBleCallbackChannel {
    fun capabilities(epoch: UUID): DurableSyncCapabilities?
    fun acquire(epoch: UUID): DurableTransportLease
    fun isCurrent(epoch: UUID): Boolean
    fun onCallbackThread(): Boolean
    fun post(action: () -> Unit): Boolean
    fun submit(lease: DurableTransportLease, call: DurableSyncCall, request: DurableBleRequest,
        success: (ByteArray) -> Unit, failure: () -> Unit)
    fun close(lease: DurableTransportLease)
}

/** Worker-only bounded wait, without any coordinator/file/ownership monitor.
 * Timeout includes main-queue delay, never renews the logical deadline. Platform
 * dispatch/close can themselves block: in that case the lease remains fenced,
 * not declared quiescent by the timeout. A late callback is discarded/wiped.
 */
internal class DurableBleCallbackExchange(private val channel: DurableBleCallbackChannel,
    private val clock: () -> Long = { System.nanoTime() / 1_000_000 }) : DurableBleExchange {
    override fun capabilities(epoch: UUID) = channel.capabilities(epoch)
    override fun acquire(epoch: UUID) = channel.acquire(epoch)
    override fun isCurrent(epoch: UUID) = channel.isCurrent(epoch)
    override fun cancel(lease: DurableTransportLease) {
        // Hold even an idle/completed logical lease BEFORE posting. Otherwise
        // session.retire could free the gate before main sees the close request.
        // This is admission fencing, not source revocation or closure proof.
        if (!lease.revokeForTransportClose()) return
        // No proof on post failure. Exact-lease client close performs source
        // revocation and release only after successful platform closure.
        channel.post { try { channel.close(lease) } catch (_: Throwable) { } }
    }
    override fun exchange(lease: DurableTransportLease, call: DurableSyncCall, request: DurableBleRequest): ByteArray {
        check(!channel.onCallbackThread()) { "Bluetooth wait cannot run on callback thread" }
        require(call.belongsTo(lease))
        val start = clock()
        val frameBudget = if (request.command == DurableBleCodec.FULL_GET_CATALOG &&
            request is DurableBleReadRequest && request.offset == 0) DurableRecordingSyncSession.FULL_METADATA_MILLIS
            else if (request.command == DurableBleCodec.FULL_RECEIVE_RANGE) DurableRecordingSyncSession.MAX_CALL_MILLIS else FRAME_MILLIS
        require(start >= 0 && start <= Long.MAX_VALUE - frameBudget)
        val deadline = minOf(call.deadlineMillis, start + frameBudget)
        val timeLock = Any(); var last = start
        fun time() = synchronized(timeLock) {
            val now = clock()
            check(now >= last && now < deadline) { "Bluetooth frame deadline expired" }
            last = now; now
        }
        fun active() {
            time(); call.checkActive(); check(channel.isCurrent(call.connection.epoch)); time()
        }
        val pending = Pending(DurableBleCodec.maxResponseBody(request.command))
        var body: ByteArray? = null
        try {
            active()
            check(channel.post {
                if (!pending.waiting()) return@post
                try {
                    active()
                    channel.submit(lease, call, request, { bytes ->
                        try { active(); pending.accept(bytes) }
                        catch (_: Throwable) {
                            pending.fail()
                            try { channel.close(lease) } catch (_: Throwable) { }
                        }
                    }, { pending.fail() })
                } catch (_: Throwable) {
                    pending.fail()
                    try { channel.close(lease) } catch (_: Throwable) { }
                }
            })
            while (true) {
                active()
                if (pending.signal.await(minOf(50L, deadline - time()), TimeUnit.MILLISECONDS)) break
            }
            active(); body = pending.take(); active()
            return checkNotNull(body)
        } catch (failure: Throwable) {
            body?.fill(0); pending.abandon()
            try { cancel(lease) } catch (_: Throwable) { }
            if (failure is InterruptedException) Thread.currentThread().interrupt()
            throw DurableSyncException(failure)
        }
    }
    private class Pending(private val maximumBody: Int) {
        val signal = CountDownLatch(1)
        private var terminal = false
        private var body: ByteArray? = null
        @Synchronized fun waiting() = !terminal
        @Synchronized fun accept(borrowed: ByteArray) {
            if (terminal) return
            require(borrowed.size <= maximumBody)
            body = borrowed.copyOf(); terminal = true; signal.countDown()
        }
        @Synchronized fun fail() { if (!terminal) { terminal = true; signal.countDown() } }
        @Synchronized fun take(): ByteArray { check(terminal); return checkNotNull(body).also { body = null } }
        @Synchronized fun abandon() { body?.fill(0); body = null; terminal = true; signal.countDown() }
    }
    companion object { const val FRAME_MILLIS = 4000L }
}
