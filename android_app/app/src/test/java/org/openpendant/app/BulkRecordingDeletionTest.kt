package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class BulkRecordingDeletionTest {
    private val volume=RecordingVolume(UUID(1,2),UUID(3,4),1)
    private fun row(n:Long):RecordingSyncSnapshot {
        val id=DurableRecordingId(volume,UUID(5,n))
        val part=SegmentIdentity(id,0,"ab".repeat(32),425)
        return RecordingSyncSnapshot(id,manifest=RecordingManifest(id,5,true,"cd".repeat(32),listOf(part)),
            pendantCopy=PendantCopy.PRESENT,phoneSegments=setOf(part))
    }
    private fun pending(r:RecordingSyncSnapshot)=r.copy(deletions=listOf(RecordingDeletionIntent(UUID(8,9),
        r.recording,r.manifest!!.sha256,DeleteLocation.BOTH,false,true,PendantDeletion.PENDING)))
    @Test fun allMatchesAreSelectedBeyondFirstPageAndLaterMatchesAreNeverAdded() {
        val rows=(1L..41L).map(::row)
        val plan=BulkRecordingDeletion.create(rows,volume,DeleteLocation.BOTH)
        assertEquals(41,plan.targets.size);assertEquals(41,plan.phoneCount);assertEquals(41,plan.pendantCount)
        plan.validate(rows+row(42),volume)
        assertEquals(rows.map { it.recording },plan.targets.map { it.recording })
        val filtered=BulkRecordingDeletion.create(listOf(rows[3],rows[20]),volume,DeleteLocation.PHONE_ONLY)
        filtered.validate(rows,volume)
        assertEquals(listOf(rows[3].recording,rows[20].recording),filtered.targets.map { it.recording })
    }
    @Test fun partialCopiesCountButUnavailableScopeAndPendingOrStaleRowsAreSkipped() {
        val all=row(1);val remoteOnly=row(2).copy(phoneSegments=emptySet())
        val localOnly=row(3).copy(pendantCopy=PendantCopy.DELETED)
        val active=row(4).let { it.copy(manifest=RecordingManifest(it.recording,4,false,it.manifest!!.sha256,it.manifest.segments)) }
        val stale=row(5).copy(staleVolume=true)
        val old=row(6).let { it.copy(recording=it.recording.copy(volume=volume.copy(generation=2))) }
        val rows=listOf(all,remoteOnly,localOnly,active,stale,old,pending(row(7)))
        val phone=BulkRecordingDeletion.create(rows,volume,DeleteLocation.PHONE_ONLY)
        assertEquals(listOf(all.recording,localOnly.recording),phone.targets.map { it.recording })
        assertEquals(5,phone.skippedCount)
        val pendant=BulkRecordingDeletion.create(rows,volume,DeleteLocation.PENDANT_ONLY)
        assertEquals(listOf(all.recording,remoteOnly.recording),pendant.targets.map { it.recording })
        val both=BulkRecordingDeletion.create(rows,volume,DeleteLocation.BOTH)
        assertEquals(3,both.targets.size)
        assertEquals(DeleteLocation.PHONE_ONLY,both.targets.last().location) // Already removed remotely.
        assertEquals(2,both.pendantCount)
    }
    @Test fun anyChangedSelectedIdentityStopsPreflightBeforeWrites() {
        val rows=listOf(row(1),row(2));val plan=BulkRecordingDeletion.create(rows,volume,DeleteLocation.BOTH)
        for(changed in listOf(rows.take(1),listOf(rows[0],rows[1].copy(revision=1)),
            listOf(rows[0],pending(rows[1])),listOf(rows[0],rows[1].copy(staleVolume=true)))) {
            assertThrows(IllegalArgumentException::class.java){plan.validate(changed,volume)}
        }
        assertThrows(IllegalArgumentException::class.java){plan.validate(rows,volume.copy(generation=2))}
        val otherManifest=rows[0].manifest!!.let { RecordingManifest(it.recording,it.revision,true,"ef".repeat(32),it.segments) }
        assertThrows(IllegalArgumentException::class.java){plan.validate(listOf(rows[0].copy(manifest=otherManifest),rows[1]),volume)}
    }
    @Test fun retiredVolumeAllowsOnlyLocalCopiesAndNeverRetargetsPendantDeletion() {
        val current=volume.copy(generation=2)
        val old=row(1).copy(staleVolume=true,pendantCopy=PendantCopy.UNKNOWN)
        val empty=old.copy(recording=old.recording.copy(recordingId=UUID(5,2)),phoneSegments=emptySet())
        for(location in listOf(DeleteLocation.PHONE_ONLY,DeleteLocation.BOTH)) {
            val plan=BulkRecordingDeletion.create(listOf(old,empty),current,location)
            assertEquals(1,plan.targets.size);assertEquals(1,plan.phoneCount);assertEquals(0,plan.pendantCount)
            assertEquals(old.recording,plan.targets.single().recording)
            assertEquals(DeleteLocation.PHONE_ONLY,plan.targets.single().location)
            plan.validate(listOf(old,empty),current)
            assertThrows(IllegalArgumentException::class.java) { plan.validate(listOf(old.copy(staleVolume=false),empty),current) }
        }
        assertTrue(BulkRecordingDeletion.create(listOf(old),current,DeleteLocation.PENDANT_ONLY).targets.isEmpty())
        assertTrue(BulkRecordingDeletion.create(listOf(old),current.copy(deviceId=UUID(99,99)),DeleteLocation.BOTH).targets.isEmpty())
        assertFalse(RecordingDeletionScope.allowed(old,current,DeleteLocation.BOTH))
        assertTrue(RecordingDeletionScope.allowed(old,current,DeleteLocation.PHONE_ONLY))
        assertFalse(RecordingDeletionScope.allowed(old,volume,DeleteLocation.PHONE_ONLY))
    }
    @Test fun retiredPhoneDeletionPersistsSuppressionAndThenHidesTheEmptyRow() {
        val current=volume.copy(generation=2)
        val initial=row(1)
        var saved=initial
        val core=RecordingSyncContract(initial,RecordingSyncOwnership()) { revision,next ->
            assertEquals(saved.revision,revision);saved=next
        }
        try {
            core.authenticatedConnection(current,true)
            val old=core.snapshot()
            val plan=BulkRecordingDeletion.create(listOf(old),current,DeleteLocation.BOTH)
            assertEquals(DeleteLocation.PHONE_ONLY,plan.targets.single().location)
            val intent=core.requestDeletion(UUID.randomUUID(),plan.targets.single().location,false)
            assertTrue(viewMatches(core.snapshot()))
            assertEquals(PendantDeletion.NOT_REQUESTED,intent.pendant)
            assertTrue(core.performPhoneDeletion(intent.operationId,{},{}))
            assertTrue(saved.staleVolume);assertTrue(saved.downloadSuppressed)
            assertEquals(PendantCopy.UNKNOWN,saved.pendantCopy)
            assertEquals(1,saved.deletions.size)
            assertNull(core.nextPendantDeletion())
            assertTrue(RecordingListPresentation.retiredEmpty(saved))
            assertFalse(viewMatches(saved))
            assertEquals(saved,RecordingSyncSnapshotCodec.decode(RecordingSyncSnapshotCodec.encode(saved)))
        } finally { core.close() }
    }
    private fun viewMatches(row:RecordingSyncSnapshot)=RecordingListPresentation.matches(row,RecordingListFilter.ALL)
    @Test fun retiredPendingBothCanOnlyResumeItsPreviouslyAuthorizedLocalWork() {
        val initial=row(1)
        val core=RecordingSyncContract(initial,RecordingSyncOwnership()) { _,_-> }
        try {
            val intent=core.requestDeletion(UUID.randomUUID(),DeleteLocation.BOTH,false)
            val current=volume.copy(generation=2)
            core.authenticatedConnection(current,true)
            val old=core.snapshot()
            assertEquals(PendantDeletion.STALE_GENERATION,old.deletions.single().pendant)
            assertTrue(RecordingDeletionScope.allowed(old,current,DeleteLocation.BOTH))
            assertFalse(RecordingDeletionScope.allowed(old,current,DeleteLocation.PENDANT_ONLY))
            assertTrue(BulkRecordingDeletion.create(listOf(old),current,DeleteLocation.BOTH).targets.isEmpty())
            assertTrue(core.performPhoneDeletion(intent.operationId,{},{}))
            assertFalse(RecordingDeletionScope.allowed(core.snapshot(),current,DeleteLocation.BOTH))
            assertNull(core.nextPendantDeletion())
            assertTrue(RecordingListPresentation.retiredEmpty(core.snapshot()))
        } finally { core.close() }
    }
    @Test fun duplicateAndOversizedSelectionsAreRejectedAndNothingToDeleteIsEmpty() {
        assertThrows(IllegalArgumentException::class.java){BulkRecordingDeletion.create(listOf(row(1),row(1)),volume,DeleteLocation.BOTH)}
        assertThrows(IllegalArgumentException::class.java){BulkRecordingDeletion.create((1L..129L).map(::row),volume,DeleteLocation.BOTH)}
        val removed=row(1).copy(pendantCopy=PendantCopy.DELETED,phoneSegments=emptySet())
        val plan=BulkRecordingDeletion.create(listOf(removed),volume,DeleteLocation.BOTH)
        assertTrue(plan.targets.isEmpty());assertEquals(1,plan.skippedCount)
    }
    @Test fun sequentialCoreIntentsKeepOtherRowsAndTombstonesAfterLastCopyDisappears() {
        val original=(1L..3L).map(::row)
        val stored=original.associateBy { it.recording }.toMutableMap()
        val plan=BulkRecordingDeletion.create(original.take(2),volume,DeleteLocation.BOTH)
        plan.validate(stored.values.toList(),volume)
        for(target in plan.targets) {
            val core=RecordingSyncContract(stored.getValue(target.recording),RecordingSyncOwnership()) { rev,next ->
                assertEquals(stored.getValue(target.recording).revision,rev);stored[target.recording]=next
            }
            try {
                val intent=core.requestDeletion(UUID.randomUUID(),target.location)
                assertTrue(core.performPhoneDeletion(intent.operationId,{},{}))
                assertTrue(RecordingListPresentation.matches(core.snapshot(),RecordingListFilter.ALL))
                core.authenticatedConnection(volume,true)
                val request=core.nextPendantDeletion()!!
                assertTrue(core.confirmPendantDeletion(request,6,target.manifest.sha256))
                assertTrue(RecordingListPresentation.fullyDeleted(core.snapshot()))
                assertTrue(core.snapshot().downloadSuppressed)
                assertEquals(1,core.snapshot().deletions.size) // Retained, not physically dropped from DB.
            } finally {core.close()}
        }
        assertEquals(original[2],stored.getValue(original[2].recording))
        assertEquals(1,stored.values.count { RecordingListPresentation.matches(it,RecordingListFilter.ALL) })
    }
}
