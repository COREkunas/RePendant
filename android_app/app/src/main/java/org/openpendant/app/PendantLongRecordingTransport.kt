package org.openpendant.app

import android.os.Handler
import android.os.Looper

internal class PendantLongRecordingTransport(client: PendantClient, peer: LongRecordingPeer) : LongRecordingControlTransport {
    private val main=Handler(Looper.getMainLooper())
    private val delegate=LongRecordingCallbackTransport(peer,object: LongRecordingCallbackChannel {
        override fun acquire(peer: LongRecordingPeer) = client.acquireLong(peer)
        override fun current(peer: LongRecordingPeer) = client.isCurrentRadioEpoch(peer.epoch)
        override fun onCallbackThread() = Looper.myLooper()===main.looper
        override fun post(action: () -> Unit) = main.post { action() }
        override fun close(lease: DurableTransportLease) = client.closeLong(lease)
        override fun submit(lease: DurableTransportLease,ticket: DurableTransportRequest,
            request: LongRecordingControlCodec.Request,deadline: Long,cancelled: () -> Boolean,
            success: (ByteArray)->Unit,failure: ()->Unit) =
            client.exchangeLong(lease,ticket,request,deadline,cancelled,success,failure)
    })
    override fun peer() = delegate.peer()
    override fun acquire(peer: LongRecordingPeer) = delegate.acquire(peer)
    override fun current(peer: LongRecordingPeer) = delegate.current(peer)
    override fun exchange(lease: DurableTransportLease,ticket: DurableTransportRequest,
        request: LongRecordingControlCodec.Request,deadlineMillis: Long,cancelled: () -> Boolean) =
        delegate.exchange(lease,ticket,request,deadlineMillis,cancelled)
    override fun retire(lease: DurableTransportLease) = delegate.retire(lease)
}
