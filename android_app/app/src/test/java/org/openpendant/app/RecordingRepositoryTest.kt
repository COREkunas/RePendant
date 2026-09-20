package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.nio.file.attribute.FileTime

class RecordingRepositoryTest {
    private fun rejected(action: () -> Unit) {
        try { action(); fail("Invalid repository operation accepted") } catch (_: ProtocolException) { }
    }

    private fun withDirectory(action: (File) -> Unit) {
        val directory = Files.createTempDirectory("openpendant-library-unit-").toFile()
        try { action(directory) } finally {
            // These are exclusively synthetic files in this test's own temporary directory.
            directory.listFiles()?.forEach { file ->
                if (file.isDirectory) file.listFiles()?.forEach { it.delete() }
                file.delete()
            }
            directory.delete()
        }
    }

    private fun pcm(size: Int = 640, seed: Int = 11): ByteArray = ByteArray(size) { (it + seed).toByte() }
    private fun fileFor(directory: File, item: LocalRecording) = File(directory, "recording_" + item.id + ".wav")

    @Test fun legacyStaysByteForByteInPlaceAndListingReadsMetadataOnly() = withDirectory { directory ->
        val legacy = File(directory, "latest_verified_clip.wav")
        // Deliberately not a WAV: listing must not inspect or validate audio contents.
        val contents = ByteArray(684) { 0x7f }
        legacy.writeBytes(contents)
        Files.setLastModifiedTime(legacy.toPath(), FileTime.fromMillis(1700000000000))
        val originalTime = legacy.lastModified()
        val pending = File(directory, "pending_verified_clip.wav").also { it.writeBytes(byteArrayOf(1, 2, 3)) }
        val unknown = File(directory, "unrelated.wav").also { it.writeBytes(contents) }
        val repository = RecordingRepository(directory)
        val item = repository.entries().single()
        assertEquals(RecordingRepository.LEGACY_ID, item.id)
        assertEquals("Verified clip", item.title)
        assertEquals(originalTime, item.createdAtMillis)
        assertEquals(684L, item.byteCount)
        assertEquals(0.02, item.durationSeconds, 0.0)
        assertArrayEquals(contents, legacy.readBytes())
        assertEquals(originalTime, legacy.lastModified())
        assertTrue(pending.exists()); assertTrue(unknown.exists())
        assertEquals(item, RecordingRepository(directory).find(RecordingRepository.LEGACY_ID))
        rejected { repository.readPcm(RecordingRepository.LEGACY_ID) }
        assertArrayEquals(contents, legacy.readBytes())
    }

    @Test fun multipleSavesKeepStableIdsAndOriginalAudioAcrossRestart() = withDirectory { directory ->
        val legacyPcm = pcm(seed = 2)
        File(directory, "latest_verified_clip.wav").writeBytes(WavCodec.encode(legacyPcm))
        val repository = RecordingRepository(directory)
        val firstPcm = pcm(1280, 3)
        val untouched = firstPcm.copyOf()
        val first = repository.save(firstPcm)
        val secondPcm = pcm(32000, 4)
        val second = repository.save(secondPcm)
        assertNotEquals(first.id, second.id)
        assertEquals(0.04, first.durationSeconds, 0.0)
        assertEquals(1.0, second.durationSeconds, 0.0)
        assertArrayEquals(untouched, firstPcm)
        assertTrue(fileFor(directory, first).isFile)
        val restarted = RecordingRepository(directory)
        assertEquals(setOf(RecordingRepository.LEGACY_ID, first.id, second.id), restarted.entries().map { it.id }.toSet())
        assertEquals(first, restarted.find(first.id))
        assertArrayEquals(legacyPcm, restarted.readPcm(RecordingRepository.LEGACY_ID))
        assertArrayEquals(firstPcm, restarted.readPcm(first.id))
        assertArrayEquals(secondPcm, restarted.readPcm(second.id))
        assertFalse(directory.listFiles()!!.any { it.name.endsWith(".pending") })
    }

    @Test fun invalidAndPathTraversalIdsCannotReadFindOrDeleteFiles() = withDirectory { directory ->
        val repository = RecordingRepository(directory)
        val entry = repository.save(pcm())
        val invalidIds = listOf("", ".", "..", "../latest_verified_clip.wav", "..\\latest_verified_clip.wav",
            "latest_verified_clip.wav", "recording_" + entry.id + ".wav", entry.id + "/..", entry.id + "\\..",
            "C:\\recording.wav", "/recording.wav", entry.id + ":stream", entry.id + "\u0000",
            "00000000-0000-0000-0000-00000000000A")
        for (id in invalidIds) {
            rejected { repository.find(id) }
            rejected { repository.readPcm(id) }
            rejected { repository.deletePhone(id) }
        }
        assertEquals(listOf(entry), repository.entries())
        assertArrayEquals(pcm(), repository.readPcm(entry.id))
        assertNull(repository.find("00000000-0000-0000-0000-000000000000"))
    }

    @Test fun invalidSaveCannotLoseExistingClipsOrLeaveStagingFiles() = withDirectory { directory ->
        val repository = RecordingRepository(directory)
        val first = repository.save(pcm())
        val original = fileFor(directory, first).readBytes()
        for (size in listOf(0, 1, 638, 642, 32002, 32640)) {
            rejected { repository.save(ByteArray(size)) }
            assertEquals(listOf(first), repository.entries())
        }
        assertEquals(1, directory.listFiles()!!.size)
        assertArrayEquals(original, fileFor(directory, first).readBytes())
    }

    @Test fun explicitDeletionAffectsOnlySelectedPhoneItemIncludingLegacy() = withDirectory { directory ->
        val legacy = File(directory, "latest_verified_clip.wav").also { it.writeBytes(WavCodec.encode(pcm())) }
        val legacyBytes = legacy.readBytes()
        val repository = RecordingRepository(directory)
        val first = repository.save(pcm(seed = 12))
        val second = repository.save(pcm(seed = 13))
        val staging = File(directory, ".recording-interrupted.pending").also { it.writeBytes(byteArrayOf(9)) }
        repository.deletePhone(first.id)
        repository.deletePhone(first.id) // Idempotent after deletion, with no other effects.
        assertNull(repository.find(first.id))
        rejected { repository.readPcm(first.id) }
        assertEquals(setOf(RecordingRepository.LEGACY_ID, second.id), repository.entries().map { it.id }.toSet())
        assertArrayEquals(legacyBytes, legacy.readBytes())
        assertArrayEquals(pcm(seed = 13), repository.readPcm(second.id))
        repository.deletePhone(RecordingRepository.LEGACY_ID)
        assertFalse(legacy.exists())
        assertEquals(listOf(second), repository.entries())
        assertTrue(staging.exists())
    }

    @Test fun listingIgnoresUnknownStagingAndUnboundedFilesWithoutRemovingThem() = withDirectory { directory ->
        val repository = RecordingRepository(directory)
        val valid = repository.save(pcm())
        val names = listOf("recording_bad-id.wav", ".recording-interrupted.pending", "pending_verified_clip.wav",
            "recording_00000000-0000-0000-0000-000000000000.wav", "latest_verified_clip.wav")
        names.forEach { File(directory, it).writeBytes(ByteArray(32045)) }
        val directoryId = "00000000-0000-0000-0000-000000000001"
        assertTrue(File(directory, "recording_" + directoryId + ".wav").mkdir())
        assertEquals(listOf(valid), repository.entries())
        assertNull(repository.find(RecordingRepository.LEGACY_ID))
        rejected { repository.readPcm(RecordingRepository.LEGACY_ID) }
        rejected { repository.deletePhone(directoryId) }
        names.forEach { assertTrue(File(directory, it).isFile) }
    }

    @Test fun inaccessibleDirectorySaveFailureDoesNotAffectSeparateExistingLibrary() = withDirectory { directory ->
        val library = File(directory, "library").also { assertTrue(it.mkdir()) }
        val repository = RecordingRepository(library)
        val original = repository.save(pcm())
        val missing = File(directory, "gone").also { assertTrue(it.mkdir()) }
        val unavailable = RecordingRepository(missing)
        assertTrue(missing.delete())
        try { unavailable.save(pcm()); fail("Save to a removed directory was accepted") }
        catch (_: java.io.IOException) { }
        assertEquals(listOf(original), repository.entries())
        assertArrayEquals(pcm(), repository.readPcm(original.id))
        assertFalse(missing.exists())
    }
}
