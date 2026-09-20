package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class BatterySyncTest {
    private val battery=BatteryTelemetry(7,80,4100,2981,1000,9,8)
    private val recorder=RecorderTelemetry(2+4+8+512,5,1,0,3,32,43,5120,0,0)
    private val idle=DeviceTelemetry(1000,"0.4.56",false,false,0,0,0,0,recorder,battery)
    private fun refusal(value:DeviceTelemetry=idle,bits:Long=32479,now:Long=1001)=
        StorageSyncPower.refusal(TimedDeviceTelemetry(value,1000),true,now,bits)
    @Test fun exactCapabilityNegotiatesBothIdleAndRecording() {
        for(mic in 0..1){
            val info=byteArrayOf(0,1,0,0,0,0,mic.toByte(),0)
            OpProtocol.put32(info,2,32479)
            assertEquals(32479,DurableBleCodec.validatedCapabilities(info))
        }
        for(bits in listOf(16384L,31L or 16384L,7903L or 16384L,32479L or 32768L)){
            val info=byteArrayOf(0,1,0,0,0,0,0,0);OpProtocol.put32(info,2,bits)
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.validatedCapabilities(info)}
        }
    }
    @Test fun legacyNeverReceivesBatteryPermission() {
        assertNull(refusal())
        for(bits in listOf(0L,16095L,16384L,32479L or 32768L))
            assertEquals(StorageSyncPower.USB_REQUIRED,refusal(bits=bits))
    }
    @Test fun requiresFreshSafeBatteryNotJustUsbAbsent() {
        for(b in listOf(null,battery.copy(flags=4),battery.copy(flags=12),battery.copy(flags=5),
            battery.copy(percent=24),battery.copy(millivolts=3799),battery.copy(millivolts=4451),
            battery.copy(temperatureDecikelvin=2780),battery.copy(temperatureDecikelvin=3132),
            battery.copy(ageMs=20000)))assertNotNull(refusal(idle.copy(battery=b)))
        assertNull(refusal(idle.copy(battery=battery.copy(percent=25,millivolts=3800,temperatureDecikelvin=2781))))
        assertNotNull(refusal(now=11001))
        assertNotNull(refusal(now=999))
    }
    @Test fun batteryDoesNotOverrideActivityOrFault() {
        for(v in listOf(idle.copy(microphonePower=true),idle.copy(resourceBusy=true),idle.copy(faults=1),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 64)),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 128)),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 256))))assertNotNull(refusal(v))
    }
    @Test fun usbFallbackStillSupportsAnUnavailableGauge() {
        assertNull(refusal(idle.copy(battery=null,recorder=recorder.copy(flags=recorder.flags or 1))))
    }
    @Test fun reconnectRequiresBatteryCapabilityAndJoinedStorage() {
        assertTrue(StorageContinuationCheck.retired(idle,32479))
        assertFalse(StorageContinuationCheck.retired(idle.copy(recorder=recorder.copy(flags=recorder.flags and 8.inv())),32479))
        assertFalse(StorageContinuationCheck.retired(idle.copy(resourceBusy=true),32479))
        assertThrows(IllegalArgumentException::class.java){StorageContinuationCheck.retired(idle,16095)}
        assertThrows(IllegalArgumentException::class.java){StorageContinuationCheck.retired(idle.copy(battery=null),32479)}
    }
}
