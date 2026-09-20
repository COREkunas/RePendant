package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class PendantStoragePresentationTest {
    @Test fun fullProfileShowsActualAllocatedSpaceAndConservesBytes() {
        val old=value(0,0);val r=old.recorder!!
        val full=old.copy(firmware="0.4.29",recorder=r.copy(rootsTotal=32,slotsTotal=5120,slotsUsed=100))
        val p=PendantStoragePresentation.from(full)!!
        assertEquals(170L*1024*1024,p.totalBytes)
        assertEquals(100L*17*2048,p.occupiedBytes)
        assertEquals(p.totalBytes,p.occupiedBytes+p.availableBytes)
        assertFalse(p.detail.contains("test area"))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.30")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.35")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.36")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.38")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.40")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.42")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.43")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.44")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.45")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.49")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.49")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.50")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.50")))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.51")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.51")))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.51",recorder=full.recorder!!.copy(slotsTotal=5119))))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.51",recorder=full.recorder!!.copy(rootsTotal=31))))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.52")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.52")))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.52",recorder=full.recorder!!.copy(slotsTotal=5119))))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.52",recorder=full.recorder!!.copy(rootsTotal=31))))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.53")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.53")))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.53",recorder=full.recorder!!.copy(slotsTotal=5119))))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.53",recorder=full.recorder!!.copy(rootsTotal=31))))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.54")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.54")))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.54",recorder=full.recorder!!.copy(slotsTotal=5119))))
        assertEquals(p,PendantStoragePresentation.from(full.copy(firmware="0.4.55")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.55")))
        assertNotNull(PendantStoragePresentation.from(full.copy(firmware="0.4.56")))
        assertNotNull(PendantStoragePresentation.from(full.copy(firmware="0.4.57")))
        assertNotNull(PendantStoragePresentation.from(full.copy(firmware="0.4.58")))
        assertNotNull(PendantStoragePresentation.from(full.copy(firmware="0.4.59")))
        assertNotNull(PendantStoragePresentation.from(full.copy(firmware="0.4.60")))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.61")))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.99")))
        assertNull(PendantStoragePresentation.from(old.copy(firmware="0.4.30")))
        assertNull(PendantStoragePresentation.from(full.copy(firmware="0.4.27")))
    }
    private fun value(used: Int = 21, roots: Int = 8) = DeviceTelemetry(0, "0.4.27", false, false,
        0, 0, 0, 0, RecorderTelemetry(512 + 1 + 2 + 4 + 8 + 16, 0, 0, 0, roots, 8, used, 22, 0, 0))

    @Test fun realCurrentPoolDistinguishesBytesFromEntryExhaustion() {
        val p = PendantStoragePresentation.from(value())!!
        assertEquals(748L * 1024, p.totalBytes)
        assertEquals(714L * 1024, p.occupiedBytes)
        assertEquals(34L * 1024, p.availableBytes)
        assertEquals(95, p.percentUsed)
        assertTrue(p.entryLimitReached)
        assertEquals("714 KiB occupied · 34 KiB available", p.summary)
        assertTrue(p.detail.contains("entry limit reached"))
        assertTrue(p.detail.contains("not enabled"))
    }
    @Test fun emptyPartialAndFullAreConserved() {
        for (used in 0..22) {
            val p = PendantStoragePresentation.from(value(used, 0))!!
            assertEquals(p.totalBytes, p.occupiedBytes + p.availableBytes)
            assertFalse(p.entryLimitReached)
            assertTrue(p.percentUsed in 0..100)
        }
        assertEquals(0, PendantStoragePresentation.from(value(0, 0))!!.percentUsed)
        assertEquals(100, PendantStoragePresentation.from(value(22))!!.percentUsed)
    }
    @Test fun unknownFaultedLegacyAndFutureFirmwareNeverInventCapacity() {
        assertNull(PendantStoragePresentation.from(null))
        assertNull(PendantStoragePresentation.from(value().copy(recorder = null)))
        assertNull(PendantStoragePresentation.from(value().copy(firmware = "0.4.28")))
        val r = value().recorder!!
        assertNull(PendantStoragePresentation.from(value().copy(recorder = r.copy(flags = r.flags and 16.inv()))))
        assertNull(PendantStoragePresentation.from(value().copy(recorder = r.copy(flags = r.flags or 256))))
        assertNull(PendantStoragePresentation.from(value().copy(recorder = r.copy(slotsTotal = 23))))
        assertNull(PendantStoragePresentation.from(value().copy(recorder = r.copy(rootsTotal = 7))))
    }
}
