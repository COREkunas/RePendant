package org.openpendant.app

import java.security.MessageDigest
import java.util.Locale
import java.util.Random
import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class CanonicalHexTest {
    private fun historical(bytes: ByteArray) = bytes.joinToString("") {
        String.format(Locale.US, "%02x", it.toInt() and 255)
    }

    @Test fun everyByteAndEveryPairExactlyMatchesHistoricalUnsignedFormatting() {
        val old = Array(256) { historical(byteArrayOf(it.toByte())) }
        assertEquals("", canonicalHex(byteArrayOf()))
        val all = ByteArray(256) { it.toByte() }
        assertEquals(historical(all), canonicalHex(all))
        for (a in 0..255) for (b in 0..255) {
            assertEquals(old[a] + old[b], canonicalHex(byteArrayOf(a.toByte(), b.toByte())))
        }
        assertArrayEquals(ByteArray(256) { it.toByte() }, all)
    }

    @Test fun hashAndDatabaseIdentityRepresentationIsUnchangedAtRealBounds() {
        val random = Random(20260919)
        for (size in listOf(0, 1, 32, 56, 128, 4096, 65745)) {
            val bytes = ByteArray(size).also { random.nextBytes(it) }
            val before = bytes.copyOf()
            assertEquals(historical(bytes), canonicalHex(bytes))
            assertEquals(historical(MessageDigest.getInstance("SHA-256").digest(bytes)), digestHex(bytes))
            assertArrayEquals(before, bytes)
        }
        val identity = DurableRecordingId(RecordingVolume(UUID(Long.MIN_VALUE, -1), UUID(-7, 8), Long.MAX_VALUE), UUID(9, -10))
        val bytes = RecordingSyncSnapshotCodec.identityBytes(identity)
        assertEquals(112, canonicalHex(bytes).length)
        assertEquals(historical(bytes), canonicalHex(bytes))
    }
}
