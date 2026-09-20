package org.openpendant.app

import java.io.InputStream
import java.io.OutputStream

/** Bounded, explicit document I/O. Never opens a path, URI, network or clipboard.
 * The caller owns streams and must close them. Export success is NOT verification
 * that the chosen storage provider durably retained a recoverable backup.
 */
internal object RecoveryDocumentIO {
    fun readBackup(input: InputStream): RecipientRecoveryBytes {
        val bytes = ByteArray(RecipientRecoveryCodec.ENCODED_BYTES)
        var transferred = false
        try {
            var position = 0
            while (position < bytes.size) {
                val count = input.read(bytes, position, bytes.size - position)
                if (count < 0) throw RecoveryDocumentException()
                if (count == 0) {
                    // Broken/short-read providers must not cause a busy loop.
                    val next = input.read()
                    if (next < 0) throw RecoveryDocumentException()
                    bytes[position++] = next.toByte()
                } else {
                    if (count > bytes.size - position) throw RecoveryDocumentException()
                    position += count
                }
            }
            if (input.read() != -1) throw RecoveryDocumentException()
            return RecipientRecoveryBytes(bytes).also { transferred = true }
        } catch (_: Exception) {
            throw RecoveryDocumentException()
        } finally {
            if (!transferred) bytes.fill(0)
        }
    }

    fun writeBackup(output: OutputStream, backup: RecipientRecoveryBytes) {
        var bytes: ByteArray? = null
        try {
            bytes = backup.copyForExplicitExport()
            if (bytes.size != RecipientRecoveryCodec.ENCODED_BYTES) throw RecoveryDocumentException()
            output.write(bytes)
            output.flush()
        } catch (_: Exception) {
            throw RecoveryDocumentException()
        } finally { bytes?.fill(0) }
    }
}

/** Never includes provider text, URI, private bytes or a nested exception. */
internal class RecoveryDocumentException : IllegalArgumentException("Recovery document operation failed")
