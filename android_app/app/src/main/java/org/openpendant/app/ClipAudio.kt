package org.openpendant.app

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.Executors
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.roundToInt

object WavCodec {
    fun encode(pcm: ByteArray): ByteArray {
        ensure(pcm.size in 640..OpProtocol.MAX_BYTES && pcm.size % 640 == 0, "Invalid recording sample length")
        val result = ByteArray(44 + pcm.size)
        "RIFF".toByteArray(Charsets.US_ASCII).copyInto(result)
        OpProtocol.put32(result, 4, (36 + pcm.size).toLong())
        "WAVEfmt ".toByteArray(Charsets.US_ASCII).copyInto(result, 8)
        OpProtocol.put32(result, 16, 16)
        result[20] = 1; result[22] = 1
        OpProtocol.put32(result, 24, 16000); OpProtocol.put32(result, 28, 32000)
        result[32] = 2; result[34] = 16
        "data".toByteArray(Charsets.US_ASCII).copyInto(result, 36)
        OpProtocol.put32(result, 40, pcm.size.toLong())
        pcm.copyInto(result, 44)
        return result
    }

    fun decode(wav: ByteArray): ByteArray {
        ensure(wav.size in 684..32044, "Local WAV exceeds bounded recording format")
        ensure(wav.copyOfRange(0, 4).contentEquals("RIFF".toByteArray(Charsets.US_ASCII)) &&
            wav.copyOfRange(8, 16).contentEquals("WAVEfmt ".toByteArray(Charsets.US_ASCII)) &&
            wav.copyOfRange(36, 40).contentEquals("data".toByteArray(Charsets.US_ASCII)), "Unsupported local WAV layout")
        val count = OpProtocol.u32(wav, 40)
        ensure(count in 640..32000 && count % 640 == 0L && wav.size == 44 + count.toInt() &&
            OpProtocol.u32(wav, 4) == 36 + count && OpProtocol.u32(wav, 16) == 16L &&
            OpProtocol.u16(wav, 20) == 1 && OpProtocol.u16(wav, 22) == 1 &&
            OpProtocol.u32(wav, 24) == 16000L && OpProtocol.u32(wav, 28) == 32000L &&
            OpProtocol.u16(wav, 32) == 2 && OpProtocol.u16(wav, 34) == 16, "Local WAV metadata mismatch")
        return wav.copyOfRange(44, wav.size)
    }

    /** Listening copy only: RC one-pole 100 Hz high-pass, then peak 0.7.
     * The raw WAV is unchanged; normalization is not SNR/quality evidence.
     * x[-1]=x[0], y[-1]=0 avoids a fabricated DC boundary impulse.
     */
    fun preview(pcm: ByteArray): ByteArray {
        ensure(pcm.size in 640..OpProtocol.MAX_BYTES && pcm.size % 640 == 0, "Invalid listening-copy size")
        val values = DoubleArray(pcm.size / 2)
        val result = ByteArray(pcm.size)
        val alpha = 1.0 / (1.0 + 2.0 * PI * 100.0 / 16000.0)
        var previousInput = OpProtocol.u16(pcm, 0).toShort().toDouble()
        var previousOutput = 0.0
        var peak = 0.0
        try {
            values.indices.forEach { index ->
                val value = OpProtocol.u16(pcm, index * 2).toShort().toDouble()
                previousOutput = alpha * (previousOutput + value - previousInput)
                previousInput = value
                values[index] = previousOutput
                peak = maxOf(peak, abs(previousOutput))
            }
            val gain = if (peak > 0) 0.7 * 32767.0 / peak else 1.0
            values.indices.forEach { index ->
                val value = (values[index] * gain).roundToInt().coerceIn(-32768, 32767)
                result[index * 2] = value.toByte(); result[index * 2 + 1] = (value shr 8).toByte()
            }
            return result
        } finally { values.fill(0.0) }
    }
}

/** A single app-private, explicitly deleted recording. No backup or export API. */
class ClipRepository(private val directory: File) {
    private val clip get() = File(directory, "latest_verified_clip.wav")
    private val temporary get() = File(directory, "pending_verified_clip.wav")
    init {
        // Only our unpublished staging file is discarded after process death.
        // The user's finalized recording is never removed automatically.
        if (temporary.exists()) ensure(temporary.delete(), "Could not discard an incomplete local file")
    }
    fun exists() = clip.isFile
    fun bytes() = if (exists()) clip.length() else 0L
    fun save(pcm: ByteArray) {
        ensure(!clip.exists() && !temporary.exists(), "Delete the existing local clip before recording another")
        val encoded = WavCodec.encode(pcm)
        try {
            FileOutputStream(temporary).use { it.write(encoded); it.fd.sync() }
            ensure(temporary.renameTo(clip), "Could not finalize the local recording")
        } finally {
            encoded.fill(0)
            if (temporary.exists()) temporary.delete()
        }
    }
    fun readPcm(): ByteArray {
        ensure(clip.isFile && clip.length() in 684..32044, "Local recording is unavailable or invalid")
        val bytes = clip.readBytes()
        try { return WavCodec.decode(bytes) } finally { bytes.fill(0) }
    }
    fun delete() {
        ensure(!clip.exists() || clip.delete(), "Could not delete the local recording")
        ensure(!temporary.exists() || temporary.delete(), "Could not delete an incomplete local file")
    }
}

/** Static tracks become STATE_INITIALIZED only after the first successful write. */
internal fun loadStaticPlayback(pcm: ByteArray, state: () -> Int, write: (ByteArray) -> Int) {
    ensure(pcm.size in 640..OpProtocol.MAX_BYTES && pcm.size % 640 == 0,
        "Invalid playback buffer length")
    val before = state()
    ensure(before == AudioTrack.STATE_NO_STATIC_DATA || before == AudioTrack.STATE_INITIALIZED,
        "Audio output could not be initialized")
    ensure(write(pcm) == pcm.size, "Audio output did not accept the complete clip")
    ensure(state() == AudioTrack.STATE_INITIALIZED, "Audio output is not ready after loading the clip")
}

/** Playback is only called by a user tap; stops on background/disconnect UI action. */
class ClipPlayer(private val onState: (Boolean, String?) -> Unit) {
    private val main = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor()
    private var track: AudioTrack? = null
    private var stopTask: Runnable? = null
    private var generation = 0
    var playing = false
        private set

    fun play(repository: ClipRepository) {
        play { repository.readPcm() }
    }

    /** Library reads one stable ID only after an explicit Play tap. */
    fun play(readPcm: () -> ByteArray) {
        stop()
        val token = ++generation
        playing = true; onState(true, null)
        executor.execute {
            var pcm: ByteArray? = null
            var listening: ByteArray? = null
            try {
                pcm = readPcm()
                listening = WavCodec.preview(pcm)
                val retained = listening
                listening = null
                main.post {
                    try {
                        if (token != generation) return@post
                        val output = AudioTrack.Builder()
                            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                            .setAudioFormat(AudioFormat.Builder().setSampleRate(16000)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                            .setTransferMode(AudioTrack.MODE_STATIC).setBufferSizeInBytes(retained.size).build()
                        track = output
                        loadStaticPlayback(retained, { output.state }) { data ->
                            output.write(data, 0, data.size, AudioTrack.WRITE_BLOCKING)
                        }
                        output.play()
                        stopTask = Runnable { if (token == generation) stop() }
                        main.postDelayed(stopTask!!, retained.size * 1000L / 32000L + 150)
                    } catch (error: Exception) {
                        stop()
                        val detail = if (error is ProtocolException) " ${error.message}." else ""
                        onState(false, "Playback could not start.$detail The saved clip is unchanged.")
                    }
                    finally { retained.fill(0) }
                }
            } catch (_: Exception) {
                main.post { if (token == generation) { stop(); onState(false, "Local recording could not be played.") } }
            } finally { pcm?.fill(0); listening?.fill(0) }
        }
    }

    fun stop() {
        generation++
        stopTask?.let(main::removeCallbacks); stopTask = null
        track?.let { try { it.stop() } catch (_: Exception) { }; it.release() }; track = null
        val wasPlaying = playing; playing = false
        if (wasPlaying) onState(false, null)
    }
    fun close() { stop(); executor.shutdownNow() }
}
