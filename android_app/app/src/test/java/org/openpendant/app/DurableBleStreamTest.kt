package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class DurableBleStreamTest {
    private val volume = RecordingVolume(UUID(1,2),UUID(3,4),1)
    private val segment = SegmentIdentity(DurableRecordingId(volume,UUID(5,6)),0,"12".repeat(32),34357)
    private fun request(offset: Int = 0, maximum: Int = 4096) = DurableBleCodec.stream(UUID(7,8),segment,offset,maximum)
    private fun packet(r: DurableBleReadRequest, offset: Int, count: Int, total: Int = 34357): ByteArray =
        ByteBuffer.allocate(33+count).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(79);put(80);put(1);put(0xbd.toByte());putShort(17);putShort((25+count).toShort());put(0)
            put(r.payload(),0,16);putInt(offset);putInt(total)
            repeat(count){put(((offset+it)%251).toByte())}
        }.array()
    @Test fun everyLengthAndTailRequiresWholeRangeAndBothCallbacks() {
        for (maximum in 1..4096) for (offset in listOf(0,34158,34356)) {
            val r=request(offset,maximum);val wanted=minOf(maximum,34357-offset)
            val gate=ResponseGate(0x3d,17,DurableBleStreamAssembler(r.payload()))
            if(maximum%2==0)gate.written(true)
            var used=0
            while(used<wanted){
                assertFalse(gate.ready);val count=minOf(192,wanted-used)
                gate.notified(packet(r,offset+used,count));used+=count
            }
            if(maximum%2!=0){assertFalse(gate.ready);gate.written(true)}
            assertTrue(gate.ready);val body=gate.take()
            DurableBleCodec.readBody(body,r).use { part ->
                assertEquals(wanted,part.bytes.size)
                assertArrayEquals(ByteArray(wanted){((offset+it)%251).toByte()},part.bytes)
            }
            body.fill(0)
            assertThrows(ProtocolException::class.java){gate.notified(packet(r,offset,1))}
        }
    }
    @Test fun reorderedDuplicateChangedTotalNonceAndFramingFailClosed() {
        for(at in listOf(0,2,3,4,6,8,9,25,29)){
            val r=request();val gate=ResponseGate(0x3d,17,DurableBleStreamAssembler(r.payload()))
            val p=packet(r,0,192);p[at]=(p[at].toInt() xor 1).toByte()
            assertThrows(Exception::class.java){gate.notified(p)};gate.discard();assertFalse(gate.ready)
        }
        for(mode in 0..3){
            val r=request();val gate=ResponseGate(0x3d,17,DurableBleStreamAssembler(r.payload()))
            gate.written(true);gate.notified(packet(r,0,192))
            val next=when(mode){0->packet(r,0,192);1->packet(r,384,192);2->packet(r,192,191);else->packet(r,192,192,34289)}
            assertThrows(IllegalArgumentException::class.java){gate.notified(next)};gate.discard()
        }
    }
    @Test fun capabilitiesAndAdmissionAreExplicit() {
        for(bits in listOf(159L,671L,1695L))for(mtu in listOf(96,227,228,247))
            assertEquals(bits==1695L&&mtu>=228,DurableBleCodec.streamEnabled(bits,mtu))
        for(bits in listOf(1679L,1695L)){
            val info=byteArrayOf(0,1,0,0,0,0,0,0);OpProtocol.put32(info,2,bits)
            assertEquals(bits,DurableBleCodec.validatedCapabilities(info))
        }
        for(maximum in listOf(0,4097,65535)) assertThrows(IllegalArgumentException::class.java){request(0,maximum)}
        assertThrows(IllegalArgumentException::class.java){ResponseGate(0x3d,17)}
        val r=request();val encoded=DurableBleCodec.encodeRequest(0x3d,17,r.payload())
        assertEquals(82,encoded.size);assertEquals(4096,OpProtocol.u16(encoded,48))
    }
    @Test fun truncatedStreamNeverCompletesAndDiscardRejectsLateBytes() {
        val r=request();val gate=ResponseGate(0x3d,17,DurableBleStreamAssembler(r.payload()))
        gate.written(true);gate.notified(packet(r,0,192));assertFalse(gate.ready)
        assertThrows(ProtocolException::class.java){gate.take()}
        gate.discard();assertThrows(ProtocolException::class.java){gate.notified(packet(r,192,192))}
    }
}
