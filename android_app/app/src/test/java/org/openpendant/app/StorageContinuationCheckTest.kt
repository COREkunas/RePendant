package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class StorageContinuationCheckTest {
    private val recorder = RecorderTelemetry(1+2+4+8+512,5,1,0,0,0,0,0,0,0)
    private val idle = DeviceTelemetry(1000,"0.4.39",false,false,0,0,0,0,recorder)
    @Test fun requiresSuspensionAfterAdmittedWorkerJoins() {
        assertTrue(StorageContinuationCheck.retired(idle))
        for (flag in listOf(2,4,8))
            assertFalse(StorageContinuationCheck.retired(idle.copy(recorder=recorder.copy(flags=recorder.flags and flag.inv()))))
        assertFalse(StorageContinuationCheck.retired(idle.copy(recorder=recorder.copy(flags=recorder.flags or 64))))
        assertFalse(StorageContinuationCheck.retired(idle.copy(resourceBusy=true)))
    }
    @Test fun cannotWaitThroughRecordingFaultOrMissingPower() {
        for (value in listOf(idle.copy(microphonePower=true),idle.copy(faults=1),idle.copy(recorder=null),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 128)),
            idle.copy(recorder=recorder.copy(flags=recorder.flags or 256)),
            idle.copy(recorder=recorder.copy(flags=recorder.flags and 1.inv()))))
            assertThrows(IllegalArgumentException::class.java) { StorageContinuationCheck.retired(value) }
    }
}
