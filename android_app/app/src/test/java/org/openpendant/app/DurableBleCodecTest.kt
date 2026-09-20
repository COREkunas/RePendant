package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class DurableBleCodecTest {
    private val nonce = UUID.fromString("a1a2a3a4-a5a6-a7a8-a9aa-abacadaeafb0")
    private val volume = RecordingVolume(UUID.fromString("11121314-1516-1718-191a-1b1c1d1e1f20"),
        UUID.fromString("21222324-2526-2728-292a-2b2c2d2e2f30"), 0x0102030405060708L)
    private val recording = DurableRecordingId(volume, UUID.fromString("31323334-3536-3738-393a-3b3c3d3e3f40"))
    private val connection = DurableSyncConnection(UUID(31, 32), volume,
        "4142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60")
    private val row = DurableCatalogEntry(recording, 13,
        "78b67f5fe13886b05d3425b73af30ccaaa9ceda2812320625ac6ae6b49131779", 3, true)
    private val second = DurableCatalogEntry(DurableRecordingId(volume,
        UUID.fromString("71727374-7576-7778-797a-7b7c7d7e7f80")), 2,
        "487557487778129d63dd4ad9fc1700bd180c608be75a0be79176aaa345084d16", 0, true)
    private val segment = SegmentIdentity(recording, 1,
        "752e2d463641c5df3b3f6e0c6ccc3712de32271bcab3a72d74b767bc82cbf980", 34357)
    private val intent = RecordingDeletionIntent(UUID.fromString("c1c2c3c4-c5c6-c7c8-c9ca-cbcccdcecfd0"),
        recording, row.manifestSha256, DeleteLocation.PENDANT_ONLY, false, false, PendantDeletion.PENDING)
    private val json get() = javaClass.getResourceAsStream("/durable_ble_v1_public.json")!!.use { it.readBytes().toString(Charsets.US_ASCII) }
    private fun unhex(s: String) = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    private fun fixture(group: String, name: String): ByteArray {
        val objectBody = Regex("\"$group\": \\{([^}]+)\\}").find(json)!!.groupValues[1]
        return unhex(Regex("\"$name\": \"([0-9a-f]+)\"").find(objectBody)!!.groupValues[1])
    }
    private fun catalogBytes() = fixture("catalog", "hex")
    private fun put16(b: ByteArray, at: Int, value: Int) { ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).putShort(at, value.toShort()) }
    private fun put32(b: ByteArray, at: Int, value: Int) { ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).putInt(at, value) }
    private fun put64(b: ByteArray, at: Int, value: Long) { ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).putLong(at, value) }
    private fun response(command: Int, sequence: Int, body: ByteArray, status: Int = 0) =
        ByteBuffer.allocate(9 + body.size).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(79); put(80); put(1); put((command or 128).toByte()); putShort(sequence.toShort())
            putShort((body.size + 1).toShort()); put(status.toByte()); put(body)
        }.array()
    private fun fragment(request: DurableBleReadRequest, sequence: Int, bytes: ByteArray): ByteArray {
        val payload = request.payload().copyOfRange(0, 16) + ByteArray(4)
        put16(payload, 16, request.offset); put16(payload, 18, bytes.size)
        return response(request.command, sequence, payload + bytes.copyOfRange(request.offset, minOf(bytes.size, request.offset + request.maximum)))
    }
    private fun rejectsCatalog(change: (ByteArray) -> Unit) {
        val changed = catalogBytes(); change(changed)
        assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.parseCatalog(changed, connection, nonce) }
    }

    @Test fun exactIndependentPythonRequestsAllFitOriginalOpEnvelope() {
        val cases = mapOf("catalog" to DurableBleCodec.catalog(nonce, 0),
            "manifest" to DurableBleCodec.manifest(nonce, row, 0),
            "segment" to DurableBleCodec.segment(nonce, segment, 34320),
            "receipt" to DurableBleCodec.receipt(nonce, segment), "delete" to DurableBleCodec.delete(nonce, intent))
        for ((name, request) in cases) {
            assertArrayEquals(fixture("requests", name), request.encode(0x1234))
            assertTrue(request.encode(1).size <= 80)
            for (sequence in listOf(0, -1, 65536)) assertThrows(IllegalArgumentException::class.java) { request.encode(sequence) }
            val altered = request.payload(); altered.fill(0)
            assertArrayEquals(fixture("requests", name), request.encode(0x1234))
        }
        assertEquals(32L, DurableBleCodec.CAPABILITY)
        assertThrows(ProtocolException::class.java) { OpProtocol.encode(DurableBleCodec.GET_CATALOG, 1) }
    }

    @Test fun publicCatalogExactBytesIdentityHighIntegersAndImmutableEntries() {
        val expected = catalogBytes()
        assertArrayEquals(expected, DurableBleCodec.encodeCatalog(connection, nonce, 0x0102030405060709L, listOf(row, second)))
        assertEquals("c88dcaf59b707cab235108e706fa65ec72b8c17f58f9c2aefd793f2654fdf19f",
            MessageDigest.getInstance("SHA-256").digest(expected).joinToString("") { "%02x".format(it.toInt() and 255) })
        val parsed = DurableBleCodec.parseCatalog(expected, connection, nonce)
        expected.fill(0)
        assertEquals(listOf(row, second), parsed.entries); assertEquals(nonce, parsed.nonce)
        assertEquals(0x0102030405060709L, parsed.snapshotRevision); assertEquals(connection, parsed.connection)
        assertThrows(UnsupportedOperationException::class.java) { (parsed.entries as MutableList).clear() }
    }

    @Test fun allCountsStatesAndProducerSizesRoundTripWithoutTruncation() {
        for (count in 0..8) for (segments in 0..32) for (state in 0..2) {
            val rows = List(count) { index -> row.copy(recording = DurableRecordingId(volume, UUID(0, index + 1L)),
                manifestRevision = segments * 4L + state, sealedSegments = segments, finished = state != 0) }
            val bytes = DurableBleCodec.encodeCatalog(connection, nonce, Long.MAX_VALUE, rows)
            assertEquals(128 + count * 64, bytes.size)
            assertEquals(rows, DurableBleCodec.parseCatalog(bytes, connection, nonce).entries)
        }
        val oversized = object : AbstractList<DurableCatalogEntry>() {
            override val size = 9
            override fun get(index: Int): DurableCatalogEntry = throw AssertionError("Do not iterate oversized catalog")
        }
        assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.encodeCatalog(connection, nonce, 1, oversized) }
    }

    @Test fun catalogRejectsWrongBindingReservedFieldsUnknownFlagsAndCounts() {
        for (offset in listOf(0, 7, 8, 10, 12, 14, 16, 32, 48, 56, 80, 116, 120, 127)) rejectsCatalog { it[offset] = (it[offset].toInt() xor 1).toByte() }
        for (count in listOf(-1, 0, 1, 3, 8, 9, Int.MAX_VALUE)) rejectsCatalog { put32(it, 112, count) }
        for (snapshot in listOf(0L, -1L, Long.MIN_VALUE)) rejectsCatalog { put64(it, 72, snapshot) }
        for (fill in listOf(0, 255)) for (offset in listOf(16, 32, 56, 128)) rejectsCatalog { it.fill(fill.toByte(), offset, offset + 16) }
        for (fill in listOf(0, 255)) rejectsCatalog { it.fill(fill.toByte(), 160, 192) }
        assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.parseCatalog(catalogBytes(), connection.copy(recipientFingerprint = "12".repeat(32)), nonce) }
        assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.parseCatalog(catalogBytes(), connection, UUID(1, 2)) }
    }

    @Test fun catalogRejectsUnorderedDuplicateRevisionStateAndSentinelDigests() {
        rejectsCatalog { val first = it.copyOfRange(128, 192); it.copyInto(it, 128, 192, 256); first.copyInto(it, 192) }
        rejectsCatalog { it.copyInto(it, 192, 128, 144) }
        for (offset in listOf(144, 152, 156)) rejectsCatalog { put32(it, offset, -1) }
        rejectsCatalog { put64(it, 144, (1L shl 32) + 13) }
        rejectsCatalog { put32(it, 156, 0) }
        for (fill in listOf(0, 255)) rejectsCatalog { it.fill(fill.toByte(), 160, 192) }
        for (bad in listOf(row.copy(manifestRevision = Long.MAX_VALUE), row.copy(manifestRevision = 15),
            row.copy(sealedSegments = 33), row.copy(finished = false), row.copy(manifestSha256 = "ff".repeat(32)))) {
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.manifest(nonce, bad, 0) }
        }
    }

    @Test fun exactCatalogLengthRejectsTruncationTrailingAndOversizedInput() {
        val bytes = catalogBytes()
        for (length in 0 until bytes.size) assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.parseCatalog(bytes.copyOf(length), connection, nonce) }
        for (length in listOf(257, 320, 640, 641, 10000)) assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.parseCatalog(bytes.copyOf(length), connection, nonce) }
    }

    @Test fun independentFragmentResponsesMatchNonceOffsetsTotalsAndOwnedWipe() {
        val cases = mapOf("catalog" to DurableBleCodec.catalog(nonce, 0), "manifest" to DurableBleCodec.manifest(nonce, row, 0),
            "segment" to DurableBleCodec.segment(nonce, segment, 34320))
        for ((name, request) in cases) {
            val fragment = DurableBleCodec.readResponse(fixture("responses", name), 0x1234, request)
            assertEquals(request.offset, fragment.offset)
            assertEquals(if (name == "segment") 37 else 52, fragment.bytes.size)
            if (name == "segment") assertArrayEquals(ByteArray(37) { it.toByte() }, fragment.bytes)
            fragment.close(); assertTrue(fragment.bytes.all { it == 0.toByte() })
        }
    }

    @Test fun fragmentsRejectWrongNonceSequenceCommandOffsetTotalAndShortOrTrailingBytes() {
        val request = DurableBleCodec.segment(nonce, segment, 34320)
        val good = fixture("responses", "segment")
        for (offset in listOf(0, 1, 2, 3, 4, 5, 6, 7)) {
            val changed = good.copyOf(); changed[offset] = (changed[offset].toInt() xor 1).toByte()
            assertThrows(ProtocolException::class.java) { DurableBleCodec.readResponse(changed, 0x1234, request) }
        }
        for (offset in listOf(9, 24, 25, 26, 27, 28)) {
            val changed = good.copyOf(); changed[offset] = (changed[offset].toInt() xor 1).toByte()
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.readResponse(changed, 0x1234, request) }
        }
        for (extra in listOf(-1, 1)) {
            val changed = good.copyOf(good.size + extra); put16(changed, 6, changed.size - 8)
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.readResponse(changed, 0x1234, request) }
        }
        for (status in 1..255) {
            val changed = good.copyOf(); changed[8] = status.toByte()
            assertThrows(ProtocolException::class.java) { DurableBleCodec.readResponse(changed, 0x1234, request) }
        }
    }

    @Test fun requestBoundsRejectZeroSentinelOverflowAndUnsupportedProducerShape() {
        for (n in listOf(UUID(0, 0), UUID(-1, -1))) assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.catalog(n, 0) }
        for (maximum in listOf(-1, 0, 53, 256)) assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.catalog(nonce, 0, maximum) }
        for (offset in listOf(-1, 640, 65535, Int.MAX_VALUE)) assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.catalog(nonce, offset) }
        for (offset in listOf(-1, 320, 65535)) assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.manifest(nonce, row, offset) }
        for (bad in listOf(segment.copy(sequence = 32), segment.copy(byteCount = 424), segment.copy(byteCount = 426),
            segment.copy(byteCount = 34358), segment.copy(sha256 = "00".repeat(32)))) {
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.segment(nonce, bad, 0) }
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.receipt(nonce, bad) }
        }
    }

    @Test fun metadataAssemblySupportsEveryBoundedFragmentSizeAndExactFinish() {
        val bytes = catalogBytes()
        for (maximum in listOf(1, 7, 51, 52)) {
            val assembler = DurableBleMetadataAssembler(DurableBleCodec.catalog(nonce, 0, maximum))
            var seq = 1
            while (assembler.received < bytes.size) {
                val request = DurableBleCodec.catalog(nonce, assembler.received, maximum)
                assembler.accept(request, seq, fragment(request, seq++, bytes))
            }
            assertArrayEquals(bytes, assembler.finish())
            assertThrows(IllegalStateException::class.java) { assembler.finish() }
        }
    }

    @Test fun metadataAssemblyRejectsReplayWrongObjectChangedTotalAndPrematureFinishPermanently() {
        val bytes = catalogBytes(); val first = DurableBleCodec.catalog(nonce, 0)
        for (mode in 0..3) {
            val assembler = DurableBleMetadataAssembler(first)
            assembler.accept(first, 1, fragment(first, 1, bytes))
            if (mode == 3) assertThrows(IllegalArgumentException::class.java) { assembler.finish() }
            else {
                val request = if (mode == 0) first else DurableBleCodec.catalog(if (mode == 1) UUID(1, 2) else nonce, 52)
                val packet = fragment(request, 2, if (mode == 2) bytes.copyOf(320) else bytes)
                assertThrows(IllegalArgumentException::class.java) { assembler.accept(request, 2, packet) }
            }
            assertThrows(IllegalStateException::class.java) { assembler.accept(first, 3, fragment(first, 3, bytes)) }
        }
        assertThrows(IllegalArgumentException::class.java) { DurableBleMetadataAssembler(DurableBleCodec.segment(nonce, segment, 0)) }
    }

    @Test fun manifestAssemblyUsesExactTrustedIdentityHashAndMaximum2176() {
        val hex = Regex("\"state\": \"finalized\"[\\s\\S]*?\"hex\": \"([0-9a-f]+)\"").find(
            javaClass.getResourceAsStream("/durable_manifest_v1_public.json")!!.use { it.readBytes().toString(Charsets.US_ASCII) })!!.groupValues[1]
        val bytes = unhex(hex); val assembler = DurableBleMetadataAssembler(DurableBleCodec.manifest(nonce, row, 0))
        var seq = 1
        while (assembler.received < bytes.size) {
            val request = DurableBleCodec.manifest(nonce, row, assembler.received)
            assembler.accept(request, seq, fragment(request, seq++, bytes))
        }
        val parsed = DurableManifestCodec.parse(assembler.finish(), row, connection.recipientFingerprint)
        assertEquals(recording, parsed.manifest.recording); assertEquals(3, parsed.segments.size)
        val max = row.copy(manifestRevision = 130, sealedSegments = 32)
        assertEquals(2176, DurableBleCodec.manifest(nonce, max, 2175, 1).total)
    }

    @Test fun receiptSuccessRequiresExactAllFieldsAndNeverCreatesTombstone() {
        val good = fixture("responses", "receipt")
        assertEquals(DurableReceiptReply(connection.epoch, segment), DurableBleCodec.receiptResponse(good, 0x1234, nonce, connection, segment))
        for (offset in 9 until good.size) {
            val bad = good.copyOf(); bad[offset] = (bad[offset].toInt() xor 1).toByte()
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.receiptResponse(bad, 0x1234, nonce, connection, segment) }
        }
        assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.receiptResponse(good, 0x1234, nonce,
            connection.copy(volume = volume.copy(generation = 2)), segment) }
    }

    @Test fun deletionRequiresPersistedIntentEchoAndNewDurableTombstoneRevision() {
        val good = fixture("responses", "delete")
        val expected = DurableTombstoneReply(connection.epoch, recording, intent.operationId, row.manifestSha256, 14)
        assertEquals(expected, DurableBleCodec.deleteResponse(good, 0x1234, nonce, connection, intent, 13))
        for (offset in 9 until 73) {
            val changed = good.copyOf(); changed[offset] = (changed[offset].toInt() xor 1).toByte()
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.deleteResponse(changed, 0x1234, nonce, connection, intent, 13) }
        }
        for (revision in listOf(0L, 12L, 13L, -1L, Long.MIN_VALUE)) {
            val changed = good.copyOf(); put64(changed, 73, revision)
            assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.deleteResponse(changed, 0x1234, nonce, connection, intent, 13) }
        }
        for (revision in listOf(0L, 3L, 12L, 131L, Long.MAX_VALUE)) assertThrows(IllegalArgumentException::class.java) {
            DurableBleCodec.deleteResponse(good, 0x1234, nonce, connection, intent, revision)
        }
        for (bad in listOf(intent.copy(location = DeleteLocation.PHONE_ONLY), intent.copy(pendant = PendantDeletion.CONFIRMED),
            intent.copy(manifestSha256 = "ff".repeat(32)))) assertThrows(IllegalArgumentException::class.java) { DurableBleCodec.delete(nonce, bad) }
    }
}
