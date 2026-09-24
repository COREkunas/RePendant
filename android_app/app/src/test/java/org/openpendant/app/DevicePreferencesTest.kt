package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class DevicePreferencesTest {
    @Test fun defaultWireMatchesFirmware() {
        val bytes=byteArrayOf(1,0,32,1,4,0,0,1,25,1,0,0,0,0,0,0)
        assertArrayEquals(bytes,DevicePreferences().encode())
        assertEquals(DevicePreferences(),DevicePreferences.decode(bytes))
    }
    @Test fun exactAcknowledgementRequired() {
        val request=DevicePreferences(profile=2,brightness=8,recording=3,lowPercent=30,revision=42)
        assertEquals(request.copy(revision=43),DevicePreferences.accepted(request,request.copy(revision=43).encode()))
        assertTrue(runCatching { DevicePreferences.accepted(request,request.encode()) }.isFailure)
        assertTrue(runCatching { DevicePreferences.accepted(request,request.copy(revision=43,brightness=9).encode()) }.isFailure)
        assertTrue(runCatching { DevicePreferences.accepted(request.copy(revision=0xffffffffL),request.encode()) }.isFailure)
    }
    @Test fun mandatoryIndicatorsCannotBeDisabled() {
        for(index in listOf(3,4,7)) {
            val data=DevicePreferences().encode();data[index]=0
            assertTrue(runCatching { DevicePreferences.decode(data) }.isFailure)
        }
        assertEquals(0,DevicePreferences.decode(DevicePreferences(connected=0,usb=0).encode()).connected)
    }
    @Test fun malformedFieldsFailClosed() {
        for((index,value) in listOf(0 to 3,1 to 3,2 to 7,2 to 65,3 to 8,4 to 8,5 to 8,6 to 8,
            7 to 8,8 to 19,8 to 51,9 to 4,10 to 1,11 to 1)) {
            val data=DevicePreferences().encode();data[index]=value.toByte()
            assertTrue("field $index value $value",runCatching { DevicePreferences.decode(data) }.isFailure)
        }
        for(size in listOf(0,15,17))assertTrue(runCatching { DevicePreferences.decode(ByteArray(size)) }.isFailure)
    }
    @Test fun schemaTwoPersistsHiddenModeChargingAndAcknowledgesExactly() {
        val p=DevicePreferences(schema=2,recordingBehavior=1,charging=5,recording=3,revision=81)
        assertEquals(2,p.encode()[0].toInt());assertEquals(1,p.encode()[10].toInt());assertEquals(5,p.encode()[11].toInt())
        assertEquals(p,DevicePreferences.decode(p.encode()))
        assertEquals(p.copy(revision=82),DevicePreferences.accepted(p,p.copy(revision=82).encode()))
        assertThrows(IllegalArgumentException::class.java){DevicePreferences.accepted(p,p.copy(revision=82,charging=0).encode())}
        assertThrows(IllegalArgumentException::class.java){p.copy(schema=1).encode()}
        for((index,value) in listOf(10 to 2,11 to 8,0 to 3)) {
            val bytes=p.encode();bytes[index]=value.toByte()
            assertThrows(IllegalArgumentException::class.java){DevicePreferences.decode(bytes)}
        }
    }
    @Test fun commandsEnforceBodyGrammar() {
        assertEquals(8,OpProtocol.encode(OpProtocol.GET_PREFERENCES,1).size)
        assertEquals(24,OpProtocol.encode(OpProtocol.SET_PREFERENCES,2,DevicePreferences().encode()).size)
        assertTrue(runCatching { OpProtocol.encode(OpProtocol.GET_PREFERENCES,1,byteArrayOf(0)) }.isFailure)
        assertTrue(runCatching { OpProtocol.encode(OpProtocol.SET_PREFERENCES,1) }.isFailure)
    }
    @Test fun capabilityIsNarrowAndKeepsLongRecording() {
        val info=byteArrayOf(0,1,0,0,0,0,1,0).also { OpProtocol.put32(it,2,7903) }
        assertEquals(7903,DurableBleCodec.validatedCapabilities(info))
        OpProtocol.put32(info,2,7903L or (1L shl 14))
        assertTrue(runCatching { DurableBleCodec.validatedCapabilities(info) }.isFailure)
    }
    @Test fun standaloneProfileConnectsIdleAndDuringPhysicalRecording() {
        val bits=7903L or LongRecordingControlCodec.STANDALONE_CAPABILITY
        assertEquals(16095L,bits)
        for(microphone in 0..1) {
            val info=byteArrayOf(0,1,0,0,0,0,microphone.toByte(),0)
            OpProtocol.put32(info,2,bits)
            assertEquals(bits,DurableBleCodec.validatedCapabilities(info))
            assertEquals(microphone,info[6].toInt()) // validation must not mask the original status
        }
    }
    @Test fun standaloneAdmissionRejectsPartialAndUnknownProfiles() {
        val info=byteArrayOf(0,1,0,0,0,0,0,0)
        for(bits in listOf(8192L,31L or 8192L,3807L or 8192L,16095L or 32768L)) {
            OpProtocol.put32(info,2,bits)
            assertTrue(runCatching { DurableBleCodec.validatedCapabilities(info) }.isFailure)
        }
        OpProtocol.put32(info,2,16095L);info[6]=2
        assertTrue(runCatching { DurableBleCodec.validatedCapabilities(info) }.isFailure)
        info[6]=0;info[7]=1
        assertTrue(runCatching { DurableBleCodec.validatedCapabilities(info) }.isFailure)
    }
}
