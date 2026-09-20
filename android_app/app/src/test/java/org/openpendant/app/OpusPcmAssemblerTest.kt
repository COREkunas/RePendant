package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CancellationException

class OpusPcmAssemblerTest {
    private fun fixture(): ByteArray {
        val hex = javaClass.getResourceAsStream("/recording_pipeline_public.hex")!!.use { it.readBytes().toString(Charsets.US_ASCII).trim() }
        return ByteArray(hex.length / 2) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    }
    private fun put(out: ByteArray, at: Int, value: Long, count: Int = 4) {
        repeat(count) { out[at + it] = (value ushr (it * 8)).toByte() }
    }
    private fun sized(frames: Int, delay: Int): ByteArray {
        val original = fixture()
        val count = frames + if (delay > 0) 1 else 0
        val result = ByteArray(80 + count * 68)
        original.copyInto(result, 0, 0, 80)
        put(result, 32, 0, 8); put(result, 40, frames * 320L, 8)
        put(result, 48, frames.toLong()); put(result, 52, count.toLong())
        put(result, 56, delay.toLong()); put(result, 60, if (delay > 0) 320L - delay else 0)
        put(result, 64, count * 68L); put(result, 68, frames * 320L)
        repeat(count) { i ->
            val at = 80 + i * 68
            put(result, at, 60, 2); put(result, at + 2, if (i >= frames) 1 else 0, 2)
            put(result, at + 4, i.toLong()); original.copyInto(result, at + 8, 88, 148)
        }
        original.fill(0)
        return result
    }
    private fun u32(bytes: ByteArray, at: Int) = (0 until 4).fold(0L) { v, i ->
        v or ((bytes[at + i].toLong() and 255) shl (i * 8))
    }
    private fun word(bytes: ByteArray, at: Int) = ((bytes[at].toInt() and 255) or
        ((bytes[at + 1].toInt() and 255) shl 8)).toShort()

    @Test fun trimsCodecDelayAndProducesExactWavWithoutChangingSource() {
        val source = fixture(); val before = source.copyOf()
        lateinit var borrowed: ByteArray; lateinit var native: ShortArray
        val decoder = OpusPacketDecoder { input ->
            borrowed = input; native = ShortArray(1280) { it.toShort() }; native
        }
        OpusPcmAssembler.decodeAuthenticated(source, 7, decoder).use { pcm ->
            assertEquals(7, pcm.sequence); assertEquals(960, pcm.samples)
            val bytes = pcm.copyPcm16Le(); val wav = pcm.copyWav()
            try {
                assertEquals(1920, bytes.size); assertEquals(1964, wav.size)
                repeat(960) { assertEquals((it + 104).toShort(), word(bytes, it * 2)) }
                assertArrayEquals(bytes, wav.copyOfRange(44, wav.size))
                assertEquals("RIFF", String(wav, 0, 4, Charsets.US_ASCII))
                assertEquals("WAVEfmt ", String(wav, 8, 8, Charsets.US_ASCII))
                assertEquals(1956L, u32(wav, 4)); assertEquals(16000L, u32(wav, 24))
                assertEquals(32000L, u32(wav, 28)); assertEquals(1920L, u32(wav, 40))
                assertEquals("PcmSegment[redacted]", pcm.toString())
            } finally { bytes.fill(0); wav.fill(0) }
        }
        assertTrue(borrowed.all { it == 0.toByte() }); assertTrue(native.all { it == 0.toShort() })
        assertArrayEquals(before, source)
    }
    @Test fun allTrimBoundariesAndMaximumSizeAreBounded() {
        for (frames in listOf(1, 3, 499, 500)) for (delay in listOf(0, 1, 104, 319, 320)) {
            val source = sized(frames, delay)
            OpusPcmAssembler.decodeAuthenticated(source, 0, OpusPacketDecoder { ShortArray(it.size / 60 * 320) }).use { pcm ->
                assertEquals(frames * 320, pcm.samples)
                val wav = pcm.copyWav()
                assertEquals(44 + frames * 640, wav.size); assertTrue(wav.size <= OpusPcmAssembler.MAX_WAV_BYTES)
                wav.fill(0)
            }
        }
    }
    @Test fun closedOwnerCannotProduceCopiesAndWipesRetainedPcm() {
        val owned = ShortArray(320) { 123 }; val pcm = OpusPcmAssembler.PcmSegment(0, owned)
        pcm.close(); pcm.close()
        assertTrue(owned.all { it == 0.toShort() })
        assertThrows(IllegalStateException::class.java) { pcm.copyWav() }
        assertThrows(IllegalStateException::class.java) { pcm.copyPcm16Le() }
        assertThrows(IllegalStateException::class.java) { pcm.samples }
    }
    @Test fun malformedFramingNeverCallsDecoder() {
        var calls = 0
        val decoder = OpusPacketDecoder { calls++; ShortArray(0) }
        for (at in listOf(0, 8, 24, 48, 52, 56, 60, 72, 80, 82, 84, 88)) {
            val bad = fixture(); bad[at] = (bad[at].toInt() xor 0x80).toByte()
            assertThrows(IllegalArgumentException::class.java) { OpusPcmAssembler.decodeAuthenticated(bad, 0, decoder) }
        }
        assertEquals(0, calls)
    }
    @Test fun badSequenceAndOversizeNeverCallDecoder() {
        val decoder = OpusPacketDecoder { error("must not decode") }
        assertThrows(IllegalArgumentException::class.java) { OpusPcmAssembler.decodeAuthenticated(fixture(), -1, decoder) }
        assertThrows(IllegalArgumentException::class.java) { OpusPcmAssembler.decodeAuthenticated(ByteArray(OpusSegment.MAX_BYTES + 1), 0, decoder) }
    }
    @Test fun wrongDecoderSampleCountWipesAllNativeOutput() {
        for (size in listOf(0, 1279, 1281)) {
            val output = ShortArray(size) { 123 }
            assertThrows(IllegalArgumentException::class.java) {
                OpusPcmAssembler.decodeAuthenticated(fixture(), 0, OpusPacketDecoder { output })
            }
            assertTrue(output.all { it == 0.toShort() })
        }
    }
    @Test fun providerFailureWipesPacketCopy() {
        lateinit var packets: ByteArray
        assertThrows(IllegalStateException::class.java) {
            OpusPcmAssembler.decodeAuthenticated(fixture(), 0, OpusPacketDecoder { packets = it; error("fixed test failure") })
        }
        assertTrue(packets.all { it == 0.toByte() })
    }
    @Test fun initialCancellationDoesNotInvokeDecoder() {
        var calls = 0
        assertThrows(CancellationException::class.java) {
            OpusPcmAssembler.decodeAuthenticated(fixture(), 0, OpusPacketDecoder { calls++; ShortArray(0) }) { true }
        }
        assertEquals(0, calls)
    }
    @Test fun cancellationDuringNativeCallDoesNotReturnPcm() {
        var cancelled = false; lateinit var packets: ByteArray
        val output = ShortArray(1280) { 123 }
        assertThrows(CancellationException::class.java) {
            OpusPcmAssembler.decodeAuthenticated(fixture(), 0, OpusPacketDecoder {
                packets = it; cancelled = true; output
            }) { cancelled }
        }
        assertTrue(packets.all { it == 0.toByte() }); assertTrue(output.all { it == 0.toShort() })
    }
    @Test fun failureInCancellationObserverStillWipesNativeOutput() {
        var after = false; lateinit var packets: ByteArray
        val output = ShortArray(1280) { 123 }
        assertThrows(AssertionError::class.java) {
            OpusPcmAssembler.decodeAuthenticated(fixture(), 0, OpusPacketDecoder {
                packets = it; after = true; output
            }) { if (after) throw AssertionError("fixed test observer"); false }
        }
        assertTrue(packets.all { it == 0.toByte() }); assertTrue(output.all { it == 0.toShort() })
    }
    @Test fun successiveSegmentsCallFreshDecoderBoundary() {
        var calls = 0
        val decoder = OpusPacketDecoder { calls++; ShortArray(it.size / 60 * 320) }
        repeat(2) { sequence -> OpusPcmAssembler.decodeAuthenticated(fixture(), sequence, decoder).close() }
        assertEquals(2, calls)
    }
}
