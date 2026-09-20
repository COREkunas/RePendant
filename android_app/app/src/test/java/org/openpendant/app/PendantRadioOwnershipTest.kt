package org.openpendant.app

import java.util.UUID
import java.util.concurrent.CancellationException
import org.junit.Assert.*
import org.junit.Test

/** Actual ownership helper + actual strict ResponseGate, no Android radio. */
class PendantRadioOwnershipTest {
    @Test fun settingsNeedTheIdleControlOwnerAndBothCallbacks() {
        for(command in listOf(OpProtocol.GET_PREFERENCES,OpProtocol.SET_PREFERENCES)) {
            val radio=PendantRadioOwnership();val epoch=ready(radio)
            val request=radio.legacy(epoch,command,3)
            assertThrows(DurableTransportBusyException::class.java){radio.acquireDurable(epoch)}
            complete(request,command,3,true);assertTrue(radio.idle())
            radio.startCapture(epoch)
            assertThrows(IllegalStateException::class.java){radio.legacy(epoch,command,4)}
        }
    }
    private fun packet(command: Int, sequence: Int, body: ByteArray = byteArrayOf(7)): ByteArray =
        byteArrayOf(79, 80, 1, (command or 128).toByte(), sequence.toByte(), (sequence ushr 8).toByte(),
            (body.size + 1).toByte(), 0, 0) + body
    private fun complete(exchange: PendantRadioOwnership.Exchange, command: Int, sequence: Int,
                         notificationFirst: Boolean = false) {
        if (notificationFirst) exchange.notified(packet(command, sequence)) else exchange.written(true)
        assertFalse(exchange.ready)
        if (notificationFirst) exchange.written(true) else exchange.notified(packet(command, sequence))
        assertTrue(exchange.ready); assertArrayEquals(byteArrayOf(7), exchange.take())
    }
    private fun ready(radio: PendantRadioOwnership, source: Any = Any()): PendantRadioOwnership.Connection {
        val epoch = radio.open(); radio.attach(epoch, source); radio.setupTransportCompleted(epoch)
        complete(radio.legacy(epoch, OpProtocol.PING, 1), OpProtocol.PING, 1)
        complete(radio.legacy(epoch, OpProtocol.INFO, 2), OpProtocol.INFO, 2, true)
        radio.setupValidated(epoch); return epoch
    }
    private fun call(epoch: PendantRadioOwnership.Connection, lease: DurableTransportLease,
                     check: () -> Unit = {}): DurableSyncCall = DurableSyncCall(
        DurableSyncConnection(epoch.epoch, RecordingVolume(UUID(1, 2), UUID(3, 4), 1), "12".repeat(32)),
        10000, lease.beginRequest(), check)

    @Test fun setupOwnsConnectionUntilTransportAndQueryValidation() {
        val radio = PendantRadioOwnership(); val epoch = radio.open(); val source = Any(); radio.attach(epoch, source)
        assertNull(radio.currentEpoch()); assertFalse(radio.idle()); assertTrue(radio.accepts(epoch, source))
        assertThrows(IllegalStateException::class.java) { radio.startCapture(epoch) }
        assertThrows(IllegalStateException::class.java) { radio.acquireDurable(epoch) }
        assertThrows(IllegalStateException::class.java) { radio.legacy(epoch, OpProtocol.PING, 1) }
        radio.setupTransportCompleted(epoch)
        val ping = radio.legacy(epoch, OpProtocol.PING, 1)
        assertThrows(IllegalStateException::class.java) { radio.setupValidated(epoch) }
        complete(ping, OpProtocol.PING, 1); assertFalse(radio.idle())
        complete(radio.legacy(epoch, OpProtocol.INFO, 2), OpProtocol.INFO, 2)
        radio.setupValidated(epoch); assertTrue(radio.idle()); assertEquals(epoch.epoch, radio.currentEpoch())
    }

    @Test fun controlHoldsGateUntilBothGattCallbacksInEitherOrder() {
        for (first in listOf(false, true)) {
            val radio = PendantRadioOwnership(); val epoch = ready(radio)
            val status = radio.legacy(epoch, OpProtocol.DEVICE_STATUS, 3)
            if (first) status.notified(packet(OpProtocol.DEVICE_STATUS, 3)) else status.written(true)
            assertFalse(radio.idle())
            assertThrows(DurableTransportBusyException::class.java) { radio.acquireDurable(epoch) }
            assertThrows(DurableTransportBusyException::class.java) { radio.startCapture(epoch) }
            if (first) status.written(true) else status.notified(packet(OpProtocol.DEVICE_STATUS, 3))
            assertFalse(radio.idle()); status.take().fill(0); assertTrue(radio.idle())
        }
    }

    @Test fun reconnectedReadyEpochMustWaitForAutomaticTelemetryBeforeDurableAdmission() {
        val radio = PendantRadioOwnership(); val old = ready(radio)
        val oldLease = radio.acquireDurable(old)
        val closed = checkNotNull(radio.revoke(old)); radio.closed(closed); oldLease.retire()
        val fresh = ready(radio)
        val telemetry = radio.legacy(fresh, OpProtocol.DEVICE_STATUS, 3)
        assertNotEquals(old.epoch, fresh.epoch)
        assertEquals(fresh.epoch, radio.currentEpoch()) // Ready is observable now.
        assertFalse(radio.idle()) // But the reconnect must not hand it off yet.
        assertThrows(DurableTransportBusyException::class.java) { radio.acquireDurable(fresh) }
        complete(telemetry, OpProtocol.DEVICE_STATUS, 3)
        assertTrue(radio.idle())
        val resumed = radio.acquireDurable(fresh)
        assertTrue(resumed.isActive()); assertTrue(resumed.retire()); assertTrue(radio.idle())
    }

    @Test fun retirementStatusMayBePolledDuringSetupWithoutGrantingStorageOrCapture() {
        val radio = PendantRadioOwnership(); val epoch = radio.open(); radio.attach(epoch, Any())
        assertThrows(IllegalStateException::class.java) { radio.legacy(epoch, OpProtocol.DEVICE_STATUS, 1) }
        radio.setupTransportCompleted(epoch)
        repeat(3) { index ->
            complete(radio.legacy(epoch, OpProtocol.INFO, index*2+1), OpProtocol.INFO, index*2+1)
            complete(radio.legacy(epoch, OpProtocol.DEVICE_STATUS, index*2+2), OpProtocol.DEVICE_STATUS, index*2+2)
            assertNull(radio.currentEpoch()); assertFalse(radio.idle())
            assertThrows(IllegalStateException::class.java) { radio.acquireDurable(epoch) }
            assertThrows(IllegalStateException::class.java) { radio.startCapture(epoch) }
        }
        radio.setupValidated(epoch)
        assertEquals(epoch.epoch, radio.currentEpoch()); assertTrue(radio.idle())
    }

    @Test fun captureRetainsExclusiveOwnershipAcrossEveryPollingGapAndCancellation() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); radio.startCapture(epoch)
        complete(radio.legacy(epoch, OpProtocol.BEGIN, 3), OpProtocol.BEGIN, 3)
        assertFalse(radio.idle())
        assertThrows(DurableTransportBusyException::class.java) { radio.acquireDurable(epoch) }
        assertThrows(IllegalStateException::class.java) { radio.legacy(epoch, OpProtocol.DEVICE_STATUS, 4) }
        for ((index, command) in listOf(OpProtocol.STATUS, OpProtocol.CHUNK, OpProtocol.CANCEL, OpProtocol.STATUS).withIndex()) {
            val pending = radio.legacy(epoch, command, 5 + index)
            assertThrows(IllegalStateException::class.java) { radio.finishCapture(epoch) }
            complete(pending, command, 5 + index); assertFalse(radio.idle())
        }
        radio.finishCapture(epoch); assertTrue(radio.idle()); radio.acquireDurable(epoch).retire()
    }

    @Test fun borrowedFragmentsRetainOneLogicalRequestUntilAdapterProof() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val call = call(epoch, lease)
        repeat(3) { index ->
            val exchange = radio.borrow(epoch, lease, call, OpProtocol.PING, index + 3)
            assertThrows(IllegalStateException::class.java) { radio.borrow(epoch, lease, call, OpProtocol.PING, 99) }
            complete(exchange, OpProtocol.PING, index + 3, index % 2 == 0)
            call.checkActive(); assertThrows(IllegalStateException::class.java) { call.validateReply() }
            assertFalse(radio.idle())
        }
        assertTrue(call.transportCompleted()); call.validateReply()
        assertFalse(radio.idle()); assertTrue(lease.retire()); assertTrue(radio.idle())
    }

    @Test fun borrowedOwnerCannotBypassCaptureOrUnknownOpcodeGates() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val call = call(epoch, lease)
        for (command in listOf(OpProtocol.BEGIN, OpProtocol.CHUNK, OpProtocol.CANCEL, OpProtocol.STATUS, 0x21, 0xff)) {
            assertThrows(IllegalStateException::class.java) { radio.checkBorrowAdmission(epoch, lease, call, command) }
            call.checkActive(); assertTrue(lease.isActive())
        }
        complete(radio.borrow(epoch, lease, call, OpProtocol.INFO, 3), OpProtocol.INFO, 3)
        assertThrows(IllegalStateException::class.java) { radio.legacy(epoch, OpProtocol.DEVICE_STATUS, 4) }
        assertThrows(DurableTransportBusyException::class.java) { radio.startCapture(epoch) }
        call.transportCompleted(); lease.retire(); assertTrue(radio.idle())
    }

    @Test fun foreignLeaseCallAndEpochAreRejectedWithoutRetiringCurrentOwner() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val call = call(epoch, lease)
        val otherGate = DurableTransportOwnership(); val foreign = otherGate.acquire(epoch.epoch)
        val foreignCall = call(epoch, foreign)
        assertThrows(IllegalArgumentException::class.java) { radio.checkBorrowAdmission(epoch, foreign, call, OpProtocol.PING) }
        assertThrows(IllegalStateException::class.java) { radio.checkBorrowAdmission(epoch, foreign, foreignCall, OpProtocol.PING) }
        val wrongEpoch = DurableSyncCall(DurableSyncConnection(UUID(7, 8), call.connection.volume, "12".repeat(32)),
            10000, foreignCall.borrowRequest(foreign)) {}
        assertThrows(IllegalArgumentException::class.java) { radio.borrow(epoch, foreign, wrongEpoch, OpProtocol.PING, 3) }
        assertTrue(radio.ownsDurable(epoch, lease)); call.checkActive()
        complete(radio.borrow(epoch, lease, call, OpProtocol.PING, 3), OpProtocol.PING, 3)
    }

    @Test fun timedOutRequestRetirementDoesNotProveCallbackQuiescence() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val call = call(epoch, lease); val exchange = radio.borrow(epoch, lease, call, OpProtocol.PING, 3)
        exchange.written(true); assertFalse(lease.retire()); assertFalse(radio.idle())
        assertThrows(IllegalStateException::class.java) { exchange.notified(packet(OpProtocol.PING, 3)) }
        assertThrows(DurableTransportBusyException::class.java) { radio.startCapture(epoch) }
        val closed = checkNotNull(radio.revoke(epoch)); assertFalse(radio.idle())
        radio.closed(closed); assertTrue(radio.idle()); assertFalse(call.transportCompleted())
    }

    @Test fun failedCloseKeepsGateEvenDuringIdleAndCapturePollGaps() {
        for (capture in listOf(false, true)) {
            val radio = PendantRadioOwnership(); val epoch = ready(radio)
            if (capture) { radio.startCapture(epoch); complete(radio.legacy(epoch, OpProtocol.BEGIN, 3), OpProtocol.BEGIN, 3) }
            val proof = checkNotNull(radio.revoke(epoch))
            assertFalse(radio.idle()); assertNull(radio.currentEpoch())
            // A throwing close supplies no proof and cannot authorize reconnect.
            assertThrows(DurableTransportBusyException::class.java) { radio.open() }
            radio.closed(proof); ready(radio)
        }
    }

    @Test fun completedCallbackAfterRevocationStillCannotReleaseCloseFence() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val call = call(epoch, lease); val proof = checkNotNull(radio.revoke(epoch))
        assertTrue(call.transportCompleted()) // Late adapter proof is not a local-close proof.
        assertFalse(lease.retire()); assertFalse(radio.idle())
        assertThrows(IllegalStateException::class.java) { call.validateReply() }
        assertThrows(DurableTransportBusyException::class.java) { radio.open() }
        radio.closed(proof); ready(radio)
    }

    @Test fun unsubmittedWorkerRequestIsCancelledOnlyBySuccessfulLocalClose() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val queued = call(epoch, lease) // No exchange has been posted to main yet.
        val proof = checkNotNull(radio.revoke(epoch)); assertFalse(lease.retire())
        radio.closed(proof); assertFalse(queued.transportCancelled()); assertFalse(queued.transportCompleted())
        val next = ready(radio); val newLease = radio.acquireDurable(next)
        assertThrows(IllegalStateException::class.java) { radio.borrow(epoch, lease, queued, OpProtocol.PING, 3) }
        assertTrue(newLease.isActive()); newLease.retire()
    }

    @Test fun oldSourceTokenExchangeAndCloseProofCannotAffectNewConnection() {
        val radio = PendantRadioOwnership(); val reusedFakeSource = Any(); val old = ready(radio, reusedFakeSource)
        val oldExchange = radio.legacy(old, OpProtocol.DEVICE_STATUS, 3)
        oldExchange.notified(packet(OpProtocol.DEVICE_STATUS, 3))
        val proof = checkNotNull(radio.revoke(old)); radio.closed(proof)
        val fresh = ready(radio, reusedFakeSource); radio.startCapture(fresh)
        assertNotEquals(old.epoch, fresh.epoch)
        assertFalse(radio.accepts(old, reusedFakeSource)); assertTrue(radio.accepts(fresh, reusedFakeSource))
        assertThrows(IllegalStateException::class.java) { oldExchange.written(true) }
        assertNull(radio.revoke(old)); radio.closed(proof); assertFalse(radio.idle())
        complete(radio.legacy(fresh, OpProtocol.BEGIN, 3), OpProtocol.BEGIN, 3)
        radio.finishCapture(fresh)
    }

    @Test fun cancellationOrDeadlineBeforeSecondCallbackNeverDeliversSuccess() {
        for (notifyFirst in listOf(false, true)) {
            var cancelled = false
            val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
            val call = call(epoch, lease) { if (cancelled) throw CancellationException() }
            val exchange = radio.borrow(epoch, lease, call, OpProtocol.PING, 3)
            if (notifyFirst) exchange.notified(packet(OpProtocol.PING, 3)) else exchange.written(true)
            cancelled = true
            assertThrows(CancellationException::class.java) {
                if (notifyFirst) exchange.written(true) else exchange.notified(packet(OpProtocol.PING, 3))
            }
            assertFalse(exchange.ready); assertFalse(lease.retire())
            radio.closed(checkNotNull(radio.revoke(epoch))); assertFalse(call.transportCompleted())
        }
    }

    @Test fun finalFreshnessCheckAfterBothCallbacksStillRequiredBeforeTake() {
        var now = 0L
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val call = call(epoch, lease) { check(now < 10000) }
        val exchange = radio.borrow(epoch, lease, call, OpProtocol.PING, 3)
        exchange.written(true); exchange.notified(packet(OpProtocol.PING, 3)); assertTrue(exchange.ready)
        now = 10000
        assertThrows(IllegalStateException::class.java) { exchange.take() }
        assertFalse(lease.retire()); radio.closed(checkNotNull(radio.revoke(epoch)))
    }

    @Test fun malformedAndDuplicateResponsesDoNotReleaseCapture() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); radio.startCapture(epoch)
        val exchange = radio.legacy(epoch, OpProtocol.BEGIN, 3)
        assertThrows(ProtocolException::class.java) { exchange.notified(packet(OpProtocol.BEGIN, 2)) }
        exchange.written(true)
        assertThrows(ProtocolException::class.java) { exchange.written(true) }
        assertFalse(radio.idle()); radio.closed(checkNotNull(radio.revoke(epoch)))
    }

    @Test fun previouslyRetiredIdleDurableScopeStillGetsClosureFence() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        assertTrue(lease.retire())
        val closed = checkNotNull(radio.revoke(epoch)); assertFalse(radio.idle())
        assertThrows(DurableTransportBusyException::class.java) { radio.open() }
        radio.closed(closed); ready(radio)
    }

    @Test fun racingWorkerRetirementAndMainRevocationAlwaysNeedsCloseProof() {
        repeat(100) {
            val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
            val call = call(epoch, lease)
            var failure: Throwable? = null
            val worker = Thread { try { call.transportCompleted(); lease.retire() } catch (error: Throwable) { failure = error } }
            worker.start(); val proof = checkNotNull(radio.revoke(epoch)); worker.join(2000)
            assertFalse(worker.isAlive); assertNull(failure)
            assertFalse(radio.idle()); assertThrows(DurableTransportBusyException::class.java) { radio.open() }
            radio.closed(proof); ready(radio)
        }
    }

    @Test fun recreatedClientCannotBypassPriorLiveOrFailedCloseOwner() {
        val processGate = DurableTransportOwnership()
        val old = PendantRadioOwnership(processGate); val oldEpoch = ready(old)
        old.startCapture(oldEpoch)
        val recreated = PendantRadioOwnership(processGate)
        assertFalse(recreated.idle())
        assertThrows(DurableTransportBusyException::class.java) { recreated.open() }
        val proof = checkNotNull(old.revoke(oldEpoch))
        assertThrows(DurableTransportBusyException::class.java) { recreated.open() }
        old.closed(proof); val fresh = ready(recreated)
        recreated.startCapture(fresh); old.closed(proof)
        assertFalse(recreated.idle()); recreated.finishCapture(fresh)
    }

    @Test fun sharedProcessHelperRejectsSecondClientEvenWhenConnectedIdle() {
        val radio = PendantRadioOwnership(); val original = ready(radio)
        assertTrue(radio.idle())
        assertThrows(IllegalStateException::class.java) { radio.open() }
        assertEquals(original.epoch, radio.currentEpoch())
        radio.closed(checkNotNull(radio.revoke(original)))
        val fresh = ready(radio); assertNotEquals(original.epoch, fresh.epoch)
    }

    @Test fun prematureLogicalProofCannotBypassActualPendingGattFrame() {
        val radio = PendantRadioOwnership(); val epoch = ready(radio); val lease = radio.acquireDurable(epoch)
        val call = call(epoch, lease); val pending = radio.borrow(epoch, lease, call, OpProtocol.PING, 3)
        pending.notified(packet(OpProtocol.PING, 3))
        assertTrue(call.transportCompleted()); assertTrue(lease.retire()) // Deliberately invalid adapter proof.
        assertFalse(radio.idle())
        assertThrows(DurableTransportBusyException::class.java) { radio.startCapture(epoch) }
        assertThrows(IllegalStateException::class.java) { pending.written(true) }
        val proof = checkNotNull(radio.revoke(epoch)); assertFalse(radio.idle())
        radio.closed(proof); ready(radio)
        assertThrows(IllegalStateException::class.java) { pending.take() }
    }
}
