package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class DashboardReadingsTest {
    private fun device() = DeviceTelemetry(1000, "0.4.53", false, false, 0, 0, 0, 0,
        RecorderTelemetry(1+2+4+16+32,0,0,0,23,32,979,5120,0,0),
        BatteryTelemetry(7,87,4110,2981,1000,1,8))
    private fun timed() = TimedDeviceTelemetry(device(),1000)
    private fun presentation(connected:Boolean=true, now:Long=1000, value:TimedDeviceTelemetry?=timed()) =
        DashboardReadings.from(value, now, connected)
    private fun observation(phase:LongRecordingControlCodec.Phase, flags:Int=0) = LongRecordingObservation(
        LongRecordingOutcome.OBSERVED, LongRecordingControlCodec.State(UUID(1,2),null,null,phase,0,flags,0,15000,15000,15000))
    private fun recording(observation:LongRecordingObservation?=null, connected:Boolean=true,
        value:TimedDeviceTelemetry?=timed(), now:Long=1000) = DashboardReadings.recording(connected,value,now,observation,1000,false)

    @Test fun missingReadingsNeverMeanEmptyChargedOrIdle() {
        val p=presentation(value=null)
        assertEquals("Not available",p.battery);assertEquals("Not checked",p.storageTitle)
        assertEquals("Not reported",p.mode);assertEquals("Check recording status",recording(value=null))
    }
    @Test fun liveReadingsShowUsableCapacityNotRawChipSize() {
        val p=presentation()
        assertEquals("87%",p.battery);assertEquals("Off",p.led)
        assertEquals("Manual · start / stop",p.mode);assertEquals("19% used",p.storageTitle)
        assertTrue(p.storageDetail.contains("170 MiB enabled total"));assertFalse(p.storageDetail.contains("512"))
        assertEquals("23 / 32 recording spaces",p.entries);assertTrue(p.device.endsWith("USB power"))
    }
    @Test fun disconnectionImmediatelyInvalidatesReadingsAndRecordingClaims() {
        val p=presentation(false)
        assertEquals("Refresh needed",p.battery);assertTrue(p.led.startsWith("Last:"))
        assertTrue(p.storageTitle.startsWith("Last:"));assertTrue(p.mode.startsWith("Last:"))
        assertEquals("Connect to check recording",recording(observation(LongRecordingControlCodec.Phase.RUNNING),false))
    }
    @Test fun expiredOrFutureReceiptNeverLooksLive() {
        for(now in listOf(999L,11001L)) {
            val p=presentation(now=now)
            assertEquals("Refresh needed",p.battery);assertTrue(p.storageTitle.startsWith("Last:"))
            assertEquals("Check recording status",recording(now=now))
        }
    }
    @Test fun oldGaugeExpiresEvenWhenDevicePacketIsFresh() {
        val t=TimedDeviceTelemetry(device().copy(battery=device().battery!!.copy(ageMs=19999)),1000)
        assertEquals("Refresh needed",presentation(now=1002,value=t).battery)
    }
    @Test fun unknownFirmwareCannotInventFreeSpace() {
        val p=presentation(value=TimedDeviceTelemetry(device().copy(firmware="0.9.99"),1000))
        assertEquals("Not checked",p.storageTitle);assertEquals("",p.entries)
    }
    @Test fun entryLimitCanBeFullWhileBytesRemain() {
        val t=TimedDeviceTelemetry(device().copy(recorder=device().recorder!!.copy(rootsUsed=32)),1000)
        assertTrue(presentation(value=t).entries.endsWith("Full"));assertEquals("Storage full",recording(value=t))
    }
    @Test fun observedRecordingStatesHaveDistinctLabels() {
        val cases=mapOf(LongRecordingControlCodec.Phase.STARTING to "Preparing recording…",
            LongRecordingControlCodec.Phase.RUNNING to "Recording · 5:00",
            LongRecordingControlCodec.Phase.STOPPING to "Saving recording…",
            LongRecordingControlCodec.Phase.DRAINING to "Saving recording…",
            LongRecordingControlCodec.Phase.STOPPED to "Saved on pendant",
            LongRecordingControlCodec.Phase.FAULT to "Recording needs attention",
            LongRecordingControlCodec.Phase.NO_CAPACITY to "Storage full",
            LongRecordingControlCodec.Phase.CANCELLED_BEFORE_START to "Recording cancelled",
            LongRecordingControlCodec.Phase.IDLE to "Ready to record")
        cases.forEach { (phase,label) -> assertEquals(label,recording(observation(phase))) }
    }
    @Test fun armingIsNotRecording() {
        assertEquals("Ready for your button tap",recording(observation(LongRecordingControlCodec.Phase.STARTING,LongRecordingControlCodec.BUTTON_ARMED)))
    }
    @Test fun observedIdleCannotHideFailedBatteryMonitor() {
        val t=TimedDeviceTelemetry(device().copy(battery=BatteryTelemetry(12,null,null,null,0,0,0)),1000)
        assertEquals("Battery monitor needs attention",recording(observation(LongRecordingControlCodec.Phase.IDLE),value=t))
        assertEquals("Recording · 5:00",recording(observation(LongRecordingControlCodec.Phase.RUNNING),value=t))
    }
    @Test fun unknownOrRebootedOutcomeOverridesAnOldIdleReading() {
        assertEquals("Check recording status",recording(LongRecordingObservation(LongRecordingOutcome.UNKNOWN,null)))
        assertEquals("Restarted · check recovery",recording(LongRecordingObservation(LongRecordingOutcome.BOOT_CHANGED,null)))
    }
    @Test fun deviceFaultAndBusyAreNotReady() {
        assertEquals("Device needs attention",recording(value=TimedDeviceTelemetry(device().copy(faults=1),1000)))
        assertEquals("Pendant is busy",recording(value=TimedDeviceTelemetry(device().copy(resourceBusy=true),1000)))
    }
    @Test fun shortTestNeverClaimsALongRecording() {
        assertEquals("Short microphone test",DashboardReadings.recording(true,timed(),1000,null,0,true))
    }
}
