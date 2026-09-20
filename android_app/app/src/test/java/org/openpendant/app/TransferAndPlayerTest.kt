package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class TransferAndPlayerTest {
    private val id=DurableRecordingId(RecordingVolume(UUID(1,2),UUID(3,4),1),UUID(5,6))
    private val segments=listOf(500,100).mapIndexed { i,n->SegmentIdentity(id,i,"ab".repeat(32),357L+n*68) }
    private val row=RecordingSyncSnapshot(id,manifest=RecordingManifest(id,9,true,"cd".repeat(32),segments),
        phoneSegments=segments.toSet(),pendantCopy=PendantCopy.PRESENT)
    private val peer=DurableConnectedPeer(UUID(7,8),"12:34:56:78:9A:BC",DurableSyncCapabilities(true,true,true,true))
    @Test fun cursorIgnoresStaleProgressAndPausedSeekDoesNotResume() {
        val p=RecordingPlayerControl(id,12000)
        val playing=p.snapshot()
        assertTrue(p.progress(playing.version,4000))
        val paused=p.move(playing=false)
        assertEquals(4000L,paused.positionMillis);assertFalse(paused.playing)
        assertFalse(p.progress(playing.version,6000))
        val seek=p.move(10000)
        assertFalse(seek.playing);assertEquals(10000L,seek.positionMillis)
        assertFalse(p.current(paused.version))
        assertTrue(p.move(playing=true).playing)
        assertEquals(12000L,p.move(Long.MAX_VALUE).positionMillis)
        assertEquals(0L,p.move(-1).positionMillis)
    }
    @Test fun sliderMapsWholeHourAndPartialTailWithoutRoundingToSegments() {
        assertEquals(RecordingSeekPoint(0,0),RecordingTimeline.seek(row,0))
        assertEquals(RecordingSeekPoint(0,80016),RecordingTimeline.seek(row,5001))
        assertEquals(RecordingSeekPoint(1,0),RecordingTimeline.seek(row,10000))
        assertEquals(RecordingSeekPoint(1,16000),RecordingTimeline.seek(row,11000))
        assertEquals(RecordingSeekPoint(1,31999),RecordingTimeline.seek(row,12000))
        assertThrows(IllegalArgumentException::class.java){RecordingTimeline.seek(row,12001)}
        assertEquals("1:00:00",RecordingTimeline.label(3600000));assertEquals("0:00",RecordingTimeline.label(-1))
    }
    @Test fun automationDefaultsOffAndNeverBypassesEligibilityOrForeground() {
        val p=TransferPolicy();val on=TransferPreferences(automatic=true)
        assertFalse(p.claim(TransferPreferences(),peer,true,true,0))
        assertFalse(p.claim(on,peer,false,true,0));assertFalse(p.claim(on,peer,true,false,0))
        assertFalse(p.claim(on,null,true,true,0))
        assertTrue(p.claim(on,peer,true,true,0))
        assertFalse(p.claim(on,peer,true,true,600000)) // Stop is not a retry.
        val next=peer.copy(epoch=UUID(9,10))
        assertFalse(p.claim(on,next,true,true,50000));assertTrue(p.claim(on,next,true,true,60000))
    }
    @Test fun recoveredConnectionCannotScheduleAnotherAutomaticTransfer() {
        val p=TransferPolicy();val on=TransferPreferences(automatic=true)
        assertTrue(p.claim(on,peer,true,true,0))
        val restored=peer.copy(epoch=UUID(9,10));p.attempted(restored,2000)
        assertFalse(p.claim(on,restored,true,true,120000))
    }
    @Test fun lowBatteryWaitsForSafeUsbAndNeverMigratesToAnotherBond() {
        val p=TransferPolicy();val low=TransferPreferences(lowBattery=true)
        p.observeLow(low,peer.bondAddress,26);assertFalse(p.waitingForUsb(peer.bondAddress))
        p.observeLow(low,peer.bondAddress,25);assertTrue(p.waitingForUsb(peer.bondAddress))
        p.observeLow(low,peer.bondAddress,null) // Missing gauge does not manufacture a new trigger.
        assertFalse(p.claim(low,peer,false,true,1000))
        assertFalse(p.claim(low,peer.copy(bondAddress="12:34:56:78:9A:BD"),true,true,1000))
        assertTrue(p.claim(low,peer,true,true,1000));assertFalse(p.waitingForUsb(peer.bondAddress))
        p.observeLow(low,peer.bondAddress,20);p.observeLow(TransferPreferences(),peer.bondAddress,20)
        assertFalse(p.waitingForUsb(peer.bondAddress))
    }
    @Test fun autoRemovalOnlyAdmitsNewCompleteReceiptedCurrentPhoneCopies() {
        assertTrue(TransferPolicy.removable(emptyList(),row))
        assertTrue(TransferPolicy.removable(listOf(row.copy(phoneSegments=emptySet())),row))
        assertFalse(TransferPolicy.removable(listOf(row),row))
        for(bad in listOf(row.copy(phoneSegments=segments.take(1).toSet()),row.copy(staleVolume=true),
            row.copy(downloadSuppressed=true),row.copy(pendingReceipts=segments.toSet()),row.copy(pendantCopy=PendantCopy.UNKNOWN),
            row.copy(manifest=RecordingManifest(id,8,false,row.manifest!!.sha256,segments))))assertFalse(TransferPolicy.removable(emptyList(),bad))
        val pending=RecordingDeletionIntent(UUID(9,10),id,row.manifest!!.sha256,DeleteLocation.PENDANT_ONLY,true,false,PendantDeletion.PENDING)
        assertFalse(TransferPolicy.removable(emptyList(),row.copy(deletions=listOf(pending))))
    }
    @Test fun removalMustVerifyEveryFileAndRecheckOptInBeforeSavingIntent() {
        var verified=0;var queued=0;var enabled=true
        fun run()=TransferPolicy.queueVerified(emptyList(),listOf(row),{enabled},{verified++;true}) {
            assertEquals(2,verified);queued++
        }
        assertEquals(1,run());assertEquals(1,queued)
        verified=0;queued=0;enabled=false
        assertEquals(0,run());assertEquals(0,verified);assertEquals(0,queued)
        assertThrows(IllegalStateException::class.java) {
            TransferPolicy.queueVerified(emptyList(),listOf(row),{true},{false}) { queued++ }
        }
        assertEquals(0,queued)
        enabled=true
        assertEquals(0,TransferPolicy.queueVerified(emptyList(),listOf(row),{enabled},{enabled=false;true}) { queued++ })
        assertEquals(0,queued)
    }
}
