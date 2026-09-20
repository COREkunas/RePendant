package org.openpendant.app

import java.util.Collections
import java.util.UUID

/** Proposed app metadata identities, NOT the installed v1 BLE wire protocol. */
data class RecordingVolume(val deviceId: UUID, val volumeId: UUID, val generation: Long) {
    init {
        require(validOwnedUuid(deviceId) && validOwnedUuid(volumeId) && generation > 0) { "Invalid owned recording volume identity" }
    }
}
data class DurableRecordingId(val volume: RecordingVolume, val recordingId: UUID) {
    init { require(validOwnedUuid(recordingId)) { "Invalid durable recording identifier" } }
}

data class SegmentIdentity(
    val recording: DurableRecordingId,
    val sequence: Int,
    val sha256: String,
    val byteCount: Long,
) {
    init {
        require(sequence >= 0 && byteCount > 0) { "Invalid sealed segment geometry" }
        require(isContentDigest(sha256)) { "Invalid segment content digest" }
    }
}

/** Immutable catalog snapshot supplied only after the future protocol validates it. */
class RecordingManifest(
    val recording: DurableRecordingId,
    val revision: Long,
    val finished: Boolean,
    val sha256: String,
    segments: List<SegmentIdentity>,
) {
    val segments: List<SegmentIdentity> = Collections.unmodifiableList(ArrayList(segments))

    init {
        require(revision >= 0 && isContentDigest(sha256)) { "Invalid recording manifest" }
        require(this.segments.size <= MAX_SEGMENTS) { "Recording metadata limit exceeded" }
        require(this.segments.withIndex().all { (index, segment) ->
            segment.recording == recording && segment.sequence == index
        }) { "Segment recording identity or ordering differs" }
    }

    /** Construction proves sequence == immutable list position. Indexing only
     * selects a candidate; full equality still checks recording/volume, digest
     * and byte count. No mutable membership cache or additional authority. */
    internal fun containsExact(segment: SegmentIdentity): Boolean =
        segments.getOrNull(segment.sequence) == segment

    override fun equals(other: Any?): Boolean = other is RecordingManifest &&
        recording == other.recording && revision == other.revision && finished == other.finished &&
        sha256 == other.sha256 && segments == other.segments

    override fun hashCode(): Int = listOf(recording, revision, finished, sha256, segments).hashCode()

    companion object {
        // Local defensive metadata ceiling, not negotiated wire/storage geometry.
        const val MAX_SEGMENTS = 5120
    }
}

internal fun isContentDigest(value: String): Boolean = Regex("[0-9a-f]{64}").matches(value)
internal fun validOwnedUuid(value: UUID): Boolean =
    (value.mostSignificantBits != 0L || value.leastSignificantBits != 0L) &&
    (value.mostSignificantBits != -1L || value.leastSignificantBits != -1L)
