package org.openpendant.app

import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean

enum class RecordingGapPolicy { PAUSE_AT_GAP, INSERT_BOUNDED_SILENCE }
enum class RecordingPlaybackEnd { COMPLETED, PAUSED_AT_GAP }
data class RecordingPlaybackResult(val end: RecordingPlaybackEnd, val nextSequence: Int,
                                   val renderedSamples: Long, val insertedSilenceSamples: Long)
class RecordingPlaybackException : IllegalStateException("Recording playback stopped; no recording was changed")

/** These ports are exclusive short-lived ownership boundaries, not a protocol.
 * read returns one caller-owned ciphertext <=65745 bytes, never retained/logged.
 * withKey must provide only the authenticated active recipient, close it before
 * returning, and never create/import/rotate a key. */
fun interface PlaybackCiphertextSource { fun read(expected: EncryptedSegmentExpectation): ByteArray }
fun interface PlaybackKeyAccess { fun withKey(action: (RecipientRecoveryKey) -> Unit) }

/** All operations except close execute on ONE playback worker. start creates an
 * EMPTY output and may block in platform setup; it runs OUTSIDE the coordinator
 * lock, with cancellation checks before/after, and cannot output PCM by itself.
 * write/pendingFrames are bounded and nonblocking. write accepts even byte counts
 * <=640, returns an even accepted count in0..length; it cannot retain arrays.
 * close is idempotent/thread-safe and MUST stop, discard queued PCM and release,
 * including after partial start. No close implementation may join the worker.
 * No sink may persist/export audio. The Android adapter is dormant until start. */
interface RecordingPcmSink : AutoCloseable {
    fun start()
    fun write(pcm16le: ByteArray, offset: Int, length: Int): Int
    fun pendingFrames(): Long
    override fun close()
}

/** Explicit whole-final-recording job; never starts from construction or startup.
 * One ciphertext, one authenticated plaintext and one <=10s decoded segment at
 * a time; no recording-sized allocation or disk plaintext. Boundary seeking
 * reauthenticates skipped layouts (no decoding) to preserve profile/continuity.
 *
 * Default gaps stop before the next segment. Explicit silence is limited to60s
 * per gap and2h total output; there is no silent gap concatenation. firstSample
 * is a recording timeline counter, not an instruction to synthesize leading
 * silence. A first-segment GAP also pauses unless silence policy was explicit.
 * No receipt/source deletion/transcription/UI/audio-focus policy is performed.
 */
class RecordingPlayback private constructor(
    private val coordinator: RecordingSyncContract,
    private val manifest: RecordingManifest,
    private val loadAuthenticated: (SegmentIdentity, () -> Unit) -> ByteArray,
    private val decoder: OpusPacketDecoder,
    private val sink: RecordingPcmSink,
    private val registry: RecordingPlaybackRegistry,
    private val policy: RecordingGapPolicy,
    private val clockMillis: () -> Long,
    private val waitBriefly: () -> Unit,
) {
    constructor(coordinator: RecordingSyncContract, manifest: RecordingManifest,
                source: PlaybackCiphertextSource, keys: PlaybackKeyAccess,
                decoder: OpusPacketDecoder, sink: RecordingPcmSink,
                registry: RecordingPlaybackRegistry,
                policy: RecordingGapPolicy = RecordingGapPolicy.PAUSE_AT_GAP,
                clockMillis: () -> Long = { System.nanoTime() / 1_000_000 },
                waitBriefly: () -> Unit = { Thread.sleep(5) }) : this(coordinator, manifest,
        { segment, current -> authenticated(segment, source, keys, current) }, decoder, sink, registry, policy, clockMillis, waitBriefly)

    /** Generated-only parser/scheduling fixture seam. Production uses real HPKE. */
    internal constructor(coordinator: RecordingSyncContract, manifest: RecordingManifest,
                         loader: (SegmentIdentity) -> ByteArray, decoder: OpusPacketDecoder,
                         sink: RecordingPcmSink, policy: RecordingGapPolicy,
                         clockMillis: () -> Long, waitBriefly: () -> Unit,
                         @Suppress("UNUSED_PARAMETER") testOnly: Unit,
                         registry: RecordingPlaybackRegistry = RecordingPlaybackRegistry()) :
        this(coordinator, manifest, { segment, current -> current(); loader(segment) }, decoder, sink, registry, policy, clockMillis, waitBriefly)

    private val used = AtomicBoolean(false)
    private val cancelled = AtomicBoolean(false)
    /** UI pause/seek: signal only; worker drains no more audio and owns output
     * disposal. No simultaneous platform close races during ordinary seeking. */
    internal fun interruptForSeek() { cancelled.set(true) }
    /** Stops at next bounded read/decrypt/decode/write boundary; native decode
     * is not interruptible. No queued write is admitted after ticket invalidation. */
    fun cancel() {
        cancelled.set(true)
        try { sink.close() } catch (failure: Throwable) { registry.fence(); throw failure }
    }

    fun run(startSequence: Int = 0, startOffsetSamples: Int = 0,
        onPosition: (Long) -> Unit = {}): RecordingPlaybackResult {
        require(startOffsetSamples >= 0)
        check(used.compareAndSet(false, true)) { "Playback sessions are single-use" }
        var ticket: RecordingWorkTicket? = null
        var registration: AutoCloseable? = null
        var rendered = 0L; var silence = 0L; var started = false
        var prefix = 0L
        var reportAt = Long.MIN_VALUE
        var lastClock = clockMillis()
        fun now(): Long {
            val value = clockMillis()
            if (value < lastClock) throw RecordingPlaybackException()
            lastClock = value; return value
        }
        fun current() {
            if (cancelled.get() || ticket?.let { !coordinator.isCurrent(it) } == true)
                throw CancellationException("Playback cancelled")
            now()
        }
        fun guarded(action: () -> Unit) {
            current()
            if (!coordinator.playbackStep(ticket!!, action)) throw CancellationException("Playback cancelled")
            current()
        }
        fun report(force: Boolean = false) {
            val time = now()
            if (!force && reportAt != Long.MIN_VALUE && time - reportAt < 200) return
            var pending = 0L
            if (started) guarded { pending = sink.pendingFrames() }
            if (pending !in 0..MAX_PENDING_FRAMES || pending > rendered) throw RecordingPlaybackException()
            onPosition(prefix + startOffsetSamples + (rendered - pending - silence).coerceAtLeast(0))
            reportAt = time
        }
        fun write(bytes: ByteArray, initialOffset: Int = 0) {
            require(bytes.size % 2 == 0)
            require(initialOffset in 0..bytes.size && initialOffset % 2 == 0)
            requireOutputRoom(rendered, (bytes.size - initialOffset) / 2)
            if (!started) {
                current(); sink.start(); current(); started = true
            }
            var offset = initialOffset; var progressAt = now()
            while (offset < bytes.size) {
                val size = minOf(640, bytes.size - offset)
                var accepted = 0
                guarded { accepted = sink.write(bytes, offset, size) }
                if (accepted !in 0..size || accepted % 2 != 0) throw RecordingPlaybackException()
                if (accepted > 0) { offset += accepted; rendered += accepted / 2; progressAt = now() }
                else { if (now() - progressAt >= STALL_MILLIS) throw RecordingPlaybackException(); waitBriefly() }
                report()
            }
        }
        fun drain() {
            if (!started) return
            val since = now()
            while (true) {
                var pending = 0L
                guarded { pending = sink.pendingFrames() }
                if (pending !in 0..MAX_PENDING_FRAMES) throw RecordingPlaybackException()
                report()
                if (pending == 0L) { report(true); return }
                if (now() - since >= STALL_MILLIS) throw RecordingPlaybackException()
                waitBriefly()
            }
        }
        try {
            current()
            registration = registry.register(manifest) { cancel() }
            ticket = coordinator.beginPlayback(manifest, startSequence)
            var previous: OpusSegment.Layout? = null
            for (segment in manifest.segments) {
                current()
                val plain = loadAuthenticated(segment, ::current)
                try {
                    current()
                    val layout = OpusSegment.parse(plain, segment.sequence)
                    previous?.let { OpusSegment.requireFollowing(it, layout) }
                    if (segment.sequence < startSequence) prefix += layout.validSamples
                    if (segment.sequence == startSequence) require(startOffsetSamples < layout.validSamples)
                    if (segment.sequence >= startSequence) {
                        if (layout.gapBefore && policy == RecordingGapPolicy.PAUSE_AT_GAP) {
                            drain(); current()
                            if (!coordinator.finishPlayback(ticket)) throw CancellationException("Playback cancelled")
                            return RecordingPlaybackResult(RecordingPlaybackEnd.PAUSED_AT_GAP, segment.sequence, rendered, silence)
                        }
                        if (layout.gapBefore && segment.sequence > startSequence && previous != null) {
                            val gap = layout.firstSample - previous.nextSample
                            if (gap > MAX_GAP_SAMPLES.toULong()) throw RecordingPlaybackException()
                            var remaining = gap.toLong()
                            val zeros = ByteArray(640)
                            while (remaining > 0) {
                                val count = minOf(320L, remaining).toInt()
                                val block = if (count == 320) zeros else ByteArray(count * 2)
                                try { write(block) } finally { block.fill(0) }
                                remaining -= count; silence += count
                            }
                        }
                        OpusPcmAssembler.decodeAuthenticated(plain, segment.sequence, decoder) {
                            cancelled.get() || !coordinator.isCurrent(ticket)
                        }.use { pcm ->
                            val bytes = pcm.copyPcm16Le()
                            try { write(bytes, if (segment.sequence == startSequence) startOffsetSamples * 2 else 0) }
                            finally { bytes.fill(0) }
                        }
                    }
                    previous = layout
                } finally { plain.fill(0) }
            }
            drain(); current()
            if (!coordinator.finishPlayback(ticket)) throw CancellationException("Playback cancelled")
            return RecordingPlaybackResult(RecordingPlaybackEnd.COMPLETED, manifest.segments.size, rendered, silence)
        } finally {
            // No restart/resume token survives. The encrypted source is untouched.
            try { sink.close() } catch (failure: Throwable) { registry.fence(); throw failure } finally {
                try { ticket?.let { if (coordinator.isCurrent(it)) coordinator.cancelWork(it) } }
                finally { registration?.close() }
            }
        }
    }

    companion object {
        // Streamed, one segment at a time. Match the full-volume upper bound
        // without silently stopping otherwise valid recordings at two hours.
        const val MAX_OUTPUT_SAMPLES = DurableManifestCodec.FULL_MAX_SEGMENTS * 10L * 16000L
        internal fun requireOutputRoom(rendered: Long, samples: Int) {
            if (rendered < 0 || samples < 0 || rendered > MAX_OUTPUT_SAMPLES - samples)
                throw RecordingPlaybackException()
        }
        const val MAX_GAP_SAMPLES = 60L * 16000
        const val MAX_PENDING_FRAMES = 16000L
        const val STALL_MILLIS = 5000L
        private fun authenticated(segment: SegmentIdentity, source: PlaybackCiphertextSource,
                                  keys: PlaybackKeyAccess, current: () -> Unit): ByteArray {
            var answer: ByteArray? = null
            var callbacks = 0
            try {
                current()
                keys.withKey { key ->
                    current()
                    check(++callbacks == 1) { "Invalid recipient callback" }
                    val plainBytes = segment.byteCount - 209
                    require(plainBytes in 1..EncryptedSegmentHeader.MAX_PLAINTEXT_BYTES.toLong())
                    val fingerprint = key.publicFingerprint()
                    val expected = try { EncryptedSegmentExpectation(segment, fingerprint, plainBytes.toInt()) }
                        finally { fingerprint.fill(0) }
                    val cipher = source.read(expected)
                    try {
                        current()
                        if (cipher.size.toLong() != segment.byteCount || digestHex(cipher) != segment.sha256)
                            throw EncryptedSegmentException()
                        EncryptedSegmentRecipient.decrypt(cipher, key, segment.recording, segment.sequence, plainBytes.toInt()).use {
                            answer = it.copyForAuthenticatedUse()
                        }
                        current()
                    } finally { cipher.fill(0) }
                }
                check(callbacks == 1 && answer != null) { "Recipient unavailable" }
                current()
                return answer!!.also { answer = null }
            } finally { answer?.fill(0) }
        }
    }
}
