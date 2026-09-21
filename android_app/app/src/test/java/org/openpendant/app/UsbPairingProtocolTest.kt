package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class UsbPairingProtocolTest {
    private val address = "C6:05:04:03:02:01"
    private fun identity(address: String = this.address, type: Int = 1) = "PAIRING_USB_ID v=1 address=$address type=$type"
    private fun state(bonds: Int = 0, open: Int = 0, remaining: Int = 0, pending: Int = 0, ready: Int = 0) =
        "PAIRING_STATUS rc=0 bonds=$bonds open=$open remaining_ms=$remaining pending=$pending code_ready=$ready; native bond durability requires reboot/reconnect validation"
    private fun code(value: String = "000037") = "PAIRING_PASSKEY $value; enter only in your phone's system pairing dialog"
    private fun rejected(block: () -> Unit) { try { block(); fail("Must refuse") } catch (_: IllegalArgumentException) { } catch (_: IllegalStateException) { } }

    @Test fun matchesCableIdentityNotDeviceName() {
        assertEquals(address, UsbPairingProtocol.identity("pairing usbinfo\r\n${identity()}\r\n").address)
        assertEquals(1, UsbPairingProtocol.identity(identity()).type)
    }
    @Test fun publicIdentitySupported() {
        assertEquals(0, UsbPairingProtocol.identity(identity("16:05:04:03:02:01",0)).type)
    }
    @Test fun privateOrUnknownAddressTypeRefused() {
        for (s in listOf(identity(type=2),identity("86:05:04:03:02:01"),identity("46:05:04:03:02:01")))
            rejected { UsbPairingProtocol.identity(s) }
    }
    @Test fun emptyAndBroadcastIdentityRefused() {
        for (s in listOf(identity("00:00:00:00:00:00",0),identity("FF:FF:FF:FF:FF:FF")))
            rejected { UsbPairingProtocol.identity(s) }
    }
    @Test fun duplicateMissingMalformedIdentityRefused() {
        for (s in listOf("", "OpenPendant",identity()+"\n"+identity(),identity().replace("v=1","v=2"),identity()+" junk",identity().lowercase()))
            rejected { UsbPairingProtocol.identity(s) }
    }
    @Test fun boundedReplies() {
        rejected { UsbPairingProtocol.identity("x".repeat(4097)+identity()) }
        rejected { UsbPairingProtocol.status("\u0000"+state()) }
    }
    @Test fun idleNoCode() {
        UsbPairingProtocol.status(state()).use { assertEquals(0,it.bonds);assertFalse(it.open);assertNull(it.takeCode()) }
    }
    @Test fun codeKeepsLeadingZeroesAndIsConsumedOnce() {
        UsbPairingProtocol.status(state(open=1,remaining=59000,pending=1,ready=1)+"\r\n"+code()).use {
            assertTrue(it.codeReady)
            val bytes=it.takeCode()!!
            assertArrayEquals("000037".toByteArray(),bytes);bytes.fill(0)
            assertNull(it.takeCode());assertFalse(it.codeReady)
        }
    }
    @Test fun closeDiscardsCode() {
        val s=UsbPairingProtocol.status(state(open=1,remaining=1,pending=1,ready=1)+"\n"+code())
        s.close();assertNull(s.takeCode())
    }
    @Test fun codeNeverAppearsInStatusToString() {
        UsbPairingProtocol.status(state(open=1,remaining=1,pending=1,ready=1)+"\n"+code("234567")).use {
            assertFalse(it.toString().contains("234567"))
        }
    }
    @Test fun missingDuplicateUnexpectedCodeRefused() {
        val s=state(open=1,remaining=1,pending=1,ready=1)
        for (text in listOf(s,s+"\n"+code()+"\n"+code(),state()+"\n"+code()))
            rejected { UsbPairingProtocol.status(text) }
    }
    @Test fun onlySixAsciiDigitsAllowed() {
        for (value in listOf("12345","1234567","+12345"," 12345","１２３４５６","00003x"))
            rejected { UsbPairingProtocol.status(state(open=1,remaining=1,pending=1,ready=1)+"\n"+code(value)) }
    }
    @Test fun failedOrDuplicateStatusRefused() {
        for (s in listOf(state().replace("rc=0","rc=-5"),state()+"\n"+state(),"")) rejected { UsbPairingProtocol.status(s) }
    }
    @Test fun inconsistentWindowRefused() {
        for (s in listOf(state(open=1),state(remaining=1),state(open=1,remaining=60001),state(pending=1),state(bonds=1,open=1,remaining=1)))
            rejected { UsbPairingProtocol.status(s) }
    }
    @Test fun codeNeedsPendingAndUnownedWindow() {
        for (s in listOf(state(ready=1),state(open=1,remaining=1,ready=1),state(bonds=1,open=1,remaining=1,pending=1,ready=1)))
            rejected { UsbPairingProtocol.status(s+"\n"+code()) }
    }
    @Test fun successNeedsBothEndsAndSubmittedCode() {
        UsbPairingProtocol.status(state(bonds=1)).use {
            assertTrue(UsbPairingProtocol.complete(true,true,it))
            assertFalse(UsbPairingProtocol.complete(false,true,it))
            assertFalse(UsbPairingProtocol.complete(true,false,it))
        }
        UsbPairingProtocol.status(state(open=1,remaining=100)).use { assertFalse(UsbPairingProtocol.complete(true,true,it)) }
    }
    @Test fun openMustBeExplicitlyAcknowledged() {
        assertTrue(UsbPairingProtocol.opened("PAIRING_OPEN seconds=60; connect phone, then request pairing status for the passkey; microphone remains off"))
        assertFalse(UsbPairingProtocol.opened("PAIRING_REFUSED owner_already_bonded; remote replacement disabled"))
        assertFalse(UsbPairingProtocol.opened("PAIRING_OPEN seconds=60"))
    }
    @Test fun fixedCommandsCannotUnpairOrErase() {
        assertEquals(setOf("pairing usbinfo","pairing status","pairing open confirm","pairing close","recorder fullinfo"),UsbPairingProtocol.Command.values().map { it.wire }.toSet())
    }
    @Test fun codeClaimNeedsMatchingSystemPairRequest() {
        val a=UsbPairingAttempt(address,100)
        assertFalse(a.claimCode(address,true,10))
        assertFalse(a.pairingRequest("C6:00:00:00:00:00",1,true,10))
        assertFalse(a.claimCode(address,true,10))
        assertTrue(a.pairingRequest(address,1,true,10))
        assertFalse(a.claimCode("C6:00:00:00:00:00",true,10))
        assertFalse(a.claimCode(address,false,10))
        assertTrue(a.claimCode(address,true,10))
        assertFalse(a.claimCode(address,true,11))
        assertFalse(a.pairingRequest(address,1,true,11))
    }
    @Test fun neverAcceptsNumericComparisonOrConsent() {
        for (variant in listOf(-1,2,3,4,5,6,7,255)) {
            val a=UsbPairingAttempt(address,100)
            assertFalse(a.pairingRequest(address,variant,true,10));assertFalse(a.claimCode(address,true,10))
        }
    }
    @Test fun pinAndPasskeyVariantsSupported() {
        for (variant in 0..1) {
            val a=UsbPairingAttempt(address,100)
            assertTrue(a.pairingRequest(address,variant,true,10));assertTrue(a.claimCode(address,true,10))
        }
    }
    @Test fun expiryAndCancellationFenceCodeSubmission() {
        val a=UsbPairingAttempt(address,100)
        assertFalse(a.pairingRequest(address,1,false,10))
        assertTrue(a.pairingRequest(address,1,true,10));assertFalse(a.claimCode(address,true,100))
        val b=UsbPairingAttempt(address,100)
        assertTrue(b.pairingRequest(address,1,true,10));b.cancel();assertFalse(b.claimCode(address,true,11))
        assertFalse(b.pairingRequest(address,1,true,11))
        assertFalse(UsbPairingAttempt(address,100).pairingRequest(address,1,true,100))
    }
}
