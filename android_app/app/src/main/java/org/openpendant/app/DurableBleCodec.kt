package org.openpendant.app

import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Collections
import java.util.UUID

/** Bounded wire codec. The authenticated GATT/session
 * adapter must enforce the original deadline and both-callback radio proof.
 */
object DurableBleCodec {
    const val CAPABILITY = 1L shl 5
    const val GET_CATALOG = 0x30
    const val GET_MANIFEST = 0x31
    const val GET_SEGMENT = 0x32
    const val RECEIVE_ACK = 0x33
    const val DELETE = 0x34
    const val FULL_CAPABILITY = 1L shl 7
    const val WIDE_CAPABILITY = 1L shl 9
    const val STREAM_CAPABILITY = 1L shl 10
    const val RECEIPT_BATCH_CAPABILITY = 1L shl 11
    const val BATTERY_SYNC_CAPABILITY = 1L shl 14
    const val FULL_RECEIVE_RANGE = 0x3e
    const val RECEIPT_BATCH_MAX = 32
    const val FULL_STREAM = 0x3d
    const val STREAM_MAX = 4096
    fun streamEnabled(bits: Long, mtu: Int) = wideEnabled(bits, mtu) && bits and STREAM_CAPABILITY != 0L
    const val WIDE_MAX_FRAGMENT = 192
    const val WIDE_MIN_MTU = 228 // 8 OP + 1 status + 24 selector + 192 data + 3 ATT
    fun wideEnabled(bits: Long, mtu: Int): Boolean =
        bits and FULL_CAPABILITY != 0L && bits and WIDE_CAPABILITY != 0L && mtu >= WIDE_MIN_MTU
    fun maxResponseBody(command: Int): Int =
        if (command == FULL_STREAM) 24 + STREAM_MAX
        else if (command in FULL_GET_CATALOG..FULL_GET_SEGMENT) 24 + WIDE_MAX_FRAGMENT else 72
    const val FULL_GET_CATALOG = 0x38
    const val FULL_GET_MANIFEST = 0x39
    const val FULL_GET_SEGMENT = 0x3a
    const val FULL_RECEIVE_ACK = 0x3b
    const val FULL_DELETE = 0x3c
    const val MAX_PAYLOAD = 74
    const val FULL_MAX_RECORDINGS = 32
    const val FULL_MAX_CATALOG_BYTES = 2176
    const val FULL_MAX_FRAGMENT = 48
    fun isFull(command: Int) = command in FULL_GET_CATALOG..FULL_RECEIVE_RANGE
    fun isDurable(command: Int) = command in GET_CATALOG..DELETE || isFull(command)
    private fun command(legacy: Int, full: Boolean) = legacy + if (full) 8 else 0
    const val MAX_FRAGMENT = 52
    const val CATALOG_HEADER = 128
    const val CATALOG_ENTRY = 64
    const val MAX_RECORDINGS = 8
    const val MAX_CATALOG_BYTES = CATALOG_HEADER + MAX_RECORDINGS * CATALOG_ENTRY
    private val catalogMagic = byteArrayOf(79, 80, 78, 68, 67, 84, 49, 0)

    fun catalog(nonce: UUID, offset: Int, maximum: Int = MAX_FRAGMENT, fullProfile: Boolean = false): DurableBleReadRequest {
        val limit = if (fullProfile) FULL_MAX_CATALOG_BYTES else MAX_CATALOG_BYTES
        nonce(nonce); range(offset, maximum, limit, fullProfile)
        val body = buffer(if (fullProfile) 22 else 20).apply {
            putUuid(nonce); if (fullProfile) putInt(offset) else putShort(offset.toShort()); put(maximum.toByte()); put(0)
        }.array()
        return DurableBleReadRequest(command(GET_CATALOG, fullProfile), body, nonce, offset, maximum, null, bytes(nonce), limit)
    }

    fun manifest(nonce: UUID, expected: DurableCatalogEntry, offset: Int,
                 maximum: Int = MAX_FRAGMENT, fullProfile: Boolean = false): DurableBleReadRequest {
        nonce(nonce); entry(expected, expected.recording.volume, fullProfile)
        val total = DurableManifestCodec.HEADER_BYTES + expected.sealedSegments * DurableManifestCodec.ENTRY_BYTES
        range(offset, maximum, total, fullProfile)
        val body = buffer(if (fullProfile) 74 else 72).apply {
            putUuid(nonce); putUuid(expected.recording.recordingId); putInt(expected.manifestRevision.toInt())
            put(hexBytes(expected.manifestSha256)); if (fullProfile) putInt(offset) else putShort(offset.toShort()); put(maximum.toByte()); put(0)
        }.array()
        return DurableBleReadRequest(command(GET_MANIFEST, fullProfile), body, nonce, offset, maximum, total,
            body.copyOfRange(0, 68), if (fullProfile) DurableManifestCodec.FULL_MAX_BYTES else DurableManifestCodec.MAX_BYTES)
    }

    fun segment(nonce: UUID, expected: SegmentIdentity, offset: Int,
                maximum: Int = MAX_FRAGMENT, fullProfile: Boolean = false): DurableBleReadRequest {
        nonce(nonce); segment(expected, fullProfile); range(offset, maximum, expected.byteCount.toInt(), fullProfile)
        val body = buffer(if (fullProfile) 74 else 72).apply {
            putUuid(nonce); putUuid(expected.recording.recordingId); putInt(expected.sequence)
            if (fullProfile) putInt(offset) else putShort(offset.toShort()); put(maximum.toByte()); put(0); put(hexBytes(expected.sha256))
        }.array()
        return DurableBleReadRequest(command(GET_SEGMENT, fullProfile), body, nonce, offset, maximum, expected.byteCount.toInt(),
            body.copyOfRange(0, 36) + body.copyOfRange(if (fullProfile) 42 else 40, body.size), DurableManifestCodec.MAX_CONTAINER_BYTES)
    }

    fun receipt(nonce: UUID, expected: SegmentIdentity, fullProfile: Boolean = false): DurableBleRequest {
        nonce(nonce); segment(expected, fullProfile)
        return DurableBleRequest(command(RECEIVE_ACK, fullProfile), buffer(72).apply {
            putUuid(nonce); putUuid(expected.recording.recordingId); putInt(expected.sequence)
            putInt(expected.byteCount.toInt()); put(hexBytes(expected.sha256))
        }.array())
    }

    /** Exact immutable final manifest binds every receipt; count is not bytes.
     * The coordinator separately proves every selected file durable on phone. */
    fun receiptRange(nonce: UUID, manifest: RecordingManifest, first: Int, count: Int): DurableBleRequest {
        nonce(nonce)
        require(manifest.finished && digest(manifest.sha256) && count in 1..RECEIPT_BATCH_MAX &&
            first >= 0 && first <= manifest.segments.size - count)
        manifest.segments.subList(first, first + count).forEach { segment(it, true) }
        return DurableBleRequest(FULL_RECEIVE_RANGE, buffer(72).apply {
            putUuid(nonce); putUuid(manifest.recording.recordingId); putInt(first); putInt(count)
            put(hexBytes(manifest.sha256))
        }.array())
    }

    internal fun receiptRangeBody(body: ByteArray, nonce: UUID, connection: DurableSyncConnection,
        manifest: RecordingManifest, first: Int, count: Int): DurableReceiptRangeReply {
        require(manifest.recording.volume == connection.volume)
        val encoded = receiptRange(nonce, manifest, first, count).payload()
        try {
            require(body.contentEquals(encoded)) { "Durable receipt range binding differs" }
            return DurableReceiptRangeReply(connection.epoch, manifest.recording, manifest.sha256, first, count)
        } finally { encoded.fill(0) }
    }

    fun stream(nonce: UUID, expected: SegmentIdentity, offset: Int, maximum: Int): DurableBleReadRequest {
        nonce(nonce); segment(expected, true)
        require(offset in 0 until expected.byteCount.toInt() && maximum in 1..STREAM_MAX)
        val body = buffer(74).apply {
            putUuid(nonce); putUuid(expected.recording.recordingId); putInt(expected.sequence)
            putInt(offset); putShort(maximum.toShort()); put(hexBytes(expected.sha256))
        }.array()
        return DurableBleReadRequest(FULL_STREAM, body, nonce, offset, maximum, expected.byteCount.toInt(),
            body.copyOfRange(0, 36) + body.copyOfRange(42, 74), DurableManifestCodec.MAX_CONTAINER_BYTES)
    }

    fun delete(nonce: UUID, intent: RecordingDeletionIntent, fullProfile: Boolean = false): DurableBleRequest {
        nonce(nonce); deletion(intent)
        return DurableBleRequest(command(DELETE, fullProfile), buffer(72).apply {
            putUuid(nonce); putUuid(intent.operationId); put(hexBytes(intent.manifestSha256)); putLong(0)
        }.array())
    }

    /** Returned fragment owns its ciphertext/metadata bytes; caller closes or
     * wipes it after copying into its durable checkpoint/metadata assembler. */
    fun readResponse(packet: ByteArray, sequence: Int, request: DurableBleReadRequest): DurableBleFragment {
        val body = response(packet, request.command, sequence)
        return try { readBody(body, request) } finally { body.fill(0) }
    }

    /** Internal only: actual ResponseGate already checked OP/status/sequence and
     * both GATT callbacks. This method borrows (does not retain/wipe) its body. */
    internal fun readBody(body: ByteArray, request: DurableBleReadRequest): DurableBleFragment {
            val full = isFull(request.command); val prefix = if (full) 24 else 20
            require(body.size in (prefix + 1)..maxResponseBody(request.command) && uuid(body, 0) == request.nonce) { "Durable fragment framing differs" }
            val input = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN)
            val offset = if (full) input.getInt(16) else input.getShort(16).toInt() and 65535
            val total = if (full) input.getInt(20) else input.getShort(18).toInt() and 65535
            require(offset == request.offset && total in 1..request.limit && offset < total) { "Durable fragment range differs" }
            if (request.command == GET_CATALOG || request.command == FULL_GET_CATALOG) require(total in CATALOG_HEADER..request.limit &&
                (total - CATALOG_HEADER) % CATALOG_ENTRY == 0) { "Durable catalog total differs" }
            require(request.total == null || total == request.total) { "Durable object total differs" }
            val count = minOf(request.maximum, total - offset)
            require(body.size == prefix + count) { "Durable fragment is short or padded" }
            return DurableBleFragment(offset, total, body.copyOfRange(prefix, body.size))
    }

    fun receiptResponse(packet: ByteArray, sequence: Int, nonce: UUID,
                        connection: DurableSyncConnection, expected: SegmentIdentity, fullProfile: Boolean = false): DurableReceiptReply {
        require(expected.recording.volume == connection.volume)
        val encoded = receipt(nonce, expected, fullProfile).payload()
        val body = response(packet, command(RECEIVE_ACK, fullProfile), sequence)
        try {
            require(body.contentEquals(encoded)) { "Durable receipt binding differs" }
            return DurableReceiptReply(connection.epoch, expected)
        } finally { encoded.fill(0); body.fill(0) }
    }

    internal fun receiptBody(body: ByteArray, nonce: UUID, connection: DurableSyncConnection,
        expected: SegmentIdentity, fullProfile: Boolean = false): DurableReceiptReply {
        require(expected.recording.volume == connection.volume)
        val encoded = receipt(nonce, expected, fullProfile).payload()
        try { require(body.contentEquals(encoded)); return DurableReceiptReply(connection.epoch, expected) }
        finally { encoded.fill(0) }
    }

    fun deleteResponse(packet: ByteArray, sequence: Int, nonce: UUID,
        connection: DurableSyncConnection, intent: RecordingDeletionIntent, originalManifestRevision: Long, fullProfile: Boolean = false): DurableTombstoneReply {
        val body = response(packet, command(DELETE, fullProfile), sequence)
        return try { deleteBody(body, nonce, connection, intent, originalManifestRevision, fullProfile) }
        finally { body.fill(0) }
    }

    internal fun deleteBody(body: ByteArray, nonce: UUID, connection: DurableSyncConnection,
        intent: RecordingDeletionIntent, originalManifestRevision: Long, fullProfile: Boolean = false): DurableTombstoneReply {
        require(intent.recording.volume == connection.volume)
        // The original terminal revision comes from persisted metadata, including
        // replay after catalog omission; never infer it from this response.
        require(originalManifestRevision in 1..(if (fullProfile) 20482 else 130) && originalManifestRevision % 4 in 1..2)
        val expected = delete(nonce, intent, fullProfile).payload()
        try {
            require(body.size == 72 && (0 until 64).all { body[it] == expected[it] }) { "Durable tombstone binding differs" }
            val revision = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN).getLong(64)
            require(revision > originalManifestRevision) { "Durable tombstone revision is stale" }
            return DurableTombstoneReply(connection.epoch, intent.recording, intent.operationId, intent.manifestSha256, revision)
        } finally { expected.fill(0) }
    }

    fun encodeCatalog(connection: DurableSyncConnection, nonce: UUID, snapshotRevision: Long,
                      entries: List<DurableCatalogEntry>, fullProfile: Boolean = false): ByteArray {
        nonce(nonce); require(snapshotRevision > 0 && entries.size <= if (fullProfile) FULL_MAX_RECORDINGS else MAX_RECORDINGS)
        val stable = ArrayList(entries)
        catalogEntries(stable, connection.volume, fullProfile)
        return buffer(CATALOG_HEADER + stable.size * CATALOG_ENTRY).apply {
            put(catalogMagic.copyOf().also { if (fullProfile) it[6] = 50 }); putShort(if (fullProfile) 2 else 1); putShort(128); putShort(64); putShort(0)
            putUuid(connection.volume.deviceId); putUuid(connection.volume.volumeId); putLong(connection.volume.generation)
            putUuid(nonce); putLong(snapshotRevision); put(hexBytes(connection.recipientFingerprint))
            putInt(stable.size); putInt(2); putLong(0)
            for (row in stable) {
                putUuid(row.recording.recordingId); putLong(row.manifestRevision); putInt(row.sealedSegments)
                putInt((row.manifestRevision % 4).toInt()); put(hexBytes(row.manifestSha256))
            }
        }.array()
    }

    fun parseCatalog(bytes: ByteArray, connection: DurableSyncConnection, expectedNonce: UUID, fullProfile: Boolean = false): DurableBleCatalog {
        nonce(expectedNonce)
        val maxRecords = if (fullProfile) FULL_MAX_RECORDINGS else MAX_RECORDINGS
        require(bytes.size in CATALOG_HEADER..(CATALOG_HEADER + maxRecords * CATALOG_ENTRY)) { "Invalid durable catalog size" }
        val stable = bytes.copyOf(); val input = ByteBuffer.wrap(stable).order(ByteOrder.LITTLE_ENDIAN)
        val magic = catalogMagic.copyOf().also { if (fullProfile) it[6] = 50 }
        require(magic.indices.all { stable[it] == magic[it] } && input.getShort(8).toInt() == (if (fullProfile) 2 else 1) &&
            input.getShort(10).toInt() == 128 && input.getShort(12).toInt() == 64 && input.getShort(14).toInt() == 0 &&
            input.getInt(116) == 2 && input.getLong(120) == 0L) { "Invalid durable catalog framing" }
        val volume = RecordingVolume(uuid(stable, 16), uuid(stable, 32), input.getLong(48))
        val nonce = uuid(stable, 56); val revision = input.getLong(72); val count = input.getInt(112)
        require(volume == connection.volume && nonce == expectedNonce &&
            hex(stable, 80, 32) == connection.recipientFingerprint && revision > 0 && count in 0..maxRecords &&
            stable.size == CATALOG_HEADER + count * CATALOG_ENTRY) { "Durable catalog binding or bounds differ" }
        val rows = ArrayList<DurableCatalogEntry>(count)
        repeat(count) { index ->
            val at = CATALOG_HEADER + index * CATALOG_ENTRY
            val state = input.getInt(at + 28)
            require(state in 0..2) { "Invalid durable catalog state" }
            val row = DurableCatalogEntry(DurableRecordingId(volume, uuid(stable, at)), input.getLong(at + 16),
                hex(stable, at + 32, 32), input.getInt(at + 24), state != 0)
            require(row.manifestRevision == row.sealedSegments * 4L + state) { "Durable catalog revision differs" }
            rows.add(row)
        }
        catalogEntries(rows, connection.volume, fullProfile)
        return DurableBleCatalog(connection, nonce, revision, rows)
    }

    internal fun response(packet: ByteArray, command: Int, sequence: Int): ByteArray {
        require(isDurable(command) && sequence in 1..65535)
        return OpProtocol.response(packet, command, sequence)
    }

    internal fun validatedCapabilities(info: ByteArray): Long {
        require(info.size == 8)
        val bits = OpProtocol.u32(info, 2)
        require(bits in setOf(15L, 31L, 47L, 63L, 143L, 159L, 655L, 671L, 1679L, 1695L, 3727L, 3743L, 3807L, 7903L, 16095L, 32479L))
        val longControl = bits == 3807L || bits == 7903L || bits == 16095L || bits == 32479L
        require(info[6].toInt() == 0 || (longControl && info[6].toInt() == 1))
        val legacy = info.copyOf()
        try {
            OpProtocol.put32(legacy, 2, bits and (CAPABILITY or FULL_CAPABILITY or WIDE_CAPABILITY or STREAM_CAPABILITY or RECEIPT_BATCH_CAPABILITY or LongRecordingControlCodec.CAPABILITY or DevicePreferences.CAPABILITY or LongRecordingControlCodec.STANDALONE_CAPABILITY or BATTERY_SYNC_CAPABILITY).inv())
            // This narrowly negotiated profile permits reconnect during an explicit
            // autonomous recording. It is not a claim that the microphone is off.
            if (longControl) legacy[6] = 0
            OpProtocol.validateInfo(legacy)
        }
        finally { legacy.fill(0) }
        return bits
    }

    /** Strict command-body admission; independent full-object identity and
     * durable-intent authority still belong to the typed adapter/backend. */
    internal fun encodeRequest(command: Int, sequence: Int, body: ByteArray): ByteArray {
        if (isFull(command)) return encodeFullRequest(command, sequence, body)
        require(command in GET_CATALOG..DELETE && body.size == if (command == GET_CATALOG) 20 else 72)
        nonce(uuid(body, 0))
        val input = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN)
        fun hash(at: Int) { require(digest(hex(body, at, 32))) }
        fun part(at: Int, limit: Int) {
            range(input.getShort(at).toInt() and 65535, body[at + 2].toInt() and 255, limit)
            require(body[at + 3].toInt() == 0)
        }
        when (command) {
            GET_CATALOG -> part(16, MAX_CATALOG_BYTES)
            GET_MANIFEST -> { uuid(body, 16); require(input.getInt(32) in 0..130); hash(36); part(68, DurableManifestCodec.MAX_BYTES) }
            GET_SEGMENT -> { uuid(body, 16); require(input.getInt(32) in 0..31); part(36, DurableManifestCodec.MAX_CONTAINER_BYTES); hash(40) }
            RECEIVE_ACK -> { uuid(body, 16); require(input.getInt(32) in 0..31); val size = input.getInt(36); require(size in 425..34357 && (size - 357) % 68 == 0); hash(40) }
            DELETE -> { uuid(body, 16); hash(32); require(input.getLong(64) == 0L) }
        }
        return DurableBleRequest(command, body).encode(sequence)
    }
    private fun catalogEntries(rows: List<DurableCatalogEntry>, volume: RecordingVolume, fullProfile: Boolean = false) {
        var previous: ByteArray? = null
        for (row in rows) {
            entry(row, volume, fullProfile)
            val current = bytes(row.recording.recordingId)
            previous?.let { prior -> require(compare(prior, current) < 0) { "Catalog UUIDs are duplicated or unordered" } }
            previous = current
        }
    }
    private fun entry(row: DurableCatalogEntry, volume: RecordingVolume, fullProfile: Boolean = false) {
        require(row.recording.volume == volume && row.sealedSegments in 0..(if (fullProfile) 5120 else 32) && digest(row.manifestSha256))
        val state = row.manifestRevision - row.sealedSegments * 4L
        require(state in 0..2 && row.finished == (state != 0L)) { "Invalid producer manifest revision" }
    }
    private fun segment(value: SegmentIdentity, fullProfile: Boolean = false) {
        require(value.sequence in 0 until (if (fullProfile) 5120 else 32) && digest(value.sha256) && value.byteCount in 425..34357 &&
            (value.byteCount - 357) % 68 == 0L) { "Invalid producer segment binding" }
    }
    private fun deletion(intent: RecordingDeletionIntent) {
        require(validOwnedUuid(intent.operationId) && digest(intent.manifestSha256) &&
            intent.location != DeleteLocation.PHONE_ONLY && intent.pendant == PendantDeletion.PENDING) { "Remote deletion intent is not pending" }
    }
    private fun range(offset: Int, maximum: Int, total: Int, full: Boolean = false) { require(offset in 0 until total && maximum in 1..(if (full) WIDE_MAX_FRAGMENT else MAX_FRAGMENT)) }
    private fun encodeFullRequest(command: Int, sequence: Int, body: ByteArray): ByteArray {
        require(isFull(command) && body.size == when(command) { FULL_GET_CATALOG -> 22; FULL_GET_MANIFEST, FULL_GET_SEGMENT, FULL_STREAM -> 74; else -> 72 })
        nonce(uuid(body, 0)); val input = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN)
        fun hash(at: Int) { require(digest(hex(body, at, 32))) }
        fun part(at: Int, limit: Int) {
            range(input.getInt(at), body[at + 4].toInt() and 255, limit, true); require(body[at + 5].toInt() == 0)
        }
        when(command) {
            FULL_GET_CATALOG -> part(16, FULL_MAX_CATALOG_BYTES)
            FULL_GET_MANIFEST -> { uuid(body, 16); val revision = input.getInt(32); require(revision in 0..20482 && revision % 4 != 3); hash(36); part(68, 128 + 64 * (revision / 4)) }
            FULL_GET_SEGMENT -> { uuid(body, 16); require(input.getInt(32) in 0..5119); part(36, DurableManifestCodec.MAX_CONTAINER_BYTES); hash(42) }
            FULL_STREAM -> { uuid(body, 16); require(input.getInt(32) in 0..5119)
                require(input.getInt(36) in 0 until DurableManifestCodec.MAX_CONTAINER_BYTES &&
                    (input.getShort(40).toInt() and 65535) in 1..STREAM_MAX); hash(42) }
            FULL_RECEIVE_ACK -> { uuid(body, 16); require(input.getInt(32) in 0..5119); val size = input.getInt(36); require(size in 425..34357 && (size - 357) % 68 == 0); hash(40) }
            FULL_RECEIVE_RANGE -> { uuid(body, 16); val first = input.getInt(32); val count = input.getInt(36)
                require(first in 0..5119 && count in 1..RECEIPT_BATCH_MAX && count <= 5120 - first); hash(40) }
            FULL_DELETE -> { uuid(body, 16); hash(32); require(input.getLong(64) == 0L) }
        }
        return DurableBleRequest(command, body).encode(sequence)
    }
    private fun nonce(value: UUID) { require(validOwnedUuid(value)) }
    private fun digest(value: String) = isContentDigest(value) && value != "00".repeat(32) && value != "ff".repeat(32)
    private fun buffer(size: Int) = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
    private fun bytes(value: UUID) = buffer(16).apply { putUuid(value) }.array()
    private fun ByteBuffer.putUuid(value: UUID) { for (shift in 56 downTo 0 step 8) put((value.mostSignificantBits ushr shift).toByte()); for (shift in 56 downTo 0 step 8) put((value.leastSignificantBits ushr shift).toByte()) }
    private fun uuid(bytes: ByteArray, offset: Int) = ByteBuffer.wrap(bytes, offset, 16).order(ByteOrder.BIG_ENDIAN).let { UUID(it.long, it.long) }.also { nonce(it) }
    private fun compare(a: ByteArray, b: ByteArray): Int { for (i in a.indices) { val delta = (a[i].toInt() and 255) - (b[i].toInt() and 255); if (delta != 0) return delta }; return 0 }
    private fun hexBytes(value: String) = ByteArray(value.length / 2) { value.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    private fun hex(bytes: ByteArray, offset: Int, count: Int) = buildString(count * 2) { repeat(count) { val v = bytes[offset + it].toInt() and 255; append("0123456789abcdef"[v ushr 4]); append("0123456789abcdef"[v and 15]) } }
}

open class DurableBleRequest internal constructor(val command: Int, payload: ByteArray) {
    private val body = payload.copyOf()
    init { require(DurableBleCodec.isDurable(command) && body.size <= (if (DurableBleCodec.isFull(command)) 74 else 72)) }
    fun payload(): ByteArray = body.copyOf()
    fun encode(sequence: Int): ByteArray {
        require(sequence in 1..65535)
        return ByteBuffer.allocate(8 + body.size).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(79); put(80); put(1); put(command.toByte()); putShort(sequence.toShort()); putShort(body.size.toShort()); put(body)
        }.array()
    }
    override fun toString() = "DurableBleRequest(command=$command, bytes=${body.size})"
}

class DurableBleReadRequest internal constructor(command: Int, body: ByteArray, val nonce: UUID,
    val offset: Int, val maximum: Int, val total: Int?,
    selector: ByteArray, internal val limit: Int) : DurableBleRequest(command, body) {
    private val selector = selector.copyOf()
    internal fun sameObject(other: DurableBleReadRequest) = command == other.command && nonce == other.nonce &&
        total == other.total && selector.contentEquals(other.selector)
}

class DurableBleFragment internal constructor(val offset: Int, val total: Int, val bytes: ByteArray) : Closeable {
    override fun close() { bytes.fill(0) }
    override fun toString() = "DurableBleFragment(offset=$offset, total=$total, bytes=${bytes.size})"
}

class DurableBleCatalog internal constructor(val connection: DurableSyncConnection, val nonce: UUID,
    val snapshotRevision: Long, entries: List<DurableCatalogEntry>) {
    val entries: List<DurableCatalogEntry> = Collections.unmodifiableList(ArrayList(entries))
}

/** Metadata only. Ciphertext segments stream into the existing checkpoint
 * engine instead; this assembler never buffers an entire recording. Any invalid
 * fragment permanently closes/wipes this instance; there is no resync/retry. */
class DurableBleMetadataAssembler(private val first: DurableBleReadRequest) : Closeable {
    private var storage: ByteArray? = null
    private var total: Int? = first.total
    var received: Int = 0
        private set
    init {
        require(first.offset == 0 && first.command in setOf(DurableBleCodec.GET_CATALOG, DurableBleCodec.GET_MANIFEST, DurableBleCodec.FULL_GET_CATALOG, DurableBleCodec.FULL_GET_MANIFEST))
        storage = ByteArray(first.total ?: first.limit)
    }
    fun accept(request: DurableBleReadRequest, sequence: Int, packet: ByteArray) {
        try {
            val body = DurableBleCodec.response(packet, request.command, sequence)
            try { acceptBody(request, body) } finally { body.fill(0) }
        } catch (failure: Throwable) { close(); throw failure }
    }
    internal fun acceptBody(request: DurableBleReadRequest, body: ByteArray) {
        val target = checkNotNull(storage) { "Metadata assembly is closed" }
        try {
            require(request.sameObject(first) && request.offset == received)
            DurableBleCodec.readBody(body, request).use { fragment ->
                require(total == null || fragment.total == total)
                require(fragment.total <= target.size && received <= fragment.total - fragment.bytes.size)
                total = fragment.total
                fragment.bytes.copyInto(target, received); received += fragment.bytes.size
            }
        } catch (failure: Throwable) { close(); throw failure }
    }
    fun finish(): ByteArray {
        val target = checkNotNull(storage)
        return try {
            require(total != null && received == total) { "Metadata is incomplete" }
            target.copyOf(received)
        } finally { close() }
    }
    override fun close() { storage?.fill(0); storage = null }
}
