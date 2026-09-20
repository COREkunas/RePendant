package org.openpendant.app

import org.openpendant.app.ModelRepository.Companion.hex
import java.security.MessageDigest
import java.util.concurrent.CancellationException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/** Adapter boundary keeps native model code and Android JSON out of JVM storage/job tests. */
interface TranscriptionEngine {
    val version: String
    fun transcribe(modelPath: String, pcm16kMono: FloatArray, language: String, callbacks: Callbacks): List<TranscriptSegment>
    interface Callbacks {
        fun isCancelled(): Boolean
        fun onProgress(percent: Int)
    }
}

enum class TranscriptionPhase { IDLE, RUNNING, CANCELLING, COMPLETE, CANCELLED, FAILED }
data class TranscriptionState(
    val phase: TranscriptionPhase = TranscriptionPhase.IDLE,
    val recordingId: String? = null,
    val progress: Int = 0,
    val message: String? = null,
) {
    val busy: Boolean get() = phase == TranscriptionPhase.RUNNING || phase == TranscriptionPhase.CANCELLING
}

/**
 * Explicit Tap Transcribe only. One inference at a time; no persisted automatic queue.
 * Root supplies a main-thread dispatcher and must route phone deletion through deletePhone.
 * Closing/activity destruction cooperatively cancels native work, never reports partial success.
 */
class TranscriptionController(
    private val recordings: RecordingRepository,
    private val transcripts: TranscriptionRepository,
    private val models: ModelRepository,
    private val engine: TranscriptionEngine,
    private val dispatch: (() -> Unit) -> Unit,
    private val onState: (TranscriptionState) -> Unit,
    private val executor: ExecutorService = Executors.newSingleThreadExecutor(),
    private val now: () -> Long = System::currentTimeMillis,
) {
    private val lock = Any()
    private var generation = 0L
    private var active: Job? = null
    private var closed = false
    private var state = TranscriptionState()
    private class Job(val generation: Long, val id: String) {
        @Volatile var cancelled = false
        var committed = false // Accessed under lock; cancellation after commit is too late.
    }

    fun snapshot(): TranscriptionState = synchronized(lock) { state }

    /** Returns false if another native invocation has not yet stopped. Does not enqueue audio. */
    fun start(recordingId: String, language: String = "lt"): Boolean = synchronized(lock) {
        check(!closed) { "Transcription controller is closed" }
        require(TranscriptionRepository.validId(recordingId) && language == "lt") { "Unsupported transcription request" }
        if (active != null) return@synchronized false
        require(recordings.find(recordingId) != null) { "Recording is no longer on this phone" }
        val job = Job(++generation, recordingId)
        active = job
        state = TranscriptionState(TranscriptionPhase.RUNNING, recordingId, 0, "Checking the local model")
        publish(job.generation)
        try { executor.execute { run(job, language) } }
        catch (_: RuntimeException) {
            active = null
            state = TranscriptionState(TranscriptionPhase.FAILED, recordingId, message = "Transcription worker could not start")
            publish(job.generation)
        }
        true
    }

    fun cancel() = synchronized(lock) {
        val job = active ?: return@synchronized
        if (job.committed) return@synchronized
        job.cancelled = true
        state = TranscriptionState(TranscriptionPhase.CANCELLING, job.id, state.progress, "Stopping transcription")
        publish(generation)
    }

    /** Atomic with respect to this controller's publication; no late transcript resurrection. */
    fun deletePhone(recordingId: String) = synchronized(lock) {
        require(TranscriptionRepository.validId(recordingId)) { "Invalid recording identifier" }
        val job = active
        if (job?.id == recordingId) {
            job.cancelled = true
            generation++ // Invalidate already-posted progress/results as well as native completion.
        }
        // Remove derived sensitive text first. If this fails, the WAV stays in
        // the visible library so deletion can be retried; never strand an
        // invisible transcript by removing its library item first. These two
        // filesystem deletions are ordered, not a cross-file atomic operation.
        transcripts.delete(recordingId)
        recordings.deletePhone(recordingId)
        if (job?.id == recordingId) {
            state = TranscriptionState(TranscriptionPhase.CANCELLING, recordingId, message = "Deleted from phone; stopping transcription")
            publish(generation)
        } else if (state.recordingId == recordingId) {
            generation++
            state = TranscriptionState()
            publish(generation)
        }
    }

    fun close() {
        synchronized(lock) {
            if (closed) return
            closed = true
            generation++
            active?.cancelled = true
        }
        executor.shutdownNow()
    }

    private fun run(job: Job, language: String) {
        var rawPcm: ByteArray? = null
        var canonicalWav: ByteArray? = null
        var samples: FloatArray? = null
        var outcome = TranscriptionPhase.FAILED
        var message = "Transcription failed; the recording is unchanged"
        try {
            models.withVerifiedModel({ stopped(job) }) { model ->
                checkRunning(job)
                rawPcm = recordings.readPcm(job.id)
                canonicalWav = WavCodec.encode(rawPcm!!)
                val audioHash = MessageDigest.getInstance("SHA-256").digest(canonicalWav!!).hex()
                samples = FloatArray(rawPcm!!.size / 2) { index ->
                    val offset = index * 2
                    (((rawPcm!![offset].toInt() and 0xff) or (rawPcm!![offset + 1].toInt() shl 8)).toShort().toInt()) / 32768f
                }
                checkRunning(job)
                val segments = engine.transcribe(model.file.absolutePath, samples!!, language, object : TranscriptionEngine.Callbacks {
                    override fun isCancelled(): Boolean = stopped(job)
                    override fun onProgress(percent: Int) = synchronized(lock) {
                        if (active === job && !stopped(job)) {
                            state = TranscriptionState(TranscriptionPhase.RUNNING, job.id,
                                maxOf(state.progress, percent.coerceIn(0, 99)), "Transcribing locally")
                            publish(job.generation)
                        }
                    }
                })
                checkRunning(job)
                val document = TranscriptDocument(job.id, audioHash, model.spec.id, model.spec.sha256,
                    engine.version, language, now(), segments.toList())
                synchronized(lock) {
                    checkRunning(job)
                    require(recordings.find(job.id) != null) { "Recording was removed" }
                    transcripts.saveVerified(document)
                    job.committed = true
                    outcome = TranscriptionPhase.COMPLETE
                    message = if (document.text.isEmpty()) "Complete; no speech text returned" else "Transcription saved on this phone"
                }
            }
        } catch (_: CancellationException) {
            outcome = TranscriptionPhase.CANCELLED
            message = "Transcription cancelled; no partial result was saved"
        } catch (_: OutOfMemoryError) {
            message = "Not enough memory for the local model; the recording is unchanged"
        } catch (_: UnsatisfiedLinkError) {
            message = "The local transcription engine is unavailable on this device"
        } catch (_: Exception) {
            if (stopped(job)) {
                outcome = TranscriptionPhase.CANCELLED
                message = "Transcription cancelled; no partial result was saved"
            }
            // Do not publish exception/native text, filesystem paths, model prompts or audio.
        } finally {
            rawPcm?.fill(0); canonicalWav?.fill(0); samples?.fill(0f)
            synchronized(lock) {
                if (active === job) {
                    active = null
                    if (!closed) {
                        if (job.cancelled || job.generation != generation) {
                            state = TranscriptionState(TranscriptionPhase.CANCELLED, job.id,
                                message = "Transcription cancelled; no partial result was saved")
                        } else {
                            state = TranscriptionState(outcome, job.id, if (outcome == TranscriptionPhase.COMPLETE) 100 else 0, message)
                        }
                        publish(generation)
                    }
                }
            }
        }
    }

    private fun stopped(job: Job): Boolean = synchronized(lock) {
        closed || job.cancelled || active !== job || job.generation != generation || Thread.currentThread().isInterrupted
    }
    private fun checkRunning(job: Job) { if (stopped(job)) throw CancellationException() }
    private fun publish(token: Long) {
        val retained = state
        try {
            dispatch {
                val current = synchronized(lock) { !closed && token == generation && state == retained }
                // A detached/broken UI must not abort cleanup or turn a queued job into
                // a permanently busy controller. No transcript text is sent in events.
                if (current) try { onState(retained) } catch (_: RuntimeException) { }
            }
        } catch (_: RuntimeException) { }
    }
}
