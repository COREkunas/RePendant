package org.openpendant.app

import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.UUID

/** Public-only, fixed commands. No arbitrary console text or private-key payload. */
internal object KeyResetProtocol {
    data class State(val phase: Int, val reset: Boolean, val busy: Boolean, val fault: Boolean,
                     val fresh: Boolean, val descriptor: String, val parent: String?)
    sealed class Command(val wire: String) {
        data object Info : Command("recorder keyinfo")
        data object Restart : Command("pendant restart confirm")
        data object Battery : Command("recorder batterywatch")
        data object PauseBattery : Command("recorder batterywatchstop")
        class Prepare internal constructor(val plan: Plan) : Command("recorder keyprepare ${plan.oldDescriptor} ${compact(plan.next.volume.volumeId)} ${plan.next.recipientFingerprint} ${plan.point} delete-pendant-recordings")
        class Erase internal constructor(val confirmation: String) : Command("recorder keyerase $confirmation delete-pendant-recordings") {
            init { require(isContentDigest(confirmation)) }
        }
    }
    data class Plan(val old: DurablePublicBinding, val oldDescriptor: String, val next: DurablePublicBinding, val point: String) {
        init {
            require(isContentDigest(oldDescriptor) && oldDescriptor != "00".repeat(32))
            require(old.volume.generation in 2 until Long.MAX_VALUE && next.volume.generation == old.volume.generation + 1)
            require(old.volume.deviceId == next.volume.deviceId && old.volume.volumeId != next.volume.volumeId && old.bondAddress == next.bondAddress)
            require(old.recipientFingerprint != next.recipientFingerprint && point.matches(Regex("04[0-9a-f]{128}")))
            RecipientRecoveryCodec.importPublicP256(hexBytes(point))
            require(RecipientRecoveryCodec.fingerprint(hexBytes(point)).contentEquals(hexBytes(next.recipientFingerprint)))
        }
        fun checkChild(storage: PhoneMigrationProtocol.Storage, state: State) {
            require(storage.binding == next && storage.publicPoint == point && storage.phase == state.phase &&
                state.reset && state.parent == oldDescriptor && !state.fault && !state.busy)
        }
    }
    fun state(reply: String): State {
        require(reply.length <= 4096 && '\u0000' !in reply)
        val rows = reply.lineSequence().map { it.trimEnd('\r') }.filter { it.startsWith("RECORDER_KEY_") }.toList()
        require(rows.size in 2..3)
        val h = checkNotNull(Regex("RECORDER_KEY_RESET v=1 phase=([1-3]) reset=([01]) busy=([01]) fault=([01]) fresh=([01])").matchEntire(rows[0])).groupValues
        fun digest(row: String, prefix: String) = checkNotNull(Regex("$prefix sha=([0-9a-f]{64})").matchEntire(row)).groupValues[1]
        val reset = h[2] == "1"; require(rows.size == if (reset) 3 else 2)
        return State(h[1].toInt(), reset, h[3] == "1", h[4] == "1", h[5] == "1", digest(rows[1], "RECORDER_KEY_DESCRIPTOR"),
            if (reset) digest(rows[2], "RECORDER_KEY_PARENT") else null)
    }
    fun compact(id: UUID) = id.toString().replace("-", "")
    fun encode(plan: Plan): ByteArray = ByteArray(400).also { bytes ->
        val b = ByteBuffer.wrap(bytes)
        b.put("OPNKR01\u0000".toByteArray(Charsets.US_ASCII)); b.put(DurablePublicBindingCodec.encode(plan.old))
        b.put(hexBytes(plan.oldDescriptor)); b.put(DurablePublicBindingCodec.encode(plan.next)); b.put(hexBytes(plan.point))
        MessageDigest.getInstance("SHA-256").digest(bytes.copyOf(368)).copyInto(bytes,368)
    }
    fun decode(bytes: ByteArray): Plan {
        require(bytes.size == 400 && bytes.copyOfRange(0,8).contentEquals("OPNKR01\u0000".toByteArray(Charsets.US_ASCII)))
        require(bytes.copyOfRange(361,368).all { it == 0.toByte() } && MessageDigest.getInstance("SHA-256").digest(bytes.copyOf(368)).contentEquals(bytes.copyOfRange(368,400)))
        fun hex(a: Int,b: Int) = bytes.copyOfRange(a,b).joinToString("") { "%02x".format(it.toInt() and 255) }
        return Plan(DurablePublicBindingCodec.decode(bytes.copyOfRange(8,136)),hex(136,168),
            DurablePublicBindingCodec.decode(bytes.copyOfRange(168,296)),hex(296,361))
    }
}
