package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class StorageSubscriptionResetTest {
    @Test fun localCloseDoesNotProveRemoteRetirement() {
        val f=StorageSubscriptionReset();assertFalse(f.required("a"));f.touched("a")
        repeat(3){assertTrue(f.required("a"));f.observed("a",false)}
        f.observed("a",true);assertFalse(f.required("a"))
        f.touched("a");assertTrue(f.required("a"))
    }
    @Test fun anotherBondCannotClearOrInheritPendingCatalog() {
        val f=StorageSubscriptionReset();f.touched("a")
        assertFalse(f.required(null));assertFalse(f.required("b"))
        f.observed("b",true);f.observed(null,true);assertTrue(f.required("a"))
        f.touched("b");f.observed("a",true);assertTrue(f.required("b"));assertFalse(f.required("a"))
    }
    @Test fun normalRecordingLowPowerAndUnknownStatusNeverThrowOrClearPendingEvidence() {
        val r=RecorderTelemetry(1+2+4+8+512,1,0,0,0,0,0,0,0,0)
        val idle=DeviceTelemetry(1,"0.4.57",false,false,0,0,0,0,r)
        val f=StorageSubscriptionReset();f.touched("a")
        for(v in listOf(idle.copy(microphonePower=true),idle.copy(faults=1),idle.copy(resourceBusy=true),
            idle.copy(recorder=null),idle.copy(recorder=r.copy(flags=r.flags or 128)),
            idle.copy(recorder=r.copy(flags=r.flags or 64)),idle.copy(recorder=r.copy(flags=r.flags or 256)),
            idle.copy(recorder=r.copy(flags=r.flags and 8.inv())),idle.copy(recorder=r.copy(flags=r.flags and 1.inv())))) {
            f.observed("a",v,32479);assertTrue(f.required("a"))
        }
        f.observed("a",idle,32479);assertFalse(f.required("a"))
    }
}
