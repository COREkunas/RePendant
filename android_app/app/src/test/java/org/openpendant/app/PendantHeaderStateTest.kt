package org.openpendant.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class PendantHeaderStateTest {
    private val peer = "AA:BB:CC:DD:EE:FF"
    private val wall = 1_800_000_000_000L
    private fun value() = DeviceTelemetry(1000, "0.4.62", false, false, 0, 0, 0, 0,
        RecorderTelemetry(512+1+2+4+8+16+32,0,0,0,2,32,2560,5120,0,0),
        BatteryTelemetry(7,80,4100,2900,2000,1,8))
    private fun snapshot(v:DeviceTelemetry=value(), old:PendantHeaderSnapshot?=null, at:Long=wall) =
        PendantHeaderSnapshot.observe(old,peer,TimedDeviceTelemetry(v,10_000),true,11_000,at)!!
    private fun presentation(s:PendantHeaderSnapshot?=snapshot(),v:DeviceTelemetry=value(),
        connected:Boolean=true,connecting:Boolean=false,ready:Boolean=true,now:Long=11_000,transfer:Boolean=false) =
        PendantHeaderPresentation.from(s,connected,connecting,ready,TimedDeviceTelemetry(v,10_000),now,transfer)

    @Test fun snapshotRoundTripsWithoutSensitiveData() {
        val s=snapshot();assertEquals(s,PendantHeaderSnapshot.decode(s.encode()))
        assertEquals(setOf("v","peer","readAt","activity","usb","battery","storage"),JSONObject(s.encode()).keySet())
        assertEquals(wall-1000,s.readAt);assertEquals(wall-3000,s.battery!!.readAt)
        assertEquals("Idle",s.activity);assertEquals(true,s.usbPower)
    }
    @Test fun unknownFieldsRemainUnknownAfterRestart() {
        val s=snapshot(value().copy(recorder=null,battery=null))
        assertEquals(s,PendantHeaderSnapshot.decode(s.encode()))
        assertNull(s.battery);assertNull(s.storage);assertNull(s.usbPower)
        assertEquals("State unavailable",s.activity)
        val p=presentation(s);assertEquals("—",p.batteryText);assertEquals("—",p.storageText)
    }
    @Test fun invalidOrOversizedCacheIsRejected() {
        for (text in listOf("", "{", "x".repeat(2049), "{}")) assertNull(PendantHeaderSnapshot.decode(text))
        for ((key,v) in listOf("v" to 2,"peer" to "other","readAt" to 0,"activity" to "secret")) {
            assertNull(PendantHeaderSnapshot.decode(JSONObject(snapshot().encode()).put(key,v).toString()))
        }
    }
    @Test fun corruptBatteryAndFutureMetricAreRejected() {
        for ((key,v) in listOf("percent" to -1,"percent" to 101,"at" to 0,"at" to wall)) {
            val j=JSONObject(snapshot().encode());j.getJSONObject("battery").put(key,v)
            assertNull(PendantHeaderSnapshot.decode(j.toString()))
        }
    }
    @Test fun corruptStorageCannotBecomeAUsableMeter() {
        for ((key,v) in listOf("used" to -1,"used" to 536870912,"total" to 0,"total" to 536870913,
            "entriesUsed" to -1,"entriesUsed" to 33,"entriesTotal" to 0,"entriesTotal" to 33,"at" to 0,"at" to wall)) {
            val j=JSONObject(snapshot().encode());j.getJSONObject("storage").put(key,v)
            assertNull(PendantHeaderSnapshot.decode(j.toString()))
        }
    }
    @Test fun disconnectedStaleFutureAndMissingObservationsNeverUpdateCache() {
        val timed=TimedDeviceTelemetry(value(),10_000)
        assertNull(PendantHeaderSnapshot.observe(snapshot(),peer,timed,false,11_000,wall))
        assertNull(PendantHeaderSnapshot.observe(snapshot(),peer,timed,true,20_001,wall))
        assertNull(PendantHeaderSnapshot.observe(snapshot(),peer,timed,true,9_999,wall))
        assertNull(PendantHeaderSnapshot.observe(snapshot(),null,timed,true,11_000,wall))
        assertNull(PendantHeaderSnapshot.observe(snapshot(),"invalid",timed,true,11_000,wall))
        assertNull(PendantHeaderSnapshot.observe(snapshot(),peer,null,true,11_000,wall))
        assertNull(PendantHeaderSnapshot.observe(snapshot(),peer,timed,true,11_000,100))
    }
    @Test fun freshReadKeepsUnavailableMetricsWithOriginalTimes() {
        val old=snapshot()
        val next=snapshot(value().copy(battery=null,recorder=null,resourceBusy=true),old,wall+5000)
        assertEquals(old.battery,next.battery);assertEquals(old.storage,next.storage)
        assertEquals(wall+4000,next.readAt);assertEquals("Busy",next.activity)
        val p=presentation(next,value().copy(battery=null,recorder=null))
        assertTrue(p.live);assertFalse(p.batteryLive);assertFalse(p.storageLive)
    }
    @Test fun anotherDeviceNeverInheritsOldMetrics() {
        val previous=snapshot().copy(peer="00:11:22:33:44:55")
        val next=snapshot(value().copy(battery=null,recorder=null),previous,wall+5000)
        assertNull(next.battery);assertNull(next.storage)
    }
    @Test fun wallClockRollbackDropsFutureMetrics() {
        val next=snapshot(value().copy(battery=null,recorder=null),snapshot(),wall-5000)
        assertNull(next.battery);assertNull(next.storage)
    }
    @Test fun expiredGaugeDoesNotBecomeFreshByRepeatingTelemetry() {
        val v=value().copy(battery=value().battery!!.copy(ageMs=20000))
        assertNull(snapshot(v).battery)
        val old=snapshot();val s=snapshot(v,old,wall+5000)
        assertEquals(old.battery,s.battery);assertFalse(presentation(s,v).batteryLive)
    }
    @Test fun knownStorageIsEnabledAllocationNotChipSize() {
        val s=snapshot().storage!!
        assertEquals(170L*1024*1024,s.total);assertEquals(85L*1024*1024,s.used);assertEquals(50,s.percent)
        assertEquals("50% used",presentation().storageText)
        assertEquals("85 / 170 MiB",presentation().storageSize)
    }
    @Test fun unknownFirmwareDoesNotInventStorage() {
        assertNull(snapshot(value().copy(firmware="9.9.9")).storage)
    }
    @Test fun fullEntryLimitIsReportedEvenWithByteSpace() {
        val v=value().copy(recorder=value().recorder!!.copy(rootsUsed=32))
        assertEquals("Storage full",snapshot(v).activity)
        assertEquals(50,snapshot(v).storage!!.percent)
    }
    @Test fun telemetryStatePriorityIsConservative() {
        val v=value();val r=v.recorder!!
        assertEquals("Needs attention",snapshot(v.copy(faults=1,microphonePower=true)).activity)
        assertEquals("Saving recording",snapshot(v.copy(recorder=r.copy(flags=512+32+128,state=4))).activity)
        assertEquals("Recording",snapshot(v.copy(microphonePower=true)).activity)
        assertEquals("Recording",snapshot(v.copy(recorder=r.copy(flags=512+32+128,state=3))).activity)
        assertEquals("Busy",snapshot(v.copy(resourceBusy=true)).activity)
    }
    @Test fun connectionAndFreshnessAreIndependent() {
        assertEquals("Connected",presentation().connection);assertTrue(presentation().live)
        val offline=presentation(connected=false)
        assertEquals("Disconnected",offline.connection);assertFalse(offline.live);assertFalse(offline.batteryLive);assertFalse(offline.storageLive)
        assertEquals(snapshot().battery,offline.battery);assertEquals(snapshot().readAt,offline.readAt)
        assertEquals("Connecting…",presentation(connected=false,connecting=true).connection)
        assertEquals("Checking connection…",presentation(ready=false).connection)
        assertFalse(presentation(ready=false).live);assertFalse(presentation(now=20_001).live)
        assertFalse(presentation(transfer=true).live);assertFalse(presentation(s=null).live)
    }
}
