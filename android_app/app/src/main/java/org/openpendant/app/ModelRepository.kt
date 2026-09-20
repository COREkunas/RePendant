package org.openpendant.app

import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.concurrent.CancellationException

/** A pinned, multilingual model selected by the application, never by a provider filename. */
data class ModelSpec(val id: String, val fileName: String, val byteCount: Long, val sha256: String)
data class VerifiedModel(val spec: ModelSpec, val file: File)

/** No network access, directory scanning, model loading or hashing during construction. */
class ModelRepository(directory: File, val spec: ModelSpec) {
    private val root: Path = directory.canonicalFile.toPath()
    private val destination: Path

    init {
        require(spec.id.matches(Regex("[a-zA-Z0-9._-]{1,80}")))
        require(spec.fileName.matches(Regex("[a-zA-Z0-9_-][a-zA-Z0-9._-]{0,100}\\.bin")))
        require(spec.byteCount in 4..MAX_MODEL_BYTES && hashPattern.matches(spec.sha256))
        require(Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) { "Model directory is unavailable" }
        destination = root.resolve(spec.fileName)
    }

    /** Size/name availability only; must not be presented as cryptographic verification. */
    fun available(): Boolean = run {
        Files.isRegularFile(destination, LinkOption.NOFOLLOW_LINKS) && Files.size(destination) == spec.byteCount
    }

    /**
     * Explicit user-triggered operation. Caller owns/closes the input stream.
     * Existing verified model remains unchanged if size, magic, hash or cancellation fails.
     */
    fun importVerified(input: InputStream, isCancelled: () -> Boolean = { false }) = synchronized(modelLock) {
        cancelled(isCancelled)
        require(Files.getFileStore(root).usableSpace >= spec.byteCount + DISK_RESERVE_BYTES) {
            "Not enough space to safely import the model"
        }
        require(!Files.exists(destination, LinkOption.NOFOLLOW_LINKS) ||
            Files.isRegularFile(destination, LinkOption.NOFOLLOW_LINKS)) { "Invalid model destination" }
        val staging = Files.createTempFile(root, ".model-", ".pending")
        val buffer = ByteArray(64 * 1024)
        try {
            var total = 0L
            FileOutputStream(staging.toFile()).use { output ->
                while (true) {
                    cancelled(isCancelled)
                    val count = input.read(buffer, 0, minOf(buffer.size.toLong(), spec.byteCount - total + 1).toInt())
                    if (count < 0) break
                    require(count > 0) { "Model provider stopped responding" }
                    total += count
                    require(total <= spec.byteCount) { "Model exceeds the expected size" }
                    output.write(buffer, 0, count)
                }
                require(total == spec.byteCount) { "Model size does not match" }
                output.fd.sync()
            }
            verify(staging, isCancelled)
            cancelled(isCancelled)
            // No non-atomic fallback: leave the previous model intact if unsupported.
            Files.move(staging, destination, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        } finally {
            buffer.fill(0)
            Files.deleteIfExists(staging)
        }
    }

    /** Lease excludes cooperating imports until the native model context is released. */
    fun <T> withVerifiedModel(isCancelled: () -> Boolean = { false }, action: (VerifiedModel) -> T): T =
        synchronized(modelLock) {
            verify(destination, isCancelled)
            cancelled(isCancelled)
            action(VerifiedModel(spec, destination.toFile()))
        }

    private fun verify(path: Path, isCancelled: () -> Boolean) {
        cancelled(isCancelled)
        require(Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS) && Files.size(path) == spec.byteCount) {
            "Install the expected multilingual model first"
        }
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(64 * 1024)
        val magic = ByteArray(4)
        var countTotal = 0L
        try {
            Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS).use { input ->
                while (true) {
                    cancelled(isCancelled)
                    val count = input.read(buffer)
                    if (count < 0) break
                    require(count > 0 && countTotal + count <= spec.byteCount) { "Model changed during verification" }
                    for (index in 0 until minOf(count, (4 - countTotal).coerceAtLeast(0).toInt())) {
                        magic[countTotal.toInt() + index] = buffer[index]
                    }
                    digest.update(buffer, 0, count)
                    countTotal += count
                }
            }
            require(countTotal == spec.byteCount && magic.contentEquals(byteArrayOf(0x6c, 0x6d, 0x67, 0x67))) {
                "Unsupported or incomplete Whisper model"
            }
            require(digest.digest().hex() == spec.sha256) { "Model checksum does not match the trusted release" }
        } finally { buffer.fill(0); magic.fill(0) }
    }

    companion object {
        const val MAX_MODEL_BYTES = 512L * 1024 * 1024
        private const val DISK_RESERVE_BYTES = 32L * 1024 * 1024
        internal val hashPattern = Regex("[0-9a-f]{64}")
        private val modelLock = Any()
        internal fun cancelled(check: () -> Boolean) {
            if (check() || Thread.currentThread().isInterrupted) throw CancellationException("Transcription cancelled")
        }
        internal fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
