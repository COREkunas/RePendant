package org.openpendant.app

import android.content.Context

/** The legacy reserved audio namespace still must be empty. MindyLink transcript
 * cleanup is an explicit addition, using the same serialized phone-deletion path. */
internal class MindyLinkRecordingDerivatives(context:Context):RecordingDiskDerivatives {
    private val ram=RamOnlyRecordingDerivatives(context.noBackupFilesDir.canonicalFile.toPath(),
        SegmentDirectorySync(AndroidDurableSegments::syncDirectory))
    private val cloud=MindyLinkStore(context)
    override fun removeAudioAndSync(plan:PhoneDeletionPlan){
        ram.removeAudioAndSync(plan);cloud.deleteRecording(plan.recording,true)
    }
    override fun removeTranscriptAndSync(plan:PhoneDeletionPlan){
        ram.removeTranscriptAndSync(plan);cloud.deleteRecording(plan.recording,false)
    }
}
