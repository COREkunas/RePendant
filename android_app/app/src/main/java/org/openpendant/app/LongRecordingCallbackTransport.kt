package org.openpendant.app

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

internal interface LongRecordingCallbackChannel {
    fun acquire(peer: LongRecordingPeer): DurableTransportLease
    fun current(peer: LongRecordingPeer): Boolean
    fun onCallbackThread(): Boolean
    fun post(action: () -> Unit): Boolean
    fun close(lease: DurableTransportLease)
    fun submit(lease: DurableTransportLease, ticket: DurableTransportRequest,
        request: LongRecordingControlCodec.Request, deadline: Long, cancelled: () -> Boolean,
        success: (ByteArray) -> Unit, failure: () -> Unit)
}

/** One explicit control session, bounded worker waits, same real radio owner.
 * A timeout closes only local transport; it NEVER sends Start or Stop again. */
internal class LongRecordingCallbackTransport(private val selected: LongRecordingPeer,
    private val channel: LongRecordingCallbackChannel,
    private val clock: () -> Long = { System.nanoTime()/1_000_000 }) : LongRecordingControlTransport {
    override fun peer() = selected
    override fun acquire(peer: LongRecordingPeer): DurableTransportLease {
        check(!channel.onCallbackThread() && peer == selected && current(peer))
        return channel.acquire(peer)
    }
    override fun current(peer: LongRecordingPeer) = peer == selected && channel.current(peer)
    override fun retire(lease: DurableTransportLease) {
        lease.revokeForTransportClose()
        channel.post { try { channel.close(lease) } catch (_: Throwable) { } }
    }
    override fun exchange(lease: DurableTransportLease, ticket: DurableTransportRequest,
        request: LongRecordingControlCodec.Request, deadlineMillis: Long, cancelled: () -> Boolean): ByteArray {
        check(!channel.onCallbackThread() && ticket.belongsTo(lease) && lease.epoch == selected.epoch)
        val timeLock=Any();var last=clock();check(last>=0 && deadlineMillis>last && deadlineMillis-last<=4000)
        fun time(): Long = synchronized(timeLock) {
            val now=clock();check(now>=last && now<deadlineMillis);last=now;now
        }
        fun active() { time();ticket.requirePending();check(!cancelled()&&current(selected));time() }
        val pending=Pending();var owned: ByteArray?=null
        try {
            active()
            check(channel.post {
                if (!pending.waiting()) return@post
                try {
                    active()
                    channel.submit(lease,ticket,request,deadlineMillis,cancelled,{ bytes ->
                        try { active();pending.accept(bytes) }
                        catch (_: Throwable) { pending.fail();retire(lease) }
                    },{ pending.fail() })
                } catch (_: Throwable) { pending.fail();retire(lease) }
            })
            while (true) {
                active()
                if (pending.signal.await(minOf(50L,deadlineMillis-time()),TimeUnit.MILLISECONDS)) break
            }
            active();owned=pending.take();active();return checkNotNull(owned)
        } catch (failure: Throwable) {
            owned?.fill(0);pending.abandon();retire(lease)
            if (failure is InterruptedException) Thread.currentThread().interrupt()
            throw IllegalStateException("Recording response unavailable; check status before another Start",failure)
        }
    }
    private class Pending {
        val signal=CountDownLatch(1)
        private var terminal=false
        private var bytes: ByteArray?=null
        @Synchronized fun waiting() = !terminal
        @Synchronized fun accept(value: ByteArray) {
            if (terminal) return
            require(value.size==81);bytes=value.copyOf();terminal=true;signal.countDown()
        }
        @Synchronized fun fail() { if (!terminal) { terminal=true;signal.countDown() } }
        @Synchronized fun take(): ByteArray { check(terminal);return checkNotNull(bytes).also { bytes=null } }
        @Synchronized fun abandon() { bytes?.fill(0);bytes=null;terminal=true;signal.countDown() }
    }
}
