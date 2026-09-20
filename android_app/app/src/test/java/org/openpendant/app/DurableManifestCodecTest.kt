package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

/** Fixed public metadata only. No ciphertext/audio files, keys, transport or
 * Android state. Resource was independently generated with Python struct/UUID
 * network bytes/hashlib, not this encoder or the firmware encoder. */
class DurableManifestCodecTest {
    private val recording=DurableRecordingId(RecordingVolume(
        UUID.fromString("11121314-1516-1718-191a-1b1c1d1e1f20"),
        UUID.fromString("21222324-2526-2728-292a-2b2c2d2e2f30"),0x0102030405060708L),
        UUID.fromString("31323334-3536-3738-393a-3b3c3d3e3f40"))
    private val fingerprint="4142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60"
    private val hashes=listOf(
        "0bad6f43e93a6759a48b3ed1f1b263938435dbccfb5d2e924f3101edbb2c6336",
        "752e2d463641c5df3b3f6e0c6ccc3712de32271bcab3a72d74b767bc82cbf980",
        "22c91a0f9e6199e80269c9243f0fcfa21f7bfcf68155d27e45d96bd35af0119c")
    private val sampleCounts=listOf(320,160000,640)
    private val firstSamples=listOf(72623859790381056L,72623859790381376L,72623859790542016L)
    private fun entries()=List(3) { i -> DurableManifestSegment(
        SegmentIdentity(recording,i,hashes[i],listOf(425L,34357L,493L)[i]),firstSamples[i],
        firstSamples[i]+sampleCounts[i],sampleCounts[i],i==2) }
    private val pinned=listOf(
        "6727e7a57e6ffc3ef69ebffe751e2b133128ee326c3b03d0446aadf47bc545f1",
        "78b67f5fe13886b05d3425b73af30ccaaa9ceda2812320625ac6ae6b49131779",
        "821bf6c2dea3bec0e99430f42f04e8e2362f96545b13772909d098eda8d05d4e")
    private fun sha(bytes:ByteArray)=MessageDigest.getInstance("SHA-256").digest(bytes).joinToString(""){"%02x".format(it.toInt() and 255)}
    private fun fixture(state:DurableManifestState=DurableManifestState.FINALIZED):ByteArray {
        val json=javaClass.getResourceAsStream("/durable_manifest_v1_public.json")!!.use { it.readBytes().toString(Charsets.US_ASCII) }
        val hex=Regex("\"state\": \"${state.name.lowercase()}\"[\\s\\S]*?\"hex\": \"([0-9a-f]+)\"").find(json)!!.groupValues[1]
        return ByteArray(hex.length/2){hex.substring(it*2,it*2+2).toInt(16).toByte()}.also {
            assertEquals(320,it.size);assertEquals(pinned[state.flags],sha(it))
        }
    }
    private fun expectation(bytes:ByteArray,state:DurableManifestState=DurableManifestState.FINALIZED,count:Int=3,id:DurableRecordingId=recording)=
        DurableCatalogEntry(id,count*4L+state.flags,sha(bytes),count,state!=DurableManifestState.IN_PROGRESS)
    private fun put16(bytes:ByteArray,at:Int,value:Int) { ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putShort(at,value.toShort()) }
    private fun put32(bytes:ByteArray,at:Int,value:Int) { ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(at,value) }
    private fun put64(bytes:ByteArray,at:Int,value:Long) { ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putLong(at,value) }
    /** Recompute the expected digest so structural failures cannot pass merely
     * because the original fixture hash no longer matches. Full trusted identity,
     * manifest revision/count/finality and recipient remain independently pinned. */
    private fun rejects(change:(ByteArray)->Unit) {
        val bytes=fixture();change(bytes)
        assertThrows(IllegalArgumentException::class.java){DurableManifestCodec.parse(bytes,expectation(bytes),fingerprint)}
    }

    @Test fun allIndependentFixturesMatchEncoderBytesHashStateTimelineAndFullIdentity() {
        for(state in DurableManifestState.entries) {
            val public=fixture(state)
            val encoded=DurableManifestCodec.encode(recording,fingerprint,state,entries())
            assertArrayEquals(public,encoded);assertEquals(pinned[state.flags],sha(encoded))
            val decoded=DurableManifestCodec.parse(public,expectation(public,state),fingerprint)
            assertEquals(recording,decoded.manifest.recording);assertEquals(12L+state.flags,decoded.manifest.revision)
            assertEquals(state,decoded.state);assertEquals(state!=DurableManifestState.IN_PROGRESS,decoded.manifest.finished)
            assertEquals(fingerprint,decoded.recipientFingerprint);assertEquals(160960L,decoded.totalCommittedSamples)
            assertEquals(entries(),decoded.segments);assertEquals(entries().map { it.identity },decoded.manifest.segments)
            assertEquals(OpusSegment.CodecProfile.CELT_LOW_DELAY,decoded.profile)
            assertEquals(0x0102030405060708L,decoded.manifest.recording.volume.generation)
        }
    }

    @Test fun everySingleBitMutationFailsAgainstOriginalTrustedCatalog() {
        val original=fixture();val expected=expectation(original)
        for(index in original.indices)for(bit in 0..7) {
            val changed=original.copyOf();changed[index]=(changed[index].toInt() xor (1 shl bit)).toByte()
            assertThrows(IllegalArgumentException::class.java){DurableManifestCodec.parse(changed,expected,fingerprint)}
        }
    }

    @Test fun exactLengthNoTrailingFooterNoTruncationAndBoundedAllocation() {
        val original=fixture();val expected=expectation(original)
        for(size in 0 until original.size)assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.parse(original.copyOf(size),expected,fingerprint)
        }
        for(size in listOf(321,352,2176,2177,100000))assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.parse(original.copyOf(size),expected,fingerprint)
        }
        val tooMany=object:AbstractList<DurableManifestSegment>() {
            override val size=33
            override fun get(index:Int):DurableManifestSegment=throw AssertionError("Must reject before copying")
        }
        assertThrows(IllegalArgumentException::class.java) { DurableManifestCodec.encode(recording,fingerprint,DurableManifestState.IN_PROGRESS,tooMany) }
    }

    @Test fun exactMagicVersionHeaderEntryAndFlagsAreRequiredEvenWithNewDigest() {
        rejects { it[0]=0 }
        for(at in listOf(8,10,12))for(value in listOf(0,2,64,128,256,65535)) {
            if((at==10 && value==128)||(at==12 && value==64))continue
            rejects { put16(it,at,value) }
        }
        for(value in listOf(3,4,8,255,256,65535))rejects { put16(it,14,value) }
        rejects { it[7]=1 }
    }

    @Test fun countProfileRevisionAndTotalCannotBeForged() {
        for(value in listOf(-1,0,2,4,32,33,Int.MAX_VALUE))rejects { put32(it,112,value) }
        for(value in listOf(-1,0,1,3,Int.MAX_VALUE))rejects { put32(it,116,value) }
        for(value in listOf(-1L,0L,12L,14L,Long.MAX_VALUE,Long.MIN_VALUE))rejects { put64(it,72,value) }
        for(value in listOf(-1L,0L,160959L,160961L,Long.MAX_VALUE,Long.MIN_VALUE))rejects { put64(it,120,value) }
    }

    @Test fun uuidSentinelsUnsignedGenerationAndFullBindingAreRejected() {
        for(at in listOf(16,32,56)) {
            for(value in listOf(0.toByte(),0xff.toByte()))rejects { it.fill(value,at,at+16) }
            rejects { it[at]=(it[at].toInt() xor 1).toByte() }
        }
        for(value in listOf(0L,-1L,Long.MIN_VALUE))rejects { put64(it,48,value) }
        rejects { put64(it,48,recording.volume.generation+1) }
        val bytes=fixture();val expected=expectation(bytes)
        val other=recording.copy(recordingId=UUID(1,2))
        assertThrows(IllegalArgumentException::class.java){DurableManifestCodec.parse(bytes,expected.copy(recording=other),fingerprint)}
    }

    @Test fun catalogDigestCountRevisionTerminalStateAndRecipientMustMatchIndependently() {
        val bytes=fixture();val expected=expectation(bytes)
        for(wrong in listOf(expected.copy(manifestRevision=12),expected.copy(manifestSha256="cd".repeat(32)),
            expected.copy(sealedSegments=2),expected.copy(finished=false))) {
            assertThrows(IllegalArgumentException::class.java){DurableManifestCodec.parse(bytes,wrong,fingerprint)}
        }
        for(wrong in listOf("00".repeat(32),"ff".repeat(32),"12".repeat(32),fingerprint.uppercase(),"12")) {
            assertThrows(IllegalArgumentException::class.java){DurableManifestCodec.parse(bytes,expected,wrong)}
        }
        rejects { it[80]=(it[80].toInt() xor 1).toByte() }
        for(value in listOf(0.toByte(),0xff.toByte()))rejects { it.fill(value,80,112) }
    }

    @Test fun segmentSequenceFrameCountsAndExactProfile2ContainerGeometryAreRequired() {
        for(value in listOf(-1,1,2,32))rejects { put32(it,128,value) }
        for(value in listOf(-1,0,1,319,321,160001,160320))rejects { put32(it,128+24,value) }
        for(value in listOf(-1,0,210,424,426,34357,34358))rejects { put32(it,128+4,value) }
        for(value in listOf(-1,2,3))rejects { put32(it,128+28,value) }
        // All-zero/all-FF SHA are reserved invalid sentinels on both platforms.
        for(value in listOf(0.toByte(),0xff.toByte()))rejects { it.fill(value,128+32,128+64) }
    }

    @Test fun signedSampleBoundsNoOverlapNoInventedGapAndExactDifference() {
        for(value in listOf(-1L,Long.MIN_VALUE,Long.MAX_VALUE)) {
            rejects { put64(it,128+8,value) };rejects { put64(it,128+16,value) }
        }
        rejects { put64(it,128+16,firstSamples[0]+319) }
        rejects { put64(it,128+16,firstSamples[0]-1) }
        rejects { put32(it,192+28,1) } // A zero-length gap is not a gap.
        rejects { put32(it,256+28,0) } // A real gap cannot be silently concatenated.
        for(delta in listOf(-320L,320L))rejects {
            put64(it,192+8,firstSamples[1]+delta);put64(it,192+16,firstSamples[1]+delta+160000)
        }
        rejects {
            val priorEnd=firstSamples[1]+160000
            put64(it,256+8,priorEnd);put64(it,256+16,priorEnd+640)
        }
    }

    @Test fun firstGapIsAllowedForPauseBeforeFirstFrame() {
        val entries=entries().mapIndexed { i,entry -> if(i==0)entry.copy(gapBefore=true) else entry }
        val encoded=DurableManifestCodec.encode(recording,fingerprint,DurableManifestState.FINALIZED,entries)
        val decoded=DurableManifestCodec.parse(encoded,expectation(encoded),fingerprint)
        assertTrue(decoded.segments.first().gapBefore)
        assertEquals(160960L,decoded.totalCommittedSamples)
    }

    @Test fun emptyManifestsAndFull32SegmentLimitRoundTrip() {
        for(state in DurableManifestState.entries) {
            val empty=DurableManifestCodec.encode(recording,fingerprint,state,emptyList())
            val decoded=DurableManifestCodec.parse(empty,expectation(empty,state,0),fingerprint)
            assertEquals(128,empty.size);assertEquals(0,decoded.segments.size);assertEquals(0L,decoded.totalCommittedSamples)
        }
        val entries=List(32) { i -> DurableManifestSegment(SegmentIdentity(recording,i,hashes[0],34357),
            i*160000L,(i+1)*160000L,160000,false) }
        val full=DurableManifestCodec.encode(recording,fingerprint,DurableManifestState.FINALIZED,entries)
        val parsed=DurableManifestCodec.parse(full,expectation(full,count=32),fingerprint)
        assertEquals(2176,full.size);assertEquals(5120000L,parsed.totalCommittedSamples)
        assertEquals(129L,parsed.manifest.revision);assertEquals(entries,parsed.segments)
    }

    @Test fun signedLongMaximumIsExactWithoutDoubleRounding() {
        val id=recording.copy(volume=recording.volume.copy(generation=Long.MAX_VALUE))
        val entries=listOf(DurableManifestSegment(SegmentIdentity(id,0,hashes[0],425),Long.MAX_VALUE-320,Long.MAX_VALUE,320,false))
        val bytes=DurableManifestCodec.encode(id,fingerprint,DurableManifestState.INTERRUPTED,entries)
        val parsed=DurableManifestCodec.parse(bytes,expectation(bytes,DurableManifestState.INTERRUPTED,1,id),fingerprint)
        assertEquals(Long.MAX_VALUE,parsed.segments.single().nextSample)
        assertEquals(Long.MAX_VALUE,parsed.manifest.recording.volume.generation)
        assertTrue(parsed.manifest.finished);assertEquals(DurableManifestState.INTERRUPTED,parsed.state)
    }

    @Test fun encoderRejectsWrongEntryIdentitySequenceSentinelsAndTimeline() {
        val first=entries().first()
        val wrong=listOf(
            first.copy(identity=first.identity.copy(recording=recording.copy(recordingId=UUID(1,2)))),
            first.copy(identity=first.identity.copy(sequence=1)),
            first.copy(identity=first.identity.copy(sha256="00".repeat(32))),
            first.copy(identity=first.identity.copy(sha256="ff".repeat(32))),
            first.copy(identity=first.identity.copy(byteCount=426)),
            first.copy(samples=0),first.copy(samples=321),first.copy(firstSample=-1),first.copy(nextSample=-1))
        wrong.forEach { entry -> assertThrows(IllegalArgumentException::class.java) {
            DurableManifestCodec.encode(recording,fingerprint,DurableManifestState.FINALIZED,listOf(entry))
        } }
    }

    @Test fun parsedMetadataDoesNotAliasMutableInputOrExposeMutableLists() {
        val bytes=fixture();val expected=expectation(bytes)
        val parsed=DurableManifestCodec.parse(bytes,expected,fingerprint)
        bytes.fill(0)
        assertEquals(entries(),parsed.segments);assertEquals(pinned[1],parsed.manifest.sha256)
        @Suppress("UNCHECKED_CAST") val list=parsed.segments as MutableList<DurableManifestSegment>
        assertThrows(UnsupportedOperationException::class.java){list.clear()}
        @Suppress("UNCHECKED_CAST") val identities=parsed.manifest.segments as MutableList<SegmentIdentity>
        assertThrows(UnsupportedOperationException::class.java){identities.clear()}
    }
}
