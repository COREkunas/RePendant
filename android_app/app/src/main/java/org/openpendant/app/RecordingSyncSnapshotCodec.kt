package org.openpendant.app

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.security.MessageDigest
import java.util.UUID

/** Bounded canonical metadata, not recording plaintext. Checksum detects disk
 * corruption; authentication/authorization must come from the catalog session. */
object RecordingSyncSnapshotCodec {
    const val MAX_BYTES = 262144
    private val magic = "OPNDSY1\u0000".toByteArray(Charsets.US_ASCII)
    private val wideMagic = "OPNDSY2\u0000".toByteArray(Charsets.US_ASCII)

    fun encode(snapshot: RecordingSyncSnapshot): ByteArray {
        val validated = RecordingSyncContract(snapshot, RecordingSyncOwnership()) { _,_ -> error("No writes in validation") }
        val value = try { validated.snapshot() } finally { validated.close() }
        val wide = (value.manifest?.segments?.size ?: 0) > 4096
        val bytes = ByteArrayOutputStream()
        DataOutputStream(bytes).use { out ->
            out.write(if(wide) wideMagic else magic); out.identity(value.recording)
            out.writeLong(value.revision); out.writeLong(value.workGeneration)
            out.writeBoolean(value.manifest != null)
            value.manifest?.let { manifest ->
                out.writeLong(manifest.revision); out.writeBoolean(manifest.finished); out.write(hexBytes(manifest.sha256))
                out.writeInt(manifest.segments.size)
                manifest.segments.forEach { segment ->
                    require(segment.byteCount in 210..65745) { "Unsupported encrypted segment length" }
                    out.writeInt(segment.sequence); out.write(hexBytes(segment.sha256)); out.writeLong(segment.byteCount)
                }
            }
            out.writeByte(value.pendantCopy.ordinal); out.writeBoolean(value.downloadSuppressed); out.writeBoolean(value.staleVolume)
            for (set in listOf(value.phoneSegments,value.pendingReceipts)) {
                out.writeInt(set.size)
                if(wide) {
                    val bitmap=ByteArray((value.manifest!!.segments.size+7)/8)
                    set.forEach { bitmap[it.sequence/8]=(bitmap[it.sequence/8].toInt() or (1 shl (it.sequence%8))).toByte() }
                    out.write(bitmap)
                } else set.sortedBy { it.sequence }.forEach { out.writeInt(it.sequence) }
            }
            out.writeShort(value.deletions.size)
            value.deletions.forEach { deletion ->
                out.uuid(deletion.operationId); out.write(hexBytes(deletion.manifestSha256))
                out.writeByte(deletion.location.ordinal); out.writeBoolean(deletion.keepTranscript)
                out.writeBoolean(deletion.phonePending); out.writeByte(deletion.pendant.ordinal)
                out.writeLong(deletion.tombstoneRevision ?: -1)
            }
        }
        val body = bytes.toByteArray()
        require(body.size <= MAX_BYTES - 32) { "Recording metadata exceeds limit" }
        return body + MessageDigest.getInstance("SHA-256").digest(body)
    }

    fun decode(encoded: ByteArray): RecordingSyncSnapshot {
        require(encoded.size in 126..MAX_BYTES) { "Invalid recording metadata length" }
        val body = encoded.copyOfRange(0,encoded.size-32)
        require(MessageDigest.isEqual(MessageDigest.getInstance("SHA-256").digest(body),encoded.copyOfRange(body.size,encoded.size))) {
            "Recording metadata checksum differs"
        }
        try {
            val input = DataInputStream(ByteArrayInputStream(body))
            val tag=input.bytes(8)
            val wide=tag.contentEquals(wideMagic)
            require(wide || tag.contentEquals(magic)) { "Unsupported recording metadata" }
            val recording = input.identity(); val revision = input.readLong(); val generation = input.readLong()
            val manifest = if(input.boolean()) {
                val manifestRevision=input.readLong(); val finished=input.boolean(); val hash=input.hash()
                val count=input.readInt(); require(count in 0..RecordingManifest.MAX_SEGMENTS)
                RecordingManifest(recording,manifestRevision,finished,hash,List(count) {
                    SegmentIdentity(recording,input.readInt(),input.hash(),input.readLong())
                })
            } else null
            val pendant=PendantCopy.entries.getOrNull(input.readUnsignedByte()) ?: error("Invalid copy state")
            val suppressed=input.boolean(); val stale=input.boolean()
            fun readSet(): Set<SegmentIdentity> {
                val count=input.readInt(); require(count in 0..(manifest?.segments?.size ?: 0))
                if(wide) {
                    val segments=checkNotNull(manifest).segments
                    require(segments.size>4096)
                    val bitmap=input.bytes((segments.size+7)/8)
                    val found=LinkedHashSet<SegmentIdentity>()
                    for(index in 0 until bitmap.size*8) if(bitmap[index/8].toInt() and (1 shl (index%8)) != 0) {
                        require(index<segments.size);found.add(segments[index])
                    }
                    require(found.size==count);return found
                }
                var previous=-1
                return List(count) {
                    val index=input.readInt(); require(index>previous); previous=index
                    manifest!!.segments.getOrNull(index) ?: error("Invalid segment reference")
                }.toSet()
            }
            val phone=readSet(); val receipts=readSet()
            val count=input.readUnsignedShort(); require(count<=RecordingSyncContract.MAX_DELETE_INTENTS)
            val deletions=List(count) {
                val operation=input.uuid(); val hash=input.hash()
                val location=DeleteLocation.entries.getOrNull(input.readUnsignedByte()) ?: error("Invalid deletion location")
                val keep=input.boolean(); val pending=input.boolean()
                val remote=PendantDeletion.entries.getOrNull(input.readUnsignedByte()) ?: error("Invalid deletion state")
                val tombstone=input.readLong(); require(tombstone>=-1)
                RecordingDeletionIntent(operation,recording,hash,location,keep,pending,remote,tombstone.takeIf { it>=0 })
            }
            require(input.available()==0) { "Trailing recording metadata" }
            val snapshot=RecordingSyncSnapshot(recording,revision,generation,manifest,pendant,suppressed,stale,phone,receipts,deletions)
            require(encode(snapshot).contentEquals(encoded)) { "Noncanonical recording metadata" }
            return snapshot
        } catch (_: Exception) { throw IllegalArgumentException("Invalid recording metadata") }
    }

    fun identityBytes(recording: DurableRecordingId): ByteArray = ByteArrayOutputStream(56).also { bytes ->
        DataOutputStream(bytes).use { it.identity(recording) }
    }.toByteArray()

    private fun DataOutputStream.uuid(value:UUID) { writeLong(value.mostSignificantBits);writeLong(value.leastSignificantBits) }
    private fun DataOutputStream.identity(value:DurableRecordingId) {
        uuid(value.volume.deviceId);uuid(value.volume.volumeId);writeLong(value.volume.generation);uuid(value.recordingId)
    }
    private fun DataInputStream.uuid()=UUID(readLong(),readLong())
    private fun DataInputStream.identity()=DurableRecordingId(RecordingVolume(uuid(),uuid(),readLong()),uuid())
    private fun DataInputStream.bytes(count:Int)=ByteArray(count).also { readFully(it) }
    private fun DataInputStream.hash()=canonicalHex(bytes(32))
    private fun DataInputStream.boolean():Boolean=when(readUnsignedByte()) { 0->false;1->true;else->error("Invalid Boolean") }
}
