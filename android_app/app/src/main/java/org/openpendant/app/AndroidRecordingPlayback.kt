package org.openpendant.app

import android.os.Looper

/** Wiring only, dormant until the returned single-use session's explicit run on
 * a worker. Uses existing ciphertext store and active VERIFIED recipient; never
 * creates a key, repairs a slot, enrolls a device or writes plaintext. Caller
 * must share coordinator/registry ownership, obtain audio focus, and cancel on
 * lifecycle/focus loss. The library invokes it only after explicit Play. */
object AndroidRecordingPlayback {
    fun prepare(coordinator: RecordingSyncContract, manifest: RecordingManifest,
                store: DurableSegmentStore, vault: RecipientKeyVault,
                registry: RecordingPlaybackRegistry,
                policy: RecordingGapPolicy = RecordingGapPolicy.PAUSE_AT_GAP): RecordingPlayback =
        RecordingPlayback(coordinator, manifest, PlaybackCiphertextSource {
            checkWorker(); store.readPublished(it)
        }, PlaybackKeyAccess { action ->
            checkWorker(); vault.withActiveKey(action)
        }, NativeOpusPacketDecoder, AndroidRecordingPcmSink(), registry, policy)

    private fun checkWorker() {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Playback must run off the main thread" }
    }
}
