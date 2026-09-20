package org.openpendant.app

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.View
import android.view.ViewGroup
import android.widget.*

/** Local drafts only until an explicit, acknowledged pendant write. */
internal class DeviceSettingsSection(private val activity:Activity,private val client:PendantClient,
    private val otherBusy:()->Boolean) {
    private val ink=Color.rgb(246,247,242)
    private val muted=Color.rgb(162,174,190)
    private val panel=Color.rgb(23,31,41)
    private val edits=client.preferencesDraft
    private var renderedGeneration=-1
    private var notice:String?=null
    private var ledExpanded=false
    private var safetyExpanded=false
    private var confirmation:AlertDialog?=null
    private val colors=listOf("Off","Red","Green","Blue","Yellow","Cyan","Purple","White")
    val view=column()
    private val status=text("Connect to read pendant settings.",14,muted)
    private val read=button("Read pendant settings") {
        notice=if(client.canReadPreferences&&!otherBusy())null else "Wait for the current pendant check, then read again."
        if(notice==null)client.readPreferences()
        render()
    }
    private val fields=column()
    private val save=button("Save on pendant") { confirm() }
    private val saveStatus=text("",14,muted)
    private val discard=button("Discard unsaved changes") { edits.discard();notice=null;render() }
    init {
        view.background=shape();view.setPadding(dp(18),dp(18),dp(18),dp(18))
        view.addView(text("Lights & battery",21,ink,true));view.addView(status);view.addView(read)
        view.addView(fields);view.addView(saveStatus);view.addView(save);view.addView(discard)
        render()
    }
    fun render() {
        val value=client.devicePreferences.takeIf { client.preferencesSupported }
        edits.observe(client.preferencesPeer,value)
        if(renderedGeneration!=edits.generation){
            fields.removeAllViews();edits.draft?.let { buildFields(it) };renderedGeneration=edits.generation
        }
        val saving=client.preferencesSave
        val available=client.canReadPreferences&&!otherBusy()
        read.isEnabled=available
        // A dashboard status read must not disable a user's Save tap. The
        // client serializes the explicitly confirmed request after that read.
        save.isEnabled=client.preferencesSupported&&!otherBusy()&&!saving.busy&&
            saving.phase!=DeviceSettingsSave.Phase.UNKNOWN&&edits.loaded!=null&&edits.dirty&&!edits.conflict
        save.visibility=if(edits.draft==null)View.GONE else View.VISIBLE
        saveStatus.visibility=save.visibility
        save.stableText=when(saving.phase){
            DeviceSettingsSave.Phase.CHECKING -> "Checking pendant…"
            DeviceSettingsSave.Phase.WRITING,DeviceSettingsSave.Phase.VERIFYING -> "Saving…"
            else -> "Save on pendant"
        }
        discard.visibility=if(edits.dirty)View.VISIBLE else View.GONE
        discard.isEnabled=edits.loaded!=null&&!saving.busy
        val description=when {
            saving.busy -> saving.message!!
            notice!=null -> notice!!
            !client.ready&&edits.dirty -> "Reconnect and read settings. Your unsaved changes are kept."
            !client.ready -> "Connect to read pendant settings."
            !client.preferencesSupported -> "These controls need newer pendant firmware."
            value==null -> "Read the current settings before making changes."
            saving.phase==DeviceSettingsSave.Phase.UNKNOWN -> saving.message!!
            edits.conflict -> "Pendant settings changed. Your edits are kept. Discard them to load the current settings, then review your changes."
            edits.dirty&&saving.phase==DeviceSettingsSave.Phase.NOT_SENT&&saving.request==edits.draft -> saving.message!!
            otherBusy() -> "Finish the current pendant operation first. Your changes are kept."
            edits.dirty -> "Unsaved changes · tap Save on pendant below."
            saving.phase==DeviceSettingsSave.Phase.SAVED -> saving.message!!
            else -> "Saved on pendant · changes work without your phone."
        }
        status.stableText=description;saveStatus.stableText=description
        enableTree(fields,!saving.busy&&!otherBusy()&&edits.draft!=null)
    }
    private fun buildFields(p:DevicePreferences) {
        fields.addView(text("Battery profile",16,ink,true))
        fields.addView(chooser("Battery profile",listOf("Responsive","Balanced","Battery saver"),p.profile) {
            edit { copy(profile=it) }
        })
        fields.addView(chooser("Enter idle mode",listOf("Never slow discovery","After 1 minute","After 5 minutes","After 15 minutes"),p.idleDelay) {
            edit { copy(idleDelay=it) }
        })
        fields.addView(text("Idle mode slows Bluetooth discovery when disconnected: responsive ~30 ms, balanced ~250 ms, saver ~1 second. Recording and transfer quality stay unchanged. Battery-life gains are not measured yet.",13,muted))
        val standalone=client.longPeer()?.capabilityBits?.and(LongRecordingControlCodec.STANDALONE_CAPABILITY)==LongRecordingControlCodec.STANDALONE_CAPABILITY
        fields.addView(text(if(standalone)
            "Sleep: Balanced and Battery saver use low-power standby after the idle delay, on battery and disconnected. A short button tap wakes and starts recording. Bluetooth discovery and battery checks remain available; USB recovery is unchanged. No reboot or app permission is needed."
            else "Sleep: light idle only on this firmware. Deep power-off is not enabled; USB recovery stays available.",13,muted))
        val led=column().apply { visibility=if(ledExpanded)View.VISIBLE else View.GONE }
        fields.addView(button("LED colors & meaning  ▾") { ledExpanded=!ledExpanded;led.visibility=if(ledExpanded)View.VISIBLE else View.GONE })
        fields.addView(led)
        fun color(label:String,current:Int,required:Boolean,change:DevicePreferences.(Int)->DevicePreferences) {
            led.addView(text(label,14,ink,true))
            val first=if(required)1 else 0
            led.addView(chooser(label,colors.drop(first),current-first) { index -> edit { change(index+first) } })
        }
        color("Recording · steady light",p.recording,true){copy(recording=it)}
        color("Low battery · brief blinking",p.lowBattery,true){copy(lowBattery=it)}
        color("Bluetooth connected",p.connected,false){copy(connected=it)}
        color("USB connected · not charging status",p.usb,false){copy(usb=it)}
        color("Device fault",p.fault,true){copy(fault=it)}
        led.addView(text("Recording takes priority, then faults, low battery, Bluetooth and USB. Recording cannot be made invisible.",13,muted))
        val brightness=text("Brightness: ${p.brightness} / 64",14,ink)
        led.addView(brightness)
        led.addView(SeekBar(activity).apply {
            max=56;progress=p.brightness-8;minimumHeight=dp(48);contentDescription="LED brightness, 8 to 64"
            setOnSeekBarChangeListener(object:SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar:SeekBar?,value:Int,fromUser:Boolean){if(fromUser){
                    brightness.text="Brightness: ${value+8} / 64";edit { copy(brightness=value+8) }
                }}
                override fun onStartTrackingTouch(bar:SeekBar?)=Unit
                override fun onStopTrackingTouch(bar:SeekBar?)=Unit
            })
        })
        val percentages=(listOf(20,25,30,40,50)+p.lowPercent).distinct().sorted()
        led.addView(chooser("Low-battery LED threshold",percentages.map { "Low battery light: $it%" },percentages.indexOf(p.lowPercent)) {
            index -> edit { copy(lowPercent=percentages[index]) }
        })
        led.addView(text("This threshold controls the light only. Recording still follows the fixed battery safety checks.",13,muted))
        val safety=column().apply { visibility=if(safetyExpanded)View.VISIBLE else View.GONE }
        fields.addView(button("Current power-saving behavior  ▾") { safetyExpanded=!safetyExpanded;safety.visibility=if(safetyExpanded)View.VISIBLE else View.GONE })
        fields.addView(safety)
        safety.addView(text("Microphones turn off after recording. The CPU returns to its previous lower-speed setting after audio work. Idle code waits between checks; safety monitoring remains active. Low or stale battery readings trigger a bounded attempt to stop and save. Sudden power loss can still lose the final audio. Supported firmware can sync on battery after fresh power checks.",13,muted))
    }
    private fun edit(change:DevicePreferences.()->DevicePreferences){edits.edit(change);notice=null;render()}
    private fun confirm() {
        if(confirmation?.isShowing==true)return
        val value=edits.draft?:return
        val base=edits.loaded
        val epoch=client.currentRadioEpoch()
        if(!client.preferencesSupported||base==null||edits.conflict||otherBusy()||!edits.dirty||
            client.preferencesSave.busy||client.preferencesSave.phase==DeviceSettingsSave.Phase.UNKNOWN){
            notice="Not saved. Read current settings and finish any active pendant operation first.";render();return
        }
        notice=null
        confirmation=AlertDialog.Builder(activity).setTitle("Save pendant settings?")
            .setMessage("Keep the pendant nearby and idle until saving finishes. Supported firmware can save on battery after fresh power checks. Recordings, pairing and keys are kept.")
            .setNegativeButton("Cancel",null).setPositiveButton("Save"){_,_->
                if(otherBusy()||epoch!=client.currentRadioEpoch()||client.devicePreferences!=base||edits.draft!=value){
                    notice="Not saved. Pendant state changed while confirmation was open. Read settings and try again."
                }else if(!client.savePreferences(value))notice=client.preferencesSave.message?:"Not saved. Read settings and try again."
                render()
            }.create().also{it.setOnDismissListener{confirmation=null};it.show()}
    }
    private fun chooser(name:String,labels:List<String>,selected:Int,change:(Int)->Unit):Spinner = Spinner(activity).apply {
        minimumHeight=dp(48);contentDescription=name
        adapter=object:ArrayAdapter<String>(activity,android.R.layout.simple_spinner_item,labels){
            override fun getView(position:Int,convertView:View?,parent:ViewGroup):View=
                super.getView(position,convertView,parent).also { (it as TextView).apply { setTextColor(ink);textSize=14f;isSingleLine=false;maxLines=3;setPadding(dp(4),dp(10),dp(22),dp(10)) } }
            override fun getDropDownView(position:Int,convertView:View?,parent:ViewGroup):View=
                super.getDropDownView(position,convertView,parent).also { (it as TextView).apply { setTextColor(ink);setBackgroundColor(panel);textSize=15f;isSingleLine=false;maxLines=3;minHeight=dp(48);setPadding(dp(14),dp(12),dp(14),dp(12)) } }
        }
        setSelection(selected)
        onItemSelectedListener=object:AdapterView.OnItemSelectedListener {
            private var previous=selected
            override fun onNothingSelected(parent:AdapterView<*>?)=Unit
            override fun onItemSelected(parent:AdapterView<*>?,view:View?,position:Int,id:Long){if(position!=previous){previous=position;change(position)}}
        }
    }
    private fun enableTree(view:View,enabled:Boolean){view.isEnabled=enabled;if(view is ViewGroup)for(i in 0 until view.childCount)enableTree(view.getChildAt(i),enabled)}
    private fun dp(value:Int)=(value*activity.resources.displayMetrics.density).toInt()
    private fun column()=LinearLayout(activity).apply { orientation=LinearLayout.VERTICAL }
    private fun shape()=GradientDrawable().apply { setColor(panel);cornerRadius=dp(22).toFloat() }
    private fun text(value:String,size:Int,color:Int,bold:Boolean=false)=TextView(activity).apply {
        text=value;textSize=size.toFloat();setTextColor(color);setPadding(0,dp(6),0,dp(6));if(bold)setTypeface(typeface,Typeface.BOLD)
    }
    private fun button(value:String,action:()->Unit)=Button(activity).apply {
        text=value;isAllCaps=false;setTextColor(ink);textSize=14f;minHeight=dp(48);setOnClickListener{action()}
    }
}
