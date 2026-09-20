package org.openpendant.app

/** A retired volume may still have local files. Its identity stays intact and
 * may authorize PHONE_ONLY deletion, never a request against the new volume. */
internal object RecordingDeletionScope {
    fun retired(row: RecordingSyncSnapshot, current: RecordingVolume): Boolean =
        row.staleVolume && row.recording.volume != current && row.recording.volume.deviceId == current.deviceId

    fun allowed(row: RecordingSyncSnapshot, current: RecordingVolume, location: DeleteLocation): Boolean =
        row.manifest?.finished == true &&
            ((!row.staleVolume && row.recording.volume == current) ||
                (retired(row, current) && (location == DeleteLocation.PHONE_ONLY ||
                    row.deletions.singleOrNull { it.phonePending }?.let {
                        // Only finish local work from the original saved scope.
                        // Its remote request was fenced by volume replacement.
                        it.location == location && it.pendant == PendantDeletion.STALE_GENERATION
                    } == true)))
}
