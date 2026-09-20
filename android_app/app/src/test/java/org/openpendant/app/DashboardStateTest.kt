package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class DashboardStateTest {
    @Test fun disconnectedNeverShowsStaleMicOrSecureState() {
        val state = DashboardState.from(false, false, true, true, ClipState.RECORDING, false)
        assertEquals("Disconnected", state.connection)
        assertTrue(state.capture.startsWith("Unknown"))
        assertFalse(state.led.startsWith("Red"))
    }
    @Test fun connectedIsNotTheSameAsAuthenticated() {
        val state = DashboardState.from(true, false, false, false, null, false)
        assertEquals("Verifying secure connection…", state.connection)
        assertTrue(state.capture.startsWith("Unknown"))
    }
    @Test fun connectingDoesNotClaimAnActiveMicrophone() {
        val state = DashboardState.from(false, true, false, false, null, false)
        assertEquals("Connecting…", state.connection)
        assertTrue(state.capture.startsWith("Unknown"))
    }
    @Test fun idleAndEachCaptureStageAreDistinct() {
        assertEquals("App idle · microphone last checked off", DashboardState.from(true, false, true, false, null, false).capture)
        assertEquals("Waiting for your short button tap", DashboardState.from(true, false, false, true, ClipState.WAITING, false).capture)
        val active = DashboardState.from(true, false, false, true, ClipState.RECORDING, false)
        assertEquals("Connected securely", active.connection)
        assertEquals("Recording short clip", active.capture)
        assertTrue(active.led.contains("no LED readback"))
        assertEquals("Receiving clip · app capture finished", DashboardState.from(true, false, false, true, ClipState.READY, false).capture)
    }
    @Test fun cancellationDoesNotPrematurelyPromiseMicOff() {
        val state = DashboardState.from(true, false, false, true, ClipState.RECORDING, true)
        assertEquals("Stopping · awaiting confirmation", state.capture)
        assertFalse(state.led.startsWith("Red"))
    }
    @Test fun unavailableFeaturesAreExplicit() {
        assertTrue(DashboardState.BATTERY.contains("Not reported"))
        assertTrue(DashboardState.STORAGE.contains("not ready"))
        assertTrue(DashboardState.MODES.contains("need new firmware"))
        assertTrue(DashboardState.TRANSCRIPTION.contains("import the trusted model"))
        assertTrue(DashboardState.TRANSCRIPTION.contains("tap Transcribe"))
    }
}
