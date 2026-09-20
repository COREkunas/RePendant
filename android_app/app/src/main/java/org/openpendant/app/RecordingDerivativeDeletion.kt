package org.openpendant.app

/** A single shared instance must cover all playback in the app process. Entries
 * are bound to the full immutable final manifest, never a legacy clip UUID.
 * stopAndDiscard must be bounded/thread-safe and MUST NOT join the playback
 * worker (deletion owns its coordinator lock). The worker wipes its bounded
 * in-flight plaintext when the current synchronous decode returns. AudioTrack
 * queued PCM is discarded immediately; this is not secure RAM erasure. */
class RecordingPlaybackRegistry {
    private class Entry(val manifest: RecordingManifest, val stopAndDiscard: () -> Unit)
    private val lock = Any()
    private val entries = mutableMapOf<DurableRecordingId, Entry>()
    private var cleanupUncertain = false
    internal fun fence() = synchronized(lock) { cleanupUncertain = true }
    internal fun register(manifest: RecordingManifest, stopAndDiscard: () -> Unit): AutoCloseable = synchronized(lock) {
        require(manifest.finished)
        if (cleanupUncertain) throw RecordingPlaybackException()
        // One audio output across the app, not one AudioTrack per recording.
        if (entries.isNotEmpty()) throw RecordingSyncBusyException()
        val entry = Entry(manifest, stopAndDiscard)
        entries[manifest.recording] = entry
        AutoCloseable { synchronized(lock) { if (entries[manifest.recording] === entry) entries.remove(manifest.recording) } }
    }
    internal fun stop(plan: PhoneDeletionPlan) = synchronized(lock) {
        if (cleanupUncertain) throw RecordingPlaybackException()
        val entry = entries[plan.recording] ?: return@synchronized
        require(entry.manifest.sha256 == plan.manifestSha256 && entry.manifest.segments == plan.segments)
        try { entry.stopAndDiscard() } catch (failure: Throwable) { cleanupUncertain = true; throw failure }
        // Keep until worker cleanup. A second job cannot race decode teardown.
    }
}

/** Explicit FULL-identity mapping of disk derivatives. Each method must validate
 * its complete finite binding before unlinking, tolerate only real ENOENT, and
 * fsync affected directories before return. No guessed legacy clip filenames,
 * broad directory cleanup, default no-op, or source/download mutation.
 *
 * RAM-only playback creates no WAV/cache/transcript. This app currently has no
 * durable full-identity disk derivative format, so production integration must
 * supply a reviewed mapping or refuse deletion; legacy files are NOT that map.
 */
interface RecordingDiskDerivatives {
    fun removeAudioAndSync(plan: PhoneDeletionPlan)
    fun removeTranscriptAndSync(plan: PhoneDeletionPlan)
}

/** Always clear transient output and all phone audio derivatives. keepTranscript
 * controls ONLY transcript retention; failure preserves pending deletion and
 * its existing suppression. PENDANT_ONLY never admits a PhoneDeletionPlan. */
class RecordingDerivativeDeletion(private val playback: RecordingPlaybackRegistry,
                                  private val disk: RecordingDiskDerivatives) : PhoneDeletionDerivatives {
    override fun removeAndSync(plan: PhoneDeletionPlan) {
        playback.stop(plan)
        disk.removeAudioAndSync(plan)
        if (!plan.keepTranscript) disk.removeTranscriptAndSync(plan)
    }
}
