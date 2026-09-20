package org.openpendant.app

import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean

internal data class LongRecordingPeer(val epoch: UUID,val bondAddress: String,val capabilityBits: Long)

/** The exact-GATT adapter shares PendantClient's radio gate. exchange
 * returns only after successful write AND complete copied OP reply with no
 * pending callback; the core then validates semantics before completing ticket.
 * retire closes/revokes this exact source, and releases only on actual local
 * quiescence. Never infer remote stop/save from local closure. No default port. */
internal interface LongRecordingControlTransport {
    fun peer(): LongRecordingPeer
    fun acquire(peer: LongRecordingPeer): DurableTransportLease
    fun current(peer: LongRecordingPeer): Boolean
    fun exchange(lease: DurableTransportLease,ticket: DurableTransportRequest,request: LongRecordingControlCodec.Request,
        deadlineMillis: Long,cancelled: () -> Boolean): ByteArray
    fun retire(lease: DurableTransportLease)
}

internal enum class LongRecordingOutcome { OBSERVED, UNKNOWN, BOOT_CHANGED }
internal data class LongRecordingObservation(val outcome: LongRecordingOutcome,val state: LongRecordingControlCodec.State?)

/** Worker-only explicit operations; no timers, automatic START/STOP/retry,
 * connection, scanning or microphone APIs. Holds only RADIO ownership during
 * waits, never the journal/file monitor. Caller serializes one session; lifecycle
 * close is nonblocking and may race an in-flight request (which is then fenced).
 * The first profile is USB-powered engineering continuous/manual stop only. */
internal class LongRecordingControl(private val binding: DurablePublicBinding,private val store: LongRecordingIntentStore,
    private val transport: LongRecordingControlTransport,private val requireStartAuthority: () -> Unit,
    private val clock: () -> Long = { System.nanoTime()/1_000_000 }) {
    init { require(store.binding==binding) }
    fun openExplicit(): Session {
        val peer=transport.peer();require(validOwnedUuid(peer.epoch) && peer.bondAddress==binding.bondAddress &&
            peer.capabilityBits and LongRecordingControlCodec.CAPABILITY != 0L)
        check(transport.current(peer));val lease=transport.acquire(peer)
        try { check(lease.epoch==peer.epoch && lease.isActive() && transport.current(peer));return Session(peer,lease) }
        catch(failure: Throwable) { transport.retire(lease);throw failure }
    }
    inner class Session internal constructor(private val peer: LongRecordingPeer,private val lease: DurableTransportLease): AutoCloseable {
        private val busy=AtomicBoolean(false)
        private val closed=AtomicBoolean(false)
        private var sequence=0
        private var boot: UUID?=null
        private var global: LongRecordingControlCodec.State?=null
        private var last=0L
        private fun time(deadline: Long): Long {
            val value=clock();check(value>=last && value>=0 && value<deadline);last=value;return value
        }
        private fun active(deadline: Long,cancelled: () -> Boolean) {
            time(deadline);check(!closed.get() && !cancelled() && lease.isActive() && transport.current(peer));time(deadline)
        }
        private fun next(command: Int,operation: UUID?=null,selectedBoot: UUID?=boot,waitForButton:Boolean=false): LongRecordingControlCodec.Request {
            check(sequence<65535);return LongRecordingControlCodec.request(command,++sequence,selectedBoot,operation,binding,waitForButton)
        }
        private fun <T> operation(cancelled: () -> Boolean,action: (Long) -> T): T {
            check(busy.compareAndSet(false,true))
            try {
                val now=clock();check(now>=0 && now>=last && now<=Long.MAX_VALUE-4000);last=now
                val deadline=now+4000;active(deadline,cancelled)
                val result=action(deadline);active(deadline,cancelled);return result
            } catch(failure: Throwable) {
                // Timeout/cancel/error closes the local channel, never clears the
                // persisted operation. STATUS after explicit reconnect is the
                // only reconciliation path; no second START or automatic STOP.
                close();throw IllegalStateException("Long recording outcome unavailable; reconcile status without repeating start",failure)
            } finally { busy.set(false) }
        }
        private fun exchange(request: LongRecordingControlCodec.Request,deadline: Long,cancelled: () -> Boolean,
            persist: Boolean): LongRecordingControlCodec.State {
            active(deadline,cancelled);val ticket=lease.beginRequest()
            val reply=transport.exchange(lease,ticket,request,deadline,cancelled)
            try {
                active(deadline,cancelled);ticket.requirePending()
                val state=LongRecordingControlCodec.parse(reply,request)
                global?.takeIf { it.operation!=null && it.operation==state.operation && it.boot==state.boot }
                    ?.let { LongRecordingControlCodec.progress(it,state) }
                if(persist)store.observe(request,reply)
                active(deadline,cancelled);check(ticket.completed());ticket.requireSuccessful();active(deadline,cancelled);return state
            } finally { reply.fill(0) }
        }
        fun discover(cancelled: () -> Boolean = { false }): LongRecordingObservation = operation(cancelled) { deadline ->
            val state=exchange(next(LongRecordingControlCodec.STATUS,selectedBoot=null),deadline,cancelled,false)
            boot=state.boot;global=state
            val pending=store.read().values.filter { it.unresolved }
            if(pending.any { it.start.boot!=boot })LongRecordingObservation(LongRecordingOutcome.BOOT_CHANGED,null)
            else LongRecordingObservation(if(pending.isEmpty())LongRecordingOutcome.OBSERVED else LongRecordingOutcome.UNKNOWN,state)
        }
        fun startExplicit(operationId: UUID,waitForButton:Boolean=false,cancelled: () -> Boolean = { false }): LongRecordingObservation = operation(cancelled) { deadline ->
            require(validOwnedUuid(operationId));requireStartAuthority();active(deadline,cancelled)
            val state=checkNotNull(global);check(boot==state.boot &&
                (state.has(LongRecordingControlCodec.USB)||state.has(LongRecordingControlCodec.PORTABLE)) && !state.has(LongRecordingControlCodec.MIC))
            check(state.phase in setOf(LongRecordingControlCodec.Phase.IDLE,LongRecordingControlCodec.Phase.STOPPED,
                LongRecordingControlCodec.Phase.NO_CAPACITY,LongRecordingControlCodec.Phase.CANCELLED_BEFORE_START))
            check(store.read().values.none { it.unresolved })
            val request=next(LongRecordingControlCodec.START,operationId,waitForButton=waitForButton)
            store.begin(peer.epoch,request) // Durable irreversible intent BEFORE any START write.
            active(deadline,cancelled);requireStartAuthority();active(deadline,cancelled)
            val observed=exchange(request,deadline,cancelled,true);global=observed
            LongRecordingObservation(LongRecordingOutcome.OBSERVED,observed)
        }
        fun reconcile(operationId: UUID,cancelled: () -> Boolean = { false }): LongRecordingObservation = operation(cancelled) { deadline ->
            val intent=checkNotNull(store.read()[operationId]);checkNotNull(boot)
            if(intent.start.boot!=boot) return@operation LongRecordingObservation(LongRecordingOutcome.BOOT_CHANGED,null)
            val state=exchange(next(LongRecordingControlCodec.STATUS,operationId),deadline,cancelled,true);global=state
            LongRecordingObservation(LongRecordingOutcome.OBSERVED,state)
        }
        fun stopExplicit(operationId: UUID,cancelled: () -> Boolean = { false }): LongRecordingObservation = operation(cancelled) { deadline ->
            val intent=checkNotNull(store.read()[operationId]);check(intent.start.boot==boot && boot!=null && intent.unresolved)
            if(intent.stop!=null) {
                val observed=checkNotNull(global)
                check(observed.operation==operationId && observed.boot==boot && !observed.terminal &&
                    observed.reason==0 && !observed.has(LongRecordingControlCodec.STOP_REQUESTED))
            }
            val request=next(LongRecordingControlCodec.STOP,operationId)
            store.stop(peer.epoch,request);active(deadline,cancelled)
            // Separate0x42 frame; a refusal/pending/error is UNKNOWN, never a
            // synthetic USER/STOPPING which could overwrite a concurrent FULL.
            val state=exchange(request,deadline,cancelled,true);global=state
            LongRecordingObservation(LongRecordingOutcome.OBSERVED,state)
        }
        /** Explicit STOP of the physical recording selected by the user. No
         * synthetic START intent is created: the app did not start this audio.
         * Reconcile the exact boot/operation before STOP. A lost reply stays
         * unknown and cannot trigger an automatic retry or stop a replacement. */
        fun stopObservedExplicit(selected: LongRecordingControlCodec.State,
            cancelled: () -> Boolean = { false }): LongRecordingObservation = operation(cancelled) { deadline ->
            check(peer.capabilityBits and LongRecordingControlCodec.STANDALONE_CAPABILITY != 0L)
            check(store.read().values.none { it.unresolved })
            val id=checkNotNull(selected.operation)
            check(selected.boot==boot && !selected.terminal && selected.phase!=LongRecordingControlCodec.Phase.IDLE)
            val current=exchange(next(LongRecordingControlCodec.STATUS,id,selected.boot),deadline,cancelled,false)
            LongRecordingControlCodec.progress(selected,current);global=current
            if(current.terminal || current.reason!=0 || current.has(LongRecordingControlCodec.STOP_REQUESTED))
                return@operation LongRecordingObservation(LongRecordingOutcome.OBSERVED,current)
            val stopped=exchange(next(LongRecordingControlCodec.STOP,id,selected.boot),deadline,cancelled,false)
            global=stopped;LongRecordingObservation(LongRecordingOutcome.OBSERVED,stopped)
        }
        override fun close() {
            if(closed.compareAndSet(false,true)) {
                lease.revokeForTransportClose()
                try { transport.retire(lease) } catch(_: Throwable) { /* Keep radio fence on unknown close. */ }
            }
        }
    }
}
