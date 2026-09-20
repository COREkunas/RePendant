package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID

/** Bit6/0x40..42 grammar. A valid packet alone does not authorize capture. */
internal object LongRecordingControlCodec {
    const val CAPABILITY = 1L shl 6
    const val STANDALONE_CAPABILITY = 1L shl 13
    const val STATUS = 0x40
    const val START = 0x41
    const val STOP = 0x42
    const val USB = 1
    const val MIC = 2
    const val PRODUCER_JOINED = 4
    const val STORAGE_JOINED = 8
    const val FINALIZED = 16
    const val RELEASED = 32
    const val CREATED = 64
    const val FAULT = 128
    const val CAPACITY = 256
    const val STOP_REQUESTED = 512
    const val BUTTON_ARMED = 1024
    const val PORTABLE = 2048
    const val MAX_FRAMES = 5120 * 500
    enum class Phase { IDLE, STARTING, RUNNING, STOPPING, DRAINING, STOPPED, NO_CAPACITY, CANCELLED_BEFORE_START, FAULT }
    data class State(val boot: UUID, val operation: UUID?, val recording: UUID?, val phase: Phase,
        val reason: Int, val flags: Int, val epoch: Int, val accepted: Int, val committed: Int, val admitted: Int) {
        val terminal get() = phase.ordinal >= Phase.STOPPED.ordinal
        fun has(flag: Int) = flags and flag != 0
    }
    class Request internal constructor(val command: Int, val sequence: Int, val boot: UUID?, val operation: UUID?,
        private val bytes: ByteArray) {
        fun frame() = bytes.copyOf()
        val waitForButton get() = command==START && bytes[72]==2.toByte()
    }
    fun bindingDigest(binding: DurablePublicBinding): ByteArray {
        val data = ByteBuffer.allocate(16 + 16 + 8 + 32).order(ByteOrder.LITTLE_ENDIAN)
        data.put(uuid(binding.volume.deviceId)); data.put(uuid(binding.volume.volumeId)); data.putLong(binding.volume.generation)
        data.put(hexBytes(binding.recipientFingerprint))
        return MessageDigest.getInstance("SHA-256").apply {
            update("OpenPendant long recording control binding v1\u0000".toByteArray(Charsets.US_ASCII))
        }.digest(data.array())
    }
    fun request(command: Int, sequence: Int, boot: UUID?, operation: UUID?, binding: DurablePublicBinding, waitForButton: Boolean=false): Request {
        require(command in STATUS..STOP && sequence in 1..65535)
        require(!waitForButton || command==START)
        require(boot == null || validOwnedUuid(boot)); require(operation == null || validOwnedUuid(operation))
        if (command == STATUS) require(boot != null || operation == null) else require(boot != null && operation != null)
        val p = ByteBuffer.allocate(80).order(ByteOrder.LITTLE_ENDIAN)
        p.put('O'.code.toByte()); p.put('P'.code.toByte()); p.put(1); p.put(command.toByte()); p.putShort(sequence.toShort()); p.putShort(72)
        p.put(uuid(boot)); p.put(uuid(operation)); p.put(bindingDigest(binding)); p.put(if (command == START) { if(waitForButton) 2 else 1 } else 0)
        return Request(command, sequence, boot, operation, p.array())
    }
    fun parseRequest(bytes: ByteArray, binding: DurablePublicBinding): Request {
        require(bytes.size == 80)
        val p = bytes.copyOf(); val input = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        val expected = request(p[3].toInt() and 255, input.getShort(4).toInt() and 65535, readUuid(p,8), readUuid(p,24), binding,p[72]==2.toByte())
        require(p.contentEquals(expected.frame())); return expected
    }
    fun parse(bytes: ByteArray, request: Request): State {
        require(bytes.size == 81)
        val p = bytes.copyOf(); val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        require(p[0] == 79.toByte() && p[1] == 80.toByte() && p[2] == 1.toByte() && (p[3].toInt() and 255) == (request.command or 128))
        require((b.getShort(4).toInt() and 65535) == request.sequence && b.getShort(6).toInt() == 73 && p[8] == 0.toByte())
        require((77..80).all { p[it] == 0.toByte() })
        val phase = p[57].toInt() and 255; require(phase < Phase.entries.size)
        val result = State(requireNotNull(readUuid(p,9)), readUuid(p,25), readUuid(p,41), Phase.entries[phase],
            p[58].toInt() and 255, b.getShort(59).toInt() and 65535, b.getInt(61), b.getInt(65), b.getInt(69), b.getInt(73))
        validate(result)
        require(request.boot == null || result.boot == request.boot)
        require(request.operation == null || result.operation == request.operation)
        require(request.command != START || result.phase != Phase.IDLE)
        require(request.command != STOP || (result.phase != Phase.IDLE &&
            (result.has(STOP_REQUESTED) || result.phase !in setOf(Phase.STARTING,Phase.RUNNING))))
        return result
    }
    fun validate(s: State) {
        require(validOwnedUuid(s.boot) && (s.operation == null || validOwnedUuid(s.operation)) && (s.recording == null || validOwnedUuid(s.recording)))
        require(s.reason in 0..12 && s.flags in 0..4095 && s.epoch >= 0 && s.accepted in 0..MAX_FRAMES && s.committed in 0..s.accepted)
        require(!s.has(BUTTON_ARMED)||(s.phase==Phase.STARTING&&!s.has(CREATED)&&!s.has(MIC)&&
            !s.has(STOP_REQUESTED)&&s.epoch==0&&s.accepted==0&&s.committed==0))
        require(s.admitted in 0..MAX_FRAMES && s.admitted % 500 == 0 && (s.has(CAPACITY) || s.admitted == 0))
        require(!s.has(CAPACITY) || s.accepted <= s.admitted)
        require(!(s.has(MIC) && s.has(PRODUCER_JOINED)))
        require(!s.has(RELEASED) || (s.has(PRODUCER_JOINED) && s.has(STORAGE_JOINED) && !s.has(MIC)))
        fun lacks(mask: Int) = s.flags and mask == 0
        fun all(mask: Int) = s.flags and mask == mask
        when (s.phase) {
            Phase.IDLE -> { require(s.operation == null && s.recording == null && s.epoch == 0 && s.reason == 0 && s.accepted == 0 && s.committed == 0 && all(44) && lacks(MIC or FINALIZED or CREATED or FAULT or STOP_REQUESTED)); return }
            else -> require(s.operation != null)
        }
        if (s.phase == Phase.FAULT) { require(s.has(FAULT) && s.reason != 0 && !s.has(FINALIZED)); return }
        require(!s.has(FAULT))
        when (s.phase) {
            Phase.NO_CAPACITY -> { require(s.reason == 11 && s.recording == null && s.epoch == 0 && s.accepted == 0 && s.committed == 0 && s.admitted == 0 && all(300) && lacks(MIC or FINALIZED or CREATED)); return }
            Phase.CANCELLED_BEFORE_START -> { require(s.reason == 1 && s.recording == null && s.epoch == 0 && s.accepted == 0 && s.committed == 0 && all(44) && lacks(MIC or FINALIZED or CREATED)); return }
            Phase.STARTING -> {
                require(s.reason == 0 && s.accepted == 0 && s.committed == 0 && lacks(MIC or FINALIZED))
                if (s.has(CREATED)) require(s.epoch != 0 && s.recording != null && s.has(CAPACITY) && s.admitted > 0 && !s.has(RELEASED))
                else require(s.epoch == 0 && s.recording == null)
                return
            }
            Phase.STOPPING -> if (!s.has(CREATED)) {
                require(s.reason == 1 && s.epoch == 0 && s.recording == null && s.accepted == 0 && s.committed == 0 && lacks(MIC or FINALIZED or RELEASED)); return
            }
            else -> Unit
        }
        require(s.recording != null && s.epoch != 0 && s.has(CREATED) && s.has(CAPACITY) && s.admitted > 0)
        when (s.phase) {
            Phase.RUNNING -> require(s.reason == 0 && s.has(MIC) && (s.has(USB)||s.has(PORTABLE)) && lacks(PRODUCER_JOINED or FINALIZED or RELEASED))
            Phase.STOPPED -> require((s.reason in setOf(1,11)||s.reason==2&&s.has(PORTABLE)) && s.accepted == s.committed && all(60) && !s.has(MIC) && (s.reason != 11 || s.accepted == s.admitted))
            else -> {
                require((s.reason in setOf(1,11)||s.reason==2&&s.has(PORTABLE)) && lacks(FINALIZED or RELEASED))
                if (s.phase == Phase.DRAINING) require(s.has(PRODUCER_JOINED) && !s.has(MIC))
            }
        }
    }
    fun progress(a: State, b: State) {
        validate(a); validate(b)
        require(a.operation != null && a.operation == b.operation && a.boot == b.boot)
        require(a.has(PORTABLE)==b.has(PORTABLE))
        require(a.recording == null || a.recording == b.recording); require(a.epoch == 0 || a.epoch == b.epoch)
        require(a.accepted <= b.accepted && a.committed <= b.committed && a.phase.ordinal <= b.phase.ordinal)
        require(a.reason == 0 || b.phase == Phase.FAULT || a.reason == b.reason)
        require(!a.has(CREATED) || b.has(CREATED)); require(!a.has(CAPACITY) || (b.has(CAPACITY) && a.admitted == b.admitted))
        require(b.phase != Phase.NO_CAPACITY || a.phase in setOf(Phase.STARTING,Phase.NO_CAPACITY))
        require(b.phase != Phase.CANCELLED_BEFORE_START || !a.has(CREATED))
        if (a.terminal) require(a.copy(flags = a.flags and USB.inv()) == b.copy(flags = b.flags and USB.inv()))
    }
    internal fun uuid(value: UUID?): ByteArray = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN).apply {
        putLong(value?.mostSignificantBits ?: 0); putLong(value?.leastSignificantBits ?: 0)
    }.array()
    internal fun readUuid(bytes: ByteArray, offset: Int): UUID? = ByteBuffer.wrap(bytes,offset,16).order(ByteOrder.BIG_ENDIAN).let {
        val value = UUID(it.long,it.long); if (value == UUID(0,0)) null else value.also { require(validOwnedUuid(it)) }
    }
}
