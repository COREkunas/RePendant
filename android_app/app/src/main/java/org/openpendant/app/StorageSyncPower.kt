package org.openpendant.app

/** Cached admission advice only; firmware still enforces power/ownership on
 * every storage operation. Never starts a status request, transfer or recording. */
internal object StorageSyncPower {
    const val USB_REQUIRED="Connect the pendant’s USB cable to sync. Battery-only transfer is not supported yet."
    fun supported(bits:Long)=bits==32479L
    fun powerRefusal(sample:TimedDeviceTelemetry?, connected:Boolean, now:Long, bits:Long=0):String? {
        if(!connected)return "Connect securely to the pendant before syncing."
        if(sample==null||!sample.fresh(now,true))return "Refresh pendant status before syncing."
        val recorder=sample.value.recorder ?: return "Pendant storage power status is unavailable."
        if(recorder.usbPowered)return null
        if(!supported(bits))return USB_REQUIRED
        val battery=sample.freshBattery(now,true) ?: return "Waiting for a fresh pendant battery reading."
        if(!battery.portable||battery.stopped||!battery.startPowerReady||
            battery.percent !in 25..100||battery.millivolts !in 3800..4450||
            battery.temperatureDecikelvin !in 2781..3131)
            return "Charge the pendant before syncing, or connect USB. Battery sync needs at least 25% and a safe temperature."
        return null
    }
    fun refusal(sample:TimedDeviceTelemetry?, connected:Boolean, now:Long, bits:Long=0):String? {
        powerRefusal(sample,connected,now,bits)?.let { return it }
        val status=requireNotNull(sample).value
        val recorder=requireNotNull(status.recorder)
        if(status.microphonePower||recorder.recording)return "Stop and save the recording before syncing."
        if(status.faults!=0L||recorder.fault)return "Pendant storage needs attention. No transfer was started."
        if(status.resourceBusy||recorder.busy||!recorder.ready)return "Wait for the pendant to finish saving, then sync."
        return null
    }
}
