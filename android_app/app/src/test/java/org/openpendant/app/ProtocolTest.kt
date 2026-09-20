package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.util.zip.CRC32
import kotlin.math.abs

class ProtocolTest {
    private fun rejected(action: () -> Unit) {
        try { action(); fail("Invalid protocol input accepted") } catch (_: ProtocolException) { }
    }

    private fun response(command: Int, sequence: Int, body: ByteArray, status: Int = 0): ByteArray {
        val result = ByteArray(9 + body.size)
        result[0] = 79; result[1] = 80; result[2] = 1; result[3] = (command or 128).toByte()
        result[4] = sequence.toByte(); result[5] = (sequence shr 8).toByte()
        result[6] = (body.size + 1).toByte(); result[7] = ((body.size + 1) shr 8).toByte()
        result[8] = status.toByte(); body.copyInto(result, 9)
        return result
    }

    private fun status(id: Long = 7, state: Int = ClipState.READY, total: Int = 640,
                       crc: Long = 123, remaining: Int = total, error: Int = 0): ByteArray = ByteArray(19).also {
        OpProtocol.put32(it, 0, id); it[4] = state.toByte(); OpProtocol.put32(it, 5, total.toLong())
        OpProtocol.put32(it, 9, crc); OpProtocol.put32(it, 13, remaining.toLong())
        it[17] = error.toByte(); it[18] = (error shr 8).toByte()
    }

    private fun pcm(samples: Int, value: (Int) -> Int = { (it % 65536) - 32768 }) = ByteArray(samples * 2).also {
        repeat(samples) { index -> val v = value(index); it[index * 2] = v.toByte(); it[index * 2 + 1] = (v shr 8).toByte() }
    }

    private fun metadata(pcm: ByteArray): ClipStatus = ClipStatus.parse(status(total = pcm.size,
        crc = CRC32().also { it.update(pcm) }.value), 7)

    private fun chunk(id: Long, offset: Int, source: ByteArray, length: Int = minOf(64, source.size - offset)) =
        ByteArray(8 + length).also { OpProtocol.put32(it, 0, id); OpProtocol.put32(it, 4, offset.toLong()); source.copyInto(it, 8, offset, offset + length) }

    @Test fun exactRequestFramesAndLengthGuards() {
        assertArrayEquals(byteArrayOf(79, 80, 1, 16, 0x34, 0x12, 0, 0), OpProtocol.encode(OpProtocol.BEGIN, 0x1234))
        assertArrayEquals(byteArrayOf(7, 0, 0, 0), OpProtocol.idPayload(7))
        assertArrayEquals(byteArrayOf(7, 0, 0, 0, 64, 0, 0, 0), OpProtocol.chunkPayload(7, 64))
        for (seq in listOf(-1, 0, 65536)) rejected { OpProtocol.encode(OpProtocol.PING, seq) }
        for (cmd in listOf(0, 3, 4, 15, 20, 255)) rejected { OpProtocol.encode(cmd, 1) }
        rejected { OpProtocol.encode(OpProtocol.PING, 1, ByteArray(11)) }
        rejected { OpProtocol.encode(OpProtocol.INFO, 1, byteArrayOf(0)) }
        rejected { OpProtocol.encode(OpProtocol.BEGIN, 1, byteArrayOf(0)) }
        for (size in listOf(0, 1, 3, 5, 8)) rejected { OpProtocol.encode(OpProtocol.STATUS, 1, ByteArray(size)) }
        for (size in listOf(0, 1, 4, 7, 9)) rejected { OpProtocol.encode(OpProtocol.CHUNK, 1, ByteArray(size)) }
        rejected { OpProtocol.chunkPayload(0, 0) }; rejected { OpProtocol.chunkPayload(7, -1) }
        rejected { OpProtocol.chunkPayload(7, 32000) }
    }

    @Test fun responseFramingFailsClosed() {
        val packet = response(OpProtocol.PING, 65535, "test".toByteArray())
        assertArrayEquals("test".toByteArray(), OpProtocol.response(packet, OpProtocol.PING, 65535))
        for (size in 0 until packet.size) rejected { OpProtocol.response(packet.copyOf(size), OpProtocol.PING, 65535) }
        rejected { OpProtocol.response(packet + byteArrayOf(0), OpProtocol.PING, 65535) }
        for (index in listOf(0, 1, 2, 3, 4, 5, 6, 7)) {
            val changed = packet.copyOf(); changed[index] = (changed[index].toInt() xor 1).toByte()
            rejected { OpProtocol.response(changed, OpProtocol.PING, 65535) }
        }
        for (value in 1..255) rejected { OpProtocol.response(response(OpProtocol.PING, 1, byteArrayOf(), value), OpProtocol.PING, 1) }
        rejected { OpProtocol.response(ByteArray(82), OpProtocol.CHUNK, 1) }
    }

    @Test fun capabilityAndIdleStateGuards() {
        val info = byteArrayOf(0, 1, 15, 0, 0, 0, 0, 0)
        OpProtocol.validateInfo(info)
        for (size in 0..7) rejected { OpProtocol.validateInfo(info.copyOf(size)) }
        rejected { OpProtocol.validateInfo(info + byteArrayOf(0)) }
        for (index in info.indices) {
            val changed = info.copyOf(); changed[index] = (changed[index].toInt() xor 1).toByte()
            rejected { OpProtocol.validateInfo(changed) }
        }
        OpProtocol.validateInfo(byteArrayOf(0, 1, 31, 0, 0, 0, 0, 0))
        for (caps in listOf(0, 3, 7, 11, 47, 63)) rejected { OpProtocol.validateInfo(byteArrayOf(0, 1, caps.toByte(), 0, 0, 0, 0, 0)) }
    }

    @Test fun bothGattCallbacksRequiredInEitherOrder() {
        val packet = response(OpProtocol.PING, 4, byteArrayOf(1, 2))
        for (notificationFirst in listOf(true, false)) {
            val gate = ResponseGate(OpProtocol.PING, 4)
            if (notificationFirst) gate.notified(packet) else gate.written(true)
            assertFalse(gate.ready)
            rejected { gate.take() }
            if (notificationFirst) gate.written(true) else gate.notified(packet)
            assertTrue(gate.ready)
            assertArrayEquals(byteArrayOf(1, 2), gate.take())
            assertFalse(gate.ready)
            rejected { gate.take() }; rejected { gate.written(true) }; rejected { gate.notified(packet) }
        }
        val duplicateWrite = ResponseGate(OpProtocol.PING, 4)
        duplicateWrite.written(true); rejected { duplicateWrite.written(true) }
        val duplicateNotify = ResponseGate(OpProtocol.PING, 4)
        duplicateNotify.notified(packet); rejected { duplicateNotify.notified(packet) }
        val failedWrite = ResponseGate(OpProtocol.PING, 4)
        rejected { failedWrite.written(false) }
        val stale = ResponseGate(OpProtocol.PING, 5)
        rejected { stale.notified(packet) }
        val cancelled = ResponseGate(OpProtocol.PING, 4)
        cancelled.notified(packet); cancelled.discard(); rejected { cancelled.written(true) }
    }

    @Test fun beginAndAsynchronousCancel() {
        val body = OpProtocol.idPayload(7) + byteArrayOf(ClipState.WAITING.toByte())
        assertEquals(7L, OpProtocol.begin(body))
        for (state in listOf(0, 2, 3, 4, 5, 6, 7, 8)) rejected { OpProtocol.begin(OpProtocol.idPayload(7) + byteArrayOf(state.toByte())) }
        rejected { OpProtocol.begin(OpProtocol.idPayload(0) + byteArrayOf(1)) }
        rejected { OpProtocol.begin(body.copyOf(4)) }; rejected { OpProtocol.begin(body + byteArrayOf(0)) }
        for (state in listOf(ClipState.WAITING, ClipState.RECORDING, ClipState.DRAINED,
            ClipState.EXPIRED, ClipState.CANCELLED, ClipState.ERROR))
            assertEquals(state, OpProtocol.cancellation(OpProtocol.idPayload(7) + byteArrayOf(state.toByte()), 7))
        for (state in listOf(0, 3, 8)) rejected { OpProtocol.cancellation(OpProtocol.idPayload(7) + byteArrayOf(state.toByte()), 7) }
        rejected { OpProtocol.cancellation(OpProtocol.idPayload(8) + byteArrayOf(6), 7) }
    }

    @Test fun strictStatusStateMetadataAndErrors() {
        for (state in 0..7) {
            val total = if (state in 0..2) 0 else 640
            val remaining = if (state == ClipState.READY) 640 else 0
            val error = if (state in listOf(ClipState.ERROR, ClipState.EXPIRED)) -5 else 0
            val parsed = ClipStatus.parse(status(state = state, total = total, remaining = remaining,
                crc = if (total == 0) 0 else 123, error = error), 7)
            assertEquals(state, parsed.state)
        }
        for (size in 0..18) rejected { ClipStatus.parse(status().copyOf(size), 7) }
        rejected { ClipStatus.parse(status() + byteArrayOf(0), 7) }
        rejected { ClipStatus.parse(status(id = 8), 7) }; rejected { ClipStatus.parse(status(id = 0), 0) }
        for (state in listOf(8, 255)) rejected { ClipStatus.parse(status(state = state), 7) }
        for (size in listOf(0, 1, 2, 639, 641, 32001, 32640)) rejected { ClipStatus.parse(status(total = size), 7) }
        rejected { ClipStatus.parse(status(total = 640, remaining = 642), 7) }
        rejected { ClipStatus.parse(status(total = 640, remaining = 63), 7) }
        rejected { ClipStatus.parse(status(state = ClipState.DRAINED, remaining = 64), 7) }
        for (state in 0..2) rejected { ClipStatus.parse(status(state = state), 7) }
        rejected { ClipStatus.parse(status(error = -5), 7) }
        rejected { ClipStatus.parse(status(state = ClipState.ERROR, error = 0, remaining = 0), 7) }
        rejected { ClipStatus.parse(status(state = ClipState.EXPIRED, error = 0, remaining = 0), 7) }
        rejected { ClipStatus.parse(status(state = ClipState.CANCELLED, error = -1, remaining = 0), 7) }
    }

    @Test fun verifiedTransfersAtBoundsAndPartialFinalWindow() {
        for (samples in listOf(320, 8000, 15680, 16000)) {
            val source = pcm(samples)
            val meta = metadata(source)
            val assembler = ClipAssembler(meta)
            for (offset in source.indices step 64) assembler.accept(chunk(7, offset, source))
            assertEquals(source.size / 64, assembler.chunks)
            val final = meta.copy(state = ClipState.DRAINED, remaining = 0)
            assertArrayEquals(source, assembler.finish(final))
            rejected { assembler.finish(final) }; rejected { assembler.accept(chunk(7, 0, source)) }
        }
        assertEquals(0xcbf43926L, CRC32().also { it.update("123456789".toByteArray()) }.value)
    }

    @Test fun chunksRejectStaleIdsOffsetsLengthsAndExtraData() {
        val source = pcm(320)
        val meta = metadata(source)
        val packet = chunk(7, 0, source)
        for (size in 0 until packet.size) rejected { ClipAssembler(meta).accept(packet.copyOf(size)) }
        rejected { ClipAssembler(meta).accept(packet + byteArrayOf(0)) }
        rejected { ClipAssembler(meta).accept(chunk(8, 0, source)) }
        rejected { ClipAssembler(meta).accept(chunk(7, 64, source)) }
        val duplicate = ClipAssembler(meta)
        duplicate.accept(packet); rejected { duplicate.accept(packet) }
        val discarded = ClipAssembler(meta)
        discarded.accept(packet); discarded.discard(); rejected { discarded.accept(chunk(7, 64, source)) }
        rejected { ClipAssembler(meta.copy(remaining = 576)) }
        rejected { ClipAssembler(meta.copy(state = ClipState.DRAINED)) }
        rejected { ClipAssembler(meta.copy(total = 0, remaining = 0)) }
        rejected { ClipAssembler(meta.copy(total = 32002, remaining = 32002)) }
    }

    @Test fun finalChecksumAndCleanupMustAllMatch() {
        val source = pcm(320)
        val meta = metadata(source)
        val final = meta.copy(state = ClipState.DRAINED, remaining = 0)
        fun full(): ClipAssembler = ClipAssembler(meta).also { a -> for (offset in source.indices step 64) a.accept(chunk(7, offset, source)) }
        for (changed in listOf(final.copy(id = 8), final.copy(total = 1280), final.copy(crc = 0),
            final.copy(remaining = 64), final.copy(state = ClipState.READY), final.copy(error = -5)))
            rejected { full().finish(changed) }
        rejected { ClipAssembler(meta).finish(final) }
        val wrong = ClipAssembler(meta)
        val mutated = source.copyOf(); mutated[0] = (mutated[0].toInt() xor 1).toByte()
        for (offset in mutated.indices step 64) wrong.accept(chunk(7, offset, mutated))
        rejected { wrong.finish(final) }; wrong.discard()
    }

    @Test fun wavRoundTripAndStrictHeader() {
        val source = pcm(16000)
        val wav = WavCodec.encode(source)
        assertEquals(32044, wav.size)
        assertArrayEquals(source, WavCodec.decode(wav))
        for (index in listOf(0, 4, 8, 12, 16, 20, 22, 24, 28, 32, 34, 36, 40)) {
            val changed = wav.copyOf(); changed[index] = (changed[index].toInt() xor 1).toByte()
            rejected { WavCodec.decode(changed) }
        }
        for (size in listOf(0, 2, 638, 642, 32002)) rejected { WavCodec.encode(ByteArray(size)) }
        rejected { WavCodec.decode(wav.copyOf(wav.size - 1)) }
        rejected { WavCodec.decode(wav + byteArrayOf(0)) }
    }

    @Test fun previewDoesNotAlterRawAndSuppressesConstantDc() {
        for (constant in listOf(-32768, -1000, 0, 1000, 32767)) {
            val source = pcm(320) { constant }
            val copy = source.copyOf()
            assertTrue(WavCodec.preview(source).all { it == 0.toByte() })
            assertArrayEquals(copy, source)
        }
        val source = pcm(16000) { if (it % 2 == 0) -32768 else 32767 }
        val copy = source.copyOf()
        val preview = WavCodec.preview(source)
        assertArrayEquals(copy, source)
        var peak = 0
        for (offset in preview.indices step 2) peak = maxOf(peak, abs(OpProtocol.u16(preview, offset).toShort().toInt()))
        assertTrue(peak in 22936..22937)
        assertEquals(source.size, preview.size)
    }

    @Test fun repositoryRetainsOnlyExplicitVerifiedClipAndNoOverwrite() {
        val directory = Files.createTempDirectory("openpendant-unit-").toFile()
        try {
            val repo = ClipRepository(directory)
            val source = pcm(320)
            assertFalse(repo.exists())
            repo.save(source)
            assertTrue(repo.exists()); assertEquals(684L, repo.bytes())
            assertArrayEquals(source, repo.readPcm())
            rejected { repo.save(source) }
            assertFalse(File(directory, "pending_verified_clip.wav").exists())
            repo.delete(); assertFalse(repo.exists())
            rejected { repo.readPcm() }
            File(directory, "pending_verified_clip.wav").writeBytes(byteArrayOf(1, 2, 3))
            val reopened = ClipRepository(directory)
            assertFalse(File(directory, "pending_verified_clip.wav").exists())
            reopened.save(source)
            val again = ClipRepository(directory)
            assertTrue(again.exists()); assertArrayEquals(source, again.readPcm())
            again.delete()
        } finally { directory.listFiles()?.forEach { it.delete() }; directory.delete() }
    }
}
