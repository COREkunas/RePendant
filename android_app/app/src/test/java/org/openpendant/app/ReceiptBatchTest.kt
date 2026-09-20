package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

/** Public metadata only, no radio/files/keys. */
class ReceiptBatchTest {
    private val volume=RecordingVolume(UUID(1,2),UUID(3,4),2)
    private val id=DurableRecordingId(volume,UUID(5,6))
    private val nonce=UUID(7,8)
    private val connection=DurableSyncConnection(UUID(9,10),volume,"12".repeat(32))
    private val parts=List(40){SegmentIdentity(id,it,"ab".repeat(32),34357)}
    private val manifest=RecordingManifest(id,161,true,"cd".repeat(32),parts)

    @Test fun rangeWireFitsOldMtuAndEchoBindsEveryByte() {
        val r=DurableBleCodec.receiptRange(nonce,manifest,8,32)
        val p=r.payload();val frame=DurableBleCodec.encodeRequest(r.command,17,p)
        assertEquals(0x3e,r.command);assertEquals(80,frame.size)
        assertEquals(8L,OpProtocol.u32(p,32));assertEquals(32L,OpProtocol.u32(p,36))
        assertEquals(DurableReceiptRangeReply(connection.epoch,id,manifest.sha256,8,32),
            DurableBleCodec.receiptRangeBody(p,nonce,connection,manifest,8,32))
        for(i in p.indices){val bad=p.copyOf();bad[i]=(bad[i].toInt() xor 1).toByte()
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.receiptRangeBody(bad,nonce,connection,manifest,8,32)}}
        for(size in listOf(0,71,73))assertThrows(IllegalArgumentException::class.java){
            DurableBleCodec.receiptRangeBody(p.copyOf(size),nonce,connection,manifest,8,32)}
    }

    @Test fun rangeBoundsCapabilitiesAndFinalManifestAreExplicit() {
        for((first,count) in listOf(-1 to 1,0 to 0,0 to 33,39 to 2,Int.MAX_VALUE to 32))
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.receiptRange(nonce,manifest,first,count)}
        val open=RecordingManifest(id,160,false,manifest.sha256,parts)
        assertThrows(IllegalArgumentException::class.java){DurableBleCodec.receiptRange(nonce,open,0,1)}
        for(bits in listOf(1679L,1695L,3727L,3743L)){
            val info=byteArrayOf(0,1,0,0,0,0,0,0);OpProtocol.put32(info,2,bits)
            assertEquals(bits,DurableBleCodec.validatedCapabilities(info))
        }
        for(bits in listOf(15L or DurableBleCodec.RECEIPT_BATCH_CAPABILITY,3743L or (1L shl 20))){
            val info=byteArrayOf(0,1,0,0,0,0,0,0);OpProtocol.put32(info,2,bits)
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.validatedCapabilities(info)}
        }
        val p=DurableBleCodec.receiptRange(nonce,manifest,0,32).payload()
        for(count in listOf(0,33,34357,Int.MAX_VALUE)){
            val bad=p.copyOf();OpProtocol.put32(bad,36,count.toLong())
            assertThrows(IllegalArgumentException::class.java){DurableBleCodec.encodeRequest(0x3e,17,bad)}
        }
    }

    private inner class State(pending:Set<SegmentIdentity> = parts.toSet()) {
        var saved=RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=parts.toSet(),pendingReceipts=pending)
        var fail=false;var commits=0
        val core=RecordingSyncContract(saved,RecordingSyncOwnership()){rev,next->
            check(!fail);assertEquals(saved.revision,rev);saved=next;commits++
        }
        init{core.authenticatedConnection(volume,true)}
    }

    @Test fun batchStopsAtGapsAndCommitsAllIntentsOnce() {
        val s=State(setOf(parts[0],parts[1],parts[3],parts[4]))
        val a=s.core.nextReceiptBatch();assertEquals(listOf(0,1),a.map{it.segment.sequence})
        assertTrue(s.core.confirmReceipts(a));assertEquals(1,s.commits)
        val b=s.core.nextReceiptBatch();assertEquals(listOf(3,4),b.map{it.segment.sequence})
        assertTrue(s.core.confirmReceipts(b));assertEquals(2,s.commits)
        assertTrue(s.saved.pendingReceipts.isEmpty());assertEquals(parts.toSet(),s.saved.phoneSegments)
        assertFalse(s.core.confirmReceipts(a));s.core.close()
    }

    @Test fun staleReorderedDuplicateOrForeignReceiptNeverClearsAnyIntent() {
        for(mode in 0..5){
            val s=State();val batch=s.core.nextReceiptBatch();assertEquals(32,batch.size)
            val bad=when(mode){
                0->batch.reversed();1->listOf(batch.first(),batch.first());2->batch.drop(1).toMutableList().also{it.add(batch.first())}
                3->batch.map{SegmentReceipt(it.segment,UUID(90,91))}
                else->batch
            }
            if(mode==4)s.core.disconnected()
            if(mode==5)s.core.requestDeletion(UUID(11,12),DeleteLocation.PENDANT_ONLY)
            val before=s.saved
            assertFalse(s.core.confirmReceipts(bad));assertEquals(before,s.saved);s.core.close()
        }
    }

    @Test fun failedPhoneCommitKeepsPendingAndFencesFurtherWork() {
        val s=State();val batch=s.core.nextReceiptBatch();val before=s.saved;s.fail=true
        assertThrows(IllegalStateException::class.java){s.core.confirmReceipts(batch)}
        assertEquals(before,s.saved);assertTrue(s.core.requiresReconciliation())
        assertTrue(s.core.nextReceiptBatch().isEmpty());s.core.close()
    }
}
