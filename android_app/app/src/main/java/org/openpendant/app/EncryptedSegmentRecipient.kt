package org.openpendant.app

/**
 * In-memory, fixed-suite proposed-container decryption, not a sync protocol.
 * Expected recording/sequence/length must be trusted catalog values, not values
 * copied out of this untrusted container. Fingerprint comes from the actual key.
 * Decryption success does not replace catalog authorization, replay/deletion
 * checks or the guarded durable file publication in RecordingSyncContract.
 * No playback/transcription/storage starts here. Inputs must remain stable while
 * snapshotted; callers own and must wipe explicit plaintext copies after use.
 */
object EncryptedSegmentRecipient {
    fun decrypt(container: ByteArray, key: RecipientRecoveryKey,
                recording: DurableRecordingId, sequence: Int, plaintextBytes: Int): DecryptedSegmentBytes {
        if (sequence < 0 || plaintextBytes !in 1..EncryptedSegmentHeader.MAX_PLAINTEXT_BYTES ||
            container.size != EncryptedSegmentHeader.HEADER_BYTES + EncryptedSegmentHeader.ENCAP_BYTES +
                plaintextBytes + EncryptedSegmentHeader.TAG_BYTES) throw EncryptedSegmentException()
        var snapshot: ByteArray? = null
        var fingerprint: ByteArray? = null
        var header: ByteArray? = null
        var info: ByteArray? = null
        var enc: ByteArray? = null
        var cipher: ByteArray? = null
        try {
            snapshot = container.copyOf()
            fingerprint = key.publicFingerprint()
            if (!EncryptedSegmentHeader.validateContainer(snapshot, recording, fingerprint, sequence, plaintextBytes))
                throw EncryptedSegmentException()
            header = snapshot.copyOfRange(0, EncryptedSegmentHeader.HEADER_BYTES)
            info = EncryptedSegmentHeader.hpkeInfo(header, recording, fingerprint, sequence, plaintextBytes)
            enc = snapshot.copyOfRange(EncryptedSegmentHeader.HEADER_BYTES,
                EncryptedSegmentHeader.HEADER_BYTES + EncryptedSegmentHeader.ENCAP_BYTES)
            cipher = snapshot.copyOfRange(EncryptedSegmentHeader.HEADER_BYTES + EncryptedSegmentHeader.ENCAP_BYTES, snapshot.size)
            return HpkeP256Recipient.open(key, enc, info, byteArrayOf(), cipher)
        } catch (_: RuntimeException) {
            throw EncryptedSegmentException()
        } finally {
            snapshot?.fill(0); fingerprint?.fill(0); header?.fill(0)
            info?.fill(0); enc?.fill(0); cipher?.fill(0)
        }
    }
}
