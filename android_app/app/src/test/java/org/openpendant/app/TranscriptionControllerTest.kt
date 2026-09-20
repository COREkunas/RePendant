package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class TranscriptionControllerTest {
    private class Fixture(val root: File) {
        val recordings = RecordingRepository(root)
        val transcripts = TranscriptionRepository(root)
        val executor = Executors.newSingleThreadExecutor()
        val events = Collections.synchronizedList(mutableListOf<TranscriptionState>())
        val bytes = ByteArray(128) { it.toByte() }.also { byteArrayOf(0x6c, 0x6d, 0x67, 0x67).copyInto(it) }
        val spec = ModelSpec("test-only", "test.bin", bytes.size.toLong(), hash(bytes))
        val models = ModelRepository(root, spec)
        var engineCalls = 0
        var handler: (FloatArray, TranscriptionEngine.Callbacks) -> List<TranscriptSegment> = { _, callbacks ->
            callbacks.onProgress(50)
            listOf(TranscriptSegment(0, 20, "Labas"))
        }
        val engine = object : TranscriptionEngine {
            override val version = "unit-engine-v1"
            override fun transcribe(modelPath: String, pcm16kMono: FloatArray, language: String,
                callbacks: TranscriptionEngine.Callbacks): List<TranscriptSegment> {
                engineCalls++
                assertEquals("lt", language)
                return handler(pcm16kMono, callbacks)
            }
        }
        fun controller(dispatch: (() -> Unit) -> Unit = { it() }) = TranscriptionController(recordings,
            transcripts, models, engine, dispatch, { events.add(it) }, executor, { 123456L })
        fun installModel() = models.importVerified(ByteArrayInputStream(bytes))
        fun flush() { executor.submit {}.get(5, TimeUnit.SECONDS) }
        companion object {
            fun hash(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes)
                .joinToString("") { "%02x".format(it.toInt() and 255) }
        }
    }
    private fun fixture(block: (Fixture) -> Unit) {
        val root = Files.createTempDirectory("openpendant-job-unit-").toFile()
        val fixture = Fixture(root)
        try { block(fixture) } finally {
            fixture.executor.shutdownNow(); fixture.executor.awaitTermination(5, TimeUnit.SECONDS)
            root.listFiles()?.forEach { it.delete() }; root.delete()
        }
    }

    @Test fun constructionNeverReadsAudioOrStartsAnEngine() = fixture { f ->
        val broken = File(f.root, "latest_verified_clip.wav").also { it.writeBytes(ByteArray(684)) }
        val original = broken.readBytes()
        val controller = f.controller()
        assertEquals(TranscriptionPhase.IDLE, controller.snapshot().phase)
        assertEquals(0, f.engineCalls); assertArrayEquals(original, broken.readBytes())
        assertEquals(0, f.events.size)
        controller.close()
    }

    @Test fun successfulExplicitJobUsesRawPcmAndBindsAllProvenance() = fixture { f ->
        f.installModel()
        val pcm = ByteArray(640)
        pcm[0] = 0; pcm[1] = 0x40 // 0.5, deliberately not normalized preview audio.
        pcm[2] = 0; pcm[3] = 0x80.toByte() // -1.0
        val item = f.recordings.save(pcm)
        var retained: FloatArray? = null
        f.handler = { samples, callbacks ->
            assertEquals(0.5f, samples[0], 0f); assertEquals(-1f, samples[1], 0f)
            retained = samples
            callbacks.onProgress(80); callbacks.onProgress(10); callbacks.onProgress(999)
            listOf(TranscriptSegment(0, 20, "Labas, Lietuva"))
        }
        val controller = f.controller()
        assertTrue(controller.start(item.id)); f.flush()
        val output = f.transcripts.read(item.id)!!
        assertEquals(Fixture.hash(WavCodec.encode(pcm)), output.audioSha256)
        assertEquals(f.spec.sha256, output.modelSha256)
        assertEquals("unit-engine-v1", output.engineVersion)
        assertEquals("lt", output.language); assertEquals(123456L, output.createdAtMillis)
        assertEquals("Labas, Lietuva", output.text)
        assertEquals(TranscriptionPhase.COMPLETE, controller.snapshot().phase)
        assertTrue(retained!!.all { it == 0f })
        assertArrayEquals(pcm, f.recordings.readPcm(item.id))
        assertTrue(f.events.filter { it.phase == TranscriptionPhase.RUNNING }.all { it.progress <= 99 })
        controller.close()
    }

    @Test fun badModelFailsBeforeAnyAudioDecodeAndNeverStartsNative() = fixture { f ->
        File(f.root, "latest_verified_clip.wav").writeBytes(ByteArray(684))
        val controller = f.controller()
        assertTrue(controller.start(RecordingRepository.LEGACY_ID)); f.flush()
        assertEquals(TranscriptionPhase.FAILED, controller.snapshot().phase)
        assertEquals(0, f.engineCalls); assertNull(f.transcripts.read(RecordingRepository.LEGACY_ID))
        controller.close()
    }

    @Test fun onlyOneJobAndCancelledEngineResultCannotBecomeSuccess() = fixture { f ->
        f.installModel()
        val item = f.recordings.save(ByteArray(640))
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        f.handler = { _, callbacks ->
            entered.countDown(); assertTrue(release.await(5, TimeUnit.SECONDS))
            assertTrue(callbacks.isCancelled()); callbacks.onProgress(100)
            listOf(TranscriptSegment(0, 20, "This late result must not save"))
        }
        val controller = f.controller()
        assertTrue(controller.start(item.id)); assertTrue(entered.await(5, TimeUnit.SECONDS))
        assertFalse(controller.start(item.id))
        controller.cancel()
        assertEquals(TranscriptionPhase.CANCELLING, controller.snapshot().phase)
        assertFalse(controller.start(item.id))
        release.countDown(); f.flush()
        assertEquals(TranscriptionPhase.CANCELLED, controller.snapshot().phase)
        assertEquals(1, f.engineCalls); assertNull(f.transcripts.read(item.id))
        assertNotNull(f.recordings.find(item.id)); controller.close()
    }

    @Test fun phoneDeletionCancelsNativeAndPreventsLateSidecarResurrection() = fixture { f ->
        f.installModel()
        val item = f.recordings.save(ByteArray(640))
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        f.handler = { _, _ -> entered.countDown(); assertTrue(release.await(5, TimeUnit.SECONDS));
            listOf(TranscriptSegment(0, 20, "Late")) }
        val controller = f.controller()
        controller.start(item.id); assertTrue(entered.await(5, TimeUnit.SECONDS))
        controller.deletePhone(item.id)
        assertNull(f.recordings.find(item.id)); assertNull(f.transcripts.read(item.id))
        release.countDown(); f.flush()
        assertNull(f.transcripts.read(item.id)); assertNull(f.recordings.find(item.id))
        assertEquals(TranscriptionPhase.CANCELLED, controller.snapshot().phase)
        controller.close()
    }

    @Test fun completedPhoneDeletionRemovesTranscriptButKeepsOtherRecordings() = fixture { f ->
        f.installModel()
        val item = f.recordings.save(ByteArray(640)); val other = f.recordings.save(ByteArray(640))
        val controller = f.controller()
        controller.start(item.id); f.flush(); assertNotNull(f.transcripts.read(item.id))
        controller.deletePhone(item.id)
        assertNull(f.recordings.find(item.id)); assertNull(f.transcripts.read(item.id))
        assertNotNull(f.recordings.find(other.id)); assertEquals(TranscriptionPhase.IDLE, controller.snapshot().phase)
        controller.close()
    }

    @Test fun failedTranscriptDeletionLeavesTheWavVisibleAndByteForByteUnchanged() = fixture { f ->
        val pcm = ByteArray(640) { it.toByte() }
        val item = f.recordings.save(pcm)
        // A non-file at the exact sidecar path is a deterministic deletion
        // failure on both Windows and Android, without changing permissions.
        val blockedSidecar = File(f.root, "transcript_${item.id}.opt")
        assertTrue(blockedSidecar.mkdir())
        val controller = f.controller()
        try { controller.deletePhone(item.id); fail("Sidecar deletion unexpectedly succeeded") }
        catch (_: IllegalArgumentException) { }
        assertEquals(listOf(item), f.recordings.entries())
        assertArrayEquals(pcm, f.recordings.readPcm(item.id))
        assertTrue(blockedSidecar.isDirectory)
        // Retrying after the synthetic obstruction is resolved removes the
        // original recording normally; no invisible audio/transcript orphan.
        assertTrue(blockedSidecar.delete())
        controller.deletePhone(item.id)
        assertNull(f.recordings.find(item.id)); assertNull(f.transcripts.read(item.id))
        controller.close()
    }

    @Test fun failedTranscriptDeletionStillCancelsActiveGenerationWithoutLateOutput() = fixture { f ->
        f.installModel()
        val pcm = ByteArray(640) { it.toByte() }
        val item = f.recordings.save(pcm)
        val entered = CountDownLatch(1); val release = CountDownLatch(1)
        f.handler = { _, _ -> entered.countDown(); assertTrue(release.await(5, TimeUnit.SECONDS));
            listOf(TranscriptSegment(0, 20, "Late result must not return")) }
        val controller = f.controller()
        controller.start(item.id); assertTrue(entered.await(5, TimeUnit.SECONDS))
        val blockedSidecar = File(f.root, "transcript_${item.id}.opt")
        assertTrue(blockedSidecar.mkdir())
        try { controller.deletePhone(item.id); fail("Sidecar deletion unexpectedly succeeded") }
        catch (_: IllegalArgumentException) { }
        assertEquals(listOf(item), f.recordings.entries())
        assertArrayEquals(pcm, f.recordings.readPcm(item.id))
        assertTrue(blockedSidecar.delete())
        release.countDown(); f.flush()
        assertNull(f.transcripts.read(item.id))
        assertEquals(TranscriptionPhase.CANCELLED, controller.snapshot().phase)
        assertNotNull(f.recordings.find(item.id)); controller.close()
    }

    @Test fun failureCannotPublishPartialTextOrOverwriteExistingSuccessfulTranscript() = fixture { f ->
        f.installModel()
        val item = f.recordings.save(ByteArray(640)); val controller = f.controller()
        controller.start(item.id); f.flush()
        val saved = f.transcripts.read(item.id)
        f.handler = { _, callbacks -> callbacks.onProgress(50); throw IllegalStateException("private transcript text") }
        controller.start(item.id); f.flush()
        assertEquals(TranscriptionPhase.FAILED, controller.snapshot().phase)
        assertEquals(saved, f.transcripts.read(item.id))
        assertFalse(controller.snapshot().message!!.contains("private")); controller.close()
    }

    @Test fun queuedOldCallbacksAreDiscardedAfterDeletionOrClose() = fixture { f ->
        f.installModel()
        val item = f.recordings.save(ByteArray(640))
        val callbacks = Collections.synchronizedList(mutableListOf<() -> Unit>())
        val controller = f.controller { callbacks.add(it) }
        controller.start(item.id); f.flush()
        controller.deletePhone(item.id)
        callbacks.toList().forEach { it() }
        assertTrue(f.events.none { it.phase == TranscriptionPhase.COMPLETE })
        assertEquals(TranscriptionPhase.IDLE, f.events.last().phase)
        f.events.clear(); controller.close(); callbacks.toList().forEach { it() }
        assertTrue(f.events.isEmpty())
    }

    @Test fun nativeOutOfMemoryAndMissingLibraryAreExplicitFailuresWithoutPartialOutput() = fixture { f ->
        f.installModel()
        val item = f.recordings.save(ByteArray(640))
        val controller = f.controller()
        for (failure in listOf(OutOfMemoryError("private native detail"), UnsatisfiedLinkError("private path"))) {
            f.handler = { _, _ -> throw failure }
            assertTrue(controller.start(item.id)); f.flush()
            assertEquals(TranscriptionPhase.FAILED, controller.snapshot().phase)
            assertFalse(controller.snapshot().busy)
            assertFalse(controller.snapshot().message!!.contains("private"))
            assertNull(f.transcripts.read(item.id)); assertNotNull(f.recordings.find(item.id))
        }
        controller.close()
    }

    @Test fun detachedUiDispatcherCannotLeaveControllerPermanentlyBusy() = fixture { f ->
        f.installModel()
        val item = f.recordings.save(ByteArray(640))
        val controller = f.controller { throw IllegalStateException("Detached UI") }
        assertTrue(controller.start(item.id)); f.flush()
        assertEquals(TranscriptionPhase.COMPLETE, controller.snapshot().phase)
        assertNotNull(f.transcripts.read(item.id))
        controller.close()
    }
}
