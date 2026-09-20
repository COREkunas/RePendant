package org.openpendant.app

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.util.UUID
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Generated ciphertext/header fixtures, actual OP ResponseGate, canonical
 * catalog/manifest codecs, actual disk checkpoints and CAS coordinator. No
 * Bluetooth, owner key, HPKE/audio decode or claimed Android fsync proof. */
class DurableBleRecordingTransportTest {
    @get:Rule val temporary = TemporaryFolder(File("build/wire-sync-scratch").apply { mkdirs() })
    private val volume = RecordingVolume(UUID(1, 2), UUID(3, 4), 7)
    private val connection = DurableSyncConnection(UUID(5, 6), volume, "12".repeat(32))
    private val recording = DurableRecordingId(volume, UUID(7, 8))
    private val other = DurableRecordingId(volume, UUID(9, 10))
    private class ObjectData(val entry: DurableCatalogEntry, val manifest: ByteArray,
        val containers: Map<SegmentIdentity, ByteArray>)
    private fun data(recording: DurableRecordingId, frames: List<Int> = listOf(80, 1), full: Boolean = false): ObjectData {
        var sample = 0L
        val payloads = linkedMapOf<SegmentIdentity, ByteArray>()
        val timeline = frames.mapIndexed { sequence, count ->
            val size = 80 + (count + 1) * 68
            val bytes = EncryptedSegmentHeader.encode(recording, ByteArray(32) { 0x12 }, sequence, size) +
                byteArrayOf(4) + ByteArray(64 + size + 16) { (it * 13 + sequence).toByte() }
            val identity = SegmentIdentity(recording, sequence, digestHex(bytes), bytes.size.toLong())
            payloads[identity] = bytes
            DurableManifestSegment(identity, sample, sample + count * 320L, count * 320, false).also { sample = it.nextSample }
        }
        val manifest = DurableManifestCodec.encode(recording, connection.recipientFingerprint, DurableManifestState.FINALIZED, timeline, full)
        return ObjectData(DurableCatalogEntry(recording, frames.size * 4L + 1, digestHex(manifest), frames.size, true), manifest, payloads)
    }
    private class Metadata : DurableRecordingMetadata {
        val rows = linkedMapOf<DurableRecordingId, RecordingSyncSnapshot>()
        val owners = RecordingSyncOwnership()
        var commits = 0
        override fun snapshots(deviceId: UUID) = rows.values.filter { it.recording.volume.deviceId == deviceId }
        override fun invalidateOtherVolumes(current: RecordingVolume) { check(rows.keys.all { it.volume == current }) }
        override fun createRecording(recording: DurableRecordingId) { rows.putIfAbsent(recording, RecordingSyncSnapshot(recording)) }
        override fun coordinator(recording: DurableRecordingId, verifyPhoneSegment: (SegmentIdentity) -> Boolean): RecordingSyncContract {
            val row = rows.getValue(recording)
            check(row.deletions.none { it.phonePending })
            check(row.phoneSegments.all(verifyPhoneSegment))
            return core(recording)
        }
        fun core(recording: DurableRecordingId) = RecordingSyncContract(rows.getValue(recording), owners) { previous, next ->
            check(rows.getValue(recording).revision == previous); commits++; rows[recording] = next
        }
    }
    private class Witness(private val root: Path) : SegmentFileIdentity {
        private val witnesses = mutableListOf<Pair<Path, Any>>()
        override fun key(path: Path, isDirectory: Boolean): Any {
            if (isDirectory) check(Files.isDirectory(path, NOFOLLOW_LINKS))
            val actual = if (isDirectory) path.resolve(".store.lock") else path
            check(Files.isRegularFile(actual, NOFOLLOW_LINKS))
            val key = witnesses.firstOrNull { Files.isSameFile(actual, it.first) }?.second ?: Any().also {
                val witness = root.resolve("w-${witnesses.size}"); Files.createLink(witness, actual); witnesses += witness to it
            }
            return isDirectory to key
        }
    }
    private inner class Fixture(vararg val objects: ObjectData) {
        val metadata = Metadata()
        val files = DurableSegmentStore(temporary.newFolder(), SegmentDirectorySync { check(Files.isDirectory(it)) }, Witness(temporary.newFolder().toPath()))
        val ownership = DurableTransportOwnership()
        var now = 1000L
        var epoch = 20L
        lateinit var wire: Wire
        fun adapter(configure: (Wire) -> Unit = {}): Pair<DurableSyncConnection, DurableBleRecordingTransport> {
            val current = connection.copy(epoch = UUID(5, epoch++))
            wire = Wire(current, objects.toList(), metadata, ownership).also(configure)
            return current to DurableBleRecordingTransport(current, wire,
                RetainedDurableDeletion.fromSnapshots(current, metadata.rows.values.toList()), UUID(21, epoch))
        }
        fun run(configure: (Wire) -> Unit = {}): DurableSyncResult {
            val (current, transport) = adapter(configure)
            return DurableRecordingSyncSession(current, transport, metadata, files, clockMillis = { now }).run()
        }
    }
    private inner class Wire(val current: DurableSyncConnection, values: List<ObjectData>,
        val metadata: Metadata, val ownership: DurableTransportOwnership) : DurableBleExchange {
        var caps: DurableSyncCapabilities? = DurableSyncCapabilities(true, true, true, true)
        var connected = true
        var cancels = 0
        var sequence = 1
        val objects = values.associateBy { it.entry.recording.recordingId }.toMutableMap()
        val calls = mutableListOf<Pair<Int, Int>>()
        val returned = mutableListOf<ByteArray>()
        val receipts = mutableListOf<SegmentIdentity>()
        val deleted = mutableListOf<UUID>()
        var onRequest: (DurableBleRequest, DurableSyncCall) -> Unit = { _, _ -> }
        var mutate: (DurableBleRequest, ByteArray) -> ByteArray = { _, bytes -> bytes }
        override fun capabilities(epoch: UUID) = caps?.takeIf { epoch == current.epoch && connected }
        override fun acquire(epoch: UUID) = ownership.acquire(epoch)
        override fun isCurrent(epoch: UUID) = connected && epoch == current.epoch
        override fun cancel(lease: DurableTransportLease) {
            cancels++; connected = false
            if (lease.revokeForTransportClose()) lease.cancelAfterTransportClosed()
        }
        override fun exchange(lease: DurableTransportLease, call: DurableSyncCall, request: DurableBleRequest): ByteArray {
            call.checkActive(); assertTrue(call.belongsTo(lease))
            val payload = request.payload()
            assertArrayEquals(request.encode(sequence), DurableBleCodec.encodeRequest(request.command, sequence, payload))
            val input = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
            val full = caps?.fullStorage == true
            assertEquals(full, DurableBleCodec.isFull(request.command))
            val command = request.command - if (full) 8 else 0
            val offsetAt = when(command) { 0x30 -> 16; 0x31 -> 68; 0x32 -> 36; else -> -1 }
            val offset = if(offsetAt < 0) 0 else if(full) input.getInt(offsetAt) else input.getShort(offsetAt).toInt() and 65535
            calls += request.command to offset
            onRequest(request, call)
            fun uuid(at: Int) = ByteBuffer.wrap(payload, at, 16).order(ByteOrder.BIG_ENDIAN).let { UUID(it.long, it.long) }
            val body = when (command) {
                0x30, 0x31, 0x32 -> {
                    val read = request as DurableBleReadRequest
                    val source = when (command) {
                        0x30 -> DurableBleCodec.encodeCatalog(current, uuid(0), 7, objects.values.map { it.entry }.sortedBy { it.recording.recordingId }, full)
                        0x31 -> objects.getValue(uuid(16)).manifest
                        else -> objects.getValue(uuid(16)).containers.entries.single { it.key.sequence == input.getInt(32) }.value
                    }
                    val prefix = if(full)24 else 20
                    ByteBuffer.allocate(prefix + minOf(read.maximum, source.size - offset)).order(ByteOrder.LITTLE_ENDIAN).apply {
                        put(payload, 0, 16)
                        if(full){putInt(offset);putInt(source.size)}else{putShort(offset.toShort());putShort(source.size.toShort())}
                        put(source, offset, capacity() - prefix)
                    }.array()
                }
                0x33 -> {
                    val segment = objects.getValue(uuid(16)).containers.keys.single { it.sequence == input.getInt(32) }
                    val row = metadata.rows.getValue(segment.recording)
                    assertTrue(segment in row.phoneSegments && segment in row.pendingReceipts)
                    receipts += segment; payload.copyOf()
                }
                0x36 -> {
                    check(full && caps?.receiptBatch == true)
                    val value = objects.getValue(uuid(16))
                    assertArrayEquals(hexBytes(value.entry.manifestSha256), payload.copyOfRange(40,72))
                    val first = input.getInt(32); val count = input.getInt(36)
                    assertTrue(count in 1..32)
                    val selected = value.containers.keys.filter { it.sequence in first until first+count }
                    assertEquals(count, selected.size)
                    for(segment in selected) {
                        val row = metadata.rows.getValue(segment.recording)
                        assertTrue(segment in row.phoneSegments && segment in row.pendingReceipts)
                    }
                    receipts += selected; payload.copyOf()
                }
                0x34 -> {
                    val intent = metadata.rows.values.flatMap { it.deletions }.single { it.operationId == uuid(16) }
                    assertEquals(PendantDeletion.PENDING, intent.pendant)
                    deleted += intent.operationId
                    val manifest = metadata.rows.getValue(intent.recording).manifest!!
                    payload.copyOf().also { ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putLong(64, manifest.revision + 1) }
                }
                else -> error("Unexpected opcode")
            }
            val changed = mutate(request, body)
            val packet = ByteBuffer.allocate(9 + changed.size).order(ByteOrder.LITTLE_ENDIAN).apply {
                put(79); put(80); put(1); put((request.command or 128).toByte()); putShort(sequence.toShort()); putShort((changed.size + 1).toShort()); put(0); put(changed)
            }.array()
            val gate = ResponseGate(request.command, sequence++)
            if (sequence % 2 == 0) { gate.notified(packet); assertFalse(gate.ready); gate.written(true) }
            else { gate.written(true); assertFalse(gate.ready); gate.notified(packet) }
            call.checkActive() // Every intermediate frame still owns SAME pending ticket.
            return gate.take().also { returned += it; packet.fill(0); body.fill(0); changed.fill(0); payload.fill(0) }
        }
    }

    @Test fun realWireTwoRecordingsThreeSegmentsDurableReceiptsAndNoSourceDeletion() {
        val f = Fixture(data(recording), data(other, listOf(2)))
        assertEquals(DurableSyncResult(2, 3, 3, 0, 0, 0, 0), f.run())
        assertTrue(f.wire.calls.count { it.first == 0x32 } > 100)
        assertEquals(3, f.wire.receipts.size); assertTrue(f.wire.deleted.isEmpty())
        assertEquals(1, f.wire.cancels); assertFalse(f.wire.connected); assertTrue(f.ownership.isIdle())
        assertTrue(f.wire.returned.all { b -> b.all { it == 0.toByte() } })
        assertTrue(f.metadata.rows.values.all { it.pendingReceipts.isEmpty() && it.pendantCopy == PendantCopy.PRESENT })
        assertEquals(0, f.run().segmentsPublished)
        assertTrue(f.wire.calls.none { it.first == 0x32 || it.first == 0x33 })
    }
    @Test fun fullWireBeyondOld32SegmentLimitPublishesReceiptsAndResumesCleanly() {
        val f = Fixture(data(recording, List(40) { 1 }, full = true))
        val first = f.run { it.caps = DurableSyncCapabilities(true,true,true,true,true) }
        assertEquals(40, first.segmentsPublished); assertEquals(40, first.receiptsConfirmed)
        assertTrue(f.wire.calls.all { it.first in 0x38..0x3c })
        assertEquals(0, f.run { it.caps = DurableSyncCapabilities(true,true,true,true,true) }.segmentsPublished)
        assertTrue(f.ownership.isIdle())
    }
    @Test fun fullWireBatchesExactManifestBoundReceiptsAndReplaysWithoutDownload() {
        val f = Fixture(data(recording, List(40) { 1 }, full = true))
        fun configure(w: Wire) { w.caps=DurableSyncCapabilities(true,true,true,true,true,receiptBatch=true) }
        val first=f.run(::configure)
        assertEquals(40,first.segmentsPublished);assertEquals(40,first.receiptsConfirmed)
        assertEquals(2,f.wire.calls.count{it.first==DurableBleCodec.FULL_RECEIVE_RANGE})
        assertFalse(f.wire.calls.any{it.first==DurableBleCodec.FULL_RECEIVE_ACK})
        assertEquals(40,f.wire.receipts.size);assertTrue(f.wire.deleted.isEmpty())
        assertTrue(f.metadata.rows.values.all{it.pendingReceipts.isEmpty()})
        assertEquals(0,f.run(::configure).segmentsPublished)
        assertFalse(f.wire.calls.any{it.first==DurableBleCodec.FULL_RECEIVE_RANGE})
        assertTrue(f.ownership.isIdle())
    }
    @Test fun wideFullWirePreservesPublicationReceiptsAndCutsRangeFrames() {
        val objectData=data(recording,listOf(500),full=true)
        val narrow=Fixture(objectData)
        assertEquals(1,narrow.run{it.caps=DurableSyncCapabilities(true,true,true,true,true)}.receiptsConfirmed)
        val wide=Fixture(objectData)
        assertEquals(1,wide.run{it.caps=DurableSyncCapabilities(true,true,true,true,true,true)}.receiptsConfirmed)
        val oldFrames=narrow.wire.calls.count{it.first==0x3a}
        val newFrames=wide.wire.calls.count{it.first==0x3a}
        assertTrue(newFrames*3<oldFrames)
        assertEquals(narrow.metadata.rows,wide.metadata.rows)
        assertTrue(wide.wire.returned.all{b->b.all{it==0.toByte()}})
        assertTrue(wide.ownership.isIdle());assertTrue(wide.wire.deleted.isEmpty())
    }
    @Test fun wideCapabilityCannotChangeDuringOwnedSession() {
        val f=Fixture(data(recording,listOf(500),full=true))
        assertThrows(DurableSyncException::class.java){f.run{w->
            w.caps=DurableSyncCapabilities(true,true,true,true,true,true)
            w.onRequest={_,_->w.caps=DurableSyncCapabilities(true,true,true,true,true,false)}
        }}
        assertTrue(f.wire.receipts.isEmpty());assertTrue(f.ownership.isIdle())
    }
    @Test fun fullWireFortyThousandFrameBatchEndsBeforeSequenceExhaustionAndFreshEpochResumes() {
        val f = Fixture(data(recording, List(80) { 500 }, full = true))
        fun full(w: Wire) { w.caps = DurableSyncCapabilities(true,true,true,true,true) }
        val first = f.run(::full)
        val firstEpoch = f.wire.current.epoch
        assertTrue(first.morePending)
        assertTrue(first.segmentsPublished in 1..79)
        assertTrue(f.wire.calls.size in 40_000..49_000)
        assertTrue(f.wire.sequence < 60_000)
        assertEquals(1, f.wire.cancels);assertTrue(f.ownership.isIdle())
        val saved = f.metadata.rows.getValue(recording).phoneSegments.map { it.sequence }.toSet()
        val second = f.run { w -> full(w);w.onRequest = { request, _ ->
            if(request.command == DurableBleCodec.FULL_GET_SEGMENT) {
                val sequence = ByteBuffer.wrap(request.payload()).order(ByteOrder.LITTLE_ENDIAN).getInt(32)
                assertFalse(sequence in saved)
            }
        } }
        assertNotEquals(firstEpoch, f.wire.current.epoch)
        assertFalse(second.morePending)
        assertEquals(80, first.segmentsPublished + second.segmentsPublished)
        assertEquals(80, first.receiptsConfirmed + second.receiptsConfirmed)
        assertEquals(80, f.metadata.rows.getValue(recording).phoneSegments.size)
        assertTrue(f.metadata.rows.getValue(recording).pendingReceipts.isEmpty())
        assertTrue(f.ownership.isIdle())
    }
    @Test fun interruptedWireResumesOnlyFsynced4096PrefixOnFreshEpochAndNonce() {
        val f = Fixture(data(recording))
        assertThrows(DurableSyncException::class.java) { f.run { wire -> wire.onRequest = { request, _ ->
            if (request is DurableBleReadRequest && request.command == 0x32 && request.offset >= 4200) error("Injected disconnect")
        } } }
        assertTrue(f.wire.receipts.isEmpty()); assertEquals(1, f.wire.cancels)
        assertEquals(2, f.run().segmentsPublished)
        assertEquals(4096, f.wire.calls.first { it.first == 0x32 }.second)
    }
    @Test fun lostReceiptReplyRetainsOutboxAndNeverRedownloadsPublishedSegment() {
        val f = Fixture(data(recording, listOf(1)))
        assertThrows(DurableSyncException::class.java) { f.run { wire -> wire.mutate = { request, bytes ->
            if (request.command == 0x33) bytes.also { it[71] = (it[71].toInt() xor 1).toByte() } else bytes
        } } }
        assertEquals(1, f.metadata.rows.getValue(recording).pendingReceipts.size)
        assertEquals(1, f.run().receiptsConfirmed)
        assertTrue(f.wire.calls.none { it.first == 0x32 || it.first == 0x34 })
    }
    @Test fun corruptedFullContainerCannotPublishOrReceiptDespiteValidFragments() {
        val f = Fixture(data(recording, listOf(1)))
        assertThrows(DurableSyncException::class.java) { f.run { wire -> wire.mutate = { request, bytes ->
            if (request.command == 0x32 && (request as DurableBleReadRequest).offset == 0) bytes[30] = (bytes[30].toInt() xor 1).toByte()
            bytes
        } } }
        assertTrue(f.metadata.rows.getValue(recording).phoneSegments.isEmpty()); assertTrue(f.wire.receipts.isEmpty())
    }
    @Test fun wrongNonceShortPaddingOffsetAndTotalRejectBeforeMetadataAdmission() {
        for (mode in 0..4) {
            val f = Fixture(data(recording, listOf(1)))
            assertThrows(DurableSyncException::class.java) { f.run { wire -> wire.mutate = { _, bytes ->
                when (mode) { 0 -> bytes.also { it[0] = (it[0].toInt() xor 1).toByte() }; 1 -> bytes.copyOf(bytes.size - 1)
                    2 -> bytes.copyOf(bytes.size + 1); 3 -> bytes.also { it[16] = 1 }; else -> bytes.also { it[18] = 1 } }
            } } }
            assertTrue(f.metadata.rows.isEmpty()); assertEquals(1, f.wire.cancels)
        }
    }
    @Test fun wrongManifestDigestAndRecipientFailBeforeAnySegmentRead() {
        for (at in listOf(80, 140)) {
            val original = data(recording, listOf(1))
            val changed = original.manifest.copyOf().also { it[at] = (it[at].toInt() xor 1).toByte() }
            val f = Fixture(ObjectData(original.entry, changed, original.containers))
            assertThrows(DurableSyncException::class.java) { f.run() }
            assertTrue(f.wire.calls.none { it.first in 0x32..0x34 })
        }
    }
    @Test fun lateEpochDuringFragmentCannotAckOrPublishAndWipesReturnedBytes() {
        val f = Fixture(data(recording, listOf(1)))
        assertThrows(DurableSyncException::class.java) { f.run { wire -> wire.mutate = { request, bytes ->
            if (request.command == 0x32) wire.connected = false; bytes
        } } }
        assertTrue(f.wire.receipts.isEmpty()); assertTrue(f.wire.returned.all { b -> b.all { it == 0.toByte() } })
    }
    @Test fun logicalDeadlineAppliesAcrossAllFragmentsWithoutRenewal() {
        val f = Fixture(data(recording, listOf(1)))
        assertThrows(DurableSyncException::class.java) { f.run { wire -> wire.onRequest = { _, _ -> f.now += 10_000 } } }
        assertTrue(f.wire.calls.size <= 3); assertTrue(f.metadata.rows.isEmpty())
    }
    @Test fun persistedDeleteWorksAfterCatalogOmissionAndPreservesPhoneCopy() {
        val f = Fixture(data(recording, listOf(1)))
        f.run()
        val core = f.metadata.core(recording)
        try { core.authenticatedConnection(volume, true); core.requestDeletion(UUID(31, 32), DeleteLocation.PENDANT_ONLY, false) }
        finally { core.close() }
        assertEquals(1, f.run { it.objects.clear() }.tombstonesConfirmed)
        val row = f.metadata.rows.getValue(recording)
        assertEquals(PendantCopy.DELETED, row.pendantCopy); assertEquals(1, row.phoneSegments.size)
        assertTrue(f.wire.calls.none { it.first in 0x31..0x33 })
    }
    @Test fun staleTombstoneNeverConfirmsOrChangesExistingPhoneState() {
        val f = Fixture(data(recording, listOf(1))); f.run()
        val core = f.metadata.core(recording)
        try { core.authenticatedConnection(volume, true); core.requestDeletion(UUID(31, 32), DeleteLocation.PENDANT_ONLY, true) }
        finally { core.close() }
        assertThrows(DurableSyncException::class.java) { f.run { wire -> wire.objects.clear(); wire.mutate = { request, bytes ->
            if (request.command == 0x34) ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putLong(64, 5); bytes
        } } }
        assertEquals(PendantDeletion.PENDING, f.metadata.rows.getValue(recording).deletions.single().pendant)
        assertFalse(f.metadata.rows.getValue(recording).downloadSuppressed) // PENDANT_ONLY preserves local policy.
        assertEquals(1, f.metadata.rows.getValue(recording).phoneSegments.size)
        assertTrue(f.wire.calls.none { it.first in 0x31..0x33 })
    }
    @Test fun phoneDeletionPendingNeverReadsManifestCiphertextOrEmitsRemoteMutation() {
        val f = Fixture(data(recording, listOf(1))); f.run()
        val core = f.metadata.core(recording)
        try { core.authenticatedConnection(volume, true); core.requestDeletion(UUID(31, 32), DeleteLocation.PHONE_ONLY, true) }
        finally { core.close() }
        assertEquals(1, f.run().pendingPhoneDeletion)
        assertTrue(f.wire.calls.all { it.first == 0x30 })
    }
    @Test fun missingCapabilityRefusesBeforeWireOrMetadataAndAdapterIsSingleUse() {
        val f = Fixture(data(recording))
        val (current, adapter) = f.adapter { it.caps = DurableSyncCapabilities(true, true, true, false) }
        assertThrows(DurableSyncException::class.java) { DurableRecordingSyncSession(current, adapter, f.metadata, f.files, clockMillis = { f.now }).run() }
        assertTrue(f.metadata.rows.isEmpty()); assertTrue(f.wire.calls.isEmpty())
        assertThrows(IllegalStateException::class.java) { adapter.acquireSession(current) }
    }
    @Test fun retainedDeleteRejectsWrongIdentityHashActiveRevisionOrUnpersistedIntent() {
        val objectData = data(recording, listOf(1))
        val manifest = DurableManifestCodec.parse(objectData.manifest, objectData.entry, connection.recipientFingerprint).manifest
        val intent = RecordingDeletionIntent(UUID(31, 32), recording, manifest.sha256, DeleteLocation.PENDANT_ONLY, true, false, PendantDeletion.PENDING)
        for (bad in listOf(intent.copy(recording = other), intent.copy(manifestSha256 = "11".repeat(32)),
            intent.copy(location = DeleteLocation.PHONE_ONLY), intent.copy(pendant = PendantDeletion.CONFIRMED))) {
            assertThrows(IllegalArgumentException::class.java) { RetainedDurableDeletion(bad, manifest) }
        }
        assertThrows(IllegalArgumentException::class.java) { RetainedDurableDeletion(intent, RecordingManifest(recording, 4, false, manifest.sha256, manifest.segments)) }
    }
    @Test fun missingRetainedDeletionContextRefusesBeforeDeleteFrame() {
        val f = Fixture(data(recording, listOf(1))); f.run()
        val core = f.metadata.core(recording)
        try { core.authenticatedConnection(volume, true); core.requestDeletion(UUID(31, 32), DeleteLocation.PENDANT_ONLY) }
        finally { core.close() }
        val (current, _) = f.adapter { it.objects.clear() }
        val unbound = DurableBleRecordingTransport(current, f.wire)
        assertThrows(DurableSyncException::class.java) { DurableRecordingSyncSession(current, unbound, f.metadata, f.files, clockMillis = { f.now }).run() }
        assertTrue(f.wire.calls.all { it.first == 0x30 })
        assertEquals(PendantDeletion.PENDING, f.metadata.rows.getValue(recording).deletions.single().pendant)
    }
    @Test fun reentrantOperationAndSecondSessionCannotDisruptAdmittedRadioJob() {
        val f = Fixture(data(recording, listOf(1)))
        val (current, adapter) = f.adapter()
        var attempted = false
        f.wire.onRequest = { _, call ->
            if (!attempted) {
                attempted = true
                assertThrows(IllegalStateException::class.java) { adapter.catalog(0, 8, null, call) }
                assertThrows(DurableTransportBusyException::class.java) { f.ownership.acquire(current.epoch) }
                call.checkActive()
            }
        }
        assertEquals(1, DurableRecordingSyncSession(current, adapter, f.metadata, f.files, clockMillis = { f.now }).run().receiptsConfirmed)
        assertTrue(attempted); assertEquals(1, f.wire.cancels) // Only terminal disconnect, not reentry failure.
    }
    @Test fun outerMalformedFramePermanentlyClosesMetadataAssembler() {
        val first = DurableBleCodec.catalog(UUID(1, 2), 0)
        val assembler = DurableBleMetadataAssembler(first)
        assertThrows(ProtocolException::class.java) { assembler.accept(first, 1, byteArrayOf()) }
        assertThrows(IllegalStateException::class.java) { assembler.finish() }
    }
    @Test fun full5120ManifestIndexedReadsKeepExactWireBindingAndRejectBeforeAnyFrame() {
        val objects = data(recording, List(5120) { 1 }, full = true)
        val last = objects.containers.keys.last()
        val invalid = listOf(last.copy(sha256="fe".repeat(32)), last.copy(byteCount=426),
            last.copy(sequence=5120), last.copy(sequence=Int.MAX_VALUE),
            last.copy(recording=other),
            last.copy(recording=recording.copy(volume=volume.copy(generation=volume.generation+1))),
            last.copy(recording=recording.copy(volume=volume.copy(deviceId=UUID(51,52)))),
            last.copy(recording=recording.copy(volume=volume.copy(volumeId=UUID(53,54)))))
        // Every negative case gets its own admitted epoch: the first rejected
        // operation permanently fences the transport, not just that selector.
        for (bad in invalid) for (receipt in listOf(false,true)) {
            val f = Fixture(objects)
            val (current, adapter) = f.adapter { it.caps=DurableSyncCapabilities(true,true,true,true,true,true) }
            val lease = adapter.acquireSession(current)
            fun call() = DurableSyncCall(current, f.now+30_000, lease.beginRequest()) {
                check(f.wire.connected && f.now<31_000)
            }
            try {
                val catalogCall=call()
                val catalog=adapter.catalog(0,8,null,catalogCall)
                catalogCall.validateReply()
                assertEquals(listOf(objects.entry),catalog.entries)
                val manifestCall=call()
                val frozen=adapter.manifest(objects.entry,manifestCall).manifest
                manifestCall.validateReply()
                assertEquals(5120,frozen.segments.size)
                for (position in listOf(0,2559,5119)) {
                    val wanted=frozen.segments[position].copy()
                    val rangeCall=call()
                    val range=adapter.read(wanted,0,4096,rangeCall)
                    rangeCall.validateReply()
                    assertEquals(wanted,range.chunk.segment)
                    assertArrayEquals(objects.containers.getValue(wanted),range.chunk.bytes)
                    range.chunk.bytes.fill(0)
                }
                val frames=f.wire.calls.size
                val rejectedCall=call()
                assertThrows(IllegalArgumentException::class.java) {
                    if(receipt) adapter.receipt(bad,rejectedCall) else adapter.read(bad,0,4096,rejectedCall)
                }
                assertEquals(frames,f.wire.calls.size)
                assertTrue(f.wire.receipts.isEmpty());assertTrue(f.wire.deleted.isEmpty())
                assertEquals(1,f.wire.cancels);assertFalse(f.wire.connected)
                assertFalse(adapter.isCurrent(current));assertTrue(f.ownership.isIdle())
                assertTrue(f.wire.returned.all { bytes->bytes.all { it==0.toByte() } })
                assertTrue(f.metadata.rows.isEmpty()) // No file/coordinator admission.
            } finally {
                adapter.endSession(lease);assertTrue(lease.retire())
            }
            assertEquals(1,f.wire.cancels)
        }
    }

    @Test fun catalogAloneDoesNotAuthorizeAnIndexedSegmentBeforeManifestValidation() {
        val objects=data(recording,listOf(1),full=true)
        for (receipt in listOf(false,true)) {
            val f=Fixture(objects)
            val (current,adapter)=f.adapter { it.caps=DurableSyncCapabilities(true,true,true,true,true,true) }
            val lease=adapter.acquireSession(current)
            try {
                val catalogCall=DurableSyncCall(current,f.now+30_000,lease.beginRequest()) {}
                adapter.catalog(0,8,null,catalogCall);catalogCall.validateReply()
                val before=f.wire.calls.size
                val untrusted=objects.containers.keys.single()
                val call=DurableSyncCall(current,f.now+30_000,lease.beginRequest()) {}
                assertThrows(IllegalArgumentException::class.java) {
                    if(receipt) adapter.receipt(untrusted,call) else adapter.read(untrusted,0,4096,call)
                }
                assertEquals(before,f.wire.calls.size)
                assertEquals(1,f.wire.cancels);assertTrue(f.ownership.isIdle())
                assertTrue(f.metadata.rows.isEmpty())
            } finally { adapter.endSession(lease);assertTrue(lease.retire()) }
        }
    }
    @Test fun negotiatedInfoAllowsOnlyReviewedBitAndStrictBodyAdmissionMatchesBuilder() {
        for (bits in listOf(15L, 31L, 47L, 63L)) {
            val info = byteArrayOf(0, 1, 0, 0, 0, 0, 0, 0); OpProtocol.put32(info, 2, bits)
            assertEquals(bits, DurableBleCodec.validatedCapabilities(info))
            info[7] = 1; assertThrows(ProtocolException::class.java) { DurableBleCodec.validatedCapabilities(info) }
        }
        for (bits in listOf(0L, 32L, 64L, 127L, 0xffffffffL)) {
            val info = byteArrayOf(0, 1, 0, 0, 0, 0, 0, 0); OpProtocol.put32(info, 2, bits)
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.validatedCapabilities(info) }
        }
        val request = DurableBleCodec.catalog(UUID(1, 2), 0)
        for (at in listOf(18, 19)) {
            val bad = request.payload(); bad[at] = 53
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.encodeRequest(0x30, 1, bad) }
        }
    }
}
