package org.openpendant.app

import java.util.Locale

/** A byte estimate is valid only for a firmware layout we actually know.
 * v2 reports occupied fixed-size allocation slots, NOT audio file lengths or
 * bad-block-adjusted raw-chip free space. Never substitute the chip size for it.
 */
data class PendantStoragePresentation(val totalBytes: Long, val occupiedBytes: Long,
    val availableBytes: Long, val entriesUsed: Int, val entriesTotal: Int) {
    val percentUsed: Int get() = ((occupiedBytes * 100L) / totalBytes).toInt()
    val entryLimitReached: Boolean get() = entriesUsed == entriesTotal
    val summary: String get() = "${format(occupiedBytes)} occupied · ${format(availableBytes)} available"
    val detail: String get() = buildString {
        append("${format(totalBytes)} total enabled recording space")
        append("\nRecording entries: $entriesUsed of $entriesTotal occupied")
        if (entryLimitReached) append("\nRecording entry limit reached—even if some byte space remains.")
        append("\nCounts include reserved space in partly filled audio slots; they are not exact audio file sizes.")
        if (entriesTotal == 32) append("\nPhysical NAND: 512 MiB. Space outside the recording allocation is reserved for mapping, metadata and wear management.")
        else append("\nPhysical NAND: 512 MiB. Current firmware uses an 8 MiB test area; the rest is not enabled for recordings.")
    }
    companion object {
        internal val FULL_FIRMWARE = setOf("0.4.29", "0.4.30", "0.4.31", "0.4.32", "0.4.33", "0.4.34", "0.4.35", "0.4.36", "0.4.37", "0.4.38", "0.4.39", "0.4.40", "0.4.41", "0.4.42", "0.4.43", "0.4.44", "0.4.45", "0.4.46", "0.4.49", "0.4.50", "0.4.51", "0.4.52", "0.4.53", "0.4.54", "0.4.55")
        // One allowlist shared with the wire parser, not a display-only exception.
        internal fun fullLayoutFirmware(firmware: String) = firmware in FULL_FIRMWARE || firmware in setOf("0.4.56", "0.4.57", "0.4.58", "0.4.59", "0.4.60")
        // Profile0.4.27: each audio slot reserves17 logical2048-byte payload pages.
        private const val BYTES_PER_SLOT = 17L * 2048L
        fun from(value: DeviceTelemetry?): PendantStoragePresentation? {
            if (value == null || value.firmware != "0.4.27" && !fullLayoutFirmware(value.firmware)) return null
            val r = value.recorder ?: return null
            val layout = value.firmware == "0.4.27" && r.rootsTotal == 8 && r.slotsTotal == 22 ||
                fullLayoutFirmware(value.firmware) && r.rootsTotal == 32 && r.slotsTotal == 5120
            if (!r.capacityKnown || r.fault || !layout) return null
            return PendantStoragePresentation(r.slotsTotal * BYTES_PER_SLOT, r.slotsUsed * BYTES_PER_SLOT,
                (r.slotsTotal - r.slotsUsed) * BYTES_PER_SLOT, r.rootsUsed, r.rootsTotal)
        }
        private fun format(bytes: Long): String = when {
            bytes >= 1024L * 1024L -> String.format(Locale.ROOT, "%.2f MiB", bytes / (1024.0 * 1024.0))
            else -> String.format(Locale.ROOT, "%.0f KiB", bytes / 1024.0)
        }
    }
}
