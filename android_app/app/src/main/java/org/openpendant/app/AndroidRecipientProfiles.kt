package org.openpendant.app

import android.content.Context

/** Additional recipients never replace the legacy active key or its aliases.
 * Fingerprints are public, bounded namespace selectors. There is no delete API.
 * Callers explicitly choose a local recovery file; no automatic key import. */
internal object AndroidRecipientProfiles {
    fun checkFingerprint(value: String) {
        require(isContentDigest(value) && value != "00".repeat(32) && value != "ff".repeat(32))
    }
    fun legacy(context: Context): RecipientKeyVault {
        val monitor = AndroidRecipientVaultOwner.monitor
        return RecipientKeyVault(AndroidRecipientVaultStorage(context, monitor),
            AndroidRecipientVaultWrapper(monitor), JcaRecipientVaultGenerator(), monitor)
    }
    fun profile(context: Context, fingerprint: String): RecipientKeyVault {
        checkFingerprint(fingerprint)
        val monitor = AndroidRecipientVaultOwner.monitor
        return RecipientKeyVault(AndroidRecipientVaultStorage.forRecipient(context, monitor, fingerprint),
            AndroidRecipientVaultWrapper.forRecipient(monitor, fingerprint), JcaRecipientVaultGenerator(), monitor)
    }
    fun selected(context: Context, fingerprint: String?): RecipientKeyVault {
        if (fingerprint == null) return legacy(context)
        val candidate = profile(context, fingerprint)
        // Corrupt/orphaned profile state must not silently fall back to another key.
        return if (candidate.summary().state == RecipientVaultState.EMPTY) legacy(context) else candidate
    }
    fun importMatching(context: Context, binding: DurablePublicBinding, bytes: ByteArray): RecipientVaultSummary {
        RecipientRecoveryCodec.decode(bytes).use { key ->
            val fingerprint = key.publicFingerprint()
            try { require(fingerprint.contentEquals(hexBytes(binding.recipientFingerprint))) }
            finally { fingerprint.fill(0) }
        }
        val summary = profile(context, binding.recipientFingerprint).restore(bytes)
        binding.requireVerifiedRecipient(summary)
        return summary
    }
}
