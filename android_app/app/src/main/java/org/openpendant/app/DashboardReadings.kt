package org.openpendant.app

/** Read-only presentation. Missing or expired measurements never become zero,
 * idle, charging, or free space. No radio/storage action is performed here. */
internal data class DashboardReadings(val battery: String, val led: String, val mode: String,
    val device: String, val storageTitle: String, val storageDetail: String, val entries: String) {
    companion object {
        fun from(timed: TimedDeviceTelemetry?, now: Long, connected: Boolean): DashboardReadings {
            val fresh = timed?.fresh(now, connected) == true
            val value = timed?.value
            val prefix = if (fresh) "" else "Last: "
            val battery = timed?.freshBattery(now, connected)?.let { "${it.percent}%" }
                ?: if (value?.battery?.valid == true) "Refresh needed" else "Not available"
            val led = value?.let { prefix + it.ledSummary.removeSuffix(" commanded") } ?: "Not available"
            val recorder = value?.recorder
            val mode = if (recorder?.workerKnown == true)
                prefix + if (recorder.mode == 0) "Manual · start / stop" else "Continuous"
                else "Not reported"
            val device = if (value == null) "Connect to check battery and device status."
                else buildString {
                    append("Firmware ${value.firmware}")
                    append(if (!fresh) " · last known" else if (recorder?.usbPowered == true) " · USB power"
                        else if (recorder != null) " · Battery power" else "")
                }
            val capacity = PendantStoragePresentation.from(value)
            return DashboardReadings(battery, led, mode, device,
                capacity?.let { prefix + "${it.percentUsed}% used" } ?: "Not checked",
                capacity?.let { "${it.summary}\n${formatMiB(it.totalBytes)} enabled total" }
                    ?: "Connect and refresh to check free space.",
                capacity?.let { "${it.entriesUsed} / ${it.entriesTotal} recording spaces" +
                    if (it.entryLimitReached) " · Full" else "" } ?: "")
        }

        fun recording(connected: Boolean, timed: TimedDeviceTelemetry?, now: Long,
            observed: LongRecordingObservation?, observedAt: Long, shortActive: Boolean): String {
            if (shortActive) return "Short microphone test"
            if (!connected) return "Connect to check recording"
            if (observed?.outcome == LongRecordingOutcome.BOOT_CHANGED) return "Restarted · check recovery"
            if (observed?.outcome == LongRecordingOutcome.UNKNOWN) return "Check recording status"
            val fresh = now - observedAt in 0..10_000 && observed?.outcome == LongRecordingOutcome.OBSERVED
            val state = observed?.state
            if (fresh && state != null) {
                if (state.has(LongRecordingControlCodec.BUTTON_ARMED)) return "Ready for your button tap"
                return when (state.phase) {
                    LongRecordingControlCodec.Phase.IDLE -> "Ready to record"
                    LongRecordingControlCodec.Phase.STARTING -> "Preparing recording…"
                    LongRecordingControlCodec.Phase.RUNNING -> "Recording · ${clock(state.accepted / 50)}"
                    LongRecordingControlCodec.Phase.STOPPING, LongRecordingControlCodec.Phase.DRAINING -> "Saving recording…"
                    LongRecordingControlCodec.Phase.STOPPED -> "Saved on pendant"
                    LongRecordingControlCodec.Phase.NO_CAPACITY -> "Storage full"
                    LongRecordingControlCodec.Phase.CANCELLED_BEFORE_START -> "Recording cancelled"
                    LongRecordingControlCodec.Phase.FAULT -> "Recording needs attention"
                }
            }
            if (timed?.fresh(now, connected) == true) {
                val device = timed.value
                if (device.faults != 0L || device.recorder?.fault == true) return "Device needs attention"
                val recorder = device.recorder
                if (recorder?.recording == true || device.microphonePower) return "Recording on pendant"
                if (recorder?.busy == true || device.resourceBusy) return "Pendant is busy"
                if (recorder?.full == true) return "Storage full"
                if (recorder?.ready == true) return "Ready to record"
            }
            return "Check recording status"
        }

        private fun clock(seconds: Int) = java.lang.String.format(java.util.Locale.ROOT, "%d:%02d", seconds / 60, seconds % 60)
        private fun formatMiB(bytes: Long) = java.lang.String.format(java.util.Locale.ROOT, "%.0f MiB", bytes / 1048576.0)
    }
}
