package org.openpendant.app

import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.util.UUID
import java.util.concurrent.CancellationException
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Real temporary ciphertext files and the real coordinator/download engine;
 * fake authenticated transport and durable-CAS port. No Bluetooth, owner keys,
 * HPKE/audio decode or Android filesystem/durability claims from this JVM test. */
class DurableRecordingSyncTest {
    @get:Rule val temporary=TemporaryFolder(File("build/sync-test-scratch").apply { mkdirs() })
    private val volume=RecordingVolume(UUID(1,2),UUID(3,4),7)
    private val fingerprint="12".repeat(32)
    private val connection=DurableSyncConnection(UUID(5,6),volume,fingerprint)
    private val recording=DurableRecordingId(volume,UUID(7,8))
    private val other=DurableRecordingId(volume,UUID(9,10))
    private fun bytes(id:DurableRecordingId,seq:Int,size:Int=500)=
        EncryptedSegmentHeader.encode(id,ByteArray(32){0x12},seq,size)+byteArrayOf(4)+ByteArray(64+size+16){(it*13+seq).toByte()}
    private fun manifest(id:DurableRecordingId,count:Int=2,size:Int=500,finished:Boolean=true):Pair<RecordingManifest,Map<SegmentIdentity,ByteArray>> {
        val data=(0 until count).map { seq ->
            val payload=bytes(id,seq,size)
            SegmentIdentity(id,seq,digestHex(payload),payload.size.toLong()) to payload
        }.toMap()
        return RecordingManifest(id,1,finished,"ab".repeat(32),data.keys.toList()) to data
    }

    private class Metadata : DurableRecordingMetadata {
        val rows=linkedMapOf<DurableRecordingId,RecordingSyncSnapshot>()
        val owners=RecordingSyncOwnership()
        var opened=0;var verified=0;var failPublication=false
        var afterVerification:()->Unit={}
        var beforeCoordinator:(DurableRecordingId)->Unit={}
        override fun snapshots(deviceId:UUID)=rows.values.filter { it.recording.volume.deviceId==deviceId }
        override fun invalidateOtherVolumes(current:RecordingVolume) {
            snapshots(current.deviceId).filter { it.recording.volume!=current && !it.staleVolume }.forEach { row ->
                val core=core(row)
                try { core.authenticatedConnection(current,true) } finally { core.close() }
            }
        }
        override fun createRecording(recording:DurableRecordingId) { rows.putIfAbsent(recording,RecordingSyncSnapshot(recording)) }
        override fun coordinator(recording:DurableRecordingId,verifyPhoneSegment:(SegmentIdentity)->Boolean):RecordingSyncContract {
            beforeCoordinator(recording)
            val row=rows.getValue(recording)
            if(row.deletions.any { it.phonePending }) throw DeletionRecoveryRequiredException()
            check(row.phoneSegments.all { verified++;verifyPhoneSegment(it) })
            afterVerification()
            opened++
            return core(row)
        }
        fun core(row:RecordingSyncSnapshot)=RecordingSyncContract(row,owners) { previous,next ->
            check(rows.getValue(row.recording).revision==previous)
            if(failPublication && next.phoneSegments.isNotEmpty()) throw IllegalStateException("Synthetic CAS failure")
            rows[row.recording]=next
        }
        fun change(id:DurableRecordingId,action:(RecordingSyncContract)->Unit) {
            val core=core(rows.getValue(id))
            try { core.authenticatedConnection(id.volume,true);action(core) } finally { core.close() }
        }
    }

    /** Same real-hardlink witness policy as existing disk tests on Windows JBR,
     * whose BasicFileAttributes.fileKey is null. Witnesses live outside quota
     * root; Android production uses actual device/inode/type from Os.lstat. */
    private class Witness(private val directory:Path):SegmentFileIdentity {
        private val retained=mutableListOf<Pair<Path,Any>>()
        override fun key(path:Path,isDirectory:Boolean):Any {
            if(isDirectory)check(Files.isDirectory(path,NOFOLLOW_LINKS))
            val target=if(isDirectory)path.resolve(".store.lock") else path
            check(Files.isRegularFile(target,NOFOLLOW_LINKS))
            val token=retained.firstOrNull { Files.isSameFile(target,it.first) }?.second ?: Any().also {
                val witness=directory.resolve("inode-${retained.size}")
                Files.createLink(witness,target);retained+=witness to it
            }
            return isDirectory to token
        }
    }
    private inner class Fixture(vararg values:Pair<RecordingManifest,Map<SegmentIdentity,ByteArray>>) {
        val directory=temporary.newFolder()
        val store=DurableSegmentStore(directory,SegmentDirectorySync { check(Files.isDirectory(it)) },Witness(temporary.newFolder().toPath()))
        val metadata=Metadata()
        var now=1000L
        val remote=FakeTransport(values.toList(),metadata)
        fun session(clock:()->Long={now})=DurableRecordingSyncSession(connection,remote,metadata,store,clock)
        fun run()=session().run()
        fun runMode(mode:DurableSyncMode)=DurableRecordingSyncSession(connection,
            if(mode==DurableSyncMode.INVENTORY_ONLY)MetadataOnlyRecordingTransport(remote) else remote,
            metadata,store,{now},mode=mode).run()
        fun seed(manifest:RecordingManifest) {
            metadata.rows[manifest.recording]=RecordingSyncSnapshot(manifest.recording,manifest=manifest,pendantCopy=PendantCopy.PRESENT)
        }
        fun expect(segment:SegmentIdentity)=EncryptedSegmentExpectation(segment,ByteArray(32){0x12},(segment.byteCount-209).toInt())
    }
    private inner class FakeTransport(values:List<Pair<RecordingManifest,Map<SegmentIdentity,ByteArray>>>,val metadata:Metadata):DurableRecordingTransport {
        var onCapabilities:()->Unit={}
        override var capabilities=DurableSyncCapabilities(true,true,true,true)
            get() { onCapabilities();return field }
        var batchLimit: () -> Boolean = { false }
        override fun batchLimitReached() = batchLimit()
        var onAcquire:()->Unit={}
        val ownership=DurableTransportOwnership()
        var completeReplies=true
        var knownCancellation=true
        var lastCall:DurableSyncCall?=null
        override fun acquireSession(connection:DurableSyncConnection)=ownership.acquire(connection.epoch).also { onAcquire() }
        var ended = 0
        override fun endSession(lease: DurableTransportLease) { ended++ }
        var current=true
        var onFreshness:()->Unit={}
        val manifests=values.associate { it.first.recording to it.first }.toMutableMap()
        val payloads=values.flatMap { it.second.entries }.associate { it.key to it.value }
        val calls=mutableListOf<String>();val reads=mutableListOf<Pair<SegmentIdentity,Long>>()
        val buffers=mutableListOf<ByteArray>();val receipts=mutableListOf<SegmentIdentity>()
        val deletes=mutableListOf<UUID>();val tombstones=mutableMapOf<UUID,DurableTombstoneReply>()
        var pageSize=1;var chunkSize=4096
        var before:(String,DurableSyncCall)->Unit={_,_->}
        var after:(String,DurableSyncCall)->Unit={_,_->}
        var pageTransform:(DurableCatalogPage)->DurableCatalogPage={it}
        var manifestTransform:(DurableManifestReply)->DurableManifestReply={it}
        var rangeTransform:(DurableRangeReply)->DurableRangeReply={it}
        var receiptTransform:(DurableReceiptReply)->DurableReceiptReply={it}
        var batchTransform:(DurableReceiptRangeReply)->DurableReceiptRangeReply={it}
        val receiptBatches=mutableListOf<Pair<Int,Int>>()
        var tombstoneTransform:(DurableTombstoneReply)->DurableTombstoneReply={it}
        override fun isCurrent(connection:DurableSyncConnection):Boolean {
            onFreshness();return current && connection==this@DurableRecordingSyncTest.connection
        }
        private fun start(name:String,call:DurableSyncCall) {
            lastCall=call;calls+=name;call.checkActive()
            try { before(name,call) }
            catch(failure:Throwable) { if(knownCancellation)call.transportCancelled();throw failure }
        }
        private fun finish(name:String,call:DurableSyncCall) {
            if(completeReplies)check(call.transportCompleted())
            after(name,call)
        }
        override fun catalog(offset:Int,maximumEntries:Int,snapshotRevision:Long?,call:DurableSyncCall):DurableCatalogPage {
            start("catalog",call);assertTrue(maximumEntries<=8);assertTrue(snapshotRevision==null || snapshotRevision==3L)
            val entries=manifests.values.map { DurableCatalogEntry(it.recording,it.revision,it.sha256,it.segments.size,it.finished) }
            val result=DurableCatalogPage(connection.epoch,3,offset,entries.size,entries.drop(offset).take(minOf(pageSize,maximumEntries)))
            finish("catalog",call);return pageTransform(result)
        }
        override fun manifest(entry:DurableCatalogEntry,call:DurableSyncCall):DurableManifestReply {
            start("manifest",call);finish("manifest",call)
            return manifestTransform(DurableManifestReply(connection.epoch,manifests.getValue(entry.recording)))
        }
        override fun read(segment:SegmentIdentity,offset:Long,maximumBytes:Int,call:DurableSyncCall):DurableRangeReply {
            start("read",call);assertTrue(maximumBytes in 1..4096);reads+=segment to offset
            val bytes=payloads.getValue(segment).copyOfRange(offset.toInt(),minOf(segment.byteCount.toInt(),offset.toInt()+maximumBytes,offset.toInt()+chunkSize))
            buffers+=bytes
            finish("read",call)
            return rangeTransform(DurableRangeReply(connection.epoch,EncryptedSegmentChunk(segment,offset,bytes)))
        }
        override fun receipt(segment:SegmentIdentity,call:DurableSyncCall):DurableReceiptReply {
            start("receipt",call)
            // This assertion is the fake pendant's independent observation of
            // app CAS: not allowed to send an ACK based on an in-RAM byte count.
            assertTrue(segment in metadata.rows.getValue(segment.recording).phoneSegments)
            assertTrue(segment in metadata.rows.getValue(segment.recording).pendingReceipts)
            receipts+=segment;finish("receipt",call)
            return receiptTransform(DurableReceiptReply(connection.epoch,segment))
        }
        override fun delete(intent:RecordingDeletionIntent,call:DurableSyncCall):DurableTombstoneReply {
            start("delete",call)
            assertTrue(metadata.rows.getValue(intent.recording).deletions.any { it==intent })
            deletes+=intent.operationId
            val reply=tombstones.getOrPut(intent.operationId) { DurableTombstoneReply(connection.epoch,intent.recording,intent.operationId,intent.manifestSha256,2) }
            manifests.remove(intent.recording)
            finish("delete",call);return tombstoneTransform(reply)
        }
        override fun receiptRange(manifest:RecordingManifest,first:Int,count:Int,call:DurableSyncCall):DurableReceiptRangeReply {
            start("receiptRange",call)
            check(capabilities.receiptBatch && count in 1..32 && manifest.finished)
            val selected=manifest.segments.subList(first,first+count)
            val row=metadata.rows.getValue(manifest.recording)
            assertTrue(row.phoneSegments.containsAll(selected));assertTrue(row.pendingReceipts.containsAll(selected))
            receiptBatches+=first to count;receipts+=selected;finish("receiptRange",call)
            return batchTransform(DurableReceiptRangeReply(connection.epoch,manifest.recording,manifest.sha256,first,count))
        }
    }

    @Test fun inventoryFindsEveryPageWithoutPayloadReceiptOrDeletion() {
        val f=Fixture(manifest(recording,3),manifest(other,2))
        val result=f.runMode(DurableSyncMode.INVENTORY_ONLY)
        assertEquals(2,result.catalogEntries.size);assertEquals(2,result.manifestsObserved)
        assertEquals(setOf(recording,other),f.metadata.rows.keys)
        assertTrue(f.metadata.rows.values.all { it.phoneSegments.isEmpty() && it.deletions.isEmpty() })
        assertTrue(f.remote.calls.all { it=="catalog" || it=="manifest" })
        assertTrue(f.remote.reads.isEmpty());assertTrue(f.remote.receipts.isEmpty());assertTrue(f.remote.deletes.isEmpty())
        f.remote.calls.clear();assertEquals(0,f.runMode(DurableSyncMode.INVENTORY_ONLY).manifestsObserved)
        assertEquals(listOf("catalog","catalog"),f.remote.calls)
    }
    @Test fun inventoryCannotSendAlreadyPendingRemoteDeleteOrReceipt() {
        val data=manifest(recording,1);val f=Fixture(data);f.seed(data.first)
        f.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        val before=f.metadata.rows.toMap()
        val r=f.runMode(DurableSyncMode.INVENTORY_ONLY)
        assertEquals(1,r.catalogEntries.size);assertEquals(0,r.tombstonesConfirmed)
        assertEquals(before,f.metadata.rows);assertTrue(f.remote.deletes.isEmpty());assertTrue(f.remote.receipts.isEmpty())
    }
    @Test fun metadataOnlyGuardRejectsEveryPayloadAndMutationBeforeDelegate() {
        val data=manifest(recording,1);val f=Fixture(data);f.seed(data.first)
        f.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        f.runMode(DurableSyncMode.INVENTORY_ONLY)
        val guard=MetadataOnlyRecordingTransport(f.remote);val call=checkNotNull(f.remote.lastCall)
        val segment=data.first.segments.single();val intent=f.metadata.rows.getValue(recording).deletions.single()
        val before=f.remote.calls.toList()
        assertThrows(IllegalStateException::class.java){guard.read(segment,0,256,call)}
        assertThrows(IllegalStateException::class.java){guard.receipt(segment,call)}
        assertThrows(IllegalStateException::class.java){guard.receiptRange(data.first,0,1,call)}
        assertThrows(IllegalStateException::class.java){guard.delete(intent,call)}
        assertEquals(before,f.remote.calls)
    }
    @Test fun inventoryPreservesRealPendingReceiptAndExistingPhoneBytes() {
        val data=manifest(recording,1);val f=Fixture(data)
        f.remote.after={name,_->if(name=="receipt")error("synthetic lost receipt acknowledgement")}
        assertThrows(DurableSyncException::class.java){f.run()}
        val before=f.metadata.rows.getValue(recording)
        assertEquals(1,before.pendingReceipts.size);assertEquals(1,before.phoneSegments.size)
        val path=File(f.directory,f.expect(data.first.segments.single()).slot+".segment")
        val ciphertext=path.readBytes();f.remote.after={_,_->};f.remote.calls.clear()
        val result=f.runMode(DurableSyncMode.INVENTORY_ONLY)
        assertEquals(0,result.receiptsConfirmed);assertEquals(before,f.metadata.rows.getValue(recording))
        assertArrayEquals(ciphertext,path.readBytes());assertTrue(f.remote.calls.all{it=="catalog"||it=="manifest"})
    }
    @Test fun inventoryRefreshDoesNotPreventLaterExplicitAudioSync() {
        val f=Fixture(manifest(recording,2),manifest(other,3))
        val details=f.runMode(RecordingSyncContent.DETAILS_ONLY.mode)
        assertEquals(2,details.catalogRecords);assertEquals(0,details.segmentsPublished)
        assertEquals(5,f.runMode(RecordingSyncContent.RECORDINGS_AND_AUDIO.mode).segmentsPublished)
        assertEquals(5,f.metadata.rows.values.sumOf{it.phoneSegments.size})
    }
    @Test fun emptyInventoryIsSuccessfulAndDoesNotInventDeletionProof() {
        val f=Fixture();val existing=manifest(recording,1).first;f.seed(existing)
        val before=f.metadata.rows.toMap();val result=f.runMode(DurableSyncMode.INVENTORY_ONLY)
        assertEquals(0,result.catalogRecords);assertEquals(before,f.metadata.rows)
        assertEquals(listOf("catalog"),f.remote.calls);assertEquals(1,f.remote.ended)
    }
    @Test fun deletionOnlyDoesNotDownloadNewOrUnselectedRecordings() {
        val first=manifest(recording,1);val f=Fixture(first,manifest(other,1));f.seed(first.first)
        f.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        val r=f.runMode(DurableSyncMode.DELETIONS_ONLY)
        assertEquals(1,r.tombstonesConfirmed);assertEquals(setOf(recording),f.metadata.rows.keys)
        assertTrue(f.remote.calls.all { it=="catalog" || it=="delete" });assertTrue(f.remote.reads.isEmpty())
        assertEquals(setOf(other),f.remote.manifests.keys)
        val after=f.runMode(DurableSyncMode.INVENTORY_ONLY)
        assertEquals(listOf(other),after.catalogEntries.map { it.recording })
        assertTrue(f.remote.deletes.size==1) // Newly discovered recording is NOT cleared.
    }
    @Test fun deletionOnlyNoIntentsOnlyReadsCatalogAndDoesNotCreateRows() {
        val f=Fixture(manifest(recording,1));val r=f.runMode(DurableSyncMode.DELETIONS_ONLY)
        assertEquals(0,r.tombstonesConfirmed);assertTrue(f.metadata.rows.isEmpty())
        assertEquals(listOf("catalog"),f.remote.calls)
    }
    @Test fun deletionOnlyMissedAckRemainsPendingAndExplicitReplayIsSameIntent() {
        val data=manifest(recording,1);val f=Fixture(data);f.seed(data.first)
        f.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        f.remote.after={name,_->if(name=="delete")error("synthetic lost reply")}
        assertThrows(DurableSyncException::class.java){f.runMode(DurableSyncMode.DELETIONS_ONLY)}
        assertEquals(PendantDeletion.PENDING,f.metadata.rows.getValue(recording).deletions.single().pendant)
        f.remote.after={_,_->}
        assertEquals(1,f.runMode(DurableSyncMode.DELETIONS_ONLY).tombstonesConfirmed)
        assertEquals(listOf(UUID(11,12),UUID(11,12)),f.remote.deletes)
        assertEquals(0,f.runMode(DurableSyncMode.INVENTORY_ONLY).catalogRecords)
    }
    @Test fun deletionOnlyKeepsPhoneBytesAndFlagsAndSkipsIncompleteLocalCleanup() {
        val data=manifest(recording,1);val f=Fixture(data);f.run()
        val path=File(f.directory,f.expect(data.first.segments.single()).slot+".segment")
        val before=path.readBytes()
        f.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        f.runMode(DurableSyncMode.DELETIONS_ONLY)
        assertArrayEquals(before,path.readBytes());assertFalse(f.metadata.rows.getValue(recording).downloadSuppressed)
        val g=Fixture(data);g.seed(data.first)
        g.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.BOTH)}
        assertEquals(1,g.runMode(DurableSyncMode.DELETIONS_ONLY).pendingPhoneDeletion)
        assertTrue(g.remote.deletes.isEmpty())
    }
    @Test fun inventoryStillRejectsMixedCatalogOrInvalidManifestAndHonorsCancellation() {
        for(mode in 0..2) {
            val f=Fixture(manifest(recording,1),manifest(other,1))
            if(mode==0)f.remote.pageTransform={p->DurableCatalogPage(p.epoch,p.snapshotRevision+p.offset,p.offset,p.total,p.entries)}
            if(mode==1)f.remote.manifestTransform={it.copy(epoch=UUID(99,99))}
            if(mode==2)f.remote.after={_,_->f.now+=DurableRecordingSyncSession.MAX_SESSION_MILLIS}
            assertThrows(DurableSyncException::class.java){f.runMode(DurableSyncMode.INVENTORY_ONLY)}
            assertTrue(f.remote.deletes.isEmpty());assertTrue(f.remote.reads.isEmpty());assertTrue(f.remote.receipts.isEmpty())
        }
    }
    @Test fun inventoryYieldsOnSuccessfulManifestBoundaryAndContinuesWithoutRefetchingIt() {
        val f=Fixture(manifest(recording,1),manifest(other,1));f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true)
        f.remote.batchLimit={f.remote.calls.count { it=="manifest" }>=1}
        val first=f.runMode(DurableSyncMode.INVENTORY_ONLY)
        assertTrue(first.morePending);assertEquals(1,first.manifestsObserved)
        f.remote.batchLimit={false};f.remote.calls.clear()
        val next=f.runMode(DurableSyncMode.INVENTORY_ONLY)
        assertFalse(next.morePending);assertEquals(1,next.manifestsObserved)
        assertEquals(2,next.catalogEntries.size);assertTrue(f.remote.reads.isEmpty())
    }
    @Test fun settledFullStorageUsesFreshCatalogAndRechecksFilesWithoutManifestOrReceipts() {
        val f=Fixture(manifest(recording,3),manifest(other,2))
        f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true)
        f.run();val before=f.metadata.rows.toMap();val verified=f.metadata.verified
        f.remote.calls.clear();f.remote.reads.clear();f.remote.receipts.clear()
        val result=f.run()
        assertEquals(2,result.catalogRecords);assertEquals(0,result.segmentsPublished)
        assertEquals(0,result.receiptsConfirmed);assertEquals(0,result.tombstonesConfirmed)
        assertEquals(listOf("catalog","catalog"),f.remote.calls)
        assertEquals(verified+5,f.metadata.verified);assertEquals(before,f.metadata.rows)
        assertTrue(f.remote.reads.isEmpty());assertTrue(f.remote.receipts.isEmpty())
    }

    @Test fun settledShortcutRequiresExactIdentityFinalityAndNoOutstandingWork() {
        val m=manifest(recording,2).first
        val e=DurableCatalogEntry(recording,m.revision,m.sha256,2,true)
        val row=RecordingSyncSnapshot(recording,manifest=m,pendantCopy=PendantCopy.PRESENT,phoneSegments=m.segments.toSet())
        assertEquals(m,row.settledCatalogManifest(e))
        for(changed in listOf(e.copy(recording=other),e.copy(manifestRevision=2),
            e.copy(manifestSha256="cd".repeat(32)),e.copy(sealedSegments=1),e.copy(finished=false)))
            assertNull(row.settledCatalogManifest(changed))
        val deletion=RecordingDeletionIntent(UUID(11,12),recording,m.sha256,DeleteLocation.PHONE_ONLY,false,false,PendantDeletion.NOT_REQUESTED)
        for(changed in listOf(row.copy(recording=other),row.copy(manifest=null),row.copy(staleVolume=true),
            row.copy(downloadSuppressed=true),row.copy(pendantCopy=PendantCopy.UNKNOWN),row.copy(pendantCopy=PendantCopy.DELETED),
            row.copy(phoneSegments=m.segments.take(1).toSet()),row.copy(pendingReceipts=setOf(m.segments.first())),
            row.copy(deletions=listOf(deletion)),row.copy(manifest=RecordingManifest(recording,m.revision,false,m.sha256,m.segments))))
            assertNull(changed.settledCatalogManifest(e))
    }

    @Test fun settledShortcutCannotHideMissingOrCorruptedRetainedCiphertext() {
        for(missing in listOf(false,true)) {
            val data=manifest(recording,1);val f=Fixture(data)
            f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true);f.run()
            val file=File(f.directory,f.expect(data.first.segments.single()).slot+".segment")
            if(missing) check(file.delete()) else file.writeBytes(file.readBytes().also{it[it.lastIndex]++})
            f.remote.calls.clear();f.remote.reads.clear();f.remote.receipts.clear()
            assertThrows(DurableSyncException::class.java){f.run()}
            assertTrue(f.metadata.verified>0);assertTrue(f.remote.reads.isEmpty());assertTrue(f.remote.receipts.isEmpty())
        }
    }

    @Test fun legacyAndUnfinishedManifestsStillFetchEvenWhenCopiesAreComplete() {
        for(full in listOf(false,true)) {
            val f=Fixture(manifest(recording,2,finished=!full))
            f.remote.capabilities=f.remote.capabilities.copy(fullStorage=full);f.run();f.remote.calls.clear()
            assertEquals(0,f.run().segmentsPublished)
            assertTrue(f.remote.calls.contains("manifest"))
        }
    }

    @Test fun changedCatalogCannotReuseSettledManifestAndKeepsExactValidation() {
        for(mode in 0..3) {
            val f=Fixture(manifest(recording,2))
            f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true);f.run()
            val before=f.metadata.rows.toMap();f.remote.calls.clear()
            f.remote.pageTransform={p->DurableCatalogPage(p.epoch,p.snapshotRevision,p.offset,p.total,p.entries.map{
                when(mode){0->it.copy(manifestRevision=2);1->it.copy(manifestSha256="cd".repeat(32))
                    2->it.copy(sealedSegments=1);else->it.copy(finished=false)}
            })}
            assertThrows(DurableSyncException::class.java){f.run()}
            assertTrue(f.remote.calls.contains("manifest"));assertEquals(before,f.metadata.rows)
        }
    }

    @Test fun settledSyncStillHonorsCancellationAndRemoteDeletion() {
        val f=Fixture(manifest(recording,1));f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true)
        f.run();f.remote.calls.clear();val job=f.session()
        f.remote.after={name,_->if(name=="catalog")job.cancel()}
        assertThrows(CancellationException::class.java){job.run()}
        assertEquals(listOf("catalog"),f.remote.calls)
        f.remote.after={_,_->};f.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        assertEquals(1,f.run().tombstonesConfirmed)
        assertEquals(listOf(UUID(11,12)),f.remote.deletes)
    }

    @Test fun fullCatalogOfSettledRecordsStillDownloadsOneNewHourAndSettlesItsTail() {
        // Thirty-one older recordings plus one 360-part hour: full pendant root
        // count, real temporary ciphertext files, fake radio (not a speed test).
        val older=(0 until 31).map { manifest(DurableRecordingId(volume,UUID(100,it.toLong()+1)),1) }
        val hour=manifest(recording,360)
        val f=Fixture(*(older+hour).toTypedArray())
        f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true,receiptBatch=true)
        f.remote.pageSize=8
        f.remote.manifests.remove(recording)
        assertEquals(31,f.run().segmentsPublished)
        val before=f.metadata.rows.toMap();val verified=f.metadata.verified
        f.remote.manifests[recording]=hour.first
        f.remote.calls.clear();f.remote.reads.clear();f.remote.receipts.clear();f.remote.receiptBatches.clear()
        val result=f.run()
        assertEquals(32,result.catalogRecords);assertEquals(360,result.segmentsPublished)
        assertEquals(360,result.receiptsConfirmed);assertFalse(result.morePending)
        assertEquals(4,f.remote.calls.count{it=="catalog"});assertEquals(1,f.remote.calls.count{it=="manifest"})
        assertEquals(verified+31,f.metadata.verified)
        assertTrue(f.remote.reads.all{it.first.recording==recording})
        assertEquals((0 until 11).map{it*32 to 32}+listOf(352 to 8),f.remote.receiptBatches)
        before.forEach{(id,row)->assertEquals(row,f.metadata.rows.getValue(id))}
        val saved=f.metadata.rows.getValue(recording)
        assertEquals(hour.first.segments.toSet(),saved.phoneSegments);assertTrue(saved.pendingReceipts.isEmpty())
        assertTrue(f.remote.deletes.isEmpty());assertTrue(f.remote.ownership.isIdle())
        f.remote.calls.clear();val settled=f.metadata.rows.toMap();val checked=f.metadata.verified
        assertEquals(0,f.run().segmentsPublished)
        assertEquals(List(4){"catalog"},f.remote.calls);assertEquals(checked+391,f.metadata.verified)
        assertEquals(settled,f.metadata.rows)
    }

    @Test fun settledShortcutCannotHideCancellationEpochLossOrClockFailureDuringFileVerification() {
        for(mode in 0..3) {
            val f=Fixture(manifest(recording,2));f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true)
            f.run();val before=f.metadata.rows.toMap();f.remote.calls.clear()
            val job=f.session()
            f.metadata.afterVerification={when(mode){
                0->job.cancel();1->f.remote.current=false
                2->f.now+=DurableRecordingSyncSession.MAX_SESSION_MILLIS;else->f.now--
            }}
            if(mode==0)assertThrows(CancellationException::class.java){job.run()}
            else assertThrows(DurableSyncException::class.java){job.run()}
            assertEquals(listOf("catalog"),f.remote.calls);assertEquals(before,f.metadata.rows)
            assertTrue(f.remote.ownership.isIdle())
        }
    }

    @Test fun settledFullStorageStillReplaysLostTailReceiptWithoutDownloadingAgain() {
        val f=Fixture(manifest(recording,3));f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true,receiptBatch=true)
        f.remote.after={name,_->if(name=="receiptRange")throw IllegalStateException("Lost tail reply")}
        assertThrows(DurableSyncException::class.java){f.run()}
        assertEquals(3,f.metadata.rows.getValue(recording).phoneSegments.size)
        assertEquals(3,f.metadata.rows.getValue(recording).pendingReceipts.size)
        f.remote.after={_,_->};f.remote.calls.clear();f.remote.reads.clear()
        val result=f.run()
        assertEquals(0,result.segmentsPublished);assertEquals(3,result.receiptsConfirmed)
        assertEquals(listOf("catalog","manifest","receiptRange"),f.remote.calls)
        assertTrue(f.remote.reads.isEmpty());assertTrue(f.metadata.rows.getValue(recording).pendingReceipts.isEmpty())
        assertTrue(f.remote.deletes.isEmpty())
    }

    @Test fun settledTailAfterLongDownloadDoesNotRequireAnotherCatalogConnection() {
        for(limitOrChanged in 0..3) {
            val fresh=manifest(recording,3);val old=manifest(other,1)
            val f=Fixture(fresh,old);f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true)
            f.remote.pageSize=8;f.remote.manifests.remove(recording);f.run()
            f.remote.manifests.clear();f.remote.manifests[recording]=fresh.first
            f.remote.manifests[other]=if(limitOrChanged==2)
                RecordingManifest(other,2,true,"cd".repeat(32),old.first.segments) else old.first
            f.remote.calls.clear();f.remote.receipts.clear();val checked=f.metadata.verified
            f.remote.after={name,_->f.now+=when(name){
                "catalog"->240_000L;"manifest"->200_000L;"read","receipt"->15_000L;else->0L
            }}
            if(limitOrChanged==1)f.remote.batchLimit={f.remote.receipts.size==3}
            if(limitOrChanged==3)f.metadata.beforeCoordinator={id->if(id==other){
                val row=f.metadata.rows.getValue(id)
                f.metadata.rows[id]=row.copy(revision=row.revision+1,pendingReceipts=old.first.segments.toSet())
            }}
            val result=f.run()
            assertEquals(3,result.segmentsPublished);assertEquals(3,result.receiptsConfirmed)
            assertEquals(limitOrChanged!=0,result.morePending)
            assertEquals(1,f.remote.calls.count{it=="catalog"})
            assertEquals(1,f.remote.calls.count{it=="manifest"})
            assertEquals(checked+if(limitOrChanged==0||limitOrChanged==3)1 else 0,f.metadata.verified)
            if(limitOrChanged==3)assertEquals(old.first.segments.toSet(),f.metadata.rows.getValue(other).pendingReceipts)
            assertTrue(f.remote.ownership.isIdle());assertTrue(f.remote.deletes.isEmpty())
        }
    }

    @Test fun batchReceiptsWaitForDurableFilesFlushTailAndNeverDeleteSource() {
        val f=Fixture(manifest(recording,70))
        f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true,receiptBatch=true)
        val result=f.run()
        assertEquals(70,result.segmentsPublished);assertEquals(70,result.receiptsConfirmed)
        assertEquals(listOf(0 to 32,32 to 32,64 to 6),f.remote.receiptBatches)
        assertFalse(f.remote.calls.contains("receipt"));assertTrue(f.remote.deletes.isEmpty())
        assertTrue(f.metadata.rows.getValue(recording).pendingReceipts.isEmpty())
        assertEquals(PendantCopy.PRESENT,f.metadata.rows.getValue(recording).pendantCopy)
    }

    @Test fun missedBatchReplyKeepsIntentsAndNextSessionDoesNotRedownloadFiles() {
        val f=Fixture(manifest(recording,35))
        f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true,receiptBatch=true)
        f.remote.after={name,_->if(name=="receiptRange")throw IllegalStateException("Lost acknowledgement")}
        assertThrows(DurableSyncException::class.java){f.run()}
        val row=f.metadata.rows.getValue(recording)
        assertEquals(32,row.phoneSegments.size);assertEquals(32,row.pendingReceipts.size)
        f.remote.after={_,_->};f.remote.reads.clear()
        val result=f.run()
        assertEquals(3,result.segmentsPublished);assertEquals(35,result.receiptsConfirmed)
        assertTrue(f.remote.reads.all { it.first.sequence>=32 })
        assertEquals(listOf(0 to 32,0 to 32,32 to 3),f.remote.receiptBatches)
    }

    @Test fun batchReplyMustBindEpochRecordingManifestFirstAndCount() {
        for(mode in 0..4){
            val f=Fixture(manifest(recording,2));f.remote.capabilities=f.remote.capabilities.copy(fullStorage=true,receiptBatch=true)
            f.remote.batchTransform={when(mode){0->it.copy(epoch=UUID(91,92));1->it.copy(recording=other)
                2->it.copy(manifestSha256="cd".repeat(32));3->it.copy(firstSequence=1);else->it.copy(count=1)}}
            assertThrows(DurableSyncException::class.java){f.run()}
            assertEquals(2,f.metadata.rows.getValue(recording).pendingReceipts.size)
            assertEquals(2,f.metadata.rows.getValue(recording).phoneSegments.size)
        }
    }

    @Test fun oldFirmwareAndUnfinishedRecordingsRetainSingleReceiptPath() {
        for(batch in listOf(false,true)){
            val f=Fixture(manifest(recording,3,finished=!batch))
            f.remote.capabilities=f.remote.capabilities.copy(fullStorage=batch,receiptBatch=batch)
            assertEquals(3,f.run().receiptsConfirmed)
            assertTrue(f.remote.receiptBatches.isEmpty());assertEquals(3,f.remote.calls.count{it=="receipt"})
        }
    }

    @Test fun twoRecordingCatalogDownloadsFourSegmentsAndDurableReceiptsWithoutDeletion() {
        val fixture=Fixture(manifest(recording),manifest(other));fixture.remote.chunkSize=23
        val result=fixture.run()
        assertEquals(1,fixture.remote.ended)
        assertEquals(DurableSyncResult(2,4,4,0,0,0,0),result)
        assertEquals(2,fixture.remote.calls.count { it=="catalog" })
        assertTrue(fixture.remote.deletes.isEmpty())
        fixture.metadata.rows.values.forEach { row ->
            assertEquals(2,row.phoneSegments.size);assertTrue(row.pendingReceipts.isEmpty())
            assertEquals(PendantCopy.PRESENT,row.pendantCopy)
        }
        assertTrue(fixture.remote.buffers.all { bytes -> bytes.all { it==0.toByte() } })
        fixture.remote.reads.clear()
        assertEquals(0,fixture.run().segmentsPublished)
        assertTrue(fixture.remote.reads.isEmpty());assertEquals(4,fixture.metadata.verified)
    }

    @Test fun interruptedRangeResumesDurable4096CheckpointAcrossNewSession() {
        val fixture=Fixture(manifest(recording,2,8000),manifest(other,1))
        var reads=0
        fixture.remote.before={name,_->if(name=="read" && ++reads==2)throw IllegalStateException("Injected disconnect")}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertEquals(1,fixture.remote.ended)
        assertTrue(fixture.remote.receipts.isEmpty());assertTrue(fixture.metadata.rows.getValue(recording).phoneSegments.isEmpty())
        fixture.remote.before={_,_->};fixture.remote.reads.clear()
        assertEquals(3,fixture.run().segmentsPublished)
        assertEquals(4096L,fixture.remote.reads.first().second)
    }

    @Test fun progressObserverReportsDurableResumeSeparatelyFromVerifiedPublication() {
        val fixture=Fixture(manifest(recording,2,8000))
        val offsets=mutableListOf<Pair<Int,Long>>()
        val published=mutableListOf<Int>()
        var manifests=0
        val observer=object:DurableSyncObserver {
            override fun recording(row:RecordingSyncSnapshot,position:Int,total:Int) {
                assertEquals(1,position);assertEquals(1,total);assertEquals(recording,row.recording);manifests++
            }
            override fun checkpoint(segment:SegmentIdentity,offset:Long) {
                // An offset callback is never a verified-file or receipt claim.
                assertTrue(segment !in fixture.metadata.rows.getValue(recording).phoneSegments)
                offsets+=segment.sequence to offset
            }
            override fun published(row:RecordingSyncSnapshot) { published+=row.phoneSegments.size }
        }
        fun runObserved()=DurableRecordingSyncSession(connection,fixture.remote,fixture.metadata,fixture.store,
            clockMillis={fixture.now},observer=observer).run()
        var reads=0
        fixture.remote.before={name,_->if(name=="read"&&++reads==2)throw IllegalStateException("Injected loss")}
        assertThrows(DurableSyncException::class.java){runObserved()}
        assertEquals(listOf(0 to 0L,0 to 4096L),offsets);assertTrue(published.isEmpty())
        assertTrue(fixture.remote.receipts.isEmpty())
        fixture.remote.before={_,_->};offsets.clear()
        assertEquals(2,runObserved().segmentsPublished)
        assertEquals(0 to 4096L,offsets.first());assertEquals(listOf(1,2),published)
        assertEquals(2,manifests);assertEquals(2,fixture.remote.receipts.size)
        assertTrue(fixture.remote.deletes.isEmpty())
    }

    @Test fun lostReceiptReplyReplaysReceiptWithoutRedownloadOrSourceDeletion() {
        val fixture=Fixture(manifest(recording,1))
        fixture.remote.after={name,_->if(name=="receipt")throw IllegalStateException("Injected lost ACK")}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertEquals(1,fixture.metadata.rows.getValue(recording).pendingReceipts.size)
        fixture.remote.after={_,_->};fixture.remote.reads.clear()
        assertEquals(1,fixture.run().receiptsConfirmed)
        assertTrue(fixture.remote.reads.isEmpty());assertEquals(2,fixture.remote.receipts.size)
        assertTrue(fixture.remote.deletes.isEmpty())
    }

    @Test fun staleEpochReadIsWipedAndCannotPublishOrReceipt() {
        val fixture=Fixture(manifest(recording,1))
        fixture.remote.rangeTransform={DurableRangeReply(UUID(99,99),it.chunk)}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertTrue(fixture.remote.buffers.single().all { it==0.toByte() })
        assertTrue(fixture.metadata.rows.getValue(recording).phoneSegments.isEmpty())
        assertTrue(fixture.remote.receipts.isEmpty())
    }

    @Test fun changedEpochDuringReceiptRetainsOutboxAndLateAckCannotConfirm() {
        val fixture=Fixture(manifest(recording,1))
        fixture.remote.after={name,_->if(name=="receipt")fixture.remote.current=false}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertEquals(1,fixture.metadata.rows.getValue(recording).pendingReceipts.size)
        assertTrue(fixture.remote.deletes.isEmpty())
    }

    @Test fun wrongReceiptBindingIsNotConfirmed() {
        val fixture=Fixture(manifest(recording,1))
        fixture.remote.receiptTransform={it.copy(segment=it.segment.copy(sha256="34".repeat(32)))}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertEquals(1,fixture.metadata.rows.getValue(recording).pendingReceipts.size)
    }

    @Test fun manifestMismatchRejectedBeforePayloadReadOrManifestCommit() {
        val fixture=Fixture(manifest(recording,1))
        fixture.remote.manifestTransform={it.copy(manifest=RecordingManifest(recording,2,true,"cd".repeat(32),it.manifest.segments))}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertTrue(fixture.remote.reads.isEmpty());assertNull(fixture.metadata.rows.getValue(recording).manifest)
    }

    @Test fun changedCatalogSnapshotDuplicateAndOversizePagesRefuseBeforeFiles() {
        for(mode in 0..3) {
            val fixture=Fixture(manifest(recording,1),manifest(other,1))
            fixture.remote.pageTransform={page -> when(mode) {
                0 -> DurableCatalogPage(page.epoch,if(page.offset==0)3 else 4,page.offset,page.total,page.entries)
                1 -> DurableCatalogPage(page.epoch,page.snapshotRevision,page.offset,page.total,listOf(DurableCatalogEntry(recording,1,"ab".repeat(32),1,true)))
                2 -> DurableCatalogPage(page.epoch,page.snapshotRevision,page.offset,129,page.entries)
                else -> DurableCatalogPage(page.epoch,page.snapshotRevision,page.offset,page.total,emptyList())
            } }
            assertThrows(DurableSyncException::class.java){fixture.run()}
            assertTrue(fixture.remote.reads.isEmpty());assertTrue(fixture.metadata.rows.isEmpty())
        }
    }

    @Test fun finalizedManifestMutationAndSealedPrefixReplacementAreRefused() {
        for(finished in listOf(false,true)) {
            val original=manifest(recording,1,finished=finished)
            val fixture=Fixture(original);fixture.run()
            val old=original.first
            fixture.remote.manifests[recording]=RecordingManifest(recording,2,true,"ef".repeat(32),listOf(old.segments.single().copy(sha256="cd".repeat(32))))
            fixture.remote.reads.clear()
            assertThrows(DurableSyncException::class.java){fixture.run()}
            assertEquals(old,fixture.metadata.rows.getValue(recording).manifest)
            assertTrue(fixture.remote.reads.isEmpty())
        }
    }

    @Test fun activeManifestGrowthDownloadsOnlyNewSealedPrefixWithoutFalseFinality() {
        val original=manifest(recording,1,finished=false)
        val grown=manifest(recording,2,finished=false)
        val fixture=Fixture(grown)
        fixture.remote.manifests[recording]=original.first;fixture.run();fixture.remote.reads.clear()
        fixture.remote.manifests[recording]=RecordingManifest(recording,2,false,"cd".repeat(32),grown.first.segments)
        assertEquals(1,fixture.run().segmentsPublished)
        assertTrue(fixture.remote.reads.all { it.first.sequence==1 })
        assertFalse(fixture.metadata.rows.getValue(recording).manifest!!.finished)
    }

    @Test fun pendingPhoneDeletionUsesRestrictedRecoveryAndNeverReadsOrRecreatesFiles() {
        val data=manifest(recording,1);val fixture=Fixture(data);fixture.seed(data.first)
        fixture.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.BOTH)}
        val result=fixture.run()
        assertEquals(1,result.pendingPhoneDeletion);assertEquals(0,fixture.metadata.opened)
        assertTrue(fixture.remote.reads.isEmpty());assertTrue(fixture.remote.deletes.isEmpty())
        assertEquals(0,fixture.directory.listFiles()!!.size)
    }

    @Test fun completedPhoneDeletionSuppressionSurvivesCatalogReplay() {
        val data=manifest(recording,1);val fixture=Fixture(data);fixture.seed(data.first)
        fixture.metadata.change(recording){core ->
            core.requestDeletion(UUID(11,12),DeleteLocation.PHONE_ONLY,true)
            core.performPhoneDeletion(UUID(11,12),{error("Must preserve transcript")},{})
        }
        assertEquals(1,fixture.run().suppressedRecordings)
        assertTrue(fixture.remote.reads.isEmpty());assertTrue(fixture.remote.receipts.isEmpty())
        assertTrue(fixture.metadata.rows.getValue(recording).downloadSuppressed)
    }

    @Test fun emptyInterruptedRecordingBothDeletionWaitsForExplicitSyncAndConfirmsExactIntent() {
        val empty=RecordingManifest(recording,2,true,"ab".repeat(32),emptyList())
        val fixture=Fixture(empty to emptyMap());fixture.seed(empty)
        fixture.metadata.change(recording) { core ->
            core.requestDeletion(UUID(11,12),DeleteLocation.BOTH)
            core.performPhoneDeletion(UUID(11,12),{},{})
        }
        val queued=fixture.metadata.rows.getValue(recording)
        assertFalse(queued.deletions.single().phonePending)
        assertEquals(PendantDeletion.PENDING,queued.deletions.single().pendant)
        assertEquals(DurableDeletionAction.SYNC_PENDANT,queued.deletionAction())
        assertTrue(fixture.remote.calls.isEmpty()) // Saving the intent did not sync.
        fixture.remote.tombstoneTransform={it.copy(revision=3)}
        assertEquals(1,fixture.run().tombstonesConfirmed)
        assertEquals(listOf(UUID(11,12)),fixture.remote.deletes)
        assertTrue(fixture.remote.reads.isEmpty());assertTrue(fixture.remote.receipts.isEmpty())
        val deleted=fixture.metadata.rows.getValue(recording)
        assertEquals(PendantCopy.DELETED,deleted.pendantCopy)
        assertEquals(PendantDeletion.CONFIRMED,deleted.deletions.single().pendant)
        assertTrue(deleted.downloadSuppressed)
    }

    @Test fun pendingRemoteDeletionLostReplyReplaysExactOperationEvenAbsentFromCatalog() {
        val data=manifest(recording,1);val fixture=Fixture(data);fixture.seed(data.first)
        fixture.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        fixture.remote.after={name,_->if(name=="delete")throw IllegalStateException("Lost tombstone reply")}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertEquals(PendantDeletion.PENDING,fixture.metadata.rows.getValue(recording).deletions.single().pendant)
        fixture.remote.after={_,_->}
        assertEquals(1,fixture.run().tombstonesConfirmed)
        assertEquals(listOf(UUID(11,12),UUID(11,12)),fixture.remote.deletes)
        assertEquals(PendantCopy.DELETED,fixture.metadata.rows.getValue(recording).pendantCopy)
        assertTrue(fixture.remote.reads.isEmpty())
    }

    @Test fun pendantDeletionKeepsAlreadyDownloadedPhoneCiphertext() {
        val data=manifest(recording,1);val fixture=Fixture(data);fixture.run()
        val path=File(fixture.directory,fixture.expect(data.first.segments.single()).slot+".segment")
        fixture.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        assertEquals(1,fixture.run().tombstonesConfirmed)
        assertTrue(path.isFile);assertEquals(data.second.values.single().size.toLong(),path.length())
        assertEquals(1,fixture.metadata.rows.getValue(recording).phoneSegments.size)
    }

    @Test fun tombstoneMustMatchOperationManifestRecordingEpochAndNewerRevision() {
        for(mode in 0..4) {
            val data=manifest(recording,1);val fixture=Fixture(data);fixture.seed(data.first)
            fixture.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
            fixture.remote.tombstoneTransform={ when(mode) {
                0 -> it.copy(operationId=UUID(77,88));1 -> it.copy(manifestSha256="ef".repeat(32))
                2 -> it.copy(recording=other);3 -> it.copy(epoch=UUID(77,88));else -> it.copy(revision=1)
            } }
            assertThrows(DurableSyncException::class.java){fixture.run()}
            assertEquals(PendantDeletion.PENDING,fixture.metadata.rows.getValue(recording).deletions.single().pendant)
        }
    }

    @Test fun missingCapabilityRefusesLegacyTransportAndUnavailableDeletionStaysPending() {
        val data=manifest(recording,1);val fixture=Fixture(data)
        fixture.remote.capabilities=DurableSyncCapabilities(false,true,true,true)
        assertThrows(DurableSyncException::class.java){fixture.run()};assertTrue(fixture.remote.calls.isEmpty())
        fixture.seed(data.first);fixture.metadata.change(recording){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        fixture.remote.capabilities=DurableSyncCapabilities(true,true,true,false)
        assertEquals(1,fixture.run().unsupportedRemoteDeletion)
        assertTrue(fixture.remote.deletes.isEmpty());assertTrue(fixture.remote.reads.isEmpty())
    }

    @Test fun newVolumeMarksOldPendingDeletionStaleWithoutReadingOldCiphertext() {
        val fixture=Fixture()
        val oldId=recording.copy(volume=volume.copy(generation=6))
        val old=manifest(oldId,1).first;fixture.seed(old)
        fixture.metadata.change(oldId){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
        fixture.run()
        val row=fixture.metadata.rows.getValue(oldId)
        assertTrue(row.staleVolume);assertEquals(PendantDeletion.STALE_GENERATION,row.deletions.single().pendant)
        assertEquals(0,fixture.metadata.verified);assertTrue(fixture.remote.deletes.isEmpty())
    }

    @Test fun missingCatalogEntryIsNotTombstoneOrLocalDelete() {
        val data=manifest(recording,1);val fixture=Fixture(data);fixture.run()
        fixture.remote.manifests.clear()
        assertEquals(0,fixture.run().catalogRecords)
        assertEquals(PendantCopy.PRESENT,fixture.metadata.rows.getValue(recording).pendantCopy)
        assertEquals(1,fixture.metadata.rows.getValue(recording).phoneSegments.size)
    }

    @Test fun cancellationOnLateRangeWipesBytesAndNextExplicitSessionCanResume() {
        val fixture=Fixture(manifest(recording,1));val session=fixture.session()
        fixture.remote.after={name,_->if(name=="read")session.cancel()}
        assertThrows(CancellationException::class.java){session.run()}
        assertTrue(fixture.remote.buffers.single().all { it==0.toByte() })
        assertTrue(fixture.remote.receipts.isEmpty())
        fixture.remote.after={_,_->};assertEquals(1,fixture.run().segmentsPublished)
        assertThrows(IllegalStateException::class.java){session.run()}
    }

    @Test fun cancelledBeforeRunAndExpiredOrBackwardCallbackCannotMutate() {
        val early=Fixture(manifest(recording,1));val session=early.session();session.cancel()
        assertThrows(CancellationException::class.java){session.run()};assertTrue(early.remote.calls.isEmpty())
        for(delta in listOf(-1L,DurableRecordingSyncSession.MAX_CALL_MILLIS,DurableRecordingSyncSession.MAX_SESSION_MILLIS)) {
            val fixture=Fixture(manifest(recording,1))
            fixture.remote.after={name,_->if(name=="catalog")fixture.now+=delta}
            assertThrows(DurableSyncException::class.java){fixture.run()}
            assertTrue(fixture.metadata.rows.isEmpty());assertTrue(fixture.remote.reads.isEmpty())
        }
    }

    @Test fun badCiphertextDigestAndAmbiguousPublicationNeverIssueReceipt() {
        val data=manifest(recording,1)
        for(corrupt in listOf(false,true)) {
            val fixture=Fixture(data)
            if(corrupt)fixture.remote.rangeTransform={it.also { reply -> reply.chunk.bytes[reply.chunk.bytes.lastIndex]++ }}
            else fixture.metadata.failPublication=true
            assertThrows(DurableSyncException::class.java){fixture.run()}
            assertTrue(fixture.remote.receipts.isEmpty());assertTrue(fixture.remote.deletes.isEmpty())
            assertTrue(fixture.metadata.rows.getValue(recording).phoneSegments.isEmpty())
        }
    }

    @Test fun overlappingSyncOrCaptureLeaseRefusesBeforeCatalogWithoutDisturbingOwner() {
        val fixture=Fixture(manifest(recording,1))
        val control=fixture.remote.ownership.acquire(connection.epoch)
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertTrue(fixture.remote.calls.isEmpty());assertTrue(control.isActive());assertTrue(control.retire())
        var checked=false
        fixture.remote.before={name,_->if(name=="read" && !checked) {
            checked=true
            assertThrows(DurableSyncException::class.java){fixture.run()}
            assertThrows(DurableTransportBusyException::class.java){fixture.remote.ownership.acquire(connection.epoch)}
        }}
        assertEquals(1,fixture.run().receiptsConfirmed);assertTrue(checked)
    }

    @Test fun fullStorageSuccessfulBatchClosesAndResumesOnlyMissingSegments() {
        val fixture=Fixture(manifest(recording,3))
        fixture.remote.capabilities=DurableSyncCapabilities(true,true,true,true,true)
        fixture.remote.batchLimit={fixture.remote.receipts.size>=1}
        val first=fixture.run()
        assertTrue(first.morePending);assertEquals(1,first.segmentsPublished)
        assertEquals(1,fixture.remote.ended);assertTrue(fixture.remote.ownership.isIdle())
        val reads=fixture.remote.reads.size
        fixture.remote.batchLimit={false}
        val resumed=fixture.run()
        assertFalse(resumed.morePending);assertEquals(2,resumed.segmentsPublished)
        assertEquals(2,resumed.receiptsConfirmed);assertEquals(reads+2,fixture.remote.reads.size)
        assertEquals(3,fixture.metadata.rows.getValue(recording).phoneSegments.size)
    }
    @Test fun fullStorageExpiredCallIsFailureNotSuccessfulBatch() {
        val fixture=Fixture(manifest(recording,2))
        fixture.remote.capabilities=DurableSyncCapabilities(true,true,true,true,true)
        fixture.remote.after={name,_->if(name=="manifest")fixture.now=750_000L}
        // An expired manifest must still fail, never become a successful yield.
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertTrue(fixture.remote.receipts.isEmpty());assertEquals(1,fixture.remote.ended)
    }
    @Test fun unknownRadioFailureKeepsLeaseUntilActualCancellationAndOldCallbacksCannotAck() {
        val fixture=Fixture(manifest(recording,1))
        fixture.remote.knownCancellation=false
        fixture.remote.before={name,_->if(name=="read")throw IllegalStateException("Uncertain radio state")}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        val old=fixture.remote.lastCall!!
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertTrue(fixture.remote.receipts.isEmpty())
        assertTrue(old.transportCancelled()) // External disconnect lifecycle finished.
        fixture.remote.before={_,_->};fixture.remote.knownCancellation=true
        var checked=false
        fixture.remote.before={name,_->if(name=="read") {
            checked=true;assertFalse(old.transportCompleted())
            assertThrows(IllegalStateException::class.java){old.checkActive()}
        }}
        assertEquals(1,fixture.run().receiptsConfirmed);assertTrue(checked)
        assertEquals(1,fixture.remote.receipts.size)
    }

    @Test fun successfulLookingReplyWithoutBothCallbacksCannotCommitAndQuarantinesLease() {
        val fixture=Fixture(manifest(recording,1));fixture.remote.completeReplies=false
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertTrue(fixture.metadata.rows.isEmpty());assertTrue(fixture.remote.receipts.isEmpty())
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertTrue(fixture.remote.lastCall!!.transportCancelled())
        fixture.remote.completeReplies=true
        assertEquals(1,fixture.run().receiptsConfirmed)
    }

    @Test fun deadlineCrossingInsideFreshnessCheckCannotConfirmReceipt() {
        val fixture=Fixture(manifest(recording,1))
        fixture.remote.after={name,_->if(name=="receipt")fixture.remote.onFreshness={fixture.now+=DurableRecordingSyncSession.MAX_SESSION_MILLIS}}
        assertThrows(DurableSyncException::class.java){fixture.run()}
        assertEquals(1,fixture.metadata.rows.getValue(recording).pendingReceipts.size)
        assertTrue(fixture.remote.deletes.isEmpty())
    }

    @Test fun staleAdmissionOrCapabilityCallbackCannotInvalidateAnyOldMetadata() {
        for(mode in 0..3) {
            val fixture=Fixture()
            val oldId=recording.copy(volume=volume.copy(generation=6))
            fixture.seed(manifest(oldId,1).first)
            fixture.metadata.change(oldId){it.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)}
            val before=fixture.metadata.rows.getValue(oldId)
            val change={if(mode%2==0)fixture.now+=DurableRecordingSyncSession.MAX_SESSION_MILLIS else fixture.remote.current=false}
            if(mode<2)fixture.remote.onAcquire=change else fixture.remote.onCapabilities=change
            assertThrows(DurableSyncException::class.java){fixture.run()}
            assertEquals(before,fixture.metadata.rows.getValue(oldId))
            assertFalse(before.staleVolume);assertTrue(fixture.remote.calls.isEmpty())
            // No radio request was started, so retirement can safely free it.
            assertTrue(fixture.remote.ownership.acquire(connection.epoch).retire())
        }
    }

    @Test fun callbackThreadsCanCheckSamePendingRequestWithoutRacingMonotonicGuard() {
        val fixture=Fixture(manifest(recording,1))
        val clock=java.util.concurrent.atomic.AtomicLong(1000)
        val failures=java.util.concurrent.ConcurrentLinkedQueue<Throwable>()
        fixture.remote.before={name,call -> if(name=="read") {
            val workers=List(4) { Thread {
                try { repeat(100) { call.checkActive() } } catch(failure:Throwable) { failures.add(failure) }
            } }
            workers.forEach { it.start() };workers.forEach { it.join(2000) }
            assertTrue(workers.none { it.isAlive });assertTrue(failures.isEmpty())
        } }
        assertEquals(1,fixture.session { clock.incrementAndGet() }.run().receiptsConfirmed)
    }
}
