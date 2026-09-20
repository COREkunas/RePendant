package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class RecordingPowerStatusTest {
    private val ready = DeviceTelemetry(1000,"0.4.59",false,false,0,0,0,0,
        RecorderTelemetry(1+2+4+16+32,0,0,0,0,32,0,5120,0,0),
        BatteryTelemetry(7,90,4100,2981,1000,1,8))
    private fun sample(flags: Int) = TimedDeviceTelemetry(ready.copy(battery=
        if(flags and 1 != 0) ready.battery!!.copy(flags=flags)
        else BatteryTelemetry(flags,null,null,null,0,0,0)),1000)
    @Test fun unavailableAndStoppedNeverClaimReady() {
        for(flags in listOf(4,12)) {
            val t=sample(flags)
            assertNotNull(RecordingPowerStatus.blocked(t,1000,true))
            assertNotEquals("Ready to record",DashboardReadings.recording(true,t,1000,null,0,false))
        }
        assertEquals("Checking battery setup…",RecordingPowerStatus.blocked(sample(4),1000,true))
        assertEquals("Battery monitor needs attention",RecordingPowerStatus.blocked(sample(12),1000,true))
    }
    @Test fun readyPowerRequiresFreshSufficientSample() {
        assertNull(RecordingPowerStatus.blocked(sample(7),1000,true))
        assertNotNull(RecordingPowerStatus.blocked(sample(5),1000,true))
        assertNotNull(RecordingPowerStatus.blocked(sample(7),11001,true))
        assertNotNull(RecordingPowerStatus.blocked(sample(7),1000,false))
        assertNotNull(RecordingPowerStatus.blocked(sample(7),999,true))
    }
    @Test fun deferredBatteryBootExplainsUsbRequirement() {
        val t=sample(4).let { it.copy(value=it.value.copy(recorder=it.value.recorder!!.copy(flags=2+4+16+32))) }
        assertEquals("Battery setup needed · connect USB",RecordingPowerStatus.blocked(t,1000,true))
    }
    @Test fun activeRecordingRemainsVisibleDuringPowerFailure() {
        val t=sample(12).let { it.copy(value=it.value.copy(microphonePower=true)) }
        assertEquals("Recording on pendant",DashboardReadings.recording(true,t,1000,null,0,false))
    }
    @Test fun newFirmwareKeepsCapacityAndSettingsCompatibility() {
        assertNotNull(PendantStoragePresentation.from(ready))
        assertTrue(DeviceSettingsPower.batterySupported(TimedDeviceTelemetry(ready,1000),32479))
    }
}
