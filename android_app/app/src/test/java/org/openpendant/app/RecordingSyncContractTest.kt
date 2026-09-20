package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

/** Metadata and fake callbacks only. No files, Android, BLE, playback or ASR. */
class RecordingSyncContractTest {
    private val volume = RecordingVolume(UUID(1, 1), UUID(2, 2), 1)
    private val id = DurableRecordingId(volume, UUID(3, 3))
    private val first = SegmentIdentity(id, 0, "a".repeat(64), 100)
    private val second = SegmentIdentity(id, 1, "b".repeat(64), 200)

    private fun manifest(finished: Boolean = true, revision: Long = 1,
                         segments: List<SegmentIdentity> = listOf(first, second), digest: String = "c".repeat(64)) =
        RecordingManifest(id, revision, finished, digest, segments)

    private class Store(initial: RecordingSyncSnapshot) {
        val ownership = RecordingSyncOwnership()
        var saved = initial
        var fail = false
        val events = mutableListOf<String>()
        fun commit(expected: Long, next: RecordingSyncSnapshot) {
            check(saved.revision == expected) { "CAS conflict" }
            if (fail) throw IllegalStateException("Synthetic persistence failure")
            events += "commit"
            saved = next
        }
    }

    private fun setup(finished: Boolean = true): Pair<RecordingSyncContract, Store> {
        val store = Store(RecordingSyncSnapshot(id))
        val core = RecordingSyncContract(store.saved, store.ownership, store::commit)
        core.authenticatedConnection(volume, true)
        core.observeManifest(manifest(finished))
        return core to store
    }

    private fun download(core: RecordingSyncContract, segment: SegmentIdentity = first) {
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, segment)
        assertTrue(core.publishDownloadedSegment(ticket, segment.sha256, segment.byteCount) {})
    }

    private fun rejected(action: () -> Unit) {
        try { action(); fail("Invalid state transition accepted") }
        catch (_: IllegalArgumentException) { }
        catch (_: IllegalStateException) { }
    }

    @Test fun constructionAndSnapshotDoNotRunCallbacksOrClaimOnline() {
        var commits = 0
        val core = RecordingSyncContract(RecordingSyncSnapshot(id), RecordingSyncOwnership()) { _, _ -> commits++ }
        assertEquals(0, commits)
        assertFalse(core.remoteStateIsFresh())
        assertEquals(PendantCopy.UNKNOWN, core.snapshot().pendantCopy)
        assertNull(core.nextReceipt())
        assertNull(core.nextPendantDeletion())
    }

    @Test fun digestAndExactDeviceGenerationRecordingSegmentBindAllWork() {
        val (core, _) = setup()
        for (bad in listOf(first.copy(recording = id.copy(recordingId = UUID(9, 1))),
                           first.copy(recording = id.copy(volume = volume.copy(generation = 2))),
                           first.copy(recording = id.copy(volume = volume.copy(volumeId = UUID(9, 2)))),
                           first.copy(recording = id.copy(volume = volume.copy(deviceId = UUID(9, 3)))),
                           first.copy(sha256 = "d".repeat(64)), first.copy(byteCount = 101), first.copy(sequence = 1))) {
            rejected { core.beginWork(RecordingWork.DOWNLOAD, bad) }
        }
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        var publishes = 0
        rejected { core.publishDownloadedSegment(ticket, second.sha256, 100) { publishes++ } }
        rejected { core.publishDownloadedSegment(ticket, first.sha256, 99) { publishes++ } }
        assertEquals(0, publishes)
        assertTrue(core.publishDownloadedSegment(ticket, first.sha256, 100) { publishes++ })
        assertEquals(1, publishes)
    }

    @Test fun filePublicationBeforeMetadataBeforeReceiptNeverDeletesSource() {
        val (core, store) = setup()
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        store.events.clear()
        core.publishDownloadedSegment(ticket, first.sha256, 100) {
            assertNull(core.nextReceipt())
            store.events += "verified-file-published"
        }
        assertEquals(listOf("verified-file-published", "commit"), store.events)
        assertEquals(setOf(first), core.snapshot().phoneSegments)
        val receipt = core.nextReceipt()!!
        assertEquals(first, receipt.segment)
        assertTrue(core.confirmReceipt(receipt))
        assertFalse(core.confirmReceipt(receipt))
        assertEquals(PendantCopy.PRESENT, core.snapshot().pendantCopy)
        assertTrue(core.snapshot().deletions.isEmpty())
        assertEquals(setOf(first), core.snapshot().phoneSegments)
    }

    @Test fun metadataFailureAfterFilePublicationProducesNoReceiptOrReplay() {
        val (core, store) = setup()
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        store.fail = true
        var publications = 0
        rejected { core.publishDownloadedSegment(ticket, first.sha256, 100) { publications++ } }
        assertEquals(1, publications)
        assertTrue(core.snapshot().phoneSegments.isEmpty())
        assertNull(core.nextReceipt())
        assertTrue(core.requiresReconciliation())
        rejected { core.publishDownloadedSegment(ticket, first.sha256, 100) { publications++ } }
        assertEquals(1, publications)
    }

    @Test fun phoneOnlyDeletionPersistsSuppressionBeforeOrderedRemoval() {
        val (core, store) = setup()
        download(core)
        val operation = UUID(10, 1)
        val intent = core.requestDeletion(operation, DeleteLocation.PHONE_ONLY)
        assertTrue(store.saved.downloadSuppressed && intent.phonePending)
        assertNull(core.nextPendantDeletion())
        val calls = mutableListOf<String>()
        assertTrue(core.performPhoneDeletion(operation,
            { assertTrue(store.saved.deletions.single().phonePending); calls += "transcript" }, { calls += "audio" }))
        assertEquals(listOf("transcript", "audio"), calls)
        assertTrue(core.snapshot().phoneSegments.isEmpty())
        assertEquals(PendantCopy.PRESENT, core.snapshot().pendantCopy)
        assertFalse(core.performPhoneDeletion(operation, { fail() }, { fail() }))
        rejected { core.beginWork(RecordingWork.DOWNLOAD, first) }
        core.observeManifest(manifest()) // Reconnect/catalog replay cannot clear suppression.
        rejected { core.beginWork(RecordingWork.DOWNLOAD, first) }
        core.allowDownloadAgain()
        download(core)
        assertEquals(setOf(first), core.snapshot().phoneSegments)
    }

    @Test fun deleteIntentPersistenceFailurePerformsNoDeletionAndFencesReload() {
        val (core, store) = setup()
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        store.fail = true
        rejected { core.requestDeletion(UUID(10, 2), DeleteLocation.BOTH) }
        assertFalse(core.isCurrent(ticket))
        assertTrue(core.requiresReconciliation())
        assertFalse(core.snapshot().downloadSuppressed)
        assertTrue(core.snapshot().deletions.isEmpty())
    }

    @Test fun derivativeFailurePreservesAudioAndPendingPhoneIntent() {
        val (core, _) = setup()
        download(core)
        val op = UUID(10, 3)
        core.requestDeletion(op, DeleteLocation.PHONE_ONLY)
        var audioDeletes = 0
        rejected { core.performPhoneDeletion(op, { error("Synthetic derivative failure") }, { audioDeletes++ }) }
        assertEquals(0, audioDeletes)
        assertTrue(core.snapshot().deletions.single().phonePending)
        assertEquals(setOf(first), core.snapshot().phoneSegments)
    }

    @Test fun keepTranscriptIsExplicitAndPendantOnlyCannotDeletePhone() {
        val (core, _) = setup()
        download(core)
        val op = UUID(10, 4)
        core.requestDeletion(op, DeleteLocation.PHONE_ONLY, keepTranscript = true)
        var audio = 0
        core.performPhoneDeletion(op, { fail("Kept transcript must not be deleted") }, { audio++ })
        assertEquals(1, audio)
        val (other, _) = setup()
        download(other)
        val remote = UUID(10, 5)
        other.requestDeletion(remote, DeleteLocation.PENDANT_ONLY)
        assertFalse(other.performPhoneDeletion(remote, { fail() }, { fail() }))
        val request = other.nextPendantDeletion()!!
        assertTrue(other.confirmPendantDeletion(request, 2, manifest().sha256))
        assertEquals(setOf(first), other.snapshot().phoneSegments)
    }

    @Test fun bothOfflineShowsPendingRemoteAfterPhoneRemovalUntilExactAck() {
        val (core, store) = setup()
        download(core)
        core.disconnected()
        val op = UUID(10, 6)
        core.requestDeletion(op, DeleteLocation.BOTH)
        core.performPhoneDeletion(op, {}, {})
        assertFalse(core.remoteStateIsFresh())
        assertEquals(PendantCopy.PRESENT, core.snapshot().pendantCopy)
        assertEquals(PendantDeletion.PENDING, core.snapshot().deletions.single().pendant)
        assertNull(core.nextPendantDeletion())
        core.close()
        val restarted = RecordingSyncContract(store.saved, store.ownership, store::commit)
        assertNull(restarted.nextPendantDeletion())
        restarted.authenticatedConnection(volume, true)
        val request = restarted.nextPendantDeletion()!!
        assertEquals(op, request.intent.operationId)
        rejected { restarted.confirmPendantDeletion(request, 1, manifest().sha256) }
        rejected { restarted.confirmPendantDeletion(request, 2, "d".repeat(64)) }
        assertTrue(restarted.confirmPendantDeletion(request, 2, manifest().sha256))
        val revision = restarted.snapshot().revision
        assertTrue(restarted.confirmPendantDeletion(request, 2, manifest().sha256))
        assertEquals(revision, restarted.snapshot().revision)
        assertEquals(PendantCopy.DELETED, restarted.snapshot().pendantCopy)
    }

    @Test fun newVolumeRefusesOldPendingIntentsWithoutDeletingOldPhoneCopy() {
        for (newVolume in listOf(volume.copy(generation = 2), volume.copy(volumeId = UUID(9, 9)))) {
            val (core, store) = setup()
            download(core)
            core.requestDeletion(UUID(10, 7), DeleteLocation.PENDANT_ONLY)
            val request = core.nextPendantDeletion()!!
            core.authenticatedConnection(newVolume, true)
            assertTrue(core.snapshot().staleVolume)
            assertEquals(PendantDeletion.STALE_GENERATION, core.snapshot().deletions.single().pendant)
            assertNull(core.nextPendantDeletion())
            assertFalse(core.confirmPendantDeletion(request, 2, manifest().sha256))
            assertEquals(setOf(first), core.snapshot().phoneSegments)
            rejected { core.authenticatedConnection(volume, true) }
            core.close()
            val restarted = RecordingSyncContract(store.saved, store.ownership, store::commit)
            rejected { restarted.authenticatedConnection(volume, true) }
            assertEquals(setOf(first), restarted.snapshot().phoneSegments)
        }
    }

    @Test fun pendingRemoteDeletionCannotCrossConnectionsOrChangeOperationPayload() {
        val (core, _) = setup()
        val op = UUID(10, 8)
        val firstIntent = core.requestDeletion(op, DeleteLocation.PENDANT_ONLY)
        assertEquals(firstIntent, core.requestDeletion(op, DeleteLocation.PENDANT_ONLY))
        rejected { core.requestDeletion(op, DeleteLocation.BOTH) }
        rejected { core.requestDeletion(op, DeleteLocation.PENDANT_ONLY, keepTranscript = true) }
        val old = core.nextPendantDeletion()!!
        core.disconnected()
        core.authenticatedConnection(volume, true)
        assertFalse(core.confirmPendantDeletion(old, 2, manifest().sha256))
        val current = core.nextPendantDeletion()!!
        assertEquals(op, current.intent.operationId)
        assertTrue(core.confirmPendantDeletion(current, 2, manifest().sha256))
        rejected { core.observeManifest(manifest(revision = 3)) }
    }

    @Test fun deleteInvalidatesLateDownloadAndTranscriptCallbacks() {
        val (core, _) = setup()
        val download = core.beginWork(RecordingWork.DOWNLOAD, first)
        core.requestDeletion(UUID(10, 9), DeleteLocation.PHONE_ONLY)
        var publications = 0
        assertFalse(core.publishDownloadedSegment(download, first.sha256, 100) { publications++ })
        assertEquals(0, publications)
        val (other, _) = setup()
        download(other)
        val asr = other.beginWork(RecordingWork.TRANSCRIPTION, first)
        other.requestDeletion(UUID(10, 10), DeleteLocation.BOTH)
        assertFalse(other.completeLocalWork(asr) { publications++ })
        assertEquals(0, publications)
    }

    @Test fun processRestartAndCancelInvalidateTicketsWithoutStartupCallbacks() {
        val (core, store) = setup()
        val old = core.beginWork(RecordingWork.DOWNLOAD, first)
        val staleSnapshot = store.saved
        core.cancelWork(old)
        assertFalse(core.publishDownloadedSegment(old, first.sha256, 100) { fail() })
        core.close()
        val commits = store.events.size
        val restored = RecordingSyncContract(staleSnapshot, store.ownership, store::commit)
        assertEquals(commits, store.events.size)
        assertFalse(restored.publishDownloadedSegment(old, first.sha256, 100) { fail() })
        restored.authenticatedConnection(volume, true)
        // This fake store still has the first coordinator's newer cancel commit:
        // CAS prevents stale restored metadata from launching any work.
        rejected { restored.beginWork(RecordingWork.DOWNLOAD, first) }
    }

    @Test fun activeRecordingCanSyncSealedSegmentsButCannotBeDeleted() {
        val (core, _) = setup(finished = false)
        download(core)
        for (location in DeleteLocation.entries) rejected { core.requestDeletion(UUID.randomUUID(), location) }
        core.observeManifest(manifest(finished = true, revision = 2, digest = "d".repeat(64)))
        assertTrue(core.requestDeletion(UUID(10, 11), DeleteLocation.BOTH).phonePending)
    }

    @Test fun sealedManifestAppendRulesRejectChangedDigestsAndStaleRevision() {
        val (core, _) = setup(finished = false)
        rejected { core.observeManifest(manifest(revision = 0)) }
        rejected { core.observeManifest(manifest(finished = false, digest = "d".repeat(64))) }
        rejected { core.observeManifest(manifest(finished = false, revision = 2, segments = listOf(first.copy(sha256 = "e".repeat(64))))) }
        val third = SegmentIdentity(id, 2, "d".repeat(64), 50)
        core.observeManifest(manifest(finished = false, revision = 2, segments = listOf(first, second, third)))
        assertEquals(3, core.snapshot().manifest!!.segments.size)
    }

    @Test fun authenticationUnsupportedProtocolAndWrongDeviceDoNotEnableRemoteWork() {
        val (core, _) = setup()
        core.disconnected()
        rejected { core.beginWork(RecordingWork.DOWNLOAD, first) }
        rejected { core.authenticatedConnection(volume, false) }
        rejected { core.authenticatedConnection(volume.copy(deviceId = UUID(99, 1)), true) }
        assertFalse(core.remoteStateIsFresh())
        assertNull(core.nextReceipt())
        assertNull(core.nextPendantDeletion())
    }

    @Test fun publicationAndPersistenceCallbacksCannotReenterMutation() {
        val (core, _) = setup()
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        core.publishDownloadedSegment(ticket, first.sha256, 100) {
            rejected { core.requestDeletion(UUID(30, 1), DeleteLocation.BOTH) }
            rejected { core.disconnected() }
        }
        assertTrue(core.snapshot().deletions.isEmpty())
        val secondTicket = core.beginWork(RecordingWork.DOWNLOAD, second)
        assertNotNull(core.nextReceipt()) // First segment is already durable.
        core.publishDownloadedSegment(secondTicket, second.sha256, second.byteCount) {
            assertNull(core.nextReceipt()) // No request escapes an in-flight callback.
        }
        val op = UUID(30, 2)
        core.requestDeletion(op, DeleteLocation.BOTH)
        assertNotNull(core.nextPendantDeletion())
        core.performPhoneDeletion(op, { assertNull(core.nextPendantDeletion()) }, {})
    }

    @Test fun ambiguousPublicationFencesAllWorkUntilExternalReconciliationAndReplacement() {
        val (core, store) = setup()
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        var filePublished = false
        store.fail = true
        rejected { core.publishDownloadedSegment(ticket, first.sha256, 100) { filePublished = true } }
        assertTrue(filePublished && core.requiresReconciliation())
        store.fail = false
        assertNull(core.nextReceipt())
        assertNull(core.nextPendantDeletion())
        assertFalse(core.remoteStateIsFresh())
        rejected { core.authenticatedConnection(volume, true) }
        rejected { core.beginWork(RecordingWork.DOWNLOAD, first) }
        rejected { core.requestDeletion(UUID(50, 1), DeleteLocation.BOTH) }
        rejected { core.completeLocalWork(ticket) { fail("Fenced callbacks must not execute") } }
        rejected { RecordingSyncContract(store.saved, store.ownership, store::commit) }
        core.close()
        // Synthetic reconciliation knows our fake file was published and its
        // verified digest matched. Real file/DB reconciliation is NOT supplied.
        store.saved = store.saved.copy(revision = store.saved.revision + 1,
            phoneSegments = setOf(first), pendingReceipts = setOf(first))
        val reconciled = RecordingSyncContract(store.saved, store.ownership, store::commit)
        assertFalse(reconciled.requiresReconciliation())
        reconciled.authenticatedConnection(volume, true)
        assertNotNull(reconciled.nextReceipt())
        rejected { reconciled.beginWork(RecordingWork.DOWNLOAD, first) }
        val play = reconciled.beginWork(RecordingWork.PLAYBACK, first)
        assertTrue(reconciled.completeLocalWork(play) {})
    }

    @Test fun audioRemovedButMetadataCommitFailsCannotRepeatMutationWithoutReconciliation() {
        val (core, store) = setup()
        download(core)
        val op = UUID(50, 2)
        core.requestDeletion(op, DeleteLocation.BOTH)
        var transcriptDeletes = 0
        var audioDeletes = 0
        rejected { core.performPhoneDeletion(op, { transcriptDeletes++ }, { audioDeletes++; store.fail = true }) }
        assertEquals(1, transcriptDeletes)
        assertEquals(1, audioDeletes)
        assertTrue(core.requiresReconciliation())
        assertTrue(core.snapshot().deletions.single().phonePending) // No false completion.
        assertNull(core.nextPendantDeletion())
        assertNull(core.nextReceipt())
        store.fail = false
        rejected { core.performPhoneDeletion(op, { transcriptDeletes++ }, { audioDeletes++ }) }
        rejected { core.beginWork(RecordingWork.PLAYBACK, first) }
        assertEquals(1, transcriptDeletes)
        assertEquals(1, audioDeletes)
        core.close()
        store.saved = store.saved.copy(revision = store.saved.revision + 1,
            phoneSegments = emptySet(), pendingReceipts = emptySet(),
            deletions = store.saved.deletions.map { it.copy(phonePending = false) })
        val reconciled = RecordingSyncContract(store.saved, store.ownership, store::commit)
        reconciled.authenticatedConnection(volume, true)
        assertEquals(op, reconciled.nextPendantDeletion()!!.intent.operationId)
        assertFalse(reconciled.performPhoneDeletion(op, { fail() }, { fail() }))
    }

    @Test fun callbackThrowsBeforeMetadataAndStillRequiresReconciliation() {
        val (core, store) = setup()
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        val revision = store.saved.revision
        rejected { core.publishDownloadedSegment(ticket, first.sha256, 100) { error("Unknown partial publication") } }
        assertTrue(core.requiresReconciliation())
        assertEquals(revision, store.saved.revision)
        rejected { core.publishDownloadedSegment(ticket, first.sha256, 100) { fail() } }
    }

    @Test fun revisionExhaustionAfterPublicationFencesWithoutRepeatingFileCallback() {
        val store = Store(RecordingSyncSnapshot(id, revision = Long.MAX_VALUE - 1,
            manifest = manifest(), pendantCopy = PendantCopy.PRESENT))
        val core = RecordingSyncContract(store.saved, store.ownership, store::commit)
        core.authenticatedConnection(volume, true)
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        assertEquals(Long.MAX_VALUE, store.saved.revision)
        var publications = 0
        rejected { core.publishDownloadedSegment(ticket, first.sha256, first.byteCount) { publications++ } }
        assertEquals(1, publications)
        assertTrue(core.requiresReconciliation())
        assertTrue(core.snapshot().phoneSegments.isEmpty())
        assertNull(core.nextReceipt())
        assertFalse(core.remoteStateIsFresh())
        rejected { core.publishDownloadedSegment(ticket, first.sha256, first.byteCount) { publications++ } }
        rejected { core.beginWork(RecordingWork.DOWNLOAD, first) }
        assertEquals(1, publications)
    }

    @Test fun revisionExhaustionAfterRemovalFencesWithoutRepeatingDeleteCallbacks() {
        val store = Store(RecordingSyncSnapshot(id, revision = Long.MAX_VALUE - 1,
            manifest = manifest(), pendantCopy = PendantCopy.PRESENT,
            phoneSegments = setOf(first), pendingReceipts = setOf(first)))
        val core = RecordingSyncContract(store.saved, store.ownership, store::commit)
        core.authenticatedConnection(volume, true)
        val op = UUID(50, 3)
        core.requestDeletion(op, DeleteLocation.BOTH)
        assertEquals(Long.MAX_VALUE, store.saved.revision)
        var transcriptDeletes = 0
        var audioDeletes = 0
        rejected { core.performPhoneDeletion(op, { transcriptDeletes++ }, { audioDeletes++ }) }
        assertEquals(1, transcriptDeletes)
        assertEquals(1, audioDeletes)
        assertTrue(core.requiresReconciliation())
        assertTrue(core.snapshot().deletions.single().phonePending)
        assertNull(core.nextReceipt())
        assertNull(core.nextPendantDeletion())
        rejected { core.performPhoneDeletion(op, { transcriptDeletes++ }, { audioDeletes++ }) }
        rejected { core.beginWork(RecordingWork.PLAYBACK, first) }
        assertEquals(1, transcriptDeletes)
        assertEquals(1, audioDeletes)
    }

    @Test fun deletionOperationSentinelsAreRejectedOnRequestAndMetadataConstruction() {
        val (core, store) = setup()
        val revision = store.saved.revision
        for (sentinel in listOf(UUID(0, 0), UUID(-1, -1))) {
            rejected { core.requestDeletion(sentinel, DeleteLocation.BOTH) }
            rejected {
                RecordingSyncSnapshot(id, manifest = manifest(), deletions = listOf(
                    RecordingDeletionIntent(sentinel, id, manifest().sha256,
                        DeleteLocation.PENDANT_ONLY, false, false, PendantDeletion.PENDING)))
            }
        }
        assertEquals(revision, store.saved.revision)
        assertTrue(core.snapshot().deletions.isEmpty())
        assertFalse(core.requiresReconciliation()) // No file/metadata operation began.
    }

    @Test fun sharedOwnershipRejectsDuplicateBeforeAnyFileCallbackAndCloseInvalidatesOldOwner() {
        val (core, store) = setup()
        val ticket = core.beginWork(RecordingWork.DOWNLOAD, first)
        assertThrows(RecordingSyncBusyException::class.java) {
            RecordingSyncContract(store.saved, store.ownership, store::commit)
        }
        assertFalse(core.requiresReconciliation())
        assertTrue(core.isCurrent(ticket))
        val before = store.events.size
        core.close()
        core.close()
        assertEquals(before, store.events.size)
        assertFalse(core.isCurrent(ticket))
        rejected { core.publishDownloadedSegment(ticket, first.sha256, 100) { fail() } }
        val next = RecordingSyncContract(store.saved, store.ownership, store::commit)
        assertFalse(next.publishDownloadedSegment(ticket, first.sha256, 100) { fail() })
        next.authenticatedConnection(volume, true)
        download(next)
    }

    @Test fun snapshotsDefensivelyCopyCollectionsAndRejectCorruptDeletionScope() {
        val segments = mutableListOf(first, second)
        val manifest = manifest(segments = segments)
        segments.clear()
        assertEquals(2, manifest.segments.size)
        val local = mutableSetOf(first)
        val initial = RecordingSyncSnapshot(id, manifest = manifest, pendantCopy = PendantCopy.PRESENT, phoneSegments = local)
        val core = RecordingSyncContract(initial, RecordingSyncOwnership()) { _, _ -> }
        local.clear()
        assertEquals(setOf(first), core.snapshot().phoneSegments)
        val bad = RecordingDeletionIntent(UUID(31, 1), id, manifest.sha256, DeleteLocation.PENDANT_ONLY,
            false, true, PendantDeletion.PENDING)
        rejected { RecordingSyncContract(initial.copy(downloadSuppressed = true, deletions = listOf(bad)), RecordingSyncOwnership()) { _, _ -> } }
        rejected { RecordingSyncContract(initial.copy(pendantCopy = PendantCopy.DELETED), RecordingSyncOwnership()) { _, _ -> } }
    }

    @Test fun malformedContentIdentitiesAndMetadataOverflowAreRejected() {
        for (digest in listOf("", "A".repeat(64), "a".repeat(63), "../a")) rejected { first.copy(sha256 = digest) }
        rejected { first.copy(sequence = -1) }
        rejected { first.copy(byteCount = 0) }
        rejected { RecordingManifest(id, 1, true, "c".repeat(64), listOf(second, first)) }
        val max = RecordingSyncSnapshot(id, revision = Long.MAX_VALUE, manifest = manifest())
        val core = RecordingSyncContract(max, RecordingSyncOwnership()) { _, _ -> fail("Overflow must not persist") }
        core.authenticatedConnection(volume, true)
        rejected { core.requestDeletion(UUID(32, 1), DeleteLocation.PHONE_ONLY) }
        rejected { volume.copy(deviceId = UUID(0, 0)) }
        rejected { volume.copy(deviceId = UUID(-1, -1)) }
        rejected { volume.copy(volumeId = UUID(0, 0)) }
        rejected { volume.copy(volumeId = UUID(-1, -1)) }
        rejected { volume.copy(generation = 0) }
        rejected { volume.copy(generation = -1) }
        rejected { id.copy(recordingId = UUID(0, 0)) }
        rejected { id.copy(recordingId = UUID(-1, -1)) }
        assertEquals(Long.MAX_VALUE, volume.copy(generation = Long.MAX_VALUE).generation)
    }
}
