package org.openpendant.app

import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.channels.OverlappingFileLockException
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.NoSuchFileException
import java.nio.file.Path
import java.nio.file.StandardOpenOption.READ
import java.nio.file.StandardOpenOption.WRITE
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest

class PhoneDeletionStorageException : IllegalStateException("Phone deletion remains pending; local storage needs reconciliation")

/** Required explicit derivative mapping, not legacy clip/transcript cleanup.
 * Implementations must derive a bounded exact artifact set from this full plan,
 * check bindings/no-follow identities, accept only real ENOENT as absence, and
 * fsync affected directories. No default no-op provider or wildcard deletion.
 * Must be idempotent after any partial failure and must not reenter this store.
 * Durable derivative format/UI wiring remains a separate integration gate. */
fun interface PhoneDeletionDerivatives { fun removeAndSync(plan: PhoneDeletionPlan) }

internal enum class PhoneDeletionStep { UNLINKED, DIRECTORY_SYNCED }
internal interface PhoneDeletionFileIo {
    fun attributes(path: Path): BasicFileAttributes
    fun unlink(path: Path)
}

/** Explicit bounded removal of ciphertext slots from an existing private root.
 * No payload is opened, hashed or decrypted. Only 200-byte intents and 80-byte
 * checkpoints are read to bind paths to the persisted finalized manifest.
 * No intent/part/checkpoint/fault is created or repaired; target fault markers
 * stop removal. Unrecognized entries and conflicting aliases stop the operation.
 *
 * Shared .store.lock excludes cooperating download processes. Recording-level
 * ownership/intent MUST be held by PendingPhoneDeletion.complete throughout.
 * The mandatory derivative adapter always runs first. Transcript retention must
 * never preserve phone audio or queued playback; the typed adapter applies it.
 * Data/checkpoints are unlinked and directory-synced BEFORE removing their
 * binding intent, then synced again: a crash cannot durably remove authority
 * while leaving a previously removed data path eligible to reappear.
 * This is unlink durability, not secure erasure or a physical power-cut proof.
 */
class PhoneRecordingDeletion internal constructor(directory: File,
    private val sync: SegmentDirectorySync,
    private val identity: SegmentFileIdentity,
    private val derivatives: PhoneDeletionDerivatives,
    private val io: PhoneDeletionFileIo = NIO,
    private val observe: (PhoneDeletionStep, Path) -> Unit = { _, _ -> }) : PhoneDeletionRemoval {
    private val root = directory.toPath().toAbsolutePath().normalize()
    private val rootKey: Any
    init {
        checkRootPath()
        rootKey = identity.key(root, true)
    }

    override fun removeAndSync(plan: PhoneDeletionPlan) {
        // No generic caller-selected path or segment list is accepted here.
        require(plan.segments.size <= RecordingManifest.MAX_SEGMENTS &&
            plan.segments.withIndex().all { (index, s) -> s.recording == plan.recording && s.sequence == index && s.byteCount in 210..65745 })
        checkRoot()
        val lockPath = root.resolve(".store.lock")
        val lockInfo = lookup(lockPath) ?: throw PhoneDeletionStorageException()
        FileChannel.open(lockPath, WRITE, NOFOLLOW_LINKS).use { channel ->
            val lock = try { channel.tryLock() ?: throw SegmentStorageBusyException() }
                catch (_: OverlappingFileLockException) { throw SegmentStorageBusyException() }
            lock.use {
                fun active() {
                    checkRoot()
                    if (!lock.isValid || lookup(lockPath)?.key != lockInfo.key) throw PhoneDeletionStorageException()
                }
                active()
                // Inspection is bounded, and never grants ownership from a
                // filename. Other recognized slots remain completely untouched.
                var entries = 0
                Files.newDirectoryStream(root).use { children ->
                    for (path in children) {
                        if (++entries > DurableSegmentStore.MAX_DIRECTORY_ENTRIES) throw PhoneDeletionStorageException()
                        if (path.fileName.toString() != ".store.lock" &&
                            !path.fileName.toString().matches(KNOWN_NAME)) throw PhoneDeletionStorageException()
                        if (lookup(path) == null) throw PhoneDeletionStorageException()
                    }
                }
                val slots = plan.segments.map { segment -> Slot(segment) }
                // Validate the complete finite plan before any physical removal.
                slots.forEach { active(); inspect(it) }
                derivatives.removeAndSync(plan)
                active()
                for (slot in slots) {
                    active()
                    val found = inspect(slot)
                    for (suffix in DATA_SUFFIXES) {
                        remove(slot.path(suffix), found[suffix], ::active)
                    }
                    barrier(::active)
                    // Preserve binding until all preceding removal is durable.
                    remove(slot.path(".intent"), found[".intent"], ::active)
                    barrier(::active)
                    for (suffix in ALL_SUFFIXES) if (lookup(slot.path(suffix)) != null) throw PhoneDeletionStorageException()
                }
                barrier(::active) //Also persists the all-absent/empty-manifest case.
                active()
            }
        }
    }

    private data class Info(val size: Long, val key: Any)
    private inner class Slot(val segment: SegmentIdentity) {
        val name = digestHex(RecordingSyncSnapshotCodec.identityBytes(segment.recording) +
            ByteBuffer.allocate(4).putInt(segment.sequence).array())
        fun path(suffix: String): Path = root.resolve(name + suffix)
    }
    private fun checkRootPath() {
        val attributes = io.attributes(root)
        if (!attributes.isDirectory || attributes.isSymbolicLink || root.toRealPath() != root) throw PhoneDeletionStorageException()
    }
    private fun checkRoot() {
        checkRootPath()
        if (identity.key(root, true) != rootKey) throw PhoneDeletionStorageException()
    }
    private fun lookup(path: Path): Info? = try {
        val attributes = io.attributes(path)
        if (!attributes.isRegularFile || attributes.isSymbolicLink) throw PhoneDeletionStorageException()
        Info(attributes.size(), identity.key(path, false))
    } catch (_: NoSuchFileException) { null } //Only real not-found; EACCES/EIO propagate.

    private fun inspect(slot: Slot): Map<String, Info?> {
        if (lookup(slot.path(".fault")) != null) throw PhoneDeletionStorageException()
        val found = ALL_SUFFIXES.associateWith { lookup(slot.path(it)) }
        val intent = found[".intent"]
        if (intent == null) {
            if (found.values.any { it != null }) throw PhoneDeletionStorageException()
            return found //Exactly authorized paths already absent, never recreated.
        }
        val encoded = readMetadata(slot.path(".intent"), intent, 200)
        try {
            val body = encoded.copyOfRange(0, 168)
            if (!encoded.copyOfRange(0, 8).contentEquals("OPNDDI1\u0000".toByteArray(Charsets.US_ASCII)) ||
                !MessageDigest.isEqual(MessageDigest.getInstance("SHA-256").digest(body), encoded.copyOfRange(168, 200)) ||
                !encoded.copyOfRange(136, 168).contentEquals(hexBytes(slot.segment.sha256))) throw PhoneDeletionStorageException()
            val fingerprint = encoded.copyOfRange(28, 60)
            try {
                val expected = EncryptedSegmentHeader.encode(slot.segment.recording, fingerprint, slot.segment.sequence,
                    slot.segment.byteCount.toInt() - 209)
                if (!encoded.copyOfRange(8, 136).contentEquals(expected)) throw PhoneDeletionStorageException()
            } finally { fingerprint.fill(0); body.fill(0) }
        } finally { encoded.fill(0) }
        found[".segment"]?.let { if (it.size != slot.segment.byteCount) throw PhoneDeletionStorageException() }
        found[".part"]?.let { if (it.size !in 0..slot.segment.byteCount) throw PhoneDeletionStorageException() }
        if (found[".part"] != null && found[".segment"] != null && found[".part"]!!.key != found[".segment"]!!.key)
            throw PhoneDeletionStorageException()
        for (suffix in listOf(".checkpoint", ".checkpoint-next")) found[suffix]?.let { info ->
            val checkpoint = readMetadata(slot.path(suffix), info, 80)
            try {
                if (!checkpoint.copyOfRange(0, 8).contentEquals("OPNDDC1\u0000".toByteArray(Charsets.US_ASCII)) ||
                    ByteBuffer.wrap(checkpoint, 8, 8).long !in 0..slot.segment.byteCount ||
                    !MessageDigest.isEqual(MessageDigest.getInstance("SHA-256").digest(checkpoint.copyOfRange(0, 48)),
                        checkpoint.copyOfRange(48, 80))) throw PhoneDeletionStorageException()
            } finally { checkpoint.fill(0) }
        }
        return found
    }
    private fun readMetadata(path: Path, expected: Info, size: Int): ByteArray {
        if (expected.size != size.toLong()) throw PhoneDeletionStorageException()
        val bytes = ByteArray(size)
        try {
            FileChannel.open(path, READ, NOFOLLOW_LINKS).use { channel ->
                if (channel.size() != size.toLong() || lookup(path) != expected) throw PhoneDeletionStorageException()
                val buffer = ByteBuffer.wrap(bytes)
                var reads = 0
                while (buffer.hasRemaining()) if (++reads > size || channel.read(buffer) <= 0) throw PhoneDeletionStorageException()
                if (channel.read(ByteBuffer.allocate(1)) != -1 || lookup(path) != expected) throw PhoneDeletionStorageException()
            }
            return bytes
        } catch (failure: Throwable) { bytes.fill(0); throw failure }
    }
    private fun remove(path: Path, expected: Info?, active: () -> Unit) {
        active()
        val actual = lookup(path)
        if (actual == null) return
        if (actual != expected) throw PhoneDeletionStorageException()
        try { io.unlink(path) } catch (_: NoSuchFileException) { /* Exact authorized path already absent. */ }
        observe(PhoneDeletionStep.UNLINKED, path)
        active()
        if (lookup(path) != null) throw PhoneDeletionStorageException()
    }
    private fun barrier(active: () -> Unit) {
        active(); sync.sync(root); observe(PhoneDeletionStep.DIRECTORY_SYNCED, root); active()
    }
    companion object {
        private val DATA_SUFFIXES = listOf(".segment", ".part", ".checkpoint-next", ".checkpoint")
        private val ALL_SUFFIXES = DATA_SUFFIXES + ".intent"
        private val KNOWN_NAME = Regex("[0-9a-f]{64}\\.(segment|part|intent|checkpoint|checkpoint-next|fault)")
        internal val NIO = object : PhoneDeletionFileIo {
            override fun attributes(path: Path): BasicFileAttributes = Files.readAttributes(path, BasicFileAttributes::class.java, NOFOLLOW_LINKS)
            override fun unlink(path: Path) = Files.delete(path)
        }
    }
}
