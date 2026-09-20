package org.openpendant.app

import java.util.UUID
import org.junit.Assert.*
import org.junit.Test

class DurablePublicBindingTest {
    private val binding = DurablePublicBinding("12:34:56:78:9A:BC", RecordingVolume(UUID(1,2),UUID(3,4),0x0102030405060708L), "12".repeat(32))
    @Test fun exactPublicRecordRoundTripAndEveryByteTamperRejects() {
        val bytes=DurablePublicBindingCodec.encode(binding)
        assertEquals(128,bytes.size);assertEquals(binding,DurablePublicBindingCodec.decode(bytes))
        for(index in bytes.indices) { val changed=bytes.copyOf();changed[index]=(changed[index].toInt() xor 1).toByte()
            assertThrows(IllegalArgumentException::class.java){DurablePublicBindingCodec.decode(changed)} }
        for(size in listOf(0,127,129,4096))assertThrows(IllegalArgumentException::class.java){DurablePublicBindingCodec.decode(bytes.copyOf(size))}
    }
    @Test fun fullBondEpochAndCapabilitiesAreRequired() {
        val peer=DurableConnectedPeer(UUID(5,6),binding.bondAddress,DurableSyncCapabilities(true,true,true,true))
        assertEquals(DurableSyncConnection(peer.epoch,binding.volume,binding.recipientFingerprint),binding.connection(peer))
        assertThrows(IllegalArgumentException::class.java){binding.connection(peer.copy(bondAddress="12:34:56:78:9A:BD"))}
        for(caps in listOf(DurableSyncCapabilities(false,true,true,true),DurableSyncCapabilities(true,false,true,true),
            DurableSyncCapabilities(true,true,false,true),DurableSyncCapabilities(true,true,true,false)))
            assertThrows(IllegalArgumentException::class.java){binding.connection(peer.copy(capabilities=caps))}
    }
    @Test fun backupUnverifiedNeverAuthorizesSyncOrPlayback() {
        binding.requireVerifiedRecipient(RecipientVaultSummary(RecipientVaultState.READY,binding.recipientFingerprint,true))
        for(value in listOf(RecipientVaultSummary(RecipientVaultState.EMPTY),RecipientVaultSummary(RecipientVaultState.RECOVERY_REQUIRED),
            RecipientVaultSummary(RecipientVaultState.READY,binding.recipientFingerprint,false),
            RecipientVaultSummary(RecipientVaultState.READY,"13".repeat(32),true)))
            assertThrows(IllegalArgumentException::class.java){binding.requireVerifiedRecipient(value)}
    }
    @Test fun invalidAndSentinelPublicIdentityCannotBeEnrolled() {
        for(address in listOf("00:00:00:00:00:00","FF:FF:FF:FF:FF:FF","12:34:56:78:9a:bc","bad"))
            assertThrows(IllegalArgumentException::class.java){binding.copy(bondAddress=address)}
        for(fingerprint in listOf("00".repeat(32),"ff".repeat(32),"12"))
            assertThrows(IllegalArgumentException::class.java){binding.copy(recipientFingerprint=fingerprint)}
    }
}
