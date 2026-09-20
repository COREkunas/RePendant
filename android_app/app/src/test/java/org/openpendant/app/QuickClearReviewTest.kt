package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class QuickClearReviewTest {
    private val volume=RecordingVolume(UUID(1,2),UUID(3,4),2)
    private fun row(n:Long):RecordingSyncSnapshot {
        val id=DurableRecordingId(volume,UUID(5,n));val segment=SegmentIdentity(id,0,"ab".repeat(32),425)
        return RecordingSyncSnapshot(id,manifest=RecordingManifest(id,5,true,"cd".repeat(32),listOf(segment)),
            pendantCopy=PendantCopy.PRESENT,phoneSegments=setOf(segment))
    }
    private fun entry(r:RecordingSyncSnapshot)=r.manifest!!.let { DurableCatalogEntry(r.recording,it.revision,it.sha256,it.segments.size,it.finished) }
    @Test fun allPagesIncludedAndPhoneCleanupIsExplicitWithLocalOnlyCopies() {
        val remote=(1L..32L).map(::row);val local=row(40).copy(pendantCopy=PendantCopy.DELETED)
        val review=QuickClearReview.create(volume,remote.map(::entry),remote+local)
        assertEquals(32,review.pendantCount);assertEquals(33,review.phoneCount)
        assertEquals(0,review.plan(false).phoneCount);assertEquals(32,review.plan(false).targets.size)
        val both=review.plan(true);assertEquals(33,both.targets.size);assertEquals(32,both.pendantCount)
        assertEquals(DeleteLocation.PHONE_ONLY,both.targets.last().location)
        both.validate(remote+local+row(41),volume);assertFalse(both.targets.any { it.recording==row(41).recording })
    }
    @Test fun noCatalogOmissionOrPendingDeletionCanPretendToBeCleared() {
        val r=row(1)
        assertThrows(IllegalArgumentException::class.java){QuickClearReview.create(volume,emptyList(),listOf(r))}
        val pending=r.copy(deletions=listOf(RecordingDeletionIntent(UUID(8,9),r.recording,r.manifest!!.sha256,
            DeleteLocation.PENDANT_ONLY,false,false,PendantDeletion.PENDING)))
        assertThrows(IllegalArgumentException::class.java){QuickClearReview.create(volume,listOf(entry(r)),listOf(pending))}
        assertThrows(IllegalArgumentException::class.java){QuickClearReview.create(volume,listOf(entry(r)),listOf(r.copy(staleVolume=true)))}
    }
    @Test fun activeWrongManifestDuplicateVolumeAndOversizedReviewsRefuse() {
        val r=row(1);val e=entry(r)
        for(wrong in listOf(e.copy(finished=false),e.copy(manifestSha256="ef".repeat(32)),
            e.copy(manifestRevision=9),e.copy(sealedSegments=0),e.copy(recording=r.recording.copy(volume=volume.copy(generation=3)))))
            assertThrows(IllegalArgumentException::class.java){QuickClearReview.create(volume,listOf(wrong),listOf(r))}
        assertThrows(IllegalArgumentException::class.java){QuickClearReview.create(volume,listOf(e,e),listOf(r))}
        assertThrows(IllegalArgumentException::class.java){QuickClearReview.create(volume,listOf(e),listOf(r,r))}
        val large=(1L..33L).map(::row)
        assertThrows(IllegalArgumentException::class.java){QuickClearReview.create(volume,large.map(::entry),large)}
    }
    @Test fun emptyPendantCanKeepOrExplicitlyRemoveOnlyCurrentPhoneCopies() {
        val local=row(1).copy(pendantCopy=PendantCopy.DELETED)
        val old=row(2).let { it.copy(recording=it.recording.copy(volume=volume.copy(generation=1)),staleVolume=true) }
        val review=QuickClearReview.create(volume,emptyList(),listOf(local,old))
        assertTrue(review.plan(false).targets.isEmpty());assertEquals(0,review.pendantCount)
        assertEquals(1,review.plan(true).targets.size);assertEquals(DeleteLocation.PHONE_ONLY,review.plan(true).targets.single().location)
    }
    @Test fun selectionIsFrozenAndFullPreflightRejectsChangedRows() {
        val rows=mutableListOf(row(1));val entries=mutableListOf(entry(rows[0]))
        val review=QuickClearReview.create(volume,entries,rows);val plan=review.plan(false)
        entries.clear();rows.add(row(2));assertEquals(1,review.pendantCount);assertEquals(1,plan.targets.size)
        assertThrows(IllegalArgumentException::class.java){plan.validate(listOf(rows[0].copy(revision=1)),volume)}
        assertThrows(IllegalArgumentException::class.java){plan.validate(rows,volume.copy(generation=3))}
    }
}
