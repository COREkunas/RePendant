package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.Collections
import java.util.UUID

enum class DurableManifestState(val flags:Int) {
    IN_PROGRESS(0), FINALIZED(1), INTERRUPTED(2)
}

/** Public timeline metadata; never plaintext audio or an authentication token. */
data class DurableManifestSegment(val identity:SegmentIdentity, val firstSample:Long,
    val nextSample:Long, val samples:Int, val gapBefore:Boolean)

class DurableManifestSnapshot internal constructor(val manifest:RecordingManifest,
    val recipientFingerprint:String, val state:DurableManifestState,
    val totalCommittedSamples:Long, segments:List<DurableManifestSegment>) {
    val segments:List<DurableManifestSegment> = Collections.unmodifiableList(ArrayList(segments))
    val profile:OpusSegment.CodecProfile get()=OpusSegment.CodecProfile.CELT_LOW_DELAY
}

/** Canonical OPNDMF1 metadata for the initial owned volume: profile2, <=32 sealed
 * segments. Not the legacy BLE protocol, NAND ownership, HPKE authentication or
 * permission to read/delete anything. SHA is whole-object structural integrity;
 * callers must obtain the expected catalog and fingerprint from independently
 * authenticated/enrolled state, never manufacture them from these input bytes.
 *
 * Integers are LE; UUIDs retain canonical network-byte order, as in ES headers.
 * Encoder input is trusted local metadata and must remain immutable during the
 * call. Parser copies its bounded input. Returned lists are immutable. No I/O,
 * key access, capture, decode, playback, publication or protocol negotiation.
 */
object DurableManifestCodec {
    const val HEADER_BYTES=128
    const val ENTRY_BYTES=64
    const val MAX_SEGMENTS=32
    const val MAX_BYTES=HEADER_BYTES+MAX_SEGMENTS*ENTRY_BYTES
    const val FULL_MAX_SEGMENTS=5120
    const val FULL_MAX_BYTES=HEADER_BYTES+FULL_MAX_SEGMENTS*ENTRY_BYTES
    const val MAX_CONTAINER_BYTES=34357
    const val MIN_CONTAINER_BYTES=425
    private val magic=byteArrayOf(79,80,78,68,77,70,49,0)
    private val fullMagic=byteArrayOf(79,80,78,68,77,70,50,0)

    fun encode(recording:DurableRecordingId, recipientFingerprint:String,
        state:DurableManifestState, segments:List<DurableManifestSegment>, fullProfile:Boolean=false):ByteArray {
        val maximum=if(fullProfile)FULL_MAX_SEGMENTS else MAX_SEGMENTS
        require(segments.size<=maximum) { "Durable manifest segment limit exceeded" }
        val entries=ArrayList(segments)
        val total=validate(recording,recipientFingerprint,entries,maximum)
        return ByteBuffer.allocate(HEADER_BYTES+entries.size*ENTRY_BYTES).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(if(fullProfile)fullMagic else magic);putShort(if(fullProfile)2 else 1);putShort(HEADER_BYTES.toShort());putShort(ENTRY_BYTES.toShort());putShort(state.flags.toShort())
            putUuid(recording.volume.deviceId);putUuid(recording.volume.volumeId)
            putLong(recording.volume.generation);putUuid(recording.recordingId)
            putLong(revision(entries.size,state));put(hexBytes(recipientFingerprint))
            putInt(entries.size);putInt(2);putLong(total)
            check(position()==HEADER_BYTES)
            for(entry in entries) {
                putInt(entry.identity.sequence);putInt(entry.identity.byteCount.toInt())
                putLong(entry.firstSample);putLong(entry.nextSample);putInt(entry.samples)
                putInt(if(entry.gapBefore)1 else 0);put(hexBytes(entry.identity.sha256))
            }
            check(position()==capacity())
        }.array()
    }

    fun parse(bytes:ByteArray, expected:DurableCatalogEntry,
        trustedRecipientFingerprint:String, fullProfile:Boolean=false):DurableManifestSnapshot {
        // Profile is chosen by the authenticated transport, never by incoming
        // bytes. Old transport callers remain strictly v1/32-segment bounded.
        val maximum=if(fullProfile)FULL_MAX_SEGMENTS else MAX_SEGMENTS
        val expectedMagic=if(fullProfile)fullMagic else magic
        require(bytes.size in HEADER_BYTES..(HEADER_BYTES+maximum*ENTRY_BYTES)) { "Invalid durable manifest size" }
        require(validFingerprint(trustedRecipientFingerprint) && expected.manifestRevision>=0 &&
            isContentDigest(expected.manifestSha256) && expected.sealedSegments in 0..maximum) { "Invalid trusted manifest expectation" }
        val stable=bytes.copyOf()
        val input=ByteBuffer.wrap(stable).order(ByteOrder.LITTLE_ENDIAN)
        require(expectedMagic.indices.all { stable[it]==expectedMagic[it] } && u16(input,8)==(if(fullProfile)2 else 1) &&
            u16(input,10)==HEADER_BYTES && u16(input,12)==ENTRY_BYTES) { "Invalid durable manifest framing" }
        val state=DurableManifestState.entries.singleOrNull { it.flags==u16(input,14) }
            ?: throw IllegalArgumentException("Invalid durable manifest state")
        val device=uuid(stable,16);val volume=uuid(stable,32)
        val generation=input.getLong(48)
        val recording=DurableRecordingId(RecordingVolume(device,volume,generation),uuid(stable,56))
        val revision=input.getLong(72)
        val fingerprint=hex(stable,80,32)
        val count=input.getInt(112)
        val total=input.getLong(120)
        require(count in 0..maximum && stable.size==HEADER_BYTES+count*ENTRY_BYTES &&
            input.getInt(116)==2 && total>=0 && revision==revision(count,state)) { "Invalid durable manifest bounds" }
        require(recording==expected.recording && fingerprint==trustedRecipientFingerprint &&
            count==expected.sealedSegments && revision==expected.manifestRevision &&
            (state!=DurableManifestState.IN_PROGRESS)==expected.finished) { "Durable manifest catalog binding differs" }
        val entries=ArrayList<DurableManifestSegment>(count)
        repeat(count) { index ->
            val at=HEADER_BYTES+index*ENTRY_BYTES
            val sequence=input.getInt(at)
            val containerBytes=input.getInt(at+4)
            val first=input.getLong(at+8);val next=input.getLong(at+16)
            val samples=input.getInt(at+24);val gap=input.getInt(at+28)
            require(sequence==index && gap in 0..1) { "Invalid durable manifest entry" }
            val identity=SegmentIdentity(recording,sequence,hex(stable,at+32,32),containerBytes.toLong())
            entries.add(DurableManifestSegment(identity,first,next,samples,gap==1))
        }
        require(validate(recording,fingerprint,entries,maximum)==total) { "Durable manifest sample total differs" }
        val digest=hex(MessageDigest.getInstance("SHA-256").digest(stable),0,32)
        require(digest==expected.manifestSha256) { "Durable manifest digest differs" }
        val manifest=RecordingManifest(recording,revision,state!=DurableManifestState.IN_PROGRESS,digest,entries.map { it.identity })
        return DurableManifestSnapshot(manifest,fingerprint,state,total,entries)
    }

    private fun validate(recording:DurableRecordingId,fingerprint:String,entries:List<DurableManifestSegment>,maximum:Int):Long {
        require(validFingerprint(fingerprint) && entries.size<=maximum) { "Invalid durable manifest input" }
        var total=0L
        var previous:DurableManifestSegment?=null
        entries.forEachIndexed { index,entry ->
            require(entry.identity.recording==recording && entry.identity.sequence==index &&
                validFingerprint(entry.identity.sha256) &&
                entry.samples in 320..160000 && entry.samples%320==0 && entry.firstSample>=0 &&
                entry.nextSample>=entry.firstSample && entry.nextSample-entry.firstSample==entry.samples.toLong()) { "Invalid durable manifest segment timeline" }
            val size=80L+(entry.samples/320+1)*68L+209L
            require(size in MIN_CONTAINER_BYTES..MAX_CONTAINER_BYTES && entry.identity.byteCount==size) { "Invalid durable manifest segment size" }
            previous?.let { prior ->
                require(if(entry.gapBefore)entry.firstSample>prior.nextSample else entry.firstSample==prior.nextSample) { "Invalid durable manifest segment continuity" }
            }
            // GAP on the first segment is legitimate pause-before-first-frame.
            require(total<=Long.MAX_VALUE-entry.samples) { "Durable manifest sample count overflows" }
            total+=entry.samples;previous=entry
        }
        return total
    }

    private fun revision(count:Int,state:DurableManifestState)=count*4L+state.flags
    private fun validFingerprint(value:String)=isContentDigest(value) && value!="00".repeat(32) && value!="ff".repeat(32)
    private fun u16(input:ByteBuffer,at:Int)=input.getShort(at).toInt() and 65535
    private fun hexBytes(value:String)=ByteArray(value.length/2) { value.substring(it*2,it*2+2).toInt(16).toByte() }
    private fun hex(bytes:ByteArray,at:Int,count:Int)=buildString(count*2) {
        repeat(count) { append("0123456789abcdef"[(bytes[at+it].toInt() and 255) ushr 4]);append("0123456789abcdef"[bytes[at+it].toInt() and 15]) }
    }
    private fun ByteBuffer.putUuid(value:UUID) {
        for(shift in 56 downTo 0 step 8)put((value.mostSignificantBits ushr shift).toByte())
        for(shift in 56 downTo 0 step 8)put((value.leastSignificantBits ushr shift).toByte())
    }
    private fun uuid(bytes:ByteArray,at:Int):UUID {
        val value=ByteBuffer.wrap(bytes,at,16).order(ByteOrder.BIG_ENDIAN)
        return UUID(value.long,value.long)
    }
}
