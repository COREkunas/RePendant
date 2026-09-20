package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID

/** Ciphertext-state token excludes alias creation. Returned arrays are caller-owned.
 * Empty is proven only when active/archive/staging files AND namespace aliases are absent.
 * Bounded corrupt bytes must be returned for explicit preservation/recovery; unsafe
 * staging residue, oversized or unreadable storage must throw without changing it.
 */
data class RecipientVaultStored(val token: String, val envelope: ByteArray?, val provablyEmpty: Boolean) {
    override fun toString() = "RecipientVaultStored(<redacted>)"
}
data class RecipientVaultWrapped(val iv: ByteArray, val ciphertext: ByteArray) {
    override fun toString() = "RecipientVaultWrapped(<redacted>)"
}

interface RecipientVaultStorage {
    fun read(): RecipientVaultStored
    /** Under the shared monitor: exact ciphertext-state CAS; preserve the old file
     * in a NEW archive before every replacement; AtomicFile+sync+readback. Never
     * normalize residue/delete aliases. Any failure, even after commit, must throw.
     */
    fun commit(expectedToken: String, envelope: ByteArray)
}

interface RecipientVaultWrapper {
    /** New UUID alias and new nonexportable AES-256 KEK only. Never replace/delete. */
    fun createAlias(): UUID
    /** Fresh provider-generated IV12 each call; AES-256-GCM tag16. Plaintext145. */
    fun encrypt(alias: UUID, plaintext: ByteArray, aad: ByteArray): RecipientVaultWrapped
    /** Authenticate complete AAD before returning exactly145 plaintext bytes. */
    fun decrypt(alias: UUID, iv: ByteArray, ciphertext: ByteArray, aad: ByteArray): ByteArray
}

fun interface RecipientVaultGenerator { fun generate(): RecipientRecoveryKey }
enum class RecipientVaultState { EMPTY, READY, RECOVERY_REQUIRED }
data class RecipientVaultSummary(val state: RecipientVaultState, val fingerprintHex: String? = null,
                                 val backupVerified: Boolean = false)
enum class RecipientVaultFailure {
    NOT_EMPTY, NO_ACTIVE_KEY, RECOVERY_REQUIRED, INVALID_BACKUP, DIFFERENT_RECIPIENT,
    BACKUP_NOT_VERIFIED, OPERATION_IN_PROGRESS, OPERATION_FAILED,
}
class RecipientVaultException(val failure: RecipientVaultFailure) :
    IllegalStateException("Recipient vault: ${failure.name}")

/**
 * Single active recipient, no rotation/deletion/automatic creation. Pure ports;
 * no files, Android Keystore, UI, device, network or owner-key generation here.
 * The injected generator runs ONLY during explicit create() after proven empty.
 * All instances/adapters in an app process MUST share the same monitor. This is
 * not an inter-process lock; production must not open the vault from other processes.
 *
 * Export is an explicit plain145B SECRET bearer backup, not a Keystore export or
 * password-encrypted file. Export alone never sets backupVerified. Readback must
 * decode/pair-validate and exactly match the active key before the authenticated
 * durable flag changes. Wrapped records contain no plaintext private material.
 *
 * Ambiguous mutation failure fences this instance. Missing/corrupt protection,
 * aliases without a file, or unsafe residue never authorize create(). Explicit
 * restore may reconcile only after a fresh readable storage snapshot; it keeps
 * old ciphertext/aliases and uses a new alias after any fault. A different key is
 * always rejected while the current recipient can still be authenticated/read.
 * Private holders are short-lived; JVM/provider copies cannot promise erasure.
 */
class RecipientKeyVault(
    private val storage: RecipientVaultStorage,
    private val wrapper: RecipientVaultWrapper,
    private val generator: RecipientVaultGenerator,
    private val sharedMonitor: Any,
) {
    private var fenced = false
    private var operating = false

    fun summary(): RecipientVaultSummary = synchronized(sharedMonitor) {
        if (operating) fail(RecipientVaultFailure.OPERATION_IN_PROGRESS)
        if (fenced) return@synchronized recoverySummary()
        operating = true
        try {
            inspect().use { view ->
                if (view.kind == RecipientVaultState.RECOVERY_REQUIRED) fenced = true
                view.summary()
            }
        } catch (_: Exception) {
            fenced = true
            recoverySummary()
        } finally { operating = false }
    }

    fun create(): RecipientVaultSummary = operation {
        ensureUnfenced()
        inspect().use { view ->
            when (view.kind) {
                RecipientVaultState.READY -> fail(RecipientVaultFailure.NOT_EMPTY)
                RecipientVaultState.RECOVERY_REQUIRED -> requireRecovery()
                RecipientVaultState.EMPTY -> Unit
            }
            mutation {
                generator.generate().use { key ->
                    val alias = wrapper.createAlias()
                    validAlias(alias)
                    persist(view.stored, key, alias, verified = false)
                }
            }
        }
    }

    fun export(): RecipientRecoveryBytes = operation {
        ensureUnfenced()
        inspect().use { view ->
            requireActive(view)
            RecipientRecoveryCodec.encode(view.key!!)
        }
    }

    fun verifyBackup(bytes: ByteArray): RecipientVaultSummary = operation {
        ensureUnfenced()
        decodeBackup(bytes).use { backup ->
            inspect().use { view ->
                requireActive(view)
                requireSameKey(view.key!!, backup)
                if (view.verified) view.summary()
                else persist(view.stored, view.key, view.alias!!, verified = true)
            }
        }
    }

    fun restore(bytes: ByteArray): RecipientVaultSummary = operation {
        decodeBackup(bytes).use { backup ->
            val wasFenced = fenced
            inspect().use { view ->
                if (view.kind == RecipientVaultState.READY) requireSameKey(view.key!!, backup)
                if (view.kind == RecipientVaultState.READY && !wasFenced) {
                    // Importing the same readable key is verification, never rotation.
                    if (view.verified) view.summary()
                    else persist(view.stored, view.key!!, view.alias!!, verified = true)
                } else {
                    mutation {
                        val alias = wrapper.createAlias()
                        validAlias(alias)
                        persist(view.stored, backup, alias, verified = true).also { fenced = false }
                    }
                }
            }
        }
    }

    /** Future trusted crypto/provisioning boundary; no key may escape its callback.
     * New recording/provisioning work remains unavailable until backup readback.
     */
    internal fun <T> withActiveKey(action: (RecipientRecoveryKey) -> T): T = operation {
        ensureUnfenced()
        inspect().use { view ->
            requireActive(view)
            if (!view.verified) fail(RecipientVaultFailure.BACKUP_NOT_VERIFIED)
            action(view.key!!)
        }
    }

    private fun persist(stored: RecipientVaultStored, key: RecipientRecoveryKey,
                        alias: UUID, verified: Boolean): RecipientVaultSummary = mutation {
        var fingerprint: ByteArray? = null
        var header: ByteArray? = null
        var plaintext: ByteArray? = null
        var wrapped: RecipientVaultWrapped? = null
        var envelope: ByteArray? = null
        try {
            fingerprint = key.publicFingerprint()
            header = header(alias, fingerprint, verified)
            plaintext = RecipientRecoveryCodec.encode(key).use { it.copyForExplicitExport() }
            wrapped = wrapper.encrypt(alias, plaintext, header)
            if (wrapped.iv.size != IV_BYTES || wrapped.ciphertext.size != CIPHERTEXT_BYTES)
                fail(RecipientVaultFailure.OPERATION_FAILED)
            envelope = header + wrapped.iv + wrapped.ciphertext
            // Authenticate/decode the proposed wrapper before writing anything.
            openEnvelope(stored.copy(envelope = envelope, provablyEmpty = false)).use { check ->
                if (check.kind != RecipientVaultState.READY || check.verified != verified)
                    fail(RecipientVaultFailure.OPERATION_FAILED)
                requireSameKey(key, check.key!!)
            }
            storage.commit(stored.token, envelope)
            RecipientVaultSummary(RecipientVaultState.READY, fingerprint.toHex(), verified)
        } finally {
            plaintext?.fill(0); fingerprint?.fill(0); header?.fill(0)
            wrapped?.iv?.fill(0); wrapped?.ciphertext?.fill(0); envelope?.fill(0)
        }
    }

    private fun inspect(): View {
        val stored = storage.read()
        if (stored.token.isEmpty() || stored.token.length > 256 ||
            (stored.envelope?.size ?: 0) > MAX_STORED_BYTES ||
            (stored.provablyEmpty && stored.envelope != null)) fail(RecipientVaultFailure.OPERATION_FAILED)
        if (stored.envelope == null) return View(
            if (stored.provablyEmpty) RecipientVaultState.EMPTY else RecipientVaultState.RECOVERY_REQUIRED, stored)
        return openEnvelope(stored)
    }

    private fun openEnvelope(stored: RecipientVaultStored): View {
        val envelope = stored.envelope ?: return View(RecipientVaultState.RECOVERY_REQUIRED, stored)
        var key: RecipientRecoveryKey? = null
        var plaintext: ByteArray? = null
        var transferred = false
        var aad: ByteArray? = null
        var iv: ByteArray? = null
        var ciphertext: ByteArray? = null
        var fingerprint: ByteArray? = null
        try {
            if (envelope.size != ENVELOPE_BYTES) return View(RecipientVaultState.RECOVERY_REQUIRED, stored)
            val reader = ByteBuffer.wrap(envelope).order(ByteOrder.BIG_ENDIAN)
            if (!MAGIC.indices.all { envelope[it] == MAGIC[it] } ||
                reader.getShort(8).toInt() != 1 || reader.getShort(10).toInt() != HEADER_BYTES ||
                envelope[60].toInt() !in 0..1 || !(61..63).all { envelope[it] == 0.toByte() } ||
                reader.getShort(64).toInt() != IV_BYTES || reader.getShort(66).toInt() != CIPHERTEXT_BYTES ||
                !(68..79).all { envelope[it] == 0.toByte() }) return View(RecipientVaultState.RECOVERY_REQUIRED, stored)
            val alias = UUID(reader.getLong(12), reader.getLong(20))
            if (!validOwnedUuid(alias)) return View(RecipientVaultState.RECOVERY_REQUIRED, stored)
            fingerprint = envelope.copyOfRange(28, 60)
            aad = envelope.copyOfRange(0, HEADER_BYTES)
            iv = envelope.copyOfRange(HEADER_BYTES, HEADER_BYTES + IV_BYTES)
            ciphertext = envelope.copyOfRange(HEADER_BYTES + IV_BYTES, ENVELOPE_BYTES)
            plaintext = wrapper.decrypt(alias, iv, ciphertext, aad)
            key = RecipientRecoveryCodec.decode(plaintext)
            val actual = key.publicFingerprint()
            try { if (!MessageDigest.isEqual(fingerprint, actual)) return View(RecipientVaultState.RECOVERY_REQUIRED, stored) }
            finally { actual.fill(0) }
            return View(RecipientVaultState.READY, stored, alias, envelope[60].toInt() == 1,
                fingerprint.toHex(), key).also { transferred = true }
        } catch (_: Exception) {
            return View(RecipientVaultState.RECOVERY_REQUIRED, stored)
        } finally {
            plaintext?.fill(0); aad?.fill(0); iv?.fill(0); ciphertext?.fill(0); fingerprint?.fill(0)
            if (!transferred) key?.close()
        }
    }

    private fun requireSameKey(active: RecipientRecoveryKey, backup: RecipientRecoveryKey) {
        val a = active.publicFingerprint(); val b = backup.publicFingerprint()
        var left: ByteArray? = null; var right: ByteArray? = null
        try {
            if (!MessageDigest.isEqual(a, b)) fail(RecipientVaultFailure.DIFFERENT_RECIPIENT)
            left = RecipientRecoveryCodec.encode(active).use { it.copyForExplicitExport() }
            right = RecipientRecoveryCodec.encode(backup).use { it.copyForExplicitExport() }
            if (!MessageDigest.isEqual(left, right)) fail(RecipientVaultFailure.DIFFERENT_RECIPIENT)
        } finally { a.fill(0); b.fill(0); left?.fill(0); right?.fill(0) }
    }

    private fun decodeBackup(bytes: ByteArray): RecipientRecoveryKey = try { RecipientRecoveryCodec.decode(bytes) }
        catch (_: Exception) { fail(RecipientVaultFailure.INVALID_BACKUP) }
    private fun requireActive(view: View) {
        when (view.kind) {
            RecipientVaultState.READY -> Unit
            RecipientVaultState.EMPTY -> fail(RecipientVaultFailure.NO_ACTIVE_KEY)
            RecipientVaultState.RECOVERY_REQUIRED -> requireRecovery()
        }
    }
    private fun requireRecovery(): Nothing { fenced = true; fail(RecipientVaultFailure.RECOVERY_REQUIRED) }
    private fun ensureUnfenced() { if (fenced) fail(RecipientVaultFailure.RECOVERY_REQUIRED) }
    /** Start before any alias/provider mutation, including allocation/validation.
     * Business rejections happen outside this boundary; all mutation failures are
     * ambiguous and remain fenced even when a port throws our exception type.
     */
    private fun <T> mutation(action: () -> T): T = try { action() }
        catch (_: Exception) { fenced = true; fail(RecipientVaultFailure.OPERATION_FAILED) }
        catch (error: Throwable) { fenced = true; throw error }
    private fun <T> operation(action: () -> T): T = synchronized(sharedMonitor) {
        if (operating) fail(RecipientVaultFailure.OPERATION_IN_PROGRESS)
        operating = true
        try { action() }
        catch (error: RecipientVaultException) { throw error }
        catch (_: Exception) { fenced = true; fail(RecipientVaultFailure.OPERATION_FAILED) }
        catch (error: Throwable) { fenced = true; throw error }
        finally { operating = false }
    }
    private class View(val kind: RecipientVaultState, val stored: RecipientVaultStored,
                       val alias: UUID? = null, val verified: Boolean = false,
                       val fingerprint: String? = null, val key: RecipientRecoveryKey? = null) : AutoCloseable {
        fun summary() = RecipientVaultSummary(kind, fingerprint, verified)
        override fun close() { key?.close() }
    }

    companion object {
        const val HEADER_BYTES = 80
        const val IV_BYTES = 12
        const val CIPHERTEXT_BYTES = 161
        const val ENVELOPE_BYTES = 253
        const val MAX_STORED_BYTES = 4096
        private val MAGIC = byteArrayOf(0x4f, 0x50, 0x4e, 0x44, 0x52, 0x56, 0x31, 0)
        private fun fail(reason: RecipientVaultFailure): Nothing = throw RecipientVaultException(reason)
        private fun recoverySummary() = RecipientVaultSummary(RecipientVaultState.RECOVERY_REQUIRED)
        private fun validAlias(alias: UUID) { if (!validOwnedUuid(alias)) fail(RecipientVaultFailure.OPERATION_FAILED) }
        private fun ByteArray.toHex() = joinToString("") { "%02x".format(it.toInt() and 255) }
        private fun header(alias: UUID, fingerprint: ByteArray, verified: Boolean): ByteArray {
            validAlias(alias)
            if (fingerprint.size != 32) fail(RecipientVaultFailure.OPERATION_FAILED)
            return ByteBuffer.allocate(HEADER_BYTES).order(ByteOrder.BIG_ENDIAN).apply {
                put(MAGIC); putShort(1); putShort(HEADER_BYTES.toShort())
                putLong(alias.mostSignificantBits); putLong(alias.leastSignificantBits)
                put(fingerprint); put(if (verified) 1.toByte() else 0.toByte()); put(ByteArray(3))
                putShort(IV_BYTES.toShort()); putShort(CIPHERTEXT_BYTES.toShort()); put(ByteArray(12))
            }.array()
        }
    }
}
