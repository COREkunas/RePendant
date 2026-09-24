package org.openpendant.app

import org.json.JSONObject
import java.util.Locale

internal data class HeaderBattery(val percent: Int, val readAt: Long)
internal data class HeaderStorage(val used: Long, val total: Long, val entriesUsed: Int, val entriesTotal: Int, val readAt: Long) {
    val percent get() = (used * 100 / total).toInt()
}

/** Public display cache only. Never used for recording, power or deletion authority. */
internal data class PendantHeaderSnapshot(val peer: String, val readAt: Long, val activity: String,
    val usbPower: Boolean?, val battery: HeaderBattery?, val storage: HeaderStorage?) {
    fun encode(): String = JSONObject().put("v", 1).put("peer", peer).put("readAt", readAt).put("activity", activity)
        .put("usb", usbPower).put("battery", battery?.let { JSONObject().put("percent", it.percent).put("at", it.readAt) })
        .put("storage", storage?.let { JSONObject().put("used", it.used).put("total", it.total)
            .put("entriesUsed", it.entriesUsed).put("entriesTotal", it.entriesTotal).put("at", it.readAt) }).toString()
    companion object {
        private val activities = setOf("Recording", "Saving recording", "Busy", "Needs attention", "Storage full", "Idle", "State unavailable")
        private fun peerValid(peer: String) = Regex("(?:[0-9A-F]{2}:){5}[0-9A-F]{2}").matches(peer)
        fun decode(text: String): PendantHeaderSnapshot? = try {
            require(text.length <= 2048)
            val j = JSONObject(text); require(j.getInt("v") == 1)
            val peer = j.getString("peer"); val at = j.getLong("readAt"); val activity = j.getString("activity")
            require(peerValid(peer) && at > 0 && activity in activities)
            val battery = j.optJSONObject("battery")?.let {
                HeaderBattery(it.getInt("percent"), it.getLong("at")).also { b -> require(b.percent in 0..100 && b.readAt in 1..at) }
            }
            val storage = j.optJSONObject("storage")?.let {
                HeaderStorage(it.getLong("used"), it.getLong("total"), it.getInt("entriesUsed"), it.getInt("entriesTotal"), it.getLong("at"))
                    .also { s -> require(s.total in 1..536870912L && s.used in 0..s.total && s.readAt in 1..at &&
                        s.entriesTotal in 1..32 && s.entriesUsed in 0..s.entriesTotal) }
            }
            PendantHeaderSnapshot(peer, at, activity, if (j.has("usb")) j.getBoolean("usb") else null, battery, storage)
        } catch (_: Exception) { null }

        fun observe(previous: PendantHeaderSnapshot?, peer: String?, timed: TimedDeviceTelemetry?,
            connected: Boolean, now: Long, wall: Long): PendantHeaderSnapshot? {
            if (peer == null || !peerValid(peer) || timed == null || !timed.fresh(now, connected)) return null
            val at = wall - timed.ageMs(now); if (at <= 0) return null
            // On a wall-clock rollback, do not carry a future metric into a new snapshot.
            val old = previous?.takeIf { it.peer == peer && it.readAt <= at }
            val value = timed.value; val recorder = value.recorder
            val battery = timed.freshBattery(now, connected)?.let { b ->
                b.percent?.takeIf { it in 0..100 && at - b.ageMs > 0 }?.let { HeaderBattery(it, at - b.ageMs) }
            } ?: old?.battery
            val storage = PendantStoragePresentation.from(value)?.let {
                HeaderStorage(it.occupiedBytes, it.totalBytes, it.entriesUsed, it.entriesTotal, at)
            } ?: old?.storage
            val activity = when {
                value.faults != 0L || recorder?.fault == true -> "Needs attention"
                recorder?.recording == true && recorder.workerKnown && recorder.state in listOf(4, 7) -> "Saving recording"
                value.microphonePower || recorder?.recording == true -> "Recording"
                value.resourceBusy || recorder?.busy == true -> "Busy"
                recorder?.full == true -> "Storage full"
                recorder?.ready == true -> "Idle"
                else -> "State unavailable"
            }
            return PendantHeaderSnapshot(peer, at, activity, recorder?.usbPowered, battery, storage)
        }
    }
}

internal data class PendantHeaderPresentation(val connection: String, val activity: String,
    val readAt: Long?, val battery: HeaderBattery?, val storage: HeaderStorage?,
    val live: Boolean, val batteryLive: Boolean, val storageLive: Boolean, val usbPower: Boolean?) {
    val batteryText get() = battery?.let { "${it.percent}%" } ?: "—"
    val storageText get() = storage?.let { "${it.percent}% used" } ?: "—"
    val storageSize get() = storage?.let { "${mib(it.used)} / ${mib(it.total)} MiB" } ?: "Not read yet"
    companion object {
        fun from(snapshot: PendantHeaderSnapshot?, connected: Boolean, connecting: Boolean, ready: Boolean,
            timed: TimedDeviceTelemetry?, now: Long, transfer: Boolean): PendantHeaderPresentation {
            val live = connected && ready && timed?.fresh(now, true) == true && !transfer && snapshot != null
            return PendantHeaderPresentation(when { connecting -> "Connecting…"; connected && !ready -> "Checking connection…"
                connected -> "Connected"; else -> "Disconnected" }, snapshot?.activity ?: "State unavailable",
                snapshot?.readAt, snapshot?.battery, snapshot?.storage, live,
                live && snapshot?.battery != null && timed?.freshBattery(now, true) != null,
                live && snapshot?.storage != null && PendantStoragePresentation.from(timed?.value) != null, snapshot?.usbPower)
        }
        private fun mib(bytes: Long) = String.format(Locale.getDefault(), if (bytes % 1048576L == 0L) "%.0f" else "%.1f", bytes / 1048576.0)
    }
}
