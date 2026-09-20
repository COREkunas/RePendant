package org.openpendant.app

import java.util.concurrent.CancellationException

/** A fresh mono16k decoder per call. Input is 1..501 concatenated60-byte packets.
 * Ownership of the returned untrimmed PCM transfers to the assembler, which
 * wipes it on every path. Implementations must not retain input/output arrays.
 */
fun interface OpusPacketDecoder { fun decode(packets: ByteArray): ShortArray }

/** Explicit RAM-only derivative of an ALREADY authenticated, identity-bound
 * segment. No disk, UI, playback, owner-key lookup, or automatic decoding.
 * Caller owns and must later wipe plaintext. A parsed layout is not auth proof.
 */
object OpusPcmAssembler {
    const val MAX_SAMPLES = 500 * 320
    const val MAX_WAV_BYTES = 44 + MAX_SAMPLES * 2

    fun decodeAuthenticated(
        plaintext: ByteArray, expectedSequence: Int, decoder: OpusPacketDecoder,
        cancelled: () -> Boolean = { false }
    ): PcmSegment {
        fun current() { if (cancelled()) throw CancellationException("Opus decode cancelled") }
        current()
        val layout = OpusSegment.parse(plaintext, expectedSequence)
        var packets: ByteArray? = null
        var decoded: ShortArray? = null
        var trimmed: ShortArray? = null
        try {
            packets = ByteArray(layout.packets.size * OpusSegment.PACKET_BYTES)
            layout.packets.forEachIndexed { index, packet ->
                current()
                plaintext.copyInto(packets, index * OpusSegment.PACKET_BYTES, packet.offset, packet.offset + packet.length)
            }
            current()
            decoded = decoder.decode(packets)
            current() // cancellation during blocking native decode cannot return PCM
            require(decoded.size == layout.packets.size * 320) { "Decoded sample count differs" }
            require(layout.validSamples in 320..MAX_SAMPLES &&
                decoded.size - layout.preSkip - layout.endTrim == layout.validSamples) { "Decoded trimming differs" }
            trimmed = decoded.copyOfRange(layout.preSkip, decoded.size - layout.endTrim)
            current()
            return PcmSegment(expectedSequence, trimmed).also { trimmed = null }
        } finally {
            packets?.fill(0); decoded?.fill(0); trimmed?.fill(0)
        }
    }

    /** Bounded, closeable PCM owner. Explicit copies belong to the caller and
     * must be wiped after use. This is not a persistent derivative format.
     */
    class PcmSegment internal constructor(val sequence: Int, private val pcm: ShortArray) : AutoCloseable {
        init { require(sequence >= 0 && pcm.size in 320..MAX_SAMPLES && pcm.size % 320 == 0) { "Invalid PCM owner" } }
        private var closed = false
        val samples: Int get() = synchronized(this) { checkOpen(); pcm.size }
        @Synchronized fun copyPcm16Le(): ByteArray {
            checkOpen()
            return ByteArray(pcm.size * 2).also { bytes -> copySamples(bytes, 0) }
        }
        @Synchronized fun copyWav(): ByteArray {
            checkOpen()
            val dataBytes = pcm.size * 2
            return ByteArray(44 + dataBytes).also { wav ->
                "RIFF".toByteArray(Charsets.US_ASCII).copyInto(wav)
                put(wav, 4, 36 + dataBytes, 4)
                "WAVEfmt ".toByteArray(Charsets.US_ASCII).copyInto(wav, 8)
                put(wav, 16, 16, 4); put(wav, 20, 1, 2); put(wav, 22, 1, 2)
                put(wav, 24, 16000, 4); put(wav, 28, 32000, 4)
                put(wav, 32, 2, 2); put(wav, 34, 16, 2)
                "data".toByteArray(Charsets.US_ASCII).copyInto(wav, 36)
                put(wav, 40, dataBytes, 4); copySamples(wav, 44)
            }
        }
        private fun copySamples(out: ByteArray, start: Int) {
            pcm.forEachIndexed { i, value ->
                out[start + i * 2] = value.toByte(); out[start + i * 2 + 1] = (value.toInt() shr 8).toByte()
            }
        }
        private fun put(out: ByteArray, at: Int, value: Int, bytes: Int) {
            repeat(bytes) { out[at + it] = (value ushr (it * 8)).toByte() }
        }
        private fun checkOpen() { check(!closed) { "PCM owner is closed" } }
        @Synchronized override fun close() { pcm.fill(0); closed = true }
        override fun toString() = "PcmSegment[redacted]"
    }
}
