package org.openpendant.app

/** Frozen, finished recording selection. Never authorizes deletions or another
 * recording, including after a bounded Bluetooth reconnect. */
data class SelectedRecordingDownload(val recording: DurableRecordingId, val manifestSha256: String) {
    init { require(isContentDigest(manifestSha256)) }
    fun matches(row: RecordingSyncSnapshot): Boolean = row.recording == recording &&
        !row.staleVolume && !row.downloadSuppressed && !RecordingListPresentation.pending(row) &&
        row.pendantCopy == PendantCopy.PRESENT && row.manifest?.let {
            it.finished && it.segments.isNotEmpty() && it.sha256 == manifestSha256
        } == true
    fun matches(entry: DurableCatalogEntry) = entry.recording == recording && entry.finished &&
        entry.sealedSegments > 0 && entry.manifestSha256 == manifestSha256
    companion object {
        fun from(row: RecordingSyncSnapshot): SelectedRecordingDownload? = row.manifest?.let {
            SelectedRecordingDownload(row.recording, it.sha256).takeIf { choice ->
                choice.matches(row) && !RecordingListPresentation.phoneComplete(row)
            }
        }
    }
}

/** Defense in depth: even a scheduler mistake cannot download another item or
 * apply a queued deletion during a selected download. Catalog remains read-only. */
class SelectedRecordingTransport(private val source: DurableRecordingTransport,
    private val selection: SelectedRecordingDownload): DurableRecordingTransport by source {
    override fun manifest(entry: DurableCatalogEntry, call: DurableSyncCall): DurableManifestReply {
        require(selection.matches(entry)); return source.manifest(entry, call)
    }
    override fun read(segment: SegmentIdentity, offset: Long, maximumBytes: Int, call: DurableSyncCall): DurableRangeReply {
        require(segment.recording == selection.recording); return source.read(segment, offset, maximumBytes, call)
    }
    override fun receipt(segment: SegmentIdentity, call: DurableSyncCall): DurableReceiptReply {
        require(segment.recording == selection.recording); return source.receipt(segment, call)
    }
    override fun receiptRange(manifest: RecordingManifest, first: Int, count: Int, call: DurableSyncCall): DurableReceiptRangeReply {
        require(manifest.recording == selection.recording && manifest.sha256 == selection.manifestSha256)
        return source.receiptRange(manifest, first, count, call)
    }
    override fun delete(intent: RecordingDeletionIntent, call: DurableSyncCall): DurableTombstoneReply =
        error("Selected downloads never delete recordings")
}
