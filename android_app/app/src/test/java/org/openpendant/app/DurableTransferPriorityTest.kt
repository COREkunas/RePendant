package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class DurableTransferPriorityTest {
    @Test fun hintsOnceForManyFragmentsWithoutOwningOrStartingARequest() {
        val owner = DurableTransportOwnership(); val lease = owner.acquire(UUID.randomUUID())
        val policy = DurableTransferPriority()
        assertTrue(policy.begin(lease))
        repeat(1000) { assertFalse(policy.begin(lease)) }
        assertTrue(lease.isActive())
        val request = lease.beginRequest(); assertTrue(request.completed())
        assertTrue(policy.clear()); assertFalse(policy.clear()); assertTrue(lease.retire())
    }
    @Test fun rejectsRetiredLeaseBeforeHinting() {
        val lease = DurableTransportOwnership().acquire(UUID.randomUUID()); assertTrue(lease.retire())
        val policy = DurableTransferPriority()
        assertThrows(IllegalStateException::class.java) { policy.begin(lease) }
        assertFalse(policy.clear())
    }
    @Test fun cannotReplaceAnUnclearedSession() {
        val first = DurableTransportOwnership().acquire(UUID.randomUUID())
        val second = DurableTransportOwnership().acquire(UUID.randomUUID())
        val policy = DurableTransferPriority(); assertTrue(policy.begin(first))
        assertThrows(IllegalStateException::class.java) { policy.begin(second) }
        assertFalse(policy.begin(first)); assertTrue(policy.clear())
        assertTrue(policy.begin(second)); assertTrue(policy.clear())
        assertTrue(first.retire()); assertTrue(second.retire())
    }
    @Test fun clearNeverReleasesPendingRadioCallbacksOrCloseFence() {
        val owner = DurableTransportOwnership(); val lease = owner.acquire(UUID.randomUUID())
        val policy = DurableTransferPriority(); assertTrue(policy.begin(lease))
        lease.beginRequest(); assertTrue(lease.revokeForTransportClose())
        assertTrue(policy.clear()); assertFalse(lease.retire())
        assertThrows(DurableTransportBusyException::class.java) { owner.acquire(UUID.randomUUID()) }
        assertTrue(lease.cancelAfterTransportClosed()); assertTrue(owner.isIdle())
    }
    @Test fun revokedLeaseCannotRenewHighPriority() {
        val lease = DurableTransportOwnership().acquire(UUID.randomUUID())
        val policy = DurableTransferPriority(); assertTrue(policy.begin(lease))
        assertTrue(lease.revokeForTransportClose())
        assertThrows(IllegalStateException::class.java) { policy.begin(lease) }
        assertTrue(policy.clear()); assertTrue(lease.cancelAfterTransportClosed())
    }
}
