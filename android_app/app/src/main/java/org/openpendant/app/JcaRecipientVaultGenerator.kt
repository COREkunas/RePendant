package org.openpendant.app

import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.PrivateKey
import java.security.SecureRandom
import java.security.interfaces.ECPrivateKey
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import javax.security.auth.DestroyFailedException

/** Exportable recipient key generation, only through an explicit vault create.
 * Never called by a constructor, status probe, import or missing-key fallback.
 * The recipient is generated outside AndroidKeyStore; only its encrypted145B
 * recovery representation may be persisted by the vault wrapper/storage ports.
 * JCA/BigInteger/provider copies cannot guarantee physical memory erasure.
 */
class JcaRecipientVaultGenerator : RecipientVaultGenerator {
    override fun generate(): RecipientRecoveryKey {
        var generatedPrivate: PrivateKey? = null
        var scalar: ByteArray? = null
        var point: ByteArray? = null
        var x: ByteArray? = null
        var y: ByteArray? = null
        try {
            val generator = KeyPairGenerator.getInstance("EC")
            generator.initialize(ECGenParameterSpec("secp256r1"), SecureRandom())
            val pair = generator.generateKeyPair()
            generatedPrivate = pair.private
            val private = pair.private as? ECPrivateKey ?: throw RecipientVaultAdapterException()
            val public = pair.public as? ECPublicKey ?: throw RecipientVaultAdapterException()
            scalar = unsigned32(private.s)
            x = unsigned32(public.w.affineX)
            y = unsigned32(public.w.affineY)
            point = ByteArray(65)
            point[0] = 4
            x.copyInto(point, 1)
            y.copyInto(point, 33)
            // Full P-256 point/range/parameter and provider pair validation before
            // transferring owned copies. No unvalidated/generated material escapes.
            return RecipientRecoveryCodec.importP256(scalar, point)
        } catch (_: Exception) {
            throw RecipientVaultAdapterException()
        } finally {
            scalar?.fill(0); point?.fill(0); x?.fill(0); y?.fill(0)
            try { generatedPrivate?.destroy() } catch (_: DestroyFailedException) { }
            catch (_: RuntimeException) { }
        }
    }

    private fun unsigned32(value: BigInteger): ByteArray {
        if (value.signum() < 0 || value.bitLength() > 256) throw RecipientVaultAdapterException()
        val signed = value.toByteArray()
        try {
            val offset = if (signed.size == 33 && signed[0] == 0.toByte()) 1 else 0
            val length = signed.size - offset
            if (length !in 1..32) throw RecipientVaultAdapterException()
            return ByteArray(32).also { signed.copyInto(it, 32 - length, offset, signed.size) }
        } finally { signed.fill(0) }
    }
}
