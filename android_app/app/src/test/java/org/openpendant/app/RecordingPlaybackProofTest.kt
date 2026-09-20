package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

/** Synthetic metadata only. Deterministic complexity checks, not wall-clock thresholds. */
class RecordingPlaybackProofTest {
    private class Fixture(count: Int = 360) {
        val volume = RecordingVolume(UUID(1, 2), UUID(3, 4), 1)
        val id = DurableRecordingId(volume, UUID(5, 6))
        val segments = List(count) { SegmentIdentity(id, it, "ab".repeat(32), 34357) }
        val manifest = RecordingManifest(id, 1441, true, "cd".repeat(32), segments)
        // Deliberately a distinct-but-equal manifest: equality can also be O(n).
        val ticketManifest = RecordingManifest(id, 1441, true, "cd".repeat(32), segments.map { it.copy() })
        var saved = RecordingSyncSnapshot(id, manifest = manifest, phoneSegments = segments.toSet(),
            pendingReceipts = setOf(segments[0]), pendantCopy = PendantCopy.PRESENT)
        var fail = false
        val core = RecordingSyncContract(saved, RecordingSyncOwnership()) { rev, next ->
            check(!fail); assertEquals(saved.revision, rev); saved = next
        }
        init { core.authenticatedConnection(volume, true) }
    }

    @Test fun hourAndFullVolumeRequireOnlyOneFullProofForThousandsOfPcmBlocks() {
        for (count in listOf(360, 5120)) {
            val s = Fixture(count)
            val ticket = s.core.beginPlayback(s.ticketManifest)
            repeat(10_000) {
                assertTrue(s.core.isCurrent(ticket))
                assertTrue(s.core.playbackStep(ticket) {})
                assertTrue(s.core.isCurrent(ticket))
            }
            assertEquals(1L, s.core.playbackValidationScans())
            assertTrue(s.core.finishPlayback(ticket))
            assertFalse(s.core.isCurrent(ticket))
            assertEquals(s.segments.toSet(), s.saved.phoneSegments)
            s.core.close()
        }
    }

    @Test fun metadataReplacementRequiresAFreshCompleteProof() {
        val s = Fixture()
        val ticket = s.core.beginPlayback(s.ticketManifest)
        assertTrue(s.core.isCurrent(ticket))
        assertTrue(s.core.confirmReceipt(s.core.nextReceipt()!!))
        assertTrue(s.core.isCurrent(ticket))
        assertEquals(2L, s.core.playbackValidationScans())
        repeat(1000) { assertTrue(s.core.playbackStep(ticket) {}) }
        assertEquals(2L, s.core.playbackValidationScans())
        s.core.close()
    }

    @Test fun cachedProofNeverAuthorizesForeignOrInvalidatedTickets() {
        for (mode in 0..6) {
            val s = Fixture()
            val ticket = s.core.beginPlayback(s.ticketManifest)
            assertTrue(s.core.isCurrent(ticket))
            val impostor = RecordingWorkTicket(ticket.segment, ticket.kind, ticket.generation,
                ticket.instance, ticket.job, ticket.finalManifest)
            assertFalse(s.core.isCurrent(impostor))
            when (mode) {
                0 -> s.core.cancelWork(ticket)
                1 -> s.core.disconnected()
                2 -> s.core.authenticatedConnection(s.volume.copy(generation = 2), true)
                3 -> s.core.requestDeletion(UUID(7, 8), DeleteLocation.PHONE_ONLY)
                4 -> s.core.requestDeletion(UUID(7, 8), DeleteLocation.PENDANT_ONLY)
                5 -> s.core.close()
                6 -> {
                    val receipt = s.core.nextReceipt()!!
                    s.fail = true
                    assertThrows(IllegalStateException::class.java) { s.core.confirmReceipt(receipt) }
                    assertTrue(s.core.requiresReconciliation())
                }
            }
            assertFalse(s.core.isCurrent(ticket))
            if (mode in 5..6) {
                assertThrows(IllegalStateException::class.java) { s.core.playbackStep(ticket) { fail("Stale write") } }
            } else assertFalse(s.core.playbackStep(ticket) { fail("Stale write") })
            s.core.close()
        }
    }

    @Test fun newJobCannotBorrowOldProofAndIncompleteOrAlteredManifestsStayRejected() {
        val s = Fixture()
        val first = s.core.beginPlayback(s.ticketManifest)
        assertTrue(s.core.isCurrent(first)); assertTrue(s.core.finishPlayback(first))
        val second = s.core.beginPlayback(s.ticketManifest)
        assertFalse(s.core.isCurrent(first)); assertTrue(s.core.isCurrent(second))
        assertEquals(2L, s.core.playbackValidationScans())
        s.core.cancelWork(second)
        val changed = RecordingManifest(s.id, 1442, true, s.manifest.sha256, s.segments)
        assertThrows(IllegalArgumentException::class.java) { s.core.beginPlayback(changed) }
        s.core.close()
        val partial = RecordingSyncContract(s.saved.copy(phoneSegments = s.segments.dropLast(1).toSet()),
            RecordingSyncOwnership()) { _, _ -> }
        assertThrows(IllegalArgumentException::class.java) { partial.beginPlayback(s.manifest) }
        partial.close()
    }
}
