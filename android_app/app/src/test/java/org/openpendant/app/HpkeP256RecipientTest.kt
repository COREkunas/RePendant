package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** PUBLIC CFRG/RFC9180 test-vector material only, never owner keys/audio/files.
 * Suite0/16/1/2 selected from pinned cfrg commit5f503c564da00b0687b3de75f1dfbdfc4079ad31.
 * The repository's tools/tests/recording_hpke_rfc9180_public.json preserves provenance.
 */
class HpkeP256RecipientTest {
    private fun bytes(hex: String): ByteArray = hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private fun scalar() = bytes("317f915db7bc629c48fe765587897e01e282d3e8445f79f27f65d031a88082b2")
    private fun point() = bytes("04abc7e49a4c6b3566d77d0304addc6ed0e98512ffccf505e6a8e3eb25c685136f853148544876de76c0f2ef99cdc3a05ccf5ded7860c7c021238f9e2073d2356c")
    private fun enc() = bytes("04c06b4f6bebc7bb495cb797ab753f911aff80aefb86fd8b6fcc35525f3ab5f03e0b21bd31a86c6048af3cb2d98e0d3bf01da5cc4c39ff5370d331a4f1f7d5a4e0")
    private fun info() = bytes("4f6465206f6e2061204772656369616e2055726e")
    private fun aad() = bytes("436f756e742d30")
    private fun cipher() = bytes("58c61a45059d0c5704560e9d88b564a8b63f1364b8d1fcb3c4c6ddc1d291742465e902cd216f8908da49f8f96f")
    private fun plaintext() = bytes("4265617574792069732074727574682c20747275746820626561757479")
    private fun key() = RecipientRecoveryCodec.importP256(scalar(), point())
    private fun rejected(action: () -> Unit) {
        try { action(); fail("Unauthenticated plaintext accepted") }
        catch (error: EncryptedSegmentException) {
            assertEquals("Invalid or unauthenticated encrypted segment", error.message)
            assertNull(error.cause)
        }
    }

    @Test fun exactCfrgCiphertextOpensToKnownPublicPlaintext() {
        key().use { recipient ->
            HpkeP256Recipient.open(recipient, enc(), info(), aad(), cipher()).use {
                val output = it.copyForAuthenticatedUse()
                try { assertArrayEquals(plaintext(), output) } finally { output.fill(0) }
                assertEquals("DecryptedSegmentBytes[REDACTED]", it.toString())
            }
        }
    }

    @Test fun recoveryExportImportRestoresCapabilityAfterOriginalOwnerClosed() {
        val wire = key().use { original ->
            RecipientRecoveryCodec.encode(original).use { it.copyForExplicitExport() }
        }
        try {
            RecipientRecoveryCodec.decode(wire).use { restored ->
                HpkeP256Recipient.open(restored, enc(), info(), aad(), cipher()).use {
                    val output = it.copyForAuthenticatedUse()
                    try { assertArrayEquals(plaintext(), output) } finally { output.fill(0) }
                }
            }
            wire[20] = (wire[20].toInt() xor 1).toByte()
            assertThrows(RecipientRecoveryException::class.java) { RecipientRecoveryCodec.decode(wire).close() }
        } finally { wire.fill(0) }
    }

    @Test fun ciphertextAndEveryTagByteMustAuthenticateBeforeAnyPlaintextIsReturned() {
        key().use { recipient ->
            for (i in cipher().indices) {
                val changed = cipher().also { it[i] = (it[i].toInt() xor 1).toByte() }
                rejected { HpkeP256Recipient.open(recipient, enc(), info(), aad(), changed).close() }
            }
            rejected { HpkeP256Recipient.open(recipient, enc(), info(), aad(), cipher().copyOf(16)).close() }
            rejected { HpkeP256Recipient.open(recipient, enc(), info(), aad(), cipher() + 0.toByte()).close() }
        }
    }

    @Test fun wrongKeyInfoAndAadCannotOpenTheKnownCiphertext() {
        val one = ByteArray(32).also { it[31] = 1 }
        val generator = bytes("046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")
        RecipientRecoveryCodec.importP256(one, generator).use { wrong ->
            rejected { HpkeP256Recipient.open(wrong, enc(), info(), aad(), cipher()).close() }
        }
        key().use { recipient ->
            rejected { HpkeP256Recipient.open(recipient, enc(), info().also { it[0] = 0 }, aad(), cipher()).close() }
            rejected { HpkeP256Recipient.open(recipient, enc(), info(), byteArrayOf(), cipher()).close() }
            rejected { HpkeP256Recipient.open(recipient, enc(), info(), aad().also { it[0] = 0 }, cipher()).close() }
            // A different but fully valid P-256 point fails authentication too.
            rejected { HpkeP256Recipient.open(recipient, generator, info(), aad(), cipher()).close() }
        }
    }

    @Test fun encapsulatedPointNeedsFullCurveValidationNotOnlyPrefix() {
        key().use { recipient ->
            for (bad in listOf(ByteArray(65), ByteArray(65).also { it[0] = 4 },
                enc().also { it[0] = 2 }, enc().also { it[64] = (it[64].toInt() xor 1).toByte() },
                enc().also { bytes("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff").copyInto(it, 1) })) {
                rejected { HpkeP256Recipient.open(recipient, bad, info(), aad(), cipher()).close() }
            }
        }
    }

    @Test fun fixedBoundsRejectOversizedContextsAndCiphertexts() {
        key().use { recipient ->
            for (length in listOf(0, 64, 66, 4096)) rejected {
                HpkeP256Recipient.open(recipient, ByteArray(length), info(), aad(), cipher()).close()
            }
            for (length in listOf(0, 15, 65553)) rejected {
                HpkeP256Recipient.open(recipient, enc(), info(), aad(), ByteArray(length)).close()
            }
            rejected { HpkeP256Recipient.open(recipient, enc(), ByteArray(257), aad(), cipher()).close() }
            rejected { HpkeP256Recipient.open(recipient, enc(), info(), ByteArray(257), cipher()).close() }
        }
    }

    @Test fun standardAesWithPublishedVectorKeyExercisesZeroAndMaximumPlaintextBounds() {
        // The already-public RFC schedule key/nonce are reused ONLY in this test.
        // This is not a sender API, RNG, ephemeral key or production nonce policy.
        val publicAes = bytes("ee16802a936d5f544771131900ee6973d0551de9e852ece2ef34bf0d5f9e1d1d")
        val publicNonce = bytes("9bc50980832a7b4b58c40161")
        key().use { recipient ->
            for (length in listOf(0, 1, 65536)) {
                val expected = ByteArray(length) { (it % 251).toByte() }
                val standard = Cipher.getInstance("AES/GCM/NoPadding")
                standard.init(Cipher.ENCRYPT_MODE, SecretKeySpec(publicAes, "AES"), GCMParameterSpec(128, publicNonce))
                standard.updateAAD(aad())
                val ciphertext = standard.doFinal(expected)
                HpkeP256Recipient.open(recipient, enc(), info(), aad(), ciphertext).use {
                    val output = it.copyForAuthenticatedUse()
                    try { assertArrayEquals(expected, output) } finally { output.fill(0) }
                }
                expected.fill(0); ciphertext.fill(0)
            }
        }
        publicAes.fill(0); publicNonce.fill(0)
    }

    @Test fun callerInputsRemainUnchangedAndPlaintextCopiesAreIndependent() {
        val inputEnc = enc(); val inputInfo = info(); val inputAad = aad(); val inputCipher = cipher()
        key().use { recipient ->
            HpkeP256Recipient.open(recipient, inputEnc, inputInfo, inputAad, inputCipher).use { clear ->
                val first = clear.copyForAuthenticatedUse(); first.fill(0)
                val second = clear.copyForAuthenticatedUse()
                try { assertArrayEquals(plaintext(), second) } finally { second.fill(0) }
                assertArrayEquals(enc(), inputEnc); assertArrayEquals(info(), inputInfo)
                assertArrayEquals(aad(), inputAad); assertArrayEquals(cipher(), inputCipher)
            }
        }
    }

    @Test fun closingPlaintextWipesItsOwnedBufferAndClosedKeyCannotDecrypt() {
        val owned = plaintext()
        val clear = DecryptedSegmentBytes(owned)
        clear.close(); clear.close()
        assertTrue(owned.all { it == 0.toByte() })
        assertThrows(IllegalStateException::class.java) { clear.copyForAuthenticatedUse() }
        val closedKey = key(); closedKey.close()
        rejected { HpkeP256Recipient.open(closedKey, enc(), info(), aad(), cipher()).close() }
    }
}
