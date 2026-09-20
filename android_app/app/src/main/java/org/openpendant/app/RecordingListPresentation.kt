package org.openpendant.app

import java.util.Locale

enum class RecordingListFilter(val label: String) {
    ALL("All"), PENDANT("On pendant"), PHONE("On phone"), NEEDS_SYNC("Needs sync"), HISTORY("History")
}

/** Presentation from existing validated metadata only. Never opens audio,
 * invents recording dates, changes a manifest, or grants any storage action. */
object RecordingListPresentation {
    fun hasAudio(row: RecordingSyncSnapshot) = row.manifest?.segments?.isNotEmpty() == true
    fun pending(row: RecordingSyncSnapshot) = row.deletions.any { it.phonePending || it.pendant == PendantDeletion.PENDING }
    /** Retain tombstones in the database but never show a finished deletion as
     * a recording. Offline/unknown/pending absence is not confirmed deletion. */
    fun fullyDeleted(row: RecordingSyncSnapshot): Boolean = !pending(row) && !row.staleVolume &&
        row.pendantCopy == PendantCopy.DELETED && row.phoneSegments.isEmpty() && row.pendingReceipts.isEmpty()
    /** An authenticated storage replacement retires its old namespace. Keep its
     * suppression/outbox metadata internally, but an empty retired placeholder
     * is not a saved recording. This is NOT proof of a remote deletion. */
    fun retiredEmpty(row: RecordingSyncSnapshot): Boolean = row.staleVolume && !pending(row) &&
        row.phoneSegments.isEmpty() && row.pendingReceipts.isEmpty()
    fun hasPhoneCopy(row: RecordingSyncSnapshot): Boolean = row.phoneSegments.isNotEmpty()
    fun phoneComplete(row: RecordingSyncSnapshot): Boolean = row.manifest?.let {
        it.finished && it.segments.isNotEmpty() && row.phoneSegments == it.segments.toSet() &&
            !row.downloadSuppressed && !row.staleVolume && row.deletions.none { deletion -> deletion.phonePending }
    } == true
    fun history(row: RecordingSyncSnapshot): Boolean = !pending(row) && !row.staleVolume &&
        ((row.manifest?.finished == true && !hasAudio(row) && row.phoneSegments.isEmpty()) ||
            (row.pendantCopy == PendantCopy.DELETED && row.phoneSegments.isEmpty()))
    fun needsSync(row: RecordingSyncSnapshot): Boolean = pending(row) || row.pendingReceipts.isNotEmpty() ||
        (!row.staleVolume && !row.downloadSuppressed && hasAudio(row) && row.pendantCopy == PendantCopy.PRESENT && !phoneComplete(row))
    fun matches(row: RecordingSyncSnapshot, filter: RecordingListFilter): Boolean = !fullyDeleted(row) && !retiredEmpty(row) && when (filter) {
        RecordingListFilter.HISTORY -> history(row)
        RecordingListFilter.ALL -> !history(row)
        RecordingListFilter.PENDANT -> !history(row) && !row.staleVolume && row.pendantCopy == PendantCopy.PRESENT
        RecordingListFilter.PHONE -> !history(row) && row.phoneSegments.isNotEmpty()
        RecordingListFilter.NEEDS_SYNC -> needsSync(row)
    }

    /** OPNDMF1 profile2 enforces this fixed20ms/68B frame geometry. This is
     * committed AUDIO duration, not wall-clock span including recorded gaps.
     * Unknown/noncanonical geometry is never guessed from approximate bytes. */
    fun audioMillis(row: RecordingSyncSnapshot): Long? {
        val manifest = row.manifest ?: return null
        if (manifest.segments.size > DurableManifestCodec.FULL_MAX_SEGMENTS ||
            manifest.revision != manifest.segments.size * 4L + (manifest.revision % 4) ||
            manifest.revision % 4 !in 0L..2L) return null
        var frames = 0L
        for (segment in manifest.segments) {
            val encoded = segment.byteCount - 357L
            if (segment.byteCount !in 425L..34357L || encoded % 68L != 0L) return null
            val count = encoded / 68L
            if (count !in 1L..500L) return null
            frames += count
        }
        return frames * 20L
    }
    fun duration(row: RecordingSyncSnapshot): String = audioMillis(row)?.let { ms ->
        if (ms in 1L..999L) "${ms} ms" else if (ms < 60_000) {
            if (ms % 1000L == 0L) "${ms / 1000} sec" else String.format(Locale.ROOT, "%.1f sec", ms / 1000.0)
        } else String.format(Locale.ROOT, "%d:%02d", ms / 60_000, ms / 1000 % 60)
    } ?: "Duration unavailable"
    fun title(row: RecordingSyncSnapshot) = when {
        row.manifest == null -> "Recording details pending"
        row.manifest.finished && !hasAudio(row) -> "Empty recording"
        !row.manifest.finished -> "Recording in progress"
        row.manifest.revision % 4 == 2L -> "Recovered recording · ${duration(row)}"
        else -> "Recording · ${duration(row)}"
    }
    fun phoneLabel(row: RecordingSyncSnapshot): String = when {
        row.deletions.any { it.phonePending } -> "Phone · Delete pending"
        row.staleVolume && hasPhoneCopy(row) -> "Phone · Old storage copy (${row.phoneSegments.size}/${row.manifest?.segments?.size ?: "?"} parts)"
        row.phoneSegments.isNotEmpty() && phoneComplete(row) -> "Phone · Saved"
        row.phoneSegments.isNotEmpty() -> "Phone · ${row.phoneSegments.size}/${row.manifest?.segments?.size ?: "?"} parts"
        row.downloadSuppressed -> "Phone · Removed"
        row.manifest?.finished == true && !hasAudio(row) -> "Phone · No audio"
        else -> "Phone · Not downloaded"
    }
    fun pendantLabel(row: RecordingSyncSnapshot): String = when {
        row.staleVolume -> "Pendant · Old storage"
        row.deletions.any { it.pendant == PendantDeletion.PENDING } -> "Pendant · Delete queued"
        row.pendantCopy == PendantCopy.DELETED -> "Pendant · Removed"
        row.pendantCopy == PendantCopy.PRESENT -> "Pendant · Saved"
        else -> "Pendant · Not checked"
    }
    fun ordered(rows: List<RecordingSyncSnapshot>, firstSynced: Map<DurableRecordingId, Long>) =
        rows.sortedWith(compareByDescending<RecordingSyncSnapshot> { firstSynced[it.recording] ?: Long.MIN_VALUE }
            .thenBy { it.recording.recordingId.toString() }.thenBy { it.recording.toString() })

    /** Only a real successful explicit sync can add a first-sync timestamp.
     * Existing complete entries stay undated; refresh/install is not recording
     * creation time. Re-download never overwrites an existing first-sync date. */
    fun newSyncTimes(before: List<RecordingSyncSnapshot>, after: List<RecordingSyncSnapshot>,
        existing: Map<DurableRecordingId, Long>, nowMillis: Long): Map<DurableRecordingId, Long> {
        if (nowMillis !in 946684800000L..4102444800000L) return emptyMap()
        val old = before.associateBy { it.recording }
        return after.filter { it.recording !in existing && phoneComplete(it) && it.pendingReceipts.isEmpty() &&
            old[it.recording]?.let(::phoneComplete) != true }.associate { it.recording to nowMillis }
    }
}
