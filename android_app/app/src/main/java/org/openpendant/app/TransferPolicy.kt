package org.openpendant.app

import java.util.UUID

data class TransferPreferences(val automatic: Boolean = false, val removeAfterSync: Boolean = false,
    val lowBattery: Boolean = false, val lowBatteryPercent: Int = 25)

/** Foreground-only, opt-in scheduling; no connection/enrollment or power-policy
 * override. One attempt per connection. A manual Stop is not an auto-retry. */
internal class TransferPolicy {
    private var attemptedEpoch: UUID? = null
    private var lastAttempt = Long.MIN_VALUE
    private var lowPendingFor: String? = null
    fun waitingForUsb(bond: String?) = bond != null && lowPendingFor == bond
    fun observeLow(preferences: TransferPreferences, bond: String?, percent: Int?) {
        if (!preferences.lowBattery) lowPendingFor = null
        else if (bond != null && percent != null && percent <= preferences.lowBatteryPercent) lowPendingFor = bond
    }
    fun claim(preferences: TransferPreferences, peer: DurableConnectedPeer?, eligible: Boolean,
        foreground: Boolean, now: Long): Boolean {
        if (peer == null || !foreground || !eligible ||
            !(preferences.automatic || preferences.lowBattery && lowPendingFor == peer.bondAddress) ||
            peer.epoch == attemptedEpoch || lastAttempt != Long.MIN_VALUE && now - lastAttempt < 60_000) return false
        attempted(peer, now)
        return true
    }
    fun attempted(peer: DurableConnectedPeer, now: Long) {
        attemptedEpoch = peer.epoch; lastAttempt = now
        if (lowPendingFor == peer.bondAddress) lowPendingFor = null
    }
    companion object {
        fun queueVerified(before: List<RecordingSyncSnapshot>, after: List<RecordingSyncSnapshot>,
            enabled: () -> Boolean, verifyFile: (SegmentIdentity) -> Boolean,
            queue: (RecordingSyncSnapshot) -> Unit): Int {
            var count = 0
            for (row in after.filter { removable(before, it) }) {
                if (!enabled()) break
                check(requireNotNull(row.manifest).segments.all(verifyFile))
                if (!enabled()) break
                queue(row); count++
            }
            return count
        }
        /** Only copies newly completed by this transfer, not old phone files.
         * Actual on-disk verification is mandatory immediately before intent. */
        fun removable(before: List<RecordingSyncSnapshot>, after: RecordingSyncSnapshot): Boolean =
            RecordingListPresentation.phoneComplete(after) && after.pendingReceipts.isEmpty() &&
                after.pendantCopy == PendantCopy.PRESENT && !RecordingListPresentation.pending(after) &&
                before.singleOrNull { it.recording == after.recording }?.let(RecordingListPresentation::phoneComplete) != true
    }
}
