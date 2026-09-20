package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.UUID

/** Actual C sender + real Python provider output from NEW PUBLIC fixture
 * tools/tests/recording_hpke_container_public.json; C sender SHA256
 * f9a9bdd7a736fb602e6b6f16c81258721a0ed0cd01a929608fa727a64039cb58.
 * No fixture files, owner secrets or device are read by these pure JVM tests.
 */
class EncryptedSegmentRecipientTest {
    private fun bytes(hex: String): ByteArray = hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private fun uuid(start: Int): UUID = ByteBuffer.wrap(ByteArray(16) { (start + it).toByte() }).run { UUID(long, long) }
    private val recording = DurableRecordingId(RecordingVolume(uuid(17), uuid(33), 0x0102030405060708L), uuid(49))
    private fun key() = RecipientRecoveryCodec.importP256(
        bytes("317f915db7bc629c48fe765587897e01e282d3e8445f79f27f65d031a88082b2"),
        bytes("04abc7e49a4c6b3566d77d0304addc6ed0e98512ffccf505e6a8e3eb25c685136f853148544876de76c0f2ef99cdc3a05ccf5ded7860c7c021238f9e2073d2356c"))
    private fun fingerprint() = bytes("74451bdc1fe4abf607783bc8b224c75f4b809ee3c15610d56e6cb4f00eb12322")
    private fun plaintext() = bytes("4265617574792069732074727574682c20747275746820626561757479")
    private fun header() = bytes("4f504e444553310000010080001000010002000074451bdc1fe4abf607783bc8b224c75f4b809ee3c15610d56e6cb4f00eb123221112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f3001020304050607083132333435363738393a3b3c3d3e3f40000000070000001d000000000000000000000000")
    private fun enc() = bytes("04c06b4f6bebc7bb495cb797ab753f911aff80aefb86fd8b6fcc35525f3ab5f03e0b21bd31a86c6048af3cb2d98e0d3bf01da5cc4c39ff5370d331a4f1f7d5a4e0")
    private fun ciphertext() = bytes("428313c6eb360f205976963bac77299aeb348c4c02d0846eedbc9df080f865c136199c5cfd95f1142c685a4388")
    private fun container() = header() + enc() + ciphertext()
    private fun sha(value: ByteArray) = MessageDigest.getInstance("SHA-256").digest(value)
    private fun rejected(action: () -> Unit) {
        try { action(); fail("Untrusted segment was decrypted") }
        catch (error: EncryptedSegmentException) {
            assertEquals("Invalid or unauthenticated encrypted segment", error.message)
            assertNull(error.cause)
        }
    }

    @Test fun actualCSenderCiphertextDecryptsAfterIndependentRecoveryExportAndImport() {
        assertEquals(72623859790382856L, recording.volume.generation)
        assertArrayEquals(header(), EncryptedSegmentHeader.encode(recording, fingerprint(), 7, 29))
        assertArrayEquals(bytes("9c5deb4e33682740c81e891ab2d4900fea2a955aeca8c23057d433ff2c13ba26"), sha(header()))
        assertArrayEquals(bytes("3fb5d093a2ae4d6b41e782f6241d90ed22664692d1764b039cbf385522fcdd5b"), sha(ciphertext()))
        assertEquals(238, container().size)
        assertArrayEquals(bytes("471bb75496060c7fcd1aba38edb8fc9fdcf8b2426581747f09561706cdba0ab2"), sha(container()))
        val recovery = key().use { original ->
            assertArrayEquals(fingerprint(), original.publicFingerprint())
            RecipientRecoveryCodec.encode(original).use { it.copyForExplicitExport() }
        }
        try {
            RecipientRecoveryCodec.decode(recovery).use { restored ->
                val input = container()
                EncryptedSegmentRecipient.decrypt(input, restored, recording, 7, 29).use { clear ->
                    val output = clear.copyForAuthenticatedUse()
                    try { assertArrayEquals(plaintext(), output) } finally { output.fill(0) }
                }
                assertArrayEquals(container(), input)
            }
        } finally { recovery.fill(0) }
    }

    @Test fun everyHeaderByteRemainsBoundToTrustedCatalogFields() {
        key().use { recipient ->
            for (i in 0 until 128) {
                val tampered = container().also { it[i] = (it[i].toInt() xor 1).toByte() }
                rejected { EncryptedSegmentRecipient.decrypt(tampered, recipient, recording, 7, 29).close() }
            }
            for (other in listOf(recording.copy(recordingId = uuid(65)),
                recording.copy(volume = recording.volume.copy(deviceId = uuid(81))),
                recording.copy(volume = recording.volume.copy(volumeId = uuid(97))),
                recording.copy(volume = recording.volume.copy(generation = recording.volume.generation + 1)))) {
                rejected { EncryptedSegmentRecipient.decrypt(container(), recipient, other, 7, 29).close() }
            }
            rejected { EncryptedSegmentRecipient.decrypt(container(), recipient, recording, 8, 29).close() }
        }
    }

    @Test fun structurallyMatchingSubstitutedHeaderStillFailsCryptographicAuthentication() {
        key().use { recipient ->
            val changed = recording.copy(recordingId = uuid(65))
            val frame = EncryptedSegmentHeader.encode(changed, fingerprint(), 8, 29) + enc() + ciphertext()
            assertTrue(EncryptedSegmentHeader.validateContainer(frame, changed, fingerprint(), 8, 29))
            rejected { EncryptedSegmentRecipient.decrypt(frame, recipient, changed, 8, 29).close() }
        }
    }

    @Test fun alteredCiphertextTagAndInvalidFullPointNeverYieldPlaintext() {
        key().use { recipient ->
            for (i in 193 until container().size) {
                val frame = container().also { it[i] = (it[i].toInt() xor 1).toByte() }
                rejected { EncryptedSegmentRecipient.decrypt(frame, recipient, recording, 7, 29).close() }
            }
            val invalidPoint = header() + byteArrayOf(4) + ByteArray(64) + ciphertext()
            assertTrue(EncryptedSegmentHeader.validateContainer(invalidPoint, recording, fingerprint(), 7, 29))
            rejected { EncryptedSegmentRecipient.decrypt(invalidPoint, recipient, recording, 7, 29).close() }
            val compressed = container().also { it[128] = 2 }
            rejected { EncryptedSegmentRecipient.decrypt(compressed, recipient, recording, 7, 29).close() }
        }
    }

    @Test fun actualRecipientFingerprintCannotBeReplacedByAnUntrustedHeaderClaim() {
        val one = ByteArray(32).also { it[31] = 1 }
        val generator = bytes("046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")
        RecipientRecoveryCodec.importP256(one, generator).use { wrong ->
            rejected { EncryptedSegmentRecipient.decrypt(container(), wrong, recording, 7, 29).close() }
            val rebound = EncryptedSegmentHeader.encode(recording, wrong.publicFingerprint(), 7, 29) + enc() + ciphertext()
            assertTrue(EncryptedSegmentHeader.validateContainer(rebound, recording, wrong.publicFingerprint(), 7, 29))
            rejected { EncryptedSegmentRecipient.decrypt(rebound, wrong, recording, 7, 29).close() }
        }
    }

    @Test fun exactLengthBoundsClosedKeyAndDamagedRecoveryFailClosed() {
        key().use { recipient ->
            for (length in listOf(0, 127, 193, 237, 239, EncryptedSegmentHeader.MAX_CONTAINER_BYTES + 1)) {
                rejected { EncryptedSegmentRecipient.decrypt(container().copyOf(length), recipient, recording, 7, 29).close() }
            }
            for (length in listOf(0, -1, 30, 65537)) {
                rejected { EncryptedSegmentRecipient.decrypt(container(), recipient, recording, 7, length).close() }
            }
            rejected { EncryptedSegmentRecipient.decrypt(container(), recipient, recording, -1, 29).close() }
            val recovery = RecipientRecoveryCodec.encode(recipient).use { it.copyForExplicitExport() }
            recovery[16] = (recovery[16].toInt() xor 1).toByte()
            try { assertThrows(RecipientRecoveryException::class.java) { RecipientRecoveryCodec.decode(recovery).close() } }
            finally { recovery.fill(0) }
        }
        val closedKey = key(); closedKey.close()
        rejected { EncryptedSegmentRecipient.decrypt(container(), closedKey, recording, 7, 29).close() }
    }
}
