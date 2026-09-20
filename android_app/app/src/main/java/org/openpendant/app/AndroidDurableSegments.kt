package org.openpendant.app

import android.content.Context
import android.os.Looper
import android.os.Build
import android.system.Os
import android.system.OsConstants
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.util.UUID

/** Production directory barriers; never place ciphertext in shared/backup
 * storage. No transport, scanning, key creation or download starts here. */
object AndroidDurableSegments {
    fun create(context: Context): DurableSegmentStore = createAt(context, "encrypted-segments-v1")
    fun openExisting(context: Context): DurableSegmentStore? {
        check(Looper.myLooper() != Looper.getMainLooper())
        val directory=context.noBackupFilesDir.canonicalFile.toPath().resolve("encrypted-segments-v1")
        val found=AndroidDurableBinding.attributes(directory) ?: return null
        check(found.isDirectory)
        return createAt(context,"encrypted-segments-v1", allowCreate = false)
    }

    /** Explicit generated-data runner namespace, never aliases production files. */
    fun forSyntheticTests(context: Context, id: UUID, diagnostic:((String,String,Int)->Unit)?=null): DurableSegmentStore {
        require(validOwnedUuid(id))
        return createAt(context, "segment-test-$id",diagnostic)
    }

    private fun createAt(context: Context, name: String, diagnostic:((String,String,Int)->Unit)?=null,
                         allowCreate: Boolean = true): DurableSegmentStore {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Initialize segment storage off the main thread" }
        check(Build.VERSION.SDK_INT>=30) { "Durable encrypted storage requires Android 11 or newer" }
        val parent = context.noBackupFilesDir.canonicalFile.toPath()
        require(Files.isDirectory(parent, NOFOLLOW_LINKS)) { "Private storage unavailable" }
        val directory = parent.resolve(name)
        if (AndroidDurableBinding.attributes(directory) == null) {
            check(allowCreate) { "Existing encrypted storage disappeared" }
            Files.createDirectory(directory)
            Os.chmod(directory.toString(), 448) //0700, in the app-private no-backup directory.
            syncDirectory(parent)
        }
        return DurableSegmentStore(directory.toFile(), SegmentDirectorySync(::syncDirectory),
            SegmentUsableSpace(::usableSpace), diagnostic, SegmentPublication(AndroidSegmentPublication::publish),
            SegmentFileIdentity(::fileIdentity))
    }

    private data class Inode(val device:Long,val inode:Long,val directory:Boolean)
    private fun fileIdentity(path:Path,isDirectory:Boolean):Any {
        val attributes=Os.lstat(path.toString()) //No symlink following, mount discovery or fabricated identity.
        check(if(isDirectory) OsConstants.S_ISDIR(attributes.st_mode) else OsConstants.S_ISREG(attributes.st_mode)) {
            "Segment filesystem object type changed"
        }
        return Inode(attributes.st_dev,attributes.st_ino,isDirectory)
    }

    private fun usableSpace(directory: Path): Long {
        // Android's NIO FileStore discovery walks parent directories and mount
        // tables outside our private namespace. Query this validated root only.
        check(directory.isAbsolute && directory.normalize() == directory &&
            Files.isDirectory(directory, NOFOLLOW_LINKS) && directory.toRealPath() == directory) {
            "Segment storage directory changed"
        }
        val space = Os.statvfs(directory.toString())
        return checkedSegmentUsableSpace(space.f_bavail, space.f_frsize, space.f_bfree, space.f_blocks)
    }

    internal fun syncDirectory(directory: Path) {
        val fd = Os.open(directory.toString(), OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW, 0)
        try {
            check(OsConstants.S_ISDIR(Os.fstat(fd).st_mode)) { "Segment storage directory changed" }
            Os.fsync(fd)
        } finally { Os.close(fd) }
    }
}
