package org.openpendant.app

enum class DurableLibraryWork { NONE, REFRESH, SYNC, PLAY, DELETE, EXPORT }
enum class DurableDeletionAction { CHOOSE_LOCATION, RESUME_PHONE, SYNC_PENDANT }

/** A remote-only pending intent must use sync, not repeat local deletion. */
fun RecordingSyncSnapshot.deletionAction(): DurableDeletionAction = when {
    deletions.any { it.phonePending } -> DurableDeletionAction.RESUME_PHONE
    deletions.any { it.pendant == PendantDeletion.PENDING } -> DurableDeletionAction.SYNC_PENDANT
    else -> DurableDeletionAction.CHOOSE_LOCATION
}

data class DurableLibraryState(val binding: DurablePublicBinding? = null,
    val vault: RecipientVaultSummary? = null, val recordings: List<RecordingSyncSnapshot> = emptyList(),
    val work: DurableLibraryWork = DurableLibraryWork.NONE,
    val message: String = "Check recording key and enrolled storage identity.", val needsAttention: Boolean = false,
    val firstSyncedTimes: Map<DurableRecordingId, Long> = emptyMap(),
    val transfer: SyncTransferProgress? = null) {
    val busy: Boolean get() = work != DurableLibraryWork.NONE
    val recipientReady: Boolean get() = binding?.let {
        vault?.state == RecipientVaultState.READY && vault.backupVerified && vault.fingerprintHex == it.recipientFingerprint
    } == true
    fun syncRefusal(peer: DurableConnectedPeer?): String? = when {
        busy -> "A storage operation is still running."
        needsAttention -> "Storage metadata needs attention; no automatic reset is available."
        binding == null -> "Storage identity has not been explicitly enrolled on this phone."
        vault?.state != RecipientVaultState.READY -> "Open Recording key and recovery to prepare the recipient."
        vault.backupVerified != true -> "Read back and verify your recovery backup before syncing."
        !recipientReady -> "The active recording key does not match the enrolled storage identity."
        peer == null -> "Connect securely to the enrolled pendant."
        peer.bondAddress != binding.bondAddress -> "The selected pendant differs from the enrolled device."
        !(peer.capabilities.catalog && peer.capabilities.rangedSegments && peer.capabilities.durableReceipts && peer.capabilities.tombstones) ->
            "This firmware has not enabled the complete durable-storage protocol."
        else -> null
    }
    fun playable(row: RecordingSyncSnapshot): Boolean = !busy && !needsAttention && recipientReady &&
        row.recording.volume == binding?.volume && !row.staleVolume && !row.downloadSuppressed &&
        row.deletions.none { it.phonePending || it.pendant == PendantDeletion.PENDING } &&
        row.manifest?.let { it.finished && it.segments.isNotEmpty() && row.phoneSegments.containsAll(it.segments) } == true
}
