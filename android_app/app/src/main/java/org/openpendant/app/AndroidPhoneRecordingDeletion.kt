package org.openpendant.app

import android.content.Context
import android.os.Looper
import android.system.Os
import android.system.OsConstants
import java.nio.file.Path
import java.util.UUID

/** Explicit adapter only, not installed UI/sync wiring. Requires an existing
 * validated segment namespace/lock; never creates or repairs storage to delete.
 * Derivatives require an explicit reviewed mapper; there is no default no-op. */
object AndroidPhoneRecordingDeletion {
    fun create(context: Context, derivatives: PhoneDeletionDerivatives): PhoneRecordingDeletion =
        at(context, "encrypted-segments-v1", derivatives)
    fun forSyntheticTests(context: Context, id: UUID, derivatives: PhoneDeletionDerivatives): PhoneRecordingDeletion {
        require(validOwnedUuid(id)); return at(context, "segment-test-$id", derivatives)
    }
    private fun at(context: Context, name: String, derivatives: PhoneDeletionDerivatives): PhoneRecordingDeletion {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Phone deletion requires an I/O worker" }
        val root = context.noBackupFilesDir.canonicalFile.toPath().resolve(name)
        return PhoneRecordingDeletion(root.toFile(), SegmentDirectorySync(AndroidDurableSegments::syncDirectory),
            SegmentFileIdentity(::identity), derivatives)
    }
    private data class Inode(val device: Long, val inode: Long, val directory: Boolean)
    private fun identity(path: Path, directory: Boolean): Any {
        val stat = Os.lstat(path.toString())
        check(if (directory) OsConstants.S_ISDIR(stat.st_mode) else OsConstants.S_ISREG(stat.st_mode))
        return Inode(stat.st_dev, stat.st_ino, directory)
    }
}
