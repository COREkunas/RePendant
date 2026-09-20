package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class BlePhyObservationTest {
    @Test fun acceptsOnlyObservedSuccessfulPhyPairs() {
        for (tx in 1..3) for (rx in 1..3)
            assertEquals(BlePhyObservation(tx, rx), BlePhyObservation.fromCallback(true, tx, rx))
    }
    @Test fun failureIsUnknownEvenWhenCallbackContainsTwoMegabitValues() {
        assertNull(BlePhyObservation.fromCallback(false, 2, 2))
    }
    @Test fun malformedValuesStayUnknownInsteadOfImplyingFastMode() {
        for (invalid in listOf(Int.MIN_VALUE, -1, 0, 4, Int.MAX_VALUE)) {
            assertNull(BlePhyObservation.fromCallback(true, invalid, 2))
            assertNull(BlePhyObservation.fromCallback(true, 2, invalid))
        }
    }
    @Test fun oneMegabitFallbackIsValidAndNotRelabelled() {
        assertEquals(BlePhyObservation(1, 1), BlePhyObservation.fromCallback(true, 1, 1))
    }
}
