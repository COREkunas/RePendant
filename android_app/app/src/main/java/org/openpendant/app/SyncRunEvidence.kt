package org.openpendant.app

/** Public counters only, process-local and non-authoritative. No audio, keys or
 * persistent recording IDs; never used to authorize a receipt or resume. */
internal data class SyncRunEvidence(val startedMillis: Long=0, val elapsedMillis: Long=0,
    val receivedBytes: Long=0, val resumedOffsetBytes: Long=0, val connections: Int=1,
    val reconnects: Int=0, val newSegments: Int=0, val completed: Boolean=false,
    val firstRangeMillis: Long=0, val followingRangeMillis: Long=0,
    val metadataMillis: Long=0, val receiptMillis: Long=0,
    val receiptBatches: Int=0, val batchedSegments: Int=0,
    val failureClass: String="", val failureSite: String="",
    val detailsOnly: Boolean=false, val catalogCalls: Int=0, val manifestCalls: Int=0,
    val payloadCalls: Int=0, val receiptCalls: Int=0, val deleteCalls: Int=0)
