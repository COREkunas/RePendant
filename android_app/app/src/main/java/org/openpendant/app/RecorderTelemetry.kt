package org.openpendant.app

/** Optional, read-only engineering recorder RAM snapshot. Never start authority. */
data class RecorderTelemetry(val flags: Int, val state: Int, val reason: Int, val mode: Int,
    val rootsUsed: Int, val rootsTotal: Int, val slotsUsed: Int, val slotsTotal: Int,
    val capturedSamples: Long, val committedSamples: Long) {
    val usbPowered get() = flags and 1 != 0
    val ready get() = flags and 2 != 0
    val mounted get() = flags and 4 != 0
    val capacityKnown get() = flags and 16 != 0
    val workerKnown get() = flags and 32 != 0
    val busy get() = flags and 64 != 0
    val recording get() = flags and 128 != 0
    val fault get() = flags and 256 != 0
    val full get() = capacityKnown && (rootsUsed == rootsTotal || slotsUsed == slotsTotal)
    val storageSummary: String get() = when {
        fault -> "Needs attention"
        full -> "Recording storage full"
        capacityKnown -> "${slotsTotal - slotsUsed} audio slots free"
        busy -> "Busy · capacity unavailable"
        !mounted -> "Not checked this boot"
        else -> "Capacity unavailable"
    }
    val activitySummary: String get() = when {
        fault -> "Recorder needs attention"
        recording -> when { workerKnown && state in listOf(4, 7) -> "Finalizing recording"
            workerKnown && state == 2 -> "Starting recording"
            else -> "Recording active" }
        busy -> "Recorder busy"
        workerKnown && state == 5 -> "Recording stopped"
        ready -> "Recorder idle"
        else -> "Recorder unavailable"
    }
    val details: String get() = buildString {
        append(activitySummary)
        if (workerKnown && capturedSamples > 0) append(" · captured ${capturedSamples / 16000}s, saved ${committedSamples / 16000}s")
        if (workerKnown && reason != 0) append("\nStop reason: "+ when(reason) {
            1 -> "requested"; 2 -> "power policy"; 3 -> "audio queue full"; 4 -> "audio continuity"
            5 -> "external stop/error"; 6 -> "operation timed out"; 7 -> "clock error"
            8 -> "recorder error"; 9 -> "stop failed"; 10 -> "start refused"
            11 -> "storage full"; else -> "resource release failed"
        })
        if (capacityKnown) append("\nRecording entries $rootsUsed/$rootsTotal · audio slots $slotsUsed/$slotsTotal used")
        append(if (rootsTotal == 32) "\nFull-chip recording layout. Sync power requirements depend on the negotiated firmware capability."
            else "\nEngineering storage only—not the full memory capacity. USB power is required.")
        if (!usbPowered) append(" USB is not connected.")
    }
    companion object {
        fun parse(bytes: ByteArray): RecorderTelemetry {
            ensure(bytes.size == 72, "Truncated recorder status")
            val flags=OpProtocol.u16(bytes,48)
            ensure(flags and 1023 == flags && flags and 512 != 0, "Unknown recorder status flags")
            ensure(bytes[55].toInt()==0 && (68..71).all { bytes[it].toInt()==0 }, "Reserved recorder fields")
            fun byte(at:Int)=bytes[at].toInt() and 255
            val result=RecorderTelemetry(flags,byte(50),byte(51),byte(52),byte(53),byte(54),
                OpProtocol.u16(bytes,56),OpProtocol.u16(bytes,58),OpProtocol.u32(bytes,60),OpProtocol.u32(bytes,64))
            if(result.workerKnown) ensure(result.state in 0..7 && result.reason in 0..12 && result.mode in 0..1 &&
                result.capturedSamples%320==0L && result.committedSamples%320==0L && result.committedSamples<=result.capturedSamples,
                "Invalid recorder progress")
            else ensure(result.state==0 && result.reason==0 && result.mode==0 && result.capturedSamples==0L && result.committedSamples==0L,
                "Unavailable recorder progress is not canonical")
            val firmware="${OpProtocol.u16(bytes,12)}.${OpProtocol.u16(bytes,14)}.${OpProtocol.u16(bytes,16)}"
            val fullLayout = PendantStoragePresentation.fullLayoutFirmware(firmware) &&
                result.rootsTotal==32 && result.slotsTotal==5120
            if(result.capacityKnown) ensure(flags and (4+8+64+128+256)==4+8 && (result.rootsTotal in 1..8 || fullLayout) &&
                result.rootsUsed<=result.rootsTotal && result.slotsTotal>0 && result.slotsUsed<=result.slotsTotal,
                "Invalid recorder capacity")
            else ensure(result.rootsUsed==0 && result.rootsTotal==0 && result.slotsUsed==0 && result.slotsTotal==0,
                "Unavailable recorder capacity is not canonical")
            return result
        }
    }
}
