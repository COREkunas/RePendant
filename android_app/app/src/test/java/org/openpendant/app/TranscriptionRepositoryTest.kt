package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.nio.file.Files

class TranscriptionRepositoryTest {
    private val id = "00000000-0000-0000-0000-000000000001"
    private fun document() = TranscriptDocument(id, "a".repeat(64), "synthetic-model", "b".repeat(64),
        "unit-engine-v1", "lt", 123456789L, listOf(TranscriptSegment(0, 900, "Sveiki, Lietuva!")))
    private fun directory(block: (File) -> Unit) {
        val root = Files.createTempDirectory("openpendant-transcript-unit-").toFile()
        try { block(root) } finally { root.listFiles()?.forEach { it.delete() }; root.delete() }
    }
    private fun rejected(block: () -> Unit) { try { block(); fail("Invalid transcript accepted") } catch (_: IllegalArgumentException) { } }

    @Test fun completeProvenanceAndUnicodeRoundTripAcrossRestart() = directory { root ->
        val repository = TranscriptionRepository(root)
        assertNull(repository.read(id))
        repository.saveVerified(document())
        val actual = TranscriptionRepository(root).read(id)
        assertEquals(document(), actual)
        assertEquals("Sveiki, Lietuva!", actual!!.text)
        assertEquals(1, root.list()!!.size)
    }

    @Test fun explicitIdempotentDeletionKeepsOtherSidecarsAndUnknownStaging() = directory { root ->
        val repository = TranscriptionRepository(root)
        val legacy = document().copy(recordingId = RecordingRepository.LEGACY_ID)
        repository.saveVerified(document()); repository.saveVerified(legacy)
        val pending = File(root, ".transcript-interrupted.pending").also { it.writeBytes(byteArrayOf(1)) }
        repository.delete(id); repository.delete(id)
        assertNull(repository.read(id)); assertEquals(legacy, repository.read(legacy.recordingId))
        assertTrue(pending.exists())
    }

    @Test fun invalidBindingTimestampsAndOversizeTextCannotReplaceGoodOutput() = directory { root ->
        val repository = TranscriptionRepository(root)
        repository.saveVerified(document())
        val badDocuments = listOf(document().copy(audioSha256 = "bad"), document().copy(language = "en"),
            document().copy(engineVersion = "../bad"), document().copy(segments = listOf(TranscriptSegment(-1, 1, "x"))),
            document().copy(segments = listOf(TranscriptSegment(2, 1, "x"))),
            document().copy(segments = listOf(TranscriptSegment(0, 30001, "x"))),
            document().copy(segments = listOf(TranscriptSegment(0, 10, "x".repeat(16001)))),
            document().copy(segments = listOf(TranscriptSegment(0, 1, "\u0000"))))
        badDocuments.forEach { bad -> rejected { repository.saveVerified(bad) } }
        assertEquals(document(), repository.read(id)); assertEquals(1, root.list()!!.size)
    }

    @Test fun invalidPathsAndCopiedWrongIdSidecarAreRejected() = directory { root ->
        val repository = TranscriptionRepository(root)
        repository.saveVerified(document())
        for (invalid in listOf("../x", "", "C:\\x", id + ":stream")) {
            rejected { repository.read(invalid) }; rejected { repository.delete(invalid) }
        }
        val other = "00000000-0000-0000-0000-000000000002"
        File(root, "transcript_$id.opt").copyTo(File(root, "transcript_$other.opt"))
        rejected { repository.read(other) }
        assertEquals(document(), repository.read(id))
    }

    @Test fun corruptOversizeSidecarIsNotLoadedOrSilentlyRemoved() = directory { root ->
        val file = File(root, "transcript_$id.opt").also { it.writeBytes(ByteArray(262145)) }
        val repository = TranscriptionRepository(root)
        rejected { repository.read(id) }
        assertTrue(file.exists()); assertEquals(262145, file.length().toInt())
    }
}
