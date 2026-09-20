package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import org.openpendant.app.MtuNegotiation.Action

class MtuNegotiationTest {
    private fun rejected(action: () -> Unit) {
        try { action(); fail("Invalid MTU event accepted") } catch (_: ProtocolException) { }
    }

    @Test fun earlyConnectionMtuWaitsForValidatedServices() {
        val state = MtuNegotiation()
        assertEquals(Action.NONE, state.changed(247, true))
        assertEquals(247, state.mtu)
        assertEquals(Action.NONE, state.changed(247, true))
        assertEquals(Action.SUBSCRIBE, state.servicesDiscovered())
        repeat(3) { assertEquals(Action.NONE, state.changed(247, true)) }
        rejected { state.servicesDiscovered() }
    }

    @Test fun explicitRequestCompletesOnceAndLateEventsDoNotRestartSubscription() {
        val state = MtuNegotiation()
        assertEquals(23, state.mtu)
        assertEquals(Action.REQUEST_MTU, state.servicesDiscovered())
        assertEquals(Action.SUBSCRIBE, state.changed(247, true))
        for (value in listOf(247, 517, 96)) {
            assertEquals(Action.NONE, state.changed(value, true))
            assertEquals(value, state.mtu)
        }
    }

    @Test fun defaultEarlyMtuStillRequiresExplicitNegotiation() {
        for (value in listOf(23, 95)) {
            val state = MtuNegotiation()
            assertEquals(Action.NONE, state.changed(value, true))
            assertEquals(Action.REQUEST_MTU, state.servicesDiscovered())
            assertEquals(Action.SUBSCRIBE, state.changed(96, true))
        }
    }

    @Test fun errorsAndInsufficientNegotiatedSizesFailClosed() {
        for (stage in 0..2) {
            fun state() = MtuNegotiation().also {
                if (stage >= 1) it.servicesDiscovered()
                if (stage >= 2) it.changed(247, true)
            }
            rejected { state().changed(247, false) }
            for (value in listOf(-1, 0, 22, 518, Int.MAX_VALUE)) {
                rejected { state().changed(value, true) }
            }
            if (stage >= 1) for (value in listOf(23, 95)) {
                rejected { state().changed(value, true) }
            }
        }
    }

    @Test fun separateConnectionsDoNotShareNegotiatedMtu() {
        val previous = MtuNegotiation()
        previous.changed(247, true)
        assertEquals(Action.SUBSCRIBE, previous.servicesDiscovered())
        val next = MtuNegotiation()
        assertEquals(23, next.mtu)
        assertEquals(Action.REQUEST_MTU, next.servicesDiscovered())
        assertEquals(Action.SUBSCRIBE, next.changed(517, true))
    }
}
