package org.openpendant.app

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Looper

/** Dormant AudioTrack sink. Only explicit worker start allocates/starts audio.
 * No Activity, focus request, media session, route selection or auto-resume.
 * Integration must obtain audio focus and stop on lifecycle/focus loss; these
 * remain UI integration gates. close may be called by deletion on another worker.
 */
class AndroidRecordingPcmSink : RecordingPcmSink {
    private var submitted = 0L
    private val output = EmptyPlaybackOutput<AudioTrack> { old ->
        try { old.pause() } finally {
            try { old.flush() } finally { old.release() }
        }
    }

    override fun start() {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Playback must run off the main thread" }
        output.start {
            val minimum = AudioTrack.getMinBufferSize(16000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
            check(minimum in 1..32000) { "Bounded audio output is unavailable" }
            val made = AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                .setAudioFormat(AudioFormat.Builder().setSampleRate(16000)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(maxOf(1280, minimum)).build()
            try {
                check(made.state == AudioTrack.STATE_INITIALIZED)
                made.play() // still empty/unpublished: no PCM write can reach it
                made
            } catch (failure: Throwable) {
                try { made.release() } catch (cleanup: Throwable) { output.markDisposalFailure(); throw cleanup }
                throw failure
            }
        }
    }
    override fun write(pcm16le: ByteArray, offset: Int, length: Int): Int = output.useOutput { track ->
        require(offset >= 0 && length in 2..640 && length % 2 == 0 && offset <= pcm16le.size - length)
        val count = track.write(pcm16le, offset, length, AudioTrack.WRITE_NON_BLOCKING)
        check(count in 0..length && count % 2 == 0) { "Audio output stopped" }
        submitted += count / 2
        count
    }
    override fun pendingFrames(): Long = output.useOutput { track ->
        // Total output is capped below uint32 wrap; no guessed wrap recovery.
        val consumed = track.playbackHeadPosition.toLong() and 0xffffffffL
        check(consumed <= submitted) { "Audio output position changed" }
        submitted - consumed
    }
    override fun close() = output.close()
}
