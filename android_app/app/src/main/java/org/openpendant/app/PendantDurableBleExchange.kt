package org.openpendant.app

import android.os.Handler
import android.os.Looper
import java.util.UUID

/** Production GATT adapter, constructed only for a user-requested worker sync
 * with independently enrolled full volume/key binding. No startup hook,
 * enrollment, automatic connection or capability assumptions are installed.
 * All terminal paths close exact GATT because protocol v1 has no snapshot END.
 */
internal class PendantDurableBleExchange(client: PendantClient) : DurableBleExchange {
    private val main = Handler(Looper.getMainLooper())
    private val delegate = DurableBleCallbackExchange(object : DurableBleCallbackChannel {
        override fun capabilities(epoch: UUID) = client.durableCapabilities(epoch)
        override fun acquire(epoch: UUID) = client.acquireDurableSession(epoch)
        override fun isCurrent(epoch: UUID) = client.isCurrentRadioEpoch(epoch)
        override fun onCallbackThread() = Looper.myLooper() === main.looper
        override fun post(action: () -> Unit) = main.post { action() }
        override fun close(lease: DurableTransportLease) = client.cancelDurableSession(lease)
        override fun submit(lease: DurableTransportLease, call: DurableSyncCall, request: DurableBleRequest,
            success: (ByteArray) -> Unit, failure: () -> Unit) {
            val payload = request.payload()
            try { client.exchangeBorrowed(lease, call, request.command, payload, success, failure) }
            finally { payload.fill(0) }
        }
    })
    override fun capabilities(epoch: UUID) = delegate.capabilities(epoch)
    override fun acquire(epoch: UUID) = delegate.acquire(epoch)
    override fun isCurrent(epoch: UUID) = delegate.isCurrent(epoch)
    override fun exchange(lease: DurableTransportLease, call: DurableSyncCall, request: DurableBleRequest) =
        delegate.exchange(lease, call, request)
    override fun cancel(lease: DurableTransportLease) = delegate.cancel(lease)
}
