package org.openpendant.app

import java.io.File
import java.nio.file.Files
import java.util.UUID
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class DurableLibraryStateTest {
    @get:Rule val temporary=TemporaryFolder(File("build/library-state-scratch").apply { mkdirs() })
    private val volume=RecordingVolume(UUID(1,2),UUID(3,4),7)
    private val recording=DurableRecordingId(volume,UUID(5,6))
    private val segment=SegmentIdentity(recording,0,"ab".repeat(32),425)
    private val manifest=RecordingManifest(recording,5,true,"cd".repeat(32),listOf(segment))
    private val row=RecordingSyncSnapshot(recording,manifest=manifest,pendantCopy=PendantCopy.PRESENT,phoneSegments=setOf(segment))
    private val binding=DurablePublicBinding("12:34:56:78:9A:BC",volume,"12".repeat(32))
    private val peer=DurableConnectedPeer(UUID(7,8),binding.bondAddress,DurableSyncCapabilities(true,true,true,true))
    private val ready=DurableLibraryState(binding,RecipientVaultSummary(RecipientVaultState.READY,binding.recipientFingerprint,true),listOf(row))
    private val intent=RecordingDeletionIntent(UUID(9,10),recording,manifest.sha256,DeleteLocation.PHONE_ONLY,false,true,PendantDeletion.NOT_REQUESTED)
    private fun plan(keep:Boolean=false)=PhoneDeletionPlan(row.copy(downloadSuppressed=true,deletions=listOf(intent.copy(keepTranscript=keep))),intent.operationId,manifest.sha256)
    @Test fun queuedPendantDeletionOffersSyncInsteadOfRepeatingTheLocalIntent() {
        for(location in listOf(DeleteLocation.BOTH,DeleteLocation.PENDANT_ONLY)) {
            val pending=intent.copy(location=location,phonePending=false,pendant=PendantDeletion.PENDING)
            val queued=row.copy(phoneSegments=emptySet(),deletions=listOf(pending))
            assertEquals(DurableDeletionAction.SYNC_PENDANT,queued.deletionAction())
            // The failed first take has a finished, interrupted, empty manifest.
            assertEquals(DurableDeletionAction.SYNC_PENDANT,queued.copy(
                manifest=RecordingManifest(recording,2,true,manifest.sha256,emptyList())).deletionAction())
            assertNull(ready.syncRefusal(peer))
            assertNotNull(ready.syncRefusal(null)) // No implicit reconnect.
        }
    }
    @Test fun pendingPhoneRemovalMustFinishBeforeOfferingRemoteSync() {
        assertEquals(DurableDeletionAction.RESUME_PHONE,row.copy(deletions=listOf(intent)).deletionAction())
        assertEquals(DurableDeletionAction.RESUME_PHONE,row.copy(deletions=listOf(
            intent.copy(location=DeleteLocation.BOTH,pendant=PendantDeletion.PENDING))).deletionAction())
    }
    @Test fun completedOrNewDeletionDoesNotOfferPendingWork() {
        assertEquals(DurableDeletionAction.CHOOSE_LOCATION,row.deletionAction())
        assertEquals(DurableDeletionAction.CHOOSE_LOCATION,row.copy(deletions=listOf(
            intent.copy(phonePending=false))).deletionAction())
        assertEquals(DurableDeletionAction.CHOOSE_LOCATION,row.copy(deletions=listOf(
            intent.copy(location=DeleteLocation.BOTH,phonePending=false,pendant=PendantDeletion.CONFIRMED,tombstoneRevision=6))).deletionAction())
    }
    @Test fun syncRequiresVerifiedEnrolledMatchingSupportedPeerAndIdleState() {
        assertNull(ready.syncRefusal(peer))
        for(state in listOf(ready.copy(binding=null),ready.copy(vault=null),ready.copy(vault=ready.vault!!.copy(backupVerified=false)),
            ready.copy(vault=ready.vault!!.copy(fingerprintHex="ab".repeat(32))),ready.copy(needsAttention=true),ready.copy(work=DurableLibraryWork.PLAY)))
            assertNotNull(state.syncRefusal(peer))
        assertNotNull(ready.syncRefusal(null))
        assertNotNull(ready.syncRefusal(peer.copy(bondAddress="12:34:56:78:9A:BD")))
        assertNotNull(ready.syncRefusal(peer.copy(capabilities=DurableSyncCapabilities(true,true,true,false))))
    }
    @Test fun localPlaybackNeedsNoConnectionButCompleteCurrentFinalManifestAndVerifiedKey() {
        assertTrue(ready.playable(row))
        for(bad in listOf(row.copy(phoneSegments=emptySet()),row.copy(downloadSuppressed=true),row.copy(staleVolume=true),
            row.copy(deletions=listOf(intent)),row.copy(deletions=listOf(intent.copy(phonePending=false,location=DeleteLocation.PENDANT_ONLY,pendant=PendantDeletion.PENDING))),
            row.copy(manifest=RecordingManifest(recording,4,false,manifest.sha256,listOf(segment)))))assertFalse(ready.playable(bad))
        assertFalse(ready.copy(vault=ready.vault!!.copy(backupVerified=false)).playable(row))
        assertFalse(ready.copy(work=DurableLibraryWork.SYNC).playable(row))
        assertFalse(ready.copy(binding=binding.copy(volume=volume.copy(generation=8))).playable(row))
    }
    @Test fun ramOnlyDerivativePolicyProvesAbsenceWithoutCreatingOrDeletingAnything() {
        val parent=temporary.newFolder().canonicalFile.toPath()
        val legacy=parent.resolve("legacy.wav");Files.write(legacy,byteArrayOf(1,2,3))
        val synced=mutableListOf<java.nio.file.Path>()
        val disk=RamOnlyRecordingDerivatives(parent,SegmentDirectorySync { synced.add(it) })
        disk.removeAudioAndSync(plan());disk.removeTranscriptAndSync(plan())
        assertEquals(listOf(parent,parent),synced);assertArrayEquals(byteArrayOf(1,2,3),Files.readAllBytes(legacy))
        assertFalse(Files.exists(parent.resolve("durable-derivatives-v1")))
    }
    @Test fun anyOwnedDerivativeOrInvalidNamespaceRefusesWithoutUnlink() {
        for(directory in listOf(false,true)) {
            val parent=temporary.newFolder().canonicalFile.toPath();val namespace=parent.resolve("durable-derivatives-v1")
            val artifact=if(directory) { Files.createDirectory(namespace);namespace.resolve("unexpected.wav") } else namespace
            Files.write(artifact,byteArrayOf(5))
            val disk=RamOnlyRecordingDerivatives(parent,SegmentDirectorySync { fail("No success barrier before admission") })
            assertThrows(IllegalStateException::class.java){disk.removeAudioAndSync(plan())}
            assertArrayEquals(byteArrayOf(5),Files.readAllBytes(artifact))
        }
    }
    @Test fun emptyReservedDerivativeDirectoryNeedsBothDirectoryBarriers() {
        val parent=temporary.newFolder().canonicalFile.toPath();val root=Files.createDirectory(parent.resolve("durable-derivatives-v1"))
        val synced=mutableListOf<java.nio.file.Path>()
        RamOnlyRecordingDerivatives(parent,SegmentDirectorySync { synced.add(it) }).removeAudioAndSync(plan())
        assertEquals(listOf(root,parent),synced)
        assertThrows(IllegalStateException::class.java){RamOnlyRecordingDerivatives(parent,SegmentDirectorySync { error("Injected fsync failure") }).removeAudioAndSync(plan())}
    }
    @Test fun uncertainPlaybackDisposalStillFencesDeletionAfterWorkerUnregisters() {
        val registry=RecordingPlaybackRegistry()
        val handle=registry.register(manifest) {}
        registry.fence();handle.close()
        var diskCalls=0
        val disk=object:RecordingDiskDerivatives {
            override fun removeAudioAndSync(plan:PhoneDeletionPlan){diskCalls++}
            override fun removeTranscriptAndSync(plan:PhoneDeletionPlan){diskCalls++}
        }
        assertThrows(RecordingPlaybackException::class.java){RecordingDerivativeDeletion(registry,disk).removeAndSync(plan())}
        assertEquals(0,diskCalls)
    }
    @Test fun keepingTranscriptStillChecksAndClearsAudioPolicy() {
        var audio=0;var text=0
        val disk=object:RecordingDiskDerivatives {
            override fun removeAudioAndSync(plan:PhoneDeletionPlan){audio++}
            override fun removeTranscriptAndSync(plan:PhoneDeletionPlan){text++}
        }
        RecordingDerivativeDeletion(RecordingPlaybackRegistry(),disk).removeAndSync(plan(true))
        assertEquals(1,audio);assertEquals(0,text)
    }
}
