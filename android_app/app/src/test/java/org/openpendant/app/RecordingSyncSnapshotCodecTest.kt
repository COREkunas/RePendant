package org.openpendant.app

import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class RecordingSyncSnapshotCodecTest {
    private val identity=DurableRecordingId(RecordingVolume(UUID(1,2),UUID(3,4),0x0102030405060708L),UUID(5,6))
    private val segments=List(3) { SegmentIdentity(identity,it,"ab".repeat(32),65745) }
    private val manifest=RecordingManifest(identity,6,true,"cd".repeat(32),segments)
    private val populated=RecordingSyncSnapshot(identity,12,9,manifest,PendantCopy.PRESENT,
        phoneSegments=segments.take(2).toSet(),pendingReceipts=setOf(segments[1]))

    @Test fun independentlyEncodedHistoricalMetadataCoversEveryHashByte() {
        val hashes=List(8) { group -> ByteArray(32) { (group*32+it).toByte() } }
        fun oldHex(bytes:ByteArray)=bytes.joinToString("") { java.lang.String.format(java.util.Locale.US,"%02x",it.toInt() and 255) }
        val entries=hashes.mapIndexed { index,hash -> SegmentIdentity(identity,index,oldHex(hash),34357) }
        val legacyManifest=RecordingManifest(identity,33,true,oldHex(hashes[7]),entries)
        val expected=RecordingSyncSnapshot(identity,12,9,legacyManifest,PendantCopy.PRESENT,
            phoneSegments=entries.toSet(),pendingReceipts=setOf(entries[7]))
        // Write the existing v1 layout independently, without the codec/helper.
        val body=java.io.ByteArrayOutputStream().also { buffer ->
            java.io.DataOutputStream(buffer).use { out ->
                out.write("OPNDSY1\u0000".toByteArray(Charsets.US_ASCII))
                for(value in listOf(1L,2L,3L,4L,0x0102030405060708L,5L,6L)) out.writeLong(value)
                out.writeLong(12);out.writeLong(9);out.writeBoolean(true)
                out.writeLong(33);out.writeBoolean(true);out.write(hashes[7]);out.writeInt(8)
                hashes.forEachIndexed { index,hash -> out.writeInt(index);out.write(hash);out.writeLong(34357) }
                out.writeByte(1);out.writeBoolean(false);out.writeBoolean(false)
                out.writeInt(8);repeat(8) { out.writeInt(it) };out.writeInt(1);out.writeInt(7);out.writeShort(0)
            }
        }.toByteArray()
        val legacy=body+MessageDigest.getInstance("SHA-256").digest(body)
        assertEquals(expected,RecordingSyncSnapshotCodec.decode(legacy))
        assertArrayEquals(legacy,RecordingSyncSnapshotCodec.encode(expected))
    }

    @Test fun emptyAndFullMetadataRoundTripWithExact64BitIdentity() {
        for(value in listOf(RecordingSyncSnapshot(identity),populated)) {
            val bytes=RecordingSyncSnapshotCodec.encode(value)
            assertEquals(value,RecordingSyncSnapshotCodec.decode(bytes))
            assertArrayEquals(bytes,RecordingSyncSnapshotCodec.encode(RecordingSyncSnapshotCodec.decode(bytes)))
        }
        assertEquals(126,RecordingSyncSnapshotCodec.encode(RecordingSyncSnapshot(identity)).size)
        assertEquals(56,RecordingSyncSnapshotCodec.identityBytes(identity).size)
    }

    @Test fun deleteSuppressionAndPendingRemoteIntentPersistWithoutSourceRemoval() {
        for(location in DeleteLocation.entries) {
            val intent=RecordingDeletionIntent(UUID(7,8),identity,manifest.sha256,location,true,
                location!=DeleteLocation.PENDANT_ONLY,
                if(location==DeleteLocation.PHONE_ONLY)PendantDeletion.NOT_REQUESTED else PendantDeletion.PENDING)
            val value=populated.copy(downloadSuppressed=location!=DeleteLocation.PENDANT_ONLY,deletions=listOf(intent),
                pendingReceipts=if(intent.phonePending)emptySet() else populated.pendingReceipts)
            val decoded=RecordingSyncSnapshotCodec.decode(RecordingSyncSnapshotCodec.encode(value))
            assertEquals(value,decoded);assertEquals(PendantCopy.PRESENT,decoded.pendantCopy)
        }
    }

    @Test fun allSingleByteCorruptionAndTruncationRejected() {
        val bytes=RecordingSyncSnapshotCodec.encode(populated)
        for(index in bytes.indices) {
            val changed=bytes.copyOf();changed[index]=(changed[index].toInt() xor 1).toByte()
            assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(changed) }
        }
        for(count in bytes.indices) assertThrows(IllegalArgumentException::class.java) {
            RecordingSyncSnapshotCodec.decode(bytes.copyOf(count))
        }
    }

    private fun checksum(bytes:ByteArray):ByteArray=bytes.copyOfRange(0,bytes.size-32).let {
        it+MessageDigest.getInstance("SHA-256").digest(it)
    }
    @Test fun checksumDoesNotReplaceFieldAndIdentityValidation() {
        val base=RecordingSyncSnapshotCodec.encode(RecordingSyncSnapshot(identity))
        for(index in listOf(0,80,81,82,83)) {
            val changed=base.copyOf();changed[index]=127
            assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(checksum(changed)) }
        }
        val zeroId=base.copyOf();zeroId.fill(0,8,24)
        assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(checksum(zeroId)) }
        val badGeneration=base.copyOf();ByteBuffer.wrap(badGeneration).putLong(40,0)
        assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(checksum(badGeneration)) }
        val largeCount=base.copyOf();ByteBuffer.wrap(largeCount).putInt(84,Int.MAX_VALUE)
        assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(checksum(largeCount)) }
    }

    @Test fun fullCatalogAndIntentLimitsAreBoundedAndCanonical() {
        val many=List(4096) { SegmentIdentity(identity,it,"12".repeat(32),65745) }
        val large=RecordingManifest(identity,6,true,"cd".repeat(32),many)
        val deletions=List(256) { RecordingDeletionIntent(UUID(8,it.toLong()+1),identity,large.sha256,
            DeleteLocation.PHONE_ONLY,false,false,PendantDeletion.NOT_REQUESTED) }
        val snapshot=populated.copy(manifest=large,downloadSuppressed=true,phoneSegments=many.toSet(),
            pendingReceipts=many.toSet(),deletions=deletions)
        val bytes=RecordingSyncSnapshotCodec.encode(snapshot)
        assertTrue(bytes.size<RecordingSyncSnapshotCodec.MAX_BYTES)
        assertEquals(snapshot,RecordingSyncSnapshotCodec.decode(bytes))
        // Historical completed operations may fill the retained outbox; only
        // ONE current phone operation is allowed, and it suppresses receipts.
        val pending=snapshot.copy(pendingReceipts=emptySet(),deletions=deletions.mapIndexed { index,intent ->
            if(index==deletions.lastIndex)intent.copy(phonePending=true) else intent
        })
        assertEquals(pending,RecordingSyncSnapshotCodec.decode(RecordingSyncSnapshotCodec.encode(pending)))
        assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(ByteArray(RecordingSyncSnapshotCodec.MAX_BYTES+1)) }
    }

    @Test fun InvalidReceiptOrCiphertextGeometryCannotBePersisted() {
        val bad=segments[0].copy(sha256="11".repeat(32))
        assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.encode(populated.copy(pendingReceipts=setOf(bad))) }
        for(count in listOf(1L,209L,65746L,Long.MAX_VALUE)) {
            val invalid=RecordingManifest(identity,0,true,"cd".repeat(32),listOf(segments[0].copy(byteCount=count)))
            assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.encode(RecordingSyncSnapshot(identity,manifest=invalid)) }
        }
    }
    @Test fun fullChip5120SegmentsAndAllPendingReceiptsFitExistingSqliteBoundWithoutSchemaMigration() {
        val many=List(5120) { SegmentIdentity(identity,it,"12".repeat(32),34357) }
        val large=RecordingManifest(identity,20481,true,"cd".repeat(32),many)
        val deletions=List(256) { RecordingDeletionIntent(UUID(8,it.toLong()+1),identity,large.sha256,
            DeleteLocation.PHONE_ONLY,false,false,PendantDeletion.NOT_REQUESTED) }
        val snapshot=populated.copy(manifest=large,downloadSuppressed=true,phoneSegments=many.toSet(),
            pendingReceipts=many.toSet(),deletions=deletions)
        val bytes=RecordingSyncSnapshotCodec.encode(snapshot)
        assertTrue(bytes.size<262144)
        assertEquals("OPNDSY2\u0000",bytes.copyOf(8).toString(Charsets.US_ASCII))
        assertEquals(snapshot,RecordingSyncSnapshotCodec.decode(bytes))
        val bad=bytes.copyOf().also { it[6]='1'.code.toByte() }
        assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(checksum(bad)) }
        // A v2 tag on an old small record is not a second canonical encoding.
        val old=RecordingSyncSnapshotCodec.encode(populated).also { it[6]='2'.code.toByte() }
        assertThrows(IllegalArgumentException::class.java) { RecordingSyncSnapshotCodec.decode(checksum(old)) }
    }
}
