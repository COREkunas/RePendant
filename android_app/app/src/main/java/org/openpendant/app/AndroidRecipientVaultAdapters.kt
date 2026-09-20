package org.openpendant.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyInfo
import android.security.keystore.KeyProperties
import android.system.Os
import android.system.OsConstants
import android.util.AtomicFile
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.security.KeyStore
import java.security.MessageDigest
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec

/** All production service/storage/wrapper instances must share this monitor.
 * One Android app process is supported; this is not a cross-process lock.
 * Construction has no filesystem or Keystore effects.
 */
object AndroidRecipientVaultOwner { val monitor: Any = Any() }

class RecipientVaultAdapterException internal constructor() :
    IllegalStateException("Recipient key storage needs recovery; existing data was kept")

private const val ALIAS_PREFIX = "openpendant.recipient.wrap.v1."
private const val DIRECTORY_NAME = "recipient-vault-v1"
private const val ACTIVE_NAME = "active.bin"
private const val ENVELOPE_BYTES = 253
private const val HEADER_BYTES = 80
private const val IV_BYTES = 12
private const val CIPHER_BYTES = 161
private const val MAX_STORED_BYTES = 4096
private const val MAX_ENTRIES = 256

private fun adapterCheck(ok: Boolean) { if (!ok) throw RecipientVaultAdapterException() }
private inline fun <T> adapterBoundary(action: () -> T): T = try { action() }
    catch (_: Exception) { throw RecipientVaultAdapterException() }
private fun keyStore(): KeyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
private fun alias(id: UUID, prefix: String): String = prefix + id.toString()
private fun hasNamespaceAliases(prefix: String): Boolean {
    val entries = keyStore().aliases()
    var count = 0
    while (entries.hasMoreElements()) {
        adapterCheck(++count <= 4096)
        if (entries.nextElement().startsWith(prefix)) return true
    }
    return false
}

/** Nonexportable wrapping keys only. Recipient private keys never enter Keystore.
 * Only createAlias mutates key state. Reads/encrypt/decrypt never create, replace
 * or delete an alias. Encryption IV is chosen by the Android Keystore provider.
 * No hardware-security-level claim: that needs a device-specific check.
 */
class AndroidRecipientVaultWrapper private constructor(private val monitor: Any, private val aliasPrefix: String) : RecipientVaultWrapper {
    constructor(monitor: Any) : this(monitor, ALIAS_PREFIX)

    companion object {
        /** Explicit test-only namespace; never called by production construction. */
        internal fun forSyntheticTests(monitor: Any, testId: UUID) =
            AndroidRecipientVaultWrapper(monitor, "openpendant.recipient.test.$testId.")
    }
    override fun createAlias(): UUID = synchronized(monitor) {
        adapterBoundary {
            val id = UUID.randomUUID()
            val store = keyStore()
            adapterCheck(!store.containsAlias(alias(id, aliasPrefix)))
            val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
            generator.init(KeyGenParameterSpec.Builder(alias(id, aliasPrefix),
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .setUserAuthenticationRequired(false)
                .build())
            generator.generateKey()
            // If validation fails after creation, leave the orphan alias intact.
            // Storage's missing-file probe then fails closed instead of EMPTY.
            existingKey(store, id)
            id
        }
    }

    override fun encrypt(alias: UUID, plaintext: ByteArray, aad: ByteArray): RecipientVaultWrapped = synchronized(monitor) {
        adapterBoundary {
            adapterCheck(plaintext.size == RecipientRecoveryCodec.ENCODED_BYTES && aad.size == HEADER_BYTES)
            val input = plaintext.copyOf()
            val associated = aad.copyOf()
            var iv: ByteArray? = null
            var encrypted: ByteArray? = null
            var transferred = false
            try {
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.ENCRYPT_MODE, existingKey(keyStore(), alias)) // NO caller IV.
                iv = cipher.iv
                adapterCheck(iv != null && iv.size == IV_BYTES &&
                    cipher.parameters.getParameterSpec(GCMParameterSpec::class.java).tLen == 128)
                cipher.updateAAD(associated)
                encrypted = cipher.doFinal(input)
                adapterCheck(encrypted.size == CIPHER_BYTES)
                RecipientVaultWrapped(iv, encrypted).also { transferred = true }
            } finally {
                input.fill(0); associated.fill(0)
                if (!transferred) { iv?.fill(0); encrypted?.fill(0) }
            }
        }
    }

    override fun decrypt(alias: UUID, iv: ByteArray, ciphertext: ByteArray, aad: ByteArray): ByteArray = synchronized(monitor) {
        adapterBoundary {
            adapterCheck(iv.size == IV_BYTES && ciphertext.size == CIPHER_BYTES && aad.size == HEADER_BYTES)
            val nonce = iv.copyOf()
            val encrypted = ciphertext.copyOf()
            val associated = aad.copyOf()
            var plaintext: ByteArray? = null
            var transferred = false
            try {
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.DECRYPT_MODE, existingKey(keyStore(), alias), GCMParameterSpec(128, nonce))
                cipher.updateAAD(associated)
                plaintext = cipher.doFinal(encrypted) // No update/plaintext before tag authentication.
                adapterCheck(plaintext.size == RecipientRecoveryCodec.ENCODED_BYTES)
                plaintext.also { transferred = true }
            } finally {
                nonce.fill(0); encrypted.fill(0); associated.fill(0)
                if (!transferred) plaintext?.fill(0)
            }
        }
    }

    private fun existingKey(store: KeyStore, id: UUID): SecretKey {
        adapterCheck(store.containsAlias(alias(id, aliasPrefix)))
        val entry = store.getEntry(alias(id, aliasPrefix), null) as? KeyStore.SecretKeyEntry
            ?: throw RecipientVaultAdapterException()
        val key = entry.secretKey
        val info = SecretKeyFactory.getInstance("AES", "AndroidKeyStore")
            .getKeySpec(key, KeyInfo::class.java) as? KeyInfo ?: throw RecipientVaultAdapterException()
        adapterCheck(key.algorithm == "AES" && info.keystoreAlias == alias(id, aliasPrefix) && info.keySize == 256 &&
            info.purposes == (KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT) &&
            info.blockModes.toSet() == setOf(KeyProperties.BLOCK_MODE_GCM) &&
            info.encryptionPaddings.toSet() == setOf(KeyProperties.ENCRYPTION_PADDING_NONE) &&
            info.origin == KeyProperties.ORIGIN_GENERATED)
        return key // Never call encoded/getEncoded on this nonexportable key.
    }
}

/** Only ciphertext + authenticated public header is persisted. No secrets/logs.
 * read is deliberately raw/nonmutating: AtomicFile.openRead may restore/delete
 * recovery artifacts. Any .new/.bak residue makes ordinary read/write refuse.
 * Replacements preserve old bounded bytes first in new, never-overwritten files.
 * Faults preserve bounded pending artifacts before rollback; if preservation
 * fails, close the stream without normalizing/deleting artifacts and fail closed.
 * Android AtomicFile requires caller locking; all calls use the shared monitor.
 * https://developer.android.com/reference/android/util/AtomicFile
 */
class AndroidRecipientVaultStorage private constructor(context: Context, private val monitor: Any,
    private val directoryName: String, private val aliasPrefix: String) : RecipientVaultStorage {
    constructor(context: Context, monitor: Any) : this(context, monitor, DIRECTORY_NAME, ALIAS_PREFIX)

    companion object {
        /** Dedicated generated namespace only. No test ever reads owner storage. */
        internal fun forSyntheticTests(context: Context, monitor: Any, testId: UUID) =
            AndroidRecipientVaultStorage(context, monitor, "recipient-vault-test-$testId",
                "openpendant.recipient.test.$testId.")
    }
    private val appContext = context.applicationContext

    override fun read(): RecipientVaultStored = synchronized(monitor) {
        adapterBoundary { readLocked() }
    }

    override fun commit(expectedToken: String, envelope: ByteArray): Unit = synchronized(monitor) {
        adapterBoundary {
            adapterCheck(envelope.size == ENVELOPE_BYTES)
            val input = envelope.copyOf()
            var prior: ByteArray? = null
            try {
                val state = readLocked()
                prior = state.envelope
                adapterCheck(state.token == expectedToken)
                val directory = directory(create = true) ?: throw RecipientVaultAdapterException()
                if (prior != null) archive(directory, prior, "archive")
                writeAtomic(directory, input)
                val observed = readLocked()
                try { adapterCheck(observed.envelope?.contentEquals(input) == true) }
                finally { observed.envelope?.fill(0) }
            } finally { input.fill(0); prior?.fill(0) }
        }
    }

    private fun readLocked(): RecipientVaultStored {
        val directory = directory(create = false)
        if (directory == null) return RecipientVaultStored("missing", null, !hasNamespaceAliases(aliasPrefix))
        val files = directory.listFiles() ?: throw RecipientVaultAdapterException()
        adapterCheck(files.size <= MAX_ENTRIES)
        var history = false
        for (entry in files) {
            adapterCheck(!java.nio.file.Files.isSymbolicLink(entry.toPath()) && entry.isFile)
            when {
                entry.name == ACTIVE_NAME -> Unit
                entry.name == "$ACTIVE_NAME.new" || entry.name == "$ACTIVE_NAME.bak" -> throw RecipientVaultAdapterException()
                archiveName(entry.name, "archive") || archiveName(entry.name, "residue") -> history = true
                else -> throw RecipientVaultAdapterException()
            }
        }
        val active = File(directory, ACTIVE_NAME)
        if (!active.exists()) return RecipientVaultStored("missing", null, !history && !hasNamespaceAliases(aliasPrefix))
        val bytes = readBounded(active)
        val digest = MessageDigest.getInstance("SHA-256").digest(bytes)
        val token = "file:${bytes.size}:" + digest.joinToString("") { "%02x".format(it.toInt() and 0xff) }
        digest.fill(0)
        return RecipientVaultStored(token, bytes, false)
    }

    private fun directory(create: Boolean): File? {
        // Android owns noBackupFilesDir; no module directory is created by a probe.
        val root = appContext.noBackupFilesDir.canonicalFile
        adapterCheck(root.isDirectory)
        val directory = File(root, directoryName)
        adapterCheck(!java.nio.file.Files.isSymbolicLink(directory.toPath()))
        if (!directory.exists()) {
            if (!create) return null
            adapterCheck(directory.mkdir())
            Os.chmod(directory.absolutePath, 448) //0700; never broaden permissions.
            syncDirectory(root)
        }
        adapterCheck(directory.isDirectory && directory.canonicalFile.parentFile == root)
        return directory
    }

    private fun archiveName(name: String, prefix: String): Boolean {
        if (!name.startsWith("$prefix-") || !name.endsWith(".bin")) return false
        val text = name.removePrefix("$prefix-").removeSuffix(".bin")
        return try { UUID.fromString(text).toString() == text } catch (_: IllegalArgumentException) { false }
    }

    private fun readBounded(file: File): ByteArray {
        val scratch = ByteArray(MAX_STORED_BYTES + 1)
        val descriptor = Os.open(file.absolutePath, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW, 0)
        try {
            FileInputStream(descriptor).use { input ->
                val stat = Os.fstat(descriptor)
                adapterCheck(OsConstants.S_ISREG(stat.st_mode) && stat.st_size in 0..MAX_STORED_BYTES.toLong())
                var count = 0
                while (count < scratch.size) {
                    val n = input.read(scratch, count, scratch.size - count)
                    if (n < 0) break
                    adapterCheck(n > 0)
                    count += n
                }
                adapterCheck(count <= MAX_STORED_BYTES && count.toLong() == stat.st_size)
                return scratch.copyOf(count)
            }
        } finally {
            scratch.fill(0)
            // Android streams constructed from an existing FileDescriptor do
            // not own it. This adapter owns every descriptor returned by Os.open.
            if (descriptor.valid()) Os.close(descriptor)
        }
    }

    private fun archive(directory: File, bytes: ByteArray, prefix: String) {
        adapterCheck(bytes.size <= MAX_STORED_BYTES)
        val target = File(directory, "$prefix-${UUID.randomUUID()}.bin")
        val descriptor = Os.open(target.absolutePath,
            OsConstants.O_WRONLY or OsConstants.O_CREAT or OsConstants.O_EXCL or OsConstants.O_NOFOLLOW, 384) //0600
        try {
            FileOutputStream(descriptor).use { output ->
                output.write(bytes)
                output.flush()
                output.fd.sync()
            }
        } finally { if (descriptor.valid()) Os.close(descriptor) }
        syncDirectory(directory)
        val observed = readBounded(target)
        try { adapterCheck(observed.contentEquals(bytes)) } finally { observed.fill(0) }
        // Even a failed/partial archive is retained. Never overwrite or delete it.
    }

    private fun writeAtomic(directory: File, bytes: ByteArray) {
        val active = File(directory, ACTIVE_NAME)
        adapterCheck(!File(directory, "$ACTIVE_NAME.new").exists() && !File(directory, "$ACTIVE_NAME.bak").exists())
        val atomic = AtomicFile(active)
        var pending: FileOutputStream? = null
        try {
            pending = atomic.startWrite()
            Os.fchmod(pending.fd, 384) //0600
            pending.write(bytes)
            pending.flush()
            pending.fd.sync() //Catch sync failure rather than rely on AtomicFile logging.
            atomic.finishWrite(pending)
            pending = null
            syncDirectory(directory)
            val check = readBounded(active)
            try { adapterCheck(check.contentEquals(bytes)) } finally { check.fill(0) }
            adapterCheck(!File(directory, "$ACTIVE_NAME.new").exists() && !File(directory, "$ACTIVE_NAME.bak").exists())
        } catch (_: Exception) {
            if (pending != null) {
                try {
                    // Preserve all bounded current/legacy/new artifacts before
                    // asking AtomicFile to remove its transient write on rollback.
                    for (name in arrayOf(ACTIVE_NAME, "$ACTIVE_NAME.new", "$ACTIVE_NAME.bak")) {
                        val residue = File(directory, name)
                        if (residue.exists()) {
                            val contents = readBounded(residue)
                            try { archive(directory, contents, "residue") } finally { contents.fill(0) }
                        }
                    }
                    atomic.failWrite(pending)
                    pending = null
                    syncDirectory(directory)
                } catch (_: Exception) {
                    // Preservation/rollback uncertain: leave every artifact as-is.
                    // Closing is only resource cleanup, NOT a commit or retry.
                }
            }
            throw RecipientVaultAdapterException()
        } finally {
            try { pending?.close() } catch (_: Exception) { }
        }
    }

    private fun syncDirectory(directory: File) {
        val descriptor = Os.open(directory.absolutePath,
            OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW, 0)
        try {
            adapterCheck(OsConstants.S_ISDIR(Os.fstat(descriptor).st_mode))
            Os.fsync(descriptor)
        } finally { Os.close(descriptor) }
    }
}
