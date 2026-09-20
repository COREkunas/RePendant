package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** PUBLIC RFC9180 recipient and scalar-one fixtures only. Memory-only ports,
 * deterministic TEST KEKs/nonces, no files/Android/Keystore/owner-key generation.
 */
class RecipientKeyVaultTest {
    private fun bytes(hex: String) = hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    private fun key(other: Boolean = false): RecipientRecoveryKey = if (other)
        RecipientRecoveryCodec.importP256(ByteArray(32).also { it[31] = 1 }, bytes("04" +
            "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296" +
            "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"))
    else RecipientRecoveryCodec.importP256(
        bytes("317f915db7bc629c48fe765587897e01e282d3e8445f79f27f65d031a88082b2"),
        bytes("04abc7e49a4c6b3566d77d0304addc6ed0e98512ffccf505e6a8e3eb25c685136f" +
            "853148544876de76c0f2ef99cdc3a05ccf5ded7860c7c021238f9e2073d2356c"))
    private fun backup(other: Boolean = false) = key(other).use { key ->
        RecipientRecoveryCodec.encode(key).use { it.copyForExplicitExport() }
    }
    private fun rejected(reason: RecipientVaultFailure, action: () -> Unit) {
        try { action(); fail("Vault operation unexpectedly succeeded") }
        catch (error: RecipientVaultException) {
            assertEquals(reason, error.failure)
            assertEquals("Recipient vault: ${reason.name}", error.message)
            assertNull(error.cause)
        }
    }
    private fun closed(action: () -> Unit) {
        try { action(); fail("Closed key used") } catch (_: IllegalStateException) { }
    }

    private inner class MemoryPorts : RecipientVaultStorage, RecipientVaultWrapper, RecipientVaultGenerator {
        val monitor = Any()
        val aliases = linkedMapOf<UUID, ByteArray>()
        val archives = mutableListOf<ByteArray>()
        var current: ByteArray? = null
        var reads = 0; var generates = 0; var commits = 0; var encrypts = 0
        var aliasCalls = 0; var ivCounter = 0L
        var residue = false
        var readFailure = false
        var aliasFailure = false
        var invalidAlias = false
        var encryptFailure = false
        var malformedWrap = false
        var commitFailure: String? = null
        var staleCas = false
        var lastPlaintext: ByteArray? = null
        var lastGenerated: RecipientRecoveryKey? = null
        val decryptPlaintexts = mutableListOf<ByteArray>()
        private fun token() = current?.let { MessageDigest.getInstance("SHA-256").digest(it)
            .joinToString("") { byte -> "%02x".format(byte.toInt() and 255) } } ?: "absent"
        fun vault() = RecipientKeyVault(this, this, this, monitor)
        override fun read(): RecipientVaultStored = synchronized(monitor) {
            reads++
            if (residue || readFailure) throw IllegalStateException("PRIVATE PROVIDER DETAIL")
            RecipientVaultStored(token(), current?.copyOf(), current == null && aliases.isEmpty() && archives.isEmpty())
        }
        override fun commit(expectedToken: String, envelope: ByteArray) = synchronized(monitor) {
            require(envelope.size == 253)
            commits++
            if (staleCas || token() != expectedToken) throw IllegalStateException("PRIVATE CAS DETAIL")
            if (commitFailure == "before") throw IllegalStateException("PRIVATE BEFORE DETAIL")
            current?.let { archives.add(it.copyOf()) }
            if (commitFailure == "archive") throw IllegalStateException("PRIVATE ARCHIVE DETAIL")
            current = envelope.copyOf()
            if (commitFailure == "after") throw IllegalStateException("PRIVATE AFTER DETAIL")
            check(MessageDigest.isEqual(envelope, current)) // Durable readback is the port's contract.
        }
        override fun createAlias(): UUID = synchronized(monitor) {
            aliasCalls++
            val alias = UUID(0x11223344L, aliasCalls.toLong())
            aliases[alias] = ByteArray(32) { (it + aliasCalls).toByte() }
            if (aliasFailure) throw RecipientVaultException(RecipientVaultFailure.NOT_EMPTY)
            if (invalidAlias) UUID(0, 0) else alias
        }
        override fun generate(): RecipientRecoveryKey {
            generates++
            return key().also { lastGenerated = it }
        }
        override fun encrypt(alias: UUID, plaintext: ByteArray, aad: ByteArray): RecipientVaultWrapped {
            encrypts++; lastPlaintext = plaintext
            if (encryptFailure) throw IllegalStateException("PRIVATE ENCRYPT DETAIL")
            require(plaintext.size == 145 && aad.size == 80)
            val iv = ByteBuffer.allocate(12).putInt(0).putLong(++ivCounter).array()
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(aliases.getValue(alias), "AES"), GCMParameterSpec(128, iv))
            cipher.updateAAD(aad)
            val ciphertext = cipher.doFinal(plaintext)
            return RecipientVaultWrapped(if (malformedWrap) iv.copyOf(11) else iv, ciphertext)
        }
        override fun decrypt(alias: UUID, iv: ByteArray, ciphertext: ByteArray, aad: ByteArray): ByteArray {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(aliases.getValue(alias), "AES"), GCMParameterSpec(128, iv))
            cipher.updateAAD(aad)
            return cipher.doFinal(ciphertext).also { decryptPlaintexts.add(it) }
        }
    }

    @Test fun constructionAndEmptySummaryNeverGenerateOrCreateAliases() {
        val ports = MemoryPorts(); val vault = ports.vault()
        assertEquals(0, ports.reads)
        repeat(2) { assertEquals(RecipientVaultState.EMPTY, vault.summary().state) }
        assertEquals(0, ports.generates); assertEquals(0, ports.aliasCalls); assertEquals(0, ports.commits)
        rejected(RecipientVaultFailure.NO_ACTIVE_KEY) { vault.export() }
        assertEquals(RecipientVaultState.EMPTY, vault.summary().state)
    }

    @Test fun explicitCreateStoresOnlyCiphertextAndDoesNotVerifyBackup() {
        val ports = MemoryPorts(); val vault = ports.vault()
        val state = vault.create()
        assertEquals(RecipientVaultState.READY, state.state); assertFalse(state.backupVerified)
        assertEquals(64, state.fingerprintHex!!.length); assertEquals(253, ports.current!!.size)
        assertArrayEquals(bytes("4f504e445256310000010050"), ports.current!!.copyOfRange(0, 12))
        val publicFixtureScalar = backup().copyOfRange(16, 48).toList()
        assertFalse(ports.current!!.toList().windowed(32).any { it == publicFixtureScalar })
        rejected(RecipientVaultFailure.NOT_EMPTY) { vault.create() }
        assertEquals(1, ports.generates); assertEquals(1, ports.aliasCalls)
        assertTrue(ports.lastPlaintext!!.all { it == 0.toByte() })
        assertTrue(ports.decryptPlaintexts.all { value -> value.all { it == 0.toByte() } })
        closed { ports.lastGenerated!!.publicFingerprint() }
    }

    @Test fun exportIsExplicitCloseableSecretAndDoesNotSetVerification() {
        val ports = MemoryPorts(); val vault = ports.vault(); vault.create()
        val holder = vault.export()
        assertArrayEquals(backup(), holder.copyForExplicitExport())
        assertFalse(holder.toString().contains("317f915d")); holder.close()
        closed { holder.copyForExplicitExport() }
        assertFalse(vault.summary().backupVerified); assertEquals(1, ports.commits)
    }

    @Test fun readbackVerificationDurablyAuthenticatesFlagAndArchivesPriorCiphertext() {
        val ports = MemoryPorts(); val vault = ports.vault(); vault.create()
        val before = ports.current!!.copyOf()
        val verified = vault.verifyBackup(backup())
        assertTrue(verified.backupVerified); assertEquals(1, ports.aliasCalls)
        assertArrayEquals(before, ports.archives.single())
        assertFalse(before.copyOfRange(80, 92).contentEquals(ports.current!!.copyOfRange(80, 92)))
        assertTrue(ports.vault().summary().backupVerified)
        vault.verifyBackup(backup()); assertEquals(2, ports.commits)
    }

    @Test fun invalidOrDifferentBackupDoesNotMutateOrFenceReadableKey() {
        val ports = MemoryPorts(); val vault = ports.vault(); vault.create()
        rejected(RecipientVaultFailure.INVALID_BACKUP) { vault.verifyBackup(backup().also { it[25] = (it[25].toInt() xor 1).toByte() }) }
        rejected(RecipientVaultFailure.INVALID_BACKUP) { vault.restore(ByteArray(146)) }
        rejected(RecipientVaultFailure.DIFFERENT_RECIPIENT) { vault.verifyBackup(backup(true)) }
        rejected(RecipientVaultFailure.DIFFERENT_RECIPIENT) { vault.restore(backup(true)) }
        assertEquals(RecipientVaultState.READY, vault.summary().state)
        assertEquals(1, ports.commits); assertEquals(1, ports.aliasCalls)
    }

    @Test fun explicitImportIntoEmptyPhoneCountsAsVerifiedWithoutGeneration() {
        val ports = MemoryPorts(); val vault = ports.vault()
        assertTrue(vault.restore(backup()).backupVerified)
        assertEquals(0, ports.generates); assertEquals(1, ports.aliasCalls)
        assertArrayEquals(backup(), vault.export().use { it.copyForExplicitExport() })
        vault.restore(backup()); assertEquals(1, ports.commits)
    }

    @Test fun sameKeyImportIntoReadableVaultVerifiesWithoutRotation() {
        val ports = MemoryPorts(); val vault = ports.vault(); vault.create()
        assertTrue(vault.restore(backup()).backupVerified)
        assertEquals(1, ports.aliasCalls); assertEquals(1, ports.archives.size)
    }

    @Test fun missingCiphertextWithAliasOrHistoryIsNeverProvenEmpty() {
        val ports = MemoryPorts(); ports.vault().create(); ports.current = null
        val vault = ports.vault()
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
        rejected(RecipientVaultFailure.RECOVERY_REQUIRED) { vault.create() }
        assertEquals(1, ports.generates)
        assertTrue(vault.restore(backup()).backupVerified)
        assertEquals(2, ports.aliases.size)
        ports.current = null; ports.aliases.clear(); ports.archives.add(ByteArray(253))
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, ports.vault().summary().state)
    }

    @Test fun missingWrappingKeyIsRecoveryNotAutomaticRegeneration() {
        val ports = MemoryPorts(); ports.vault().create(); ports.aliases.clear()
        val vault = ports.vault()
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
        rejected(RecipientVaultFailure.RECOVERY_REQUIRED) { vault.create() }
        assertTrue(vault.restore(backup()).backupVerified)
        assertEquals(1, ports.generates); assertEquals(1, ports.archives.size)
    }

    @Test fun corruptHeaderFlagNonceTagAndFingerprintAllRequireRecovery() {
        for (offset in listOf(0, 8, 10, 12, 28, 60, 61, 64, 66, 68, 79, 80, 91, 92, 252)) {
            val ports = MemoryPorts(); ports.vault().create()
            ports.current!![offset] = (ports.current!![offset].toInt() xor 1).toByte()
            val vault = ports.vault()
            assertEquals("offset $offset", RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
            rejected(RecipientVaultFailure.RECOVERY_REQUIRED) { vault.export() }
            assertEquals(1, ports.commits)
        }
    }

    @Test fun explicitFaultRestoreArchivesCorruptionAndUsesNewAlias() {
        val ports = MemoryPorts(); val vault = ports.vault(); vault.create()
        ports.current!![252] = (ports.current!![252].toInt() xor 1).toByte()
        val damaged = ports.current!!.copyOf()
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
        assertTrue(vault.restore(backup()).backupVerified)
        assertEquals(2, ports.aliases.size); assertArrayEquals(damaged, ports.archives.single())
        assertEquals(RecipientVaultState.READY, vault.summary().state)
    }

    @Test fun aliasFailureAfterSideEffectIsStickyAndRestartRefusesCreation() {
        val ports = MemoryPorts(); val vault = ports.vault(); ports.aliasFailure = true
        rejected(RecipientVaultFailure.OPERATION_FAILED) { vault.create() }
        assertEquals(1, ports.aliases.size); assertNull(ports.current)
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
        rejected(RecipientVaultFailure.RECOVERY_REQUIRED) { ports.vault().create() }
        assertEquals(1, ports.generates); closed { ports.lastGenerated!!.publicFingerprint() }
        ports.aliasFailure = false
        assertTrue(vault.restore(backup()).backupVerified); assertEquals(2, ports.aliases.size)
    }

    @Test fun invalidAliasAfterMutationAndEncryptFailureRemainFencedAndWipe() {
        for (mode in listOf("alias", "encrypt", "length")) {
            val ports = MemoryPorts(); val vault = ports.vault()
            ports.invalidAlias = mode == "alias"; ports.encryptFailure = mode == "encrypt"
            ports.malformedWrap = mode == "length"
            rejected(RecipientVaultFailure.OPERATION_FAILED) { vault.create() }
            assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
            assertEquals(0, ports.commits); assertEquals(1, ports.aliases.size)
            ports.lastPlaintext?.let { assertTrue(it.all { byte -> byte == 0.toByte() }) }
            rejected(RecipientVaultFailure.RECOVERY_REQUIRED) { vault.create() }
        }
    }

    @Test fun commitBeforeAndAfterFailuresNeverImplicitlyRetryOrReplace() {
        for (mode in listOf("before", "after")) {
            val ports = MemoryPorts(); val vault = ports.vault(); ports.commitFailure = mode
            rejected(RecipientVaultFailure.OPERATION_FAILED) { vault.create() }
            assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
            rejected(RecipientVaultFailure.RECOVERY_REQUIRED) { vault.export() }
            val restarted = ports.vault()
            assertEquals(if (mode == "after") RecipientVaultState.READY else RecipientVaultState.RECOVERY_REQUIRED,
                restarted.summary().state)
            rejected(if (mode == "after") RecipientVaultFailure.NOT_EMPTY else RecipientVaultFailure.RECOVERY_REQUIRED) { restarted.create() }
            assertEquals(1, ports.generates); assertEquals(1, ports.aliasCalls); assertEquals(1, ports.commits)
            assertTrue(ports.lastPlaintext!!.all { it == 0.toByte() })
        }
    }

    @Test fun failedVerificationCommitPreservesOldCiphertextAndRequiresExplicitRecovery() {
        for (mode in listOf("before", "archive", "after")) {
            val ports = MemoryPorts(); val vault = ports.vault(); vault.create()
            val original = ports.current!!.copyOf(); ports.commitFailure = mode
            rejected(RecipientVaultFailure.OPERATION_FAILED) { vault.verifyBackup(backup()) }
            assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
            if (mode == "before") assertArrayEquals(original, ports.current)
            else assertArrayEquals(original, ports.archives.first())
            ports.commitFailure = null
            rejected(RecipientVaultFailure.DIFFERENT_RECIPIENT) { vault.restore(backup(true)) }
            assertTrue(vault.restore(backup()).backupVerified)
            assertEquals(2, ports.aliasCalls); assertEquals(2, ports.aliases.size)
        }
    }

    @Test fun staleCasAfterAliasCreationFencesAndPreservesAllEvidence() {
        val ports = MemoryPorts(); val vault = ports.vault(); ports.staleCas = true
        rejected(RecipientVaultFailure.OPERATION_FAILED) { vault.create() }
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
        assertNull(ports.current); assertEquals(1, ports.aliases.size)
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, ports.vault().summary().state)
    }

    @Test fun residueAndUnreadableStorageRefuseRestoreBeforeAliasMutation() {
        for (residue in listOf(false, true)) {
            val ports = MemoryPorts(); val vault = ports.vault()
            ports.residue = residue; ports.readFailure = !residue
            assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
            rejected(RecipientVaultFailure.OPERATION_FAILED) { vault.restore(backup()) }
            assertEquals(0, ports.generates); assertEquals(0, ports.aliasCalls); assertEquals(0, ports.commits)
        }
    }

    @Test fun oversizedSnapshotFailsClosedAndBoundedCorruptionCanBeArchived() {
        val ports = MemoryPorts(); val vault = ports.vault(); ports.current = ByteArray(4097)
        assertEquals(RecipientVaultState.RECOVERY_REQUIRED, vault.summary().state)
        rejected(RecipientVaultFailure.OPERATION_FAILED) { vault.restore(backup()) }
        assertEquals(0, ports.aliasCalls)
        ports.current = byteArrayOf(1, 2, 3)
        assertTrue(vault.restore(backup()).backupVerified)
        assertArrayEquals(byteArrayOf(1, 2, 3), ports.archives.single())
    }

    @Test fun activeCallbackRequiresVerifiedBackupClosesKeyAndBlocksReentry() {
        val ports = MemoryPorts(); val vault = ports.vault(); vault.create()
        rejected(RecipientVaultFailure.BACKUP_NOT_VERIFIED) { vault.withActiveKey { fail("No callback permitted") } }
        vault.verifyBackup(backup())
        var retained: RecipientRecoveryKey? = null
        val fingerprint = vault.withActiveKey { active ->
            retained = active
            rejected(RecipientVaultFailure.OPERATION_IN_PROGRESS) { vault.export() }
            active.publicFingerprint()
        }
        assertEquals(32, fingerprint.size); closed { retained!!.publicFingerprint() }
        assertEquals(RecipientVaultState.READY, vault.summary().state)
    }

    @Test fun twoInstancesSharingMonitorCannotBothCreateOrRotate() {
        val ports = MemoryPorts(); val first = ports.vault(); val second = ports.vault()
        first.create()
        rejected(RecipientVaultFailure.NOT_EMPTY) { second.create() }
        rejected(RecipientVaultFailure.DIFFERENT_RECIPIENT) { second.restore(backup(true)) }
        assertEquals(1, ports.generates); assertEquals(1, ports.aliasCalls)
        assertTrue(second.verifyBackup(backup()).backupVerified); assertTrue(first.summary().backupVerified)
    }

    @Test fun publicPortContainersDoNotPrintCiphertextOrTokens() {
        assertEquals("RecipientVaultStored(<redacted>)", RecipientVaultStored("sensitive", byteArrayOf(1, 2), false).toString())
        assertEquals("RecipientVaultWrapped(<redacted>)", RecipientVaultWrapped(byteArrayOf(1), byteArrayOf(2)).toString())
    }
}
