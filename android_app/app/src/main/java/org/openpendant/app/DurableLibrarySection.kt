package org.openpendant.app

import android.app.Activity
import android.app.AlertDialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.*
import java.text.DateFormat
import java.util.Date

/** Expanding, filtering and moving an idle cursor never open audio or radio.
 * Workers independently repeat all identity, integrity and operation gates. */
internal class DurableLibrarySection(private val activity: Activity, private val controller: AndroidDurableLibrary,
    private val peer: () -> DurableConnectedPeer?, private val legacyBusy: () -> Boolean,
    private val beforePlay: () -> Unit, private val recovery: () -> Unit) {
    private val ink=Color.rgb(246,247,242)
    private val muted=Color.rgb(142,152,166)
    private val green=Color.rgb(85,215,139)
    private val blue=Color.rgb(94,170,255)
    private val orange=Color.rgb(255,174,83)
    private val coral=Color.rgb(255,118,87)
    val dashboard=card()
    val library=column()
    private val transferToggle=button("Transfer  ▾") { transferExpanded=!transferExpanded;render(lastQuery) }
    private val compactStatus=text("",13,muted)
    private val transferDetails=column()
    private val status=text("",13,muted)
    private val libraryStatus=text("",13,muted)
    private val transferProgress=ProgressBar(activity,null,android.R.attr.progressBarStyleHorizontal).apply { max=100 }
    private val prerequisites=text("",13,muted)
    private val sync=button("Sync recordings") { confirmSync() }
    private val cancel=button("Stop transfer") { controller.cancel() }
    private val refresh=button("Refresh library") { if(!legacyBusy())controller.refresh() }
    private val key=button("Recording key and recovery…") { if(!legacyBusy()&&!controller.busy)recovery() }
    private val rows=column()
    private val listSummary=text("",12,muted)
    private val filterDropdown=Spinner(activity)
    private val bulkDelete=button("Delete filtered recordings…") { chooseBulkDeletion() }
    private val stopDeletion=button("Stop after current recording") { controller.cancel() }
    private var bulkDialog:AlertDialog?=null
    private var filter=RecordingListFilter.ALL
    private var filterLabels=emptyList<String>()
    private var lastQuery=""
    private var shown=30
    private var rendered=""
    private var transferExpanded=false
    private val expanded=mutableSetOf<DurableRecordingId>()
    private val choices=mutableMapOf<DurableRecordingId,Pair<Boolean,Boolean>>() // phone, pendant
    private val positions=mutableMapOf<DurableRecordingId,Long>()
    private data class PlayerViews(val slider:SeekBar,val time:TextView,var stop:Button?=null)
    private val players=mutableMapOf<DurableRecordingId,PlayerViews>()
    private val prefs=controller.transferPreferences
    private var settingRemove=false
    private val autoSync=toggle("Auto sync",prefs.read().automatic) { save(prefs.read().copy(automatic=it)) }
    private val lowBattery=toggle("Sync when battery is low",prefs.read().lowBattery) { save(prefs.read().copy(lowBattery=it)) }
    private val removeAfter=toggle("Remove from pendant after sync",prefs.read().removeAfterSync) { enabled -> changeRemoval(enabled) }
    init {
        dashboard.addView(transferToggle);dashboard.addView(compactStatus)
        dashboard.addView(transferProgress);dashboard.addView(sync);dashboard.addView(cancel);dashboard.addView(transferDetails)
        transferDetails.addView(prerequisites);transferDetails.addView(status);transferDetails.addView(key)
        transferDetails.addView(autoSync)
        transferDetails.addView(text("Once per connection, while this app is open and your pendant is idle with enough power.",12,muted))
        transferDetails.addView(removeAfter)
        transferDetails.addView(text("Keep the verified phone copy. Applies to new transfers only.",12,muted))
        transferDetails.addView(lowBattery)
        transferDetails.addView(text("Queues a transfer at the threshold below. Battery sync needs supported firmware and at least 25%; choose 35% for a useful margin. Otherwise connect USB.",12,muted))
        val threshold=Spinner(activity).apply { contentDescription="Low battery threshold";minimumHeight=dp(48) }
        threshold.adapter=adapter(listOf("Low battery: 15%","Low battery: 25%","Low battery: 35%"))
        threshold.setSelection(listOf(15,25,35).indexOf(prefs.read().lowBatteryPercent))
        threshold.onItemSelectedListener=selection { index ->
            val value=listOf(15,25,35)[index]
            if(value!=prefs.read().lowBatteryPercent)save(prefs.read().copy(lowBatteryPercent=value))
        }
        transferDetails.addView(threshold)
        library.addView(text("Saved recordings",19,ink,true));library.addView(libraryStatus)
        filterDropdown.minimumHeight=dp(48);filterDropdown.contentDescription="Filter recordings, default All"
        filterDropdown.onItemSelectedListener=selection { index ->
            val next=RecordingListFilter.entries[index]
            if(next!=filter){filter=next;shown=30;rendered="";render(lastQuery)}
        }
        library.addView(filterDropdown)
        library.addView(bulkDelete);library.addView(stopDeletion)
        val legend=LinearLayout(activity).apply { orientation=LinearLayout.HORIZONTAL;gravity=Gravity.CENTER_VERTICAL }
        listOf(Triple("Pendant",green,"Green: on pendant, last sync"),Triple("Phone",blue,"Blue: complete phone copy"),
            Triple("Transcript",orange,"Orange: transcribed")).forEach { (name,color,description) ->
            legend.addView(text("● $name",11,color).apply { contentDescription=description },LinearLayout.LayoutParams(0,-2,1f))
        }
        library.addView(legend);library.addView(listSummary);library.addView(rows);library.addView(refresh)
    }
    private fun changeRemoval(enabled:Boolean) {
        if(!enabled)save(prefs.read().copy(removeAfterSync=false)) else {
            setRemoval(false)
            AlertDialog.Builder(activity).setTitle("Remove newly synced pendant copies?")
                .setMessage("Only newly completed transfers will be removed from the pendant, after every phone file and the saved receipt are verified. Existing phone recordings are not selected.\n\nKeep your recovery backup safe. Turning this off later does not undo deletion requests already saved.")
                .setNegativeButton("Keep both copies",null).setPositiveButton("Enable removal") { _,_->
                    save(prefs.read().copy(removeAfterSync=true));setRemoval(prefs.read().removeAfterSync)
                }.show()
        }
    }
    private fun setRemoval(value:Boolean) { settingRemove=true;removeAfter.isChecked=value;settingRemove=false }
    private fun save(value:TransferPreferences) {
        try { prefs.save(value) } catch (_:Exception) {
            AlertDialog.Builder(activity).setTitle("Setting not saved").setMessage("Try again before starting a transfer.").setPositiveButton("OK",null).show()
        }
        render(lastQuery)
    }
    fun render(query:String="") {
        lastQuery=query
        val state=controller.state
        val current=try { peer() } catch (_:Exception) { null }
        val refusal=controller.syncRefusal(current)
        transferToggle.stableText=if(transferExpanded)"Transfer  ▴" else "Transfer  ▾"
        transferToggle.contentDescription="Transfer settings, ${if(transferExpanded)"expanded" else "collapsed"}"
        transferDetails.visibility=if(transferExpanded)View.VISIBLE else View.GONE
        compactStatus.stableText=when {
            state.work==DurableLibraryWork.SYNC -> state.transfer?.let { "${it.percent}% · transferring to phone" }?:"Preparing transfer…"
            state.needsAttention -> "Storage needs attention"
            controller.transferPolicy.waitingForUsb(current?.bondAddress) -> "Low battery · waiting for safe power and an idle connection"
            refusal==StorageSyncPower.USB_REQUIRED -> "Connect pendant USB to sync"
            else -> "${state.recordings.count(RecordingListPresentation::needsSync)} need sync · ${state.recordings.count(RecordingListPresentation::phoneComplete)} saved on phone"
        }
        status.stableText=state.message
        status.visibility=if(state.message.startsWith("Storage metadata checked")||state.message.startsWith("Check recording key"))View.GONE else View.VISIBLE
        prerequisites.stableText=when {
            state.needsAttention -> "Storage needs attention; no recording was reset."
            state.binding==null -> "Set up recording security in Settings."
            !state.recipientReady -> "Verify your recovery backup in Settings."
            current==null -> "Connect your pendant to sync."
            state.work==DurableLibraryWork.SYNC -> if(controller.supportsBatterySync) "Sync continues with the screen off. Keep the pendant nearby." else "Keep pendant USB connected. Sync continues with the screen off."
            refusal!=null -> refusal
            else -> if(controller.supportsBatterySync) "Ready on battery or USB. You can turn the screen off once sync starts." else "USB power required. You can turn the screen off once sync starts."
        }
        sync.isEnabled=refusal==null&&!legacyBusy()&&!controller.busy
        cancel.visibility=if(state.work==DurableLibraryWork.SYNC)View.VISIBLE else View.GONE
        transferProgress.visibility=if(state.work==DurableLibraryWork.SYNC)View.VISIBLE else View.GONE
        transferProgress.isIndeterminate=state.transfer==null;transferProgress.progress=state.transfer?.percent?:0
        refresh.isEnabled=!controller.busy&&!legacyBusy()
        key.visibility=if(!state.recipientReady&&!controller.busy)View.VISIBLE else View.GONE
        key.isEnabled=!controller.busy&&!legacyBusy()
        libraryStatus.visibility=if(state.work!=DurableLibraryWork.SYNC&&(state.message.startsWith("Playback paused at")||
            state.message.startsWith("Operation could not")||state.message.startsWith("Bulk deletion")||
            state.message.startsWith("Deletion intent saved")||state.needsAttention))View.VISIBLE else View.GONE
        libraryStatus.stableText=state.message
        val filtered=filteredRows()
        bulkDelete.stableText="Delete filtered (${filtered.size})…"
        bulkDelete.isEnabled=filtered.isNotEmpty()&&!controller.busy&&!legacyBusy()&&!state.needsAttention&&state.binding!=null
        stopDeletion.visibility=if(state.work==DurableLibraryWork.DELETE)View.VISIBLE else View.GONE
        val labels=RecordingListFilter.entries.map { choice -> "${choice.label} (${state.recordings.count { RecordingListPresentation.matches(it,choice) }})" }
        if(labels!=filterLabels){filterLabels=labels;filterDropdown.adapter=adapter(labels);filterDropdown.setSelection(filter.ordinal)}
        updatePlayerViews()
        val active=controller.player
        val signature=state.recordings.joinToString { "${it.recording}:${it.revision}" }+
            ":${state.work}:${state.recipientReady}:${state.needsAttention}:${legacyBusy()}:${controller.busy}:$shown:$query:$filter:${state.firstSyncedTimes}:$expanded:${active?.recording}:${active?.snapshot()?.playing}"
        if(signature==rendered)return
        rendered=signature;rows.removeAllViews();players.clear()
        val matching=filtered
        listSummary.stableText="${matching.size} recordings"+(if(filter==RecordingListFilter.HISTORY)" · empty recordings" else "")
        listSummary.visibility=if(query.isBlank()&&filter!=RecordingListFilter.HISTORY)View.GONE else View.VISIBLE
        if(matching.isEmpty())rows.addView(text(if(query.isBlank())"No recordings in this view." else "No matching recordings.",13,muted))
        matching.take(shown).forEach { row->addRow(row) }
        if(matching.size>shown)rows.addView(button("Show more recordings") { shown+=30;rendered="";render(query) })
        updatePlayerViews()
    }
    private fun filteredRows()=RecordingListPresentation.ordered(controller.state.recordings,controller.state.firstSyncedTimes).filter {
        RecordingListPresentation.matches(it,filter)&&(lastQuery.isBlank()||listOf(title(it),it.recording.recordingId.toString(),dateLabel(it),
            RecordingListPresentation.phoneLabel(it),RecordingListPresentation.pendantLabel(it)).any { value->value.contains(lastQuery,true) })
    }
    private fun chooseBulkDeletion() {
        if(controller.busy||legacyBusy()||controller.state.needsAttention)return
        val volume=controller.state.binding?.volume?:return
        val selectedRows=filteredRows().toList() // All matches, before pagination.
        if(selectedRows.isEmpty())return
        val criteria=filter.label+(if(lastQuery.isBlank())"" else " · Search: $lastQuery")
        val body=column().apply { setPadding(dp(20),dp(8),dp(20),0) }
        body.addView(text("$criteria\n${selectedRows.size} matching recordings, including other pages.",14,ink))
        val phone=CheckBox(activity).apply { tag="bulk-phone";text="Delete from phone";setTextColor(ink);minHeight=dp(48) }
        val pendant=CheckBox(activity).apply { tag="bulk-pendant";text="Delete from pendant";setTextColor(ink);minHeight=dp(48) }
        body.addView(phone);body.addView(pendant)
        val preview=text("Select which copies to delete.",13,ink).apply { tag="bulk-preview" };body.addView(preview)
        body.addView(text("Active recordings and unfinished deletion requests are skipped. Old storage copies can only be removed from the phone. Pendant removal finishes during sync. Phone deletion cannot be undone.",12,ink))
        val dialog=AlertDialog.Builder(activity).setTitle("Delete filtered recordings")
            .setView(ScrollView(activity).apply { addView(body) }).setNegativeButton("Cancel",null).setPositiveButton("Review",null).create()
        dialog.show()
        bulkDialog=dialog
        dialog.setOnDismissListener { if(bulkDialog===dialog)bulkDialog=null }
        val review=dialog.getButton(AlertDialog.BUTTON_POSITIVE);review.isEnabled=false
        var plan:BulkRecordingDeletion?=null
        fun update() {
            val location=if(phone.isChecked&&pendant.isChecked)DeleteLocation.BOTH else if(phone.isChecked)DeleteLocation.PHONE_ONLY else if(pendant.isChecked)DeleteLocation.PENDANT_ONLY else null
            plan=location?.let { BulkRecordingDeletion.create(selectedRows,volume,it) }
            val chosen=plan
            preview.text=if(chosen==null)"Select which copies to delete." else
                "${chosen.targets.size} recordings selected · ${chosen.skippedCount} skipped\nPhone: ${chosen.phoneCount} · Pendant: ${chosen.pendantCount}"
            review.isEnabled=chosen?.targets?.isNotEmpty()==true
        }
        phone.setOnCheckedChangeListener { _,_->update() };pendant.setOnCheckedChangeListener { _,_->update() }
        review.setOnClickListener {
            val chosen=plan?:return@setOnClickListener
            if(chosen.targets.isEmpty()||controller.busy||legacyBusy())return@setOnClickListener
            dialog.dismiss()
            val confirmation=AlertDialog.Builder(activity).setTitle("Delete ${chosen.targets.size} filtered recordings?")
                .setMessage("$criteria\n\nPhone deletion: ${chosen.phoneCount} recordings\nPendant deletion: ${chosen.pendantCount} recordings\nSkipped: ${chosen.skippedCount}\n\n"+
                    "Only this reviewed selection is affected. Phone copies are removed now and will not download again automatically. Pendant removals wait for sync. A row disappears only when neither copy remains. This cannot be undone.")
                .setNegativeButton("Cancel",null).setPositiveButton("Delete ${chosen.targets.size} recordings") { _,_->
                    if(!controller.busy&&!legacyBusy())controller.deleteFiltered(chosen)
                }.show()
            bulkDialog=confirmation
            confirmation.setOnDismissListener { if(bulkDialog===confirmation)bulkDialog=null }
        }
    }
    private fun addRow(row:RecordingSyncSnapshot) {
        val item=card();rows.addView(item)
        val open=row.recording in expanded
        val header=LinearLayout(activity).apply {
            tag="recording-expander";orientation=LinearLayout.HORIZONTAL;gravity=Gravity.CENTER_VERTICAL;minimumHeight=dp(52)
            isClickable=true;isFocusable=true
            contentDescription="${dateLabel(row)}, ${title(row)}, ${RecordingListPresentation.pendantLabel(row)}, ${RecordingListPresentation.phoneLabel(row)}, not transcribed, ${if(open)"expanded" else "collapsed"}"
            setOnClickListener { if(!expanded.add(row.recording))expanded.remove(row.recording);rendered="";render(lastQuery) }
        }
        header.addView(text(dateLabel(row),14,ink,true),LinearLayout.LayoutParams(0,-2,1f))
        header.addView(dot(green,!row.staleVolume&&row.pendantCopy==PendantCopy.PRESENT,RecordingListPresentation.pendantLabel(row)+", last synced"))
        header.addView(dot(blue,RecordingListPresentation.hasPhoneCopy(row),RecordingListPresentation.phoneLabel(row)))
        header.addView(dot(orange,false,"Not transcribed"));header.addView(text(if(open)"  ▴" else "  ▾",18,muted));item.addView(header)
        if(!open)return
        item.addView(text(title(row),15,ink,true))
        item.addView(text("${RecordingListPresentation.pendantLabel(row)} · last sync\n${RecordingListPresentation.phoneLabel(row)}",12,muted))
        if(row.staleVolume)item.addView(text("From older pendant storage. Phone copies can be deleted; the current pendant storage is unchanged.",12,coral))
        if(row.pendingReceipts.isNotEmpty())item.addView(text("Phone saved · receipt awaiting sync",12,muted))
        addPlayer(item,row);item.addView(button("Recording details") { showDetails(row) });addDeletion(item,row)
    }
    private fun addPlayer(item:LinearLayout,row:RecordingSyncSnapshot) {
        val duration=RecordingListPresentation.audioMillis(row)?:0
        val own=controller.player?.recording==row.recording
        val allowed=own||controller.state.playable(row)&&!controller.busy&&!legacyBusy()
        val seek=SeekBar(activity).apply {
            tag="recording-timeline";max=duration.coerceAtMost(Int.MAX_VALUE.toLong()).toInt();minimumHeight=dp(48)
            isEnabled=allowed&&duration>0;contentDescription="Recording playback position"
            progressTintList=ColorStateList.valueOf(blue);thumbTintList=ColorStateList.valueOf(blue)
        }
        val time=text("",12,muted)
        seek.setOnSeekBarChangeListener(object:SeekBar.OnSeekBarChangeListener {
            override fun onStartTrackingTouch(bar:SeekBar){bar.isPressed=true}
            override fun onProgressChanged(bar:SeekBar,value:Int,fromUser:Boolean) {
                if(fromUser) {
                    time.text="${RecordingTimeline.label(value.toLong())} / ${RecordingTimeline.label(duration)}"
                    if(!bar.isPressed) { // Keyboard / accessibility seek, not a drag.
                        positions[row.recording]=value.toLong()
                        if(controller.player?.recording==row.recording)controller.seekPlayback(value.toLong())
                        updatePlayerViews()
                    }
                }
            }
            override fun onStopTrackingTouch(bar:SeekBar) {
                bar.isPressed=false;positions[row.recording]=bar.progress.toLong()
                if(controller.player?.recording==row.recording)controller.seekPlayback(bar.progress.toLong())
                updatePlayerViews()
            }
        })
        players[row.recording]=PlayerViews(seek,time);item.addView(seek);item.addView(time)
        val actions=LinearLayout(activity).apply { orientation=LinearLayout.HORIZONTAL;isBaselineAligned=false }
        val playing=own&&controller.player?.snapshot()?.playing==true
        actions.addView(button(if(playing)"Pause" else "Play") {
            if(controller.player?.recording==row.recording) {
                if(controller.player?.snapshot()?.playing==true)controller.pausePlayback() else controller.resumePlayback()
            } else if(controller.state.playable(row)&&!controller.busy&&!legacyBusy()) {
                beforePlay();controller.play(row.recording,positionMillis=positions[row.recording]?:0)
            }
        }.apply { isEnabled=allowed;contentDescription=if(playing)"Pause recording" else "Play recording" },
            LinearLayout.LayoutParams(0,-2,1f).apply { rightMargin=dp(8) })
        val stop=button("Stop") {
            positions[row.recording]=0
            if(controller.player?.recording==row.recording)controller.cancel()
            updatePlayerViews()
        }.apply { isEnabled=own||(positions[row.recording]?:0)>0;contentDescription="Stop and rewind recording" }
        players[row.recording]?.stop=stop
        actions.addView(stop,LinearLayout.LayoutParams(0,-2,1f))
        item.addView(actions)
        if(row.staleVolume)item.addView(text("Playback for old storage copies is unavailable.",12,muted))
        else if(!RecordingListPresentation.phoneComplete(row))item.addView(text("Sync the complete phone copy to play.",12,muted))
    }
    private fun updatePlayerViews() {
        for((id,view) in players) {
            val control=controller.player?.takeIf { it.recording==id }
            val position=control?.snapshot()?.positionMillis?:positions[id]?:0
            view.stop?.isEnabled=control!=null||position>0
            if(!view.slider.isPressed) {
                view.slider.progress=position.coerceAtMost(view.slider.max.toLong()).toInt()
                view.time.text="${RecordingTimeline.label(position)} / ${RecordingTimeline.label(view.slider.max.toLong())}"
            }
        }
    }
    private fun addDeletion(item:LinearLayout,row:RecordingSyncSnapshot) {
        val state=controller.state
        val idle=!controller.busy&&!legacyBusy()&&!state.needsAttention
        val volume=state.binding?.volume
        val phoneAllowed=idle&&volume!=null&&RecordingDeletionScope.allowed(row,volume,DeleteLocation.PHONE_ONLY)
        val pendantAllowed=idle&&volume!=null&&RecordingDeletionScope.allowed(row,volume,DeleteLocation.PENDANT_ONLY)
        val pending=row.deletions.singleOrNull { it.phonePending||it.pendant==PendantDeletion.PENDING }
        val picked=pending?.let { (it.location!=DeleteLocation.PENDANT_ONLY) to (it.location!=DeleteLocation.PHONE_ONLY) }
            ?:choices[row.recording]?:(false to false)
        item.addView(text("Delete copies",13,muted,true))
        val phone=CheckBox(activity).apply { text="Delete from phone";setTextColor(ink);minHeight=dp(48);isChecked=picked.first;isEnabled=phoneAllowed&&pending==null }
        val pendant=CheckBox(activity).apply { text="Delete from pendant";setTextColor(ink);minHeight=dp(48);isChecked=picked.second&&pendantAllowed;isEnabled=pendantAllowed&&pending==null }
        item.addView(phone);item.addView(pendant)
        val delete=button(if(pending==null)"Delete selected copies…" else if(pending.phonePending)"Resume deletion…" else "Sync to finish deletion…") {
            if(pending!=null&&!pending.phonePending)confirmSync() else {
                val location=pending?.location ?: if(phone.isChecked&&pendant.isChecked)DeleteLocation.BOTH else if(phone.isChecked)DeleteLocation.PHONE_ONLY else if(pendant.isChecked)DeleteLocation.PENDANT_ONLY else return@button
                confirmDelete(row,location,pending?.keepTranscript?:false)
            }
        }.apply {
            setTextColor(ColorStateList(arrayOf(intArrayOf(-android.R.attr.state_enabled),intArrayOf()),intArrayOf(muted,coral)))
            isEnabled=(phoneAllowed&&phone.isChecked)||(pendantAllowed&&pendant.isChecked)
        }
        fun selectionChanged(){choices[row.recording]=phone.isChecked to pendant.isChecked;delete.isEnabled=(phoneAllowed&&phone.isChecked)||(pendantAllowed&&pendant.isChecked)}
        phone.setOnCheckedChangeListener { _,_->selectionChanged() };pendant.setOnCheckedChangeListener { _,_->selectionChanged() }
        item.addView(delete)
        if(pending!=null)item.addView(text("Saved deletion request. Pendant removal is confirmed during sync.",12,muted))
    }
    private fun confirmDelete(row:RecordingSyncSnapshot,location:DeleteLocation,keepTranscript:Boolean) {
        if(controller.busy||legacyBusy())return
        val scope=if(row.staleVolume)"phone" else when(location){DeleteLocation.PHONE_ONLY->"phone";DeleteLocation.PENDANT_ONLY->"pendant";DeleteLocation.BOTH->"phone and pendant"}
        AlertDialog.Builder(activity).setTitle("Delete from $scope?")
            .setMessage("${dateLabel(row)}\n${title(row)}\n\n"+
                if(row.staleVolume)"Only the old phone copy will be removed. Current pendant storage is unchanged. This cannot be undone."
                else "Pendant deletion finishes on the next sync. Deleted phone copies will not download again automatically.")
            .setNegativeButton("Cancel",null).setPositiveButton("Delete selected copies") { _,_->
                if(!controller.busy&&!legacyBusy())controller.delete(row.recording,location,keepTranscript)
            }.show()
    }
    private fun confirmSync() {
        if(controller.busy||legacyBusy())return
        val selected=try{peer()}catch(_:Exception){null}
        val refusal=controller.syncRefusal(selected)
        if(refusal!=null){AlertDialog.Builder(activity).setTitle("Not ready to sync").setMessage(refusal).setPositiveButton("OK",null).show();return}
        AlertDialog.Builder(activity).setTitle("Sync recordings?")
            .setMessage((if(controller.supportsBatterySync) "Keep the pendant nearby with enough battery." else "Keep pendant USB power connected.")+" Saved deletion requests will also be completed.\n\n"+
                (if(prefs.read().removeAfterSync)"New phone copies will be verified before their pendant copies are removed." else "Other pendant copies will be kept."))
            .setNegativeButton("Not now",null).setPositiveButton("Sync") { _,_->
                val fresh=peer()
                if(fresh!=null&&fresh==selected&&!controller.busy&&!legacyBusy()&&controller.syncRefusal(fresh)==null)controller.sync(fresh)
            }.show()
    }
    private fun showDetails(row:RecordingSyncSnapshot) {
        AlertDialog.Builder(activity).setTitle(title(row)).setMessage("${dateLabel(row)}\n\n"+
            "${row.phoneSegments.size}/${row.manifest?.segments?.size?:0} parts on phone\n\n"+
            "First synced is the transfer date, not the recording date. Older recording dates were not saved. Duration excludes gaps; playback stops at missing audio.\n\n"+
            "Transcription for these long recordings is not available yet. The orange indicator stays off.\n\nID: ${row.recording.recordingId}")
            .setPositiveButton("Close",null).show()
    }
    private fun dateLabel(row:RecordingSyncSnapshot)=controller.state.firstSyncedTimes[row.recording]?.let {
        "First synced "+DateFormat.getDateTimeInstance(DateFormat.MEDIUM,DateFormat.SHORT).format(Date(it))
    }?:"Date not recorded\n#${row.recording.recordingId.toString().takeLast(8)}"
    private fun title(row:RecordingSyncSnapshot)=RecordingListPresentation.title(row)
    private fun dp(value:Int)=(value*activity.resources.displayMetrics.density).toInt()
    private fun column()=LinearLayout(activity).apply { orientation=LinearLayout.VERTICAL }
    private fun dot(color:Int,lit:Boolean,description:String)=View(activity).apply {
        contentDescription=description;importantForAccessibility=View.IMPORTANT_FOR_ACCESSIBILITY_YES
        background=GradientDrawable().apply { shape=GradientDrawable.OVAL;setColor(if(lit)color else Color.rgb(39,45,53));setStroke(dp(1),if(lit)color else muted) }
        layoutParams=LinearLayout.LayoutParams(dp(10),dp(10)).apply { leftMargin=dp(9) }
    }
    private fun card()=column().apply {
        setPadding(dp(16),dp(12),dp(16),dp(12));background=GradientDrawable().apply { setColor(Color.rgb(16,19,24));cornerRadius=dp(22).toFloat();setStroke(dp(1),Color.rgb(39,45,53)) }
        layoutParams=LinearLayout.LayoutParams(-1,-2).apply { topMargin=dp(10) }
    }
    private fun text(value:String,size:Int,color:Int,bold:Boolean=false)=TextView(activity).apply {
        text=value;textSize=size.toFloat();setTextColor(color);typeface=Typeface.create("sans-serif",if(bold)Typeface.BOLD else Typeface.NORMAL);setPadding(0,dp(4),0,dp(4))
    }
    private fun button(value:String,action:()->Unit)=Button(activity).apply {
        text=value;isAllCaps=false;textSize=14f;minHeight=dp(48);minimumHeight=dp(48);minimumWidth=0;minWidth=0
        setTextColor(ColorStateList(arrayOf(intArrayOf(-android.R.attr.state_enabled),intArrayOf()),intArrayOf(muted,ink)))
        setPadding(dp(10),dp(10),dp(10),dp(10));backgroundTintList=null
        background=GradientDrawable().apply { setColor(Color.rgb(23,27,33));cornerRadius=dp(15).toFloat();setStroke(dp(1),Color.rgb(39,45,53)) }
        layoutParams=LinearLayout.LayoutParams(-1,-2).apply { topMargin=dp(6) };setOnClickListener { action() }
    }
    private fun toggle(value:String,enabled:Boolean,action:(Boolean)->Unit)=Switch(activity).apply {
        text=value;textSize=14f;setTextColor(ink);minHeight=dp(56);isChecked=enabled;switchPadding=dp(12)
        setOnCheckedChangeListener { _,checked->if(!settingRemove)action(checked) }
    }
    private fun selection(action:(Int)->Unit)=object:AdapterView.OnItemSelectedListener {
        override fun onNothingSelected(parent:AdapterView<*>?){}
        override fun onItemSelected(parent:AdapterView<*>?,view:View?,position:Int,id:Long){action(position)}
    }
    private fun adapter(values:List<String>)=object:ArrayAdapter<String>(activity,android.R.layout.simple_spinner_item,values) {
        override fun getView(position:Int,convertView:View?,parent:ViewGroup)=text(values[position],14,ink).apply { gravity=Gravity.CENTER_VERTICAL;minHeight=dp(48);setPadding(dp(8),dp(8),dp(8),dp(8)) }
        override fun getDropDownView(position:Int,convertView:View?,parent:ViewGroup)=text(values[position],14,ink).apply {
            minHeight=dp(48);gravity=Gravity.CENTER_VERTICAL;setPadding(dp(16),dp(10),dp(16),dp(10));setBackgroundColor(Color.rgb(23,27,33))
        }
    }
}
