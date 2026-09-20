package org.openpendant.app

import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class EncryptedSegmentHeaderTest {
    private fun uuid(start: Int): UUID = ByteBuffer.wrap(ByteArray(16) { (start + it).toByte() }).run { UUID(long, long) }
    private val id = DurableRecordingId(RecordingVolume(uuid(17), uuid(33), 0x0102030405060708), uuid(49))
    private val fingerprint = ByteArray(32) { (it + 1).toByte() }
    private fun header(size: Int = 2048) = EncryptedSegmentHeader.encode(id, fingerprint, 7, size)
    private fun sha(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }

    @Test fun exactPublicVectorMatchesIndependentNativeOracle() {
        val h = header()
        assertEquals(128, h.size)
        assertEquals("4cdc69f74e1772ead117da893e77b93df3c82c8f6dbf3f93e980d06766497ffa", sha(h))
        val info = EncryptedSegmentHeader.hpkeInfo(h, id, fingerprint, 7, 2048)
        assertEquals(161, info.size)
        assertEquals("062dadc984c047be0a29ee47d8af34108fb34d94f51d28ea8a6ec068a309aab6", sha(info))
    }

    @Test fun everyHeaderBitIsBoundIncludingAlgorithmAndReservedFields() {
        for (byte in 0 until 128) for (bit in 0..7) {
            val h = header(); h[byte] = (h[byte].toInt() xor (1 shl bit)).toByte()
            assertFalse(EncryptedSegmentHeader.validate(h, id, fingerprint, 7, 2048))
            assertThrows(IllegalArgumentException::class.java) { EncryptedSegmentHeader.hpkeInfo(h, id, fingerprint, 7, 2048) }
        }
    }

    @Test fun identityAndGenerationCannotBeSubstituted() {
        val replacements = listOf(id.copy(recordingId = uuid(65)),
            id.copy(volume = id.volume.copy(generation = id.volume.generation + 1)),
            id.copy(volume = id.volume.copy(volumeId = uuid(81))),
            id.copy(volume = id.volume.copy(deviceId = uuid(97))))
        for (replacement in replacements) assertFalse(EncryptedSegmentHeader.validate(header(), replacement, fingerprint, 7, 2048))
        val otherKey = fingerprint.copyOf().also { it[0] = 99 }
        assertFalse(EncryptedSegmentHeader.validate(header(), id, otherKey, 7, 2048))
        assertFalse(EncryptedSegmentHeader.validate(header(), id, fingerprint, 8, 2048))
        assertFalse(EncryptedSegmentHeader.validate(header(), id, fingerprint, 7, 2049))
    }

    @Test fun boundsAndFramingAreExact() {
        for (length in listOf(1, 60, 2048, 65536)) {
            val frame = header(length) + byteArrayOf(4) + ByteArray(64 + length + 16)
            assertTrue(EncryptedSegmentHeader.validateContainer(frame, id, fingerprint, 7, length))
            assertFalse(EncryptedSegmentHeader.validateContainer(frame.copyOf(frame.size - 1), id, fingerprint, 7, length))
            assertFalse(EncryptedSegmentHeader.validateContainer(frame + 0.toByte(), id, fingerprint, 7, length))
            frame[128] = 2
            assertFalse(EncryptedSegmentHeader.validateContainer(frame, id, fingerprint, 7, length))
        }
        for (size in listOf(-1, 0, 65537)) assertThrows(IllegalArgumentException::class.java) { header(size) }
        assertThrows(IllegalArgumentException::class.java) { EncryptedSegmentHeader.encode(id, fingerprint, -1, 1) }
        for (bad in listOf(ByteArray(31), ByteArray(32), ByteArray(32) { -1 })) {
            assertThrows(IllegalArgumentException::class.java) { EncryptedSegmentHeader.encode(id, bad, 7, 1) }
        }
        assertTrue(EncryptedSegmentHeader.validate(EncryptedSegmentHeader.encode(id, fingerprint, Int.MAX_VALUE, 1),
            id, fingerprint, Int.MAX_VALUE, 1))
    }

    @Test fun parserDoesNotPretendToAuthenticateCiphertextOrEcPoint() {
        val frame = header() + byteArrayOf(4) + ByteArray(64 + 2048 + 16)
        // The all-zero curve coordinates are invalid cryptographically, but the
        // header-only parser checks neither the full point nor the GCM tag.
        assertTrue(EncryptedSegmentHeader.validateContainer(frame, id, fingerprint, 7, 2048))
        frame[frame.lastIndex] = 99
        assertTrue(EncryptedSegmentHeader.validateContainer(frame, id, fingerprint, 7, 2048))
    }

    @Test fun outputIsIndependentFromCallerArraysAndHeaders() {
        val key = fingerprint.copyOf()
        val h = EncryptedSegmentHeader.encode(id, key, 7, 2048)
        val info = EncryptedSegmentHeader.hpkeInfo(h, id, fingerprint, 7, 2048)
        key.fill(0); h.fill(0)
        assertEquals("062dadc984c047be0a29ee47d8af34108fb34d94f51d28ea8a6ec068a309aab6", sha(info))
    }
}
