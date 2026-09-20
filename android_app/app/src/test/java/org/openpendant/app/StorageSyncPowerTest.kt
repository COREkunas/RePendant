package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class StorageSyncPowerTest {
    private val recorder=RecorderTelemetry(1+2+4+8+512,5,1,0,3,32,43,5120,0,0)
    private val idle=DeviceTelemetry(1000,"0.4.55",false,false,0,0,0,0,recorder)
    private fun refusal(value:DeviceTelemetry)=StorageSyncPower.refusal(TimedDeviceTelemetry(value,1000),true,1001)
    @Test fun batteryGivesActionableReasonBeforeAnyTransfer() {
        assertEquals(StorageSyncPower.USB_REQUIRED,refusal(idle.copy(recorder=recorder.copy(flags=recorder.flags and 1.inv()))))
        assertNull(refusal(idle))
    }
    @Test fun unknownStaleDisconnectedAndBackwardsClockCannotAuthorizeSync() {
        assertNotNull(StorageSyncPower.refusal(null,true,1000))
        val timed=TimedDeviceTelemetry(idle,1000)
        assertNotNull(StorageSyncPower.refusal(timed,false,1001))
        assertNotNull(StorageSyncPower.refusal(timed,true,999))
        assertNotNull(StorageSyncPower.refusal(timed,true,11001))
        assertNull(StorageSyncPower.refusal(timed,true,11000))
        assertNotNull(refusal(idle.copy(recorder=null)))
    }
    @Test fun recordingSavingAndFaultsCannotStartTransfer() {
        for(value in listOf(idle.copy(microphonePower=true),idle.copy(resourceBusy=true),idle.copy(faults=1),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 128)),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 64)),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 256)),
            idle.copy(recorder=recorder.copy(flags=recorder.flags and 2.inv()))))assertNotNull(refusal(value))
    }
    @Test fun freshReconnectCanMountUnopenedStorageWithoutWeakeningPowerGate() {
        val unopened=idle.copy(recorder=recorder.copy(flags=1+2+512))
        assertNull(refusal(unopened))
        assertEquals(StorageSyncPower.USB_REQUIRED,refusal(unopened.copy(recorder=recorder.copy(flags=2+512))))
    }
}
