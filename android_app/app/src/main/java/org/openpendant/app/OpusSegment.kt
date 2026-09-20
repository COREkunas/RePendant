package org.openpendant.app

import java.util.Collections

/** Layout only: no audio bytes are retained, copied, decoded or authenticated.
 * Call only AFTER HPKE authentication and exact encrypted-header binding to the
 * full recording identity, expected sequence and plaintext length. The caller
 * must exclusively own the input until decoding finishes, then wipe it. A layout
 * is not an authentication token for a subsequently modified ByteArray.
 *
 * This custom OPNDOP1 stream is not an Ogg/.opus file. Each segment requires a
 * fresh 16 kHz mono Opus decoder. Decode every packet, remove preSkip samples at
 * the head and endTrim at the tail; exactly validSamples must remain. There is
 * deliberately no playback hookup until the decoder and encrypted store exist.
 */
object OpusSegment {
    const val HEADER_BYTES = 80
    const val PACKET_BYTES = 60
    const val RECORD_BYTES = 68
    const val MAX_CAPTURED_FRAMES = 500
    const val MAX_BYTES = HEADER_BYTES + (MAX_CAPTURED_FRAMES + 1) * RECORD_BYTES
    private val magic = byteArrayOf(79, 80, 78, 68, 79, 80, 49, 0)

    enum class CodecProfile(val wireId: Long) {
        LEGACY_VOIP(1), CELT_LOW_DELAY(2)
    }

    data class Packet(val index: Int, val offset: Int, val length: Int, val flush: Boolean)

    class Layout internal constructor(
        val segmentSequence: Int,
        val codecProfile: CodecProfile,
        val firstSample: ULong,
        val nextSample: ULong,
        val capturedFrames: Int,
        val preSkip: Int,
        val endTrim: Int,
        val gapBefore: Boolean,
        packets: List<Packet>
    ) {
        val packets: List<Packet> = Collections.unmodifiableList(ArrayList(packets))
        val validSamples: Int get() = capturedFrames * 320
    }

    /** expectedSequence comes from the already authenticated encrypted header. */
    fun parse(plaintext: ByteArray, expectedSequence: Int): Layout {
        require(expectedSequence >= 0) { "Invalid segment sequence" }
        require(plaintext.size in HEADER_BYTES..MAX_BYTES) { "Invalid segment size" }
        fun u16(at: Int) = (plaintext[at].toInt() and 255) or ((plaintext[at + 1].toInt() and 255) shl 8)
        fun u32(at: Int): Long = (0 until 4).fold(0L) { n, i -> n or ((plaintext[at + i].toLong() and 255L) shl (8 * i)) }
        fun u64(at: Int): ULong = (0 until 8).fold(0uL) { n, i -> n or ((plaintext[at + i].toULong() and 255uL) shl (8 * i)) }
        require(magic.indices.all { plaintext[it] == magic[it] } && u16(8) == 1 && u16(10) == HEADER_BYTES) { "Invalid segment header" }
        require(u32(12) == 16000L && u16(16) == 1 && u16(18) == 320 &&
            u32(20) == 24000L && u32(24) == 3L && u32(76) == 0L) { "Unsupported codec controls" }
        val profile = CodecProfile.entries.singleOrNull { it.wireId == u32(72) }
            ?: throw IllegalArgumentException("Unsupported codec profile")
        val flags = u32(28)
        val frames = u32(48)
        val count = u32(52)
        val delay = u32(56)
        val trim = u32(60)
        require((flags == 1L || flags == 3L) && frames in 1L..MAX_CAPTURED_FRAMES.toLong() && delay in 0L..320L) { "Invalid segment bounds" }
        // Preserve legacy v1 lookahead compatibility. Profile2 specifically
        // pins libopus1.6.1 restricted-lowdelay at16kHz: queried40-sample delay.
        require(profile != CodecProfile.CELT_LOW_DELAY || delay == 40L) { "Invalid CELT lookahead" }
        require(count == frames + if (delay > 0) 1 else 0) { "Invalid packet count" }
        require(trim == if (delay > 0) 320L - delay else 0L) { "Invalid tail trim" }
        val payload = count * RECORD_BYTES
        val valid = frames * 320
        require(u32(64) == payload && u32(68) == valid && plaintext.size.toLong() == HEADER_BYTES + payload) { "Invalid segment framing" }
        val first = u64(32)
        val next = u64(40)
        require(first <= ULong.MAX_VALUE - valid.toULong() && next == first + valid.toULong()) { "Invalid sample interval" }
        val packets = ArrayList<Packet>(count.toInt())
        repeat(count.toInt()) { index ->
            val at = HEADER_BYTES + index * RECORD_BYTES
            val flush = index >= frames
            require(u16(at) == PACKET_BYTES && u16(at + 2) == if (flush) 1 else 0) { "Invalid packet record" }
            require(u32(at + 4) == index.toLong()) { "Invalid packet continuity" }
            validateOpusFraming(plaintext, at + 8)
            require(profile != CodecProfile.CELT_LOW_DELAY || plaintext[at + 8].toInt() and 128 != 0) { "Non-CELT packet in CELT profile" }
            packets.add(Packet(index, at + 8, PACKET_BYTES, flush))
        }
        return Layout(expectedSequence, profile, first, next, frames.toInt(), delay.toInt(), trim.toInt(), flags == 3L, packets)
    }

    /** Caller must first prove both encrypted headers belong to the SAME full
     * recording identity. No silent loss, overlap, reordering or invented gap.
     * A first segment may have GAP if paused/resumed before its first frame.
     */
    fun requireFollowing(previous: Layout, next: Layout) {
        require(previous.codecProfile == next.codecProfile) { "Codec profile changed within recording" }
        require(previous.segmentSequence < Int.MAX_VALUE && next.segmentSequence == previous.segmentSequence + 1) { "Nonconsecutive segments" }
        require(if (next.gapBefore) next.firstSample > previous.nextSample else next.firstSample == previous.nextSample) { "Invalid segment continuity" }
    }

    // Public Opus packet framing rules, aligned with pinned libopus 1.6.1
    // src/opus.c opus_packet_parse_impl and opus_decoder.c metadata helpers.
    // This checks TOC, lengths/padding and 20 ms mono duration, NOT entropy
    // decoding, perceptual quality, cryptographic integrity or encoder origin.
    private fun validateOpusFraming(bytes: ByteArray, at: Int) {
        fun byte(i: Int) = bytes[i].toInt() and 255
        val end = at + PACKET_BYTES
        val toc = byte(at)
        require(toc and 4 == 0) { "Stereo packet rejected" }
        val samples = when {
            toc and 128 != 0 -> (16000 shl ((toc shr 3) and 3)) / 400
            toc and 96 == 96 -> if (toc and 8 != 0) 320 else 160
            (toc shr 3) and 3 == 3 -> 960
            else -> (16000 shl ((toc shr 3) and 3)) / 100
        }
        var cursor = at + 1
        val code = toc and 3
        var flags = 0
        val count = when (code) {
            0 -> 1
            1, 2 -> 2
            else -> { flags = byte(cursor++); flags and 63 }
        }
        require(count in 1..48 && samples * count == 320) { "Packet must contain 320 samples" }
        var dataEnd = end
        if (code == 3 && flags and 64 != 0) {
            var paddingByte: Int
            do {
                require(cursor < dataEnd) { "Truncated Opus padding" }
                paddingByte = byte(cursor++)
                dataEnd -= if (paddingByte == 255) 254 else paddingByte
                require(cursor <= dataEnd) { "Excess Opus padding" }
            } while (paddingByte == 255)
        }
        fun size(): Int {
            require(cursor < dataEnd) { "Truncated Opus length" }
            val first = byte(cursor++)
            if (first < 252) return first
            require(cursor < dataEnd) { "Truncated Opus length" }
            return first + 4 * byte(cursor++)
        }
        val vbr = code == 2 || (code == 3 && flags and 128 != 0)
        if (vbr) {
            var used = 0
            repeat(count - 1) {
                used += size()
                require(used <= dataEnd - cursor) { "Excess Opus frame length" }
            }
            require(dataEnd - cursor - used in 0..1275) { "Invalid final Opus frame" }
        } else {
            val remaining = dataEnd - cursor
            require(remaining >= 0 && remaining % count == 0 && remaining / count <= 1275) { "Invalid CBR Opus framing" }
        }
    }
}
