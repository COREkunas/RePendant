package org.openpendant.app

import java.util.Collections
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean

/** Transport-neutral contracts, NOT new BLE opcodes. The legacy short-clip
 * protocol cannot implement these ports. A future adapter must authenticate the
 * owner, negotiate durable capabilities and compare the mounted volume/key to
 * independently enrolled owner metadata before constructing this connection.
 * A new epoch is mandatory on every reconnect, even to the same volume.
 */
data class DurableSyncConnection(val epoch: UUID, val volume: RecordingVolume,
    val recipientFingerprint: String) {
    init { require(validOwnedUuid(epoch) && isContentDigest(recipientFingerprint) &&
        recipientFingerprint != "00".repeat(32) && recipientFingerprint != "ff".repeat(32)) }
}

data class DurableSyncCapabilities(val catalog: Boolean, val rangedSegments: Boolean,
    val durableReceipts: Boolean, val tombstones: Boolean, val fullStorage: Boolean = false,
    val wideFragments: Boolean = false, val rangeStream: Boolean = false, val receiptBatch: Boolean = false)

data class DurableCatalogEntry(val recording: DurableRecordingId, val manifestRevision: Long,
    val manifestSha256: String, val sealedSegments: Int, val finished: Boolean)

class DurableCatalogPage(val epoch: UUID, val snapshotRevision: Long, val offset: Int,
    val total: Int, entries: List<DurableCatalogEntry>) {
    val entries: List<DurableCatalogEntry> = Collections.unmodifiableList(ArrayList(entries))
}
data class DurableManifestReply(val epoch: UUID, val manifest: RecordingManifest)
class DurableRangeReply(val epoch: UUID, val chunk: EncryptedSegmentChunk) {
    override fun toString() = "DurableRangeReply(ciphertext bytes=${chunk.bytes.size})"
}
data class DurableReceiptReply(val epoch: UUID, val segment: SegmentIdentity)
data class DurableReceiptRangeReply(val epoch: UUID, val recording: DurableRecordingId,
    val manifestSha256: String, val firstSequence: Int, val count: Int)
data class DurableTombstoneReply(val epoch: UUID, val recording: DurableRecordingId,
    val operationId: UUID, val manifestSha256: String, val revision: Long)

/** Adapter must check this immediately before submitting EVERY underlying GATT
 * request and after both write+notification callbacks. No reset of the absolute
 * deadline across fragmentation, queues or retries. No operation is retried by
 * this session. Invalid/late replies are not permission to reconnect or delete.
 * checkActive is nonblocking; the adapter must also arrange cancellation of its
 * own bounded pending I/O. A synchronous port cannot forcibly interrupt a stuck
 * platform call. All wire framing/size validation remains the adapter's duty.
 */
class DurableSyncCall internal constructor(val connection: DurableSyncConnection,
    val deadlineMillis: Long, private val radio:DurableTransportRequest, private val check: () -> Unit) {
    fun checkActive() { check();radio.requirePending() }
    /** Adapter proof, not cancellation initiation; see DurableTransportRequest. */
    fun transportCompleted():Boolean=radio.completed()
    fun transportCancelled():Boolean=radio.cancelled()
    internal fun validateReply() { check();radio.requireSuccessful() }
    /** Borrow the SAME logical request for bounded GATT fragments; never begin
     * or complete a replacement request for an intermediate frame. */
    internal fun borrowRequest(lease:DurableTransportLease):DurableTransportRequest {
        require(radio.belongsTo(lease) && connection.epoch == lease.epoch)
        checkActive();return radio
    }
    internal fun belongsTo(lease:DurableTransportLease):Boolean =
        radio.belongsTo(lease) && connection.epoch == lease.epoch
}

interface DurableRecordingTransport {
    val capabilities: DurableSyncCapabilities
    /** Pure successful-batch boundary, never permission to retry failed I/O. */
    fun batchLimitReached(): Boolean = false
    /** Must acquire the same radio gate used by capture/control/telemetry. Pure
     * admission, no I/O/wait; returning an already borrowed lease is forbidden. */
    fun acquireSession(connection:DurableSyncConnection):DurableTransportLease
    /** Nonblocking teardown after all file/coordinator scopes end, on every
     * outcome. The real protocol retains a device catalog lease until disconnect:
     * queue closure of this EXACT local channel, fencing before returning. No
     * retry/reconnect or remote-completion proof. Invoked before lease.retire(). */
    fun endSession(lease: DurableTransportLease)
    /** Thread-safe, pure/nonblocking; false permanently for a replaced epoch. */
    fun isCurrent(connection: DurableSyncConnection): Boolean
    /** Stable complete snapshot, repeated explicitly from offset0 on a new run.
     * Snapshot omission NEVER proves deletion. No persisted incremental cursor. */
    fun catalog(offset: Int, maximumEntries: Int, snapshotRevision: Long?, call: DurableSyncCall): DurableCatalogPage
    fun manifest(entry: DurableCatalogEntry, call: DurableSyncCall): DurableManifestReply
    /** Repeatable non-destructive read, owned bytes, 1..maximumBytes on success. */
    fun read(segment: SegmentIdentity, offset: Long, maximumBytes: Int, call: DurableSyncCall): DurableRangeReply
    /** Durable idempotent receipt by full segment binding. NEVER deletes source. */
    fun receipt(segment: SegmentIdentity, call: DurableSyncCall): DurableReceiptReply
    fun receiptRange(manifest: RecordingManifest, first: Int, count: Int, call: DurableSyncCall): DurableReceiptRangeReply =
        throw UnsupportedOperationException("Receipt batches were not negotiated")
    /** Exact persisted op ID; durable tombstone before reply. Replay returns the
     * same tombstone, not a second delete. No physical address/erase permission. */
    fun delete(intent: RecordingDeletionIntent, call: DurableSyncCall): DurableTombstoneReply
}

/** Reuses the existing CAS/coordinator authority. Implementations must enumerate
 * only bounded validated metadata, not scan payloads; callbacks/commits are
 * durable before return. Exactly one process owner must be shared with playback
 * and deletion. No metadata-only coordinator is exposed to ordinary callers.
 */
interface DurableRecordingMetadata {
    fun snapshots(deviceId: UUID): List<RecordingSyncSnapshot>
    fun invalidateOtherVolumes(current: RecordingVolume)
    fun createRecording(recording: DurableRecordingId)
    fun coordinator(recording: DurableRecordingId, verifyPhoneSegment: (SegmentIdentity) -> Boolean): RecordingSyncContract
}

data class DurableSyncResult(val catalogRecords: Int, val segmentsPublished: Int,
    val receiptsConfirmed: Int, val tombstonesConfirmed: Int, val suppressedRecordings: Int,
    val pendingPhoneDeletion: Int, val unsupportedRemoteDeletion: Int, val morePending: Boolean = false,
    val catalogEntries: List<DurableCatalogEntry> = emptyList(), val manifestsObserved: Int = 0)

/** Inventory never downloads, receipts or deletes. Deletion-only dispatches only
 * existing durable intents; it never creates an intent from catalog contents. */
enum class DurableSyncMode { NORMAL, INVENTORY_ONLY, DELETIONS_ONLY }

class DurableSyncException(cause: Throwable? = null) : IllegalStateException("Durable recording sync stopped; pending work was retained", cause)

/** Only reusable AFTER coordinator admission has reverified every retained file.
 * The fresh authenticated catalog must name the exact immutable manifest. This
 * never supplies transport authorization for reads/receipts/deletion: it skips
 * those operations only when the recording has no outstanding work at all. */
internal fun RecordingSyncSnapshot.settledCatalogManifest(entry: DurableCatalogEntry): RecordingManifest? {
    val saved = manifest ?: return null
    return saved.takeIf {
        !staleVolume && !downloadSuppressed && pendantCopy == PendantCopy.PRESENT &&
            deletions.isEmpty() && pendingReceipts.isEmpty() && saved.finished && entry.finished &&
            recording == entry.recording && saved.recording == entry.recording &&
            saved.revision == entry.manifestRevision && saved.sha256 == entry.manifestSha256 &&
            saved.segments.size == entry.sealedSegments && phoneSegments == saved.segments.toSet()
    }
}

/** One explicit, single-use I/O-worker job. No startup work, capture, plaintext,
 * key creation, transcription, source reclamation or local deletion. Pending
 * PHONE/BOTH removal must finish through restricted deletion recovery first.
 * A new explicit session resumes persisted offsets/outbox after interruption;
 * a failed job is never automatically retried. Existing phone files are checked
 * before their receipts are exposed. Ciphertext verification is not HPKE auth.
 */
class DurableRecordingSyncSession(private val connection: DurableSyncConnection,
    private val transport: DurableRecordingTransport,
    private val metadata: DurableRecordingMetadata,
    private val files: DurableSegmentStore,
    private val clockMillis: () -> Long = { System.nanoTime() / 1_000_000 },
    private val observer: DurableSyncObserver = object : DurableSyncObserver {},
    private val mode: DurableSyncMode = DurableSyncMode.NORMAL,
    private val selection: SelectedRecordingDownload? = null) {
    init { require(selection == null || (mode == DurableSyncMode.NORMAL && selection.recording.volume == connection.volume)) }
    private val started = AtomicBoolean()
    private val cancelled = AtomicBoolean()
    fun cancel() { cancelled.set(true) }

    fun run(): DurableSyncResult {
        check(started.compareAndSet(false, true)) { "A sync session may run only once" }
        val start = clockMillis()
        if (start < 0 || start > Long.MAX_VALUE - MAX_SESSION_MILLIS) throw DurableSyncException()
        val deadline = start + MAX_SESSION_MILLIS
        var last = start
        var lease:DurableTransportLease?=null
        val freshness=Any()
        // Adapter callbacks may check on a different thread from this worker.
        // Serialize only the clock/freshness check, never radio I/O/file work.
        fun active():Long = synchronized(freshness) {
            if (cancelled.get()) throw CancellationException("Durable recording sync cancelled")
            val before = clockMillis()
            if (before < last || before >= deadline || !transport.isCurrent(connection) || lease?.isActive()==false) throw DurableSyncException()
            // Even a pure predicate/short ownership check may be preempted.
            // Its callback must not hide a deadline crossing/backward jump.
            val after = clockMillis()
            if(after < before || after >= deadline) throw DurableSyncException()
            last = after
            after
        }
        fun call(budget: Long = MAX_CALL_MILLIS): DurableSyncCall {
            val now=active()
            val end = minOf(deadline, now + minOf(budget, Long.MAX_VALUE - now))
            val radio=checkNotNull(lease).beginRequest()
            return DurableSyncCall(connection, end, radio) {
                if (active() >= end) throw DurableSyncException()
            }
        }
        fun <T> exchange(budget: Long = MAX_CALL_MILLIS, action: (DurableSyncCall) -> T): T {
            val request = call(budget)
            val reply = action(request)
            request.validateReply()
            return reply
        }
        fun expectation(segment: SegmentIdentity): EncryptedSegmentExpectation {
            require(segment.recording.volume == connection.volume && segment.byteCount in 210..65745)
            val fingerprint = ByteArray(32) { connection.recipientFingerprint.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
            return EncryptedSegmentExpectation(segment, fingerprint, (segment.byteCount - 209).toInt())
        }
        var published = 0; var receipts = 0; var tombstones = 0
        var manifestsObserved = 0
        val entries = ArrayList<DurableCatalogEntry>()
        var suppressed = 0; var phonePending = 0; var unsupported = 0
        fun result(count: Int, more: Boolean = false) = DurableSyncResult(count, published, receipts,
            tombstones, suppressed, phonePending, unsupported, more,
            if(mode==DurableSyncMode.NORMAL)emptyList() else Collections.unmodifiableList(ArrayList(entries)),
            if(mode==DurableSyncMode.NORMAL)0 else manifestsObserved)
        fun yieldBatch(reserve: Long): Boolean = transport.capabilities.fullStorage &&
            (transport.batchLimitReached() || deadline - active() < reserve)
        try {
            active()
            val acquired=transport.acquireSession(connection)
            lease=acquired
            require(acquired.epoch==connection.epoch && acquired.isActive())
            val caps = transport.capabilities
            require(caps.catalog && caps.rangedSegments && caps.durableReceipts)
            active() // Admission/capability callbacks cannot authorize stale metadata mutation.
            metadata.invalidateOtherVolumes(connection.volume)
            active()
            var snapshot: Long? = null
            var total: Int? = null
            do {
                val page = exchange(if (caps.fullStorage && entries.isEmpty()) FULL_METADATA_MILLIS else MAX_CALL_MILLIS) { transport.catalog(entries.size, PAGE_ENTRIES, snapshot, it) }
                require(page.epoch == connection.epoch && page.snapshotRevision >= 0 &&
                    page.offset == entries.size && page.total in 0..MAX_RECORDINGS &&
                    page.entries.size <= PAGE_ENTRIES && page.offset + page.entries.size <= page.total &&
                    (page.total == 0 || page.entries.isNotEmpty()))
                require(snapshot == null || snapshot == page.snapshotRevision)
                require(total == null || total == page.total)
                snapshot = page.snapshotRevision; total = page.total
                page.entries.forEach { entry ->
                    require(entry.recording.volume == connection.volume && entry.manifestRevision >= 0 &&
                        isContentDigest(entry.manifestSha256) && entry.sealedSegments in 0..RecordingManifest.MAX_SEGMENTS &&
                        entries.none { it.recording == entry.recording })
                    entries.add(entry)
                }
            } while (entries.size < total!!)
            active()
            val old = metadata.snapshots(connection.volume.deviceId)
            require(old.size <= MAX_RECORDINGS && old.map { it.recording }.toSet().size == old.size)
            val current = old.filter { it.recording.volume == connection.volume }
            selection?.let { selected ->
                require(entries.singleOrNull { it.recording == selected.recording }?.let(selected::matches) == true)
                require(current.singleOrNull { it.recording == selected.recording }?.let(selected::matches) == true)
            }
            val work = LinkedHashSet<DurableRecordingId>()
            if (mode != DurableSyncMode.DELETIONS_ONLY) entries.filter {
                selection == null || it.recording == selection.recording
            }.forEach { work.add(it.recording) }
            // Lost delete reply may remove the record from the remote catalog.
            // Dispatch its persisted outbox anyway; absence is not a tombstone.
            current.filter { row -> selection == null && mode != DurableSyncMode.INVENTORY_ONLY && row.deletions.any { it.pendant == PendantDeletion.PENDING } }
                .forEach { work.add(it.recording) }
            require(work.size <= MAX_RECORDINGS)
            val engine = SegmentDownloadEngine(files, clockMillis)
            val newWorkReserve = FULL_METADATA_MILLIS + SegmentDownloadEngine.MAX_JOB_MILLIS + MAX_CALL_MILLIS
            for ((recordIndex, recording) in work.withIndex()) {
                active()
                val previous = current.singleOrNull { it.recording == recording }
                val catalogEntry = entries.singleOrNull { it.recording == recording }
                if (mode == DurableSyncMode.INVENTORY_ONLY && previous?.manifest?.let { saved ->
                    !previous.staleVolume && catalogEntry != null && saved.recording == catalogEntry.recording &&
                        saved.revision == catalogEntry.manifestRevision && saved.sha256 == catalogEntry.manifestSha256 &&
                        saved.finished == catalogEntry.finished && saved.segments.size == catalogEntry.sealedSegments
                } == true) continue
                // Scheduling hint only: do not reserve a new manifest/download
                // budget for an apparently settled local copy. Actual reuse
                // still requires coordinator admission and file verification.
                val mayVerifySettled = caps.fullStorage && catalogEntry != null &&
                    previous?.settledCatalogManifest(catalogEntry) != null
                val reserve = if (mode == DurableSyncMode.INVENTORY_ONLY) FULL_METADATA_MILLIS
                    else if (mode == DurableSyncMode.DELETIONS_ONLY || mayVerifySettled) MAX_CALL_MILLIS else newWorkReserve
                if (yieldBatch(reserve))
                    return result(entries.size, true)
                if (previous?.deletions?.any { it.phonePending } == true) { phonePending++; continue }
                if (previous == null) metadata.createRecording(recording)
                active()
                val core = metadata.coordinator(recording) { files.verifiedOnDisk(expectation(it)) }
                try {
                    core.authenticatedConnection(connection.volume, true)
                    active()
                    selection?.let { require(it.matches(core.snapshot())) }
                    val pending = core.nextPendantDeletion()
                    if (pending != null && mode != DurableSyncMode.INVENTORY_ONLY) {
                        if (!caps.tombstones) { unsupported++; continue }
                        val reply = exchange { transport.delete(pending.intent, it) }
                        require(reply.epoch == connection.epoch && reply.operationId == pending.intent.operationId &&
                            reply.recording == pending.intent.recording && reply.manifestSha256 == pending.intent.manifestSha256)
                        require(core.confirmPendantDeletion(pending, reply.revision, reply.manifestSha256))
                        tombstones++
                        continue // Never download or delete phone files after a tombstone.
                    }
                    if (mode == DurableSyncMode.DELETIONS_ONLY) continue
                    val entry = catalogEntry ?: continue
                    // Keep the coordinator/file-integrity checks above. A fully
                    // settled recording needs neither another manifest transfer
                    // nor installation into the wire adapter's I/O authority.
                    val settled = if (caps.fullStorage) core.snapshot().settledCatalogManifest(entry) else null
                    if (settled != null) {
                        settled.segments.forEach { expectation(it) }
                        core.observeManifest(settled)
                        active()
                        observer.recording(core.snapshot(), recordIndex + 1, work.size)
                        continue
                    }
                    // Admission may have produced a different current snapshot.
                    // The cheap hint must never let new wire work bypass its
                    // original conservative reserve or extend any deadline.
                    if (yieldBatch(if(mode==DurableSyncMode.INVENTORY_ONLY) FULL_METADATA_MILLIS else newWorkReserve)) return result(entries.size, true)
                    val reply = exchange(if (caps.fullStorage) FULL_METADATA_MILLIS else MAX_CALL_MILLIS) { transport.manifest(entry, it) }
                    val manifest = reply.manifest
                    require(reply.epoch == connection.epoch && manifest.recording == entry.recording &&
                        manifest.revision == entry.manifestRevision && manifest.sha256 == entry.manifestSha256 &&
                        manifest.finished == entry.finished && manifest.segments.size == entry.sealedSegments)
                    manifest.segments.forEach { expectation(it) } // All bounds before durable admission.
                    core.observeManifest(manifest)
                    manifestsObserved++
                    active()
                    if (mode == DurableSyncMode.INVENTORY_ONLY) {
                        observer.recording(core.snapshot(), recordIndex + 1, work.size)
                        continue
                    }
                    if (core.snapshot().downloadSuppressed) { suppressed++; continue }
                    observer.recording(core.snapshot(), recordIndex + 1, work.size)
                    fun flushReceipts(): Boolean {
                        repeat(RecordingManifest.MAX_SEGMENTS) {
                            active()
                            if (caps.receiptBatch && manifest.finished) {
                                val batch = core.nextReceiptBatch()
                                if (batch.isEmpty()) return true
                                if (yieldBatch(MAX_CALL_MILLIS)) return false
                                val first = batch.first().segment.sequence
                                val ack = exchange { transport.receiptRange(manifest, first, batch.size, it) }
                                require(ack.epoch == connection.epoch && ack.recording == manifest.recording &&
                                    ack.manifestSha256 == manifest.sha256 && ack.firstSequence == first && ack.count == batch.size)
                                require(core.confirmReceipts(batch))
                                receipts += batch.size
                                return@repeat
                            }
                            val receipt = core.nextReceipt() ?: return true
                            if (yieldBatch(MAX_CALL_MILLIS)) return false
                            val ack = exchange { transport.receipt(receipt.segment, it) }
                            require(ack.epoch == connection.epoch && ack.segment == receipt.segment)
                            require(core.confirmReceipt(receipt))
                            receipts++
                        }
                        require(core.nextReceipt() == null)
                        return true
                    }
                    if (!flushReceipts()) return result(entries.size, true)
                    for (segment in manifest.segments) {
                        active()
                        if (segment in core.snapshot().phoneSegments) continue
                        if (yieldBatch(SegmentDownloadEngine.MAX_JOB_MILLIS + MAX_CALL_MILLIS))
                            return result(entries.size, true)
                        val ticket = core.beginWork(RecordingWork.DOWNLOAD, segment)
                        val ranged = EncryptedSegmentTransport { requested, offset, maximum ->
                            val request = call()
                            val range = transport.read(requested, offset, maximum, request)
                            try {
                                request.validateReply()
                                require(range.epoch == connection.epoch)
                                range.chunk
                            } catch (failure: Throwable) { range.chunk.bytes.fill(0); throw failure }
                        }
                        engine.download(expectation(segment), core, ticket, ranged, { active(); false },
                            onCheckpoint = { observer.checkpoint(segment, it) })
                        published++
                        observer.published(core.snapshot())
                        if ((!caps.receiptBatch || !manifest.finished ||
                                core.snapshot().pendingReceipts.size >= DurableBleCodec.RECEIPT_BATCH_MAX) &&
                            !flushReceipts()) return result(entries.size, true)
                    }
                    if (!flushReceipts()) return result(entries.size, true)
                } finally { core.close() }
            }
            active()
            return result(entries.size)
        } catch (failure: CancellationException) { throw failure }
        catch (failure: Exception) { throw DurableSyncException(failure) }
        finally {
            // All file/coordinator scopes have ended first. Ambiguous radio
            // callbacks keep the gate quarantined until the adapter proves
            // completion/cancellation; no wait, reset or forced release here.
            lease?.let { admitted ->
                try { transport.endSession(admitted) } finally { admitted.retire() }
            }
        }
    }

    companion object {
        const val MAX_RECORDINGS = 128
        const val PAGE_ENTRIES = 8
        const val MAX_CALL_MILLIS = 30_000L
        const val FULL_METADATA_MILLIS = 300_000L
        const val MAX_SESSION_MILLIS = 15 * 60_000L
    }
}
