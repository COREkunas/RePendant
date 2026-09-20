package org.openpendant.app

import java.util.Locale

/** Presentation only: bytes are durable checkpoints, not proof of complete
 * authenticated audio. ETA covers the CURRENT recording, not unseen catalog
 * entries. Network totals count retries, while checkpoint progress never does. */
data class SyncTransferProgress(val recording: DurableRecordingId, val position: Int, val recordings: Int,
    val totalBytes: Long, val checkpointBytes: Long, val verifiedSegments: Int, val segments: Int,
    val bytesPerSecond: Long?, val remainingSeconds: Long?, val checkingSavedCopy: Boolean = false) {
    val percent: Int get() = if (totalBytes == 0L) 0 else (checkpointBytes * 100 / totalBytes).toInt()
    val text: String get() = buildString {
        append("Recording $position of $recordings · ID ${recording.recordingId.toString().takeLast(8)}\n")
        if (checkingSavedCopy) {
            append(if (segments == 0) "Empty recording · no audio to download\n"
                else "Already saved on phone · $verifiedSegments/$segments parts verified\n")
            append("Checking recording library · pendant copy retained")
            return@buildString
        }
        append("${formatBytes(checkpointBytes)} / ${formatBytes(totalBytes)} checkpointed · $percent%\n")
        append(if (bytesPerSecond != null) "${formatBytes(bytesPerSecond)}/s" else "Measuring transfer speed…")
        remainingSeconds?.let { append(" · about ${if (it < 60) "$it sec" else "${(it + 59) / 60} min"} left for this recording") }
        append("\n$verifiedSegments/$segments parts verified on phone · pendant copy retained")
        if (checkpointBytes == totalBytes) append("\nFinishing verification and receipt acknowledgements…")
    }
    companion object {
        fun formatBytes(bytes: Long): String = if (bytes >= 1024 * 1024)
            String.format(Locale.ROOT,"%.2f MiB",bytes / 1048576.0)
        else String.format(Locale.ROOT,"%.1f KiB",bytes / 1024.0)
    }
}

interface DurableSyncObserver {
    fun recording(row: RecordingSyncSnapshot, position: Int, total: Int) {}
    fun checkpoint(segment: SegmentIdentity, offset: Long) {}
    fun published(row: RecordingSyncSnapshot) {}
}

internal class SyncProgressTracker(private val clock: () -> Long) : DurableSyncObserver {
    private var manifest: RecordingManifest? = null
    private var position = 0
    private var count = 0
    private var started = clock()
    private var received = 0L
    private var last = started
    private var clockValid = true
    private var checkingSavedCopy = false
    private val offsets = LinkedHashMap<SegmentIdentity, Long>()
    private var verified = emptySet<SegmentIdentity>()
    override fun recording(row: RecordingSyncSnapshot, position: Int, total: Int) {
        val next = checkNotNull(row.manifest)
        require(position in 1..total && total <= DurableRecordingSyncSession.MAX_RECORDINGS)
        manifest = next; this.position = position; count = total
        offsets.clear(); verified = row.phoneSegments.intersect(next.segments.toSet())
        verified.forEach { offsets[it] = it.byteCount }
        // Presentation only, after normal coordinator admission. Never use this
        // flag as transfer, receipt, file-integrity or deletion authority.
        checkingSavedCopy = next.finished && row.pendantCopy == PendantCopy.PRESENT &&
            !row.staleVolume && !row.downloadSuppressed && row.deletions.isEmpty() &&
            row.pendingReceipts.isEmpty() && row.phoneSegments == next.segments.toSet()
        // A new batch/reconnection gets a fresh estimate, never a renewed I/O deadline.
        started = clock(); last = started; received = 0; clockValid = started >= 0
    }
    fun received(bytes: Int) { require(bytes in 1..4096); received += bytes; checkingSavedCopy = false }
    override fun checkpoint(segment: SegmentIdentity, offset: Long) {
        require(checkNotNull(manifest).containsExact(segment) && offset in 0..segment.byteCount)
        require(offset >= (offsets[segment] ?: 0L)); offsets[segment] = offset
        checkingSavedCopy = false
    }
    override fun published(row: RecordingSyncSnapshot) {
        require(row.manifest == manifest)
        checkingSavedCopy = false // New files still need their receipt acknowledgements.
        verified = row.phoneSegments.intersect(checkNotNull(manifest).segments.toSet())
        verified.forEach { offsets[it] = it.byteCount }
    }
    fun snapshot(): SyncTransferProgress? {
        val value = manifest ?: return null
        val now = clock()
        if (now < last) clockValid = false
        last = now
        val elapsed = now - started
        val rate = if (clockValid && elapsed >= 2000 && received >= 4096) received * 1000 / elapsed else null
        val total = value.segments.sumOf { it.byteCount }
        val saved = offsets.values.sum()
        val eta = if (rate != null && rate > 0 && total > saved) (total - saved + rate - 1) / rate else null
        return SyncTransferProgress(value.recording, position, count, total, saved, verified.size,
            value.segments.size, rate, eta, checkingSavedCopy)
    }
}
