package org.openpendant.app

import java.util.UUID

/** Cable-sourced public authority, not a BLE catalog and never private material.
 * Firmware62 also reports inactive key-reset successors; binding() still
 * requires ACTIVE while storage() exposes phase for the explicit reset wizard.
 * This reader cannot provision, rotate keys, erase storage or start capture. */
internal object PhoneMigrationProtocol {
    data class Storage(val binding: DurablePublicBinding, val phase: Int, val confirmation: String, val publicPoint: String)
    fun binding(address: String, reply: String): DurablePublicBinding = storage(address, reply).also { require(it.phase == 3) }.binding
    fun storage(address: String, reply: String): Storage {
        require(reply.length <= 4096 && '\u0000' !in reply)
        val lines = reply.lineSequence().map { it.trimEnd('\r') }
            .filter { it.startsWith("RECORDER_") }.toList()
        require(lines.size == 5)
        fun field(index: Int, pattern: String) = checkNotNull(Regex(pattern).matchEntire(lines[index])).groupValues[1]
        val header = checkNotNull(Regex("RECORDER_FULL_CONFIRM sha=([0-9a-f]{64}) phase=([1-3]) generation=([0-9]{1,19}) blocks=2048 logical_sectors=94208 slots=5120").matchEntire(lines[0]))
        val phase = header.groupValues[2].toInt()
        val generation = header.groupValues[3].toLong().also { require(it >= 2 && it.toString() == header.groupValues[3]) }
        fun uuid(hex: String): UUID = UUID.fromString("${hex.substring(0,8)}-${hex.substring(8,12)}-${hex.substring(12,16)}-${hex.substring(16,20)}-${hex.substring(20)}")
        val device = uuid(field(1, "RECORDER_FULL_DEVICE id=([0-9a-f]{32})"))
        val volume = uuid(field(2, "RECORDER_FULL_VOLUME id=([0-9a-f]{32})"))
        val fingerprint = field(3, "RECORDER_FULL_RECIPIENT fingerprint=([0-9a-f]{64})")
        val pointHex = field(4, "RECORDER_FULL_PUBLIC point=(04[0-9a-f]{128})")
        val point = hexBytes(pointHex)
        try {
            // Validates the P-256 point as well as the full domain-separated hash.
            RecipientRecoveryCodec.importPublicP256(point)
            val actual = RecipientRecoveryCodec.fingerprint(point)
            try { require(actual.contentEquals(hexBytes(fingerprint))) } finally { actual.fill(0) }
        } finally { point.fill(0) }
        return Storage(DurablePublicBinding(address, RecordingVolume(device, volume, generation), fingerprint), phase, header.groupValues[1], pointHex)
    }
}

internal enum class PhoneMigrationChoice(val title: String, val explanation: String) {
    KEEP_KEY("Keep recordings · restore old key",
        "Import the old recovery backup and finish setup on this phone. Keep recordings and any newly created phone key."),
    NEW_KEY_ERASE("New key · delete pendant recordings",
        "Start fresh. This must erase the pendant's existing recordings only after a separate confirmation. Phone copies stay."),
    NEW_KEY_ARCHIVE("New key · archive recordings on phone",
        "First copy and verify every encrypted recording on this phone. Keep the old backup: the new key cannot unlock those recordings.")
}

/** Fail-closed preflight for the future firmware transaction. Choice alone is
 * NEVER authority to delete. Verification concerns ciphertext, not decryption. */
internal object PhoneMigrationPolicy {
    fun rotationRefusal(choice: PhoneMigrationChoice, firmwareSupportsRotation: Boolean,
        newBackupVerified: Boolean, newFingerprint: String?, oldFingerprint: String,
        archiveVerified: Boolean, explicitEraseConfirmed: Boolean): String? = when {
        choice == PhoneMigrationChoice.KEEP_KEY -> "This choice restores the existing key; it must not rotate or erase."
        !firmwareSupportsRotation -> "This firmware does not support a recoverable key change yet. No key was changed and no recording was erased."
        !newBackupVerified || newFingerprint == null || !isContentDigest(newFingerprint) ||
            newFingerprint in setOf(oldFingerprint, "00".repeat(32), "ff".repeat(32)) -> "Save and verify a different new key's backup first."
        choice == PhoneMigrationChoice.NEW_KEY_ARCHIVE && !archiveVerified -> "Every pendant recording must be completely archived and verified before clearing it."
        !explicitEraseConfirmed -> "Confirm removal of the old pendant recordings separately."
        else -> null
    }
    fun archiveComplete(rows: List<RecordingSyncSnapshot>, volume: RecordingVolume): Boolean =
        rows.distinctBy { it.recording }.size == rows.size && rows.all { row ->
            row.recording.volume == volume && !row.staleVolume && !row.downloadSuppressed &&
                row.deletions.isEmpty() && row.manifest?.let { manifest ->
                    manifest.finished && manifest.segments.isNotEmpty() &&
                        row.phoneSegments.containsAll(manifest.segments)
                } == true
        }
}
