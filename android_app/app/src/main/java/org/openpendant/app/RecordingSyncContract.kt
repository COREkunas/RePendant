package org.openpendant.app

import java.util.Collections
import java.util.UUID

enum class PendantCopy { UNKNOWN, PRESENT, DELETED }
enum class DeleteLocation { PHONE_ONLY, PENDANT_ONLY, BOTH }
enum class PendantDeletion { NOT_REQUESTED, PENDING, CONFIRMED, STALE_GENERATION }
enum class RecordingWork { DOWNLOAD, PLAYBACK, TRANSCRIPTION }
class RecordingSyncBusyException : IllegalStateException("Recording state is currently owned by another operation")

data class RecordingDeletionIntent(
    val operationId: UUID,
    val recording: DurableRecordingId,
    val manifestSha256: String,
    val location: DeleteLocation,
    val keepTranscript: Boolean,
    val phonePending: Boolean,
    val pendant: PendantDeletion,
    val tombstoneRevision: Long? = null,
) {
    init { require(validOwnedUuid(operationId)) { "Invalid deletion operation identifier" } }
}

/** Metadata only; persistence/schema migrations and recovery reconciliation remain unimplemented. */
data class RecordingSyncSnapshot(
    val recording: DurableRecordingId,
    val revision: Long = 0,
    val workGeneration: Long = 0,
    val manifest: RecordingManifest? = null,
    val pendantCopy: PendantCopy = PendantCopy.UNKNOWN,
    val downloadSuppressed: Boolean = false,
    val staleVolume: Boolean = false,
    val phoneSegments: Set<SegmentIdentity> = emptySet(),
    val pendingReceipts: Set<SegmentIdentity> = emptySet(),
    val deletions: List<RecordingDeletionIntent> = emptyList(),
)

class RecordingWorkTicket internal constructor(
    val segment: SegmentIdentity,
    val kind: RecordingWork,
    val generation: Long,
    internal val instance: UUID,
    internal val job: UUID,
    /** Non-null only for explicit whole-recording playback. Never inferred from files. */
    internal val finalManifest: RecordingManifest? = null,
)

class PendantDeleteRequest internal constructor(
    val intent: RecordingDeletionIntent,
    internal val connection: UUID,
)

class SegmentReceipt internal constructor(
    val segment: SegmentIdentity,
    internal val connection: UUID,
)

/** One injected authority must be shared by every recording coordinator in the
 * app process. It prevents concurrent file-owning instances for the same identity.
 * This is not an inter-process/file lock and independently constructed authorities
 * do not coordinate; actual app ownership wiring remains an integration gate.
 */
class RecordingSyncOwnership {
    private val owners = mutableMapOf<DurableRecordingId, UUID>()
    internal fun acquire(recording: DurableRecordingId, owner: UUID) = synchronized(owners) {
        if (recording in owners) throw RecordingSyncBusyException()
        owners[recording] = owner
    }
    internal fun release(recording: DurableRecordingId, owner: UUID) = synchronized(owners) {
        check(owners[recording] == owner) { "Recording coordinator ownership differs" }
        owners.remove(recording)
    }
}

/**
 * Pure Kotlin contract, deliberately NOT connected to Activity/BLE/files/Room.
 *
 * commit must atomically compare expectedRevision and durably commit next before
 * returning. Throwing prevents releasing further work/requests, but does not
 * imply rollback of any earlier file callback or ambiguous commit. It must not reenter
 * this contract. Production integration still needs a real transactional store,
 * file/DB crash reconciliation, authenticated durable-protocol negotiation and
 * explicit user controls; installed features remain unavailable.
 *
 * Local publication/removal callbacks execute under this instance's lock after
 * token checks. All future filesystem, playback and ASR mutation must use this
 * same coordinator, not a separate check-then-write operation. Callbacks must be
 * bounded/non-reentrant and never call UI/BLE. No callback is invoked on startup.
 *
 * A local callback or commit failure irreversibly fences this instance. It may
 * have published/deleted a file before throwing; later work and receipt dispatch
 * are forbidden. Close it and externally reconcile files with durable metadata
 * before constructing a replacement. Reloading the old database blindly is not
 * reconciliation. Required shared ownership excludes other cooperating instances
 * BEFORE file callbacks, unlike a CAS performed only after file publication.
 */
class RecordingSyncContract(
    initial: RecordingSyncSnapshot,
    private val ownership: RecordingSyncOwnership,
    private val commit: (expectedRevision: Long, next: RecordingSyncSnapshot) -> Unit,
) {
    private val lock = Any()
    private val instance = UUID.randomUUID()
    private var state = frozen(initial)
    private var connection: UUID? = null
    private var active: RecordingWorkTicket? = null
    private var publishing = false
    private var reconciliationRequired = false
    private var closed = false
    // These are immutable, privately owned snapshots. Cache only the expensive
    // complete-manifest proof; ticket/lifecycle/deletion checks still run for
    // every tiny PCM write. No proof survives a metadata replacement.
    private var playbackProofState: RecordingSyncSnapshot? = null
    private var playbackProofManifest: RecordingManifest? = null
    private var playbackProofScans = 0L

    internal fun playbackValidationScans(): Long = synchronized(lock) { playbackProofScans }

    init { ownership.acquire(state.recording, instance) }

    fun snapshot(): RecordingSyncSnapshot = synchronized(lock) { state }
    fun requiresReconciliation(): Boolean = synchronized(lock) { reconciliationRequired }
    fun remoteStateIsFresh(): Boolean = synchronized(lock) { !closed && !reconciliationRequired && connection != null && !state.staleVolume }

    fun close() = synchronized(lock) {
        check(!publishing) { "Publication/persistence callbacks must not reenter" }
        if (closed) return@synchronized
        closed = true
        clearPlaybackProof()
        active = null
        connection = null
        ownership.release(state.recording, instance)
    }

    /** Authentication and capability checking happen outside this metadata layer. */
    fun authenticatedConnection(volume: RecordingVolume, durableProtocolAvailable: Boolean) = synchronized(lock) {
        checkNotPublishing()
        connection = null
        active = null // Old connection/work callbacks never survive reconnection.
        require(volume.deviceId == state.recording.volume.deviceId) { "Different pendant identity" }
        if (volume != state.recording.volume) {
            update(state.copy(staleVolume = true, pendantCopy = PendantCopy.UNKNOWN,
                workGeneration = increment(state.workGeneration), deletions = state.deletions.map {
                    if (it.pendant == PendantDeletion.PENDING) it.copy(pendant = PendantDeletion.STALE_GENERATION) else it
                }))
            return@synchronized
        }
        require(durableProtocolAvailable && !state.staleVolume) { "Durable recording protocol or volume is unavailable" }
        connection = UUID.randomUUID()
    }

    fun disconnected() = synchronized(lock) {
        checkNotPublishing()
        connection = null
        active = null
        // Last known copy and pending intents remain visible, explicitly stale.
    }

    fun observeManifest(manifest: RecordingManifest) = synchronized(lock) {
        checkNotPublishing()
        requireConnected()
        require(manifest.recording == state.recording) { "Catalog recording identity differs" }
        require(state.pendantCopy != PendantCopy.DELETED) { "Deleted recording cannot reappear" }
        val old = state.manifest
        if (old != null) {
            require(manifest.revision >= old.revision) { "Stale catalog revision" }
            if (manifest.revision == old.revision) {
                require(manifest == old) { "Catalog changed without a revision" }
                return@synchronized
            }
            require(manifest.segments.take(old.segments.size) == old.segments &&
                manifest.segments.size >= old.segments.size) { "Sealed segment identity changed" }
            require(!old.finished || (manifest.finished && manifest.segments == old.segments &&
                manifest.sha256 == old.sha256)) { "Finalized recording changed" }
        }
        update(state.copy(manifest = manifest, pendantCopy = PendantCopy.PRESENT))
    }

    /** Explicit Download again is the only operation that clears suppression. */
    fun allowDownloadAgain() = synchronized(lock) {
        checkNotPublishing()
        requireConnected()
        require(!pendingDeletion() && state.pendantCopy == PendantCopy.PRESENT) { "Recording cannot be downloaded now" }
        if (state.downloadSuppressed) update(state.copy(downloadSuppressed = false))
    }

    fun beginWork(kind: RecordingWork, segment: SegmentIdentity): RecordingWorkTicket = synchronized(lock) {
        checkNotPublishing()
        require(active == null && !pendingDeletion()) { "Recording already has work or deletion pending" }
        require(knownSegment(segment)) { "Unknown sealed segment identity" }
        if (kind == RecordingWork.DOWNLOAD) {
            requireConnected()
            require(!state.downloadSuppressed && state.pendantCopy == PendantCopy.PRESENT &&
                segment !in state.phoneSegments) { "Automatic download is suppressed or unnecessary" }
        } else {
            require(segment in state.phoneSegments) { "Segment is not durably present on this phone" }
        }
        val generation = increment(state.workGeneration)
        update(state.copy(workGeneration = generation))
        RecordingWorkTicket(segment, kind, generation, instance, UUID.randomUUID()).also { active = it }
    }

    fun isCurrent(ticket: RecordingWorkTicket): Boolean = synchronized(lock) { current(ticket) }

    /** Explicit local playback only. All sealed segments, the final catalog and
     * its complete identity are pinned for this job. No network/key/audio action. */
    fun beginPlayback(manifest: RecordingManifest, startSequence: Int = 0): RecordingWorkTicket = synchronized(lock) {
        beginCompleteLocalWork(manifest,startSequence,RecordingWork.PLAYBACK)
    }

    /** Pins a complete immutable phone copy for an explicitly authorized bounded
     * remote export. No network operation is performed under this lock. */
    internal fun beginTranscription(manifest:RecordingManifest):RecordingWorkTicket = synchronized(lock) {
        beginCompleteLocalWork(manifest,0,RecordingWork.TRANSCRIPTION)
    }
    private fun beginCompleteLocalWork(manifest:RecordingManifest,startSequence:Int,kind:RecordingWork):RecordingWorkTicket {
        checkNotPublishing()
        require(active == null && !pendingDeletion() && !state.downloadSuppressed && !state.staleVolume)
        require(manifest == state.manifest && manifest.finished && manifest.recording == state.recording &&
            startSequence in manifest.segments.indices && state.phoneSegments.containsAll(manifest.segments))
        val generation = increment(state.workGeneration)
        update(state.copy(workGeneration = generation))
        return RecordingWorkTicket(manifest.segments[startSequence], kind, generation,
            instance, UUID.randomUUID(), manifest).also { active = it }
    }

    /** Small NONBLOCKING audio queue operation serialized against deletion.
     * It has no disk/metadata side effects and must not reenter the coordinator.
     * A failed sink is stopped by the playback session; it is not a database fault.
     * Already queued samples must be flushed by the session/deletion adapter. */
    internal fun playbackStep(ticket: RecordingWorkTicket, action: () -> Unit): Boolean = synchronized(lock) {
        checkNotPublishing()
        if (!current(ticket) || ticket.finalManifest == null || ticket.kind != RecordingWork.PLAYBACK) return@synchronized false
        publishing = true
        try { action(); true } finally { publishing = false }
    }

    internal fun finishPlayback(ticket: RecordingWorkTicket): Boolean = synchronized(lock) {
        checkNotPublishing()
        if (!current(ticket) || ticket.finalManifest == null || ticket.kind != RecordingWork.PLAYBACK) return@synchronized false
        active = null
        true
    }

    /** File digest verification and durable atomic publication are caller obligations.
     * Audio commits first; only successful metadata commit permits a remote receipt.
     * A metadata failure after file publication requires later file/DB reconciliation,
     * never a false receipt or automatic deletion of the pendant source.
     */
    fun publishDownloadedSegment(ticket: RecordingWorkTicket, verifiedSha256: String, byteCount: Long,
                                 publishVerifiedFile: () -> Unit): Boolean = synchronized(lock) {
        checkNotPublishing()
        if (!current(ticket)) return@synchronized false
        require(ticket.kind == RecordingWork.DOWNLOAD && verifiedSha256 == ticket.segment.sha256 &&
            byteCount == ticket.segment.byteCount) { "Downloaded segment digest or size differs" }
        try {
            external { publishVerifiedFile() }
            update(state.copy(phoneSegments = state.phoneSegments + ticket.segment,
                pendingReceipts = state.pendingReceipts + ticket.segment))
            true
        } catch (error: Throwable) {
            // Includes construction of the next snapshot before update() starts.
            fence(error)
        } finally {
            active = null // Ambiguous publication is reconciled, never replayed as the same job.
        }
    }

    /** Publication guard for explicit ASR/playback work; never starts either. */
    fun completeLocalWork(ticket: RecordingWorkTicket, publish: () -> Unit): Boolean = synchronized(lock) {
        checkNotPublishing()
        if (!current(ticket)) return@synchronized false
        require(ticket.kind != RecordingWork.DOWNLOAD) { "Download needs verified segment publication" }
        try { external { publish() }; true } finally { active = null }
    }

    fun cancelWork(ticket: RecordingWorkTicket) = synchronized(lock) {
        checkNotPublishing()
        if (current(ticket)) {
            update(state.copy(workGeneration = increment(state.workGeneration)))
            active = null
        }
    }

    /** This is a receipt only: its acknowledgement NEVER requests source deletion. */
    fun nextReceipt(): SegmentReceipt? = synchronized(lock) {
        if (closed || reconciliationRequired || publishing || connection == null || state.staleVolume || pendingDeletion()) return@synchronized null
        state.pendingReceipts.minByOrNull { it.sequence }?.let { SegmentReceipt(it, connection!!) }
    }

    fun confirmReceipt(receipt: SegmentReceipt): Boolean = synchronized(lock) {
        checkNotPublishing()
        if (connection == null || receipt.connection != connection || state.staleVolume) return@synchronized false
        require(knownSegment(receipt.segment)) { "Receipt identity differs" }
        if (receipt.segment !in state.pendingReceipts) return@synchronized false
        update(state.copy(pendingReceipts = state.pendingReceipts - receipt.segment))
        true
    }

    /** Exact contiguous prefix of pending, already-published phone identities.
     * No receipt is inferred merely from sequence numbers or a file name. */
    fun nextReceiptBatch(): List<SegmentReceipt> = synchronized(lock) {
        if (closed || reconciliationRequired || publishing || connection == null || state.staleVolume ||
            pendingDeletion() || state.manifest?.finished != true) return@synchronized emptyList()
        val pending = state.pendingReceipts.sortedBy { it.sequence }
        if (pending.isEmpty()) return@synchronized emptyList()
        val first = pending.first().sequence
        pending.take(DurableBleCodec.RECEIPT_BATCH_MAX).withIndex().takeWhile { it.value.sequence == first + it.index }
            .map { SegmentReceipt(it.value, connection!!) }
    }

    /** Called only after an exact durable batch echo; one atomic phone commit.
     * A missed reply leaves every intent pending for idempotent later replay. */
    fun confirmReceipts(receipts: List<SegmentReceipt>): Boolean = synchronized(lock) {
        checkNotPublishing()
        if (connection == null || state.staleVolume || pendingDeletion() || receipts.isEmpty() ||
            receipts.size > DurableBleCodec.RECEIPT_BATCH_MAX) return@synchronized false
        val first = receipts.first().segment.sequence
        if (receipts.withIndex().any { (index, r) -> r.connection != connection ||
                r.segment.sequence != first + index || !knownSegment(r.segment) || r.segment !in state.pendingReceipts })
            return@synchronized false
        update(state.copy(pendingReceipts = state.pendingReceipts - receipts.map { it.segment }.toSet()))
        true
    }

    /** Persist intent + suppression + generation fence before returning any work. */
    fun requestDeletion(operationId: UUID, location: DeleteLocation, keepTranscript: Boolean = false): RecordingDeletionIntent = synchronized(lock) {
        checkNotPublishing()
        val manifest = state.manifest ?: error("Recording manifest is unavailable")
        require(manifest.finished) { "Stop and finalize active recording before deletion" }
        val old = state.deletions.find { it.operationId == operationId }
        if (old != null) {
            require(old.location == location && old.keepTranscript == keepTranscript &&
                old.recording == state.recording && old.manifestSha256 == manifest.sha256) { "Deletion operation ID was reused" }
            return@synchronized old
        }
        require(!pendingDeletion() && state.deletions.size < MAX_DELETE_INTENTS) { "Deletion is pending or retained-intent limit reached" }
        require(location == DeleteLocation.PHONE_ONLY || !state.staleVolume) { "Remote deletion belongs to a stale volume" }
        val phone = location != DeleteLocation.PENDANT_ONLY
        val pendant = when {
            location == DeleteLocation.PHONE_ONLY -> PendantDeletion.NOT_REQUESTED
            state.pendantCopy == PendantCopy.DELETED -> PendantDeletion.CONFIRMED
            else -> PendantDeletion.PENDING
        }
        val tombstone = if (pendant == PendantDeletion.CONFIRMED)
            state.deletions.first { it.pendant == PendantDeletion.CONFIRMED }.tombstoneRevision else null
        val intent = RecordingDeletionIntent(operationId, state.recording, manifest.sha256,
            location, keepTranscript, phone, pendant, tombstone)
        update(state.copy(workGeneration = increment(state.workGeneration),
            downloadSuppressed = state.downloadSuppressed || phone,
            pendingReceipts = if (phone) emptySet() else state.pendingReceipts,
            deletions = state.deletions + intent))
        active = null
        intent
    }

    /** Derivatives precede audio; failure leaves pending intent and no completion claim. */
    fun performPhoneDeletion(operationId: UUID, deleteTranscript: () -> Unit, deleteAudio: () -> Unit): Boolean = synchronized(lock) {
        checkNotPublishing()
        val intent = deletion(operationId)
        if (!intent.phonePending) return@synchronized false
        try {
            external {
                if (!intent.keepTranscript) deleteTranscript()
                deleteAudio()
            }
            update(state.copy(phoneSegments = emptySet(), pendingReceipts = emptySet(),
                deletions = state.deletions.map { if (it.operationId == operationId) it.copy(phonePending = false) else it }))
            true
        } catch (error: Throwable) {
            // A copy/allocation failure after removal is ambiguous too.
            fence(error)
        }
    }

    /** Offline intent is retained; never pretend a missing link deleted anything. */
    fun nextPendantDeletion(): PendantDeleteRequest? = synchronized(lock) {
        if (closed || reconciliationRequired || publishing || connection == null || state.staleVolume) return@synchronized null
        state.deletions.firstOrNull { it.pendant == PendantDeletion.PENDING }?.let {
            PendantDeleteRequest(it, connection!!)
        }
    }

    fun confirmPendantDeletion(request: PendantDeleteRequest, tombstoneRevision: Long,
                               manifestSha256: String): Boolean = synchronized(lock) {
        checkNotPublishing()
        if (connection == null || request.connection != connection || state.staleVolume) return@synchronized false
        val intent = deletion(request.intent.operationId)
        require(intent.recording == state.recording && request.intent.recording == state.recording &&
            request.intent.manifestSha256 == intent.manifestSha256 && manifestSha256 == intent.manifestSha256) { "Remote deletion binding differs" }
        require(tombstoneRevision > (state.manifest?.revision ?: -1)) { "Remote tombstone revision is stale" }
        if (intent.pendant == PendantDeletion.CONFIRMED) return@synchronized intent.tombstoneRevision == tombstoneRevision
        require(intent.pendant == PendantDeletion.PENDING) { "Remote deletion was not requested" }
        update(state.copy(pendantCopy = PendantCopy.DELETED, pendingReceipts = emptySet(),
            deletions = state.deletions.map { if (it.operationId == intent.operationId)
                it.copy(pendant = PendantDeletion.CONFIRMED, tombstoneRevision = tombstoneRevision) else it }))
        true // Phone audio/transcripts deliberately unchanged.
    }

    private fun current(ticket: RecordingWorkTicket): Boolean = !closed && !reconciliationRequired && active === ticket && ticket.instance == instance &&
        ticket.generation == state.workGeneration && knownSegment(ticket.segment) && !pendingDeletion() &&
        (ticket.finalManifest == null || (ticket.kind in setOf(RecordingWork.PLAYBACK,RecordingWork.TRANSCRIPTION) && playbackStateValid(ticket.finalManifest))) &&
        (ticket.kind != RecordingWork.DOWNLOAD || (connection != null && !state.staleVolume && !state.downloadSuppressed))

    private fun playbackStateValid(manifest: RecordingManifest): Boolean {
        if (playbackProofState === state && playbackProofManifest === manifest) return true
        if (playbackProofScans < Long.MAX_VALUE) playbackProofScans++ // Diagnostics only, never authority.
        val valid = state.manifest == manifest && !state.downloadSuppressed && !state.staleVolume &&
            state.phoneSegments.containsAll(manifest.segments)
        if (valid) {
            playbackProofState = state
            playbackProofManifest = manifest
        }
        return valid
    }
    private fun clearPlaybackProof() {
        playbackProofState = null
        playbackProofManifest = null
    }

    private fun knownSegment(segment: SegmentIdentity): Boolean = segment.recording == state.recording &&
        state.manifest?.segments?.getOrNull(segment.sequence) == segment
    private fun pendingDeletion(): Boolean = state.deletions.any { it.phonePending || it.pendant == PendantDeletion.PENDING }
    private fun deletion(id: UUID): RecordingDeletionIntent = state.deletions.find { it.operationId == id }
        ?: error("Unknown deletion intent")
    private fun requireConnected() = require(connection != null && !state.staleVolume) { "Authenticated current durable volume is required" }
    private fun checkNotPublishing() {
        check(!closed && !reconciliationRequired) { "Recording coordinator is closed or requires reconciliation" }
        check(!publishing) { "Publication/persistence callbacks must not reenter" }
    }
    private fun external(action: () -> Unit) {
        publishing = true
        try { action() }
        catch (error: Throwable) { fence(error) }
        finally { publishing = false }
    }
    private fun update(next: RecordingSyncSnapshot) {
        try {
            val checked = frozen(next.copy(revision = increment(state.revision)))
            external { commit(state.revision, checked) }
            state = checked
            clearPlaybackProof()
        } catch (error: Throwable) {
            // Validation/counter exhaustion may also follow a completed file
            // mutation, so no update failure may bypass the reconciliation fence.
            fence(error)
        }
    }
    private fun fence(error: Throwable): Nothing {
        reconciliationRequired = true
        clearPlaybackProof()
        active = null
        connection = null
        throw error
    }

    companion object {
        // No automatic tombstone compaction: future database cursor rules must
        // prove replay safety before making room. This is not a protocol limit.
        const val MAX_DELETE_INTENTS = 256
        private fun increment(value: Long): Long {
            require(value in 0 until Long.MAX_VALUE) { "Recording metadata generation exhausted" }
            return value + 1
        }
        private fun frozen(value: RecordingSyncSnapshot): RecordingSyncSnapshot {
            require(value.revision >= 0 && value.workGeneration >= 0 &&
                (value.manifest == null || value.manifest.recording == value.recording)) { "Invalid recording metadata snapshot" }
            require(value.phoneSegments.all { it.recording == value.recording && value.manifest?.segments?.getOrNull(it.sequence) == it }
                && value.pendingReceipts.all { it in value.phoneSegments }) { "Local segment/receipt identity differs" }
            require(value.deletions.size <= MAX_DELETE_INTENTS && value.deletions.map { it.operationId }.toSet().size == value.deletions.size &&
                value.deletions.count { it.phonePending } <= 1 &&
                (value.deletions.none { it.phonePending } || value.pendingReceipts.isEmpty()) &&
                value.deletions.all {
                    it.recording == value.recording && isContentDigest(it.manifestSha256) &&
                    value.manifest?.finished == true && it.manifestSha256 == value.manifest.sha256 &&
                    (!it.phonePending || (it.location != DeleteLocation.PENDANT_ONLY && value.downloadSuppressed)) &&
                    (if (it.location == DeleteLocation.PHONE_ONLY) it.pendant == PendantDeletion.NOT_REQUESTED
                     else it.pendant != PendantDeletion.NOT_REQUESTED) &&
                    (if (it.pendant == PendantDeletion.CONFIRMED)
                        it.tombstoneRevision != null && it.tombstoneRevision > value.manifest.revision
                     else it.tombstoneRevision == null) &&
                    (it.pendant != PendantDeletion.PENDING || !value.staleVolume) &&
                    (it.pendant != PendantDeletion.STALE_GENERATION || value.staleVolume)
                }) { "Invalid deletion outbox" }
            require(value.pendantCopy != PendantCopy.DELETED ||
                value.deletions.any { it.pendant == PendantDeletion.CONFIRMED }) { "Remote deletion lacks durable tombstone" }
            return value.copy(phoneSegments = Collections.unmodifiableSet(HashSet(value.phoneSegments)),
                pendingReceipts = Collections.unmodifiableSet(HashSet(value.pendingReceipts)),
                deletions = Collections.unmodifiableList(ArrayList(value.deletions)))
        }
    }
}
