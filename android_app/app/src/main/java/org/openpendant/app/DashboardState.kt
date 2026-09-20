package org.openpendant.app

/** Validated v1 state only; unsupported telemetry is never invented. */
data class DashboardState(val connection: String, val capture: String, val led: String) {
    companion object {
        const val BATTERY = "Not reported by current firmware"
        const val STORAGE = "Not available · NAND storage is not ready"
        const val MODES = "Short clips only · button / continuous modes need new firmware"
        const val TRANSCRIPTION = "Offline Lithuanian · import the trusted model, then tap Transcribe"
        fun from(connected: Boolean, connecting: Boolean, ready: Boolean,
                 recording: Boolean, clipState: Int?, cancelling: Boolean): DashboardState {
            val authenticated = connected && (ready || recording)
            val connection = when {
                authenticated -> "Connected securely"
                connected -> "Verifying secure connection…"
                connecting -> "Connecting…"
                else -> "Disconnected"
            }
            val capture = when {
                !authenticated -> "Unknown · connect to check"
                cancelling -> "Stopping · awaiting confirmation"
                !recording -> "App idle · microphone last checked off"
                clipState == ClipState.WAITING -> "Waiting for your short button tap"
                clipState == ClipState.RECORDING -> "Recording short clip"
                clipState == ClipState.READY -> "Receiving clip · app capture finished"
                else -> "Checking recording state…"
            }
            // v1 implies red during capture, but has no LED-state readback.
            val led = if (authenticated && recording && !cancelling && clipState == ClipState.RECORDING)
                "Red requested during capture · no LED readback"
                else "Not reported · no live color control yet"
            return DashboardState(connection, capture, led)
        }
    }
}
