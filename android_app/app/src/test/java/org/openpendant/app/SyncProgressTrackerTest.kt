package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class SyncProgressTrackerTest {
    private var now = 100L
    private val id = DurableRecordingId(RecordingVolume(UUID(1,2), UUID(3,4),2),UUID(5,6))
    private val parts = (0..2).map { SegmentIdentity(id,it,"ab".repeat(32),34357) }
    private val manifest = RecordingManifest(id,13,true,"cd".repeat(32),parts)
    private fun row(saved: Set<SegmentIdentity> = emptySet()) = RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=saved)
    @Test fun nothingIsClaimedBeforeManifest() { assertNull(SyncProgressTracker { now }.snapshot()) }
    @Test fun rateNeedsEnoughEvidenceAndNetworkBytesAreNotDurableProgress() {
        val meter=SyncProgressTracker { now };meter.recording(row(),1,2)
        meter.received(4096);assertNull(meter.snapshot()!!.bytesPerSecond)
        now+=2000
        assertEquals(2048L,meter.snapshot()!!.bytesPerSecond)
        assertEquals(0L,meter.snapshot()!!.checkpointBytes)
        assertEquals(51L,meter.snapshot()!!.remainingSeconds)
        assertTrue(meter.snapshot()!!.text.contains("for this recording"))
    }
    @Test fun checkpointsResumeAndDoNotDoubleCount() {
        val meter=SyncProgressTracker { now };meter.recording(row(setOf(parts[0])),1,1)
        meter.checkpoint(parts[1],8192);meter.checkpoint(parts[1],8192)
        assertEquals(42549L,meter.snapshot()!!.checkpointBytes)
        assertEquals(1,meter.snapshot()!!.verifiedSegments)
        meter.recording(row(setOf(parts[0])),1,1) // Fresh reconciliation discards no data, just resets presentation.
        meter.checkpoint(parts[1],8192)
        assertEquals(42549L,meter.snapshot()!!.checkpointBytes)
        assertNull(meter.snapshot()!!.bytesPerSecond)
    }
    @Test fun verificationAndFinalReceiptAreNotConflated() {
        val meter=SyncProgressTracker { now };meter.recording(row(),1,1)
        parts.forEach { meter.checkpoint(it,it.byteCount) }
        assertEquals(100,meter.snapshot()!!.percent);assertEquals(0,meter.snapshot()!!.verifiedSegments)
        meter.published(row(parts.toSet()))
        assertEquals(3,meter.snapshot()!!.verifiedSegments)
        assertNull(meter.snapshot()!!.remainingSeconds)
        assertTrue(meter.snapshot()!!.text.contains("receipt acknowledgements"))
    }
    @Test fun malformedOrBackwardOffsetsAreRejected() {
        val meter=SyncProgressTracker { now };meter.recording(row(),1,1);meter.checkpoint(parts[0],4096)
        for(offset in listOf(-1L,0L,34358L))assertThrows(IllegalArgumentException::class.java){meter.checkpoint(parts[0],offset)}
        assertThrows(IllegalArgumentException::class.java){meter.checkpoint(parts[0].copy(sha256="ef".repeat(32)),5000)}
    }
    @Test fun backwardClockDisablesEstimateUntilNextManifest() {
        val meter=SyncProgressTracker { now };meter.recording(row(),1,1);meter.received(4096)
        now+=2000;assertNotNull(meter.snapshot()!!.bytesPerSecond)
        now--;assertNull(meter.snapshot()!!.bytesPerSecond)
        now+=3000;assertNull(meter.snapshot()!!.bytesPerSecond)
    }

    @Test fun alreadySettledCopyShowsLibraryCheckNotFinishingOrSpeedEstimate() {
        val meter=SyncProgressTracker { now }
        meter.recording(row(parts.toSet()).copy(pendantCopy=PendantCopy.PRESENT),7,23)
        val progress=checkNotNull(meter.snapshot())
        assertTrue(progress.checkingSavedCopy);assertEquals(100,progress.percent)
        assertTrue(progress.text.contains("Already saved on phone"))
        assertTrue(progress.text.contains("Checking recording library"))
        assertFalse(progress.text.contains("Finishing"));assertFalse(progress.text.contains("Measuring"))
        assertNull(progress.remainingSeconds)
    }

    @Test fun pendingOrUncertainCopiesDoNotClaimAlreadySavedAndSettled() {
        val complete=row(parts.toSet()).copy(pendantCopy=PendantCopy.PRESENT)
        val deletion=RecordingDeletionIntent(UUID(11,12),id,manifest.sha256,DeleteLocation.PHONE_ONLY,false,false,PendantDeletion.NOT_REQUESTED)
        for(value in listOf(complete.copy(pendingReceipts=setOf(parts.last())),
            complete.copy(phoneSegments=setOf(parts.first())),complete.copy(staleVolume=true),
            complete.copy(downloadSuppressed=true),complete.copy(pendantCopy=PendantCopy.UNKNOWN),
            complete.copy(pendantCopy=PendantCopy.DELETED),complete.copy(deletions=listOf(deletion)),
            complete.copy(manifest=RecordingManifest(id,12,false,manifest.sha256,parts)))) {
            val meter=SyncProgressTracker { now };meter.recording(value,1,1)
            assertFalse(meter.snapshot()!!.checkingSavedCopy)
            assertFalse(meter.snapshot()!!.text.contains("Already saved on phone"))
        }
    }

    @Test fun newTransferAndPublicationClearTheSavedCopyPresentation() {
        val complete=row(parts.toSet()).copy(pendantCopy=PendantCopy.PRESENT)
        for(mode in 0..2) {
            val meter=SyncProgressTracker { now };meter.recording(complete,1,1)
            assertTrue(meter.snapshot()!!.checkingSavedCopy)
            when(mode){0->meter.received(4096);1->meter.checkpoint(parts.first(),parts.first().byteCount)
                else->meter.published(complete)}
            assertFalse(meter.snapshot()!!.checkingSavedCopy)
            assertTrue(meter.snapshot()!!.text.contains("receipt acknowledgements"))
        }
    }

    @Test fun emptySettledRecordingDoesNotClaimDownloadOrPendingAcknowledgements() {
        val empty=RecordingManifest(id,1,true,manifest.sha256,emptyList())
        val meter=SyncProgressTracker { now }
        meter.recording(RecordingSyncSnapshot(id,manifest=empty,pendantCopy=PendantCopy.PRESENT),1,1)
        assertTrue(meter.snapshot()!!.checkingSavedCopy)
        assertTrue(meter.snapshot()!!.text.contains("Empty recording"))
        assertFalse(meter.snapshot()!!.text.contains("Finishing"))
    }

    @Test fun full5120CheckpointLookupRejectsSameIndexForeignIdentityWithoutChangingProgress() {
        val all=(0 until 5120).map { SegmentIdentity(id,it,"ab".repeat(32),34357) }
        val large=RecordingManifest(id,20481,true,"cd".repeat(32),all)
        val meter=SyncProgressTracker { now }
        meter.recording(RecordingSyncSnapshot(id,manifest=large),1,1)
        for(position in listOf(0,2559,5119)) meter.checkpoint(all[position].copy(),4096)
        assertEquals(12288L,meter.snapshot()!!.checkpointBytes)
        val last=all.last()
        val before=meter.snapshot()
        for(wrong in listOf(last.copy(sha256="ef".repeat(32)),last.copy(byteCount=34358),
            last.copy(sequence=5120),last.copy(sequence=Int.MAX_VALUE),
            last.copy(recording=id.copy(recordingId=UUID(71,72))),
            last.copy(recording=id.copy(volume=id.volume.copy(generation=3))))) {
            assertThrows(IllegalArgumentException::class.java) { meter.checkpoint(wrong,8192) }
            assertEquals(before,meter.snapshot())
        }
        val replacement=id.copy(recordingId=UUID(73,74))
        val newer=RecordingManifest(replacement,5,true,"cd".repeat(32),
            listOf(all.first().copy(recording=replacement)))
        meter.recording(RecordingSyncSnapshot(replacement,manifest=newer),1,1)
        assertThrows(IllegalArgumentException::class.java) { meter.checkpoint(all.first(),4096) }
        assertEquals(0L,meter.snapshot()!!.checkpointBytes)
    }
}
