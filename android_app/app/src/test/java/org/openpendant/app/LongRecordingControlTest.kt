package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class LongRecordingControlTest {
    @get:Rule val folder=TemporaryFolder()
    private val data=LongControlTestData
    private var now=100L
    private var authority=0
    private var denyAuthority=false
    private fun store(event:(String)->Unit={})=LongRecordingIntentJournal.createExplicit(folder.root.toPath().toRealPath().resolve("intent"),data.binding,{ },event)
    private inner class Radio: LongRecordingControlTransport {
        val gate=DurableTransportOwnership()
        var connection=LongRecordingPeer(UUID(5,6),data.binding.bondAddress,64)
        val writes=mutableListOf<LongRecordingControlCodec.Request>()
        var fail=false;var closeKnown=true;var changedBoot=false
        var selected="running"
        var discovered="idle"
        var after:()->Unit={}
        var transform:(ByteArray)->Unit={}
        override fun peer()=connection
        override fun acquire(peer: LongRecordingPeer)=gate.acquire(peer.epoch)
        override fun current(peer: LongRecordingPeer)=peer==connection
        override fun retire(lease: DurableTransportLease) { lease.revokeForTransportClose();if(closeKnown)lease.cancelAfterTransportClosed() }
        override fun exchange(lease: DurableTransportLease,ticket: DurableTransportRequest,request: LongRecordingControlCodec.Request,deadlineMillis: Long,cancelled:()->Boolean): ByteArray {
            ticket.requirePending();writes+=request
            if(fail)error("lost ACK")
            val name=when(request.command) { 0x41->"starting";0x42->"stopping";else->if(request.operation==null)discovered else selected }
            val reply=data.reply(name,request)
            if(changedBoot)LongRecordingControlCodec.uuid(UUID(7,8)).copyInto(reply,9)
            transform(reply)
            after();return reply
        }
    }
    private fun core(store: LongRecordingIntentStore,radio: Radio)=LongRecordingControl(data.binding,store,radio,{
        ++authority;check(!denyAuthority)
    },{ now })
    @Test fun explicitStartAfterFsyncSeparateStopAndStatusFinalizesOnlyExactOperation() {
        val events=mutableListOf<String>();val journal=store(events::add);val radio=Radio();val core=core(journal,radio)
        val session=core.openExplicit();session.discover();assertEquals(listOf(0x40),radio.writes.map { it.command })
        radio.after={ if(radio.writes.last().command==0x41)assertEquals("readback",events.last()) }
        session.startExplicit(data.operation);assertEquals(2,authority);assertTrue(journal.read().getValue(data.operation).unresolved)
        session.reconcile(data.operation);session.stopExplicit(data.operation)
        assertEquals(listOf(0x40,0x41,0x40,0x42),radio.writes.map { it.command })
        radio.selected="stopped";assertEquals(LongRecordingControlCodec.Phase.STOPPED,session.reconcile(data.operation).state?.phase)
        assertFalse(journal.read().getValue(data.operation).unresolved);session.close();assertTrue(radio.gate.isIdle())
    }
    @Test fun lostStartAckPersistsUnknownAndFreshSessionUsesOnlyStatusNoResend() {
        val journal=store();val radio=Radio();var session=core(journal,radio).openExplicit();session.discover();radio.fail=true
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation)}
        assertEquals(1,radio.writes.count { it.command==0x41 });assertNull(journal.read().getValue(data.operation).latest)
        radio.fail=false;session=core(journal,radio).openExplicit()
        assertEquals(LongRecordingOutcome.UNKNOWN,session.discover().outcome)
        assertEquals(LongRecordingControlCodec.Phase.RUNNING,session.reconcile(data.operation).state?.phase)
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation)}
        assertEquals(1,radio.writes.count { it.command==0x41 })
    }
    @Test fun lostStopReplyNeverBecomesStoppedAndDoesNotRepeatStop() {
        val journal=store();val radio=Radio();var session=core(journal,radio).openExplicit();session.discover();session.startExplicit(data.operation)
        radio.fail=true;assertThrows(IllegalStateException::class.java){session.stopExplicit(data.operation)}
        assertNotNull(journal.read().getValue(data.operation).stop)
        radio.fail=false;session=core(journal,radio).openExplicit();session.discover()
        assertThrows(IllegalStateException::class.java){session.stopExplicit(data.operation)}
        assertEquals(1,radio.writes.count { it.command==0x42 })
    }
    @Test fun newBootCannotAdoptOldOperationOrAuthorizeAnotherStart() {
        val journal=store();val radio=Radio();var session=core(journal,radio).openExplicit();session.discover();session.startExplicit(data.operation);session.close()
        radio.changedBoot=true;radio.connection=radio.connection.copy(epoch=UUID(7,8));session=core(journal,radio).openExplicit()
        assertEquals(LongRecordingOutcome.BOOT_CHANGED,session.discover().outcome)
        val count=radio.writes.size
        assertEquals(LongRecordingOutcome.BOOT_CHANGED,session.reconcile(data.operation).outcome)
        assertEquals(count,radio.writes.size)
        assertThrows(IllegalStateException::class.java){session.startExplicit(UUID(8,9))}
        assertEquals(1,radio.writes.count { it.command==0x41 })
    }
    @Test fun cancellationOrDeadlineDuringDiskBarrierRetainsIntentWithoutStartWrite() {
        var cancel=false
        val journal=store { if(it=="readback")cancel=true };val radio=Radio();val session=core(journal,radio).openExplicit();session.discover()
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation){cancel}}
        assertTrue(journal.read().containsKey(data.operation));assertEquals(listOf(0x40),radio.writes.map { it.command })
    }
    @Test fun originalDeadlineIncludesPersistentIntentAndLateCallbackCannotCommit() {
        val journal=store { if(it=="readback")now+=4000 };val radio=Radio();val session=core(journal,radio).openExplicit();session.discover()
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation)}
        assertTrue(journal.read().containsKey(data.operation));assertEquals(1,radio.writes.size)
    }
    @Test fun staleConnectionReplyLeavesUnknownAndFailedCloseKeepsSharedGate() {
        val journal=store();val radio=Radio();val core=core(journal,radio);val session=core.openExplicit();session.discover()
        radio.after={radio.connection=radio.connection.copy(epoch=UUID(9,10))};radio.closeKnown=false
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation)}
        assertNull(journal.read().getValue(data.operation).latest)
        assertFalse(radio.gate.isIdle());assertThrows(DurableTransportBusyException::class.java){core.openExplicit()}
    }
    @Test fun unsupportedWrongBondOrCaptureOwnershipRejectBeforeAnyRequest() {
        val journal=store();val radio=Radio();val core=core(journal,radio)
        radio.connection=radio.connection.copy(capabilityBits=32)
        assertThrows(IllegalArgumentException::class.java){core.openExplicit()}
        radio.connection=radio.connection.copy(capabilityBits=64,bondAddress="12:34:56:78:9A:BD")
        assertThrows(IllegalArgumentException::class.java){core.openExplicit()}
        radio.connection=radio.connection.copy(bondAddress=data.binding.bondAddress)
        val capture=radio.gate.acquire(radio.connection.epoch)
        assertThrows(DurableTransportBusyException::class.java){core.openExplicit()};assertTrue(capture.retire())
        assertTrue(radio.writes.isEmpty())
    }
    @Test fun backupAuthorityFailureNeverCreatesStartIntent() {
        val journal=store();val radio=Radio();val session=core(journal,radio).openExplicit();session.discover();denyAuthority=true
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation)}
        assertTrue(journal.read().isEmpty());assertEquals(1,radio.writes.size)
    }
    @Test fun lateResponseCannotPersistRunningOrClaimCompletedCallbacks() {
        val journal=store();val radio=Radio();val session=core(journal,radio).openExplicit();session.discover()
        radio.after={now+=4000}
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation)}
        assertNull(journal.read().getValue(data.operation).latest);assertEquals(1,radio.writes.count { it.command==0x41 })
        assertTrue(radio.gate.isIdle()) // Actual fake closure proof, not timeout itself.
    }
    @Test fun authorityRevokedAfterIntentReadbackNeverSendsStart() {
        val journal=store { if(it=="readback")denyAuthority=true };val radio=Radio();val session=core(journal,radio).openExplicit();session.discover()
        assertThrows(IllegalStateException::class.java){session.startExplicit(data.operation)}
        assertTrue(journal.read().containsKey(data.operation));assertEquals(1,radio.writes.size)
    }
    @Test fun ownerCanExplicitlyRetryLostStopAfterExactRunningReconciliation() {
        val journal=store();val radio=Radio();var session=core(journal,radio).openExplicit()
        session.discover();session.startExplicit(data.operation);radio.fail=true
        assertThrows(IllegalStateException::class.java){session.stopExplicit(data.operation)}
        radio.fail=false;radio.connection=radio.connection.copy(epoch=UUID(19,20))
        session=core(journal,radio).openExplicit();session.discover();session.reconcile(data.operation)
        assertEquals(1,radio.writes.count{it.command==0x42})
        session.stopExplicit(data.operation)
        assertEquals(2,radio.writes.count{it.command==0x42})
        assertEquals(1,radio.writes.count{it.command==0x41})
        assertTrue(journal.read().getValue(data.operation).unresolved)
    }
    @Test fun liveCounterRegressionIsRejectedEvenWhenProgressWasNotAppended() {
        val journal=store();val radio=Radio();val session=core(journal,radio).openExplicit()
        session.discover();session.startExplicit(data.operation);session.reconcile(data.operation)
        radio.transform={java.nio.ByteBuffer.wrap(it).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(65,600)}
        session.reconcile(data.operation)
        radio.transform={java.nio.ByteBuffer.wrap(it).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(65,550)}
        assertThrows(IllegalStateException::class.java){session.reconcile(data.operation)}
        assertTrue(journal.read().getValue(data.operation).unresolved)
    }
    @Test fun transportFailureRetainsDiagnosticCauseWithoutRetryOrStop() {
        val journal=store();val radio=Radio();val session=core(journal,radio).openExplicit()
        session.discover();session.startExplicit(data.operation);radio.fail=true
        val error=assertThrows(IllegalStateException::class.java){session.reconcile(data.operation)}
        assertEquals("lost ACK",error.cause?.message)
        assertTrue(radio.gate.isIdle());assertTrue(journal.read().getValue(data.operation).unresolved)
        assertEquals(1,radio.writes.count{it.command==0x41});assertEquals(0,radio.writes.count{it.command==0x42})
    }
    @Test fun physicalRecordingCanBeStoppedWithoutCreatingOrSendingAStart() {
        val journal=store();val radio=Radio();radio.discovered="running"
        radio.connection=radio.connection.copy(capabilityBits=64L or LongRecordingControlCodec.STANDALONE_CAPABILITY)
        val session=core(journal,radio).openExplicit();val state=checkNotNull(session.discover().state)
        val stopped=session.stopObservedExplicit(state)
        assertEquals(LongRecordingControlCodec.Phase.STOPPING,stopped.state?.phase)
        assertEquals(listOf(0x40,0x40,0x42),radio.writes.map { it.command })
        assertTrue(radio.writes.drop(1).all { it.operation==state.operation&&it.boot==state.boot })
        assertTrue(journal.read().isEmpty());assertEquals(0,authority)
    }
    @Test fun physicalAlreadySavedDoesNotReceiveRedundantStop() {
        val journal=store();val radio=Radio();radio.discovered="running"
        radio.connection=radio.connection.copy(capabilityBits=64L or LongRecordingControlCodec.STANDALONE_CAPABILITY)
        val session=core(journal,radio).openExplicit();val state=checkNotNull(session.discover().state)
        radio.selected="stopped";assertTrue(session.stopObservedExplicit(state).state!!.terminal)
        assertEquals(listOf(0x40,0x40),radio.writes.map { it.command });assertTrue(journal.read().isEmpty())
    }
    @Test fun physicalStopRefusesChangedIdentityBeforeStopWrite() {
        val journal=store();val radio=Radio();radio.discovered="running"
        radio.connection=radio.connection.copy(capabilityBits=64L or LongRecordingControlCodec.STANDALONE_CAPABILITY)
        val session=core(journal,radio).openExplicit();val state=checkNotNull(session.discover().state)
        radio.transform={LongRecordingControlCodec.uuid(UUID(99,100)).copyInto(it,25)}
        assertThrows(IllegalStateException::class.java){session.stopObservedExplicit(state)}
        assertEquals(listOf(0x40,0x40),radio.writes.map { it.command });assertTrue(journal.read().isEmpty())
    }
    @Test fun physicalStopDoesNotRetryOnLostReplyOrSendStartOnReconnect() {
        val journal=store();val radio=Radio();radio.discovered="running"
        radio.connection=radio.connection.copy(capabilityBits=64L or LongRecordingControlCodec.STANDALONE_CAPABILITY)
        var session=core(journal,radio).openExplicit();val state=checkNotNull(session.discover().state)
        radio.after={if(radio.writes.last().command==0x40)radio.fail=true}
        assertThrows(IllegalStateException::class.java){session.stopObservedExplicit(state)}
        radio.after={};radio.fail=false;session=core(journal,radio).openExplicit();session.discover()
        assertEquals(1,radio.writes.count { it.command==0x42 });assertEquals(0,radio.writes.count { it.command==0x41 })
        assertTrue(journal.read().isEmpty())
    }
    @Test fun oldFirmwareCannotUseObservedStopPath() {
        val journal=store();val radio=Radio();radio.discovered="running"
        val session=core(journal,radio).openExplicit();val state=checkNotNull(session.discover().state)
        assertThrows(IllegalStateException::class.java){session.stopObservedExplicit(state)}
        assertEquals(listOf(0x40),radio.writes.map { it.command })
    }
}
