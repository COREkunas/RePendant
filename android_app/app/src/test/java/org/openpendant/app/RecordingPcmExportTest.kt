package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.util.UUID

/** Public cryptographic fixture only. No owner key, microphone or audio output. */
class RecordingPcmExportTest {
    private fun uuid(n:Int)=ByteBuffer.wrap(ByteArray(16){(n+it).toByte()}).run{UUID(long,long)}
    private val id=DurableRecordingId(RecordingVolume(uuid(17),uuid(33),7),uuid(49))
    private fun fixture()=hexBytes(javaClass.getResourceAsStream("/recording_pipeline_public_container.hex")!!.use{it.readBytes().toString(Charsets.US_ASCII).trim()})
    private fun key()=RecipientRecoveryCodec.importP256(hexBytes("317f915db7bc629c48fe765587897e01e282d3e8445f79f27f65d031a88082b2"),
        hexBytes("04abc7e49a4c6b3566d77d0304addc6ed0e98512ffccf505e6a8e3eb25c685136f853148544876de76c0f2ef99cdc3a05ccf5ded7860c7c021238f9e2073d2356c"))
    @Test fun authenticatesBeforeDecodeBoundsPlaintextAndWipesAfterUse(){
        val frame=fixture();val segment=SegmentIdentity(id,0,digestHex(frame),frame.size.toLong())
        val manifest=RecordingManifest(id,1,true,"ab".repeat(32),listOf(segment))
        val core=RecordingSyncContract(RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=setOf(segment)),RecordingSyncOwnership()){_,_->}
        val read=mutableListOf<ByteArray>();val sent=mutableListOf<ByteArray>();var decoded=0
        RecordingPcmExport(core,manifest,PlaybackCiphertextSource{frame.copyOf().also(read::add)},PlaybackKeyAccess{action->key().use(action)},
            OpusPacketDecoder{decoded++;ShortArray(it.size/60*320){17}},{}).use { export ->
            assertEquals(1920L,export.measure());assertEquals(0,decoded)
            export.stream {offset,bytes->assertEquals(0L,offset);assertEquals(1920,bytes.size);assertTrue(bytes.any{it!=0.toByte()});sent+=bytes}
            assertEquals(1,decoded)
        }
        assertEquals(2,read.size);assertTrue(read.all {it.all{b->b==0.toByte()}});assertTrue(sent.all{it.all{b->b==0.toByte()}})
        assertTrue(core.snapshot().pendingReceipts.isEmpty());assertFalse(core.snapshot().downloadSuppressed)
        core.beginPlayback(manifest);core.close()
    }
    @Test fun corruptCiphertextFailsBeforeDecoderOrNetworkAndReleasesOwnership(){
        val frame=fixture().also{it[it.lastIndex]=(it.last().toInt() xor 1).toByte()}
        val segment=SegmentIdentity(id,0,digestHex(frame),frame.size.toLong());val manifest=RecordingManifest(id,1,true,"ab".repeat(32),listOf(segment))
        val core=RecordingSyncContract(RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=setOf(segment)),RecordingSyncOwnership()){_,_->}
        assertThrows(EncryptedSegmentException::class.java){RecordingPcmExport(core,manifest,PlaybackCiphertextSource{frame.copyOf()},
            PlaybackKeyAccess{action->key().use(action)},OpusPacketDecoder{error("Decoder must not run")},{}).use{it.measure()}}
        core.beginPlayback(manifest);core.close()
    }
    @Test fun pendingDeletionAndPartialPhoneCopiesRefuseBeforeKeyAccess(){
        val frame=fixture();val segment=SegmentIdentity(id,0,digestHex(frame),frame.size.toLong());val manifest=RecordingManifest(id,1,true,"ab".repeat(32),listOf(segment))
        for(complete in listOf(false,true)){
            val core=RecordingSyncContract(RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=if(complete)setOf(segment) else emptySet()),RecordingSyncOwnership()){_,_->}
            if(complete)core.requestDeletion(UUID(7,8),DeleteLocation.PHONE_ONLY)
            assertThrows(Exception::class.java){RecordingPcmExport(core,manifest,PlaybackCiphertextSource{error("No read")},PlaybackKeyAccess{error("No key")},OpusPacketDecoder{error("No decode")},{})}
            core.close()
        }
    }
    @Test fun deletionDuringExportInvalidatesTicketWithoutCoordinatorLockOnConsumer(){
        val frame=fixture();val segment=SegmentIdentity(id,0,digestHex(frame),frame.size.toLong());val manifest=RecordingManifest(id,1,true,"ab".repeat(32),listOf(segment))
        val core=RecordingSyncContract(RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=setOf(segment)),RecordingSyncOwnership()){_,_->}
        var output:ByteArray?=null
        RecordingPcmExport(core,manifest,PlaybackCiphertextSource{frame.copyOf()},PlaybackKeyAccess{action->key().use(action)},OpusPacketDecoder{ShortArray(it.size/60*320)},{})
            .use {export->export.measure();assertThrows(Exception::class.java){export.stream{_,bytes->output=bytes;core.requestDeletion(UUID(7,8),DeleteLocation.PHONE_ONLY)}}}
        assertTrue(output!!.all{it==0.toByte()});assertTrue(core.snapshot().downloadSuppressed);core.close()
    }
}
