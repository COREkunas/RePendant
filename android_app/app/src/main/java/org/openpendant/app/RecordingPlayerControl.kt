package org.openpendant.app

/** A RAM-only transport cursor. A change invalidates an old decoder's progress;
 * the worker must dispose its output before opening a replacement session. */
internal class RecordingPlayerControl(val recording: DurableRecordingId, val durationMillis: Long) {
    data class Cursor(val positionMillis: Long, val playing: Boolean, val version: Long)
    private var cursor = Cursor(0, true, 0)
    init { require(durationMillis > 0) }
    @Synchronized fun snapshot() = cursor
    @Synchronized fun move(position: Long = cursor.positionMillis, playing: Boolean = cursor.playing): Cursor {
        cursor = Cursor(position.coerceIn(0, durationMillis), playing, cursor.version + 1)
        return cursor
    }
    @Synchronized fun progress(version: Long, position: Long): Boolean {
        if (version != cursor.version) return false
        cursor = cursor.copy(positionMillis = position.coerceIn(0, durationMillis))
        return true
    }
    @Synchronized fun current(version: Long) = version == cursor.version
}

internal data class RecordingSeekPoint(val sequence: Int, val offsetSamples: Int)

/** Same canonical fixed-frame duration as the library. The decoder separately
 * authenticates every skipped layout and validates the intra-segment offset. */
internal object RecordingTimeline {
    fun seek(row: RecordingSyncSnapshot, positionMillis: Long): RecordingSeekPoint {
        val duration = requireNotNull(RecordingListPresentation.audioMillis(row))
        require(duration > 0 && positionMillis in 0..duration)
        var samples = minOf(positionMillis * 16, duration * 16 - 1)
        for (segment in requireNotNull(row.manifest).segments) {
            val count = ((segment.byteCount - 357) / 68 * 320).toInt()
            if (samples < count) return RecordingSeekPoint(segment.sequence, samples.toInt())
            samples -= count
        }
        error("Invalid recording timeline")
    }
    fun label(ms: Long): String {
        val seconds = ms.coerceAtLeast(0) / 1000
        return if (seconds < 3600) "%d:%02d".format(java.util.Locale.ROOT, seconds / 60, seconds % 60)
        else "%d:%02d:%02d".format(java.util.Locale.ROOT, seconds / 3600, seconds / 60 % 60, seconds % 60)
    }
}
