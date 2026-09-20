package org.openpendant.app
import org.junit.Assert.*
import org.junit.Test

class ConnectionRecoveryTest {
    @Test fun noStartupOrForeignBondConnection() {
        val p=ConnectionRecovery();p.closed("a",0)
        assertFalse(p.claim("a",1000,true,true))
        p.explicitConnect("a",0);p.closed("b",0)
        assertFalse(p.claim("a",1000,true,true))
    }
    @Test fun mustWaitForForegroundOwnersAndClosureProof() {
        val p=ConnectionRecovery();p.explicitConnect("a",0);p.closed("a",0)
        assertFalse(p.claim("a",749,true,true))
        assertFalse(p.claim("a",750,false,true))
        assertFalse(p.claim("a",750,true,false))
        assertTrue(p.claim("a",750,true,true))
        assertFalse(p.claim("a",751,true,true))
    }
    @Test fun explicitDisconnectAndFailuresCancelPendingRecovery() {
        val p=ConnectionRecovery();p.explicitConnect("a",0);p.closed("a",0);p.stop()
        assertFalse(p.claim("a",1000,true,true));p.closed("a",1000)
        assertFalse(p.claim("a",2000,true,true))
    }
    @Test fun bondChangeRevokesIntent() {
        val p=ConnectionRecovery();p.explicitConnect("a",0);p.closed("a",0)
        assertFalse(p.claim("b",1000,true,true));assertFalse(p.claim("a",2000,true,true))
    }
    @Test fun repeatedLinkLossHasABoundedBudget() {
        val p=ConnectionRecovery();p.explicitConnect("a",0)
        repeat(3){p.closed("a",it*1000L);assertTrue(p.claim("a",it*1000L+750,true,true))}
        p.closed("a",3000);assertFalse(p.claim("a",4000,true,true))
        p.closed("a",200_000);assertFalse(p.claim("a",201_000,true,true))
    }
    @Test fun plannedCleanupDoesNotConsumeTheFlappingBudget() {
        val p=ConnectionRecovery();p.explicitConnect("a",0)
        repeat(8){p.closed("a",it*1000L,true);assertTrue(p.claim("a",it*1000L+750,true,true))}
    }
    @Test fun clockRollbackAndExpiredReadingsNeverBecomeFresh() {
        val p=ConnectionRecovery();p.explicitConnect("a",1000);p.closed("a",1000)
        assertFalse(p.claim("a",0,true,true));assertFalse(p.waiting)
        val d=DeviceTelemetry(0,"0.4.56",false,false,0,0,0,0)
        val t=TimedDeviceTelemetry(d,Long.MIN_VALUE)
        assertFalse(t.fresh(100,true));assertEquals(Long.MAX_VALUE,t.ageMs(100))
    }
}
