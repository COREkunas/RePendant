package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.util.UUID
import java.util.concurrent.CancellationException

/** PUBLIC RFC recipient + actual C sender/Opus fixture. JVM decoder is injected
 * synthetic PCM; this proves HPKE-before-decoder, not actual audio rendering. */
class RecordingPlaybackAuthenticationTest {
    private fun uuid(n:Int)=ByteBuffer.wrap(ByteArray(16){(n+it).toByte()}).run{UUID(long,long)}
    private val id=DurableRecordingId(RecordingVolume(uuid(17),uuid(33),7),uuid(49))
    private fun fixture()=hexBytes(javaClass.getResourceAsStream("/recording_pipeline_public_container.hex")!!.use{it.readBytes().toString(Charsets.US_ASCII).trim()})
    private fun key()=RecipientRecoveryCodec.importP256(hexBytes("317f915db7bc629c48fe765587897e01e282d3e8445f79f27f65d031a88082b2"),
        hexBytes("04abc7e49a4c6b3566d77d0304addc6ed0e98512ffccf505e6a8e3eb25c685136f853148544876de76c0f2ef99cdc3a05ccf5ded7860c7c021238f9e2073d2356c"))
    @Test fun realAuthenticationPrecedesAnyDecoderOrSinkAndCiphertextCopiesAreWiped() {
        for(mode in 0..3) {
            val bytes=fixture()
            assertEquals("7d183119eac45283aaeaaabc207be0a09fc97c0b737d4ed1cf0f6671c5eb36d9",digestHex(bytes))
            if(mode==1)bytes[bytes.lastIndex]=(bytes.last().toInt() xor 1).toByte()
            if(mode==2)bytes[128]=2
            val recording=if(mode==3)id.copy(volume=id.volume.copy(generation=8)) else id
            // Even a catalog digest of a tampered frame cannot substitute for its tag.
            val segment=SegmentIdentity(recording,0,digestHex(bytes),bytes.size.toLong())
            val manifest=RecordingManifest(recording,1,true,"ab".repeat(32),listOf(segment))
            var snapshot=RecordingSyncSnapshot(recording,manifest=manifest,phoneSegments=setOf(segment))
            val core=RecordingSyncContract(snapshot,RecordingSyncOwnership()){_,n->snapshot=n}
            var decoded=0;var started=0;var closed=0
            val sink=object:RecordingPcmSink {
                override fun start(){started++}
                override fun write(pcm16le:ByteArray,offset:Int,length:Int)=length
                override fun pendingFrames()=0L
                override fun close(){closed++}
            }
            val job=RecordingPlayback(core,manifest,PlaybackCiphertextSource{bytes},PlaybackKeyAccess { action->key().use(action) },
                OpusPacketDecoder{decoded++;ShortArray(it.size/60*320)},sink,RecordingPlaybackRegistry())
            if(mode==0)assertEquals(960L,job.run().renderedSamples)
            else assertThrows(EncryptedSegmentException::class.java){job.run()}
            assertEquals(if(mode==0)1 else 0,decoded);assertEquals(decoded,started);assertEquals(1,closed)
            assertTrue(bytes.all{it==0.toByte()});assertTrue(snapshot.pendingReceipts.isEmpty());core.close()
        }
    }
    @Test fun absentOrFailingKeyProviderNeverReadsCiphertext() {
        for(afterCallback in listOf(false,true)) {
            val frame=fixture();val segment=SegmentIdentity(id,0,digestHex(frame),frame.size.toLong())
            val manifest=RecordingManifest(id,1,true,"ab".repeat(32),listOf(segment))
            val core=RecordingSyncContract(RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=setOf(segment)),RecordingSyncOwnership()){_,_->}
            var reads=0;var decodes=0
            val keys=PlaybackKeyAccess { action -> if(afterCallback){key().use(action);throw IllegalStateException("Synthetic key close failure")} }
            val sink=object:RecordingPcmSink {
                override fun start(){fail("No output")}
                override fun write(pcm16le:ByteArray,offset:Int,length:Int)=error("No output")
                override fun pendingFrames()=0L
                override fun close(){}
            }
            assertThrows(IllegalStateException::class.java){RecordingPlayback(core,manifest,
                PlaybackCiphertextSource{reads++;frame.copyOf()},keys,OpusPacketDecoder{decodes++;ShortArray(0)},sink,RecordingPlaybackRegistry()).run()}
            assertEquals(if(afterCallback)1 else 0,reads);assertEquals(0,decodes);core.close()
        }
    }
    @Test fun cancellationWhileObtainingKeyDoesNotReadOrDecode() {
        val frame=fixture();val segment=SegmentIdentity(id,0,digestHex(frame),frame.size.toLong())
        val manifest=RecordingManifest(id,1,true,"ab".repeat(32),listOf(segment))
        val core=RecordingSyncContract(RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=setOf(segment)),RecordingSyncOwnership()){_,_->}
        var reads=0;var closed=0
        val sink=object:RecordingPcmSink {
            override fun start(){fail("No output")}
            override fun write(pcm16le:ByteArray,offset:Int,length:Int)=error("No output")
            override fun pendingFrames()=0L
            override fun close(){closed++}
        }
        lateinit var job:RecordingPlayback
        job=RecordingPlayback(core,manifest,PlaybackCiphertextSource{reads++;frame.copyOf()},
            PlaybackKeyAccess{action->job.cancel();key().use(action)},OpusPacketDecoder{error("No decode")},sink,RecordingPlaybackRegistry())
        assertThrows(CancellationException::class.java){job.run()};assertEquals(0,reads);assertTrue(closed>0);core.close()
    }
}
