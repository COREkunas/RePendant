package org.openpendant.app

import java.io.Closeable
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.nio.file.StandardCopyOption.ATOMIC_MOVE
import java.nio.file.StandardCopyOption.REPLACE_EXISTING
import java.nio.file.StandardOpenOption.*
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest

/** Trusted catalog input, never inferred from the received container. Public
 * identity and ciphertext digest only; this is not tag authentication. */
class EncryptedSegmentExpectation(val segment: SegmentIdentity, fingerprint: ByteArray, val plaintextBytes: Int) {
    private val header = EncryptedSegmentHeader.encode(segment.recording, fingerprint.copyOf(), segment.sequence, plaintextBytes)
    internal val totalBytes: Int = EncryptedSegmentHeader.HEADER_BYTES + EncryptedSegmentHeader.ENCAP_BYTES + plaintextBytes + EncryptedSegmentHeader.TAG_BYTES
    init { require(segment.byteCount == totalBytes.toLong()) { "Encrypted catalog length differs" } }
    internal fun headerBytes() = header.copyOf()
    internal fun bindingBytes() = header + hexBytes(segment.sha256)
    // The digest/fingerprint are deliberately NOT part of this slot name: changing
    // a sealed identity's expected bytes must conflict, not create another copy.
    internal val slot: String = digestHex(header.copyOfRange(52, 112))
    override fun toString() = "EncryptedSegmentExpectation(publicIdentity, sequence=${segment.sequence}, bytes=$totalBytes)"
}

fun interface SegmentDirectorySync { fun sync(directory: Path) }
/** Platform query for the already validated private directory. A query failure
 * is not permission to omit the disk-space admission check. */
internal fun interface SegmentUsableSpace { fun bytes(directory: Path): Long }
/** Must atomically publish the original inode without replacing any destination.
 * A moved source or a retained same-inode hard link is supported; no copy fallback. */
internal fun interface SegmentPublication { fun publish(directory: Path, source: Path, destination: Path) }
/** Must return a real filesystem identity stable across rename and hard links,
 * with exact regular-file/directory type validation. Null/path/content-derived
 * identities are not substitutes. Android supplies device+inode through lstat.
 * JVM platforms lacking BasicFileAttributes.fileKey fail closed by default. */
internal fun interface SegmentFileIdentity { fun key(path: Path, isDirectory: Boolean): Any }
internal fun checkedSegmentUsableSpace(availableBlocks: Long, fragmentBytes: Long,
                                      freeBlocks: Long, totalBlocks: Long): Long {
    if (fragmentBytes <= 0 || totalBlocks < 0 || freeBlocks < 0 || availableBlocks < 0 ||
        availableBlocks > freeBlocks || freeBlocks > totalBlocks ||
        availableBlocks > Long.MAX_VALUE / fragmentBytes) throw SegmentStorageException()
    return availableBlocks * fragmentBytes
}
class SegmentStorageException : IllegalStateException("Encrypted segment storage needs attention; no receipt issued")
class SegmentStorageBusyException : IllegalStateException("Encrypted segment storage is currently busy; no receipt issued")
internal enum class SegmentDiskStep { PART_SYNCED, CHECKPOINT_SYNCED, CHECKPOINT_RENAMED, CONTAINER_LINKED, DIRECTORY_SYNCED }

/** Dedicated app-private ciphertext directory. Use AndroidDurableSegments.create
 * in production; the explicit sync port must really fsync a directory, not just
 * flush a stream. Constructor only checks its existing directory, never opens
 * a recording. Work is explicit and blocking: call off the Android main thread.
 *
 * An OS file lock serializes all sessions, including cooperating processes.
 * Existing RecordingSyncContract is still the publication/deletion authority.
 * Files outside this dedicated directory must not be supplied. No cleanup of
 * unknown files, key access, receipt transport or source deletion is performed.
 *
 * Publication uses a platform atomic no-replace inode operation, followed by
 * directory fsync (desktop hard link; Android renameat2 RENAME_NOREPLACE).
 * Unsupported publication/atomic checkpoint moves/fsync fail closed, without a
 * copying or overwriting fallback.
 * Checkpoint rename may replace ONLY this slot's own checkpoint. A crash after
 * publication but before coordinator metadata commit is reconciled by reopening
 * and re-verifying the complete file, then a fresh coordinator download ticket.
 */
class DurableSegmentStore private constructor(
    directory: File,
    private val directorySync: SegmentDirectorySync,
    private val observe: (SegmentDiskStep) -> Unit,
    private val diagnostic: ((String,String,Int)->Unit)?,
    private val usableSpace: SegmentUsableSpace,
    private val publication: SegmentPublication,
    private val identity: SegmentFileIdentity,
) {
    constructor(directory: File, directorySync: SegmentDirectorySync) :
        this(directory, directorySync, {}, null, DESKTOP_USABLE_SPACE, DESKTOP_PUBLICATION, DESKTOP_IDENTITY)
    internal constructor(directory: File, directorySync: SegmentDirectorySync, fault: (SegmentDiskStep) -> Unit,
                         @Suppress("UNUSED_PARAMETER") testOnly: Unit) :
        this(directory, directorySync, fault, null, DESKTOP_USABLE_SPACE, DESKTOP_PUBLICATION, DESKTOP_IDENTITY)
    internal constructor(directory: File, directorySync: SegmentDirectorySync, usableSpace: SegmentUsableSpace,
                         diagnostic: ((String,String,Int)->Unit)? = null,
                         publication: SegmentPublication = DESKTOP_PUBLICATION,
                         identity: SegmentFileIdentity = DESKTOP_IDENTITY) :
        this(directory, directorySync, {}, diagnostic, usableSpace, publication, identity)
    internal constructor(directory: File, directorySync: SegmentDirectorySync, publication: SegmentPublication,
                         fault: (SegmentDiskStep)->Unit, @Suppress("UNUSED_PARAMETER") testOnly:Unit,
                         identity: SegmentFileIdentity = DESKTOP_IDENTITY) :
        this(directory,directorySync,fault,null,DESKTOP_USABLE_SPACE,publication,identity)
    internal constructor(directory: File, directorySync: SegmentDirectorySync, identity: SegmentFileIdentity,
                         fault: (SegmentDiskStep)->Unit = {}) :
        this(directory,directorySync,fault,null,DESKTOP_USABLE_SPACE,DESKTOP_PUBLICATION,identity)
    private val root = directory.toPath().toAbsolutePath().normalize()
    // Read-only failure cleanup can fence after releasing its OS file lock;
    // another cooperating worker may already be waiting to inspect that slot.
    private val fenced = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()
    init {
        require(Files.isDirectory(root, NOFOLLOW_LINKS) && root.toRealPath() == root) { "Dedicated segment directory is unavailable" }
    }

    /** Reopen/reconcile only this expected immutable segment, never scan audio.
     * Every returned offset has an fsynced checkpoint and a re-hashed prefix. */
    internal fun open(expected: EncryptedSegmentExpectation, createIfAbsent: Boolean = true): Session {
        var channel: FileChannel? = null
        var lock: FileLock? = null
        var phase="root_validation"
        try {
            checkRoot()
            phase="store_lock_open"
            channel = FileChannel.open(root.resolve(".store.lock"), CREATE, WRITE, NOFOLLOW_LINKS)
            phase="store_lock_acquire"
            lock = try { channel.tryLock() ?: throw SegmentStorageBusyException() }
                catch (_: OverlappingFileLockException) { throw SegmentStorageBusyException() }
            val session = Session(expected, channel, lock)
            phase="recover"
            session.recover(createIfAbsent)
            return session
        } catch (failure: Exception) {
            try { lock?.release() } finally { channel?.close() }
            if (failure is SegmentStorageBusyException) throw failure
            reportDiagnostic(phase,failure)
            throw SegmentStorageException()
        }
    }

    /** Explicit integrity inspection; does not play/decrypt or issue a receipt. */
    fun verifiedOnDisk(expected: EncryptedSegmentExpectation): Boolean = open(expected, false).use { it.complete }

    /** Explicit playback read of an ALREADY published slot. Unlike open(), this
     * never repairs, creates, truncates, fsyncs or publishes anything. Caller must
     * hold a current final-manifest playback ticket before and after this read,
     * authenticate the returned ciphertext, and wipe its owned array afterwards. */
    internal fun readPublished(expected: EncryptedSegmentExpectation): ByteArray = try {
        readPublishedUnchecked(expected)
    } catch (busy: SegmentStorageBusyException) { throw busy }
      catch (_: Exception) { fenced.add(expected.slot); throw SegmentStorageException() }

    private fun readPublishedUnchecked(expected: EncryptedSegmentExpectation): ByteArray {
        checkRoot()
        val rootKey = inodeKey(root, true)
        val lockPath = root.resolve(".store.lock")
        val lockKey = inodeKey(lockPath)
        FileChannel.open(lockPath, WRITE, NOFOLLOW_LINKS).use { channel ->
            val lock = try { channel.tryLock() ?: throw SegmentStorageBusyException() }
                catch (_: OverlappingFileLockException) { throw SegmentStorageBusyException() }
            lock.use {
                fun active() {
                    checkRoot()
                    if (!lock.isValid || inodeKey(root, true) != rootKey || inodeKey(lockPath) != lockKey ||
                        expected.slot in fenced) throw SegmentStorageException()
                    try {
                        Files.readAttributes(root.resolve(expected.slot + ".fault"), BasicFileAttributes::class.java, NOFOLLOW_LINKS)
                        throw SegmentStorageException()
                    } catch (_: java.nio.file.NoSuchFileException) { /* Only ENOENT means no fault. */ }
                }
                active()
                val intent = root.resolve(expected.slot + ".intent")
                val file = root.resolve(expected.slot + ".segment")
                val intentKey = inodeKey(intent); val fileKey = inodeKey(file)
                val binding = "OPNDDI1\u0000".toByteArray(Charsets.US_ASCII) + expected.bindingBytes()
                val expectedIntent = binding + MessageDigest.getInstance("SHA-256").digest(binding)
                val storedIntent = bounded(intent, INTENT_BYTES)
                try {
                    if (!storedIntent.contentEquals(expectedIntent)) throw SegmentStorageException()
                } finally { storedIntent.fill(0) }
                val bytes = bounded(file, expected.totalBytes)
                try {
                    active()
                    if (inodeKey(intent) != intentKey || inodeKey(file) != fileKey ||
                        bytes.size != expected.totalBytes || digestHex(bytes) != expected.segment.sha256 ||
                        !bytes.copyOfRange(0, 128).contentEquals(expected.headerBytes())) throw SegmentStorageException()
                    return bytes
                } catch (failure: Throwable) { bytes.fill(0); fenced.add(expected.slot); throw failure }
            }
        }
    }

    private fun checkRoot() {
        if (!Files.isDirectory(root, NOFOLLOW_LINKS) || root.toRealPath() != root) throw SegmentStorageException()
    }
    private fun reportDiagnostic(phase:String,failure:Exception) {
        try { diagnostic?.invoke(phase,failure.javaClass.simpleName.take(80),
            failure.stackTrace.firstOrNull { it.className.startsWith(DurableSegmentStore::class.java.name) }?.lineNumber ?: -1)
        } catch (_:Exception) { /* Synthetic observers cannot bypass normal cleanup. */ }
    }
    private fun <T> observed(phase:String,action:()->T):T = try { action() }
        catch (failure:Exception) { reportDiagnostic(phase,failure);throw failure }
    private fun exists(path: Path) = Files.exists(path, NOFOLLOW_LINKS)
    private fun inodeKey(path: Path, isDirectory: Boolean = false): Any = identity.key(path,isDirectory)
    private fun bounded(path: Path, maximum: Int): ByteArray {
        if (!Files.isRegularFile(path, NOFOLLOW_LINKS) || Files.size(path) > maximum) throw SegmentStorageException()
        FileChannel.open(path, READ, NOFOLLOW_LINKS).use { channel ->
            val measured = channel.size()
            if (measured !in 0..maximum.toLong()) throw SegmentStorageException()
            val size = measured.toInt()
            val bytes = ByteArray(size)
            try {
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.read(buffer) <= 0) throw SegmentStorageException()
                if (channel.size() != size.toLong() || channel.read(ByteBuffer.allocate(1)) != -1) throw SegmentStorageException()
                return bytes
            } catch (failure: Exception) { bytes.fill(0); throw failure }
        }
    }
    private fun createSynced(path: Path, bytes: ByteArray) = observed("file_create_and_force") {
        FileChannel.open(path, CREATE_NEW, WRITE, NOFOLLOW_LINKS).use { channel ->
            val buffer = ByteBuffer.wrap(bytes)
            while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw SegmentStorageException()
            channel.force(true)
        }
    }
    private fun checkQuota(expected: EncryptedSegmentExpectation) = observed("quota_directory_scan") {
        var entries = 0
        var bytes = 0L
        Files.newDirectoryStream(root).use { children -> children.forEach { path ->
            if (++entries > MAX_DIRECTORY_ENTRIES) throw SegmentStorageException()
            // One non-following lookup supplies both type and size. Keep the
            // complete scan and exact quota; no cached admission survives a lock.
            val attributes = Files.readAttributes(path, BasicFileAttributes::class.java, NOFOLLOW_LINKS)
            if (!attributes.isRegularFile) throw SegmentStorageException()
            bytes += attributes.size()
            if (bytes > MAX_STORE_BYTES) throw SegmentStorageException()
        } }
        if (bytes > MAX_STORE_BYTES - expected.totalBytes - 4096 ||
            observed("usable_space_query") { usableSpace.bytes(root) } < expected.totalBytes + 8192) throw SegmentStorageException()
    }

    internal inner class Session(
        val expected: EncryptedSegmentExpectation,
        private val lockChannel: FileChannel,
        private val lock: FileLock,
    ) : Closeable {
        private fun path(suffix: String) = root.resolve(expected.slot + suffix)
        private val intent = path(".intent")
        private val part = path(".part")
        private val checkpoint = path(".checkpoint")
        private val next = path(".checkpoint-next")
        private val destination = path(".segment")
        private val fault = path(".fault")
        var offset = 0
            private set
        var complete = false
            private set
        private var prefixDigest = MessageDigest.getInstance("SHA-256").digest(byteArrayOf())
        private var closed = false

        private fun active() {
            checkRoot()
            if (closed || !lock.isValid || expected.slot in fenced || exists(fault)) throw SegmentStorageException()
        }
        internal fun mismatch(): Nothing {
            fenced.add(expected.slot)
            try {
                if (!exists(fault)) createSynced(fault, "OPNDFAIL".toByteArray(Charsets.US_ASCII))
                directorySync.sync(root)
            } catch (_: Exception) { /* In-memory fence remains; no operation continues. */ }
            throw SegmentStorageException()
        }
        private fun checkedRead(file: Path, maximum: Int): ByteArray = try { bounded(file, maximum) }
            catch (_: SegmentStorageException) { mismatch() }
        internal fun recover(createIfAbsent: Boolean) {
            active()
            val binding = "OPNDDI1\u0000".toByteArray(Charsets.US_ASCII) + expected.bindingBytes()
            val encoded = binding + MessageDigest.getInstance("SHA-256").digest(binding)
            if (!exists(intent)) {
                if (listOf(part, checkpoint, next, destination).any(::exists)) mismatch()
                if (!createIfAbsent) return
                checkQuota(expected)
                observed("intent_create") { createSynced(intent, encoded) }
                observed("intent_directory_sync") { directorySync.sync(root) }
            } else if (!checkedRead(intent, INTENT_BYTES).contentEquals(encoded)) mismatch()
            if (exists(destination)) {
                // A rename publisher removes .part; a hard-link publisher may
                // retain it, but it must be the exact published inode.
                if(exists(part) && inodeKey(part)!=inodeKey(destination)) mismatch()
                verify(destination)
                // Re-fsync publication on recovery before a new metadata commit.
                FileChannel.open(destination, WRITE, NOFOLLOW_LINKS).use { it.force(true) }
                directorySync.sync(root)
                complete = true; offset = expected.totalBytes
                return
            }
            if (!exists(part)) {
                if (exists(checkpoint) || exists(next)) mismatch()
                observed("part_create") { createSynced(part, byteArrayOf()) }
                observed("part_directory_sync") { directorySync.sync(root) }
            }
            val bytes = checkedRead(part, expected.totalBytes)
            try {
                val old = if (exists(checkpoint)) parseCheckpoint(checkpoint) else null
                val pending = if (exists(next)) parseCheckpoint(next) else null
                // Any corrupt checkpoint is a fault, not an invitation to fall
                // back to a less restrictive or invented offset.
                if (old != null && pending != null && pending.first < old.first) mismatch()
                val current = pending ?: old
                if (current == null) {
                    if (bytes.isNotEmpty()) mismatch()
                    writeCheckpoint(0, prefixDigest)
                } else {
                    if (current.first > bytes.size) mismatch()
                    val actual = shaPrefix(bytes, current.first)
                    if (!actual.contentEquals(current.second)) mismatch()
                    offset = current.first; prefixDigest = actual
                    if (pending != null) {
                        Files.move(next, checkpoint, ATOMIC_MOVE, REPLACE_EXISTING)
                        directorySync.sync(root)
                    }
                    // Bytes beyond the last durable checkpoint were never
                    // acknowledged. Discard only this known uncommitted tail.
                    if (bytes.size > offset) FileChannel.open(part, WRITE, NOFOLLOW_LINKS).use {
                        it.truncate(offset.toLong()); it.force(true)
                    }
                }
            } finally { bytes.fill(0) }
        }

        private fun parseCheckpoint(file: Path): Pair<Int, ByteArray> {
            val encoded = checkedRead(file, CHECKPOINT_BYTES)
            try {
                if (encoded.size != CHECKPOINT_BYTES ||
                    !encoded.copyOfRange(0, 8).contentEquals("OPNDDC1\u0000".toByteArray(Charsets.US_ASCII)) ||
                    !MessageDigest.getInstance("SHA-256").digest(encoded.copyOfRange(0, 48)).contentEquals(encoded.copyOfRange(48, 80))) mismatch()
                val count = ByteBuffer.wrap(encoded, 8, 8).order(ByteOrder.BIG_ENDIAN).long
                if (count !in 0..expected.totalBytes.toLong()) mismatch()
                return count.toInt() to encoded.copyOfRange(16, 48)
            } finally { encoded.fill(0) }
        }
        private fun writeCheckpoint(count: Int, hash: ByteArray) {
            if (exists(next)) mismatch()
            val body = ByteBuffer.allocate(48).order(ByteOrder.BIG_ENDIAN).apply {
                put("OPNDDC1\u0000".toByteArray(Charsets.US_ASCII)); putLong(count.toLong()); put(hash)
            }.array()
            observed("checkpoint_create") { createSynced(next, body + MessageDigest.getInstance("SHA-256").digest(body)) }
            observe(SegmentDiskStep.CHECKPOINT_SYNCED)
            observed("checkpoint_atomic_rename") { Files.move(next, checkpoint, ATOMIC_MOVE, REPLACE_EXISTING) }
            observe(SegmentDiskStep.CHECKPOINT_RENAMED)
            observed("checkpoint_directory_sync") { directorySync.sync(root) }
            offset = count; prefixDigest = hash.copyOf()
        }

        /** Bytes are caller-owned; never retained or logged. */
        fun append(bytes: ByteArray) {
            active()
            require(!complete && bytes.isNotEmpty() && bytes.size <= MAX_CHUNK_BYTES &&
                offset + bytes.size <= expected.totalBytes) { "Invalid encrypted chunk geometry" }
            val before = checkedRead(part, expected.totalBytes)
            try {
                if (before.size != offset || !shaPrefix(before, offset).contentEquals(prefixDigest)) mismatch()
            } finally { before.fill(0) }
            FileChannel.open(part, WRITE, NOFOLLOW_LINKS).use { channel ->
                channel.position(offset.toLong())
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) if (channel.write(buffer) <= 0) throw SegmentStorageException()
                channel.force(true)
            }
            observe(SegmentDiskStep.PART_SYNCED)
            val after = checkedRead(part, expected.totalBytes)
            try { writeCheckpoint(offset + bytes.size, shaPrefix(after, after.size)) }
            finally { after.fill(0) }
        }

        fun verifyReady() {
            active()
            if (complete) verify(destination)
            else {
                if (offset != expected.totalBytes) throw SegmentStorageException()
                verify(part)
            }
        }
        private fun verify(file: Path) {
            val bytes = checkedRead(file, expected.totalBytes)
            try {
                if (bytes.size != expected.totalBytes || digestHex(bytes) != expected.segment.sha256 ||
                    !bytes.copyOfRange(0, EncryptedSegmentHeader.HEADER_BYTES).contentEquals(expected.headerBytes()) ||
                    bytes[EncryptedSegmentHeader.HEADER_BYTES] != 4.toByte()) mismatch()
            } finally { bytes.fill(0) }
        }

        /** Must execute ONLY within publishDownloadedSegment's guarded callback. */
        fun publish() {
            verifyReady()
            if (!complete) {
                val rootKey=inodeKey(root,true)
                val sourceKey=inodeKey(part)
                observed("container_atomic_publication") { publication.publish(root,part,destination) }
                checkRoot()
                if(inodeKey(root,true)!=rootKey || inodeKey(destination)!=sourceKey ||
                    (exists(part) && inodeKey(part)!=sourceKey)) throw SegmentStorageException()
                observe(SegmentDiskStep.CONTAINER_LINKED)
                observed("container_directory_sync") { directorySync.sync(root) }
                observe(SegmentDiskStep.DIRECTORY_SYNCED)
                complete = true
            }
            // Keep bounded intent/checkpoint evidence. A retained desktop .part
            // is the same inode; Android rename publication leaves only .segment.
        }
        override fun close() {
            if (closed) return
            closed = true
            try { lock.release() } finally { lockChannel.close() }
        }
    }

    companion object {
        private val DESKTOP_USABLE_SPACE = SegmentUsableSpace { Files.getFileStore(it).usableSpace }
        private val DESKTOP_PUBLICATION = SegmentPublication { _,source,destination -> Files.createLink(destination,source);Unit }
        private val DESKTOP_IDENTITY = SegmentFileIdentity { path,isDirectory ->
            val attributes=Files.readAttributes(path,BasicFileAttributes::class.java,NOFOLLOW_LINKS)
            if(if(isDirectory) !attributes.isDirectory else !attributes.isRegularFile) throw SegmentStorageException()
            attributes.fileKey() ?: throw SegmentStorageException()
        }
        const val MAX_CHUNK_BYTES = 4096
        const val MAX_STORE_BYTES = 256L * 1024 * 1024
        const val MAX_DIRECTORY_ENTRIES = 32768
        internal const val INTENT_BYTES = 200
        internal const val CHECKPOINT_BYTES = 80
    }
}

internal fun digestHex(bytes: ByteArray): String = canonicalHex(MessageDigest.getInstance("SHA-256").digest(bytes))
internal fun hexBytes(value: String) = ByteArray(value.length / 2) { value.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
private fun shaPrefix(bytes: ByteArray, count: Int): ByteArray = MessageDigest.getInstance("SHA-256").apply { update(bytes, 0, count) }.digest()
