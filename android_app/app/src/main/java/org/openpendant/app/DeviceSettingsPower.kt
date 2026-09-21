package org.openpendant.app

/** UI advice only. Firmware checks again under exclusive ownership before NVS.
 * Battery settings is qualified separately from firmware56's battery transfer. */
internal object DeviceSettingsPower {
    fun batterySupported(sample:TimedDeviceTelemetry?,bits:Long) =
        sample?.value?.firmware in setOf("0.4.57", "0.4.58", "0.4.59", "0.4.60", "0.4.61", "0.4.62") && StorageSyncPower.supported(bits)
    fun refusal(sample:TimedDeviceTelemetry?,connected:Boolean,now:Long,bits:Long):String? {
        if(!connected)return "Connect securely to save settings."
        if(sample==null||!sample.fresh(now,true))return "Waiting for fresh pendant status before saving."
        val status=sample.value
        val recorder=status.recorder?:return "Pendant power status is unavailable."
        if(status.microphonePower||recorder.recording)return "Stop and save the recording before changing settings."
        if(status.faults!=0L||recorder.fault)return "Pendant needs attention before saving settings."
        if(status.resourceBusy||recorder.busy||!recorder.ready)return "Wait for the current pendant operation to finish."
        if(recorder.usbPowered)return null
        if(!batterySupported(sample,bits))return "Connect pendant USB to save settings on this firmware."
        val b=sample.freshBattery(now,true)?:return "Waiting for a fresh pendant battery reading."
        if(!b.portable||b.stopped||!b.startPowerReady||b.percent !in 25..100||
            b.millivolts !in 3800..4450||b.temperatureDecikelvin !in 2781..3131)
            return "Charge the pendant or connect USB. Saving settings needs at least 25% and a safe temperature."
        return null
    }
}
