package org.openpendant.app

/** Main-thread evidence only. Closing Android GATT is not proof that the
 * remote catalog retired, particularly when Android retains its physical link.
 * Never replays storage work; the next connection only toggles its subscription.
 */
internal class StorageSubscriptionReset {
    private val pending=mutableSetOf<String>()
    fun touched(address:String){pending.add(address)}
    fun required(address:String?)=address in pending
    fun observed(address:String?,retired:Boolean){if(retired)pending.remove(address)}
    fun observed(address:String?,value:DeviceTelemetry,bits:Long) {
        // Active recording, unknown/low power and faults are not retirement
        // proof. They remain ordinary dashboard states, not protocol errors.
        val r=value.recorder
        observed(address,r!=null&&!value.microphonePower&&value.faults==0L&&
            !r.fault&&!r.recording&&r.ready&&r.mounted&&!r.busy&&!value.resourceBusy&&
            r.flags and 8!=0&&StorageSyncPower.powerRefusal(TimedDeviceTelemetry(value,0),true,0,bits)==null)
    }
}
