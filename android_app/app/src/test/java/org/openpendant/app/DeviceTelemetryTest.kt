package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class DeviceTelemetryTest {
    private fun packet() = ByteArray(48).also {
        it[0] = 1; it[1] = 48; it[2] = 15; it[14] = 3; it[16] = 11
        it[26] = -1; OpProtocol.put32(it, 4, 123456)
    }
    private fun reject(bytes: ByteArray) {
        try { DeviceTelemetry.parse(bytes); fail("Invalid status accepted") } catch (_: ProtocolException) { }
    }
    @Test fun canonicalUnavailableFieldsNeverBecomeZeroMeasurements() {
        val v = DeviceTelemetry.parse(packet())
        assertEquals("0.3.11", v.firmware); assertEquals(123456L, v.uptimeMs)
        assertEquals("Off commanded", v.ledSummary); assertFalse(v.microphonePower)
        assertFalse(v.resourceBusy); assertEquals(0L, v.faults)
        for (size in 0..47) reject(packet().copyOf(size))
        reject(packet() + 0)
        for (i in listOf(0, 1, 3, 23, 24, 25, 26, 27) + (28..43).toList()) {
            reject(packet().also { it[i] = (it[i].toInt() xor 1).toByte() })
        }
    }
    @Test fun invalidFlagsBooleanAndUptimeRejected() {
        for (flag in 0..255) if (flag != 11 && flag != 15) reject(packet().also { it[2] = flag.toByte() })
        for (i in listOf(18, 19)) for (value in 2..255) reject(packet().also { it[i] = value.toByte() })
        for (value in 4..255) reject(packet().also { it[44] = value.toByte() })
        for (i in 45..47) reject(packet().also { it[i] = 1 })
        reject(packet().also { it[11] = -128 })
    }
    @Test fun appliedLedAndUnknownAreDistinct() {
        assertEquals("Red commanded", DeviceTelemetry.parse(packet().also { it[20] = 32 }).ledSummary)
        assertEquals("RGB 255 / 128 / 1", DeviceTelemetry.parse(packet().also {
            it[20] = -1; it[21] = -128; it[22] = 1
        }).ledSummary)
        val unknown = packet().also { it[2] = 11 }
        assertNull(DeviceTelemetry.parse(unknown).red)
        for (i in 20..22) reject(unknown.copyOf().also { it[i] = 1 })
    }
    @Test fun freshnessUsesMonotonicAgeAndConnectedState() {
        val sample = TimedDeviceTelemetry(DeviceTelemetry.parse(packet()), 100)
        assertTrue(sample.fresh(10100, true)); assertFalse(sample.fresh(10101, true))
        assertFalse(sample.fresh(100, false)); assertFalse(sample.fresh(99, true))
        assertEquals("Stale · disconnected; last connected observation", sample.description(1000, false))
        assertEquals(sample.description(1000, false), sample.description(900000, false))
    }
    @Test fun statusHasEmptyPayloadAndNegotiatedCapability() {
        assertArrayEquals(byteArrayOf(79, 80, 1, 32, 1, 0, 0, 0), OpProtocol.encode(OpProtocol.DEVICE_STATUS, 1))
        try { OpProtocol.encode(OpProtocol.DEVICE_STATUS, 1, byteArrayOf(1)); fail() } catch (_: ProtocolException) { }
        for (caps in listOf(15, 31)) OpProtocol.validateInfo(byteArrayOf(0, 1, caps.toByte(), 0, 0, 0, 0, 0))
    }
}
