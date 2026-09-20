package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

internal object LongControlTestData {
    val binding=DurablePublicBinding("12:34:56:78:9A:BC",RecordingVolume(UUID(1,2),UUID(3,4),7),"12".repeat(32))
    val fixtures=checkNotNull(javaClass.classLoader?.getResourceAsStream("long_recording_control_public.txt")).bufferedReader().useLines { lines ->
        lines.filter { !it.startsWith("#") && it.isNotBlank() }.map { it.split('|') }.associate { it[0] to (hex(it[1]) to hex(it[2])) }
    }
    val boot=UUID.fromString("01020304-0506-0708-090a-0b0c0d0e0f10")
    val operation=UUID.fromString("11121314-1516-1718-191a-1b1c1d1e1f20")
    fun hex(value: String)=value.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    fun request(name: String)=LongRecordingControlCodec.parseRequest(fixtures.getValue(name).first,binding)
    fun state(name: String)=LongRecordingControlCodec.parse(fixtures.getValue(name).second,request(name))
    fun reply(name: String,request: LongRecordingControlCodec.Request): ByteArray = fixtures.getValue(name).second.copyOf().also {
        it[3]=(request.command or 128).toByte();ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putShort(4,request.sequence.toShort())
        request.boot?.let { value -> LongRecordingControlCodec.uuid(value).copyInto(it,9) }
        if(name !in setOf("idle","bootstrap"))request.operation?.let { value -> LongRecordingControlCodec.uuid(value).copyInto(it,25) }
    }
}

class LongRecordingControlCodecTest {
    private val data=LongControlTestData
    @Test fun actualCThirteenWireFixturesMatchKotlinAndFullBindingDigest() {
        assertEquals(13,data.fixtures.size)
        assertArrayEquals(data.hex("8d4d879e53d41fb231737584c9e0199791c8c253d16fbf7638702afe28bc753f"),LongRecordingControlCodec.bindingDigest(data.binding))
        data.fixtures.forEach { (_,wire) ->
            val r=LongRecordingControlCodec.parseRequest(wire.first,data.binding)
            assertArrayEquals(wire.first,LongRecordingControlCodec.request(r.command,r.sequence,r.boot,r.operation,data.binding).frame())
            LongRecordingControlCodec.validate(LongRecordingControlCodec.parse(wire.second,r))
        }
    }
    @Test fun strictHeaderReservedExtentBindingAndUnsignedBounds() {
        val pair=data.fixtures.getValue("running");val request=data.request("running")
        for(offset in listOf(0,1,2,3,4,6,7,8,9,25,57,58,59,64,68,72,76,77,78,79,80)) {
            val changed=pair.second.copyOf();changed[offset]=(changed[offset].toInt() xor 255).toByte()
            assertThrows("reply byte $offset",IllegalArgumentException::class.java){LongRecordingControlCodec.parse(changed,request)}
        }
        for(size in listOf(0,79,80,82,1000))assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.parse(pair.second.copyOf(size),request)}
        for(offset in listOf(0,1,2,3,6,7,40,72,73,79)) {
            val changed=pair.first.copyOf();changed[offset]=(changed[offset].toInt() xor 255).toByte()
            assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.parseRequest(changed,data.binding)}
        }
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.parseRequest(pair.first,data.binding.copy(recipientFingerprint="13".repeat(32)))}
    }
    @Test fun stickyCreatedIdentityCountsCapacityReasonAndTerminalState() {
        val path=listOf("starting","running","stopping","draining","stopped").map(data::state)
        path.zipWithNext().forEach { (a,b)->LongRecordingControlCodec.progress(a,b) }
        val running=data.state("running");val stopped=data.state("stopped")
        for(changed in listOf(running.copy(accepted=498),running.copy(committed=500),running.copy(epoch=2),running.copy(admitted=11500),running.copy(recording=UUID(8,9))))
            assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.progress(running,changed)}
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.progress(stopped,running)}
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.progress(data.state("stopping"),data.state("full"))}
        LongRecordingControlCodec.progress(stopped,stopped.copy(flags=stopped.flags xor 1))
        LongRecordingControlCodec.progress(data.state("reserved"),data.state("empty_reserved_finalized"))
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.progress(data.state("reserved"),data.state("cancelled"))}
    }
    @Test fun stopCannotClaimRunningOrBootstrapAndRequestsAreCopied() {
        val stop=LongRecordingControlCodec.request(0x42,7,data.boot,data.operation,data.binding)
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.parse(data.reply("running",stop),stop)}
        for(command in listOf(0x41,0x42))assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.request(command,1,null,null,data.binding)}
        val frame=stop.frame();frame.fill(0);assertEquals(80,stop.frame().size);assertEquals(79,stop.frame()[0].toInt())
    }
}
