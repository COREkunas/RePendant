package org.openpendant.app

import java.util.UUID
import java.util.zip.CRC32

class ProtocolException(message: String) : Exception(message)

internal fun ensure(condition: Boolean, message: String) {
    if (!condition) throw ProtocolException(message)
}

object OpProtocol {
    val SERVICE: UUID = UUID.fromString("632de001-604c-446b-a80f-7963e950f3fb")
    val WRITE: UUID = UUID.fromString("632de002-604c-446b-a80f-7963e950f3fb")
    val NOTIFY: UUID = UUID.fromString("632de003-604c-446b-a80f-7963e950f3fb")
    val CCC: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    const val PING = 1
    const val INFO = 2
    const val BEGIN = 0x10
    const val STATUS = 0x11
    const val CHUNK = 0x12
    const val CANCEL = 0x13
    const val DEVICE_STATUS = 0x20
    const val GET_PREFERENCES = 0x21
    const val SET_PREFERENCES = 0x22
    const val MAX_BYTES = 32000
    const val MIN_MTU = 96
    private val commands = setOf(PING, INFO, BEGIN, STATUS, CHUNK, CANCEL, DEVICE_STATUS, GET_PREFERENCES, SET_PREFERENCES)

    fun u16(data: ByteArray, offset: Int): Int {
        ensure(offset >= 0 && offset + 2 <= data.size, "Truncated 16-bit field")
        return (data[offset].toInt() and 255) or ((data[offset + 1].toInt() and 255) shl 8)
    }

    fun u32(data: ByteArray, offset: Int): Long {
        ensure(offset >= 0 && offset + 4 <= data.size, "Truncated 32-bit field")
        var value = 0L
        repeat(4) { value = value or ((data[offset + it].toLong() and 255) shl (it * 8)) }
        return value
    }

    fun put32(data: ByteArray, offset: Int, value: Long) {
        ensure(value in 0..0xffffffffL && offset >= 0 && offset + 4 <= data.size, "Invalid 32-bit field")
        repeat(4) { data[offset + it] = (value shr (it * 8)).toByte() }
    }

    fun idPayload(id: Long): ByteArray = ByteArray(4).also { put32(it, 0, id) }
    fun chunkPayload(id: Long, offset: Int): ByteArray = ByteArray(8).also {
        ensure(id in 1..0xffffffffL && offset in 0 until MAX_BYTES, "Invalid clip request")
        put32(it, 0, id); put32(it, 4, offset.toLong())
    }

    fun encode(command: Int, sequence: Int, payload: ByteArray = byteArrayOf()): ByteArray {
        ensure(command in commands && sequence in 1..65535, "Invalid command or sequence")
        val validLength = when (command) {
            PING -> payload.size <= 10
            INFO, BEGIN, DEVICE_STATUS, GET_PREFERENCES -> payload.isEmpty()
            SET_PREFERENCES -> runCatching { DevicePreferences.decode(payload) }.isSuccess
            STATUS, CANCEL -> payload.size == 4
            CHUNK -> payload.size == 8
            else -> false
        }
        ensure(validLength, "Invalid request length")
        return ByteArray(8 + payload.size).also {
            it[0] = 'O'.code.toByte(); it[1] = 'P'.code.toByte(); it[2] = 1
            it[3] = command.toByte(); it[4] = sequence.toByte(); it[5] = (sequence shr 8).toByte()
            it[6] = payload.size.toByte(); it[7] = (payload.size shr 8).toByte()
            payload.copyInto(it, 8)
        }
    }

    fun response(packet: ByteArray, command: Int, sequence: Int): ByteArray {
        ensure(packet.size in 9..(9 + DurableBleCodec.maxResponseBody(command)), "Invalid response packet size")
        ensure(packet[0] == 'O'.code.toByte() && packet[1] == 'P'.code.toByte(), "Unexpected device protocol")
        ensure(packet[2].toInt() == 1, "Unsupported protocol version")
        ensure((packet[3].toInt() and 255) == (command or 0x80), "Unexpected response command")
        ensure(u16(packet, 4) == sequence, "Stale or out-of-order response sequence")
        ensure(u16(packet, 6) == packet.size - 8, "Truncated or trailing response data")
        val status = packet[8].toInt() and 255
        ensure(status in 0..10, "Unknown device status")
        if (status != 0) throw ProtocolException(when (status) {
            5 -> "Pendant hardware operation failed"
            6 -> "Pendant is busy; no automatic retry"
            7 -> "Permission denied. Pair securely and confirm on the pendant"
            8 -> "Clip state changed or its retention expired"
            9 -> "Clip offset was rejected; partial audio discarded"
            10 -> "Bluetooth packet size is too small"
            else -> "Pendant rejected the protocol request"
        })
        return packet.copyOfRange(9, packet.size)
    }

    fun validateInfo(body: ByteArray) {
        ensure(body.size == 8 && body[0].toInt() == 0 && body[1].toInt() == 1, "Unsupported pendant hardware/protocol information")
        ensure(u32(body, 2) in listOf(15L, 31L), "Unsupported firmware capabilities")
        ensure(body[6].toInt() == 0 && body[7].toInt() == 0, "Initial microphone/NAND state is not safe")
    }

    fun begin(body: ByteArray): Long {
        ensure(body.size == 5, "Malformed recording request acknowledgement")
        val id = u32(body, 0)
        ensure(id != 0L && body[4].toInt() == ClipState.WAITING, "Recording did not enter physical-confirmation waiting state")
        return id
    }

    fun cancellation(body: ByteArray, id: Long): Int {
        ensure(body.size == 5 && u32(body, 0) == id,
            "Cancellation acknowledgement is malformed or stale")
        val state = body[4].toInt()
        ensure(state in listOf(ClipState.WAITING, ClipState.RECORDING, ClipState.DRAINED,
            ClipState.EXPIRED, ClipState.CANCELLED, ClipState.ERROR),
            "Unexpected cancellation acknowledgement state")
        return state
    }
}

object ClipState {
    const val IDLE = 0; const val WAITING = 1; const val RECORDING = 2; const val READY = 3
    const val DRAINED = 4; const val EXPIRED = 5; const val CANCELLED = 6; const val ERROR = 7
}

data class ClipStatus(val id: Long, val state: Int, val total: Int, val crc: Long, val remaining: Int, val error: Int) {
    companion object {
        fun parse(body: ByteArray, expectedId: Long): ClipStatus {
            ensure(body.size == 19, "Malformed clip status length")
            val id = OpProtocol.u32(body, 0)
            val state = body[4].toInt() and 255
            val total = OpProtocol.u32(body, 5)
            val crc = OpProtocol.u32(body, 9)
            val remaining = OpProtocol.u32(body, 13)
            val error = OpProtocol.u16(body, 17).toShort().toInt()
            ensure(id == expectedId && id > 0, "Stale or unexpected clip identifier")
            ensure(state in 0..7, "Unknown clip state")
            ensure(total <= OpProtocol.MAX_BYTES && total % 640 == 0L && remaining <= total && remaining % 2 == 0L,
                "Invalid bounded PCM counts")
            ensure(if (state == ClipState.ERROR || state == ClipState.EXPIRED) error < 0 else error == 0,
                "Unexpected clip error status")
            when (state) {
                ClipState.IDLE, ClipState.WAITING, ClipState.RECORDING ->
                    ensure(total == 0L && crc == 0L && remaining == 0L, "Audio metadata appeared before capture completion")
                ClipState.READY -> ensure(total > 0 && remaining > 0, "Ready clip is empty")
                ClipState.DRAINED -> ensure(total > 0 && remaining == 0L, "Drained clip still contains retained bytes")
                ClipState.EXPIRED, ClipState.CANCELLED, ClipState.ERROR -> ensure(remaining == 0L, "Aborted clip retains readable bytes")
            }
            return ClipStatus(id, state, total.toInt(), crc, remaining.toInt(), error)
        }
    }
}

class ClipAssembler(val metadata: ClipStatus) {
    private var buffer: ByteArray? = null
    var received: Int = 0
        private set
    var chunks: Int = 0
        private set

    init {
        ensure(metadata.state == ClipState.READY && metadata.total in 640..OpProtocol.MAX_BYTES &&
            metadata.total % 640 == 0 && metadata.remaining == metadata.total && metadata.error == 0,
            "Clip transfer did not start with complete fixed metadata")
        buffer = ByteArray(metadata.total)
    }

    fun accept(body: ByteArray) {
        val target = buffer ?: throw ProtocolException("Clip transfer is no longer active")
        val expected = minOf(64, target.size - received)
        ensure(expected > 0 && body.size == 8 + expected, "Missing, short, or excessive PCM chunk")
        ensure(OpProtocol.u32(body, 0) == metadata.id && OpProtocol.u32(body, 4) == received.toLong(),
            "Stale clip or out-of-order PCM chunk")
        ensure(chunks < 500, "PCM chunk count exceeds fixed transfer bound")
        body.copyInto(target, received, 8)
        received += expected
        chunks++
    }

    fun finish(status: ClipStatus): ByteArray {
        val target = buffer ?: throw ProtocolException("Clip transfer is no longer active")
        ensure(received == target.size && status.id == metadata.id && status.state == ClipState.DRAINED &&
            status.remaining == 0 && status.total == metadata.total && status.crc == metadata.crc && status.error == 0,
            "Final complete-transfer/retained-store cleanup was not confirmed")
        val crc = CRC32().also { it.update(target) }.value
        ensure(crc == metadata.crc, "Recording checksum failed; partial audio discarded")
        buffer = null
        return target
    }

    fun discard() { buffer?.fill(0); buffer = null }
}

/** Both callbacks are required before a subsequent GATT operation is allowed. */
class ResponseGate(private val command: Int, private val sequence: Int,
    private val stream: DurableBleStreamAssembler? = null) {
    init { require((command == DurableBleCodec.FULL_STREAM) == (stream != null)) }
    private var written = false
    private var response: ByteArray? = null
    private var consumed = false
    val ready get() = !consumed && written && response != null

    fun written(success: Boolean) {
        ensure(!consumed && !written && success, "Control write failed or completed twice")
        written = true
    }

    fun notified(packet: ByteArray) {
        ensure(!consumed && response == null, "Duplicate or unsolicited response")
        val body = OpProtocol.response(packet, command, sequence)
        if (stream == null) response = body
        else try { response = stream.accept(body) } finally { body.fill(0) }
    }

    fun take(): ByteArray {
        ensure(ready, "Response received before both GATT completions")
        val result = response!!
        response = null; consumed = true
        return result
    }

    fun discard() { stream?.close(); response?.fill(0); response = null; consumed = true }
}
