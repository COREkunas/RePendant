package org.openpendant.app

import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.GeneralSecurityException
import java.security.KeyFactory
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.Signature
import java.security.interfaces.ECPrivateKey
import java.security.interfaces.ECPublicKey
import java.security.spec.ECFieldFp
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPrivateKeySpec
import java.security.spec.ECPublicKeySpec
import javax.security.auth.DestroyFailedException

/** Fixed errors never incorporate input, private values or provider exception text. */
class RecipientRecoveryException internal constructor() :
    IllegalArgumentException("Invalid or unsupported recipient recovery key")

/**
 * Pure in-memory recovery encoding. No key generation, HPKE implementation,
 * password crypto, text conversion, filesystem, UI, Keystore, BLE or network.
 *
 * Exactly 145 bytes: OPNDRK1 NUL; BE16 version1/KEM0x0010/32/65;
 * scalar32 BE; SEC1 uncompressed public65; SHA256(first113bytes).
 * The plain export is a SECRET bearer key; its unkeyed checksum catches corruption
 * but neither encrypts the key nor authenticates its owner. Caller-owned input is
 * never changed; callers must wipe their own copies. Owned scratch is wiped on
 * normal/exceptional exits, but JVM/provider/BigInteger copies cannot be guaranteed.
 *
 * P-256 arithmetic here only validates PUBLIC curve/point parameters. Private-key
 * import and pairwise signature checks use JCA, without a pinned Android provider.
 * Android API26 provides EC AlgorithmParameters; actual device compatibility is
 * an integration test still required. No private material is sent to firmware.
 */
object RecipientRecoveryCodec {
    const val ENCODED_BYTES = 145
    const val PRIVATE_BYTES = 32
    const val PUBLIC_BYTES = 65
    const val KEM_ID = 0x0010
    private const val CHECKSUM_OFFSET = 113
    private val header = byteArrayOf(0x4f, 0x50, 0x4e, 0x44, 0x52, 0x4b, 0x31, 0, 0, 1, 0, 0x10, 0, 32, 0, 65)
    private val fingerprintDomain = "OpenPendant recipient public key v1\u0000".toByteArray(Charsets.US_ASCII)
    private val pairDomain = "OpenPendant recipient recovery pair check v1\u0000".toByteArray(Charsets.US_ASCII)
    private val p = hex("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff")
    private val a = p.subtract(BigInteger.valueOf(3))
    private val b = hex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b")
    private val gx = hex("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296")
    private val gy = hex("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")
    private val n = hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551")

    /** Exact bounds are checked before any copying or provider work. */
    fun importP256(scalar32: ByteArray, publicPoint65: ByteArray): RecipientRecoveryKey {
        invalidUnless(scalar32.size == PRIVATE_BYTES && publicPoint65.size == PUBLIC_BYTES)
        val scalar = scalar32.copyOf()
        var point: ByteArray? = null
        var transferred = false
        try {
            val publicCopy = publicPoint65.copyOf()
            point = publicCopy
            checkedProvider { validatePair(scalar, publicCopy) }
            return RecipientRecoveryKey.validated(scalar, publicCopy).also { transferred = true }
        } finally {
            if (!transferred) {
                scalar.fill(0)
                point?.fill(0)
            }
        }
    }

    fun decode(encoded145: ByteArray): RecipientRecoveryKey {
        invalidUnless(encoded145.size == ENCODED_BYTES)
        val encoded = encoded145.copyOf()
        var scalar: ByteArray? = null
        var point: ByteArray? = null
        var checksum: ByteArray? = null
        var storedChecksum: ByteArray? = null
        try {
            invalidUnless(header.indices.all { encoded[it] == header[it] })
            checksum = checkedProvider { digest(encoded, CHECKSUM_OFFSET) }
            storedChecksum = encoded.copyOfRange(CHECKSUM_OFFSET, ENCODED_BYTES)
            invalidUnless(MessageDigest.isEqual(checksum, storedChecksum))
            scalar = encoded.copyOfRange(16, 48)
            point = encoded.copyOfRange(48, CHECKSUM_OFFSET)
            return importP256(scalar, point)
        } finally {
            encoded.fill(0)
            scalar?.fill(0)
            point?.fill(0)
            checksum?.fill(0)
            storedChecksum?.fill(0)
        }
    }

    /** Explicit export only; returned bytes are unencrypted secret material. */
    fun encode(key: RecipientRecoveryKey): RecipientRecoveryBytes {
        val encoded = ByteArray(ENCODED_BYTES)
        var checksum: ByteArray? = null
        var transferred = false
        try {
            header.copyInto(encoded)
            key.writePayload(encoded)
            checksum = checkedProvider { digest(encoded, CHECKSUM_OFFSET) }
            checksum.copyInto(encoded, CHECKSUM_OFFSET)
            return RecipientRecoveryBytes(encoded).also { transferred = true }
        } finally {
            checksum?.fill(0)
            if (!transferred) encoded.fill(0)
        }
    }

    internal fun fingerprint(point: ByteArray): ByteArray = checkedProvider {
        MessageDigest.getInstance("SHA-256").run {
            update(fingerprintDomain)
            update(byteArrayOf(0, 0x10))
            digest(point)
        }
    }

    /** Shared full public-point validation for the HPKE receiver; no key generation. */
    internal fun importPublicP256(point65: ByteArray): ECPublicKey {
        invalidUnless(point65.size == PUBLIC_BYTES)
        return checkedProvider { publicKey(point65, parameters()) }
    }

    internal fun <T> withImportedPrivate(scalar: ByteArray, action: (ECPrivateKey) -> T): T {
        // Provider exceptions are sanitized at import only; trusted callback
        // exceptions belong to its caller and are never logged by this layer.
        val key = checkedProvider { privateKey(scalar, parameters()) }
        try { return action(key) } finally { destroyBestEffort(key) }
    }

    private fun validatePair(scalar: ByteArray, point: ByteArray) {
        val params = parameters()
        val public = publicKey(point, params)
        val private = privateKey(scalar, params)
        var signature: ByteArray? = null
        try {
            val signer = Signature.getInstance("SHA256withECDSA")
            signer.initSign(private)
            signer.update(pairDomain)
            signature = signer.sign()
            val verifier = Signature.getInstance("SHA256withECDSA")
            verifier.initVerify(public)
            verifier.update(pairDomain)
            invalidUnless(verifier.verify(signature))
        } finally {
            signature?.fill(0)
            destroyBestEffort(private)
        }
    }

    private fun privateKey(scalar: ByteArray, params: ECParameterSpec): ECPrivateKey {
        val value = BigInteger(1, scalar)
        invalidUnless(value.signum() > 0 && value < n)
        val key = KeyFactory.getInstance("EC").generatePrivate(ECPrivateKeySpec(value, params))
        if (key !is ECPrivateKey) {
            destroyBestEffort(key)
            throw RecipientRecoveryException()
        }
        try {
            invalidUnless(key.s == value)
            checkParameters(key.params)
            return key
        } catch (error: Throwable) {
            destroyBestEffort(key)
            throw error
        }
    }

    private fun publicKey(encoded: ByteArray, params: ECParameterSpec): ECPublicKey {
        invalidUnless(encoded[0] == 4.toByte())
        val xBytes = encoded.copyOfRange(1, 33)
        val yBytes = encoded.copyOfRange(33, 65)
        val x: BigInteger
        val y: BigInteger
        try { x = BigInteger(1, xBytes); y = BigInteger(1, yBytes) }
        finally { xBytes.fill(0); yBytes.fill(0) }
        invalidUnless(x < p && y < p)
        invalidUnless(y.multiply(y).mod(p) == x.multiply(x).multiply(x).add(a.multiply(x)).add(b).mod(p))
        val point = ECPoint(x, y)
        val key = KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(point, params))
        invalidUnless(key is ECPublicKey)
        key as ECPublicKey
        invalidUnless(key.w == point)
        checkParameters(key.params)
        return key
    }

    private fun parameters(): ECParameterSpec = AlgorithmParameters.getInstance("EC").run {
        init(ECGenParameterSpec("secp256r1"))
        getParameterSpec(ECParameterSpec::class.java).also { checkParameters(it) }
    }

    private fun checkParameters(params: ECParameterSpec) {
        val field = params.curve.field
        invalidUnless(field is ECFieldFp && field.p == p && params.curve.a == a && params.curve.b == b &&
            params.generator == ECPoint(gx, gy) && params.order == n && params.cofactor == 1)
    }

    private fun digest(bytes: ByteArray, count: Int): ByteArray = MessageDigest.getInstance("SHA-256").run {
        update(bytes, 0, count)
        digest()
    }
    private fun hex(publicConstant: String) = BigInteger(publicConstant, 16)
    private fun invalidUnless(condition: Boolean) { if (!condition) throw RecipientRecoveryException() }
    private fun <T> checkedProvider(action: () -> T): T = try { action() }
        catch (_: GeneralSecurityException) { throw RecipientRecoveryException() }
        catch (_: RuntimeException) { throw RecipientRecoveryException() }
    private fun destroyBestEffort(key: PrivateKey) {
        try { key.destroy() } catch (_: DestroyFailedException) { /* Most providers do not destroy keys. */ }
        catch (_: RuntimeException) { /* Provider cannot promise erasure. */ }
    }
}
