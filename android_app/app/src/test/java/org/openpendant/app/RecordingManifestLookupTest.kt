package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

/** Pure metadata equivalence tests: no Bluetooth, files, keys or audio. */
class RecordingManifestLookupTest {
    private val id = DurableRecordingId(RecordingVolume(UUID(1,2), UUID(3,4), 2), UUID(5,6))
    private fun parts(count: Int) = (0 until count).map {
        SegmentIdentity(id, it, if (it % 2 == 0) "ab".repeat(32) else "cd".repeat(32), 34357)
    }
    private fun manifest(parts: List<SegmentIdentity>) =
        RecordingManifest(id, parts.size * 4L + 1, true, "ef".repeat(32), parts)

    @Test fun exactLookupMatchesHistoricalMembershipAcrossAll5120Positions() {
        val rows = parts(5120)
        val value = manifest(rows)
        val foreign = listOf(id.copy(recordingId=UUID(7,8)),
            id.copy(volume=id.volume.copy(deviceId=UUID(9,10))),
            id.copy(volume=id.volume.copy(volumeId=UUID(11,12))),
            id.copy(volume=id.volume.copy(generation=3)))
        rows.forEach { row ->
            // A newly constructed equal identity must work; reference equality
            // or merely accepting the index would not satisfy these checks.
            val variants = listOf(row.copy(), row.copy(sha256="fe".repeat(32)),
                row.copy(byteCount=34356), row.copy(sequence=5120), row.copy(sequence=Int.MAX_VALUE)) +
                foreign.map { row.copy(recording=it) }
            variants.forEach { requested ->
                assertEquals(value.segments.contains(requested), value.containsExact(requested))
            }
        }
    }

    @Test fun emptyAndSingleEntryBoundsRetainExactIdentityCheck() {
        val row = parts(1).single()
        assertFalse(manifest(emptyList()).containsExact(row))
        val value = manifest(listOf(row))
        assertTrue(value.containsExact(row.copy()))
        assertFalse(value.containsExact(row.copy(sequence=1)))
        assertFalse(value.containsExact(row.copy(sequence=Int.MAX_VALUE)))
        assertFalse(value.containsExact(row.copy(byteCount=1)))
    }

    @Test fun constructorMustEstablishPositionInvariantBeforeLookupCanExist() {
        val rows = parts(3)
        val invalid = listOf(rows.reversed(), listOf(rows[1]), listOf(rows[0],rows[0]),
            listOf(rows[0], rows[1].copy(recording=id.copy(recordingId=UUID(7,8)))))
        invalid.forEach { assertThrows(IllegalArgumentException::class.java) { manifest(it) } }
        assertThrows(IllegalArgumentException::class.java) { manifest(parts(5121)) }
    }

    @Test fun callerMutationAndAttemptedListMutationCannotInvalidateTheProof() {
        val original = parts(3)
        val source = original.toMutableList()
        val value = manifest(source)
        source.clear()
        original.forEach { assertTrue(value.containsExact(it)) }
        assertThrows(UnsupportedOperationException::class.java) {
            (value.segments as MutableList<SegmentIdentity>)[0] = original[0].copy(sha256="fe".repeat(32))
        }
        assertThrows(UnsupportedOperationException::class.java) {
            (value.segments as MutableList<SegmentIdentity>).clear()
        }
        original.forEach { assertTrue(value.containsExact(it)) }
    }

    @Test fun manifestEqualityAndSerializationInputsRemainUnchanged() {
        val original = parts(5120)
        val left = manifest(original)
        val right = manifest(original.map { it.copy() })
        original.forEach { left.containsExact(it) }
        assertEquals(left,right)
        assertEquals(left.hashCode(),right.hashCode())
        assertEquals(original,left.segments)
    }
}
