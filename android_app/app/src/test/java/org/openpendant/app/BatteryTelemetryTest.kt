package org.openpendant.app
import org.junit.Assert.*
import org.junit.Test

class BatteryTelemetryTest {
    private fun word(p:ByteArray,at:Int,v:Int){p[at]=v.toByte();p[at+1]=(v ushr 8).toByte()}
    private fun packet()=ByteArray(72).also {
        it[0]=3;it[1]=72;it[2]=31;it[14]=4;it[16]=46;it[26]=80;it[27]=7
        word(it,28,4100);word(it,30,2981);OpProtocol.put32(it,32,10000)
        OpProtocol.put32(it,36,1);word(it,40,8);word(it,48,512)
    }
    private fun reject(p:ByteArray){try{DeviceTelemetry.parse(p);fail("invalid accepted")}catch(_:ProtocolException){}}
    @Test fun parsesProductionWireAndGaugeEstimate(){
        val d=DeviceTelemetry.parse(packet());val b=checkNotNull(d.battery)
        assertEquals("0.4.46",d.firmware);assertEquals(80,b.percent);assertEquals(4100,b.millivolts)
        assertEquals(2981,b.temperatureDecikelvin);assertTrue(b.startPowerReady&&b.portable&&b.valid)
        assertFalse(b.stopped);assertEquals(10000,b.ageMs);assertEquals(1,b.sequence)
    }
    @Test fun recoveredGauge49AndMountedFullStorageParseTogether(){
        val p=packet().also {
            it[16]=49;it[26]=97;word(it,28,4290);word(it,40,0x1c8)
            word(it,48,512+1+2+4+8+16+32);it[50]=5;it[51]=1;it[53]=19;it[54]=32
            word(it,56,546);word(it,58,5120)
            OpProtocol.put32(it,60,960000);OpProtocol.put32(it,64,960000)
        }
        val value=DeviceTelemetry.parse(p)
        assertEquals("0.4.49",value.firmware);assertTrue(value.battery!!.startPowerReady)
        val storage=checkNotNull(PendantStoragePresentation.from(value))
        assertEquals(170L*1024*1024,storage.totalBytes)
        assertEquals(546L*17*2048,storage.occupiedBytes)
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=50 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=51 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=52 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=53 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=54 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=55 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=56 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=57 })))
        // Same capacity and battery data with USB absent.
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=57;word(it,48,512+2+4+8+16+32) })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=58 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=59 })))
        assertEquals(storage,PendantStoragePresentation.from(DeviceTelemetry.parse(p.copyOf().also { it[16]=60 })))
        reject(p.copyOf().also { it[16]=61 })
        reject(p.copyOf().also { word(it,40,0x1e8) })
        reject(p.copyOf().also { word(it,58,5119) })
    }
    @Test fun ageIncludesGaugeAndPhoneTimeWithImmediateDisconnectExpiry(){
        val t=TimedDeviceTelemetry(DeviceTelemetry.parse(packet()),100)
        assertNotNull(t.freshBattery(10100,true));assertNull(t.freshBattery(10101,true))
        assertNull(t.freshBattery(100,false));assertNull(t.freshBattery(99,true))
        val older=packet().also{OpProtocol.put32(it,32,18000)}
        val s=TimedDeviceTelemetry(DeviceTelemetry.parse(older),100)
        assertNotNull(s.freshBattery(2100,true));assertNull(s.freshBattery(2101,true))
    }
    @Test fun unknownBatteryNeverBecomesZeroPercent(){
        for(flags in listOf(4,12)){
            val p=packet().also{it[2]=15;it[26]=-1;it[27]=flags.toByte();for(i in 28..41)it[i]=0}
            val b=checkNotNull(DeviceTelemetry.parse(p).battery)
            assertNull(b.percent);assertNull(b.millivolts);assertFalse(b.valid||b.startPowerReady)
            assertEquals(flags==12,b.stopped)
            for(i in 28..43)reject(p.copyOf().also{it[i]=1})
        }
    }
    @Test fun rejectsUntrustedOrNoncanonicalSamples(){
        for(flags in 0..255)if(flags!=5&&flags!=7)reject(packet().also{it[27]=flags.toByte()})
        for((at,v) in listOf(28 to 0,28 to 6001,28 to 3799,28 to 4451,30 to 2780,30 to 3132,40 to 0,40 to 0x18,40 to 0x8008))
            reject(packet().also{word(it,at,v)})
        reject(packet().also{it[26]=101});reject(packet().also{it[26]=24})
        reject(packet().also{OpProtocol.put32(it,32,20001)})
        reject(packet().also{OpProtocol.put32(it,36,0)})
        reject(packet().also{it[2]=15});reject(packet().also{it[42]=1});reject(packet().also{it[43]=1})
    }
    @Test fun lowBatteryIsValidButNotStartReady(){
        val p=packet().also{it[27]=5;it[26]=15;word(it,28,3650)}
        val b=checkNotNull(DeviceTelemetry.parse(p).battery)
        assertTrue(b.valid);assertFalse(b.startPowerReady);assertEquals(15,b.percent)
    }
}
