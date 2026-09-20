package org.openpendant.app

import java.io.File
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.util.UUID
import java.util.concurrent.CancellationException
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Generated ciphertext-shaped public bytes only. These tests prove disk/
 * identity flow, not HPKE authentication, Android fsync or power-cut physics. */
class SegmentDownloadEngineTest {
    // Workspace-local scratch also avoids Windows sandbox ACL restrictions on
    // GetFinalPathNameByHandle for the global TEMP directory.
    @get:Rule val temporary = TemporaryFolder(File("build/segment-test-scratch").apply { mkdirs() })
    private val recording = DurableRecordingId(RecordingVolume(
        UUID.fromString("11111111-1111-4111-8111-111111111111"),
        UUID.fromString("22222222-2222-4222-8222-222222222222"), 7),
        UUID.fromString("33333333-3333-4333-8333-333333333333"))
    private val fingerprint = ByteArray(32) { (it + 1).toByte() }
    private fun payload(size: Int = 8000, headerRecording: DurableRecordingId = recording): ByteArray =
        EncryptedSegmentHeader.encode(headerRecording, fingerprint, 0, size) +
            byteArrayOf(4) + ByteArray(64 + size + 16) { (it * 17 + 3).toByte() }
    private fun expectation(bytes: ByteArray, size: Int = bytes.size - 209) =
        EncryptedSegmentExpectation(SegmentIdentity(recording, 0, digestHex(bytes), bytes.size.toLong()), fingerprint, size)

    private class Metadata(initial: RecordingSyncSnapshot) {
        var value = initial
        var failNextPublication = false
        var publications = 0
        val authority = RecordingSyncOwnership()
        fun coordinator(): RecordingSyncContract = RecordingSyncContract(value, authority) { previous, next ->
            check(value.revision == previous)
            if (next.phoneSegments.isNotEmpty()) {
                ++publications
                if (failNextPublication) { failNextPublication = false; throw IllegalStateException("Injected metadata disk failure") }
            }
            value = next
        }.also { it.authenticatedConnection(value.recording.volume, true) }
    }
    private fun metadata(expected: EncryptedSegmentExpectation): Metadata {
        val manifest = RecordingManifest(recording, 1, true, "ab".repeat(32), listOf(expected.segment))
        return Metadata(RecordingSyncSnapshot(recording, manifest = manifest, pendantCopy = PendantCopy.PRESENT))
    }
    /** Windows JBR reports null fileKey. This test-only registry keeps real inode
     * witnesses outside the quota root and compares with Files.isSameFile. The
     * root witness uses the existing .store.lock inode; that is a simulation of
     * Android's direct directory dev+inode check, not a production replacement. */
    private class WitnessIdentity(private val witnesses:Path):SegmentFileIdentity {
        private val retained=mutableListOf<Pair<Path,Any>>()
        override fun key(path:Path,isDirectory:Boolean):Any {
            if(isDirectory) check(Files.isDirectory(path,NOFOLLOW_LINKS))
            val target=if(isDirectory)path.resolve(".store.lock") else path
            check(Files.isRegularFile(target,NOFOLLOW_LINKS))
            val existing=retained.firstOrNull { Files.isSameFile(target,it.first) }
            val key=existing?.second ?: Any().also { token ->
                val witness=witnesses.resolve("inode-${retained.size}")
                Files.createLink(witness,target)
                retained+=witness to token
            }
            return isDirectory to key
        }
    }
    private class Disk(val directory: File,val identity:SegmentFileIdentity) {
        var barriers = 0
        var failBarrier: Int? = null
        val sync = SegmentDirectorySync {
            check(Files.isDirectory(it)); ++barriers
            if (barriers == failBarrier) throw IllegalStateException("Injected directory barrier failure")
        }
        fun store(observer: ((SegmentDiskStep) -> Unit)? = null) =
            DurableSegmentStore(directory,sync,identity,observer ?: {})
    }
    private fun disk() = Disk(temporary.newFolder(),WitnessIdentity(temporary.newFolder().toPath()))
    private fun file(disk: Disk, expected: EncryptedSegmentExpectation, suffix: String) =
        File(disk.directory, expected.slot + suffix)
    private fun transport(expected: EncryptedSegmentExpectation, bytes: ByteArray,
                          offsets: MutableList<Long> = mutableListOf(), short: Int = 4096) =
        EncryptedSegmentTransport { segment, offset, maximum ->
            assertEquals(expected.segment, segment)
            assertTrue(maximum in 1..4096)
            offsets.add(offset)
            EncryptedSegmentChunk(segment, offset, bytes.copyOfRange(offset.toInt(),
                minOf(bytes.size, offset.toInt() + maximum, offset.toInt() + short)))
        }
    private fun run(store: DurableSegmentStore, metadata: Metadata, expected: EncryptedSegmentExpectation,
                    source: EncryptedSegmentTransport, cancelled: () -> Boolean = { false },
                    now: () -> Long = { 1000L }): SegmentDownloadResult {
        val core = metadata.coordinator()
        try {
            val ticket = core.beginWork(RecordingWork.DOWNLOAD, expected.segment)
            return SegmentDownloadEngine(store, now).download(expected, core, ticket, source, cancelled)
        } finally { core.close() }
    }

    @Test fun verifiedDownloadUsesExistingCoordinatorAndNeverDeletesRemoteSource() {
        val bytes = payload(); val expected = expectation(bytes); val disk = disk(); val metadata = metadata(expected)
        val offsets = mutableListOf<Long>()
        val result = run(disk.store(), metadata, expected, transport(expected, bytes, offsets))
        assertEquals(listOf(0L,4096L,8192L), offsets)
        assertEquals(0L, result.resumedOffset); assertFalse(result.reusedVerifiedFile)
        assertArrayEquals(bytes, file(disk,expected,".segment").readBytes())
        assertEquals(setOf(expected.segment), metadata.value.phoneSegments)
        assertEquals(setOf(expected.segment), metadata.value.pendingReceipts)
        val core = metadata.coordinator()
        try {
            assertTrue(core.confirmReceipt(core.nextReceipt()!!))
            assertEquals(PendantCopy.PRESENT,core.snapshot().pendantCopy)
            assertTrue(core.snapshot().deletions.isEmpty())
            assertThrows(IllegalArgumentException::class.java) { core.beginWork(RecordingWork.DOWNLOAD,expected.segment) }
        } finally { core.close() }
        assertTrue(disk.store().verifiedOnDisk(expected))
        assertTrue(disk.barriers >= 5)
    }

    @Test fun directoryQuotaKeepsExactByteBoundaryAndDoesNotCreateIntentOnRefusal() {
        for (excess in listOf(0L, 1L, 4097L)) {
            val disk = disk(); val bytes = payload(); val expected = expectation(bytes)
            val ballast = disk.directory.toPath().resolve("public-sparse-quota-fixture")
            val size = DurableSegmentStore.MAX_STORE_BYTES - expected.totalBytes - 4096 + excess
            // Bounded logical-size fixture, not a 256-MiB data allocation.
            java.nio.channels.FileChannel.open(ballast, java.nio.file.StandardOpenOption.CREATE_NEW,
                java.nio.file.StandardOpenOption.WRITE, java.nio.file.StandardOpenOption.SPARSE).use {
                it.position(size - 1); it.write(java.nio.ByteBuffer.wrap(byteArrayOf(0x42)))
            }
            assertEquals(size, Files.size(ballast))
            val store = DurableSegmentStore(disk.directory, disk.sync,
                SegmentUsableSpace { expected.totalBytes + 8192L }, identity = disk.identity)
            if (excess == 0L) {
                store.open(expected).use { assertEquals(0, it.offset); assertFalse(it.complete) }
                assertTrue(file(disk, expected, ".intent").isFile)
            } else {
                assertThrows(SegmentStorageException::class.java) { store.open(expected).close() }
                assertFalse(file(disk, expected, ".intent").exists())
                assertFalse(file(disk, expected, ".part").exists())
                assertEquals(0, disk.barriers)
            }
            assertEquals(size, Files.size(ballast))
        }
    }

    @Test fun nonRegularQuotaEntryFailsClosedWithoutTouchingItOrCreatingIntent() {
        val disk = disk(); val expected = expectation(payload())
        val directory = Files.createDirectory(disk.directory.toPath().resolve("public-nested-fixture"))
        val marker = Files.write(directory.resolve("keep"), byteArrayOf(1, 2, 3))
        var spaceQueries = 0
        val store = DurableSegmentStore(disk.directory, disk.sync,
            SegmentUsableSpace { spaceQueries++; Long.MAX_VALUE }, identity = disk.identity)
        assertThrows(SegmentStorageException::class.java) { store.open(expected).close() }
        assertEquals(0, spaceQueries); assertEquals(0, disk.barriers)
        assertArrayEquals(byteArrayOf(1, 2, 3), Files.readAllBytes(marker))
        assertFalse(file(disk, expected, ".intent").exists())
        // Refusal did not leak the lock or turn the unrelated directory into a fault.
        Files.delete(marker); Files.delete(directory)
        store.open(expected).use { assertEquals(0, it.offset) }
        assertEquals(1, spaceQueries)
    }

    @Test fun shortReadsAreBoundedAndAllReturnedBuffersAreWiped() {
        val bytes = payload(500); val expected = expectation(bytes); val disk = disk(); val metadata = metadata(expected)
        val returned = mutableListOf<ByteArray>()
        val underlying = transport(expected,bytes,short=13)
        val source = EncryptedSegmentTransport { s,o,n -> underlying.read(s,o,n).also { returned += it.bytes } }
        run(disk.store(),metadata,expected,source)
        assertTrue(returned.size > 20)
        returned.forEach { assertTrue(it.all { byte -> byte == 0.toByte() }) }
        assertArrayEquals(bytes,file(disk,expected,".segment").readBytes())
    }

    @Test fun cancelledLateReadDoesNotAppendOrPublishAndCanResume() {
        val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        var cancelled=false; var returned: ByteArray?=null
        val source=EncryptedSegmentTransport { s,o,n ->
            returned=bytes.copyOfRange(o.toInt(),o.toInt()+n); cancelled=true
            EncryptedSegmentChunk(s,o,returned!!)
        }
        assertThrows(CancellationException::class.java) { run(disk.store(),metadata,expected,source,{cancelled}) }
        assertTrue(returned!!.all { it == 0.toByte() })
        assertFalse(file(disk,expected,".segment").exists())
        assertEquals(0L,file(disk,expected,".part").length())
        assertTrue(metadata.value.pendingReceipts.isEmpty())
        run(disk.store(),metadata,expected,transport(expected,bytes))
        assertEquals(setOf(expected.segment),metadata.value.phoneSegments)
    }

    @Test fun throwingDiagnosticCannotSkipTicketCancellationOrStoreCleanup() {
        for (fatal in listOf(false,true)) {
            val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
            val core=metadata.coordinator(); val ticket=core.beginWork(RecordingWork.DOWNLOAD,expected.segment)
            var diagnostics=0
            val engine=SegmentDownloadEngine.forSyntheticTests(disk.store()) { _,_,_ ->
                diagnostics++
                if(fatal) throw AssertionError("Synthetic diagnostic error")
                else throw IllegalStateException("Synthetic diagnostic exception")
            }
            try {
                assertThrows(SegmentDownloadException::class.java) {
                    engine.download(expected,core,ticket,EncryptedSegmentTransport { _,_,_ ->
                        throw IllegalStateException("Synthetic transport failure")
                    })
                }
                assertEquals(1,diagnostics)
                assertFalse(core.isCurrent(ticket)); assertFalse(core.requiresReconciliation())
                assertNull(core.nextReceipt()); assertTrue(core.snapshot().phoneSegments.isEmpty())
                assertFalse(file(disk,expected,".segment").exists())
                // A new explicit attempt can reacquire the store and a fresh
                // ticket; the failed observer must not leak either owner.
                val retry=core.beginWork(RecordingWork.DOWNLOAD,expected.segment)
                SegmentDownloadEngine(disk.store()).download(expected,core,retry,transport(expected,bytes))
                assertEquals(setOf(expected.segment),core.snapshot().pendingReceipts)
                assertEquals(PendantCopy.PRESENT,core.snapshot().pendantCopy)
            } finally { core.close() }
        }
    }

    @Test fun transportInterruptionResumesExactDurableOffsetAfterRestart() {
        val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        val normal=transport(expected,bytes)
        val interrupted=EncryptedSegmentTransport { s,o,n ->
            if(o>0) throw IllegalStateException("Generated interruption")
            normal.read(s,o,n)
        }
        assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,interrupted) }
        assertEquals(4096L,file(disk,expected,".part").length())
        val offsets=mutableListOf<Long>()
        val result=run(disk.store(),metadata,expected,transport(expected,bytes,offsets))
        assertEquals(4096L,result.resumedOffset); assertEquals(4096L,offsets.first())
        assertFalse(result.reusedVerifiedFile)
    }

    @Test fun everyDiskTransitionCutRecoversWithoutFalseReceiptOrRedownloadOfPublishedFile() {
        for(step in SegmentDiskStep.entries) {
            val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
            var cut=false
            val interrupted=disk.store { observed ->
                if(!cut && observed==step && file(disk,expected,".part").length()>0) {
                    cut=true; throw IllegalStateException("Generated disk cut")
                }
            }
            assertThrows(SegmentDownloadException::class.java) { run(interrupted,metadata,expected,transport(expected,bytes)) }
            assertTrue(cut); assertTrue(metadata.value.phoneSegments.isEmpty()); assertTrue(metadata.value.pendingReceipts.isEmpty())
            val existed=file(disk,expected,".segment").exists()
            val offsets=mutableListOf<Long>()
            val result=run(disk.store(),metadata,expected,transport(expected,bytes,offsets))
            assertEquals(existed,result.reusedVerifiedFile)
            if(existed) assertTrue(offsets.isEmpty())
            assertArrayEquals(bytes,file(disk,expected,".segment").readBytes())
            assertEquals(setOf(expected.segment),metadata.value.phoneSegments)
        }
    }

    @Test fun metadataFailureAfterAtomicPublicationRequiresNewCoordinatorAndReverification() {
        val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        metadata.failNextPublication=true
        val core=metadata.coordinator(); val ticket=core.beginWork(RecordingWork.DOWNLOAD,expected.segment)
        assertThrows(SegmentDownloadException::class.java) {
            SegmentDownloadEngine(disk.store()).download(expected,core,ticket,transport(expected,bytes))
        }
        assertTrue(core.requiresReconciliation()); assertNull(core.nextReceipt())
        assertTrue(file(disk,expected,".segment").exists()); core.close()
        var reads=0
        val result=run(disk.store(),metadata,expected,EncryptedSegmentTransport { _,_,_ -> reads++; error("Must reuse verified file") })
        assertTrue(result.reusedVerifiedFile); assertEquals(0,reads)
        assertEquals(setOf(expected.segment),metadata.value.pendingReceipts)
    }

    private fun leavePartial(disk: Disk, expected: EncryptedSegmentExpectation, bytes: ByteArray, metadata: Metadata) {
        val normal=transport(expected,bytes)
        assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,
            EncryptedSegmentTransport { s,o,n -> if(o>0) error("Generated stop") else normal.read(s,o,n) }) }
    }

    @Test fun corruptedAndTruncatedCheckpointedPrefixIsStickyAcrossRestart() {
        for(truncated in listOf(false,true)) {
            val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
            leavePartial(disk,expected,bytes,metadata)
            val partial=file(disk,expected,".part")
            if(truncated) RandomAccessFile(partial,"rw").use { it.setLength(40) }
            else RandomAccessFile(partial,"rw").use { it.seek(400); it.writeByte(99) }
            assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,transport(expected,bytes)) }
            assertTrue(file(disk,expected,".fault").exists())
            // Even restoring original bytes must not automatically clear fault.
            partial.writeBytes(bytes.copyOfRange(0,4096))
            assertThrows(SegmentStorageException::class.java) { disk.store().verifiedOnDisk(expected) }
            assertFalse(file(disk,expected,".segment").exists())
        }
    }

    @Test fun incompleteUncheckpointedTailIsDiscardedNotAcknowledged() {
        val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        leavePartial(disk,expected,bytes,metadata)
        file(disk,expected,".part").appendBytes(ByteArray(73){99})
        val offsets=mutableListOf<Long>()
        run(disk.store(),metadata,expected,transport(expected,bytes,offsets))
        assertEquals(4096L,offsets.first())
        assertArrayEquals(bytes,file(disk,expected,".segment").readBytes())
    }

    @Test fun checkpointAndIntentAreStrictBoundedAndNeverRebound() {
        for(suffix in listOf(".intent",".checkpoint",".checkpoint-next")) {
            val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
            leavePartial(disk,expected,bytes,metadata)
            file(disk,expected,suffix).writeBytes(ByteArray(201){8})
            assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,transport(expected,bytes)) }
            assertFalse(file(disk,expected,".segment").exists())
        }
        val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        leavePartial(disk,expected,bytes,metadata)
        val changed=EncryptedSegmentExpectation(expected.segment.copy(sha256="77".repeat(32)),fingerprint,8000)
        assertEquals(expected.slot,changed.slot)
        assertThrows(SegmentStorageException::class.java) { disk.store().verifiedOnDisk(changed) }
        assertTrue(file(disk,expected,".fault").exists())
    }

    @Test fun finalDigestCanonicalHeaderAndPointPrefixAreMandatoryBeforePublication() {
        for(mode in 0..2) {
            val bytes=payload()
            if(mode==1) bytes[52]=(bytes[52].toInt() xor 1).toByte()
            if(mode==2) bytes[128]=2
            val expected=if(mode==0) expectation(payload().also { it[it.lastIndex]=44 }) else expectation(bytes)
            val disk=disk(); val metadata=metadata(expected)
            assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,transport(expected,bytes)) }
            assertTrue(file(disk,expected,".fault").exists())
            assertFalse(file(disk,expected,".segment").exists()); assertTrue(metadata.value.pendingReceipts.isEmpty())
        }
    }

    @Test fun wrongOffsetIdentityAndOversizeResponseFenceAndWipe() {
        for(mode in 0..2) {
            val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
            lateinit var response:ByteArray
            val source=EncryptedSegmentTransport { s,o,n ->
                response=ByteArray(if(mode==2)n+1 else n){8}
                EncryptedSegmentChunk(if(mode==0)s.copy(sha256="44".repeat(32))else s,if(mode==1)o+1 else o,response)
            }
            assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,source) }
            assertTrue(response.all{it==0.toByte()}); assertTrue(file(disk,expected,".fault").exists())
        }
    }

    @Test fun emptyResponseStopsWithoutDeletingStagingAndOneByteReadsAreBounded() {
        val bytes=payload(2000); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,
            EncryptedSegmentTransport { s,o,_ -> EncryptedSegmentChunk(s,o,byteArrayOf()) }) }
        val offsets=mutableListOf<Long>()
        run(disk.store(),metadata,expected,transport(expected,bytes,offsets,1))
        assertEquals(bytes.size,offsets.size); assertEquals(bytes.size.toLong(),file(disk,expected,".part").length())
        assertTrue(disk.barriers < 10)
        assertEquals(setOf(expected.segment),metadata.value.phoneSegments)
    }

    @Test fun cancellationAndGenerationChangesAfterReadCannotPublish() {
        val bytes=payload(50); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        val core=metadata.coordinator(); val ticket=core.beginWork(RecordingWork.DOWNLOAD,expected.segment)
        val source=EncryptedSegmentTransport { s,o,n ->
            core.disconnected(); EncryptedSegmentChunk(s,o,bytes.copyOfRange(0,n))
        }
        assertThrows(CancellationException::class.java) { SegmentDownloadEngine(disk.store()).download(expected,core,ticket,source) }
        assertFalse(file(disk,expected,".segment").exists()); assertNull(core.nextReceipt()); core.close()
    }

    @Test fun lateTransportReturnIsWipedAndNotCheckpointed() {
        val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        var time=0L; lateinit var response:ByteArray
        val source=EncryptedSegmentTransport { s,o,n -> time=120_000; response=bytes.copyOfRange(0,n); EncryptedSegmentChunk(s,o,response) }
        assertThrows(SegmentDownloadException::class.java) { run(disk.store(),metadata,expected,source,now={time}) }
        assertTrue(response.all{it==0.toByte()}); assertEquals(0L,file(disk,expected,".part").length())
    }

    @Test fun concurrentStoreOwnerAndUnboundOrCorruptPublishedFilesAreRefused() {
        val bytes=payload(); val expected=expectation(bytes); val disk=disk()
        disk.store().open(expected).use {
            assertThrows(SegmentStorageBusyException::class.java) { disk.store().open(expected) }
        }
        file(disk,expected,".segment").writeBytes(ByteArray(20))
        assertThrows(SegmentStorageException::class.java) { disk.store().verifiedOnDisk(expected) }
        assertArrayEquals(ByteArray(20),file(disk,expected,".segment").readBytes())
        val other=this.disk()
        file(other,expected,".part").writeBytes(bytes)
        assertThrows(SegmentStorageException::class.java) { other.store().verifiedOnDisk(expected) }
        assertTrue(file(other,expected,".fault").exists())
    }

    @Test fun expectedFingerprintIsSnapshottedAndOversizeGeometryRejected() {
        val bytes=payload(); val key=fingerprint.copyOf()
        val expected=EncryptedSegmentExpectation(SegmentIdentity(recording,0,digestHex(bytes),bytes.size.toLong()),key,8000)
        key.fill(0)
        val disk=disk(); run(disk.store(),metadata(expected),expected,transport(expected,bytes))
        assertThrows(IllegalArgumentException::class.java) { EncryptedSegmentExpectation(expected.segment,fingerprint,65537) }
        assertThrows(IllegalArgumentException::class.java) { EncryptedSegmentExpectation(expected.segment.copy(byteCount=3),fingerprint,1) }
    }

    @Test fun fullSizeSegmentAt64ByteRepliesOnlyCheckpointsBoundedBatches() {
        val bytes=payload(65536); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        val offsets=mutableListOf<Long>(); val progress=mutableListOf<Int>()
        val core=metadata.coordinator(); val ticket=core.beginWork(RecordingWork.DOWNLOAD,expected.segment)
        try {
            SegmentDownloadEngine(disk.store()).download(expected,core,ticket,transport(expected,bytes,offsets,64),onProgress={progress+=it})
        } finally { core.close() }
        assertEquals(1028,offsets.size); assertEquals(17,progress.size); assertEquals(100,progress.last())
        assertTrue(disk.barriers < 25)
        assertArrayEquals(bytes,file(disk,expected,".segment").readBytes())
    }

    @Test fun interruptionMidAggregateRestartsAtDurableOffsetNotNetworkOffset() {
        val bytes=payload(); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        val normal=transport(expected,bytes,short=64); val progress=mutableListOf<Int>()
        val core=metadata.coordinator(); val ticket=core.beginWork(RecordingWork.DOWNLOAD,expected.segment)
        try {
            assertThrows(SegmentDownloadException::class.java) {
                SegmentDownloadEngine(disk.store()).download(expected,core,ticket,
                    EncryptedSegmentTransport { s,o,n -> if(o==4224L) error("Generated interruption") else normal.read(s,o,n) },
                    onProgress={progress+=it})
            }
        } finally { core.close() }
        assertEquals(listOf(4096*100/bytes.size),progress)
        assertEquals(4096L,file(disk,expected,".part").length())
        val offsets=mutableListOf<Long>()
        run(disk.store(),metadata,expected,transport(expected,bytes,offsets))
        assertEquals(4096L,offsets.first())
    }

    @Test fun publicationDirectoryBarrierFailureDoesNotIssueReceiptAndRestartRechecks() {
        val bytes=payload(10); val expected=expectation(bytes); val disk=disk(); val metadata=metadata(expected)
        var failed=false
        val failSync=SegmentDirectorySync {
            if(!failed && file(disk,expected,".segment").exists()) { failed=true; error("Generated fsync failure") }
            disk.sync.sync(it)
        }
        assertThrows(SegmentDownloadException::class.java) {
            run(DurableSegmentStore(disk.directory,failSync,disk.identity),metadata,expected,transport(expected,bytes))
        }
        assertTrue(failed); assertTrue(metadata.value.pendingReceipts.isEmpty())
        var reads=0
        val result=run(disk.store(),metadata,expected,EncryptedSegmentTransport { _,_,_ -> reads++; error("Already published") })
        assertTrue(result.reusedVerifiedFile); assertEquals(0,reads)
    }

    @Test fun absentIntegrityInspectionDoesNotCreateRecordingSidecars() {
        val expected=expectation(payload()); val disk=disk()
        assertFalse(disk.store().verifiedOnDisk(expected))
        assertEquals(listOf(".store.lock"),disk.directory.list()!!.toList())
    }

    @Test fun statvfsGeometryRejectsNegativeImpossibleAndOverflowValues() {
        val invalid = listOf(
            longArrayOf(-1,4096,2,3), longArrayOf(1,-1,2,3), longArrayOf(1,0,2,3),
            longArrayOf(0,4096,-1,3), longArrayOf(0,4096,0,-1),
            longArrayOf(3,4096,2,3), longArrayOf(1,4096,3,2),
            longArrayOf(Long.MAX_VALUE,2,Long.MAX_VALUE,Long.MAX_VALUE),
        )
        invalid.forEach { value ->
            assertThrows(SegmentStorageException::class.java) {
                checkedSegmentUsableSpace(value[0],value[1],value[2],value[3])
            }
        }
        assertEquals(0L,checkedSegmentUsableSpace(0,4096,0,0))
        assertEquals(8192L,checkedSegmentUsableSpace(2,4096,3,4))
        assertEquals(Long.MAX_VALUE,checkedSegmentUsableSpace(Long.MAX_VALUE,1,Long.MAX_VALUE,Long.MAX_VALUE))
        assertEquals((Long.MAX_VALUE/4096)*4096,
            checkedSegmentUsableSpace(Long.MAX_VALUE/4096,4096,Long.MAX_VALUE/4096,Long.MAX_VALUE/4096))
    }

    @Test fun lowOrFailedSpaceQueryRefusesBeforeTransportAndRecordingSidecars() {
        val bytes=payload(); val expected=expectation(bytes)
        for(available in listOf(-1L,0L,expected.totalBytes+8191L)) {
            val disk=disk(); val metadata=metadata(expected); var reads=0
            val store=DurableSegmentStore(disk.directory,disk.sync,SegmentUsableSpace { path ->
                assertEquals(disk.directory.toPath().toAbsolutePath().normalize(),path);available
            },identity=disk.identity)
            assertThrows(SegmentDownloadException::class.java) {
                run(store,metadata,expected,EncryptedSegmentTransport { _,_,_ -> reads++;error("Must not read") })
            }
            assertEquals(0,reads);assertTrue(metadata.value.pendingReceipts.isEmpty())
            assertEquals(listOf(".store.lock"),disk.directory.list()!!.toList())
        }
        val disk=disk();val metadata=metadata(expected)
        val failed=DurableSegmentStore(disk.directory,disk.sync,SegmentUsableSpace { throw SecurityException("Synthetic query failure") },identity=disk.identity)
        assertThrows(SegmentDownloadException::class.java) { run(failed,metadata,expected,transport(expected,bytes)) }
        assertEquals(listOf(".store.lock"),disk.directory.list()!!.toList())
        assertTrue(metadata.value.pendingReceipts.isEmpty())
        // The failed query released the store lock; a later explicit attempt can
        // admit only when the original exact free-space boundary is satisfied.
        val sufficient=DurableSegmentStore(disk.directory,disk.sync,SegmentUsableSpace { expected.totalBytes+8192L },identity=disk.identity)
        run(sufficient,metadata,expected,transport(expected,bytes))
        assertEquals(setOf(expected.segment),metadata.value.pendingReceipts)
    }

    @Test fun cancellationOrDeadlineDuringPublicationNeverCommitsReceipt() {
        for(step in listOf(SegmentDiskStep.CONTAINER_LINKED,SegmentDiskStep.DIRECTORY_SYNCED)) {
            for(cancel in listOf(false,true)) {
                val bytes=payload(10);val expected=expectation(bytes);val disk=disk();val metadata=metadata(expected)
                var cancelled=false;var time=0L
                val store=disk.store { if(it==step) { if(cancel)cancelled=true else time=SegmentDownloadEngine.MAX_JOB_MILLIS } }
                val core=metadata.coordinator();val ticket=core.beginWork(RecordingWork.DOWNLOAD,expected.segment)
                try {
                    val engine=SegmentDownloadEngine(store) { time }
                    if(cancel) assertThrows(CancellationException::class.java) {
                        engine.download(expected,core,ticket,transport(expected,bytes),{cancelled})
                    } else assertThrows(SegmentDownloadException::class.java) {
                        engine.download(expected,core,ticket,transport(expected,bytes))
                    }
                    assertTrue(core.requiresReconciliation());assertNull(core.nextReceipt())
                    assertTrue(metadata.value.phoneSegments.isEmpty());assertTrue(metadata.value.pendingReceipts.isEmpty())
                    assertArrayEquals(bytes,file(disk,expected,".segment").readBytes())
                } finally { core.close() }
                // A fresh explicit ticket revalidates/re-fsyncs that orphan and
                // commits metadata without repeating a network read.
                val result=run(disk.store(),metadata,expected,EncryptedSegmentTransport { _,_,_ -> error("Already on disk") })
                assertTrue(result.reusedVerifiedFile)
                assertEquals(setOf(expected.segment),metadata.value.pendingReceipts)
            }
        }
    }

    @Test fun storeContentionDoesNotWriteStickyFaultOrDamageAnExistingOwner() {
        val expected=expectation(payload());val disk=disk()
        disk.store().open(expected).use { first ->
            assertEquals(0,first.offset)
            assertThrows(SegmentStorageBusyException::class.java) { disk.store().verifiedOnDisk(expected) }
            assertFalse(file(disk,expected,".fault").exists())
            first.append(payload().copyOfRange(0,4096))
            assertEquals(4096,first.offset)
        }
        disk.store().open(expected).use { assertEquals(4096,it.offset) }
    }

    @Test fun movedSourcePublicationInterruptionReopensWithoutRedownload() {
        // This injected host operation models the moved-inode layout only. The
        // Android native suite proves renameat2 flags and atomic no-replace use.
        val moved=SegmentPublication { _,source,destination -> Files.move(source,destination);Unit }
        for(interruptAt in listOf<SegmentDiskStep?>(null,SegmentDiskStep.CONTAINER_LINKED,SegmentDiskStep.DIRECTORY_SYNCED)) {
            val bytes=payload(10);val expected=expectation(bytes);val disk=disk();val metadata=metadata(expected)
            var interrupted=false
            val store=DurableSegmentStore(disk.directory,disk.sync,moved,{ step ->
                if(!interrupted && step==interruptAt) { interrupted=true;error("Synthetic publication interruption") }
            },Unit,disk.identity)
            if(interruptAt==null) run(store,metadata,expected,transport(expected,bytes))
            else {
                assertThrows(SegmentDownloadException::class.java) { run(store,metadata,expected,transport(expected,bytes)) }
                assertTrue(interrupted);assertTrue(metadata.value.pendingReceipts.isEmpty())
            }
            assertFalse(file(disk,expected,".part").exists())
            assertArrayEquals(bytes,file(disk,expected,".segment").readBytes())
            assertTrue(disk.store().verifiedOnDisk(expected))
            if(interruptAt!=null) {
                val result=run(disk.store(),metadata,expected,EncryptedSegmentTransport { _,_,_ -> error("Must not redownload") })
                assertTrue(result.reusedVerifiedFile)
            }
            assertEquals(setOf(expected.segment),metadata.value.pendingReceipts)
        }
    }

    @Test fun publisherRefusesExistingDestinationWithoutReplacingEitherFile() {
        val bytes=payload(10);val expected=expectation(bytes);val disk=disk();val metadata=metadata(expected)
        val occupied=ByteArray(240){91}
        val publish=SegmentPublication { _,source,destination ->
            // Model a competing destination appearing at the publication point.
            Files.write(destination,occupied)
            Files.move(source,destination) //No REPLACE_EXISTING; must fail.
            Unit
        }
        val store=DurableSegmentStore(disk.directory,disk.sync,publish,{},Unit,disk.identity)
        assertThrows(SegmentDownloadException::class.java) { run(store,metadata,expected,transport(expected,bytes)) }
        assertArrayEquals(occupied,file(disk,expected,".segment").readBytes())
        assertArrayEquals(bytes,file(disk,expected,".part").readBytes())
        assertTrue(metadata.value.pendingReceipts.isEmpty())
        assertThrows(SegmentStorageException::class.java) { disk.store().verifiedOnDisk(expected) }
    }

    @Test fun copiedInodeIsNotAcceptedAsAtomicPublicationOrRecoveryEvidence() {
        val bytes=payload(10);val expected=expectation(bytes);val disk=disk();val metadata=metadata(expected)
        val copying=SegmentPublication { _,source,destination -> Files.copy(source,destination);Unit }
        val store=DurableSegmentStore(disk.directory,disk.sync,copying,{},Unit,disk.identity)
        assertThrows(SegmentDownloadException::class.java) { run(store,metadata,expected,transport(expected,bytes)) }
        assertTrue(metadata.value.pendingReceipts.isEmpty())
        assertThrows(SegmentStorageException::class.java) { disk.store().verifiedOnDisk(expected) }
        assertTrue(file(disk,expected,".fault").exists())
    }

    @Test fun playbackReadVerifiesExactPublishedBytesWithoutAnyWritesOrRecovery() {
        val bytes=payload(10);val expected=expectation(bytes);val disk=disk();val metadata=metadata(expected)
        run(disk.store(),metadata,expected,transport(expected,bytes))
        val before=disk.directory.listFiles()!!.associate { it.name to it.readBytes().toList() }
        val barriers=disk.barriers
        val read=disk.store().readPublished(expected)
        assertArrayEquals(bytes,read);read.fill(0)
        assertEquals(before,disk.directory.listFiles()!!.associate { it.name to it.readBytes().toList() })
        assertEquals(barriers,disk.barriers)
        disk.store().open(expected).use {
            assertThrows(SegmentStorageBusyException::class.java){disk.store().readPublished(expected)}
        }
    }

    @Test fun playbackReadNeverRepairsPartialOrMissingSlotAndTamperIsFenced() {
        val bytes=payload(10);val expected=expectation(bytes)
        val empty=disk()
        assertThrows(SegmentStorageException::class.java){empty.store().readPublished(expected)}
        assertTrue(empty.directory.listFiles()!!.isEmpty())
        val disk=disk()
        disk.store().open(expected).use { it.append(bytes.copyOfRange(0,10)) }
        val before=disk.directory.listFiles()!!.associate { it.name to it.readBytes().toList() }
        assertThrows(SegmentStorageException::class.java){disk.store().readPublished(expected)}
        assertEquals(before,disk.directory.listFiles()!!.associate { it.name to it.readBytes().toList() })
        run(disk.store(),metadata(expected),expected,transport(expected,bytes))
        val store=disk.store()
        RandomAccessFile(file(disk,expected,".segment"),"rw").use{it.seek(210);it.write(99)}
        assertThrows(SegmentStorageException::class.java){store.readPublished(expected)}
        file(disk,expected,".segment").writeBytes(bytes)
        assertThrows(SegmentStorageException::class.java){store.readPublished(expected)}
    }

    @Test fun playbackReadRejectsReplacedIntentInodeEvenWithIdenticalMetadata() {
        for(changeBytes in listOf(false,true)) {
            val bytes=payload(10);val expected=expectation(bytes);val disk=disk()
            run(disk.store(),metadata(expected),expected,transport(expected,bytes))
            val intent=file(disk,expected,".intent").toPath();var replaced=false
            val changed=SegmentFileIdentity { path,directory ->
                val original=disk.identity.key(path,directory)
                if(!directory && path.fileName.toString().endsWith(".segment") && !replaced) {
                    replaced=true;val body=Files.readAllBytes(intent)
                    if(changeBytes)body[20]=(body[20].toInt() xor 1).toByte()
                    Files.delete(intent);Files.write(intent,body)
                }
                original
            }
            val store=DurableSegmentStore(disk.directory,disk.sync,changed)
            assertThrows(SegmentStorageException::class.java){store.readPublished(expected)}
            assertTrue(replaced);assertArrayEquals(bytes,file(disk,expected,".segment").readBytes())
        }
    }

    @Test fun pendingDeletionCancelsPlaybackAdmissionWhileCiphertextStoreIsBusy() {
        val bytes=payload(10);val expected=expectation(bytes);val disk=disk();val metadata=metadata(expected)
        run(disk.store(),metadata,expected,transport(expected,bytes))
        val core=metadata.coordinator()
        val ticket=core.beginPlayback(metadata.value.manifest!!)
        disk.store().open(expected).use {
            core.requestDeletion(UUID(7,8),DeleteLocation.PHONE_ONLY,true)
            assertFalse(core.isCurrent(ticket))
            assertThrows(SegmentStorageBusyException::class.java){disk.store().readPublished(expected)}
            assertTrue(metadata.value.downloadSuppressed)
        }
        assertFalse(core.playbackStep(ticket){fail("Deleted playback cannot write")})
        assertThrows(IllegalArgumentException::class.java){core.beginPlayback(metadata.value.manifest!!)}
        assertArrayEquals(bytes,file(disk,expected,".segment").readBytes());core.close()
    }

    @Test fun witnessIdentityUsesActualInodesAndDetectsPhysicalRootReplacement() {
        val disk=disk();val root=disk.directory.toPath()
        Files.createFile(root.resolve(".store.lock"))
        val before=disk.identity.key(root,true)
        val moved=root.resolveSibling(root.fileName.toString()+"-moved")
        Files.move(root,moved)
        Files.createDirectory(root);Files.createFile(root.resolve(".store.lock"))
        assertNotEquals(before,disk.identity.key(root,true))
        assertEquals(before,disk.identity.key(moved,true))
        val file=Files.write(root.resolve("original"),byteArrayOf(1,2,3))
        val same=Files.createLink(root.resolve("same"),file)
        val copy=Files.copy(file,root.resolve("copy"))
        assertEquals(disk.identity.key(file,false),disk.identity.key(same,false))
        assertNotEquals(disk.identity.key(file,false),disk.identity.key(copy,false))
    }
}
