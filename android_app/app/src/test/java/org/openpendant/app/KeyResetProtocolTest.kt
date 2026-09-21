package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.security.MessageDigest
import java.util.UUID

class KeyResetProtocolTest {
    private val point="046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"
    private val fingerprint=RecipientRecoveryCodec.fingerprint(hexBytes(point)).joinToString(""){"%02x".format(it.toInt() and 255)}
    private val old=DurablePublicBinding("C6:05:04:03:02:01",RecordingVolume(UUID(1,2),UUID(3,4),2),"12".repeat(32))
    private val next=old.copy(volume=old.volume.copy(volumeId=UUID(5,6),generation=3),recipientFingerprint=fingerprint)
    private fun plan()=KeyResetProtocol.Plan(old,"34".repeat(32),next,point)
    private fun refused(run:()->Unit){try{run();fail("Must refuse")}catch(_:IllegalArgumentException){}catch(_:IllegalStateException){}}
    @Test fun exactPublicIntentRoundTrip(){val p=plan();assertEquals(400,KeyResetProtocol.encode(p).size);assertEquals(p,KeyResetProtocol.decode(KeyResetProtocol.encode(p)))}
    @Test fun everyByteAndLengthProtected(){val bytes=KeyResetProtocol.encode(plan());for(i in bytes.indices){val b=bytes.copyOf();b[i]=(b[i].toInt() xor 1).toByte();refused{KeyResetProtocol.decode(b)}};refused{KeyResetProtocol.decode(bytes.copyOf(399))};refused{KeyResetProtocol.decode(bytes.copyOf(401))}}
    @Test fun rehashedReservedBytesRefused(){val b=KeyResetProtocol.encode(plan());b[363]=1;MessageDigest.getInstance("SHA-256").digest(b.copyOf(368)).copyInto(b,368);refused{KeyResetProtocol.decode(b)}}
    @Test fun oldCommandsCannotAddressNewVolume(){for(b in listOf(next.copy(volume=next.volume.copy(generation=2)),next.copy(volume=next.volume.copy(generation=4)),next.copy(volume=next.volume.copy(volumeId=old.volume.volumeId)),next.copy(volume=next.volume.copy(deviceId=UUID(9,8))),next.copy(bondAddress="C6:05:04:03:02:02"),next.copy(recipientFingerprint=old.recipientFingerprint)))refused{KeyResetProtocol.Plan(old,"34".repeat(32),b,point)}}
    @Test fun keyMustMatchPoint(){refused{plan().copy(point="04"+"00".repeat(64))};refused{plan().copy(next=next.copy(recipientFingerprint="56".repeat(32)))}}
    @Test fun boundedFixedMutations(){val wire=KeyResetProtocol.Command.Prepare(plan()).wire;assertTrue(wire.length<384);assertTrue(wire.endsWith(" delete-pendant-recordings"));assertFalse(wire.contains("\n"));refused{KeyResetProtocol.Command.Erase("12\npendant restart confirm")};assertEquals("recorder keyerase ${"78".repeat(32)} delete-pendant-recordings",KeyResetProtocol.Command.Erase("78".repeat(32)).wire)}
    @Test fun strictStatusWithOptionalParent(){
        val line="RECORDER_KEY_RESET v=1 phase=1 reset=1 busy=0 fault=0 fresh=1\nRECORDER_KEY_DESCRIPTOR sha=${"56".repeat(32)}\nRECORDER_KEY_PARENT sha=${"34".repeat(32)}\n"
        val s=KeyResetProtocol.state(line);assertEquals(1,s.phase);assertTrue(s.reset&&s.fresh&&!s.busy&&!s.fault)
        for(b in listOf(line+line,line.replace("phase=1","phase=0"),line.replace("v=1","v=2"),line.substringBefore("RECORDER_KEY_PARENT"),line.replace("reset=1","reset=0"),line+"\u0000"))refused{KeyResetProtocol.state(b)}
        val original=line.replace("reset=1","reset=0").substringBefore("RECORDER_KEY_PARENT");assertNull(KeyResetProtocol.state(original).parent)
    }
    @Test fun childNeedsExactTransactionAndIdleState(){
        val p=plan();val s=KeyResetProtocol.State(1,true,false,false,true,"56".repeat(32),p.oldDescriptor)
        val storage=PhoneMigrationProtocol.Storage(next,1,"78".repeat(32),point);p.checkChild(storage,s)
        for(b in listOf(s.copy(busy=true),s.copy(fault=true),s.copy(reset=false),s.copy(parent="90".repeat(32)),s.copy(phase=3)))refused{p.checkChild(storage,b)}
        refused{p.checkChild(storage.copy(binding=old),s)}
    }
}
