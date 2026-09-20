package org.openpendant.app

import java.security.interfaces.ECPrivateKey

/**
 * Exportable SECRET recipient material, not an Android Keystore key or a file.
 * Do not log/copy private material except through an explicit recovery export.
 * close() best-effort wipes owned arrays. JCA providers, immutable BigInteger,
 * GC/JIT copies and a callback retaining a key cannot be securely erased here.
 * All callbacks are trusted, synchronous and must not retain/log the private key.
 */
class RecipientRecoveryKey private constructor(
    private val scalar: ByteArray,
    private val publicPoint: ByteArray,
) : AutoCloseable {
    private var closed = false
    private var usingKey = false

    @Synchronized fun publicKeyBytes(): ByteArray {
        checkOpen()
        return publicPoint.copyOf()
    }

    /** Full SHA-256 public fingerprint, not a secret or an ownership proof. */
    @Synchronized fun publicFingerprint(): ByteArray {
        checkOpen()
        return RecipientRecoveryCodec.fingerprint(publicPoint)
    }

    @Synchronized fun <T> withPrivateKey(action: (ECPrivateKey) -> T): T {
        checkOpen()
        check(!usingKey) { "Recipient key callback must not reenter" }
        usingKey = true
        try {
            return RecipientRecoveryCodec.withImportedPrivate(scalar, action)
        } finally {
            usingKey = false
        }
    }

    @Synchronized internal fun writePayload(destination: ByteArray) {
        checkOpen()
        check(!usingKey) { "Recipient key callback must not export" }
        require(destination.size == RecipientRecoveryCodec.ENCODED_BYTES)
        scalar.copyInto(destination, 16)
        publicPoint.copyInto(destination, 48)
    }

    @Synchronized override fun close() {
        check(!usingKey) { "Recipient key callback must not close its owner" }
        if (!closed) {
            scalar.fill(0)
            publicPoint.fill(0)
            closed = true
        }
    }

    override fun toString(): String = "RecipientRecoveryKey[REDACTED]"
    private fun checkOpen() = check(!closed) { "Recipient recovery key is closed" }

    companion object {
        /** Internal ownership transfer only after the codec's full validation. */
        internal fun validated(scalar: ByteArray, publicPoint: ByteArray) =
            RecipientRecoveryKey(scalar, publicPoint)
    }
}

/**
 * Plain, unencrypted bearer-key backup. The checksum is NOT encryption or
 * authentication. File protection, explicit consent and storage are not supplied.
 * A copied export belongs to its caller, who must wipe it when no longer needed.
 */
class RecipientRecoveryBytes internal constructor(private val ownedBytes: ByteArray) : AutoCloseable {
    private var closed = false

    @Synchronized fun copyForExplicitExport(): ByteArray {
        check(!closed) { "Recipient recovery export is closed" }
        return ownedBytes.copyOf()
    }

    @Synchronized override fun close() {
        if (!closed) {
            ownedBytes.fill(0)
            closed = true
        }
    }

    override fun toString(): String = "RecipientRecoveryBytes[REDACTED]"
}
