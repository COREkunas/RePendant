package org.openpendant.app

import android.content.Context
import android.os.Looper

/** Explicit foreground-control factory; no boot-time creation. No existing metadata is
 * adopted or replaced. Android's real directory fsync, not a test no-op. */
internal object AndroidLongRecordingIntents {
    private const val NAMESPACE="long-recording-control-v1"
    private fun namespace(binding: DurablePublicBinding) = if(binding.volume.generation < 3) NAMESPACE
        else "long-recording-control-v2-${binding.volume.volumeId}"
    fun openOrCreateExplicit(context: Context,binding: DurablePublicBinding): LongRecordingIntentJournal {
        check(Looper.myLooper()!=Looper.getMainLooper())
        val path=context.noBackupFilesDir.canonicalFile.toPath().resolve(namespace(binding))
        return if(AndroidDurableBinding.attributes(path)==null) createExplicit(context,binding) else openExisting(context,binding)
    }
    fun createExplicit(context: Context,binding: DurablePublicBinding): LongRecordingIntentJournal {
        check(Looper.myLooper()!=Looper.getMainLooper())
        return LongRecordingIntentJournal.createExplicit(context.noBackupFilesDir.canonicalFile.toPath().resolve(namespace(binding)),binding,AndroidDurableSegments::syncDirectory)
    }
    fun openExisting(context: Context,binding: DurablePublicBinding): LongRecordingIntentJournal {
        check(Looper.myLooper()!=Looper.getMainLooper())
        return LongRecordingIntentJournal.openExisting(context.noBackupFilesDir.canonicalFile.toPath().resolve(namespace(binding)),binding,AndroidDurableSegments::syncDirectory)
    }
}
