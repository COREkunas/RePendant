package org.openpendant.app

import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.attribute.BasicFileAttributes
import java.util.UUID

/** Metadata only. These clips use the existing mono, 16 kHz, PCM16 WAV format. */
data class LocalRecording(
    val id: String,
    val title: String,
    val createdAtMillis: Long,
    val byteCount: Long,
) {
    val durationSeconds: Double get() = (byteCount - 44).coerceAtLeast(0) / 32000.0
}

/**
 * App-private library for bounded, verified clips; not the future pendant catalogue.
 *
 * Construction and listing never open, rewrite, move, or clean up audio. The legacy
 * file stays in place until the user explicitly deletes that exact item. An item's
 * original file modification time supplies its creation time; no sidecar migration
 * is required. Unknown and interrupted staging files are left untouched.
 */
class RecordingRepository(private val directory: File) {
    private val root: Path = directory.canonicalFile.toPath()

    init {
        ensure(Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS), "Local recording directory is unavailable")
    }

    /** Inspect names and filesystem metadata only; do not read/hash the audio here. */
    fun entries(): List<LocalRecording> = synchronized(fileLock) {
        Files.newDirectoryStream(root).use { children ->
            children.mapNotNull { path ->
                val id = idForFilename(path.fileName.toString()) ?: return@mapNotNull null
                metadata(id, path)
            }.sortedWith(compareByDescending<LocalRecording> { it.createdAtMillis }.thenBy { it.id })
        }
    }

    /** Caller retains ownership of pcm. Its bytes are not changed by saving. */
    fun save(pcm: ByteArray): LocalRecording = synchronized(fileLock) {
        // Validate before creating any file. The encoded temporary is always scrubbed.
        val encoded = WavCodec.encode(pcm)
        var staging: Path? = null
        try {
            val id = UUID.randomUUID().toString()
            val destination = pathForId(id)
            staging = Files.createTempFile(root, ".recording-", ".pending")
            FileOutputStream(staging.toFile()).use { output ->
                output.write(encoded)
                output.fd.sync()
            }
            // Same-directory publication with no REPLACE_EXISTING: never overwrite
            // another clip. A crash can leave only an ignored staging file or the
            // published file; this is not a claim of directory-fsync durability.
            Files.move(staging, destination)
            metadata(id, destination) ?: throw ProtocolException("Could not finalize the local recording")
        } finally {
            encoded.fill(0)
            // Only this save's unpublished staging file may be removed automatically.
            staging?.let { Files.deleteIfExists(it) }
        }
    }

    /** Explicit playback access. Caller must scrub the returned PCM when finished. */
    fun readPcm(id: String): ByteArray = synchronized(fileLock) {
        val path = pathForId(id)
        val item = metadata(id, path)
            ?: throw ProtocolException("Local recording is unavailable or invalid")
        val encoded = ByteArray(item.byteCount.toInt())
        try {
            Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS).use { input ->
                var offset = 0
                while (offset < encoded.size) {
                    val count = input.read(encoded, offset, encoded.size - offset)
                    ensure(count > 0, "Local recording was truncated")
                    offset += count
                }
                ensure(input.read() == -1, "Local recording changed or exceeds the size limit")
            }
            WavCodec.decode(encoded)
        } finally {
            encoded.fill(0)
        }
    }

    /** Idempotent, explicit removal of this phone's exact file; no pendant command. */
    fun deletePhone(id: String) = synchronized(fileLock) {
        val path = pathForId(id)
        if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
            ensure(Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS), "Recording is not a regular local file")
            Files.delete(path)
        }
    }

    fun find(id: String): LocalRecording? = synchronized(fileLock) {
        metadata(id, pathForId(id))
    }

    private fun metadata(id: String, path: Path): LocalRecording? {
        if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) return null
        val attributes = Files.readAttributes(path, BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS)
        val byteCount = attributes.size()
        if (!attributes.isRegularFile || byteCount !in 684L..32044L || (byteCount - 44) % 640 != 0L) return null
        return LocalRecording(id, if (id == LEGACY_ID) "Verified clip" else "Recording",
            attributes.lastModifiedTime().toMillis(), byteCount)
    }

    private fun pathForId(id: String): Path {
        ensure(id == LEGACY_ID || uuidPattern.matches(id), "Invalid local recording identifier")
        return root.resolve(if (id == LEGACY_ID) LEGACY_FILENAME else "recording_$id.wav")
    }

    private fun idForFilename(name: String): String? {
        if (name == LEGACY_FILENAME) return LEGACY_ID
        if (!name.startsWith("recording_") || !name.endsWith(".wav")) return null
        val id = name.removePrefix("recording_").removeSuffix(".wav")
        return id.takeIf(uuidPattern::matches)
    }

    companion object {
        const val LEGACY_ID = "legacy-verified-clip"
        private const val LEGACY_FILENAME = "latest_verified_clip.wav"
        private val uuidPattern = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
        // Serialize cooperating instances, including playback versus explicit deletion.
        private val fileLock = Any()
    }
}
