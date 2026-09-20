package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class DurableTransportOwnershipTest {
    private val epoch=UUID(1,2)
    @Test fun oneOwnerAndOnePendingOperationWithNoAutomaticRelease() {
        val gate=DurableTransportOwnership();val lease=gate.acquire(epoch);val request=lease.beginRequest()
        assertThrows(DurableTransportBusyException::class.java){gate.acquire(epoch)}
        assertThrows(IllegalStateException::class.java){lease.beginRequest()}
        assertFalse(lease.retire());assertFalse(lease.isActive())
        assertThrows(IllegalStateException::class.java){request.requirePending()}
        assertThrows(DurableTransportBusyException::class.java){gate.acquire(UUID(3,4))}
        assertTrue(request.cancelled());assertThrows(IllegalStateException::class.java){request.requireSuccessful()}
        val fresh=gate.acquire(UUID(3,4));assertTrue(fresh.isActive());fresh.retire()
    }
    @Test fun completedOldRequestCannotSubmitOrReleaseNewPendingRequest() {
        val gate=DurableTransportOwnership();val lease=gate.acquire(epoch);val first=lease.beginRequest()
        first.requirePending();assertTrue(first.completed());first.requireSuccessful()
        assertThrows(IllegalStateException::class.java){first.requirePending()}
        val next=lease.beginRequest();assertFalse(first.cancelled());assertFalse(first.completed())
        assertThrows(IllegalStateException::class.java){first.requireSuccessful()}
        next.requirePending();assertFalse(lease.retire())
        assertThrows(DurableTransportBusyException::class.java){gate.acquire(epoch)}
        assertTrue(next.completed());assertTrue(lease.retire())
        val fresh=gate.acquire(epoch);assertTrue(lease.retire());assertTrue(fresh.isActive());fresh.retire()
        assertThrows(IllegalStateException::class.java){first.requireSuccessful()}
    }
    @Test fun racingRetirementAndCompletionAlwaysReleaseOnlyOldOwner() {
        repeat(200) {
            val gate=DurableTransportOwnership();val lease=gate.acquire(epoch);val request=lease.beginRequest()
            var failure:Throwable?=null
            val worker=Thread { try { request.completed() } catch(error:Throwable) { failure=error } }
            worker.start();lease.retire();worker.join(2000)
            assertFalse(worker.isAlive);assertNull(failure)
            val fresh=gate.acquire(epoch);assertFalse(request.cancelled());assertTrue(lease.retire())
            assertTrue(fresh.isActive());fresh.retire()
        }
    }
}
