package org.openpendant.app

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.StandardCopyOption

data class TranscriptSegment(val startMs: Long, val endMs: Long, val text: String)
data class TranscriptDocument(
    val recordingId: String,
    /** SHA-256 of the validated, canonical, original PCM16 WAV, never the listening preview. */
    val audioSha256: String,
    val modelId: String,
    val modelSha256: String,
    val engineVersion: String,
    val language: String,
    val createdAtMillis: Long,
    val segments: List<TranscriptSegment>,
) {
    val text: String get() = segments.joinToString(" ") { it.text.trim() }.trim()
}

/** Bounded app-private sidecars. Reads only transcript metadata/text, never recording audio. */
class TranscriptionRepository(directory: File) {
    private val root: Path = directory.canonicalFile.toPath()

    init { require(Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) { "Transcript directory is unavailable" } }

    fun read(recordingId: String): TranscriptDocument? = synchronized(sidecarLock) {
        val path = pathFor(recordingId)
        if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return@synchronized null
        require(Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS) && Files.size(path) in 1..MAX_FILE_BYTES.toLong()) {
            "Invalid transcript sidecar"
        }
        // Bounded stream read: do not trust a size check alone if a file changes concurrently.
        val encoded = Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS).use { input ->
            val output = ByteArrayOutputStream()
            val chunk = ByteArray(4096)
            try {
                while (true) {
                    val count = input.read(chunk)
                    if (count < 0) break
                    require(count > 0 && output.size() + count <= MAX_FILE_BYTES) { "Transcript exceeds the size limit" }
                    output.write(chunk, 0, count)
                }
                output.toByteArray()
            } finally { chunk.fill(0) }
        }
        try {
            DataInputStream(ByteArrayInputStream(encoded)).use { input ->
                require(input.readInt() == MAGIC && input.readInt() == VERSION) { "Unknown transcript format" }
                val id = input.readUTF()
                val audioHash = input.readUTF()
                val modelId = input.readUTF()
                val modelHash = input.readUTF()
                val engine = input.readUTF()
                val language = input.readUTF()
                val created = input.readLong()
                val count = input.readInt()
                require(count in 0..MAX_SEGMENTS) { "Invalid transcript segment count" }
                val segments = List(count) { TranscriptSegment(input.readLong(), input.readLong(), input.readUTF()) }
                require(input.read() == -1 && id == recordingId) { "Transcript recording binding does not match" }
                TranscriptDocument(id, audioHash, modelId, modelHash, engine, language, created, segments).also(::validate)
            }
        } finally { encoded.fill(0) }
    }

    /** Complete successful output only; cancellation/failure belongs in transient job state. */
    fun saveVerified(document: TranscriptDocument) = synchronized(sidecarLock) {
        validate(document)
        val destination = pathFor(document.recordingId)
        require(!Files.exists(destination, LinkOption.NOFOLLOW_LINKS) ||
            Files.isRegularFile(destination, LinkOption.NOFOLLOW_LINKS)) { "Invalid transcript destination" }
        val output = ByteArrayOutputStream()
        DataOutputStream(output).use { data ->
            data.writeInt(MAGIC); data.writeInt(VERSION)
            data.writeUTF(document.recordingId); data.writeUTF(document.audioSha256)
            data.writeUTF(document.modelId); data.writeUTF(document.modelSha256)
            data.writeUTF(document.engineVersion); data.writeUTF(document.language)
            data.writeLong(document.createdAtMillis); data.writeInt(document.segments.size)
            document.segments.forEach { data.writeLong(it.startMs); data.writeLong(it.endMs); data.writeUTF(it.text) }
        }
        val encoded = output.toByteArray()
        require(encoded.size <= MAX_FILE_BYTES) { "Transcript exceeds the size limit" }
        val staging = Files.createTempFile(root, ".transcript-", ".pending")
        try {
            FileOutputStream(staging.toFile()).use { it.write(encoded); it.fd.sync() }
            // Same-volume atomic publication; no promise of filesystem directory fsync.
            Files.move(staging, destination, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        } finally { encoded.fill(0); Files.deleteIfExists(staging) }
    }

    fun delete(recordingId: String) = synchronized(sidecarLock) {
        val path = pathFor(recordingId)
        if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
            require(Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) { "Invalid transcript destination" }
            Files.delete(path)
        }
    }

    private fun pathFor(id: String): Path {
        require(validId(id)) { "Invalid recording identifier" }
        return root.resolve("transcript_$id.opt")
    }

    companion object {
        private const val MAGIC = 0x4f505452 // OPTR
        private const val VERSION = 1
        private const val MAX_FILE_BYTES = 256 * 1024
        private const val MAX_SEGMENTS = 512
        private val uuidPattern = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
        private val sidecarLock = Any()
        internal fun validId(id: String) = id == RecordingRepository.LEGACY_ID || uuidPattern.matches(id)
        internal fun validate(document: TranscriptDocument) {
            require(validId(document.recordingId) && ModelRepository.hashPattern.matches(document.audioSha256) &&
                ModelRepository.hashPattern.matches(document.modelSha256)) { "Invalid transcript identity" }
            require(document.modelId.matches(Regex("[a-zA-Z0-9._-]{1,80}")) &&
                document.engineVersion.matches(Regex("[a-zA-Z0-9._-]{1,100}")) && document.language == "lt" &&
                document.createdAtMillis >= 0) { "Invalid transcript provenance" }
            require(document.segments.size <= MAX_SEGMENTS) { "Too many transcript segments" }
            var previousStart = 0L
            var textBytes = 0L
            document.segments.forEach {
                require(it.startMs >= previousStart && it.endMs >= it.startMs && it.endMs <= 30000L) { "Invalid transcript timestamps" }
                require(!it.text.contains('\u0000') && it.text.length <= 16000) { "Invalid transcript text" }
                textBytes += it.text.toByteArray(Charsets.UTF_8).size
                require(textBytes <= 64000) { "Transcript exceeds the text limit" }
                previousStart = it.startMs
            }
        }
    }
}
