package org.openpendant.app

import java.util.UUID

/** Radio-only lifecycle shared by the actual client and future durable adapter.
 * Monitors protect short in-memory admissions, never Bluetooth calls or waits.
 * A new connection remains setup/query-only until the client validates INFO.
 */
internal class PendantRadioOwnership(private val ownership: DurableTransportOwnership = DurableTransportOwnership()) {
    internal class Connection internal constructor(val epoch: UUID) {
        internal var source: Any? = null
        internal var ready = false
    }
    private enum class Scope { SETUP, CONTROL, CAPTURE, DURABLE, LONG }
    private class Session(val connection: Connection, val scope: Scope, val lease: DurableTransportLease) {
        var exchange: Exchange? = null
        var setup: DurableTransportRequest? = null
    }
    internal class Closed internal constructor(internal val lease: DurableTransportLease)
    private var connection: Connection? = null
    private var session: Session? = null

    @Synchronized fun open(): Connection {
        check(connection == null)
        pruneRetired()
        if (session != null) throw DurableTransportBusyException()
        val fresh = Connection(UUID.randomUUID())
        val scope = Session(fresh, Scope.SETUP, ownership.acquire(fresh.epoch))
        scope.setup = scope.lease.beginRequest()
        session = scope; connection = fresh
        return fresh
    }
    @Synchronized fun attach(current: Connection, source: Any) {
        requireCurrent(current); check(current.source == null); current.source = source
    }
    @Synchronized fun accepts(current: Connection, source: Any): Boolean =
        connection === current && current.source === source
    @Synchronized fun currentEpoch(): UUID? = connection?.takeIf { it.ready }?.epoch
    @Synchronized fun idle(): Boolean { pruneRetired(); return session == null && ownership.isIdle() }

    @Synchronized fun setupTransportCompleted(current: Connection) {
        requireCurrent(current)
        val scope = requireScope(current, Scope.SETUP)
        check(scope.setup?.completed() == true); scope.setup = null
    }
    @Synchronized fun setupValidated(current: Connection) {
        val scope = requireScope(current, Scope.SETUP)
        check(scope.setup == null && scope.exchange == null)
        check(scope.lease.retire()); session = null; current.ready = true
    }
    @Synchronized fun startCapture(current: Connection) { acquire(current, Scope.CAPTURE) }
    @Synchronized fun finishCapture(current: Connection) {
        val scope = requireScope(current, Scope.CAPTURE)
        check(scope.exchange == null && scope.lease.retire()); session = null
    }
    @Synchronized fun acquireDurable(current: Connection): DurableTransportLease = acquire(current, Scope.DURABLE).lease
    @Synchronized fun acquireLong(current: Connection): DurableTransportLease = acquire(current, Scope.LONG).lease
    @Synchronized fun ownsLong(current: Connection, lease: DurableTransportLease): Boolean =
        connection === current && session?.let { it.connection === current && it.scope == Scope.LONG && it.lease === lease } == true
    @Synchronized fun borrowLong(current: Connection, lease: DurableTransportLease, ticket: DurableTransportRequest,
        command: Int, sequence: Int): Exchange {
        val scope = requireScope(current, Scope.LONG)
        check(scope.lease === lease && scope.exchange == null && ticket.belongsTo(lease))
        ticket.requirePending()
        check(command in LongRecordingControlCodec.STATUS..LongRecordingControlCodec.STOP)
        return exchange(scope, command, sequence, ticket, null, externalTicket = true)
    }
    @Synchronized fun ownsDurable(current: Connection, lease: DurableTransportLease): Boolean =
        connection === current && session?.let { it.connection === current && it.scope == Scope.DURABLE && it.lease === lease } == true

    @Synchronized fun legacy(current: Connection, command: Int, sequence: Int): Exchange {
        requireCurrent(current); pruneRetired()
        val scope = session ?: acquire(current, Scope.CONTROL)
        check(scope.connection === current && scope.exchange == null)
        val allowed = when(scope.scope) {
            Scope.SETUP -> scope.setup == null && command in setOf(OpProtocol.PING, OpProtocol.INFO, OpProtocol.DEVICE_STATUS)
            Scope.CAPTURE -> command in setOf(OpProtocol.BEGIN, OpProtocol.STATUS, OpProtocol.CHUNK, OpProtocol.CANCEL)
            Scope.CONTROL -> command in setOf(OpProtocol.DEVICE_STATUS,OpProtocol.GET_PREFERENCES,OpProtocol.SET_PREFERENCES)
            Scope.DURABLE -> false
            Scope.LONG -> false
        }
        check(allowed) { "Request does not belong to the radio owner" }
        val request = scope.lease.beginRequest()
        return exchange(scope, command, sequence, request, null)
    }

    fun checkBorrowAdmission(current: Connection, lease: DurableTransportLease, call: DurableSyncCall, command: Int) {
        call.borrowRequest(lease)
        synchronized(this) { checkBorrowScope(current, lease, call, command) }
    }
    private fun checkBorrowScope(current: Connection, lease: DurableTransportLease, call: DurableSyncCall, command: Int): Session {
        val scope = requireScope(current, Scope.DURABLE)
        check(scope.lease === lease && scope.exchange == null && call.connection.epoch == current.epoch)
        // Actual client additionally checks the epoch-bound negotiated bit and
        // exact body grammar. Legacy capture opcodes remain forbidden.
        check(command in setOf(OpProtocol.PING, OpProtocol.INFO, OpProtocol.DEVICE_STATUS) ||
            DurableBleCodec.isDurable(command))
        return scope
    }
    fun borrow(current: Connection, lease: DurableTransportLease, call: DurableSyncCall,
                              command: Int, sequence: Int, stream: DurableBleStreamAssembler? = null): Exchange {
        val request = call.borrowRequest(lease)
        return synchronized(this) {
            val scope = checkBorrowScope(current, lease, call, command)
            exchange(scope, command, sequence, request, call, stream)
        }
    }

    /** Revoke BEFORE any platform disconnect/close, so queued callbacks cannot
     * submit work even if close throws. No implicit reconnection or retry. */
    @Synchronized fun revoke(current: Connection): Closed? {
        if (connection !== current) return null
        pruneRetired()
        var scope = session
        val oldExchange = scope?.exchange
        // A worker may complete/retire its old lease between prune and revoke.
        // Reserve a fresh closure-only owner if that exact lease already left.
        if (scope == null || !scope.lease.revokeForTransportClose()) {
            scope = Session(current, Scope.CONTROL, ownership.acquire(current.epoch))
            check(scope.lease.revokeForTransportClose()); session = scope
        }
        connection = null; current.ready = false; current.source = null
        check(scope.connection === current)
        oldExchange?.discard(); scope.exchange = null
        return Closed(scope.lease)
    }
    @Synchronized fun closed(proof: Closed) {
        proof.lease.cancelAfterTransportClosed()
        pruneRetired()
    }

    private fun requireCurrent(current: Connection) { check(connection === current) { "Bluetooth connection epoch was revoked" } }
    private fun requireScope(current: Connection, scope: Scope): Session {
        requireCurrent(current)
        return checkNotNull(session).also { check(it.connection === current && it.scope == scope && it.lease.isActive()) }
    }
    private fun acquire(current: Connection, scope: Scope): Session {
        requireCurrent(current); check(current.ready); pruneRetired()
        if (session != null) throw DurableTransportBusyException()
        return Session(current, scope, ownership.acquire(current.epoch)).also { session = it }
    }
    private fun pruneRetired() {
        val old = session ?: return
        // A logical adapter proof cannot erase an actual in-flight GATT frame.
        // Early/incorrect proof is an adapter contract violation: keep this
        // client fenced until explicit closure, never admit the next producer.
        if (old.exchange == null && !old.lease.isActive() && old.lease.retire()) session = null
    }
    private fun exchange(scope: Session, command: Int, sequence: Int, request: DurableTransportRequest,
                         call: DurableSyncCall?, stream: DurableBleStreamAssembler? = null,
                         externalTicket: Boolean = false): Exchange {
        return Exchange(ResponseGate(command, sequence, stream),
            validate = {
                synchronized(this) {
                    requireCurrent(scope.connection)
                    check(session === scope && scope.lease.isActive())
                    request.requirePending()
                }
                // The injected freshness/cancellation predicate is outside our
                // monitor, and may only perform bounded nonblocking checks.
                call?.checkActive()
            }, finish = {
                synchronized(this) {
                    requireCurrent(scope.connection); check(session === scope)
                    request.requirePending()
                    scope.exchange = null
                    if (call == null && !externalTicket) {
                        check(request.completed())
                        if (scope.scope == Scope.CONTROL) { check(scope.lease.retire()); session = null }
                    }
                    // Borrowed logical requests may span many bounded frames.
                    // Their adapter proves transportCompleted only after the
                    // FINAL frame and semantic reply validation, not here.
                }
            }).also { scope.exchange = it }
    }

    internal class Exchange internal constructor(private val gate: ResponseGate,
        private val validate: () -> Unit, private val finish: () -> Unit) {
        val ready: Boolean get() = gate.ready
        fun written(success: Boolean) { validate(); gate.written(success) }
        fun notified(packet: ByteArray) { validate(); gate.notified(packet) }
        fun take(): ByteArray {
            validate()
            val body = gate.take()
            try { validate(); finish(); return body } catch (failure: Throwable) { body.fill(0); throw failure }
        }
        fun discard() { gate.discard() }
    }
}
