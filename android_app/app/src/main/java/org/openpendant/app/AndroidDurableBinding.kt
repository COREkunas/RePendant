package org.openpendant.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Looper
import android.util.AtomicFile
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.NoSuchFileException
import java.nio.file.Path
import java.nio.file.StandardOpenOption.*
import java.nio.file.attribute.BasicFileAttributes

/** Dedicated public-only enrollment. No catalog, owner private material or
 * arbitrary filename can supply authority. Explicit enrollment is one-use;
 * any claim/residue is retained for attention, never reset or overwritten.
 * AtomicFile publication runs only in a proven-empty claimed namespace under
 * process/OS ownership, not a claim of kernel RENAME_NOREPLACE semantics.
 */
object AndroidDurableBinding {
    private val monitor = Any()
    private const val NAMESPACE = "durable-enrollment-v1"
    private const val FULL_NAMESPACE = "durable-enrollment-full-v2"
    private const val USB_NAMESPACE = "durable-enrollment-usb-v3"
    private const val ROTATION_PREFIX = "durable-enrollment-key-v4-"
    fun read(context: Context): DurablePublicBinding? = synchronized(monitor) {
        worker()
        var selected = readBase(context)
        val roots = checkNotNull(context.noBackupFilesDir.listFiles()).filter { it.name.startsWith(ROTATION_PREFIX) }
        check(roots.size <= 64)
        val generations = roots.map { it.name.removePrefix(ROTATION_PREFIX).toLong().also { g -> check(g.toString() == it.name.removePrefix(ROTATION_PREFIX)) } }.sorted()
        for (generation in generations) {
            val previous = checkNotNull(selected)
            val next = checkNotNull(readNamespace(context, ROTATION_PREFIX + generation))
            requireKeyReset(previous, next); check(next.volume.generation == generation)
            selected = next
        }
        selected
    }
    private fun readBase(context: Context): DurablePublicBinding? {
        if (attributes(context.noBackupFilesDir.canonicalFile.toPath().resolve(USB_NAMESPACE)) != null) {
            // Direct first enrollment for a replacement phone, not a fabricated
            // generation1->2 history. Never shadow an existing legacy binding.
            check(attributes(context.noBackupFilesDir.canonicalFile.toPath().resolve(NAMESPACE)) == null)
            check(attributes(context.noBackupFilesDir.canonicalFile.toPath().resolve(FULL_NAMESPACE)) == null)
            return checkNotNull(readNamespace(context, USB_NAMESPACE)).also {
                check(it.volume.generation >= 2L)
            }
        }
        val old = readNamespace(context, NAMESPACE)
        val fullRoot = context.noBackupFilesDir.canonicalFile.toPath().resolve(FULL_NAMESPACE)
        if (attributes(fullRoot) == null) return old
        // Any partial new publication fences rather than silently falling back
        // to the obsolete volume. Original enrollment is retained unchanged.
        val full = checkNotNull(readNamespace(context, FULL_NAMESPACE))
        requireMigration(checkNotNull(old), full)
        return full
    }
    private fun readNamespace(context: Context, namespace: String): DurablePublicBinding? {
        val root = context.noBackupFilesDir.canonicalFile.toPath().resolve(namespace)
        val attributes = attributes(root) ?: return null
        check(attributes.isDirectory && root.toRealPath() == root)
        return locked(root, false) {
            requireNames(root, setOf("binding.lock", "binding.claim", "binding.bin"))
            val claim = exact(root.resolve("binding.claim"))
            val actual = exact(root.resolve("binding.bin"))
            check(claim.contentEquals(actual))
            DurablePublicBindingCodec.decode(actual)
        }
    }
    private fun requireMigration(old: DurablePublicBinding, next: DurablePublicBinding) {
        require(old.volume.generation == 1L && next.volume.generation == 2L &&
            old.volume.deviceId == next.volume.deviceId && old.volume.volumeId != next.volume.volumeId &&
            old.bondAddress == next.bondAddress && old.recipientFingerprint == next.recipientFingerprint)
    }
    /** Explicit operator-confirmed, public-only one-way storage migration.
     * Never accepts authority learned from a BLE catalog. No key/bond/file
     * deletion. A failed publication stays fenced for independent diagnosis. */
    fun migrateFullExplicit(context: Context, expected: DurablePublicBinding, next: DurablePublicBinding) = synchronized(monitor) {
        worker(); requireMigration(expected, next)
        check(read(context) == expected)
        check(attributes(context.noBackupFilesDir.canonicalFile.toPath().resolve(FULL_NAMESPACE)) == null)
        publish(context, next, FULL_NAMESPACE)
        check(read(context) == next)
    }

    /** Helper/UI must obtain explicit user authorization before invoking. It
     * verifies only public enrollment prerequisites, never creates/exports a key.
     * No production startup/connection code calls this entry point. */
    @SuppressLint("MissingPermission")
    fun enrollExplicit(context: Context, binding: DurablePublicBinding) = synchronized(monitor) {
        check(attributes(context.noBackupFilesDir.canonicalFile.toPath().resolve(USB_NAMESPACE)) == null)
        publish(context, binding, NAMESPACE)
    }
    /** Only called after an explicit cable identity check and matching local
     * backup import. Existing enrollment is immutable; no overwrite/recovery. */
    fun enrollFromUsbExplicit(context: Context, binding: DurablePublicBinding) = synchronized(monitor) {
        worker(); require(binding.volume.generation >= 2L)
        val current = read(context)
        if (current != null) {
            check(current == binding)
            binding.requireVerifiedRecipient(vaultFor(context, binding).summary())
            requireBond(context, binding)
        } else {
            publish(context, binding, USB_NAMESPACE)
            check(read(context) == binding)
        }
    }
    private fun requireKeyReset(old: DurablePublicBinding, next: DurablePublicBinding) {
        require(old.volume.generation >= 2 && old.volume.generation < Long.MAX_VALUE &&
            next.volume.generation == old.volume.generation + 1 && old.volume.deviceId == next.volume.deviceId &&
            old.volume.volumeId != next.volume.volumeId && old.bondAddress == next.bondAddress &&
            old.recipientFingerprint != next.recipientFingerprint)
    }
    /** Cable-verified ACTIVE successor only; old enrollments/copies remain immutable. */
    fun finishKeyResetExplicit(context: Context, old: DurablePublicBinding, next: DurablePublicBinding) = synchronized(monitor) {
        worker(); requireKeyReset(old, next)
        val current = read(context)
        if (current == null || current == next) enrollFromUsbExplicit(context, next)
        else {
            check(current == old)
            publish(context, next, ROTATION_PREFIX + next.volume.generation)
            check(read(context) == next)
        }
    }
    @SuppressLint("MissingPermission")
    private fun publish(context: Context, binding: DurablePublicBinding, namespace: String) {
        worker()
        requireBond(context, binding)
        val vault = vaultFor(context, binding)
        binding.requireVerifiedRecipient(vault.summary())
        val parent = context.noBackupFilesDir.canonicalFile.toPath()
        val root = parent.resolve(namespace)
        // Directory itself is the one-use reservation. Never adopt an existing
        // empty/corrupt namespace, even if a previous write failed very early.
        Files.createDirectory(root); AndroidDurableSegments.syncDirectory(parent)
        android.system.Os.chmod(root.toString(), 448)
        locked(root, true) {
            requireNames(root, setOf("binding.lock"))
            val bytes = DurablePublicBindingCodec.encode(binding)
            FileChannel.open(root.resolve("binding.claim"), CREATE_NEW, WRITE, NOFOLLOW_LINKS).use { channel ->
                val data = ByteBuffer.wrap(bytes)
                while (data.hasRemaining()) check(channel.write(data) > 0)
                channel.force(true)
            }
            AndroidDurableSegments.syncDirectory(root)
            check(attributes(root.resolve("binding.bin")) == null)
            val atomic = AtomicFile(root.resolve("binding.bin").toFile())
            val stream = atomic.startWrite()
            // On failure retain all residue. No failWrite rollback/reset path.
            try { stream.write(bytes); stream.fd.sync(); atomic.finishWrite(stream) }
            catch (failure: Throwable) { try { stream.close() } catch (_: Throwable) { }; throw failure }
            AndroidDurableSegments.syncDirectory(root)
            requireNames(root, setOf("binding.lock", "binding.claim", "binding.bin"))
            check(exact(root.resolve("binding.bin")).contentEquals(bytes))
            check(exact(root.resolve("binding.claim")).contentEquals(bytes))
            binding.requireVerifiedRecipient(vault.summary())
            requireBond(context, binding)
        }
    }
    @SuppressLint("MissingPermission")
    private fun requireBond(context: Context, binding: DurablePublicBinding) {
        check(Build.VERSION.SDK_INT < 31 || context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED)
        val bonded = context.getSystemService(BluetoothManager::class.java)?.adapter?.bondedDevices.orEmpty()
        check(bonded.any { it.address == binding.bondAddress && it.bondState == BluetoothDevice.BOND_BONDED })
    }
    internal fun vault(context: Context) = vaultFor(context, read(context))
    internal fun vaultFor(context: Context, binding: DurablePublicBinding?) =
        AndroidRecipientProfiles.selected(context, binding?.recipientFingerprint)
    private fun worker() { check(Looper.myLooper() != Looper.getMainLooper()) }
    internal fun attributes(path: Path): BasicFileAttributes? = try {
        Files.readAttributes(path, BasicFileAttributes::class.java, NOFOLLOW_LINKS)
    } catch (_: NoSuchFileException) { null }
    private fun requireNames(root: Path, allowed: Set<String>) {
        Files.newDirectoryStream(root).use { entries ->
            val names = mutableSetOf<String>()
            for (entry in entries) { check(names.size < 3 && attributes(entry)?.isRegularFile == true); check(names.add(entry.fileName.toString())) }
            check(names == allowed)
        }
    }
    private fun exact(path: Path): ByteArray {
        check(attributes(path)?.isRegularFile == true)
        return FileChannel.open(path, READ, NOFOLLOW_LINKS).use { channel ->
            check(channel.size() == 128L); val bytes = ByteArray(128); val buffer = ByteBuffer.wrap(bytes)
            while (buffer.hasRemaining()) check(channel.read(buffer) > 0)
            check(channel.read(ByteBuffer.allocate(1)) == -1); bytes
        }
    }
    private fun <T> locked(root: Path, create: Boolean, action: () -> T): T {
        val file = root.resolve("binding.lock")
        if (!create) check(attributes(file)?.isRegularFile == true)
        val options: Array<java.nio.file.OpenOption> = if (create) arrayOf(CREATE_NEW, WRITE, NOFOLLOW_LINKS) else arrayOf(WRITE, NOFOLLOW_LINKS)
        FileChannel.open(file, *options).use { channel ->
            val lock = channel.tryLock() ?: throw RecordingSyncBusyException()
            lock.use { return action() }
        }
    }
}
