package org.openpendant.app

import java.util.Collections

/** Exact metadata-only selection, frozen before confirmation. Never reruns the
 * filter on a worker or silently adds recordings that arrived later. */
internal class BulkRecordingDeletion private constructor(val volume: RecordingVolume,
    val location: DeleteLocation, val matchedCount: Int, targets: List<Target>) {
    data class Target(val recording: DurableRecordingId, val revision: Long,
        val manifest: RecordingManifest, val location: DeleteLocation)
    val targets: List<Target> = Collections.unmodifiableList(ArrayList(targets))
    val skippedCount get() = matchedCount - targets.size
    val phoneCount get() = targets.count { it.location != DeleteLocation.PENDANT_ONLY }
    val pendantCount get() = targets.count { it.location != DeleteLocation.PHONE_ONLY }

    /** Entire selection is checked before any mutation, then each row is checked
     * again before its own persisted intent. New unrelated rows are irrelevant. */
    fun validate(current: List<RecordingSyncSnapshot>, currentVolume: RecordingVolume) {
        require(currentVolume == volume && targets.isNotEmpty())
        require(current.map { it.recording }.distinct().size == current.size)
        val byId = current.associateBy { it.recording }
        targets.forEach { validate(it, requireNotNull(byId[it.recording])) }
    }
    fun validate(target: Target, current: RecordingSyncSnapshot) {
        require(target in targets && current.recording == target.recording &&
            current.revision == target.revision && current.manifest == target.manifest &&
            eligible(current, volume, location) && effectiveLocation(current, location) == target.location)
    }
    companion object {
        const val MAX_RECORDINGS = 128
        fun create(filtered: List<RecordingSyncSnapshot>, volume: RecordingVolume, location: DeleteLocation): BulkRecordingDeletion {
            require(filtered.size <= MAX_RECORDINGS && filtered.map { it.recording }.distinct().size == filtered.size)
            return BulkRecordingDeletion(volume, location, filtered.size, filtered.filter { eligible(it, volume, location) }.map {
                Target(it.recording, it.revision, requireNotNull(it.manifest), effectiveLocation(it, location))
            })
        }
        private fun eligible(row: RecordingSyncSnapshot, volume: RecordingVolume, location: DeleteLocation): Boolean {
            val effective = effectiveLocation(row, location)
            return RecordingDeletionScope.allowed(row, volume, effective) &&
                !RecordingListPresentation.pending(row) && !RecordingListPresentation.fullyDeleted(row) &&
                ((effective != DeleteLocation.PENDANT_ONLY && row.phoneSegments.isNotEmpty()) ||
                    (effective != DeleteLocation.PHONE_ONLY && row.pendantCopy != PendantCopy.DELETED))
        }
        private fun effectiveLocation(row: RecordingSyncSnapshot, location: DeleteLocation) =
            if (location == DeleteLocation.BOTH && (row.pendantCopy == PendantCopy.DELETED || row.staleVolume)) DeleteLocation.PHONE_ONLY else location
    }
}
