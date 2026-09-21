package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class DeviceSettingsPowerTest {
    private val r=RecorderTelemetry(2+4+8+16+512,5,1,0,6,32,56,5120,0,0)
    private val b=BatteryTelemetry(7,70,4100,2950,1000,5,8)
    private val value=DeviceTelemetry(1000,"0.4.57",false,false,0,0,0,0,r,b)
    private fun refusal(v:DeviceTelemetry=value,at:Long=1000,now:Long=1001,bits:Long=32479)=
        DeviceSettingsPower.refusal(TimedDeviceTelemetry(v,at),true,now,bits)
    @Test fun batterySettingsAndCapacityDoNotNeedUsb() {
        assertNull(refusal());assertNull(StorageSyncPower.refusal(TimedDeviceTelemetry(value,1000),true,1001,32479))
        assertEquals(178257920L,PendantStoragePresentation.from(value)!!.totalBytes)
        assertNull(refusal(value.copy(firmware="0.4.60")))
        assertNull(refusal(value.copy(firmware="0.4.61")))
        assertNull(refusal(value.copy(firmware="0.4.62")))
        for(v in listOf("0.4.56","0.4.63"))assertNotNull(refusal(value.copy(firmware=v)))
        for(bits in listOf(0L,16095L,32478L,65247L))assertNotNull(refusal(bits=bits))
    }
    @Test fun lowStaleMissingOrUnsafePowerCannotSave() {
        for(bad in listOf(b.copy(percent=24),b.copy(millivolts=3799),b.copy(millivolts=4451),
            b.copy(temperatureDecikelvin=2780),b.copy(temperatureDecikelvin=3132),b.copy(ageMs=20000),
            b.copy(flags=4),b.copy(flags=12)))assertNotNull(refusal(value.copy(battery=bad)))
        assertNotNull(refusal(value.copy(battery=null)));assertNotNull(refusal(at=1001,now=1000))
        assertNotNull(refusal(now=11001));assertNull(refusal(now=11000))
        assertNotNull(DeviceSettingsPower.refusal(null,true,1001,32479))
        assertNotNull(DeviceSettingsPower.refusal(TimedDeviceTelemetry(value,1000),false,1001,32479))
    }
    @Test fun usbFallbackNeverBypassesBusyOrFaults() {
        val usb=value.copy(recorder=r.copy(flags=r.flags or 1),battery=null,firmware="0.4.56")
        assertNull(refusal(usb))
        for(base in listOf(value,usb))for(v in listOf(base.copy(microphonePower=true),base.copy(resourceBusy=true),
            base.copy(faults=1),base.copy(recorder=r.copy(flags=r.flags or 64)),
            base.copy(recorder=r.copy(flags=r.flags or 128)),base.copy(recorder=r.copy(flags=r.flags or 256)),
            base.copy(recorder=r.copy(flags=r.flags and 2.inv()))))assertNotNull(refusal(v))
    }
}
