package org.openpendant.app

import android.content.Context
import android.os.Looper

/** Optional phone-side display hints, separate from authoritative sync DB,
 * receipts and ciphertext. Loss/corruption means date unavailable, never reset
 * or reconciliation of recording data. No key or recording content access. */
internal class AndroidRecordingListTimes(context: Context) {
    private val prefs = context.getSharedPreferences("recording-list-times-v1", Context.MODE_PRIVATE)
    private fun key(id: DurableRecordingId) = "${id.volume.deviceId}/${id.volume.volumeId}/${id.volume.generation}/${id.recordingId}"
    fun read(rows: List<RecordingSyncSnapshot>): Map<DurableRecordingId, Long> {
        check(Looper.myLooper() != Looper.getMainLooper())
        return rows.mapNotNull { row ->
            val value = try { prefs.getLong(key(row.recording), 0L) } catch (_: Exception) { 0L }
            if (value in 946684800000L..4102444800000L) row.recording to value else null
        }.toMap()
    }
    fun rememberSync(before: List<RecordingSyncSnapshot>, after: List<RecordingSyncSnapshot>): Boolean {
        val additions = RecordingListPresentation.newSyncTimes(before, after, read(after), System.currentTimeMillis())
        if (additions.isEmpty()) return true
        val editor = prefs.edit()
        additions.forEach { (id, time) -> editor.putLong(key(id), time) }
        return editor.commit()
    }
}
