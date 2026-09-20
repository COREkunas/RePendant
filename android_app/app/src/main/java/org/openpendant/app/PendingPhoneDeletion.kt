package org.openpendant.app

import java.io.Closeable
import java.util.Collections
import java.util.UUID

class DeletionRecoveryRequiredException : IllegalStateException("An existing phone deletion must be recovered before normal recording access")
class PhoneDeletionAdmissionException : IllegalArgumentException("No matching pending phone deletion is authorized")

/** Immutable authority derived only from the persisted finalized manifest and
 * operation, never a filename/header. Contains no payload or filesystem paths. */
class PhoneDeletionPlan internal constructor(snapshot: RecordingSyncSnapshot, operation: UUID, manifestSha256: String) {
    val recording = snapshot.recording
    val operationId = operation
    val manifestSha256: String
    val keepTranscript: Boolean
    val segments: List<SegmentIdentity>
    init {
        val manifest = snapshot.manifest
        val intent = snapshot.deletions.singleOrNull { it.phonePending }
        if (!validOwnedUuid(operation) || manifest == null || manifest.recording != recording || !manifest.finished ||
            !isContentDigest(manifestSha256) || manifest.sha256 != manifestSha256 ||
            intent == null || intent.operationId != operation || intent.recording != recording ||
            intent.manifestSha256 != manifestSha256 || intent.location == DeleteLocation.PENDANT_ONLY ||
            !snapshot.downloadSuppressed || snapshot.pendingReceipts.isNotEmpty()) throw PhoneDeletionAdmissionException()
        this.manifestSha256 = manifestSha256
        keepTranscript = intent.keepTranscript
        segments = Collections.unmodifiableList(ArrayList(manifest.segments))
    }
}

/** Trusted physical adapter contract: exact plan only, derivatives first (only
 * transcripts may be retained), then ciphertext/staging; not-found is progress.
 * Persist every affected directory before returning. Never call this from UI,
 * invoke transport, clear a fault, or recreate download state. No default no-op. */
fun interface PhoneDeletionRemoval { fun removeAndSync(plan: PhoneDeletionPlan) }

/** Restricted restart handle: intentionally exposes no connection/work/receipt,
 * new intent, remote delete or payload API. Owns the same recording authority as
 * ordinary coordinators until close. A callback/CAS failure fences this handle;
 * close and reopen the SAME pending operation after reconciling its finite plan.
 * A genuine durable metadata fault is not bypassed by constructing this handle. */
class PendingPhoneDeletion internal constructor(snapshot: RecordingSyncSnapshot,
    operation: UUID, manifestSha256: String, ownership: RecordingSyncOwnership,
    private val whileStoreOwned: ((() -> Boolean) -> Boolean) = { it() },
    commit: (Long, RecordingSyncSnapshot) -> Unit) : Closeable {
    val plan = PhoneDeletionPlan(snapshot, operation, manifestSha256)
    private val core = RecordingSyncContract(snapshot, ownership, commit)
    fun complete(removal: PhoneDeletionRemoval): Boolean = whileStoreOwned {
        core.performPhoneDeletion(plan.operationId, {}, { removal.removeAndSync(plan) })
    }
    fun requiresReconciliation(): Boolean = core.requiresReconciliation()
    override fun close() = core.close()
}
