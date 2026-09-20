package org.openpendant.app

import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.NoSuchFileException
import java.nio.file.Path
import java.nio.file.attribute.BasicFileAttributes

/** Explicit version1 derivative policy: these full-identity recordings produce
 * RAM playback only and own NO WAV/transcript/cache disk format. The reserved
 * derivative namespace must be absent or empty. Any entry refuses; no guessed
 * legacy paths, wildcard deletion or silent default no-op. Future derivative
 * production MUST replace this policy before writing any artifact. Caller owns
 * the process library job/recording/file guards for the complete deletion.
 */
internal class RamOnlyRecordingDerivatives(private val privateParent: Path,
    private val sync: SegmentDirectorySync) : RecordingDiskDerivatives {
    private fun prove(plan: PhoneDeletionPlan) {
        require(plan.segments.all { it.recording == plan.recording })
        check(privateParent.isAbsolute && privateParent.normalize() == privateParent &&
            Files.readAttributes(privateParent, BasicFileAttributes::class.java, NOFOLLOW_LINKS).isDirectory &&
            privateParent.toRealPath() == privateParent)
        val root = privateParent.resolve("durable-derivatives-v1")
        val found = try { Files.readAttributes(root, BasicFileAttributes::class.java, NOFOLLOW_LINKS) }
            catch (_: NoSuchFileException) { null }
        if (found != null) {
            check(found.isDirectory && root.toRealPath() == root)
            Files.newDirectoryStream(root).use { check(!it.iterator().hasNext()) }
            sync.sync(root)
        }
        sync.sync(privateParent)
    }
    override fun removeAudioAndSync(plan: PhoneDeletionPlan) = prove(plan)
    override fun removeTranscriptAndSync(plan: PhoneDeletionPlan) = prove(plan)
}
