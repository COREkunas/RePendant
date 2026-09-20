package org.openpendant.app

import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Test

class OpusSegmentTest {
    private fun hexResource(name: String): ByteArray {
        val hex = requireNotNull(javaClass.getResourceAsStream(name))
            .bufferedReader(Charsets.US_ASCII).use { it.readText().trim() }
        return hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    }
    private fun fixture(): ByteArray {
        return hexResource("/recording_pipeline_public.hex")
    }
    private fun put(bytes: ByteArray, at: Int, value: ULong, count: Int = 4) {
        repeat(count) { bytes[at + it] = (value shr (it * 8)).toByte() }
    }
    private fun reject(bytes: ByteArray, sequence: Int = 0) {
        assertThrows(IllegalArgumentException::class.java) { OpusSegment.parse(bytes, sequence) }
    }
    private fun segment(frames: Int, delay: Int = 104, first: ULong = 0uL, gap: Boolean = false): ByteArray {
        val template = fixture()
        val count = frames + if (delay > 0) 1 else 0
        val out = template.copyOf(80 + count * 68)
        put(out, 28, if (gap) 3uL else 1uL)
        put(out, 32, first, 8); put(out, 40, first + frames.toULong() * 320uL, 8)
        put(out, 48, frames.toULong()); put(out, 52, count.toULong()); put(out, 56, delay.toULong())
        put(out, 60, if (delay > 0) (320 - delay).toULong() else 0uL)
        put(out, 64, (count * 68).toULong()); put(out, 68, (frames * 320).toULong())
        repeat(count) { index ->
            val at = 80 + index * 68
            template.copyInto(out, at, 80, 148)
            put(out, at + 2, if (index >= frames) 1uL else 0uL, 2)
            put(out, at + 4, index.toULong())
        }
        return out
    }

    @Test fun actualCAndPinnedOpusPublicFixture() {
        val bytes = fixture()
        val hash = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
        assertEquals("ecaf916e76ba1d54f0c57df3d02fb8b78e40e11f0da2266018175925e9a35b59", hash)
        assertEquals(352, bytes.size)
        val parsed = OpusSegment.parse(bytes, 0)
        assertEquals(OpusSegment.CodecProfile.LEGACY_VOIP, parsed.codecProfile)
        assertEquals(0uL, parsed.firstSample); assertEquals(960uL, parsed.nextSample)
        assertEquals(3, parsed.capturedFrames); assertEquals(4, parsed.packets.size)
        assertEquals(104, parsed.preSkip); assertEquals(216, parsed.endTrim)
        assertEquals(960, parsed.packets.size * 320 - parsed.preSkip - parsed.endTrim)
        assertEquals(960, parsed.validSamples); assertFalse(parsed.gapBefore)
        parsed.packets.forEachIndexed { index, packet ->
            assertEquals(index, packet.index); assertEquals(88 + index * 68, packet.offset)
            assertEquals(60, packet.length); assertEquals(index == 3, packet.flush)
        }
    }

    @Test fun headerAndRecordMetadataAreStrict() {
        val bytes = fixture()
        repeat(80) { index -> reject(bytes.copyOf().also { it[index] = (it[index].toInt() xor 128).toByte() }) }
        repeat(4) { packet -> repeat(8) { field ->
            reject(bytes.copyOf().also { val index = 80 + packet * 68 + field; it[index] = (it[index].toInt() xor 128).toByte() })
        } }
        for (length in bytes.indices) reject(bytes.copyOf(length))
        reject(bytes + 0.toByte()); reject(ByteArray(OpusSegment.MAX_BYTES + 1)); reject(bytes, -1)
        assertEquals(Int.MAX_VALUE, OpusSegment.parse(bytes, Int.MAX_VALUE).segmentSequence)
    }

    @Test fun boundedFrameCountsAndExactLookaheadAccounting() {
        for (frames in listOf(1, 3, 499, 500)) for (delay in listOf(0, 1, 104, 319, 320)) {
            val bytes = segment(frames, delay)
            val layout = OpusSegment.parse(bytes, 0)
            assertEquals(frames * 320, layout.validSamples)
            assertEquals(layout.validSamples, layout.packets.size * 320 - layout.preSkip - layout.endTrim)
            assertEquals(delay > 0, layout.packets.last().flush)
            assertTrue(bytes.size <= OpusSegment.MAX_BYTES)
        }
        assertEquals(OpusSegment.MAX_BYTES, segment(500).size)
        reject(segment(0)); reject(segment(501)); reject(segment(1, 321))
        for (field in listOf(48, 52, 56, 60, 64, 68)) reject(fixture().also { put(it, field, UInt.MAX_VALUE.toULong()) })
    }

    @Test fun celtProfileHasExplicitControlsAndCannotSilentlyReplaceLegacy() {
        // Framing-only synthetic packet: CELT20ms mono code0. This does not
        // claim successful entropy decode or stand in for the real C fixture.
        fun celt(first: ULong = 0uL) = segment(3, delay = 40, first = first).also {
            put(it, 72, 2uL)
            repeat(4) { index -> it[88 + index * 68] = 0x98.toByte() }
        }
        val parsed = OpusSegment.parse(celt(), 0)
        assertEquals(OpusSegment.CodecProfile.CELT_LOW_DELAY, parsed.codecProfile)
        assertEquals(40, parsed.preSkip); assertEquals(280, parsed.endTrim)
        OpusSegment.requireFollowing(parsed, OpusSegment.parse(celt(960uL), 1))
        for (id in listOf(0uL, 3uL, 255uL, UInt.MAX_VALUE.toULong())) reject(celt().also { put(it, 72, id) })
        for (delay in listOf(0, 39, 41, 104, 320)) reject(celt().also {
            put(it, 56, delay.toULong()); put(it, 60, (320 - delay).toULong())
        })
        for ((field, value) in listOf(12 to 48000uL, 20 to 16000uL, 24 to 1uL)) reject(celt().also { put(it, field, value) })
        repeat(4) { index -> reject(celt().also { it[88 + index * 68] = 0x48 }) }
        // Existing VOIP fixture cannot be relabeled as profile2: its packets
        // and queried delay differ. Profile1 deliberately keeps old framing rules.
        reject(fixture().also { put(it, 72, 2uL) })
        assertThrows(IllegalArgumentException::class.java) {
            OpusSegment.requireFollowing(OpusSegment.parse(fixture(), 0), OpusSegment.parse(celt(960uL), 1))
        }
        assertThrows(IllegalArgumentException::class.java) {
            OpusSegment.requireFollowing(parsed, OpusSegment.parse(segment(1, first = 960uL), 1))
        }
    }

    @Test fun unsignedIntervalsDoNotWrapOrBecomeNegative() {
        val high = Long.MAX_VALUE.toULong() + 100uL
        val parsed = OpusSegment.parse(segment(1, first = high), 0)
        assertEquals(high, parsed.firstSample); assertEquals(high + 320uL, parsed.nextSample)
        val last = ULong.MAX_VALUE - 320uL
        assertEquals(ULong.MAX_VALUE, OpusSegment.parse(segment(1, first = last), 0).nextSample)
        reject(segment(1, first = last + 1uL))
        reject(fixture().also { put(it, 40, 959uL, 8) })
    }

    @Test fun crossSegmentSequenceAndGapAreExplicit() {
        val first = OpusSegment.parse(fixture(), 7)
        val consecutive = OpusSegment.parse(segment(1, first = 960uL), 8)
        OpusSegment.requireFollowing(first, consecutive)
        val gap = OpusSegment.parse(segment(1, first = 6400uL, gap = true), 8)
        OpusSegment.requireFollowing(first, gap)
        for ((sample, hasGap) in listOf(0uL to false, 640uL to false, 1280uL to false, 960uL to true, 0uL to true)) {
            assertThrows(IllegalArgumentException::class.java) {
                OpusSegment.requireFollowing(first, OpusSegment.parse(segment(1, first = sample, gap = hasGap), 8))
            }
        }
        for (sequence in listOf(0, 7, 9, Int.MAX_VALUE)) assertThrows(IllegalArgumentException::class.java) {
            OpusSegment.requireFollowing(first, OpusSegment.parse(segment(1, first = 960uL), sequence))
        }
        assertThrows(IllegalArgumentException::class.java) {
            OpusSegment.requireFollowing(OpusSegment.parse(fixture(), Int.MAX_VALUE), consecutive)
        }
        assertTrue(OpusSegment.parse(segment(1, first = 6400uL, gap = true), 0).gapBefore)
    }

    @Test fun packetTocChannelsDurationAndPaddingAreValidated() {
        reject(fixture().also { it[88] = (it[88].toInt() or 4).toByte() })
        reject(fixture().also { it[88] = 0 }) // 10ms, not20
        reject(fixture().also { it[89] = 0 }) // code3 with zero frames
        reject(fixture().also { it[89] = 2 }) // two20ms frames
        reject(fixture().also { it[89] = 65; it[90] = 59 }) // overlong padding
        reject(fixture().also { it[89] = 65; it[90] = -1 }) // padding continuation exceeds packet
        reject(fixture().also { it[88] = 0x41 }) // two10ms CBR, odd payload59
        reject(fixture().also { it[88] = 0x42; it[89] = 59 }) // two10ms VBR length exceeds remainder58
        reject(fixture().also { it[88] = 0x42; it[89] = -4; it[90] = -1 }) // two-byte length exceeds packet
        reject(fixture().also { it[88] = 0x43; it[89] = -126; it[90] = 58 }) // two10ms VBR exceeds remainder57
        // Framing-only public synthetic packets: valid one20ms, two10ms VBR,
        // and padding-adjusted two10ms CBR. No successful entropy decode claim.
        OpusSegment.parse(fixture().also { it[88] = 0x48 }, 0)
        OpusSegment.parse(fixture().also { it[88] = 0x42; it[89] = 20 }, 0)
        OpusSegment.parse(fixture().also { it[88] = 0x43; it[89] = 66; it[90] = 1 }, 0)
    }

    @Test fun layoutOwnsNoAudioAndDoesNotPretendToAuthenticateIt() {
        val bytes = fixture()
        val parsed = OpusSegment.parse(bytes, 0)
        bytes.fill(0)
        assertEquals(960, parsed.validSamples); assertEquals(4, parsed.packets.size)
        assertThrows(UnsupportedOperationException::class.java) { (parsed.packets as MutableList).clear() }
        // Entropy payload mutation is not a structural error. HPKE tag checking
        // is mandatory BEFORE parse; neither parser nor layout replaces it.
        OpusSegment.parse(fixture().also { it[100] = (it[100].toInt() xor 1).toByte() }, 0)
    }

    @Test fun framingMatches3072CasesFromActualCAndPinnedLibopus() {
        // Regenerated and compared by tools/test_recording_pipeline.py using
        // actual rp_segment_validate + opus_packet_parse, never model output.
        val mask = hexResource("/recording_pipeline_framing_mask.hex")
        assertEquals(4 * 3 * 32, mask.size)
        repeat(4) { packet -> repeat(3) { field -> repeat(256) { candidate ->
            val bytes = fixture()
            bytes[88 + packet * 68 + field] = candidate.toByte()
            val slot = (packet * 3 + field) * 32 + candidate / 8
            val expected = (mask[slot].toInt() and (1 shl (candidate % 8))) != 0
            val accepted = try { OpusSegment.parse(bytes, 0); true } catch (_: IllegalArgumentException) { false }
            assertEquals("packet=$packet field=$field candidate=$candidate", expected, accepted)
        } } }
    }

    @Test fun actualCeltProfileAnd3072CaseFramingOracle() {
        val fixture = hexResource("/recording_pipeline_celt_public.hex")
        val mask = hexResource("/recording_pipeline_celt_framing_mask.hex")
        fun hash(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it.toInt() and 255) }
        assertEquals("317311789976161255648306067876aebdd121c8f332e7f02482161d2ab6d9e2", hash(fixture))
        assertEquals("4e4082b338a5d9c20e71d665a5c86020416478a8284be7999a14e8aaeec012b8", hash(mask))
        val parsed = OpusSegment.parse(fixture, 0)
        assertEquals(OpusSegment.CodecProfile.CELT_LOW_DELAY, parsed.codecProfile)
        assertEquals(40, parsed.preSkip); assertEquals(280, parsed.endTrim)
        assertEquals(3, parsed.capturedFrames); assertEquals(4, parsed.packets.size)
        assertEquals(960, parsed.validSamples)
        repeat(4) { packet -> repeat(3) { field -> repeat(256) { candidate ->
            val changed = fixture.copyOf()
            changed[88 + packet * 68 + field] = candidate.toByte()
            val slot = (packet * 3 + field) * 32 + candidate / 8
            val expected = (mask[slot].toInt() and (1 shl (candidate % 8))) != 0
            val accepted = try { OpusSegment.parse(changed, 0); true } catch (_: IllegalArgumentException) { false }
            assertEquals("CELT packet=$packet field=$field candidate=$candidate", expected, accepted)
        } } }
    }
}
