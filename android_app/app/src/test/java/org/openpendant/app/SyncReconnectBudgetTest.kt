package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class SyncReconnectBudgetTest {
    @Test fun onlyThreeLossesCanReconnect() {
        val b=SyncReconnectBudget(100){1000};repeat(3){assertTrue(b.admit(true,false))}
        assertFalse(b.admit(true,false));assertEquals(3,b.count)
    }
    @Test fun protocolErrorIsNotPermissionToReconnectLater() {
        val b=SyncReconnectBudget(100){1000};assertFalse(b.admit(false,false));assertFalse(b.admit(true,false))
    }
    @Test fun cancellationIsPermanent() {
        val b=SyncReconnectBudget(100){1000};assertFalse(b.admit(true,true));assertFalse(b.admit(true,false))
    }
    @Test fun clockReversalAndExpiredJobNeverRenew() {
        for(time in listOf(99L,100+SyncReconnectBudget.MAX_JOB_MILLIS)) {
            val b=SyncReconnectBudget(100){time};assertFalse(b.admit(true,false));assertEquals(0,b.count)
        }
    }
}
