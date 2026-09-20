package org.openpendant.app

/** Shared by the visible Start state and the final pre-send check. Never
 * weakens firmware admission or turns a missing gauge sample into USB power. */
internal object RecordingPowerStatus {
    fun blocked(timed: TimedDeviceTelemetry?, now: Long, connected: Boolean): String? {
        val battery = timed?.value?.battery ?: return null // Legacy USB-only firmware.
        if (!battery.portable) return null
        if (!timed.fresh(now, connected)) return "Refresh battery status before recording"
        if (battery.stopped) return "Battery monitor needs attention"
        if (!battery.valid) return if (timed.value.recorder?.usbPowered == true)
            "Checking battery setup…" else "Battery setup needed · connect USB"
        if (timed.freshBattery(now, connected) == null) return "Refresh battery status before recording"
        if (!battery.startPowerReady) return "Battery is not ready for recording"
        return null
    }
}
