package org.openpendant.app

import android.os.Build
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.util.UUID

/** Android app policy forbids hard links. This private adapter instead uses
 * one same-directory renameat2(RENAME_NOREPLACE), with no fallback. It must run
 * under DurableSegmentStore's OS lock and after digest/header verification.
 * It does not fsync or issue receipts: the store must perform its directory
 * barrier and coordinator commit afterward, including after restart recovery.
 * Only API30+ supports this adapter; the rest of the app retains minSdk26. */
internal object AndroidSegmentPublication {
    private val slotPattern = Regex("[0-9a-f]{64}")
    private val testPattern = Regex("segment-test-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
    private val loaded: Unit by lazy { System.loadLibrary("openpendant_storage") }

    fun publish(directory: Path, source: Path, destination: Path) {
        check(Build.VERSION.SDK_INT >= 30) { "Atomic encrypted publication requires Android API30" }
        require(directory.isAbsolute && directory.normalize() == directory &&
            Files.isDirectory(directory, NOFOLLOW_LINKS) && directory.toRealPath() == directory &&
            directory.parent?.fileName?.toString() == "no_backup" &&
            directory.parent?.parent?.fileName?.toString() == "org.openpendant.app") {
            "Private encrypted publication namespace differs"
        }
        val namespace = directory.fileName.toString()
        require(namespace == "encrypted-segments-v1" || (testPattern.matches(namespace) &&
            validOwnedUuid(UUID.fromString(namespace.removePrefix("segment-test-"))))) {
            "Unsupported encrypted publication namespace"
        }
        require(source.parent == directory && destination.parent == directory)
        val sourceName = source.fileName.toString()
        require(sourceName.endsWith(".part"))
        val slot = sourceName.removeSuffix(".part")
        require(slotPattern.matches(slot) && destination.fileName.toString() == "$slot.segment") {
            "Encrypted publication identity differs"
        }
        loaded
        val rc = publishNative(directory.toString(), slot)
        check(rc == 0) { "Atomic encrypted publication failed (errno=${-rc}); no receipt issued" }
    }

    private external fun publishNative(directory: String, slot: String): Int
}
