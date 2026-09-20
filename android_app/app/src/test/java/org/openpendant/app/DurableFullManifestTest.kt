package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

/** Same public vectors as the native C stream test; no key/device/audio use. */
class DurableFullManifestTest {
    private val recording = DurableRecordingId(RecordingVolume(
        UUID.fromString("01020304-0506-0708-090a-0b0c0d0e0f10"),
        UUID.fromString("11121314-1516-1718-191a-1b1c1d1e1f20"), 2),
        UUID.fromString("21222324-2526-2728-292a-2b2c2d2e2f30"))
    private val fingerprint = (1..32).joinToString("") { "%02x".format(it) }
    private fun sha(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b)
        .joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun entries(count: Int): List<DurableManifestSegment> {
        var next = (1L shl 53) + 101
        return List(count) { sequence ->
            val gap = sequence % 3 == 1
            val first = next + if(gap)320L else 0L
            next = first + 160000
            DurableManifestSegment(SegmentIdentity(recording, sequence,
                sha("public encrypted segment $sequence".toByteArray(Charsets.US_ASCII)), 34357),
                first, next, 160000, gap)
        }
    }
    private fun expected(b: ByteArray, count: Int) =
        DurableCatalogEntry(recording, count*4L+1, sha(b), count, true)

    @Test fun fullManifestMatchesIndependentPythonAndNativeCVectorBeyond64KiB() {
        val segments = entries(5120)
        val bytes = DurableManifestCodec.encode(recording, fingerprint, DurableManifestState.FINALIZED,
            segments, fullProfile=true)
        assertEquals(327808, bytes.size)
        assertEquals("f0286d4e78720f8f633d62f19ce3d7afcf81156a4e30b2a36dc98b66f8e77471", sha(bytes))
        val parsed = DurableManifestCodec.parse(bytes, expected(bytes, 5120), fingerprint, fullProfile=true)
        assertEquals(819200000L, parsed.totalCommittedSamples)
        assertEquals(segments, parsed.segments)
        assertEquals(5120, parsed.manifest.segments.size)
    }
    @Test fun oldAndFullProfilesCannotBeSilentlyInterchanged() {
        val segments = entries(3)
        val old = DurableManifestCodec.encode(recording, fingerprint, DurableManifestState.FINALIZED, segments)
        val full = DurableManifestCodec.encode(recording, fingerprint, DurableManifestState.FINALIZED,
            segments, fullProfile=true)
        assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.parse(full, expected(full,3), fingerprint)
        }
        assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.parse(old, expected(old,3), fingerprint, fullProfile=true)
        }
        assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.encode(recording, fingerprint, DurableManifestState.FINALIZED, entries(33))
        }
    }
    @Test fun fullBoundsAndResealedBadTimelineAreRejected() {
        assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.encode(recording, fingerprint, DurableManifestState.FINALIZED,
                entries(5121), fullProfile=true)
        }
        val bytes = DurableManifestCodec.encode(recording, fingerprint, DurableManifestState.FINALIZED,
            entries(1024), fullProfile=true)
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(128+1023*64,1022)
        assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.parse(bytes, expected(bytes,1024), fingerprint, fullProfile=true)
        }
    }
}
