package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import java.util.concurrent.CancellationException

class ModelRepositoryTest {
    private fun syntheticModel() = ByteArray(128) { it.toByte() }.also {
        byteArrayOf(0x6c, 0x6d, 0x67, 0x67).copyInto(it)
    }
    private fun spec(bytes: ByteArray) = ModelSpec("synthetic-test-only", "test.bin", bytes.size.toLong(),
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) })
    private fun directory(block: (File) -> Unit) {
        val root = Files.createTempDirectory("openpendant-model-unit-").toFile()
        try { block(root) } finally { root.listFiles()?.forEach { it.delete() }; root.delete() }
    }
    private fun rejected(block: () -> Unit) { try { block(); fail("Invalid model accepted") } catch (_: IllegalArgumentException) { } }

    @Test fun importsOnlyExactPinnedBytesAndChecksThemAtUse() = directory { root ->
        val bytes = syntheticModel()
        val repository = ModelRepository(root, spec(bytes))
        assertFalse(repository.available())
        repository.importVerified(ByteArrayInputStream(bytes))
        assertTrue(repository.available())
        repository.withVerifiedModel { model ->
            assertEquals(spec(bytes), model.spec)
            assertArrayEquals(bytes, model.file.readBytes())
        }
        assertEquals(listOf("test.bin"), root.list()!!.toList())
    }

    @Test fun availabilityDoesNotHashOrAcceptTrustAndConstructionDoesNotMutate() = directory { root ->
        val bytes = syntheticModel()
        val bad = bytes.copyOf().also { it[50] = 0 }
        val file = File(root, "test.bin").also { it.writeBytes(bad) }
        val pending = File(root, ".model-old.pending").also { it.writeBytes(byteArrayOf(1)) }
        val repository = ModelRepository(root, spec(bytes))
        assertTrue(repository.available()) // Presence is deliberately separate from verification.
        rejected { repository.withVerifiedModel { fail("Bad hash reached model engine") } }
        assertArrayEquals(bad, file.readBytes()); assertTrue(pending.exists())
    }

    @Test fun truncatedOversizeBadHashAndBadMagicCannotReplaceExistingModel() = directory { root ->
        val bytes = syntheticModel()
        val repository = ModelRepository(root, spec(bytes))
        repository.importVerified(ByteArrayInputStream(bytes))
        for (bad in listOf(bytes.copyOf(127), bytes.copyOf(129), bytes.copyOf().also { it[50] = 0 })) {
            rejected { repository.importVerified(ByteArrayInputStream(bad)) }
            assertArrayEquals(bytes, File(root, "test.bin").readBytes())
        }
        val wrongMagic = ByteArray(128)
        val wrong = ModelRepository(root, spec(wrongMagic))
        rejected { wrong.importVerified(ByteArrayInputStream(wrongMagic)) }
        assertArrayEquals(bytes, File(root, "test.bin").readBytes())
        assertEquals(1, root.list()!!.size)
    }

    @Test fun cancelDuringImportCleansOnlyOwnStagingAndPreservesExisting() = directory { root ->
        val bytes = syntheticModel()
        val repository = ModelRepository(root, spec(bytes))
        repository.importVerified(ByteArrayInputStream(bytes))
        var checks = 0
        try { repository.importVerified(ByteArrayInputStream(bytes)) { ++checks > 2 }; fail("Cancel accepted") }
        catch (_: CancellationException) { }
        assertArrayEquals(bytes, File(root, "test.bin").readBytes())
        assertEquals(1, root.list()!!.size)
    }

    @Test fun invalidSpecPathsAndUnboundedSizesAreRejected() = directory { root ->
        val valid = spec(syntheticModel())
        for (name in listOf("../test.bin", "..\\test.bin", "/test.bin", "C:test.bin", "", "x.bin:stream")) {
            rejected { ModelRepository(root, valid.copy(fileName = name)) }
        }
        rejected { ModelRepository(root, valid.copy(byteCount = ModelRepository.MAX_MODEL_BYTES + 1)) }
        rejected { ModelRepository(root, valid.copy(sha256 = "a".repeat(63))) }
        assertEquals(0, root.list()!!.size)
    }
}
