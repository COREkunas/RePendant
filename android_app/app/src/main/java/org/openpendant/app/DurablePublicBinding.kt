package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID

/** Public enrollment authority, never derived from a received catalog. The bond
 * address selects Android's already authenticated peer; full volume/key binding
 * is independently compared with every canonical catalog and segment. */
data class DurablePublicBinding(val bondAddress: String, val volume: RecordingVolume,
    val recipientFingerprint: String) {
    init {
        require(Regex("(?:[0-9A-F]{2}:){5}[0-9A-F]{2}").matches(bondAddress) &&
            bondAddress != "00:00:00:00:00:00" && bondAddress != "FF:FF:FF:FF:FF:FF")
        require(isContentDigest(recipientFingerprint) && recipientFingerprint != "00".repeat(32) && recipientFingerprint != "ff".repeat(32))
    }
    fun connection(peer: DurableConnectedPeer): DurableSyncConnection {
        require(peer.bondAddress == bondAddress && peer.capabilities.catalog && peer.capabilities.rangedSegments &&
            peer.capabilities.durableReceipts && peer.capabilities.tombstones)
        return DurableSyncConnection(peer.epoch, volume, recipientFingerprint)
    }
    fun requireVerifiedRecipient(summary: RecipientVaultSummary) {
        require(summary.state == RecipientVaultState.READY && summary.backupVerified && summary.fingerprintHex == recipientFingerprint) {
            "Verify the matching recovery backup before storage sync or playback"
        }
    }
}

data class DurableConnectedPeer(val epoch: UUID, val bondAddress: String, val capabilities: DurableSyncCapabilities)

/** Exact public 128-byte record. SHA256 is corruption detection, not encryption
 * or device authentication. Only explicit trusted enrollment may publish it. */
object DurablePublicBindingCodec {
    const val BYTES = 128
    private val magic = byteArrayOf(79, 80, 78, 68, 66, 78, 49, 0)
    fun encode(binding: DurablePublicBinding): ByteArray {
        val bytes = ByteBuffer.allocate(BYTES).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(magic); putShort(1); putShort(BYTES.toShort()); putInt(0)
            binding.bondAddress.split(':').forEach { put(it.toInt(16).toByte()) }; putShort(0)
            fun uuid(value: UUID) { for (shift in 56 downTo 0 step 8) put((value.mostSignificantBits ushr shift).toByte()); for (shift in 56 downTo 0 step 8) put((value.leastSignificantBits ushr shift).toByte()) }
            uuid(binding.volume.deviceId); uuid(binding.volume.volumeId); putLong(binding.volume.generation)
            put(hexBytes(binding.recipientFingerprint)); check(position() == 96)
        }.array()
        MessageDigest.getInstance("SHA-256").digest(bytes.copyOf(96)).copyInto(bytes, 96)
        return bytes
    }
    fun decode(bytes: ByteArray): DurablePublicBinding {
        require(bytes.size == BYTES)
        val stable = bytes.copyOf(); val input = ByteBuffer.wrap(stable).order(ByteOrder.LITTLE_ENDIAN)
        require(magic.indices.all { stable[it] == magic[it] } && input.getShort(8).toInt() == 1 &&
            input.getShort(10).toInt() == BYTES && input.getInt(12) == 0 && input.getShort(22).toInt() == 0)
        require(MessageDigest.isEqual(MessageDigest.getInstance("SHA-256").digest(stable.copyOf(96)), stable.copyOfRange(96, 128)))
        fun uuid(at: Int) = ByteBuffer.wrap(stable, at, 16).order(ByteOrder.BIG_ENDIAN).let { UUID(it.long, it.long) }
        return DurablePublicBinding(stable.copyOfRange(16, 22).joinToString(":") { "%02X".format(it.toInt() and 255) },
            RecordingVolume(uuid(24), uuid(40), input.getLong(56)), stable.copyOfRange(64, 96).joinToString("") { "%02x".format(it.toInt() and 255) })
    }
}
