package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class DurableFullBleCodecTest {
    private val nonce = UUID(11,12)
    private val volume = RecordingVolume(UUID(1,2),UUID(3,4),2)
    private val connection = DurableSyncConnection(UUID(5,6),volume,"12".repeat(32))
    private val recording = DurableRecordingId(volume,UUID(7,8))
    private val rows = List(5120) { i -> DurableManifestSegment(
        SegmentIdentity(recording,i,digestHex("public $i".toByteArray()),34357),i*160000L,(i+1)*160000L,160000,false) }
    private val manifest = DurableManifestCodec.encode(recording,connection.recipientFingerprint,DurableManifestState.FINALIZED,rows,true)
    private val row = DurableCatalogEntry(recording,20481,digestHex(manifest),5120,true)
    private fun body(request: DurableBleReadRequest, bytes: ByteArray): ByteArray =
        ByteBuffer.allocate(24+minOf(request.maximum,bytes.size-request.offset)).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(request.payload(),0,16);putInt(request.offset);putInt(bytes.size);put(bytes,request.offset,capacity()-24)
        }.array()
    @Test fun all6830FragmentsPreserveOffsetsBeyond65535AndCanonicalManifestDigest() {
        val first=DurableBleCodec.manifest(nonce,row,0,48,true)
        DurableBleMetadataAssembler(first).use { assembly ->
            var count=0
            while(assembly.received<manifest.size){
                val request=DurableBleCodec.manifest(nonce,row,assembly.received,48,true)
                val encoded=DurableBleCodec.encodeRequest(request.command,++count,request.payload())
                assertEquals(82,encoded.size)
                assertEquals(assembly.received,ByteBuffer.wrap(encoded).order(ByteOrder.LITTLE_ENDIAN).getInt(76))
                assembly.acceptBody(request,body(request,manifest))
            }
            assertEquals(6830,count);assertArrayEquals(manifest,assembly.finish())
        }
    }
    @Test fun strictFullCatalogProfileAnd32Roots() {
        val entries=List(32){i->row.copy(recording=DurableRecordingId(volume,UUID(7,i+1L)))}
        val bytes=DurableBleCodec.encodeCatalog(connection,nonce,1,entries,true)
        assertEquals(2176,bytes.size)
        assertEquals(entries,DurableBleCodec.parseCatalog(bytes,connection,nonce,true).entries)
        assertThrows(IllegalArgumentException::class.java){DurableBleCodec.parseCatalog(bytes,connection,nonce)}
        assertThrows(IllegalArgumentException::class.java){DurableBleCodec.manifest(nonce,row,0)}
        assertThrows(IllegalArgumentException::class.java){DurableBleCodec.catalog(nonce,0,193,true)}
    }
    @Test fun lastSegmentAckAndMalformed32BitRangesAreRejectedBeforeSubmission() {
        val read=DurableBleCodec.segment(nonce,rows.last().identity,34320,37,true)
        assertEquals(82,DurableBleCodec.encodeRequest(read.command,1,read.payload()).size)
        val ack=DurableBleCodec.receipt(nonce,rows.last().identity,true)
        assertEquals(80,DurableBleCodec.encodeRequest(ack.command,2,ack.payload()).size)
        for(offset in listOf(-1,327808,Int.MAX_VALUE)){
            val p=DurableBleCodec.manifest(nonce,row,0,48,true).payload()
            ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN).putInt(68,offset)
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.encodeRequest(0x39,1,p)}
        }
        val request=DurableBleCodec.manifest(nonce,row,65536,48,true)
        for(index in listOf(0,16,20)){
            val b=body(request,manifest);b[index]=(b[index].toInt() xor 1).toByte()
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.readBody(b,request)}
        }
    }
    @Test fun wideFragmentsNeedBothCapabilityAndActualMtu() {
        for (bits in listOf(143L,159L,655L,671L)) {
            val info = byteArrayOf(0,1,0,0,0,0,0,0); OpProtocol.put32(info,2,bits)
            assertEquals(bits,DurableBleCodec.validatedCapabilities(info))
            for (mtu in listOf(23,96,227,228,247,517))
                assertEquals(bits in listOf(655L,671L) && mtu>=228,DurableBleCodec.wideEnabled(bits,mtu))
        }
        for (bits in listOf(512L,527L,543L,575L,1023L)) {
            val info = byteArrayOf(0,1,0,0,0,0,0,0); OpProtocol.put32(info,2,bits)
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.validatedCapabilities(info)}
        }
    }
    @Test fun everyWideMaximumAndTailPreservesExactFramingAndBothCallbacks() {
        for (maximum in 1..192) for (offset in listOf(0,65536,manifest.size-1)) {
            val request=DurableBleCodec.manifest(nonce,row,offset,maximum,true)
            val b=body(request,manifest)
            val packet=ByteBuffer.allocate(9+b.size).order(ByteOrder.LITTLE_ENDIAN).apply {
                put(79);put(80);put(1);put(0xb9.toByte());putShort(1);putShort((b.size+1).toShort());put(0);put(b)
            }.array()
            val gate=ResponseGate(0x39,1);gate.notified(packet);assertFalse(gate.ready);gate.written(true)
            val received=gate.take()
            DurableBleCodec.readBody(received,request).use {
                assertArrayEquals(manifest.copyOfRange(offset,offset+minOf(maximum,manifest.size-offset)),it.bytes)
            }
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.readBody(b.copyOf(b.size+1),request)}
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.readBody(b.copyOf(b.size-1),request)}
        }
    }
    @Test fun wideBoundsDoNotWidenLegacyOrReceiptResponses() {
        assertEquals(216,DurableBleCodec.maxResponseBody(0x3a))
        for(command in listOf(1,0x12,0x20,0x32,0x33,0x3b,0x3c)) assertEquals(72,DurableBleCodec.maxResponseBody(command))
        for (maximum in listOf(0,193,255,256))
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.segment(nonce,rows[0].identity,0,maximum,true)}
    }
}
