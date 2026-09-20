package org.openpendant.app

import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean

/** Actual OP framing/write+notification validation belongs to this port. Its
 * exchange returns an OWNED bounded body only after both callbacks. Admission,
 * capabilities and isCurrent are pure/nonblocking. cancel initiates exact-lease
 * local closure, not remote cancellation or permission to complete a receipt.
 * No implementation may retry/reconnect or complete a logical call per fragment.
 */
interface DurableBleExchange {
    fun capabilities(epoch: UUID): DurableSyncCapabilities?
    fun acquire(epoch: UUID): DurableTransportLease
    fun isCurrent(epoch: UUID): Boolean
    fun exchange(lease: DurableTransportLease, call: DurableSyncCall, request: DurableBleRequest): ByteArray
    fun cancel(lease: DurableTransportLease)
}

/** Construct from validated persisted metadata BEFORE the radio job, including
 * catalog-omitted delete replay. This is not authority inferred from wire bytes.
 * The existing coordinator still matches the current pending intent on dispatch.
 */
class RetainedDurableDeletion(val intent: RecordingDeletionIntent, val manifest: RecordingManifest) {
    init {
        require(intent.recording == manifest.recording && intent.manifestSha256 == manifest.sha256 &&
            manifest.finished && manifest.segments.size <= DurableManifestCodec.FULL_MAX_SEGMENTS &&
            manifest.revision - manifest.segments.size * 4L in 1..2 &&
            intent.pendant == PendantDeletion.PENDING && intent.location != DeleteLocation.PHONE_ONLY)
        DurableBleCodec.delete(UUID(1, 1), intent) // Full operation/hash sentinel validation.
    }
    companion object {
        /** One pending delete per recording is dispatched by a sync run. No file
         * reads, payloads or mutable persistence handles are retained here. */
        fun fromSnapshots(connection: DurableSyncConnection, snapshots: List<RecordingSyncSnapshot>): List<RetainedDurableDeletion> {
            require(snapshots.size <= DurableRecordingSyncSession.MAX_RECORDINGS &&
                snapshots.map { it.recording }.toSet().size == snapshots.size)
            return snapshots.filter { it.recording.volume == connection.volume && !it.staleVolume }.mapNotNull { row ->
                row.deletions.firstOrNull { it.pendant == PendantDeletion.PENDING }?.let {
                    RetainedDurableDeletion(it, checkNotNull(row.manifest))
                }
            }
        }
    }
}

/** One explicit sync job/connection/nonce. Production-neutral but wire-backed:
 * actual PendantDurableBleExchange supplies GATT, tests supply framed fake I/O.
 * The connection's full volume/fingerprint must be independently enrolled, never
 * learned by trusting this catalog. No UI, auto-sync, capture, key or audio calls.
 * Only ciphertext streams (<=4096 B); metadata is <=8 manifests of <=2176 B.
 * Any admitted failure permanently fences this instance and closes its exact
 * radio owner. A fresh explicit session may reconcile persistent work; no retry.
 */
class DurableBleRecordingTransport(private val connection: DurableSyncConnection,
    private val wire: DurableBleExchange,
    retainedDeletions: List<RetainedDurableDeletion> = emptyList(),
    private val nonce: UUID = UUID.randomUUID()) : DurableRecordingTransport {
    private val started = AtomicBoolean()
    private val executing = AtomicBoolean()
    private val closing = AtomicBoolean()
    @Volatile private var failed = false
    @Volatile private var lease: DurableTransportLease? = null
    private var catalog: DurableBleCatalog? = null
    private var fullProfile = false
    private var wideProfile = false
    private var streamProfile = false
    private var receiptBatchProfile = false
    private var frames = 0
    override fun batchLimitReached(): Boolean = fullProfile && frames >= 40_000
    private val fragment: Int get() = if (wideProfile) DurableBleCodec.WIDE_MAX_FRAGMENT
        else if (fullProfile) DurableBleCodec.FULL_MAX_FRAGMENT else DurableBleCodec.MAX_FRAGMENT
    private val maxRecords: Int get() = if (fullProfile) DurableBleCodec.FULL_MAX_RECORDINGS else DurableBleCodec.MAX_RECORDINGS
    private val manifests = LinkedHashMap<DurableRecordingId, DurableManifestSnapshot>()
    private val deletions: Map<UUID, RetainedDurableDeletion>
    init {
        require(validOwnedUuid(nonce) && retainedDeletions.size <= DurableRecordingSyncSession.MAX_RECORDINGS)
        val stable = ArrayList(retainedDeletions)
        require(stable.all { it.intent.recording.volume == connection.volume } &&
            stable.map { it.intent.operationId }.toSet().size == stable.size &&
            stable.map { it.intent.recording }.toSet().size == stable.size)
        deletions = stable.associateBy { it.intent.operationId }
    }
    override val capabilities: DurableSyncCapabilities get() = wire.capabilities(connection.epoch)
        ?: DurableSyncCapabilities(false, false, false, false)

    override fun acquireSession(connection: DurableSyncConnection): DurableTransportLease {
        require(connection == this.connection)
        check(started.compareAndSet(false, true)) { "Wire adapter is single use" }
        check(isCurrent(connection))
        val caps = capabilities
        check(caps.catalog && caps.rangedSegments && caps.durableReceipts && caps.tombstones)
        fullProfile = caps.fullStorage
        check(!caps.wideFragments || fullProfile)
        wideProfile = caps.wideFragments
        check(!caps.rangeStream || wideProfile); streamProfile = caps.rangeStream
        check(!caps.receiptBatch || fullProfile); receiptBatchProfile = caps.receiptBatch
        return wire.acquire(connection.epoch).also { acquired ->
            lease = acquired
            // Port contract requires exact epoch. If violated, preserve/close
            // the admitted owner rather than silently leaking it.
            if (acquired.epoch != connection.epoch || !acquired.isActive() || !isCurrent(connection)) {
                failed = true; try { closeOnce(acquired) } catch (_: Throwable) { }; throw DurableSyncException()
            }
        }
    }
    override fun isCurrent(connection: DurableSyncConnection): Boolean =
        connection == this.connection && !failed && wire.isCurrent(connection.epoch)

    override fun endSession(lease: DurableTransportLease) {
        require(this.lease === lease && lease.epoch == connection.epoch)
        failed = true
        // Device snapshot ownership has no wire END in v1. Successful sync also
        // closes its exact GATT; only a later explicit Connect creates an epoch.
        closeOnce(lease)
    }
    private fun closeOnce(lease: DurableTransportLease) {
        if (closing.compareAndSet(false, true)) wire.cancel(lease)
    }

    private fun active(call: DurableSyncCall) {
        require(call.connection == connection)
        call.borrowRequest(checkNotNull(lease))
        check(!failed && wire.isCurrent(connection.epoch) && capabilities.fullStorage == fullProfile &&
            capabilities.wideFragments == wideProfile && capabilities.rangeStream == streamProfile &&
            capabilities.receiptBatch == receiptBatchProfile)
        call.checkActive() // Predicate/preemption cannot hide a deadline crossing.
    }
    private fun <T> operation(call: DurableSyncCall, discard: (T) -> Unit = {}, block: () -> T): T {
        require(call.connection == connection)
        require(call.belongsTo(checkNotNull(lease))) // Foreign/old producers cannot close this owner.
        check(executing.compareAndSet(false, true)) { "Wire operation already active" }
        var result: T? = null
        try {
            active(call); val value = block(); result = value
            active(call); check(call.transportCompleted())
            return value
        } catch (failure: Throwable) {
            result?.let(discard); failed = true
            try { closeOnce(checkNotNull(lease)) } catch (_: Throwable) { /* Still fenced; never forge proof. */ }
            throw failure
        } finally { executing.set(false) }
    }
    private fun <T> body(request: DurableBleRequest, call: DurableSyncCall, parse: (ByteArray) -> T): T {
        active(call)
        check(frames < 60_000)
        frames++
        val bytes = wire.exchange(checkNotNull(lease), call, request)
        try { active(call); require(bytes.size <= DurableBleCodec.maxResponseBody(request.command)); return parse(bytes) }
        finally { bytes.fill(0) }
    }
    private fun metadata(call: DurableSyncCall, request: (Int) -> DurableBleReadRequest): ByteArray {
        val first = request(0)
        DurableBleMetadataAssembler(first).use { assembly ->
            var total = first.total
            do {
                val next = request(assembly.received)
                body(next, call) { bytes ->
                    // Checked fragment total supplies the catalog's finite size.
                    DurableBleCodec.readBody(bytes, next).use { total = it.total }
                    assembly.acceptBody(next, bytes)
                }
            } while (assembly.received < checkNotNull(total))
            return assembly.finish()
        }
    }
    override fun catalog(offset: Int, maximumEntries: Int, snapshotRevision: Long?, call: DurableSyncCall): DurableCatalogPage = operation(call) {
        require(maximumEntries in 1..8 && offset in 0..maxRecords)
        if (catalog == null) {
            require(offset == 0 && snapshotRevision == null)
            val bytes = metadata(call) { DurableBleCodec.catalog(nonce, it, fragment, fullProfile) }
            try { catalog = DurableBleCodec.parseCatalog(bytes, connection, nonce, fullProfile) } finally { bytes.fill(0) }
        }
        val frozen = checkNotNull(catalog)
        require(snapshotRevision == null && offset == 0 || snapshotRevision == frozen.snapshotRevision)
        require(offset <= frozen.entries.size && (offset < frozen.entries.size || frozen.entries.isEmpty()))
        DurableCatalogPage(connection.epoch, frozen.snapshotRevision, offset, frozen.entries.size,
            frozen.entries.drop(offset).take(maximumEntries))
    }
    override fun manifest(entry: DurableCatalogEntry, call: DurableSyncCall): DurableManifestReply = operation(call) {
        require(checkNotNull(catalog).entries.contains(entry))
        var value = manifests[entry.recording]
        if (value == null) {
            check(manifests.size < maxRecords)
            val bytes = metadata(call) { DurableBleCodec.manifest(nonce, entry, it, fragment, fullProfile) }
            try { value = DurableManifestCodec.parse(bytes, entry, connection.recipientFingerprint, fullProfile) }
            finally { bytes.fill(0) }
            manifests[entry.recording] = checkNotNull(value)
        }
        DurableManifestReply(connection.epoch, checkNotNull(value).manifest)
    }
    private fun segment(segment: SegmentIdentity) {
        require(segment.recording.volume == connection.volume &&
            manifests[segment.recording]?.manifest?.containsExact(segment) == true)
    }
    override fun read(segment: SegmentIdentity, offset: Long, maximumBytes: Int, call: DurableSyncCall): DurableRangeReply =
        operation(call, { it.chunk.bytes.fill(0) }) {
            segment(segment); require(offset in 0 until segment.byteCount && maximumBytes in 1..4096)
            val bytes = ByteArray(minOf(maximumBytes.toLong(), segment.byteCount - offset).toInt())
            try {
                var used = 0
                while (used < bytes.size) {
                    val request = if (streamProfile) DurableBleCodec.stream(nonce, segment, offset.toInt() + used, bytes.size - used)
                        else DurableBleCodec.segment(nonce, segment, offset.toInt() + used, minOf(fragment, bytes.size - used), fullProfile)
                    body(request, call) { response -> DurableBleCodec.readBody(response, request).use { fragment ->
                        fragment.bytes.copyInto(bytes, used); used += fragment.bytes.size
                    } }
                }
                DurableRangeReply(connection.epoch, EncryptedSegmentChunk(segment, offset, bytes))
            } catch (failure: Throwable) { bytes.fill(0); throw failure }
        }
    override fun receipt(segment: SegmentIdentity, call: DurableSyncCall): DurableReceiptReply = operation(call) {
        segment(segment)
        body(DurableBleCodec.receipt(nonce, segment, fullProfile), call) { DurableBleCodec.receiptBody(it, nonce, connection, segment, fullProfile) }
    }
    override fun receiptRange(manifest: RecordingManifest, first: Int, count: Int,
        call: DurableSyncCall): DurableReceiptRangeReply = operation(call) {
        check(receiptBatchProfile && fullProfile)
        require(manifests[manifest.recording]?.manifest == manifest)
        body(DurableBleCodec.receiptRange(nonce, manifest, first, count), call) {
            DurableBleCodec.receiptRangeBody(it, nonce, connection, manifest, first, count)
        }
    }
    override fun delete(intent: RecordingDeletionIntent, call: DurableSyncCall): DurableTombstoneReply = operation(call) {
        val retained = checkNotNull(deletions[intent.operationId])
        require(retained.intent == intent)
        // Frozen snapshot may omit an already-deleted recording; retained exact
        // manifest is mandatory either way. Absence never supplies a tombstone.
        body(DurableBleCodec.delete(nonce, intent, fullProfile), call) {
            DurableBleCodec.deleteBody(it, nonce, connection, intent, retained.manifest.revision, fullProfile)
        }
    }
}
