package org.openpendant.app

import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.file.AccessDeniedException
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.nio.file.StandardOpenOption.WRITE
import java.nio.channels.FileChannel
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest
import java.util.UUID

/** Generated files only. Real unlink/links/metadata reads; injected directory
 * barriers and process-cut points are NOT Android/power-loss durability proof. */
class PhoneRecordingDeletionTest {
    @get:Rule val temporary = TemporaryFolder(File("build/deletion-test-scratch").apply { mkdirs() })
    private val recording = DurableRecordingId(RecordingVolume(UUID(1,2), UUID(3,4), 7), UUID(5,6))
    private val operation = UUID(7,8)
    private val fingerprint = ByteArray(32) { (it + 1).toByte() }
    private val hash = "ab".repeat(32)
    private fun payload(sequence: Int) = EncryptedSegmentHeader.encode(recording, fingerprint, sequence, 32) +
        byteArrayOf(4) + ByteArray(64 + 32 + 16) { (it + sequence).toByte() }
    private val first get() = SegmentIdentity(recording,0,digestHex(payload(0)),241)
    private val second get() = SegmentIdentity(recording,1,digestHex(payload(1)),241)
    private fun snapshot(keep: Boolean = false, location: DeleteLocation = DeleteLocation.PHONE_ONLY): RecordingSyncSnapshot =
        RecordingSyncSnapshot(recording, revision=4, workGeneration=8,
            manifest=RecordingManifest(recording,1,true,hash,listOf(first,second)),
            pendantCopy=PendantCopy.PRESENT,downloadSuppressed=true,phoneSegments=setOf(first),
            deletions=listOf(RecordingDeletionIntent(operation,recording,hash,location,keep,true,
                if(location==DeleteLocation.PHONE_ONLY)PendantDeletion.NOT_REQUESTED else PendantDeletion.PENDING)))
    private class Metadata(var state: RecordingSyncSnapshot) {
        val owners=RecordingSyncOwnership()
        var failBefore=false;var failAfter=false;var commits=0
        fun open()=PendingPhoneDeletion(state,state.deletions.single().operationId,state.manifest!!.sha256,owners) { revision,next ->
            check(revision==state.revision)
            if(failBefore)throw IOException("Synthetic before commit")
            state=next;commits++
            if(failAfter)throw IOException("Synthetic after commit")
        }
    }
    // Real hardlink witnesses, not content/path-derived fake identities. As in
    // the download tests this simulates root identity via its lock inode on
    // Windows JBR (null fileKey); Android uses actual directory dev+inode.
    private class Witnesses(private val directory:Path):SegmentFileIdentity {
        private val identities=mutableListOf<Pair<Path,Any>>()
        override fun key(path:Path,isDirectory:Boolean):Any {
            if(isDirectory)check(Files.isDirectory(path,NOFOLLOW_LINKS))
            val target=if(isDirectory)path.resolve(".store.lock") else path
            check(Files.isRegularFile(target,NOFOLLOW_LINKS))
            val existing=identities.firstOrNull { Files.isSameFile(target,it.first) }
            val token=existing?.second ?: Any().also { key ->
                val witness=directory.resolve("inode-${identities.size}");Files.createLink(witness,target);identities+=witness to key
            }
            return isDirectory to token
        }
    }
    private inner class Disk {
        val root=temporary.newFolder().canonicalFile.toPath()
        val witnesses=Witnesses(temporary.newFolder().toPath())
        val derivative=temporary.newFile().toPath()
        var derivativeCalls=0;var barriers=0;var failBarrier=false
        val events=mutableListOf<String>()
        init { Files.write(root.resolve(".store.lock"),byteArrayOf());Files.write(derivative,byteArrayOf(1,2,3)) }
        val derivatives=PhoneDeletionDerivatives { plan ->
            assertEquals(recording,plan.recording);assertEquals(operation,plan.operationId);assertEquals(hash,plan.manifestSha256)
            derivativeCalls++;events+="derivative"
            if(plan.keepTranscript)return@PhoneDeletionDerivatives
            try { Files.delete(derivative) } catch (_:java.nio.file.NoSuchFileException) { }
            events+="derivative-sync"
        }
        val sync=SegmentDirectorySync { path ->
            assertEquals(root,path);barriers++;events+="sync"
            if(failBarrier)throw IOException("Synthetic barrier failure")
        }
        fun adapter(io:PhoneDeletionFileIo=PhoneRecordingDeletion.NIO, observe:(PhoneDeletionStep,Path)->Unit={_,_->}) =
            PhoneRecordingDeletion(root.toFile(),sync,witnesses,derivatives,io) { step,path ->
                events+=step.name;observe(step,path)
            }
        fun slot(segment:SegmentIdentity)=digestHex(RecordingSyncSnapshotCodec.identityBytes(segment.recording)+ByteBuffer.allocate(4).putInt(segment.sequence).array())
        fun path(segment:SegmentIdentity,suffix:String)=root.resolve(slot(segment)+suffix)
        fun seed(segment:SegmentIdentity,published:Boolean=false) {
            val header=EncryptedSegmentHeader.encode(recording,fingerprint,segment.sequence,32)
            val body="OPNDDI1\u0000".toByteArray()+header+hexBytes(segment.sha256)
            Files.write(path(segment,".intent"),body+MessageDigest.getInstance("SHA-256").digest(body))
            val part=if(published)payload(segment.sequence) else byteArrayOf(9,8,7)
            Files.write(path(segment,".part"),part)
            val cp=ByteBuffer.allocate(48).put("OPNDDC1\u0000".toByteArray()).putLong(part.size.toLong())
                .put(MessageDigest.getInstance("SHA-256").digest(part)).array()
            Files.write(path(segment,".checkpoint"),cp+MessageDigest.getInstance("SHA-256").digest(cp))
            if(published)Files.createLink(path(segment,".segment"),path(segment,".part"))
            else Files.write(path(segment,".checkpoint-next"),cp+MessageDigest.getInstance("SHA-256").digest(cp))
        }
        fun noSlots()=Files.newDirectoryStream(root).use { it.all { path -> path.fileName.toString()==".store.lock" } }
    }

    @Test fun exactPublishedAliasesAndUncommittedPartialAreRemovedAfterDerivatives() {
        val disk=Disk();disk.seed(first,true);disk.seed(second);val meta=Metadata(snapshot())
        meta.open().use { handle -> assertTrue(handle.complete(disk.adapter()));assertFalse(handle.complete(disk.adapter())) }
        assertTrue(disk.noSlots());assertFalse(Files.exists(disk.derivative));assertEquals(1,meta.commits)
        assertEquals(listOf("derivative","derivative-sync"),disk.events.take(2))
        assertTrue(meta.state.phoneSegments.isEmpty() && meta.state.pendingReceipts.isEmpty())
        assertTrue(meta.state.downloadSuppressed);assertEquals(PendantCopy.PRESENT,meta.state.pendantCopy)
        assertFalse(meta.state.deletions.single().phonePending);assertEquals(8,meta.state.workGeneration)
    }
    @Test fun cutsAfterEveryUnlinkAndBarrierReopenOnlyExactPendingPlan() {
        // 4 unlinks+2 barriers in each slot, plus final barrier =13 cut points.
        for(cut in 1..13) {
            val disk=Disk();disk.seed(first,true);disk.seed(second);val meta=Metadata(snapshot())
            var calls=0
            meta.open().use { handle ->
                assertThrows(IOException::class.java) { handle.complete(disk.adapter { _,_ ->
                    if(++calls==cut)throw IOException("Synthetic crash")
                }) }
                assertTrue(handle.requiresReconciliation())
                assertThrows(IllegalStateException::class.java) { handle.complete(disk.adapter()) }
            }
            assertEquals(0,meta.commits);assertTrue(meta.state.deletions.single().phonePending)
            meta.open().use { assertTrue(it.complete(disk.adapter())) }
            assertTrue(disk.noSlots());assertEquals(1,meta.commits);assertTrue(meta.state.downloadSuppressed)
        }
    }
    @Test fun missingAllAuthorizedFilesDoesNotRecreateAnyDownloadEvidence() {
        val disk=Disk();val meta=Metadata(snapshot(keep=true))
        meta.open().use { assertTrue(it.complete(disk.adapter())) }
        assertTrue(disk.noSlots());assertEquals(1,disk.derivativeCalls);assertTrue(Files.exists(disk.derivative))
        assertTrue(disk.barriers>=1)
    }
    @Test fun movedPublicationWithoutPartIsAccepted() {
        val disk=Disk();disk.seed(first,true);Files.delete(disk.path(first,".part"))
        Metadata(snapshot(true)).open().use { assertTrue(it.complete(disk.adapter())) }
        assertTrue(disk.noSlots())
    }
    @Test fun barrierFailureCannotRemoveBindingIntentOrCompleteMetadata() {
        val disk=Disk();disk.seed(first,true);disk.failBarrier=true;val meta=Metadata(snapshot(true))
        meta.open().use { handle -> assertThrows(IOException::class.java) { handle.complete(disk.adapter()) } }
        assertTrue(Files.exists(disk.path(first,".intent")));assertEquals(0,meta.commits)
        disk.failBarrier=false;meta.open().use { assertTrue(it.complete(disk.adapter())) };assertTrue(disk.noSlots())
    }
    @Test fun derivativeFailurePrecedesAudioAndCanResumeSameOperation() {
        val disk=Disk();disk.seed(first,true);val meta=Metadata(snapshot())
        val failing=PhoneRecordingDeletion(disk.root.toFile(),disk.sync,disk.witnesses,PhoneDeletionDerivatives {
            Files.delete(disk.derivative);throw IOException("Synthetic derivative barrier failure")
        })
        meta.open().use { handle -> assertThrows(IOException::class.java) { handle.complete(failing) } }
        assertTrue(Files.exists(disk.path(first,".segment")));assertEquals(0,meta.commits)
        meta.open().use { assertTrue(it.complete(disk.adapter())) };assertTrue(disk.noSlots())
    }
    @Test fun beforeCommitFailureLeavesPendingAfterFilesGoneAndCanReconcile() {
        val disk=Disk();disk.seed(first,true);val meta=Metadata(snapshot(true));meta.failBefore=true
        meta.open().use { handle -> assertThrows(IOException::class.java) { handle.complete(disk.adapter()) };assertTrue(handle.requiresReconciliation()) }
        assertTrue(disk.noSlots());assertTrue(meta.state.deletions.single().phonePending)
        meta.failBefore=false;meta.open().use { assertTrue(it.complete(disk.adapter())) };assertEquals(1,meta.commits)
    }
    @Test fun afterCommitAmbiguityDoesNotRetryOrClaimSuccess() {
        val disk=Disk();val meta=Metadata(snapshot(true));meta.failAfter=true
        meta.open().use { handle -> assertThrows(IOException::class.java) { handle.complete(disk.adapter()) };assertTrue(handle.requiresReconciliation()) }
        assertFalse(meta.state.deletions.single().phonePending);assertTrue(meta.state.downloadSuppressed)
        assertThrows(PhoneDeletionAdmissionException::class.java) { meta.open() }
    }
    @Test fun bothOnlyCompletesPhoneAndRetainsRemoteOutbox() {
        val disk=Disk();val meta=Metadata(snapshot(true,DeleteLocation.BOTH))
        meta.open().use { assertTrue(it.complete(disk.adapter())) }
        assertEquals(PendantDeletion.PENDING,meta.state.deletions.single().pendant)
        assertNull(meta.state.deletions.single().tombstoneRevision);assertEquals(PendantCopy.PRESENT,meta.state.pendantCopy)
        assertTrue(meta.state.downloadSuppressed)
    }
    @Test fun wrongOperationManifestAndNoIntentAreNonMutatingRefusals() {
        val original=snapshot()
        for((state,op,digest) in listOf(Triple(original,UUID(7,9),hash),Triple(original,operation,"ef".repeat(32)),
            Triple(original.copy(deletions=emptyList()),operation,hash),
            Triple(original.copy(recording=recording.copy(volume=recording.volume.copy(generation=8))),operation,hash))) {
            var commits=0
            assertThrows(PhoneDeletionAdmissionException::class.java) {
                PendingPhoneDeletion(state,op,digest,RecordingSyncOwnership()) { _,_->commits++ }
            }
            assertEquals(0,commits)
        }
    }
    @Test fun sameAuthorityExcludesOrdinaryAndRestrictedConcurrentOwners() {
        val meta=Metadata(snapshot());val core=RecordingSyncContract(meta.state,meta.owners) { _,_->error("No commit") }
        assertThrows(RecordingSyncBusyException::class.java) { meta.open() };core.close()
        meta.open().use {
            assertThrows(RecordingSyncBusyException::class.java) { meta.open() }
            assertThrows(RecordingSyncBusyException::class.java) { RecordingSyncContract(meta.state,meta.owners) { _,_-> } }
        }
    }
    @Test fun permissionAndIoErrorsAreNotAbsenceAndDoNotClearIntent() {
        for(unlink in listOf(false,true))for(permission in listOf(false,true)) {
            val disk=Disk();disk.seed(first,true);val meta=Metadata(snapshot(true))
            val target=disk.path(first,".segment")
            val io=object:PhoneDeletionFileIo {
                fun fail():Nothing=if(permission)throw AccessDeniedException("synthetic") else throw IOException("Synthetic EIO")
                override fun attributes(path:Path):BasicFileAttributes { if(!unlink&&path==target)fail();return PhoneRecordingDeletion.NIO.attributes(path) }
                override fun unlink(path:Path) { if(unlink&&path==target)fail();PhoneRecordingDeletion.NIO.unlink(path) }
            }
            meta.open().use { handle -> assertThrows(IOException::class.java) { handle.complete(disk.adapter(io)) } }
            assertEquals(0,meta.commits);assertTrue(Files.exists(target));assertTrue(Files.exists(disk.path(first,".intent")))
        }
    }
    @Test fun boundSidecarFaultUnknownPathAndConflictingAliasRefuseBeforeDeletion() {
        for(mode in 0..5) {
            val disk=Disk();disk.seed(first,true);val meta=Metadata(snapshot(true))
            when(mode) {
                0->Files.write(disk.path(first,".fault"),byteArrayOf(1))
                1->Files.write(disk.root.resolve("unknown.bin"),byteArrayOf(1))
                2->{ Files.delete(disk.path(first,".part"));Files.write(disk.path(first,".part"),payload(0)) }
                3->{ val bytes=Files.readAllBytes(disk.path(first,".intent"));bytes[80]=(bytes[80].toInt() xor 1).toByte();Files.write(disk.path(first,".intent"),bytes) }
                4->{ Files.delete(disk.path(first,".checkpoint"));Files.createDirectory(disk.path(first,".checkpoint")) }
                5->Files.delete(disk.path(first,".intent"))
            }
            meta.open().use { handle -> assertThrows(PhoneDeletionStorageException::class.java) { handle.complete(disk.adapter()) } }
            assertEquals(0,meta.commits);assertTrue(Files.exists(disk.path(first,".segment")))
        }
    }
    @Test fun unrelatedKnownRecordingStaysByteIdentical() {
        val disk=Disk();disk.seed(first,true)
        val unrelated=disk.root.resolve("ef".repeat(32)+".segment");val bytes=byteArrayOf(1,2,3,4)
        Files.write(unrelated,bytes)
        Metadata(snapshot(true)).open().use { assertTrue(it.complete(disk.adapter())) }
        assertArrayEquals(bytes,Files.readAllBytes(unrelated))
    }
    @Test fun storeLockContentionIsNotDeletionAndReleasesOnFailure() {
        val disk=Disk();disk.seed(first,true);val meta=Metadata(snapshot(true))
        FileChannel.open(disk.root.resolve(".store.lock"),WRITE).use { channel -> channel.lock().use {
            meta.open().use { handle -> assertThrows(SegmentStorageBusyException::class.java) { handle.complete(disk.adapter()) } }
        } }
        assertEquals(0,meta.commits);meta.open().use { assertTrue(it.complete(disk.adapter())) }
    }
    @Test fun replacedRootRefusesWithoutDeletingEitherNamespace() {
        val disk=Disk();disk.seed(first,true);val adapter=disk.adapter();val old=disk.root.resolveSibling(disk.root.fileName.toString()+"-old")
        Files.move(disk.root,old);Files.createDirectory(disk.root);Files.write(disk.root.resolve(".store.lock"),byteArrayOf())
        Metadata(snapshot(true)).open().use { handle -> assertThrows(PhoneDeletionStorageException::class.java) { handle.complete(adapter) } }
        assertTrue(Files.exists(old.resolve(disk.slot(first)+".segment")))
    }
    @Test fun persistedPhoneIntentRejectsReceiptResurrectionAndAmbiguousActiveOperations() {
        val original=snapshot()
        assertThrows(IllegalArgumentException::class.java) {
            RecordingSyncContract(original.copy(pendingReceipts=setOf(first)),RecordingSyncOwnership()) { _,_-> }
        }
        assertThrows(IllegalArgumentException::class.java) {
            RecordingSyncContract(original.copy(deletions=original.deletions+
                original.deletions.single().copy(operationId=UUID(7,9))),RecordingSyncOwnership()) { _,_-> }
        }
    }
    @Test fun storeLifetimeGuardPreventsFileCallbackAfterOwnershipClosed() {
        var owned=true;var calls=0
        val handle=PendingPhoneDeletion(snapshot(),operation,hash,RecordingSyncOwnership(),{ action ->
            check(owned);action()
        }) { _,_->error("No commit") }
        owned=false
        try { assertThrows(IllegalStateException::class.java) { handle.complete(PhoneDeletionRemoval { calls++ }) } }
        finally { handle.close() }
        assertEquals(0,calls)
    }
}
