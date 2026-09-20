package org.openpendant.app

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.StandardOpenOption.*
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.Executors
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/** Explicit durable-library jobs. Sync has a user-started foreground service;
 * playback and other actions remain foreground-only. A process-wide job gate and
 * playback registry survive Activity recreation. No worker waits under a UI,
 * coordinator or radio monitor. No startup sync, capture, enrollment or ASR.
 * Refresh reads existing public/metadata state and the vault summary only.
 */
internal class AndroidDurableLibrary(context: Context, private val client: PendantClient,
    private val changed: () -> Unit) : AutoCloseable {
    private val context = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val closed = AtomicBoolean()
    private val active = AtomicReference<Job?>()
    internal val transferPreferences = AndroidTransferPreferences(context)
    internal val transferPolicy = TransferPolicy()
    @Volatile var state = DurableLibraryState()
        private set
    val busy: Boolean get() = owner.get()
    @Volatile internal var player: RecordingPlayerControl? = null
        private set
    @Volatile internal var syncEvidence = SyncRunEvidence()
        private set
    @Volatile internal var clearReview: QuickClearReview? = null
        private set
    private var clearPeer: DurableConnectedPeer? = null
    private var clearReviewedAt = 0L
    private class Job {
        val cancelled = AtomicBoolean()
        @Volatile var sync: DurableRecordingSyncSession? = null
        @Volatile var playback: DurablePlaybackLifetime? = null
        @Volatile var interruptPlayback: (() -> Unit)? = null
        val cleanupUnconfirmed = AtomicBoolean()
        fun check() { if (cancelled.get()) throw CancellationException("Library operation cancelled") }
    }
    private companion object {
        val worker = Executors.newSingleThreadExecutor()
        val cleanup = Executors.newSingleThreadExecutor()
        val owner = AtomicBoolean()
        val playbackRegistry = RecordingPlaybackRegistry()
    }
    private fun publish(value: DurableLibraryState) {
        state = value
        main.post { if (!closed.get()) changed() }
    }
    private fun summary(message: String): DurableLibraryState {
        val binding = AndroidDurableBinding.read(context)
        val vault = AndroidDurableBinding.vault(context).summary()
        val rows = if (binding == null) emptyList() else AndroidRecordingSyncStore.openExisting(context)?.use {
            it.snapshots(binding.volume.deviceId)
        } ?: emptyList()
        val times = try { AndroidRecordingListTimes(context).read(rows) } catch (_: Exception) { emptyMap() }
        return DurableLibraryState(binding, vault, rows, message = message, firstSyncedTimes = times)
    }
    private fun launch(kind: DurableLibraryWork, action: (Job) -> String) {
        check(Looper.myLooper() === main.looper)
        if (closed.get() || active.get() != null) return
        if (!owner.compareAndSet(false, true)) {
            publish(state.copy(message = "A previous storage job is still stopping. Check again shortly.")); return
        }
        val job = Job(); active.set(job)
        publish(state.copy(work = kind, message = when (kind) {
            DurableLibraryWork.REFRESH -> "Checking existing storage metadata…"
            DurableLibraryWork.SYNC -> "Checking enrolled storage and saved progress…"
            DurableLibraryWork.PLAY -> "Authenticating encrypted recording for RAM-only playback…"
            else -> "Saving the exact deletion intent…"
        }))
        worker.execute {
            var message: String
            try { job.check(); message = action(job) }
            catch (_: CancellationException) { message = "Stopped. Saved sync checkpoints and pending deletion intents are retained." }
            catch (failure: Throwable) {
                if (kind == DurableLibraryWork.SYNC) {
                    // Public code location only. Never log an exception message,
                    // wire payload, recording identifier or cryptographic data.
                    var root = failure
                    repeat(8) { root.cause?.takeIf { it !== root }?.let { root = it } }
                    val site = root.stackTrace.firstOrNull { it.className.startsWith("org.openpendant.app.") }
                    syncEvidence = syncEvidence.copy(failureClass=root.javaClass.simpleName.take(80),
                        failureSite=site?.let { "${it.fileName}:${it.lineNumber}" } ?: "")
                }
                message = "Operation could not be confirmed. Saved progress and pending deletion intents were retained."
            }
            finally { job.sync = null; job.playback = null }
            val refreshed = try { summary(message) }
                catch (_: Throwable) { state.copy(work = DurableLibraryWork.NONE, message = "Storage metadata or recovery protection needs attention. Nothing was reset.", needsAttention = true) }
            active.compareAndSet(job, null)
            if (kind == DurableLibraryWork.PLAY) player = null
            if (job.cleanupUnconfirmed.get()) {
                // A timed-out late cleanup cannot release a later job's owner.
                publish(refreshed.copy(needsAttention = true, message = "Audio cleanup could not be confirmed. Storage actions remain fenced; no recording was changed."))
            } else { owner.set(false); publish(refreshed) }
        }
    }
    fun refresh() = launch(DurableLibraryWork.REFRESH) { "Storage metadata checked." }
    fun syncRefusal(peer:DurableConnectedPeer?):String? = state.syncRefusal(peer) ?: when {
        client.preferencesSave.busy -> "Wait for pendant settings to finish saving."
        peer==null || !client.isCurrentRadioEpoch(peer.epoch) -> "Reconnect to the pendant before syncing."
        else -> StorageSyncPower.refusal(client.telemetry,client.connected,android.os.SystemClock.elapsedRealtime(),client.longPeer()?.capabilityBits ?: 0)
    }
    val supportsBatterySync:Boolean get()=StorageSyncPower.supported(client.longPeer()?.capabilityBits ?: 0)
    fun sync(peer: DurableConnectedPeer) = storageSession(peer, DurableSyncMode.NORMAL)
    fun reviewQuickClear(peer: DurableConnectedPeer) = storageSession(peer, DurableSyncMode.INVENTORY_ONLY)
    fun finishPendingClear(peer: DurableConnectedPeer) = storageSession(peer, DurableSyncMode.DELETIONS_ONLY)
    fun quickClear(review: QuickClearReview, includePhone: Boolean) {
        if(clearReview !== review) return
        if(android.os.SystemClock.elapsedRealtime()-clearReviewedAt !in 0..900_000L) {
            clearReview=null;publish(state.copy(message="Cleanup review expired. Check recordings again before clearing."));return
        }
        val peer=client.durablePeer() ?: clearPeer ?: return
        val plan=review.plan(includePhone)
        if(plan.targets.isEmpty()) return
        storageSession(peer,DurableSyncMode.DELETIONS_ONLY,plan,!client.isCurrentRadioEpoch(peer.epoch))
    }
    private fun storageSession(peer: DurableConnectedPeer, requestedMode: DurableSyncMode,
        clearPlan: BulkRecordingDeletion? = null, continueConnection: Boolean = false) {
        if (state.syncRefusal(peer) != null || busy || closed.get() || client.preferencesSave.busy) return
        // A pending-clear continuation performs its own fresh power/idle readback
        // after reconnect. All newly started transfers check power before work.
        if(!continueConnection)syncRefusal(peer)?.let { refusal ->
            publish(state.copy(message=refusal));return
        }
        clearReview=null
        transferPolicy.attempted(peer, android.os.SystemClock.elapsedRealtime())
        val removeAfterSync = requestedMode==DurableSyncMode.NORMAL && transferPreferences.read().removeAfterSync
        try { StorageSyncService.start(context, this) }
        catch (_: Exception) {
            publish(state.copy(message = "Background transfer could not start. Keep the app open and try Sync again.")); return
        }
        val selectedBinding = state.binding!!
        launch(DurableLibraryWork.SYNC) { job ->
            val syncStarted = android.os.SystemClock.elapsedRealtime()
            syncEvidence = SyncRunEvidence(startedMillis=syncStarted)
            val binding = checkNotNull(AndroidDurableBinding.read(context))
            check(binding == selectedBinding)
            binding.requireVerifiedRecipient(AndroidDurableBinding.vault(context).summary())
            var currentPeer = peer
            var connection = binding.connection(peer); job.check()
            val reconnect = SyncReconnectBudget(android.os.SystemClock.elapsedRealtime()) { android.os.SystemClock.elapsedRealtime() }
            val jobDeadline = android.os.SystemClock.elapsedRealtime() + SyncReconnectBudget.MAX_JOB_MILLIS
            fun reconnectPeer(afterLoss: Boolean) {
                val next = AtomicReference<DurableConnectedPeer?>()
                val done = CountDownLatch(1)
                val prior = currentPeer
                val abandoned = AtomicBoolean()
                check(main.post {
                    val cancelled = { job.cancelled.get() || closed.get() || abandoned.get() }
                    val complete: (DurableConnectedPeer?) -> Unit = { next.set(it); done.countDown() }
                    if (afterLoss) client.resumeDurableAfterLinkLoss(prior, cancelled, complete)
                    else client.continueDurableBatch(prior, cancelled, complete)
                })
                try {
                    check(done.await(46, TimeUnit.SECONDS)); job.check()
                    currentPeer = checkNotNull(next.get())
                    check(currentPeer.epoch != connection.epoch)
                    connection = binding.connection(currentPeer)
                    syncEvidence = syncEvidence.copy(connections=syncEvidence.connections+1,
                        reconnects=syncEvidence.reconnects+if(afterLoss)1 else 0)
                } finally { abandoned.set(true) }
            }
            if(continueConnection) reconnectPeer(false)
            check(client.isCurrentRadioEpoch(connection.epoch))
            withStorage(create = true) { metadata, files ->
                if(requestedMode==DurableSyncMode.DELETIONS_ONLY && clearPlan==null) {
                    // Explicit recovery keeps each previously persisted scope;
                    // it does not turn pendant-only requests into phone removal.
                    for(row in metadata.snapshots(binding.volume.deviceId).filter {
                        it.recording.volume==binding.volume && it.deletions.any { d->d.phonePending }
                    }) {
                        job.check();check(AndroidDurableBinding.read(context)==binding)
                        val pending=row.deletions.single { it.phonePending || it.pendant==PendantDeletion.PENDING }
                        deleteOne(metadata,files,binding,row,pending.location,pending.keepTranscript)
                    }
                }
                if(clearPlan!=null) {
                    clearPlan.validate(metadata.snapshots(binding.volume.deviceId),binding.volume)
                    var processed=0
                    for(target in clearPlan.targets) {
                        job.check();check(AndroidDurableBinding.read(context)==binding)
                        val row=metadata.snapshots(binding.volume.deviceId).single { it.recording==target.recording }
                        clearPlan.validate(target,row)
                        deleteOne(metadata,files,binding,row,target.location,false)
                        processed++
                        publish(state.copy(recordings=metadata.snapshots(binding.volume.deviceId),
                            message="Clearing recordings · $processed of ${clearPlan.targets.size} requests saved"))
                    }
                }
                val beforeSync = metadata.snapshots(binding.volume.deviceId)
                var received = 0L
                var published = 0; var receipts = 0; var deleted = 0
                var result = DurableSyncResult(0,0,0,0,0,0,0,morePending=true)
                var batches = 0
                var removalChecked = false
                var mode=requestedMode
                var verifyingClear=false
                do {
                job.check(); check(android.os.SystemClock.elapsedRealtime() < jobDeadline)
                check(++batches <= DurableManifestCodec.FULL_MAX_SEGMENTS + SyncReconnectBudget.MAX_ATTEMPTS + 1)
                check(AndroidDurableBinding.read(context) == binding)
                binding.requireVerifiedRecipient(AndroidDurableBinding.vault(context).summary())
                val retained = RetainedDurableDeletion.fromSnapshots(connection, metadata.snapshots(binding.volume.deviceId))
                val actual = DurableBleRecordingTransport(connection, PendantDurableBleExchange(client), retained)
                val progress = DurableProgressThrottle()
                val meter = SyncProgressTracker { android.os.SystemClock.elapsedRealtime() }
                fun display(force: Boolean = false) {
                    if (force || progress.due()) meter.snapshot()?.let {
                        publish(state.copy(transfer = it, message = it.text))
                    }
                }
                val observer = object : DurableSyncObserver {
                    override fun recording(row: RecordingSyncSnapshot, position: Int, total: Int) {
                        meter.recording(row, position, total)
                        publish(state.copy(recordings = state.recordings.filterNot { it.recording == row.recording } + row))
                        display(true)
                    }
                    override fun checkpoint(segment: SegmentIdentity, offset: Long) { meter.checkpoint(segment, offset); display() }
                    override fun published(row: RecordingSyncSnapshot) {
                        meter.published(row)
                        publish(state.copy(recordings = state.recordings.filterNot { it.recording == row.recording } + row))
                        display(true)
                    }
                }
                val transport = object : DurableRecordingTransport by actual {
                    private var firstRead = true
                    private var lastSegment: SegmentIdentity? = null
                    override fun catalog(offset: Int, maximumEntries: Int, snapshotRevision: Long?, call: DurableSyncCall): DurableCatalogPage {
                        val began = android.os.SystemClock.elapsedRealtime()
                        try { return actual.catalog(offset, maximumEntries, snapshotRevision, call) }
                        finally { syncEvidence=syncEvidence.copy(metadataMillis=syncEvidence.metadataMillis+android.os.SystemClock.elapsedRealtime()-began) }
                    }
                    override fun manifest(entry: DurableCatalogEntry, call: DurableSyncCall): DurableManifestReply {
                        val began = android.os.SystemClock.elapsedRealtime()
                        try { return actual.manifest(entry, call) }
                        finally { syncEvidence=syncEvidence.copy(metadataMillis=syncEvidence.metadataMillis+android.os.SystemClock.elapsedRealtime()-began) }
                    }
                    override fun receipt(segment: SegmentIdentity, call: DurableSyncCall): DurableReceiptReply {
                        val began = android.os.SystemClock.elapsedRealtime()
                        try { return actual.receipt(segment, call) }
                        finally { syncEvidence=syncEvidence.copy(receiptMillis=syncEvidence.receiptMillis+android.os.SystemClock.elapsedRealtime()-began) }
                    }
                    override fun receiptRange(manifest: RecordingManifest, first: Int, count: Int,
                        call: DurableSyncCall): DurableReceiptRangeReply {
                        val began = android.os.SystemClock.elapsedRealtime()
                        try { return actual.receiptRange(manifest, first, count, call) }
                        finally { syncEvidence=syncEvidence.copy(receiptMillis=syncEvidence.receiptMillis+android.os.SystemClock.elapsedRealtime()-began,
                            receiptBatches=syncEvidence.receiptBatches+1, batchedSegments=syncEvidence.batchedSegments+count) }
                    }
                    override fun read(segment: SegmentIdentity, offset: Long, maximumBytes: Int, call: DurableSyncCall): DurableRangeReply {
                        val began = android.os.SystemClock.elapsedRealtime()
                        val firstRange = lastSegment != segment
                        lastSegment = segment
                        val reply = try { actual.read(segment, offset, maximumBytes, call) }
                        finally {
                            val elapsed = android.os.SystemClock.elapsedRealtime()-began
                            syncEvidence = if(firstRange) syncEvidence.copy(firstRangeMillis=syncEvidence.firstRangeMillis+elapsed)
                                else syncEvidence.copy(followingRangeMillis=syncEvidence.followingRangeMillis+elapsed)
                        }
                        received += reply.chunk.bytes.size
                        syncEvidence = syncEvidence.copy(receivedBytes=received,
                            resumedOffsetBytes=syncEvidence.resumedOffsetBytes+if(firstRead && offset>0)offset else 0)
                        firstRead = false
                        meter.received(reply.chunk.bytes.size)
                        return reply
                    }
                }
                val session = DurableRecordingSyncSession(connection, transport, metadata, files, observer = observer, mode=mode)
                job.sync = session; if (job.cancelled.get()) session.cancel()
                try { result = session.run(); job.check() }
                catch (failure: DurableSyncException) {
                    // Cleanup/inventory never automatically retries uncertain
                    // exchanges. Durable intents remain for explicit recovery.
                    if (requestedMode!=DurableSyncMode.NORMAL || !reconnect.admit(client.linkWasLost(connection.epoch), job.cancelled.get())) throw failure
                    publish(state.copy(transfer=null, message="Bluetooth interrupted · reconnecting (${reconnect.count}/3). Saved parts and checkpoints are retained."))
                    reconnectPeer(true)
                    continue
                }
                published += result.segmentsPublished; receipts += result.receiptsConfirmed; deleted += result.tombstonesConfirmed
                if(!result.morePending && mode==DurableSyncMode.DELETIONS_ONLY) {
                    check(result.pendingPhoneDeletion==0 && result.unsupportedRemoteDeletion==0)
                    check(metadata.snapshots(binding.volume.deviceId).none { it.recording.volume==binding.volume && RecordingListPresentation.pending(it) })
                    publish(state.copy(message="Removals confirmed · checking the pendant again…",transfer=null))
                    reconnectPeer(false);mode=DurableSyncMode.INVENTORY_ONLY;verifyingClear=true
                    result=result.copy(morePending=true);continue
                }
                if (!result.morePending && !removalChecked) {
                    removalChecked = true
                    if (removeAfterSync && transferPreferences.read().removeAfterSync) {
                        val queued = TransferPolicy.queueVerified(beforeSync, metadata.snapshots(binding.volume.deviceId), {
                            job.check(); transferPreferences.read().removeAfterSync
                        }, {
                            job.check(); files.verifiedOnDisk(expectation(it, binding))
                        }) { row ->
                            check(AndroidDurableBinding.read(context) == binding)
                            binding.requireVerifiedRecipient(AndroidDurableBinding.vault(context).summary())
                            val manifest = checkNotNull(row.manifest)
                            // Rehash every encrypted file now; a metadata flag
                            // alone never authorizes removal of the source.
                            val core = metadata.coordinator(row.recording) { files.verifiedOnDisk(expectation(it, binding)) }
                            try {
                                check(core.snapshot().manifest == manifest)
                                core.requestDeletion(UUID.randomUUID(), DeleteLocation.PENDANT_ONLY, true)
                            } finally { core.close() }
                        }
                        if (queued > 0) {
                            publish(state.copy(message="Phone copies verified · confirming pendant removal…", transfer=null))
                            reconnectPeer(false)
                            result = result.copy(morePending=true)
                            continue
                        }
                    }
                }
                if (result.morePending) {
                    // No looping on a non-progressing or failed batch. Pending
                    // state remains durable for a later explicit user action.
                    check(result.segmentsPublished + result.receiptsConfirmed + result.tombstonesConfirmed + result.manifestsObserved > 0)
                    reconnectPeer(false)
                }
                } while (result.morePending)
                val afterSync = metadata.snapshots(binding.volume.deviceId)
                val beforeParts = beforeSync.flatMap { it.phoneSegments }.toSet()
                val newlySaved = afterSync.flatMap { it.phoneSegments }.count { it !in beforeParts }
                syncEvidence = syncEvidence.copy(completed=true,elapsedMillis=android.os.SystemClock.elapsedRealtime()-syncStarted,
                    newSegments=newlySaved)
                // Optional presentation only; date failure must not turn a
                // successful durable transfer into a retry or metadata reset.
                try { AndroidRecordingListTimes(context).rememberSync(beforeSync, metadata.snapshots(binding.volume.deviceId)) }
                catch (_: Exception) { /* Date unavailable; recording remains authoritative. */ }
                if(requestedMode==DurableSyncMode.INVENTORY_ONLY) {
                    val reviewed=QuickClearReview.create(binding.volume,result.catalogEntries,afterSync)
                    clearPeer=currentPeer;clearReviewedAt=android.os.SystemClock.elapsedRealtime();clearReview=reviewed
                    "Storage checked · ${reviewed.pendantCount} pendant recordings · ${reviewed.phoneCount} saved phone copies. Review Clear recordings in Settings. No audio downloaded or removed."
                } else if(verifyingClear) {
                    if(result.catalogEntries.isEmpty()) "Pendant recordings cleared and empty storage confirmed. Pairing and recording keys kept. Existing storage is reused; this is not a secure erase."
                    else "Selected removals confirmed, but ${result.catalogEntries.size} recordings remain on the pendant. Nothing outside the reviewed selection was deleted; check storage again."
                } else "Sync complete · $newlySaved new parts saved · ${SyncTransferProgress.formatBytes(received)} received · $deleted pendant deletions confirmed." +
                    if (result.pendingPhoneDeletion > 0) " ${result.pendingPhoneDeletion} phone deletion(s) need Resume deletion." else ""
            }
        }
    }
    fun play(recording: DurableRecordingId, policy: RecordingGapPolicy = RecordingGapPolicy.PAUSE_AT_GAP,
        positionMillis: Long = 0) {
        val row = state.recordings.singleOrNull { it.recording == recording } ?: return
        if (!state.playable(row)) return
        val selectedManifest = checkNotNull(row.manifest)
        val duration = RecordingListPresentation.audioMillis(row) ?: return
        if (duration <= 0 || busy || closed.get()) return
        val control = RecordingPlayerControl(recording, duration)
        control.move(if (positionMillis >= duration) 0 else positionMillis.coerceAtLeast(0), true)
        player = control
        launch(DurableLibraryWork.PLAY) { job ->
            val binding = checkNotNull(AndroidDurableBinding.read(context)); check(recording.volume == binding.volume)
            val vault = AndroidDurableBinding.vault(context); binding.requireVerifiedRecipient(vault.summary()); job.check()
            withStorage(create = false) { metadata, files ->
                var outcome = "Playback stopped."
                while (true) {
                job.check()
                val cursor = control.snapshot()
                if (!cursor.playing) { Thread.sleep(25); continue }
                if (cursor.positionMillis >= duration) { outcome = "Playback complete."; break }
                val point = RecordingTimeline.seek(row, cursor.positionMillis)
                val core = metadata.coordinator(recording) { files.verifiedOnDisk(expectation(it, binding)) }
                try {
                    check(core.snapshot().manifest == selectedManifest)
                    val session = AndroidRecordingPlayback.prepare(core, selectedManifest, files, vault, playbackRegistry, policy)
                    val lifetime = DurablePlaybackLifetime(session::cancel, { cleanup.execute(it) })
                    job.playback = lifetime
                    job.interruptPlayback = session::interruptForSeek
                    try {
                        val manager = checkNotNull(context.getSystemService(AudioManager::class.java))
                        val focus = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                            .setAcceptsDelayedFocusGain(false).setWillPauseWhenDucked(true)
                            .setOnAudioFocusChangeListener({ value ->
                                if (value != AudioManager.AUDIOFOCUS_GAIN && job.playback === lifetime) cancel(job)
                            }, main).build()
                        job.check()
                        try {
                            check(manager.requestAudioFocus(focus) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED)
                            job.check()
                            if (!control.current(cursor.version)) throw CancellationException()
                            val result = session.run(point.sequence, point.offsetSamples) { samples ->
                                if (control.progress(cursor.version, samples / 16)) main.post { if (!closed.get()) changed() }
                            }
                            if (!control.current(cursor.version)) continue
                            outcome = when (result.end) {
                                RecordingPlaybackEnd.PAUSED_AT_GAP -> "Playback paused at a recorded gap. No missing audio was silently joined."
                                RecordingPlaybackEnd.COMPLETED -> "Playback complete. Decrypted audio was held only in memory."
                            }
                            break
                        } finally { manager.abandonAudioFocusRequest(focus) }
                    } catch (interrupted: Exception) {
                        // A pause/seek cancels only this decoder. Replacement
                        // output waits for the same fenced cleanup as Stop.
                        if (job.cancelled.get() || control.current(cursor.version)) throw interrupted
                    } finally {
                        if (!lifetime.finish()) {
                            job.cleanupUnconfirmed.set(true); playbackRegistry.fence()
                            throw RecordingPlaybackException()
                        }
                        job.playback = null
                        job.interruptPlayback = null
                    }
                } finally { core.close() }
                }
                outcome
            }
        }
    }
    fun pausePlayback() = changePlayback(playing = false)
    fun resumePlayback() = changePlayback(playing = true)
    fun seekPlayback(positionMillis: Long) = changePlayback(position = positionMillis)
    private fun changePlayback(position: Long? = null, playing: Boolean? = null) {
        check(Looper.myLooper() === main.looper)
        val control = player ?: return
        val job = active.get() ?: return
        if (state.work != DurableLibraryWork.PLAY || job.cancelled.get() || closed.get()) return
        val old = control.snapshot()
        control.move(position ?: old.positionMillis, playing ?: old.playing)
        job.interruptPlayback?.invoke()
        changed()
    }
    /** Remote deletion is an explicit durable outbox intent. Only a later user
     * Sync sends it; this action never silently syncs/downloads other records. */
    fun delete(recording: DurableRecordingId, location: DeleteLocation, keepTranscript: Boolean) {
        val row = state.recordings.singleOrNull { it.recording == recording } ?: return
        if (state.busy || state.needsAttention || row.manifest?.finished != true) return
        val selectedBinding = state.binding ?: return
        if (!RecordingDeletionScope.allowed(row, selectedBinding.volume, location)) return
        val manifest = row.manifest
        launch(DurableLibraryWork.DELETE) { job ->
            val binding = checkNotNull(AndroidDurableBinding.read(context)); check(binding == selectedBinding)
            withStorage(create = false) { metadata, files ->
                val current = metadata.snapshots(recording.volume.deviceId).single { it.recording == recording }
                check(current.manifest == manifest); job.check()
                val intent = deleteOne(metadata, files, binding, current, location, keepTranscript)
                if (intent.pendant == PendantDeletion.PENDING)
                    "Deletion intent saved. Tap Sync pendant storage to send and verify pendant removal."
                else "Phone copy removed. Automatic re-download stays suppressed; other recordings are unchanged."
            }
        }
    }
    fun deleteFiltered(plan: BulkRecordingDeletion) {
        if (busy || state.busy || state.needsAttention || closed.get() || plan.targets.isEmpty()) return
        val selectedBinding = state.binding ?: return
        if (selectedBinding.volume != plan.volume) return
        launch(DurableLibraryWork.DELETE) { job ->
            val binding = checkNotNull(AndroidDurableBinding.read(context)); check(binding == selectedBinding)
            withStorage(create = false) { metadata, files ->
                plan.validate(metadata.snapshots(binding.volume.deviceId), binding.volume)
                var completed = 0; var queued = 0
                try {
                    for (target in plan.targets) {
                        job.check(); check(AndroidDurableBinding.read(context) == binding)
                        val current = metadata.snapshots(binding.volume.deviceId).single { it.recording == target.recording }
                        plan.validate(target, current)
                        val intent = deleteOne(metadata, files, binding, current, target.location, false)
                        completed++
                        if (intent.pendant == PendantDeletion.PENDING) queued++
                        publish(state.copy(recordings=metadata.snapshots(binding.volume.deviceId),
                            message="Bulk deletion · $completed of ${plan.targets.size} processed · $queued pendant removals queued"))
                    }
                    "Bulk deletion · $completed recordings processed." +
                        if (queued > 0) " Sync to confirm $queued pendant removals. Rows disappear when neither copy remains." else " Fully deleted recordings are no longer listed."
                } catch (_: CancellationException) {
                    "Bulk deletion stopped · $completed of ${plan.targets.size} processed. Saved requests are retained; $queued pendant removals await sync."
                } catch (_: Exception) {
                    "Bulk deletion needs review · $completed of ${plan.targets.size} processed before an unconfirmed operation. Earlier removals are not undone; pending requests stay visible."
                }
            }
        }
    }
    /** Shared single/bulk deletion path. Once an intent is persisted, finish the
     * current local cleanup before honoring a batch stop. Never widen its scope. */
    private fun deleteOne(metadata: AndroidRecordingSyncStore, files: DurableSegmentStore,
        binding: DurablePublicBinding, current: RecordingSyncSnapshot, location: DeleteLocation,
        keepTranscript: Boolean): RecordingDeletionIntent {
        check(RecordingDeletionScope.allowed(current, binding.volume, location))
        val recording = current.recording
        val manifest = checkNotNull(current.manifest)
        val pending = current.deletions.singleOrNull { it.phonePending || it.pendant == PendantDeletion.PENDING }
        val intent = if (pending != null) {
            check(pending.location == location && pending.keepTranscript == keepTranscript); pending
        } else {
            val core = metadata.coordinator(recording) { files.verifiedOnDisk(expectation(it, binding)) }
            try { check(core.snapshot().manifest == manifest); core.requestDeletion(UUID.randomUUID(), location, keepTranscript) }
            finally { core.close() }
        }
        if (intent.phonePending) {
            val derivatives = RecordingDerivativeDeletion(playbackRegistry, RamOnlyRecordingDerivatives(
                context.noBackupFilesDir.canonicalFile.toPath(), SegmentDirectorySync(AndroidDurableSegments::syncDirectory)))
            val removal = AndroidPhoneRecordingDeletion.create(context, derivatives)
            metadata.openPendingPhoneDeletion(recording, intent.operationId, manifest.sha256).use { check(it.complete(removal)) }
        }
        return intent
    }
    fun cancel() {
        val job = active.get() ?: return
        cancel(job)
    }
    internal fun cancelSync() { if (state.work == DurableLibraryWork.SYNC) cancel() }
    private fun cancel(job: Job) {
        if (active.get() !== job) return // A queued old focus callback cannot cancel a newer job.
        job.cancelled.set(true); job.sync?.cancel()
        // Platform disposal is off-main. The worker's lifetime guard tracks the
        // task and never releases the process owner while cleanup is uncertain.
        job.playback?.cancelAsync()
    }
    override fun close() { closed.set(true); cancel() }

    private fun expectation(segment: SegmentIdentity, binding: DurablePublicBinding) =
        EncryptedSegmentExpectation(segment, hexBytes(binding.recipientFingerprint), (segment.byteCount - 209).toInt())
    private fun <T> withStorage(create: Boolean, action: (AndroidRecordingSyncStore, DurableSegmentStore) -> T): T {
        val existing = AndroidRecordingSyncStore.openExisting(context)
        if (existing != null) return existing.use {
            action(it, checkNotNull(AndroidDurableSegments.openExisting(context))) // Lost root is never silently replaced.
        }
        check(create)
        val root = context.noBackupFilesDir.canonicalFile.toPath().resolve("encrypted-segments-v1")
        if (AndroidDurableBinding.attributes(root) != null) {
            Files.newDirectoryStream(root).use { entries ->
                var count = 0
                for (entry in entries) check(++count <= 1 && entry.fileName.toString() == ".store.lock" &&
                    AndroidDurableBinding.attributes(entry)?.isRegularFile == true && Files.size(entry) == 0L)
            }
        }
        val files = AndroidDurableSegments.create(context)
        FileChannel.open(root.resolve(".store.lock"), CREATE, WRITE, NOFOLLOW_LINKS).use { it.force(true) }
        AndroidDurableSegments.syncDirectory(root)
        return AndroidRecordingSyncStore.open(context).use { action(it, files) }
    }
}
