package org.openpendant.app

import java.util.concurrent.CancellationException

/** Future authenticated durable-protocol adapter, NOT a implemented BLE opcode.
 * Must enforce a finite I/O timeout, current authenticated volume, immutable
 * catalog authorization, and return exclusive ownership of at most maximumBytes.
 * Engine never sends receipts, deletes a source or starts plaintext processing.
 */
fun interface EncryptedSegmentTransport {
    fun read(segment: SegmentIdentity, offset: Long, maximumBytes: Int): EncryptedSegmentChunk
}
class EncryptedSegmentChunk(val segment: SegmentIdentity, val offset: Long, val bytes: ByteArray) {
    override fun toString() = "EncryptedSegmentChunk(bytes=${bytes.size})"
}
data class SegmentDownloadResult(val segment: SegmentIdentity, val resumedOffset: Long, val reusedVerifiedFile: Boolean)
class SegmentDownloadException : IllegalStateException("Encrypted segment transfer stopped; no receipt issued")

/** Synchronous single job; invoke on an I/O worker. A caller must obtain a fresh
 * DOWNLOAD ticket from RecordingSyncContract after authenticated capability /
 * catalog admission. Its existing CAS commit must be durably implemented.
 * Cancellation/stale tickets cannot authorize a receipt. A cancellation during
 * publication can leave verified ciphertext for reconciliation, never a receipt.
 * A late transport return is wiped.
 * A successfully verified ciphertext is NOT authenticated/decrypted plaintext.
 */
class SegmentDownloadEngine private constructor(
    private val store: DurableSegmentStore,
    private val clockMillis: () -> Long,
    private val diagnostic:((String,String,Int)->Unit)?,
) {
    constructor(store:DurableSegmentStore,clockMillis:()->Long={System.nanoTime()/1_000_000}):this(store,clockMillis,null)
    fun download(expected: EncryptedSegmentExpectation, coordinator: RecordingSyncContract,
                 ticket: RecordingWorkTicket, transport: EncryptedSegmentTransport,
                 isCancelled: () -> Boolean = { false }, onProgress: (Int) -> Unit = {},
                 onCheckpoint: (Long) -> Unit = {}): SegmentDownloadResult {
        require(ticket.kind == RecordingWork.DOWNLOAD && ticket.segment == expected.segment) { "Download work binding differs" }
        val started = clockMillis()
        var phase="download_admission"
        fun current() {
            if (isCancelled() || !coordinator.isCurrent(ticket)) throw CancellationException("Segment download cancelled")
            val now = clockMillis()
            if (now < started || now - started >= MAX_JOB_MILLIS) throw SegmentDownloadException()
        }
        try {
            current()
            phase="store_open"
            return store.open(expected).use { session ->
                val resumed = session.offset.toLong()
                val reused = session.complete
                onCheckpoint(resumed)
                var reads = 0
                val aggregate = ByteArray(DurableSegmentStore.MAX_CHUNK_BYTES)
                var buffered = 0
                try {
                    while (!session.complete && session.offset < expected.totalBytes) {
                        current()
                        if (++reads > MAX_READS) throw SegmentDownloadException()
                        val networkOffset = session.offset + buffered
                        val maximum = minOf(aggregate.size - buffered, expected.totalBytes - networkOffset)
                        phase="transport_read"
                        val chunk = transport.read(expected.segment, networkOffset.toLong(), maximum)
                        try {
                            current()
                            if (chunk.segment != expected.segment || chunk.offset != networkOffset.toLong() ||
                                chunk.bytes.size > maximum) session.mismatch()
                            if (chunk.bytes.isEmpty()) throw SegmentDownloadException()
                            chunk.bytes.copyInto(aggregate, buffered)
                            buffered += chunk.bytes.size
                        } finally { chunk.bytes.fill(0) }
                        if (buffered == aggregate.size || session.offset + buffered == expected.totalBytes) {
                            val batch = if (buffered == aggregate.size) aggregate else aggregate.copyOf(buffered)
                            phase="durable_append"
                            try { session.append(batch) } finally { batch.fill(0) }
                            buffered = 0
                            current()
                            // Report only durable offsets, not uncommitted transport replies.
                            onProgress(session.offset * 100 / expected.totalBytes)
                            onCheckpoint(session.offset.toLong())
                        }
                    }
                } finally { aggregate.fill(0) }
                current()
                phase="final_digest_and_header"
                session.verifyReady()
                current()
                phase="guarded_publication"
                val published = coordinator.publishDownloadedSegment(ticket, expected.segment.sha256, expected.segment.byteCount) {
                    current()
                    session.publish()
                    // A cancellation/deadline can arrive during publication/fsync.
                    // Retain and reconcile any published ciphertext, but do not
                    // commit receipt-eligible metadata for that late operation.
                    current()
                }
                if (!published) throw CancellationException("Segment download cancelled")
                SegmentDownloadResult(expected.segment, resumed, reused)
            }
        } catch (cancelled: CancellationException) {
            cancelTicket(coordinator, ticket)
            throw cancelled
        } catch (failure: Exception) {
            try { diagnostic?.invoke(phase,failure.javaClass.simpleName.take(80),
                failure.stackTrace.firstOrNull { it.className.startsWith("org.openpendant.app.") }?.lineNumber ?: -1)
            } catch (_:Throwable) { /* Synthetic diagnostics cannot skip cancellation. */ }
            cancelTicket(coordinator, ticket)
            throw SegmentDownloadException()
        }
    }

    private fun cancelTicket(coordinator: RecordingSyncContract, ticket: RecordingWorkTicket) {
        // Publication/commit ambiguity already fences the coordinator. Do not
        // attempt another commit or un-fence it in that case.
        if (!coordinator.requiresReconciliation() && coordinator.isCurrent(ticket)) coordinator.cancelWork(ticket)
    }

    companion object {
        fun forSyntheticTests(store:DurableSegmentStore,diagnostic:(String,String,Int)->Unit):SegmentDownloadEngine =
            SegmentDownloadEngine(store,{System.nanoTime()/1_000_000},diagnostic)
        const val MAX_JOB_MILLIS = 120_000L
        // Every accepted reply advances by >=1 byte; this admits even one-byte
        // fragmentation of the largest fixed container without an unbounded loop.
        const val MAX_READS = 128 + 65 + 65536 + 16
    }
}
