package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

/** Disposable host-test keys only: no file, Android Keystore, owner or device. */
class JcaRecipientVaultGeneratorTest {
    @Test fun generatedRecipientHasAValidatedRecoverableP256Pair() {
        JcaRecipientVaultGenerator().generate().use { generated ->
            assertEquals(65, generated.publicKeyBytes().size)
            assertEquals(4.toByte(), generated.publicKeyBytes()[0])
            assertEquals(32, generated.publicFingerprint().size)
            assertEquals("RecipientRecoveryKey[REDACTED]", generated.toString())
            generated.withPrivateKey { assertEquals(256, it.params.curve.field.fieldSize) }
            RecipientRecoveryCodec.encode(generated).use { export ->
                val bytes = export.copyForExplicitExport()
                try {
                    assertEquals(145, bytes.size)
                    RecipientRecoveryCodec.decode(bytes).use { restored ->
                        assertArrayEquals(generated.publicFingerprint(), restored.publicFingerprint())
                        assertArrayEquals(generated.publicKeyBytes(), restored.publicKeyBytes())
                    }
                } finally { bytes.fill(0) }
            }
        }
    }

    @Test fun explicitGenerationDoesNotReturnCachedTestOrRecipientKeys() {
        val generator = JcaRecipientVaultGenerator()
        generator.generate().use { first ->
            generator.generate().use { second ->
                assertFalse(first.publicFingerprint().contentEquals(second.publicFingerprint()))
            }
        }
    }
}
