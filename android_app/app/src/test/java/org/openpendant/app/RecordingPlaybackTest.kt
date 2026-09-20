package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID
import java.util.concurrent.CancellationException

/** Synthetic layouts/PCM only. The internal loader seam does NOT prove HPKE;
 * real fixture authentication has a separate test through the public entry. */
class RecordingPlaybackTest {
    @Test fun streamedOutputBoundCoversTheFullVolumeWithoutIntegerOverflow() {
        val limit=RecordingPlayback.MAX_OUTPUT_SAMPLES
        assertEquals(819_200_000L,limit)
        RecordingPlayback.requireOutputRoom(2L*60*60*16000,320)
        RecordingPlayback.requireOutputRoom(limit-160000,160000)
        RecordingPlayback.requireOutputRoom(limit-1,1)
        RecordingPlayback.requireOutputRoom(limit,0)
        for((rendered,samples) in listOf(limit to 1,limit-1 to 2,Long.MAX_VALUE to 320,
            -1L to 320,0L to -1,0L to Int.MAX_VALUE)) {
            assertThrows(RecordingPlaybackException::class.java) { RecordingPlayback.requireOutputRoom(rendered,samples) }
        }
    }
    private val id = DurableRecordingId(RecordingVolume(UUID(1,2), UUID(3,4),7), UUID(5,6))
    private fun plain(first: Long, gap: Boolean = false, celt: Boolean = false): ByteArray {
        val name=if(celt) "/recording_pipeline_celt_public.hex" else "/recording_pipeline_public.hex"
        val bytes=hexBytes(javaClass.getResourceAsStream(name)!!.use { it.readBytes().toString(Charsets.US_ASCII).trim() })
        fun put(at:Int,value:Long,n:Int) { repeat(n) { bytes[at+it]=(value ushr(it*8)).toByte() } }
        put(32,first,8);put(40,first+960,8);put(28,if(gap)3 else 1,4)
        return bytes
    }
    private inner class Setup(val data:List<ByteArray>) {
        val segments=data.mapIndexed { i,b -> SegmentIdentity(id,i,digestHex(b),b.size+209L) }
        val manifest=RecordingManifest(id,3,true,"ab".repeat(32),segments)
        var state=RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=segments.toSet(),pendantCopy=PendantCopy.PRESENT)
        val core=RecordingSyncContract(state,RecordingSyncOwnership()) { rev,next -> assertEquals(state.revision,rev);state=next }
        val registry=RecordingPlaybackRegistry()
        var time=0L;var loads=0;var decodes=0
        val retained=mutableListOf<ByteArray>();val decoded=mutableListOf<ShortArray>()
        val sink=Sink()
        var onLoad:()->Unit={};var onDecode:()->Unit={};var wait:()->Unit={ time+=5 }
        fun job(policy:RecordingGapPolicy=RecordingGapPolicy.PAUSE_AT_GAP)=RecordingPlayback(core,manifest,{ segment ->
            assertEquals(segments[segment.sequence],segment);loads++;onLoad()
            data[segment.sequence].copyOf().also { retained+=it }
        },OpusPacketDecoder { packets ->
            decodes++;onDecode();ShortArray(packets.size/60*320) { 17 }.also { decoded+=it }
        },sink,policy,{time},{wait()},Unit,registry)
    }
    private class Sink:RecordingPcmSink {
        var starts=0;var closes=0;var writes=0;var bytes=0;var accept=640;var pending=0L
        var onWrite:()->Unit={};var onStart:()->Unit={};var onClose:()->Unit={};val held=mutableListOf<ByteArray>()
        override fun start() { starts++;onStart() }
        override fun write(pcm16le:ByteArray,offset:Int,length:Int):Int {
            assertTrue(length in 2..640 && length%2==0);writes++;held+=pcm16le;onWrite()
            return minOf(accept,length).also { if(it>0)bytes+=it }
        }
        override fun pendingFrames()=pending
        override fun close() { if(closes==0)closes++;onClose() }
    }
    @Test fun threeSegmentsStreamPartialWritesAndAllOwnedPlaintextIsWiped() {
        val s=Setup(List(3){plain(it*960L)});s.sink.accept=128
        val result=s.job().run()
        assertEquals(RecordingPlaybackEnd.COMPLETED,result.end);assertEquals(2880L,result.renderedSamples)
        assertEquals(5760,s.sink.bytes);assertEquals(3,s.decodes);assertEquals(1,s.sink.starts);assertEquals(1,s.sink.closes)
        assertTrue(s.retained.all { b->b.all { it==0.toByte() } });assertTrue(s.decoded.all { b->b.all { it==0.toShort() } })
        assertTrue(s.sink.held.all { b->b.all { it==0.toByte() } });assertTrue(s.state.pendingReceipts.isEmpty())
        assertFalse(s.state.downloadSuppressed)
    }
    @Test fun seekOnlyAtBoundaryAuthenticatesEarlierMetadataWithoutDecodingIt() {
        val s=Setup(List(3){plain(it*960L)});val result=s.job().run(2)
        assertEquals(3,s.loads);assertEquals(1,s.decodes);assertEquals(960L,result.renderedSamples)
        val bad=Setup(listOf(plain(0),plain(100),plain(1920)))
        assertThrows(IllegalArgumentException::class.java){bad.job().run(2)};assertEquals(0,bad.decodes)
    }
    @Test fun seekWithinSegmentAuthenticatesPrefixAndTrimsOnlyDecodedOutput() {
        val s=Setup(List(3){plain(it*960L)})
        val positions=mutableListOf<Long>()
        val result=s.job().run(1,480) { positions+=it }
        assertEquals(3,s.loads);assertEquals(2,s.decodes)
        assertEquals(1440L,result.renderedSamples);assertEquals(2880,s.sink.bytes)
        assertEquals(2880L,positions.last())
        assertTrue(positions.all { it in 1440..2880 })
        assertTrue(s.sink.held.all { bytes->bytes.all { it==0.toByte() } })
    }
    @Test fun invalidIntraSegmentOffsetNeverStartsAudioAndSeekDoesNotBypassGap() {
        for(offset in listOf(-1,960,Int.MAX_VALUE)) {
            val s=Setup(listOf(plain(0)))
            assertThrows(IllegalArgumentException::class.java) { s.job().run(0,offset) }
            assertEquals(0,s.sink.starts)
        }
        val gap=Setup(listOf(plain(0),plain(1280,true)))
        assertEquals(RecordingPlaybackEnd.PAUSED_AT_GAP,gap.job().run(1,320).end)
        assertEquals(0,gap.sink.starts)
    }
    @Test fun gapDefaultsToPauseAndExplicitSilencePreservesTimeline() {
        val paused=Setup(listOf(plain(0),plain(1280,true)))
        val result=paused.job().run();assertEquals(RecordingPlaybackEnd.PAUSED_AT_GAP,result.end)
        assertEquals(1,result.nextSequence);assertEquals(960L,result.renderedSamples);assertEquals(1,paused.decodes)
        val inserted=Setup(listOf(plain(0),plain(1280,true)))
        val full=inserted.job(RecordingGapPolicy.INSERT_BOUNDED_SILENCE).run()
        assertEquals(2240L,full.renderedSamples);assertEquals(320L,full.insertedSilenceSamples)
        val start=Setup(listOf(plain(1234,true)))
        assertEquals(RecordingPlaybackEnd.PAUSED_AT_GAP,start.job().run().end);assertEquals(0,start.sink.starts)
    }
    @Test fun invalidGapProfileOverlapAndHugeGapNeverDecodeAffectedSegment() {
        for(second in listOf(plain(960,true),plain(961),plain(900),plain(960,celt=true),plain(960+960001,true))) {
            val s=Setup(listOf(plain(0),second))
            assertThrows(IllegalStateException::class.java) {
                try { s.job(RecordingGapPolicy.INSERT_BOUNDED_SILENCE).run() }
                catch(e:IllegalArgumentException) { throw IllegalStateException("Synthetic invalid layout",e) }
            }
            assertEquals(1,s.decodes);assertEquals(1,s.sink.closes)
        }
    }
    @Test fun cancellationBeforeReadDuringDecodeAndDuringOutputStopsAndWipes() {
        for(point in 0..2) {
            val s=Setup(listOf(plain(0),plain(960)));val job=s.job()
            when(point) { 0->job.cancel();1->s.onDecode={job.cancel()};2->s.sink.onWrite={job.cancel()} }
            assertThrows(CancellationException::class.java){job.run()}
            assertEquals(if(point==0)0 else 1,s.loads);assertEquals(1,s.sink.closes)
            assertTrue(s.retained.all { b->b.all { it==0.toByte() } });assertTrue(s.decoded.all { b->b.all { it==0.toShort() } })
            assertThrows(IllegalStateException::class.java){job.run()}
        }
    }
    @Test fun deletionDuringDecodeInvalidatesPlaybackAndNeverRestoresSuppression() {
        val s=Setup(listOf(plain(0)));val job=s.job()
        s.onDecode={ s.core.requestDeletion(UUID(7,8),DeleteLocation.BOTH,true) }
        assertThrows(CancellationException::class.java){job.run()}
        assertEquals(0,s.sink.starts);assertTrue(s.state.downloadSuppressed)
        assertEquals(PendantDeletion.PENDING,s.state.deletions.single().pendant)
        assertTrue(s.state.pendingReceipts.isEmpty())
    }
    @Test fun stalledSinkDrainInvalidCountsAndBackwardClockFailBoundedly() {
        for(mode in 0..3) {
            val s=Setup(listOf(plain(0)))
            when(mode) { 0->s.sink.accept=0;1->s.sink.pending=320;2->s.sink.accept=1;3->s.sink.onWrite={s.time = -1} }
            assertThrows(RecordingPlaybackException::class.java){s.job().run()}
            assertTrue(s.time<=5000);assertEquals(1,s.sink.closes);assertFalse(s.core.requiresReconciliation())
        }
    }
    @Test fun finalizedFullManifestPresenceAndGenerationAreMandatory() {
        val s=Setup(listOf(plain(0),plain(960)))
        val altered=RecordingManifest(id,4,true,"cd".repeat(32),s.segments)
        assertThrows(IllegalArgumentException::class.java){s.core.beginPlayback(altered)}
        assertThrows(IllegalArgumentException::class.java){s.core.beginPlayback(s.manifest,2)}
        val ticket=s.core.beginPlayback(s.manifest)
        s.core.authenticatedConnection(id.volume.copy(generation=8),true)
        assertFalse(s.core.isCurrent(ticket));var calls=0
        assertFalse(s.core.playbackStep(ticket){calls++});assertEquals(0,calls)
        val partial=RecordingSyncContract(s.state.copy(phoneSegments=setOf(s.segments[0])),RecordingSyncOwnership()){_,_->}
        assertThrows(IllegalArgumentException::class.java){partial.beginPlayback(s.manifest)}
    }
    @Test fun sinkCallbackCannotReenterDeletionOrPublishMetadata() {
        val s=Setup(listOf(plain(0)))
        s.sink.onWrite={s.core.requestDeletion(UUID(7,8),DeleteLocation.PHONE_ONLY)}
        assertThrows(IllegalStateException::class.java){s.job().run()}
        assertTrue(s.state.deletions.isEmpty());assertFalse(s.state.downloadSuppressed)
    }
    @Test fun typedDeletionAlwaysStopsPlaybackAndAudioButRetainsOnlyTranscript() {
        for(keep in listOf(false,true)) {
            val s=Setup(listOf(plain(0)));val calls=mutableListOf<String>()
            val registration=s.registry.register(s.manifest){calls+="stop"}
            s.core.requestDeletion(UUID(7,8),DeleteLocation.BOTH,keep)
            val plan=PhoneDeletionPlan(s.state,UUID(7,8),s.manifest.sha256)
            RecordingDerivativeDeletion(s.registry,object:RecordingDiskDerivatives {
                override fun removeAudioAndSync(plan:PhoneDeletionPlan){assertEquals(id,plan.recording);calls+="audio"}
                override fun removeTranscriptAndSync(plan:PhoneDeletionPlan){calls+="transcript"}
            }).removeAndSync(plan)
            assertEquals(if(keep)listOf("stop","audio") else listOf("stop","audio","transcript"),calls)
            assertTrue(s.state.downloadSuppressed);registration.close()
        }
    }
    @Test fun unavailableDerivativeMappingFailsRatherThanGuessLegacyPaths() {
        val s=Setup(listOf(plain(0)));s.core.requestDeletion(UUID(7,8),DeleteLocation.PHONE_ONLY,true)
        var transcript=0
        val mapper=RecordingDerivativeDeletion(s.registry,object:RecordingDiskDerivatives {
            override fun removeAudioAndSync(plan:PhoneDeletionPlan){throw PhoneDeletionStorageException()}
            override fun removeTranscriptAndSync(plan:PhoneDeletionPlan){transcript++}
        })
        val plan=PhoneDeletionPlan(s.state,UUID(7,8),s.manifest.sha256)
        assertThrows(PhoneDeletionStorageException::class.java){mapper.removeAndSync(plan)}
        assertEquals(0,transcript);assertTrue(s.state.deletions.single().phonePending)
    }
    @Test fun allocatedStartFailureAlwaysClosesAndReleasesTicket() {
        val s=Setup(listOf(plain(0)));s.sink.onStart={throw IllegalStateException("Synthetic allocation failure")}
        assertThrows(IllegalStateException::class.java){s.job().run()}
        assertEquals(1,s.sink.starts);assertEquals(1,s.sink.closes);assertEquals(0,s.sink.writes)
        assertTrue(s.retained.all{b->b.all{it==0.toByte()}})
        assertNotNull(s.core.beginPlayback(s.manifest))
    }
    @Test fun platformStartOutsideCoordinatorAllowsDeletionButNoPcmAfterwards() {
        val s=Setup(listOf(plain(0)))
        s.sink.onStart={s.core.requestDeletion(UUID(7,8),DeleteLocation.PHONE_ONLY)}
        assertThrows(CancellationException::class.java){s.job().run()}
        assertEquals(0,s.sink.writes);assertEquals(1,s.sink.closes);assertTrue(s.state.downloadSuppressed)
    }
    @Test fun uncertainSinkCleanupFencesAllLaterOutputOnSharedRegistry() {
        val s=Setup(listOf(plain(0)));s.sink.onClose={throw IllegalStateException("Synthetic release failure")}
        assertThrows(IllegalStateException::class.java){s.job().run()}
        s.sink.onClose={}
        assertThrows(RecordingPlaybackException::class.java){s.job().run()}
        assertEquals(1,s.loads)
    }
}
