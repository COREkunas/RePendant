package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.lang.reflect.InvocationTargetException
import java.math.BigInteger
import java.security.MessageDigest
import java.security.Signature

/** All keys are PUBLIC RFC6979 A.2.5 / P-256 generator test fixtures, never owner keys.
 * https://www.rfc-editor.org/rfc/rfc6979#appendix-A.2.5
 * Pure JVM, no files, Android, Keystore, network, UI or real key generation.
 */
class RecipientRecoveryCodecTest {
    private fun bytes(hex: String): ByteArray = hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private fun scalar() = bytes("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721")
    private fun point() = bytes("04" +
        "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6" +
        "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299")
    private fun generator() = bytes("04" +
        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296" +
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")
    private fun order() = bytes("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551")
    private fun prime() = bytes("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff")
    private fun key() = RecipientRecoveryCodec.importP256(scalar(), point())
    private fun encoded(): ByteArray = key().use { key ->
        RecipientRecoveryCodec.encode(key).use { it.copyForExplicitExport() }
    }
    private fun repairChecksum(encoded: ByteArray) {
        MessageDigest.getInstance("SHA-256").digest(encoded.copyOfRange(0, 113)).copyInto(encoded, 113)
    }
    private fun rejected(action: () -> Unit) {
        try { action(); fail("Invalid recovery material accepted") }
        catch (error: RecipientRecoveryException) {
            assertEquals("Invalid or unsupported recipient recovery key", error.message)
            assertNull(error.cause)
        }
    }
    private fun closed(action: () -> Unit) {
        try { action(); fail("Closed secret owner accepted use") }
        catch (_: IllegalStateException) { }
    }

    @Test fun publicRfcVectorRoundTripsExact145ByteFormat() {
        val export = encoded()
        try {
            assertEquals(145, export.size)
            assertArrayEquals(bytes("4f504e44524b31000001001000200041"), export.copyOfRange(0, 16))
            assertArrayEquals(scalar(), export.copyOfRange(16, 48))
            assertArrayEquals(point(), export.copyOfRange(48, 113))
            assertArrayEquals(MessageDigest.getInstance("SHA-256").digest(export.copyOfRange(0, 113)), export.copyOfRange(113, 145))
            RecipientRecoveryCodec.decode(export).use { restored ->
                assertArrayEquals(point(), restored.publicKeyBytes())
                RecipientRecoveryCodec.encode(restored).use { assertArrayEquals(export, it.copyForExplicitExport()) }
                restored.withPrivateKey { private ->
                    assertEquals(BigInteger(1, scalar()), private.s)
                    assertEquals(256, private.params.curve.field.fieldSize)
                }
            }
        } finally { export.fill(0) }
    }

    @Test fun fingerprintUsesFullDomainSeparatedPublicHashNotPrivateScalar() {
        val expected = MessageDigest.getInstance("SHA-256").run {
            update("OpenPendant recipient public key v1\u0000".toByteArray(Charsets.US_ASCII))
            update(bytes("0010"))
            digest(point())
        }
        // Independently checked with .NET SHA256 over only this PUBLIC fixture.
        assertArrayEquals(bytes("ebf7fe2062f55c44462777a66530b6ca270f1239ce1bf49f3c0307f9b60a00cf"), expected)
        key().use {
            assertEquals(32, it.publicFingerprint().size)
            assertArrayEquals(expected, it.publicFingerprint())
            assertFalse(it.publicFingerprint().contentEquals(MessageDigest.getInstance("SHA-256").digest(scalar())))
            val copy = it.publicFingerprint()
            copy.fill(0)
            assertArrayEquals(expected, it.publicFingerprint())
        }
    }

    @Test fun everySingleByteCorruptionIsRejectedWithoutProviderErrorLeak() {
        val original = encoded()
        try {
            for (i in original.indices) {
                val damaged = original.copyOf()
                damaged[i] = (damaged[i].toInt() xor 1).toByte()
                rejected { RecipientRecoveryCodec.decode(damaged).close() }
                damaged.fill(0)
            }
        } finally { original.fill(0) }
    }

    @Test fun checksumCannotAuthorizeUnknownMagicVersionSuiteOrLengths() {
        val original = encoded()
        try {
            for (i in 0 until 16) {
                val damaged = original.copyOf()
                damaged[i] = (damaged[i].toInt() xor 1).toByte()
                repairChecksum(damaged)
                rejected { RecipientRecoveryCodec.decode(damaged).close() }
                damaged.fill(0)
            }
        } finally { original.fill(0) }
    }

    @Test fun strictInputBoundsRejectTruncatedTrailingAndOversizedMaterial() {
        val original = encoded()
        try {
            for (length in 0 until 145) rejected { RecipientRecoveryCodec.decode(original.copyOf(length)).close() }
            for (length in listOf(146, 256, 4096)) rejected { RecipientRecoveryCodec.decode(original.copyOf(length)).close() }
            for (length in listOf(0, 31, 33, 4096)) rejected { RecipientRecoveryCodec.importP256(ByteArray(length), point()).close() }
            for (length in listOf(0, 33, 64, 66, 97, 4096)) rejected { RecipientRecoveryCodec.importP256(scalar(), ByteArray(length)).close() }
        } finally { original.fill(0) }
    }

    @Test fun scalarRangeAndPublicPairMismatchAreRejectedEvenWithValidChecksum() {
        for (bad in listOf(ByteArray(32), order(), ByteArray(32) { 0xff.toByte() },
            order().also { it[31] = (it[31] + 1).toByte() })) {
            rejected { RecipientRecoveryCodec.importP256(bad, point()).close() }
            val wire = encoded()
            try {
                bad.copyInto(wire, 16)
                repairChecksum(wire)
                rejected { RecipientRecoveryCodec.decode(wire).close() }
            } finally { wire.fill(0); bad.fill(0) }
        }
        rejected { RecipientRecoveryCodec.importP256(scalar(), generator()).close() }
        val two = ByteArray(32).also { it[31] = 2 }
        rejected { RecipientRecoveryCodec.importP256(two, point()).close() }
    }

    @Test fun scalarOneAndOrderMinusOneAreValidWithTheirExactPublicPoints() {
        val one = ByteArray(32).also { it[31] = 1 }
        RecipientRecoveryCodec.importP256(one, generator()).use { assertArrayEquals(generator(), it.publicKeyBytes()) }
        val minusOne = order().also { it[31] = (it[31] - 1).toByte() }
        val opposite = generator()
        val publicNegY = BigInteger(1, prime()).subtract(BigInteger(1, opposite.copyOfRange(33, 65))).toByteArray()
        assertEquals(33, publicNegY.size)
        assertEquals(0.toByte(), publicNegY[0]) // BigInteger's public positive-sign prefix is not SEC1 coordinate data.
        publicNegY.copyInto(opposite, 33, 1, 33)
        RecipientRecoveryCodec.importP256(minusOne, opposite).use { assertArrayEquals(opposite, it.publicKeyBytes()) }
    }

    @Test fun rejectsInfinityCompressedHybridOutOfRangeAndOffCurvePoints() {
        val cases = mutableListOf(ByteArray(65), ByteArray(65) { 0xff.toByte() }, point().also { it[0] = 2 },
            point().also { it[0] = 3 }, point().also { it[0] = 6 }, point().also { it[64] = (it[64] + 1).toByte() },
            ByteArray(65).also { it[0] = 4 }, point().also { prime().copyInto(it, 1) },
            point().also { prime().copyInto(it, 33) })
        for (bad in cases) rejected { RecipientRecoveryCodec.importP256(scalar(), bad).close() }
    }

    @Test fun importedMaterialAndExportCopiesHaveIndependentOwnership() {
        val inputScalar = scalar()
        val inputPublic = point()
        RecipientRecoveryCodec.importP256(inputScalar, inputPublic).use { key ->
            inputScalar.fill(0); inputPublic.fill(0)
            val publicCopy = key.publicKeyBytes()
            publicCopy.fill(0)
            assertArrayEquals(point(), key.publicKeyBytes())
            RecipientRecoveryCodec.encode(key).use { export ->
                val first = export.copyForExplicitExport()
                val second = export.copyForExplicitExport()
                first.fill(0)
                assertFalse(first.contentEquals(second))
                RecipientRecoveryCodec.decode(second).close()
                second.fill(0)
            }
        }
        val wire = encoded()
        RecipientRecoveryCodec.decode(wire).use { key ->
            assertArrayEquals(scalar(), wire.copyOfRange(16, 48)) // Input not mutated by import.
            wire.fill(0)
            assertArrayEquals(point(), key.publicKeyBytes())
        }
    }

    @Test fun holdersAreRedactedAndCloseWipesOwnedArraysAndDisablesUse() {
        val privateOwned = scalar()
        val publicOwned = point()
        val key = RecipientRecoveryKey.validated(privateOwned, publicOwned) // Test-only ownership transfer.
        assertEquals("RecipientRecoveryKey[REDACTED]", key.toString())
        key.close(); key.close()
        assertTrue(privateOwned.all { it == 0.toByte() } && publicOwned.all { it == 0.toByte() })
        closed { key.publicKeyBytes() }
        closed { key.publicFingerprint() }
        closed { key.withPrivateKey { fail("Closed key reached callback") } }
        closed { RecipientRecoveryCodec.encode(key) }
        val exportOwned = encoded()
        val export = RecipientRecoveryBytes(exportOwned)
        assertEquals("RecipientRecoveryBytes[REDACTED]", export.toString())
        export.close(); export.close()
        assertTrue(exportOwned.all { it == 0.toByte() })
        closed { export.copyForExplicitExport() }
    }

    @Test fun trustedCallbackCannotReenterCloseOrExportAndFailureReleasesGuard() {
        key().use { key ->
            try {
                key.withPrivateKey {
                    closed { key.close() }
                    closed { RecipientRecoveryCodec.encode(key) }
                    closed { key.withPrivateKey { fail("Nested private-key callback") } }
                    throw IllegalStateException("Synthetic caller failure")
                }
                fail("Expected synthetic caller failure")
            } catch (error: IllegalStateException) { assertEquals("Synthetic caller failure", error.message) }
            key.withPrivateKey { private ->
                // Uses JCA only; no actual owner key or plaintext recording exists.
                val signer = Signature.getInstance("SHA256withECDSA")
                signer.initSign(private)
                signer.update(byteArrayOf(1, 2, 3))
                val signature = signer.sign()
                assertTrue(signature.isNotEmpty())
                signature.fill(0)
            }
        }
    }

    @Test fun unexpectedProviderRuntimeTextIsSanitizedWithoutGlobalProviderMutation() {
        // Test the private provider boundary directly: do not install/change any
        // JVM security provider, and do not widen the production key API for tests.
        val boundary = RecipientRecoveryCodec::class.java.getDeclaredMethod("checkedProvider", Function0::class.java)
        boundary.isAccessible = true
        val failure: () -> Unit = { throw IllegalStateException("synthetic-private-provider-detail") }
        try {
            boundary.invoke(RecipientRecoveryCodec, failure)
            fail("Provider failure was accepted")
        } catch (error: InvocationTargetException) {
            val sanitized = error.cause
            assertTrue(sanitized is RecipientRecoveryException)
            assertEquals("Invalid or unsupported recipient recovery key", sanitized?.message)
            assertNull(sanitized?.cause)
            assertFalse(sanitized.toString().contains("synthetic-private-provider-detail"))
        }
    }
}
