package org.openpendant.app

import java.util.Collections

/** Complete authenticated inventory, not the filtered or paginated UI list.
 * Snapshot omission does not prove deletion. Only named remote records and,
 * if selected, current-volume phone copies are included. No raw erase. */
internal class QuickClearReview private constructor(val volume:RecordingVolume,
    remote:List<DurableCatalogEntry>,rows:List<RecordingSyncSnapshot>) {
    val remote=Collections.unmodifiableList(ArrayList(remote))
    private val rows=Collections.unmodifiableList(ArrayList(rows))
    val pendantCount get()=remote.size
    val phoneCount get()=rows.count { it.phoneSegments.isNotEmpty() }
    fun plan(includePhone:Boolean):BulkRecordingDeletion {
        val ids=remote.map { it.recording }.toSet()
        val selected=rows.filter { it.recording in ids || includePhone && it.phoneSegments.isNotEmpty() }
        val plan=BulkRecordingDeletion.create(selected,volume,if(includePhone)DeleteLocation.BOTH else DeleteLocation.PENDANT_ONLY)
        require(plan.skippedCount==0 && plan.pendantCount==pendantCount)
        return plan
    }
    companion object {
        fun create(volume:RecordingVolume,entries:List<DurableCatalogEntry>,all:List<RecordingSyncSnapshot>):QuickClearReview {
            require(entries.size<=32 && entries.map { it.recording }.distinct().size==entries.size)
            require(all.size<=128 && all.map { it.recording }.distinct().size==all.size)
            val current=all.filter { it.recording.volume==volume && !RecordingListPresentation.fullyDeleted(it) }
            require(current.none { it.staleVolume || RecordingListPresentation.pending(it) })
            for(entry in entries) {
                require(entry.recording.volume==volume && entry.finished)
                val row=requireNotNull(current.singleOrNull { it.recording==entry.recording })
                val manifest=requireNotNull(row.manifest)
                require(row.pendantCopy==PendantCopy.PRESENT && manifest.finished && manifest.sha256==entry.manifestSha256 &&
                    manifest.revision==entry.manifestRevision && manifest.segments.size==entry.sealedSegments)
            }
            // An absent catalog entry with an unconfirmed source is NOT silently
            // called deleted or included in an erase-all confirmation.
            require(current.all { it.pendantCopy==PendantCopy.DELETED || entries.any { e->e.recording==it.recording } })
            require(current.all { it.manifest?.finished==true })
            return QuickClearReview(volume,entries,current)
        }
    }
}
