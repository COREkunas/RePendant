package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class LongControlIntegrationTest {
    @Test fun explicitButtonArmIsDistinctFromImmediateStartAndCannotClaimAudio() {
        val request=LongRecordingControlCodec.request(0x41,7,LongControlTestData.boot,LongControlTestData.operation,LongControlTestData.binding,true)
        assertTrue(request.waitForButton);assertEquals(2,request.frame()[72].toInt())
        assertTrue(LongRecordingControlCodec.parseRequest(request.frame(),LongControlTestData.binding).waitForButton)
        val frame=LongControlTestData.reply("starting",request);frame[60]=4
        val armed=LongRecordingControlCodec.parse(frame,request)
        assertTrue(armed.has(LongRecordingControlCodec.BUTTON_ARMED));assertFalse(armed.has(LongRecordingControlCodec.MIC))
        assertFalse(armed.terminal)
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.validate(armed.copy(flags=armed.flags or LongRecordingControlCodec.MIC))}
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.validate(armed.copy(flags=armed.flags or LongRecordingControlCodec.STOP_REQUESTED))}
        assertThrows(IllegalArgumentException::class.java){LongRecordingControlCodec.request(0x42,7,LongControlTestData.boot,LongControlTestData.operation,LongControlTestData.binding,true)}
    }
    @Test fun fullCapacityAndStopLatchAreNotPrematureSavedProof() {
        val request=LongRecordingControlCodec.request(0x42,7,LongControlTestData.boot,LongControlTestData.operation,LongControlTestData.binding)
        val frame=LongControlTestData.reply("running",request)
        frame[60]=(frame[60].toInt() or 2).toByte()
        val state=LongRecordingControlCodec.parse(frame,request)
        assertEquals(LongRecordingControlCodec.Phase.RUNNING,state.phase)
        assertTrue(state.has(LongRecordingControlCodec.STOP_REQUESTED));assertFalse(state.terminal)
        for(slots in 1..5120) {
            val capacity=slots*500
            LongRecordingControlCodec.validate(state.copy(accepted=capacity,committed=capacity,admitted=capacity))
            assertThrows(IllegalArgumentException::class.java) {
                LongRecordingControlCodec.validate(state.copy(accepted=capacity+1,committed=capacity,admitted=capacity))
            }
        }
    }
    @Test fun reconnectMayObserveMicOnlyForExactLongControlProfile() {
        val info=byteArrayOf(0,1,0,0,0,0,1,0)
        OpProtocol.put32(info,2,3807)
        assertEquals(3807L,DurableBleCodec.validatedCapabilities(info))
        for(bits in listOf(31L,3743L,1679L)){
            OpProtocol.put32(info,2,bits)
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.validatedCapabilities(info)}
        }
        OpProtocol.put32(info,2,3807);info[6]=2
        assertThrows(IllegalArgumentException::class.java){DurableBleCodec.validatedCapabilities(info)}
    }
    @Test fun longControlUsesSharedRadioAndRequiresSemanticCompletion() {
        val radio=PendantRadioOwnership();val connection=radio.open();val source=Any()
        radio.attach(connection,source);radio.setupTransportCompleted(connection);radio.setupValidated(connection)
        val lease=radio.acquireLong(connection);val ticket=lease.beginRequest()
        assertThrows(DurableTransportBusyException::class.java){radio.acquireDurable(connection)}
        assertThrows(IllegalStateException::class.java){radio.legacy(connection,OpProtocol.BEGIN,1)}
        val exchange=radio.borrowLong(connection,lease,ticket,0x40,3)
        val request=LongRecordingControlCodec.request(0x40,3,null,null,LongControlTestData.binding)
        exchange.notified(LongControlTestData.reply("idle",request));assertFalse(exchange.ready)
        exchange.written(true);assertTrue(exchange.ready);assertEquals(72,exchange.take().size)
        ticket.requirePending();assertTrue(ticket.completed());ticket.requireSuccessful()
        assertTrue(lease.retire());assertTrue(radio.idle())
        val sync=radio.acquireDurable(connection);assertTrue(sync.retire())
    }
}
