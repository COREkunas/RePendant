package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class PhoneMigrationProtocolTest {
    private val point = "04" + "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296" +
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"
    private val fp = RecipientRecoveryCodec.fingerprint(hexBytes(point)).joinToString("") { "%02x".format(it.toInt() and 255) }
    private val address = "C6:05:04:03:02:01"
    private fun reply() = "recorder fullinfo\r\n" +
        "RECORDER_FULL_CONFIRM sha=${"12".repeat(32)} phase=3 generation=2 blocks=2048 logical_sectors=94208 slots=5120\r\n" +
        "RECORDER_FULL_DEVICE id=11111111111151118111111111111111\r\n" +
        "RECORDER_FULL_VOLUME id=22222222222242228222222222222222\r\n" +
        "RECORDER_FULL_RECIPIENT fingerprint=$fp\r\nRECORDER_FULL_PUBLIC point=$point\r\n"
    private fun rejected(block: () -> Unit) {
        try { block(); fail("Must refuse") } catch (_: IllegalArgumentException) { } catch (_: IllegalStateException) { }
    }
    @Test fun cableIdentityHasNoInventedLegacyGeneration() {
        val b = PhoneMigrationProtocol.binding(address, reply())
        assertEquals(2L, b.volume.generation); assertEquals(fp, b.recipientFingerprint)
        assertEquals(address, b.bondAddress)
        assertEquals(UUID.fromString("11111111-1111-5111-8111-111111111111"), b.volume.deviceId)
        assertEquals(b, DurablePublicBindingCodec.decode(DurablePublicBindingCodec.encode(b)))
    }
    @Test fun inactiveAndDifferentGeometryAreRejected() {
        for (s in listOf(reply().replace("phase=3", "phase=1"), reply().replace("phase=3", "phase=2"),
            reply().replace("generation=2", "generation=1"), reply().replace("slots=5120", "slots=0"),
            reply().replace("blocks=2048", "blocks=4096"), reply().replace("logical_sectors=94208", "logical_sectors=0")))
            rejected { PhoneMigrationProtocol.binding(address, s) }
    }
    @Test fun duplicatesMissingUnknownAndOversizedRepliesRefused() {
        for (s in listOf("",reply()+"RECORDER_OTHER x=1",reply()+reply(),"x".repeat(4097)+reply(),
            reply().replace("RECORDER_FULL_VOLUME", "IGNORED"), reply()+"\u0000"))
            rejected { PhoneMigrationProtocol.binding(address, s) }
    }
    @Test fun invalidPublicPointCannotSupplyIdentityEvenWithCorrectHash() {
        val invalid = "04"+"00".repeat(64)
        val hash = RecipientRecoveryCodec.fingerprint(hexBytes(invalid)).joinToString("") { "%02x".format(it.toInt() and 255) }
        rejected { PhoneMigrationProtocol.binding(address, reply().replace(point,invalid).replace(fp,hash)) }
        rejected { PhoneMigrationProtocol.binding(address, reply().replace(fp, "12".repeat(32))) }
    }
    @Test fun sentinelIdentityRefused() {
        rejected { PhoneMigrationProtocol.binding("00:00:00:00:00:00", reply()) }
        rejected { PhoneMigrationProtocol.binding(address, reply().replace("11111111111151118111111111111111", "00".repeat(16))) }
    }
    @Test fun allCurrentFirmwareNewKeyChoicesStayBlockedEvenAfterConfirmations() {
        for (mode in PhoneMigrationChoice.entries)
            assertNotNull(PhoneMigrationPolicy.rotationRefusal(mode,false,true,"13".repeat(32),fp,true,true))
    }
    @Test fun restoringOldKeyNeverMeansEraseConsent() {
        assertNotNull(PhoneMigrationPolicy.rotationRefusal(PhoneMigrationChoice.KEEP_KEY,true,true,"13".repeat(32),fp,true,true))
    }
    @Test fun futureRotationNeedsDifferentVerifiedKeyAndSeparateConfirmation() {
        val mode=PhoneMigrationChoice.NEW_KEY_ERASE
        assertNotNull(PhoneMigrationPolicy.rotationRefusal(mode,true,false,"13".repeat(32),fp,true,true))
        for (key in listOf(null,fp,"00".repeat(32),"ff".repeat(32),"short"))
            assertNotNull(PhoneMigrationPolicy.rotationRefusal(mode,true,true,key,fp,true,true))
        assertNotNull(PhoneMigrationPolicy.rotationRefusal(mode,true,true,"13".repeat(32),fp,true,false))
        assertNull(PhoneMigrationPolicy.rotationRefusal(mode,true,true,"13".repeat(32),fp,false,true))
    }
    @Test fun archiveMustNotBeAssumedFromAnEraseConfirmation() {
        assertNotNull(PhoneMigrationPolicy.rotationRefusal(PhoneMigrationChoice.NEW_KEY_ARCHIVE,true,true,"13".repeat(32),fp,false,true))
    }
    @Test fun archiveRowsMustBeCompleteFinalAndUnambiguous() {
        val binding=PhoneMigrationProtocol.binding(address,reply())
        val id=DurableRecordingId(binding.volume,UUID(3,4))
        val segment=SegmentIdentity(id,0,"34".repeat(32),100)
        val manifest=RecordingManifest(id,1,true,"56".repeat(32),listOf(segment))
        val row=RecordingSyncSnapshot(id,manifest=manifest,phoneSegments=setOf(segment))
        assertTrue(PhoneMigrationPolicy.archiveComplete(listOf(row),binding.volume))
        for (bad in listOf(row.copy(phoneSegments=emptySet()), row.copy(staleVolume=true),row.copy(downloadSuppressed=true),
            row.copy(manifest=null),row.copy(manifest=RecordingManifest(id,1,false,manifest.sha256,listOf(segment)))))
            assertFalse(PhoneMigrationPolicy.archiveComplete(listOf(bad),binding.volume))
        assertFalse(PhoneMigrationPolicy.archiveComplete(listOf(row,row),binding.volume))
        assertFalse(PhoneMigrationPolicy.archiveComplete(listOf(row),binding.volume.copy(volumeId=UUID(9,9))))
    }
    @Test fun publicProfileNamesCannotEscapeTheirNamespace() {
        AndroidRecipientProfiles.checkFingerprint(fp)
        for (s in listOf("../legacy","x".repeat(64),"00".repeat(32),"ff".repeat(32),fp.uppercase()))
            rejected { AndroidRecipientProfiles.checkFingerprint(s) }
    }
}
