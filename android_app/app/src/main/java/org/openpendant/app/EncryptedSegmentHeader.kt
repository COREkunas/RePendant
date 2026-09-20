package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/** Proposed container only, not the installed BLE protocol or an HPKE decryptor.
 * Public metadata is visible. Structural validation is NOT tag authentication.
 * Callers must own input arrays exclusively while these methods execute.
 */
object EncryptedSegmentHeader {
    const val HEADER_BYTES = 128
    const val ENCAP_BYTES = 65
    const val TAG_BYTES = 16
    const val MAX_PLAINTEXT_BYTES = 65536
    const val MAX_CONTAINER_BYTES = HEADER_BYTES + ENCAP_BYTES + MAX_PLAINTEXT_BYTES + TAG_BYTES
    private val magic = byteArrayOf(0x4f, 0x50, 0x4e, 0x44, 0x45, 0x53, 0x31, 0)
    private val domain = "OpenPendant encrypted segment v1\u0000".toByteArray(Charsets.US_ASCII)

    fun encode(recording: DurableRecordingId, keyFingerprint: ByteArray,
               sequence: Int, plaintextBytes: Int): ByteArray {
        require(keyFingerprint.size == 32 && keyFingerprint.any { it != 0.toByte() } &&
            keyFingerprint.any { it != 0xff.toByte() }) { "Invalid recipient fingerprint" }
        require(sequence >= 0 && plaintextBytes in 1..MAX_PLAINTEXT_BYTES) { "Invalid encrypted segment bounds" }
        return ByteBuffer.allocate(HEADER_BYTES).order(ByteOrder.BIG_ENDIAN).apply {
            put(magic); putShort(1); putShort(HEADER_BYTES.toShort())
            putShort(0x0010); putShort(0x0001); putShort(0x0002); putShort(0)
            put(keyFingerprint); putUuid(recording.volume.deviceId); putUuid(recording.volume.volumeId)
            putLong(recording.volume.generation); putUuid(recording.recordingId)
            putInt(sequence); putInt(plaintextBytes); put(ByteArray(12))
            check(position() == HEADER_BYTES)
        }.array()
    }

    /** The expected identity must come from pinned/authenticated catalog state,
     * not be copied out of the untrusted header to make this check pass. */
    fun validate(header: ByteArray, recording: DurableRecordingId, keyFingerprint: ByteArray,
                 sequence: Int, plaintextBytes: Int): Boolean =
        header.size == HEADER_BYTES && header.contentEquals(encode(recording, keyFingerprint, sequence, plaintextBytes))

    fun hpkeInfo(header: ByteArray, recording: DurableRecordingId, keyFingerprint: ByteArray,
                 sequence: Int, plaintextBytes: Int): ByteArray {
        require(header.size == HEADER_BYTES) { "Invalid encrypted header size" }
        val snapshot = header.copyOf()
        require(validate(snapshot, recording, keyFingerprint, sequence, plaintextBytes)) { "Encrypted header binding differs" }
        check(domain.size == 33)
        return domain + snapshot //161 bytes. Production AEAD AAD is EMPTY.
    }

    /** Checks framing and the uncompressed point PREFIX only. The crypto provider
     * must validate the complete curve point and HPKE authentication tag before
     * any plaintext is published, played or transcribed. */
    fun validateContainer(container: ByteArray, recording: DurableRecordingId, keyFingerprint: ByteArray,
                          sequence: Int, plaintextBytes: Int): Boolean {
        require(plaintextBytes in 1..MAX_PLAINTEXT_BYTES) { "Invalid encrypted segment bounds" }
        if (container.size != HEADER_BYTES + ENCAP_BYTES + plaintextBytes + TAG_BYTES) return false
        return container[HEADER_BYTES] == 4.toByte() &&
            validate(container.copyOfRange(0, HEADER_BYTES), recording, keyFingerprint, sequence, plaintextBytes)
    }

    private fun ByteBuffer.putUuid(value: UUID) { putLong(value.mostSignificantBits); putLong(value.leastSignificantBits) }
}
