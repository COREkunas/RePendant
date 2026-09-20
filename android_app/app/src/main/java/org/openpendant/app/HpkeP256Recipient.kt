package org.openpendant.app

import java.security.GeneralSecurityException
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec
import javax.security.auth.DestroyFailedException

/** No input/provider text, secret diagnostics or nested causes. */
class EncryptedSegmentException internal constructor() :
    IllegalArgumentException("Invalid or unauthenticated encrypted segment")

/** Authenticated plaintext owner. No audio decoding/publication happens here.
 * close() best-effort wipes owned bytes; callers must wipe their explicit copies.
 */
class DecryptedSegmentBytes internal constructor(private val ownedBytes: ByteArray) : AutoCloseable {
    private var closed = false
    @Synchronized fun copyForAuthenticatedUse(): ByteArray {
        check(!closed) { "Decrypted segment is closed" }
        return ownedBytes.copyOf()
    }
    @Synchronized override fun close() {
        if (!closed) { ownedBytes.fill(0); closed = true }
    }
    override fun toString(): String = "DecryptedSegmentBytes[REDACTED]"
}

/**
 * Dormant single-shot RFC9180 receiver: base mode, KEM16/KDF1/AEAD2, sequence0.
 * JCA performs ECDH, HMAC-SHA256 and AES-256-GCM. This is the RFC's labeled KDF
 * composition, not custom elliptic-curve crypto, encryption or key generation.
 * https://www.rfc-editor.org/rfc/rfc9180.html sections4.1/5.1/5.2/6.1.
 *
 * Internal primitive permits bounded info/AAD for the PUBLIC RFC test vector.
 * The application wrapper below supplies fixed161-byte info and EMPTY AAD.
 * No contexts, counters, retries, alternate suites, streaming plaintext, files,
 * UI, transport, playback or transcription. All input spans must remain stable
 * for the synchronous call. Arrays are snapshotted before crypto.
 *
 * Owned secret scratch is wiped on all exits. JCA/Mac/Cipher/KeyAgreement,
 * SecretKeySpec/GCMParameterSpec/BigInteger and JVM copies cannot promise erasure.
 * Base HPKE does NOT authenticate sender identity, prevent replay or hide lengths;
 * compromise of recipient private key exposes its past ciphertexts as well.
 */
internal object HpkeP256Recipient {
    const val CONTEXT_MAX = 256
    private val version = "HPKE-v1".toByteArray(Charsets.US_ASCII)
    private val kemSuite = byteArrayOf(0x4b, 0x45, 0x4d, 0, 0x10)
    private val hpkeSuite = byteArrayOf(0x48, 0x50, 0x4b, 0x45, 0, 0x10, 0, 1, 0, 2)

    fun open(key: RecipientRecoveryKey, enc65: ByteArray, info: ByteArray,
             aad: ByteArray, ciphertextAndTag: ByteArray): DecryptedSegmentBytes {
        valid(enc65.size == 65 && info.size <= CONTEXT_MAX && aad.size <= CONTEXT_MAX &&
            ciphertextAndTag.size in 16..(EncryptedSegmentHeader.MAX_PLAINTEXT_BYTES + 16))
        val scratch = WipeScope()
        var plaintext: ByteArray? = null
        var transferred = false
        try {
            val enc = scratch.own(enc65.copyOf())
            val contextInfo = scratch.own(info.copyOf())
            val associated = scratch.own(aad.copyOf())
            val ciphertext = scratch.own(ciphertextAndTag.copyOf())
            val ephemeral = RecipientRecoveryCodec.importPublicP256(enc)
            val result = key.withPrivateKey { privateKey ->
                val recipient = scratch.own(key.publicKeyBytes())
                val agreement = KeyAgreement.getInstance("ECDH")
                agreement.init(privateKey)
                agreement.doPhase(ephemeral, true)
                val dh = scratch.own(agreement.generateSecret())
                valid(dh.size == 32 && dh.any { it != 0.toByte() })
                val kemContext = scratch.own(enc + recipient)
                val eaePrk = scratch.own(extract(kemSuite, byteArrayOf(), "eae_prk", dh))
                dh.fill(0)
                val shared = scratch.own(expand(kemSuite, eaePrk, "shared_secret", kemContext, 32))
                eaePrk.fill(0)
                val pskIdHash = scratch.own(extract(hpkeSuite, byteArrayOf(), "psk_id_hash", byteArrayOf()))
                val infoHash = scratch.own(extract(hpkeSuite, byteArrayOf(), "info_hash", contextInfo))
                val schedule = scratch.own(byteArrayOf(0) + pskIdHash + infoHash)
                val secret = scratch.own(extract(hpkeSuite, shared, "secret", byteArrayOf()))
                shared.fill(0)
                val aesKey = scratch.own(expand(hpkeSuite, secret, "key", schedule, 32))
                val nonce = scratch.own(expand(hpkeSuite, secret, "base_nonce", schedule, 12))
                secret.fill(0)
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                val secretSpec = SecretKeySpec(aesKey, "AES")
                try {
                    cipher.init(Cipher.DECRYPT_MODE, secretSpec, GCMParameterSpec(128, nonce))
                    if (associated.isNotEmpty()) cipher.updateAAD(associated)
                    // Never call update(ciphertext): no tentative plaintext is
                    // returned or published before doFinal authenticates the tag.
                    val authenticated = cipher.doFinal(ciphertext)
                    plaintext = authenticated
                    valid(authenticated.size == ciphertext.size - 16)
                    DecryptedSegmentBytes(authenticated)
                } finally { destroyBestEffort(secretSpec) }
            }
            transferred = true
            return result
        } catch (_: GeneralSecurityException) {
            throw EncryptedSegmentException()
        } catch (_: RuntimeException) {
            throw EncryptedSegmentException()
        } finally {
            scratch.close()
            if (!transferred) plaintext?.fill(0)
        }
    }

    private fun extract(suite: ByteArray, salt: ByteArray, label: String, ikm: ByteArray): ByteArray {
        val labeled = version + suite + label.toByteArray(Charsets.US_ASCII) + ikm
        try { return hmac(salt, labeled) } finally { labeled.fill(0) }
    }

    /** Every fixed suite output is <=32B: RFC5869 expansion needs exactly T(1). */
    private fun expand(suite: ByteArray, prk: ByteArray, label: String, info: ByteArray, wanted: Int): ByteArray {
        valid(wanted in 1..32 && prk.size == 32)
        val labeled = byteArrayOf(0, wanted.toByte()) + version + suite +
            label.toByteArray(Charsets.US_ASCII) + info + byteArrayOf(1)
        var block: ByteArray? = null
        try {
            block = hmac(prk, labeled)
            return block.copyOf(wanted)
        } finally { labeled.fill(0); block?.fill(0) }
    }

    private fun hmac(key: ByteArray, message: ByteArray): ByteArray {
        // RFC5869 empty salt uses HashLen zero bytes. JCA rejects a zero-length
        // SecretKeySpec, so supply that equivalent standardized salt explicitly.
        val emptySalt = if (key.isEmpty()) ByteArray(32) else null
        val spec = SecretKeySpec(emptySalt ?: key, "HmacSHA256")
        try {
            val mac = Mac.getInstance("HmacSHA256")
            mac.init(spec)
            val result = mac.doFinal(message)
            if (result.size != 32) { result.fill(0); throw EncryptedSegmentException() }
            return result
        } finally { emptySalt?.fill(0); destroyBestEffort(spec) }
    }

    private fun destroyBestEffort(spec: SecretKeySpec) {
        try { spec.destroy() } catch (_: DestroyFailedException) { }
        catch (_: RuntimeException) { }
    }
    private fun valid(condition: Boolean) { if (!condition) throw EncryptedSegmentException() }
    private class WipeScope : AutoCloseable {
        private val arrays = ArrayList<ByteArray>(20)
        fun own(bytes: ByteArray): ByteArray = bytes.also { arrays.add(it) }
        override fun close() { arrays.forEach { it.fill(0) }; arrays.clear() }
    }
}
