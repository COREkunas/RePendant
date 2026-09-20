package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class RecordingListPresentationTest {
    private val view=RecordingListPresentation
    private val volume=RecordingVolume(UUID(1,2),UUID(3,4),1)
    private fun row(id:Long=7,frames:List<Int> = listOf(500,500,500,500,500,458),state:Int=1):RecordingSyncSnapshot {
        val recording=DurableRecordingId(volume,UUID(5,id))
        val parts=frames.mapIndexed { index,n -> SegmentIdentity(recording,index,"ab".repeat(32),357L+n*68L) }
        return RecordingSyncSnapshot(recording,manifest=RecordingManifest(recording,parts.size*4L+state,state!=0,"cd".repeat(32),parts),
            pendantCopy=PendantCopy.PRESENT,phoneSegments=parts.toSet())
    }
    private fun deletion(row:RecordingSyncSnapshot,phone:Boolean=false,remote:PendantDeletion=PendantDeletion.PENDING)=
        row.copy(deletions=listOf(RecordingDeletionIntent(UUID(9,10),row.recording,row.manifest!!.sha256,
            DeleteLocation.BOTH,true,phone,remote,if(remote==PendantDeletion.CONFIRMED)99 else null)))
    @Test fun realMinuteTailDurationComesFromCanonicalMetadataWithoutDecrypting() {
        assertEquals(59160L,view.audioMillis(row()))
        assertEquals("59.2 sec",view.duration(row()))
        assertEquals("Recording · 59.2 sec",view.title(row()))
        assertEquals("1:00",view.duration(row(frames=List(6){500})))
        assertEquals("10 sec",view.duration(row(frames=listOf(500))))
        assertEquals("20 ms",view.duration(row(frames=listOf(1))))
    }
    @Test fun hourLongFullVolumeManifestHasCorrectDuration() {
        assertEquals(3_600_000L,view.audioMillis(row(frames=List(360){500})))
        assertEquals("60:00",view.duration(row(frames=List(360){500})))
    }
    @Test fun noncanonicalGeometryAndUnknownManifestsNeverGetGuessedDurations() {
        val r=row(frames=listOf(500));val part=r.manifest!!.segments.single()
        for(bytes in listOf(1L,424L,426L,34358L,Long.MAX_VALUE)) {
            val bad=RecordingManifest(r.recording,5,true,"cd".repeat(32),listOf(part.copy(byteCount=bytes)))
            assertNull(view.audioMillis(r.copy(manifest=bad)))
        }
        assertNull(view.audioMillis(r.copy(manifest=null)))
        assertNull(view.audioMillis(r.copy(manifest=RecordingManifest(r.recording,1,true,"cd".repeat(32),listOf(part)))))
        assertNull(view.audioMillis(r.copy(manifest=RecordingManifest(r.recording,7,true,"cd".repeat(32),listOf(part)))))
    }
    @Test fun copyLocationsStayIndependentAndPendingDeletionIsNotAbsence() {
        val r=row()
        assertEquals("Phone · Saved",view.phoneLabel(r))
        assertEquals("Pendant · Saved",view.pendantLabel(r))
        val phoneOnly=r.copy(pendantCopy=PendantCopy.DELETED)
        assertFalse(view.history(phoneOnly));assertTrue(view.matches(phoneOnly,RecordingListFilter.PHONE))
        assertFalse(view.matches(phoneOnly,RecordingListFilter.PENDANT))
        assertEquals("Pendant · Removed",view.pendantLabel(phoneOnly))
        val pendantOnly=r.copy(phoneSegments=emptySet())
        assertEquals("Phone · Not downloaded",view.phoneLabel(pendantOnly))
        assertTrue(view.matches(pendantOnly,RecordingListFilter.PENDANT))
        assertTrue(view.matches(pendantOnly,RecordingListFilter.NEEDS_SYNC))
        assertEquals("Pendant · Delete queued",view.pendantLabel(deletion(r)))
        assertTrue(view.matches(deletion(r),RecordingListFilter.PENDANT))
        assertEquals("Phone · Delete pending",view.phoneLabel(deletion(r,phone=true)))
    }
    @Test fun partialAndSuppressedCopiesAreNotAdvertisedAsSaved() {
        val r=row();val partial=r.copy(phoneSegments=r.phoneSegments.take(2).toSet())
        assertEquals("Phone · 2/6 parts",view.phoneLabel(partial))
        assertTrue(view.matches(partial,RecordingListFilter.PHONE));assertTrue(view.needsSync(partial))
        val removed=r.copy(phoneSegments=emptySet(),downloadSuppressed=true)
        assertEquals("Phone · Removed",view.phoneLabel(removed));assertFalse(view.needsSync(removed))
        assertEquals("Pendant · Not checked",view.pendantLabel(r.copy(pendantCopy=PendantCopy.UNKNOWN)))
        assertEquals("Pendant · Old storage",view.pendantLabel(r.copy(staleVolume=true)))
        assertFalse(view.matches(r.copy(staleVolume=true),RecordingListFilter.PENDANT))
    }
    @Test fun emptyAndFullyRemovedHistoryIsHiddenButPendingWorkIsNot() {
        val empty=row(frames=emptyList(),state=2)
        val removed=row().copy(phoneSegments=emptySet(),pendantCopy=PendantCopy.DELETED)
        for(r in listOf(empty,removed)) {
            assertTrue(view.history(r))
            assertFalse(view.matches(r,RecordingListFilter.ALL))
        }
        assertTrue(view.matches(empty,RecordingListFilter.HISTORY))
        assertTrue(view.fullyDeleted(removed))
        RecordingListFilter.entries.forEach { assertFalse(view.matches(removed,it)) }
        assertEquals("Empty recording",view.title(empty))
        assertFalse(view.history(deletion(empty)))
        assertTrue(view.matches(deletion(empty),RecordingListFilter.NEEDS_SYNC))
        assertFalse(view.history(empty.copy(staleVolume=true)))
        assertFalse(view.history(empty.copy(phoneSegments=row().phoneSegments)))
        assertFalse(view.history(row(state=0,frames=emptyList())))
    }
    @Test fun deletionDisappearsOnlyWhenBothLocationsAreConfirmedEmpty() {
        val r=row()
        val phoneRemoved=deletion(r.copy(phoneSegments=emptySet()),remote=PendantDeletion.PENDING)
        assertFalse(view.fullyDeleted(phoneRemoved));assertTrue(view.matches(phoneRemoved,RecordingListFilter.ALL))
        val remoteRemoved=r.copy(pendantCopy=PendantCopy.DELETED)
        assertFalse(view.fullyDeleted(remoteRemoved));assertTrue(view.matches(remoteRemoved,RecordingListFilter.PHONE))
        val removed=remoteRemoved.copy(phoneSegments=emptySet())
        assertTrue(view.fullyDeleted(removed))
        assertFalse(view.fullyDeleted(removed.copy(pendantCopy=PendantCopy.UNKNOWN)))
        assertFalse(view.fullyDeleted(removed.copy(staleVolume=true)))
        assertFalse(view.fullyDeleted(removed.copy(pendingReceipts=r.phoneSegments)))
        assertFalse(view.fullyDeleted(deletion(removed,phone=true)))
    }
    @Test fun interruptedAudioIsPlayableInListAndNotDiscardedIntoHistory() {
        val recovered=row(frames=listOf(500),state=2)
        assertFalse(view.history(recovered));assertEquals("Recovered recording · 10 sec",view.title(recovered))
        assertEquals("Recording in progress",view.title(row(state=0)))
    }
    @Test fun retiredEmptyRowsDisappearFromEveryFilterWithoutClaimingRemoteDeletion() {
        for (copy in PendantCopy.entries) {
            val old=row().copy(staleVolume=true,pendantCopy=copy,phoneSegments=emptySet())
            assertTrue(view.retiredEmpty(old));assertFalse(view.fullyDeleted(old))
            val before=old.copy()
            RecordingListFilter.entries.forEach { assertFalse(view.matches(old,it)) }
            assertEquals(before,old)
        }
        val unknown=row().copy(phoneSegments=emptySet(),pendantCopy=PendantCopy.UNKNOWN)
        assertFalse(view.retiredEmpty(unknown));assertTrue(view.matches(unknown,RecordingListFilter.ALL))
    }
    @Test fun retiredFilesAndUnfinishedCleanupRemainVisible() {
        val old=row().copy(staleVolume=true,pendantCopy=PendantCopy.UNKNOWN)
        assertTrue(view.hasPhoneCopy(old));assertFalse(view.phoneComplete(old))
        assertTrue(view.phoneLabel(old).contains("Old storage copy"))
        assertTrue(view.matches(old,RecordingListFilter.PHONE))
        val empty=old.copy(phoneSegments=emptySet())
        val waiting=listOf(deletion(empty,phone=true,remote=PendantDeletion.STALE_GENERATION),
            deletion(empty,remote=PendantDeletion.PENDING),empty.copy(pendingReceipts=old.phoneSegments))
        for(row in waiting) {
            assertFalse(view.retiredEmpty(row));assertTrue(view.matches(row,RecordingListFilter.ALL))
            assertTrue(view.matches(row,RecordingListFilter.NEEDS_SYNC))
        }
        assertTrue(view.retiredEmpty(deletion(empty,remote=PendantDeletion.STALE_GENERATION)))
    }
    @Test fun oldClipsDoNotAcquireFakeDatesAndResyncDoesNotRewriteFirstSync() {
        val r=row();val t=1789653600000L
        assertTrue(view.newSyncTimes(listOf(r),listOf(r),emptyMap(),t).isEmpty())
        assertEquals(mapOf(r.recording to t),view.newSyncTimes(emptyList(),listOf(r),emptyMap(),t))
        assertTrue(view.newSyncTimes(emptyList(),listOf(r),mapOf(r.recording to t),t+10).isEmpty())
        assertTrue(view.newSyncTimes(emptyList(),listOf(r),emptyMap(),0).isEmpty())
        assertTrue(view.newSyncTimes(emptyList(),listOf(r.copy(pendingReceipts=r.phoneSegments)),emptyMap(),t).isEmpty())
        assertTrue(view.newSyncTimes(emptyList(),listOf(row(frames=emptyList())),emptyMap(),t).isEmpty())
        val partial=r.copy(phoneSegments=r.phoneSegments.take(2).toSet())
        assertEquals(mapOf(r.recording to t),view.newSyncTimes(listOf(partial),listOf(r),emptyMap(),t))
    }
    @Test fun sortingUsesKnownSyncTimeThenStableIdNeverClaimsUnknownDatesAreRecent() {
        val a=row(id=1);val b=row(id=2);val c=row(id=3)
        assertEquals(listOf(c,a,b),view.ordered(listOf(b,c,a),mapOf(c.recording to 1789653600000)))
        assertEquals(listOf(b,c,a),view.ordered(listOf(a,c,b),mapOf(c.recording to 100L,b.recording to 101L)))
        assertEquals(listOf(a,b,c),view.ordered(listOf(c,b,a),emptyMap()))
    }
}
