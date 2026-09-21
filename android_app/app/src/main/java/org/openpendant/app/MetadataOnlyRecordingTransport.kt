package org.openpendant.app

/** Independent boundary: inventory code cannot accidentally read audio, acknowledge
 * downloads or apply deletions, even if the sync state machine changes later. */
internal class MetadataOnlyRecordingTransport(delegate: DurableRecordingTransport) : DurableRecordingTransport by delegate {
    override fun read(segment: SegmentIdentity, offset: Long, maximumBytes: Int, call: DurableSyncCall): DurableRangeReply = denied()
    override fun receipt(segment: SegmentIdentity, call: DurableSyncCall): DurableReceiptReply = denied()
    override fun receiptRange(manifest: RecordingManifest, first: Int, count: Int, call: DurableSyncCall): DurableReceiptRangeReply = denied()
    override fun delete(intent: RecordingDeletionIntent, call: DurableSyncCall): DurableTombstoneReply = denied()
    private fun denied(): Nothing = throw IllegalStateException("Details-only sync forbids payload and mutation operations")
}
