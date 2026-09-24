package org.openpendant.app
import org.junit.Assert.*
import org.junit.Test

class RecorderTelemetryTest {
    @Test fun everySupportedFullFirmwarePassesTheWireParserAndPresentationTogether() {
        for(version in PendantStoragePresentation.FULL_FIRMWARE + (56..62).map { "0.4.$it" }) {
            val p=packet();p[16]=version.substringAfterLast('.').toInt().toByte()
            p[53]=4;p[54]=32;p[56]=50;p[57]=0;p[58]=0;p[59]=20
            val value=DeviceTelemetry.parse(p)
            assertEquals(version,value.firmware)
            assertTrue(StorageContinuationCheck.retired(value))
            val capacity=PendantStoragePresentation.from(value)!!
            assertEquals(4,capacity.entriesUsed)
            assertEquals(50L*17*2048,capacity.occupiedBytes)
            assertEquals(5070L*17*2048,capacity.availableBytes)
            reject(p.copyOf().also { it[54]=31 })
            reject(p.copyOf().also { it[59]=19 })
        }
    }
    @Test fun orderedCatalog51UsesTheUnchangedFullStorageGeometry() {
        val p=packet();p[16]=51;p[53]=23;p[54]=32
        p[56]=979.toByte();p[57]=(979 shr 8).toByte();p[58]=0;p[59]=20
        val value=DeviceTelemetry.parse(p)
        assertEquals("0.4.51",value.firmware)
        assertTrue(StorageContinuationCheck.retired(value))
        val capacity=PendantStoragePresentation.from(value)!!
        assertEquals(170L*1024*1024,capacity.totalBytes)
        assertEquals(979L*17*2048,capacity.occupiedBytes)
        assertEquals(4141L*17*2048,capacity.availableBytes)
        assertEquals(capacity.totalBytes,capacity.occupiedBytes+capacity.availableBytes)
        reject(p.copyOf().also { it[54]=31 })
        reject(p.copyOf().also { it[59]=19 })
    }
    @Test fun unknownFirmwareStillCannotAdvertiseKnownFullChipCapacity() {
        for (version in listOf(28,47,48,64,99,255)) {
            val p=packet();p[16]=version.toByte();p[53]=23;p[54]=32
            p[56]=979.toByte();p[57]=(979 shr 8).toByte();p[58]=0;p[59]=20
            reject(p)
        }
    }
    @Test fun streamProgress52PreservesCapacityAndStrictGeometryChecks() {
        val p=packet();p[16]=52;p[53]=23;p[54]=32
        p[56]=979.toByte();p[57]=(979 shr 8).toByte();p[58]=0;p[59]=20
        val value=DeviceTelemetry.parse(p)
        assertEquals("0.4.52",value.firmware)
        assertTrue(StorageContinuationCheck.retired(value))
        val capacity=PendantStoragePresentation.from(value)!!
        assertEquals(170L*1024*1024,capacity.totalBytes)
        assertEquals(979L*17*2048,capacity.occupiedBytes)
        assertEquals(4141L*17*2048,capacity.availableBytes)
        reject(p.copyOf().also { it[54]=31 })
        reject(p.copyOf().also { it[59]=19 })
    }
    @Test fun hostTx53PreservesCapacityAndStrictGeometryChecks() {
        val p=packet();p[16]=53;p[53]=23;p[54]=32
        p[56]=979.toByte();p[57]=(979 shr 8).toByte();p[58]=0;p[59]=20
        val value=DeviceTelemetry.parse(p)
        assertEquals("0.4.53",value.firmware)
        assertTrue(StorageContinuationCheck.retired(value))
        val capacity=PendantStoragePresentation.from(value)!!
        assertEquals(170L*1024*1024,capacity.totalBytes)
        assertEquals(979L*17*2048,capacity.occupiedBytes)
        assertEquals(4141L*17*2048,capacity.availableBytes)
        reject(p.copyOf().also { it[54]=31 })
        reject(p.copyOf().also { it[59]=19 })
    }
    @Test fun deviceSettings54ConnectsWithTheKnownStorageStatus() {
        val p=packet();p[16]=54;p[53]=23;p[54]=32
        p[56]=975.toByte();p[57]=(975 shr 8).toByte();p[58]=0;p[59]=20
        val value=DeviceTelemetry.parse(p)
        assertEquals("0.4.54",value.firmware)
        assertTrue(StorageContinuationCheck.retired(value))
        val capacity=PendantStoragePresentation.from(value)!!
        assertEquals(170L*1024*1024,capacity.totalBytes)
        assertEquals(975L*17*2048,capacity.occupiedBytes)
        assertEquals(4145L*17*2048,capacity.availableBytes)
        reject(p.copyOf().also { it[54]=31 })
        reject(p.copyOf().also { it[59]=19 })
    }
    private fun packet()=ByteArray(72).also {
        it[0]=2;it[1]=72;it[2]=15;it[26]=-1
        it[14]=4;it[16]=27
        val flags=512+1+2+4+8+16+32;it[48]=flags.toByte();it[49]=(flags shr 8).toByte()
        it[50]=5;it[51]=1;it[53]=8;it[54]=8;it[56]=21;it[58]=22
        OpProtocol.put32(it,60,946560);OpProtocol.put32(it,64,946560)
    }
    private fun reject(bytes:ByteArray) {
        try { DeviceTelemetry.parse(bytes);fail("Bad status accepted") } catch(_:ProtocolException) { }
    }
    @Test fun minuteProgressAndRootFullDespiteFreeAudioSlot() {
        val value=DeviceTelemetry.parse(packet());val r=value.recorder!!
        assertEquals("0.4.27",value.firmware);assertEquals(946560L,r.capturedSamples)
        assertTrue(r.full);assertEquals(1,r.slotsTotal-r.slotsUsed)
        assertEquals("Recording storage full",r.storageSummary)
        assertEquals("Recording stopped",r.activitySummary)
        assertTrue(r.details.contains("saved 59s"));assertTrue(r.details.contains("not the full memory capacity"))
    }
    @Test fun activeRecorderNeverAdvertisesIdleCapacity() {
        val p=packet();val flags=512+1+2+4+32+64+128
        p[48]=flags.toByte();p[49]=(flags shr 8).toByte();p[50]=3;p[51]=0
        for(i in listOf(53,54,56,57,58,59))p[i]=0
        val r=DeviceTelemetry.parse(p).recorder!!
        assertEquals("Recording active",r.activitySummary);assertFalse(r.capacityKnown);assertFalse(r.full)
        p[50]=7;assertEquals("Finalizing recording",DeviceTelemetry.parse(p).recorder!!.activitySummary)
        p[48]=(flags or 16).toByte();reject(p)
    }
    @Test fun fullProfileRequiresExactNewFirmwareAndGeometry() {
        val p=packet();p[16]=29;p[53]=1;p[54]=32
        p[56]=6;p[57]=0;p[58]=0;p[59]=20
        val value=DeviceTelemetry.parse(p)
        assertEquals(5120,value.recorder!!.slotsTotal)
        assertEquals(32,value.recorder!!.rootsTotal)
        assertFalse(value.recorder!!.full)
        for (version in listOf(30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,49)) {
            val fixed=DeviceTelemetry.parse(p.copyOf().also { it[16]=version.toByte() })
            assertEquals("0.4.$version",fixed.firmware)
            assertEquals(170L*1024*1024,PendantStoragePresentation.from(fixed)!!.totalBytes)
        }
        reject(p.copyOf().also { it[16]=27 })
        reject(p.copyOf().also { it[16]=99 })
        reject(p.copyOf().also { it[59]=19 })
        reject(p.copyOf().also { it[54]=31 })
    }
    @Test fun batchFirmwareHandshakeAndFullCapacityAreAcceptedTogether() {
        val info=byteArrayOf(0,1,0,0,0,0,0,0);OpProtocol.put32(info,2,3743)
        assertEquals(3743L,DurableBleCodec.validatedCapabilities(info))
        val p=packet();p[16]=40;p[53]=12;p[54]=32
        p[56]=465.toByte();p[57]=(465 shr 8).toByte();p[58]=0;p[59]=20
        val status=DeviceTelemetry.parse(p)
        assertTrue(StorageContinuationCheck.retired(status))
        val storage=PendantStoragePresentation.from(status)!!
        assertEquals(170L*1024*1024,storage.totalBytes)
        assertEquals(465L*17*2048,storage.occupiedBytes)
        assertEquals(storage.totalBytes,storage.occupiedBytes+storage.availableBytes)
    }
    @Test fun unavailableMeansUnknownNotZeroSpace() {
        val p=packet();p.fill(0,48);p[49]=2
        val r=DeviceTelemetry.parse(p).recorder!!
        assertFalse(r.capacityKnown);assertFalse(r.full);assertFalse(r.workerKnown)
        assertEquals("Not checked this boot",r.storageSummary)
        assertTrue(r.details.contains("USB is not connected"))
        for(at in listOf(50,51,52,53,54,56,57,58,59,60,64))reject(p.copyOf().also { it[at]=1 })
    }
    @Test fun malformedAndInconsistentExtendedStatusRejected() {
        for(size in 0..71)reject(packet().copyOf(size))
        reject(packet()+0)
        for(at in listOf(55,68,69,70,71))reject(packet().also { it[at]=1 })
        reject(packet().also { it[49]=6 });reject(packet().also { it[49]=0 })
        reject(packet().also { it[50]=8 });reject(packet().also { it[51]=13 });reject(packet().also { it[52]=2 })
        reject(packet().also { it[53]=9 });reject(packet().also { it[54]=0 });reject(packet().also { it[56]=23 })
        reject(packet().also { OpProtocol.put32(it,64,946880) })
        reject(packet().also { OpProtocol.put32(it,60,946561) })
    }
    @Test fun fullBodyFitsExistingNegotiatedMtuAndV1StaysSupported() {
        val body=packet();val op=byteArrayOf(79,80,1,0xa0.toByte(),1,0,73,0,0)+body
        assertEquals(81,op.size);assertArrayEquals(body,OpProtocol.response(op,OpProtocol.DEVICE_STATUS,1))
        val legacy=body.copyOf(48).also { it[0]=1;it[1]=48 }
        assertNull(DeviceTelemetry.parse(legacy).recorder)
    }
}
