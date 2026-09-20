package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class LongRecordingCallbackTransportTest {
    private val data=LongControlTestData
    private val peer=LongRecordingPeer(UUID(9,10),data.binding.bondAddress,64)
    private class Channel: LongRecordingCallbackChannel {
        val gate=DurableTransportOwnership()
        var current=true;var callbackThread=false;var posts=true;var queued=false
        var closed=0;var submitted=0;var now=100L
        var pending: (() -> Unit)?=null
        var deliver: (ByteArray)->Unit = {}
        var fail: ()->Unit = {}
        var response: ByteArray?=null
        var delay=0L
        override fun acquire(peer:LongRecordingPeer)=gate.acquire(peer.epoch)
        override fun current(peer:LongRecordingPeer)=current
        override fun onCallbackThread()=callbackThread
        override fun post(action:()->Unit):Boolean {
            if(!posts)return false
            if(queued)pending=action else action()
            return true
        }
        override fun close(lease:DurableTransportLease){closed++;lease.cancelAfterTransportClosed()}
        override fun submit(lease:DurableTransportLease,ticket:DurableTransportRequest,
            request:LongRecordingControlCodec.Request,deadline:Long,cancelled:()->Boolean,
            success:(ByteArray)->Unit,failure:()->Unit){
            submitted++;deliver=success;fail=failure;now+=delay
            response?.let(success)?:failure()
        }
    }
    @Test fun copiedReplyRemainsUncompletedUntilDomainValidation() {
        val channel=Channel();val transport=LongRecordingCallbackTransport(peer,channel){channel.now}
        val lease=transport.acquire(peer);val ticket=lease.beginRequest();val request=data.request("idle")
        channel.response=data.reply("idle",request)
        val result=transport.exchange(lease,ticket,request,4100){false}
        channel.response!!.fill(0);assertEquals(LongRecordingControlCodec.Phase.IDLE,LongRecordingControlCodec.parse(result,request).phase)
        ticket.requirePending();assertTrue(ticket.completed());ticket.requireSuccessful()
        assertEquals(1,channel.submitted);assertEquals(0,channel.closed)
        transport.retire(lease);assertTrue(channel.gate.isIdle())
    }
    @Test fun failedOrMalformedReplyNeverCompletesOrRetriesStart() {
        for(bytes in listOf<ByteArray?>(null,ByteArray(80),ByteArray(82))){
            val channel=Channel();val transport=LongRecordingCallbackTransport(peer,channel){channel.now}
            val lease=transport.acquire(peer);val ticket=lease.beginRequest()
            channel.response=bytes
            assertThrows(IllegalStateException::class.java){transport.exchange(lease,ticket,data.request("starting"),4100){false}}
            assertEquals(1,channel.submitted);assertFalse(lease.isActive());assertTrue(channel.gate.isIdle())
            assertThrows(IllegalStateException::class.java){ticket.requireSuccessful()}
        }
    }
    @Test fun lateReplyCannotCompleteAndClosureDoesNotSendStop() {
        val channel=Channel();val transport=LongRecordingCallbackTransport(peer,channel){channel.now}
        val lease=transport.acquire(peer);val ticket=lease.beginRequest();val request=data.request("starting")
        channel.response=data.reply("starting",request);channel.delay=4000
        assertThrows(IllegalStateException::class.java){transport.exchange(lease,ticket,request,4100){false}}
        channel.deliver(data.reply("starting",request))
        assertEquals(1,channel.submitted);assertFalse(lease.isActive())
    }
    @Test fun cancelledOrStalePeerRejectsBeforeSubmitting() {
        for(stale in listOf(false,true)){
            val channel=Channel();val transport=LongRecordingCallbackTransport(peer,channel){channel.now}
            val lease=transport.acquire(peer);val ticket=lease.beginRequest();channel.current=!stale
            assertThrows(IllegalStateException::class.java){transport.exchange(lease,ticket,data.request("starting"),4100){!stale}}
            assertEquals(0,channel.submitted);assertFalse(lease.isActive())
        }
    }
    @Test fun postFailureRetainsGateUntilActualClosureAndCannotReacquire() {
        val channel=Channel();val transport=LongRecordingCallbackTransport(peer,channel){channel.now}
        val lease=transport.acquire(peer);val ticket=lease.beginRequest();channel.posts=false
        assertThrows(IllegalStateException::class.java){transport.exchange(lease,ticket,data.request("starting"),4100){false}}
        assertFalse(lease.isActive());assertFalse(channel.gate.isIdle());assertEquals(0,channel.submitted)
        assertThrows(DurableTransportBusyException::class.java){transport.acquire(peer)}
    }
    @Test fun callbackThreadCannotBlockWaitingForItself() {
        val channel=Channel();val transport=LongRecordingCallbackTransport(peer,channel){channel.now}
        channel.callbackThread=true;assertThrows(IllegalStateException::class.java){transport.acquire(peer)}
        assertTrue(channel.gate.isIdle());assertEquals(0,channel.submitted)
    }
}
