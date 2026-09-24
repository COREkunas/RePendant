package org.openpendant.app

import android.app.Activity
import android.app.AlertDialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.InputFilter
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.*

/** Account controls live in Settings; this view is a conversation workspace. */
internal class MindyLinkSection(private val activity:Activity,private val cloud:MindyLinkController,
    private val openSettings:()->Unit={}) {
    private val ink=Color.rgb(239,244,245);private val muted=Color.rgb(153,166,182)
    private val green=Color.rgb(114,214,172);private val surface=Color.rgb(28,37,48)
    val view=column()
    val settingsView=column()
    private val status=label("")
    private val chatNotice=label("",13f).apply{maxLines=2;ellipsize=android.text.TextUtils.TruncateAt.END}
    private val loginPanel=column();private val signedPanel=column()
    private val server=field("HTTPS gateway","https://forge.mindylab.com")
    private val email=field("Email").apply{inputType=InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS}
    private val password=field("Password").apply{inputType=InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD}
    private val pcs=Spinner(activity);private val hosts=Spinner(activity);private val models=Spinner(activity)
    private val language=Spinner(activity);private val conversations=Spinner(activity)
    private val automatic=Switch(activity).apply{text="Automatically transcribe new phone recordings";setTextColor(ink);minHeight=dp(48)}
    private val question=field("Message…").apply{minLines=2;maxLines=5;inputType=InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE;filters=arrayOf(InputFilter.LengthFilter(12000))}
    private val modelPanel=column().apply{visibility=View.GONE}
    private val modelToggle=button("Choose model"){showModels()}.apply{maxLines=1;ellipsize=android.text.TextUtils.TruncateAt.END}
    private val messages=column()
    private val history=ScrollView(activity).apply{addView(messages);isFillViewport=true}
    private val contextLabel=label("No recordings included",12f).apply{maxLines=2;ellipsize=android.text.TextUtils.TruncateAt.END}
    private val chooseContext=button("Recordings…"){pickRecordings()}
    private val thinking=LinearLayout(activity).apply{orientation=LinearLayout.HORIZONTAL;gravity=Gravity.CENTER_VERTICAL;visibility=View.GONE}
    private val thinkingLabel=label("",14f)
    private val send=button("Send"){if(cloud.chat(question.text.toString(),chosen.toSet()))question.setText("")}
    private val stop=button("Stop waiting"){cloud.stopChat()}.apply{visibility=View.GONE}
    private val newChat=button("New chat"){cloud.newChat(question.text.toString(),chosen.toSet())}
    private val manageChat=button("⋮"){manageChat()}.apply{contentDescription="Chat options";minWidth=0}
    private val settingsLink=button("Sign in in Settings"){openSettings()}
    private val chosen=linkedSetOf<String>()
    private var pcIds=listOf("");private var hostIds=listOf("");private var modelIds=listOf("");private var chatIds=listOf("")
    private var renderedDevices="";private var renderedModels="";private var renderedChats="";private var renderedConversation=-1
    private var updating=false
    private val accountButtons=mutableListOf<Button>();private val modelButtons=mutableListOf<Button>()
    init {
        settingsView.addView(label("MindyLink",21f));settingsView.addView(status)
        settingsView.addView(label("Optional PC transcription and AI chat. The selected PC receives readable audio or text; the gateway relays encrypted content. Pendant recovery keys stay on this phone.",13f))
        settingsView.addView(loginPanel);listOf(server,email,password).forEach(loginPanel::addView)
        accountButtons+=button("Sign in"){login(false)}.also(loginPanel::addView)
        accountButtons+=button("Create account"){login(true)}.also(loginPanel::addView)
        settingsView.addView(signedPanel)
        accountButtons+=button("Refresh PCs / jobs"){cloud.refresh()}.also(signedPanel::addView)
        signedPanel.addView(label("Transcription PC",17f));signedPanel.addView(pcs)
        signedPanel.addView(label("Transcription language",14f));signedPanel.addView(language)
        language.adapter=adapter(listOf("Lithuanian","English","Auto detect"))
        accountButtons+=button("Save transcription settings"){cloud.configure(pcIds.getOrElse(pcs.selectedItemPosition){""},cloud.host,cloud.model,listOf("lt","en","auto")[language.selectedItemPosition.coerceIn(0,2)])}.also(signedPanel::addView)
        signedPanel.addView(automatic)
        signedPanel.addView(label("Keep OpenPendant open during upload. Forge can continue transcribing after you leave. Results return when this app is open again.",13f))
        automatic.setOnCheckedChangeListener {_,on->if(!updating){
            updating=true;automatic.isChecked=cloud.automatic;updating=false
            if(on)AlertDialog.Builder(activity).setTitle("Enable automatic PC transcription?")
                .setMessage("New complete phone recordings will be sent to your selected PC while OpenPendant is open. Existing recordings are excluded. Details-only sync never downloads audio for this. Interrupted uploads need an explicit retry.")
                .setNegativeButton("Cancel",null).setPositiveButton("Enable"){_,_->cloud.setAutomatic(true)}.show()
            else cloud.setAutomatic(false)
        }}
        accountButtons+=button("Sign out"){cloud.logout()}.also(signedPanel::addView)
        view.addView(label("AI chat",28f));view.addView(chatNotice);view.addView(settingsLink)
        val toolbar=LinearLayout(activity).apply{orientation=LinearLayout.HORIZONTAL;gravity=Gravity.CENTER_VERTICAL}
        conversations.contentDescription="Choose conversation";conversations.minimumHeight=dp(48)
        toolbar.addView(conversations,LinearLayout.LayoutParams(0,-2,1f));toolbar.addView(newChat);toolbar.addView(manageChat,LinearLayout.LayoutParams(dp(48),-2))
        view.addView(toolbar)
        conversations.onItemSelectedListener=object:AdapterView.OnItemSelectedListener {
            override fun onNothingSelected(parent:AdapterView<*>?)=Unit
            override fun onItemSelected(parent:AdapterView<*>?,item:View?,position:Int,id:Long){
                val selected=chatIds.getOrNull(position)?:return
                if(!updating&&selected.isNotBlank()&&selected!=cloud.currentChat?.id)cloud.selectChat(selected,question.text.toString(),chosen.toSet())
            }
        }
        view.addView(modelToggle)
        modelPanel.addView(label("Model host",14f));modelPanel.addView(hosts)
        modelButtons+=button("Refresh available PCs"){cloud.refresh()}.also(modelPanel::addView)
        modelButtons+=button("Load models from selected host"){cloud.loadModels(hostIds.getOrElse(hosts.selectedItemPosition){""})}.also(modelPanel::addView)
        modelPanel.addView(models)
        modelButtons+=button("Use selected model"){cloud.configure(cloud.pc,hostIds.getOrElse(hosts.selectedItemPosition){""},modelIds.getOrElse(models.selectedItemPosition){""},cloud.language)}.also(modelPanel::addView)
        val contextRow=LinearLayout(activity).apply{orientation=LinearLayout.HORIZONTAL;gravity=Gravity.CENTER_VERTICAL}
        contextRow.addView(chooseContext);contextRow.addView(contextLabel,LinearLayout.LayoutParams(0,-2,1f).apply{marginStart=dp(8)})
        view.addView(contextRow)
        view.addView(history,LinearLayout.LayoutParams(-1,0,1f))
        thinking.addView(ProgressBar(activity).apply{indeterminateTintList=ColorStateList.valueOf(green)},LinearLayout.LayoutParams(dp(22),dp(22)).apply{marginEnd=dp(12)})
        thinking.addView(thinkingLabel,LinearLayout.LayoutParams(0,-2,1f));view.addView(thinking)
        view.addView(question)
        val actions=LinearLayout(activity).apply{orientation=LinearLayout.HORIZONTAL}
        actions.addView(send,LinearLayout.LayoutParams(0,-2,1f));actions.addView(stop,LinearLayout.LayoutParams(0,-2,1f));view.addView(actions)
        view.addView(label("Selected transcripts + this chat’s history. AI can make mistakes.",11f))
        render()
    }
    private fun login(signup:Boolean){val secret=password.text.toString().toCharArray();password.setText("");cloud.login(server.text.toString(),email.text.toString(),secret,signup)}
    fun persistDraft(){cloud.persistChatDraft(question.text.toString(),chosen.toSet())}
    fun render(){
        stable(status,cloud.message);stable(chatNotice,if(!cloud.signedIn)"Sign in in Settings to start chatting." else if(cloud.chatting)"" else cloud.message)
        chatNotice.visibility=if(cloud.chatting)View.GONE else View.VISIBLE
        settingsLink.visibility=if(cloud.signedIn)View.GONE else View.VISIBLE
        loginPanel.visibility=if(cloud.signedIn)View.GONE else View.VISIBLE;signedPanel.visibility=if(cloud.signedIn)View.VISIBLE else View.GONE
        accountButtons.forEach{it.isEnabled=cloud.canEdit};modelButtons.forEach{it.isEnabled=cloud.signedIn&&cloud.canEdit}
        automatic.isEnabled=cloud.canEdit
        updating=true;automatic.isChecked=cloud.automatic
        val signature=cloud.devices.toString()+cloud.pc+cloud.host+cloud.language
        if(signature!=renderedDevices){renderedDevices=signature
            val pc=cloud.devices.filter{it.role=="forge"&&it.approved&&it.online};pcIds=listOf("")+pc.map{it.id}
            pcs.adapter=adapter(listOf("Choose available PC")+pc.map{it.name});pcs.setSelection(pcIds.indexOf(cloud.pc).coerceAtLeast(0))
            val host=cloud.devices.filter{it.role=="model-host"&&it.approved&&it.online};hostIds=listOf("")+host.map{it.id}
            hosts.adapter=adapter(listOf("Choose model host")+host.map{it.name});hosts.setSelection(hostIds.indexOf(cloud.host).coerceAtLeast(0))
            language.setSelection(listOf("lt","en","auto").indexOf(cloud.language).coerceAtLeast(0))
        }
        val modelSignature=cloud.models.toString()+cloud.model
        if(modelSignature!=renderedModels){renderedModels=modelSignature;modelIds=listOf("")+cloud.models
            if(cloud.model.isNotBlank()&&cloud.model !in modelIds)modelIds=modelIds+cloud.model
            models.adapter=adapter(listOf("Choose model")+modelIds.drop(1));models.setSelection(modelIds.indexOf(cloud.model).coerceAtLeast(0))}
        stable(modelToggle,if(cloud.model.isBlank())"Choose AI model  ▾" else "${cloud.model}  ▾")
        modelToggle.isEnabled=cloud.signedIn&&cloud.canEdit
        val chatSignature=cloud.chats.toString()+cloud.currentChat?.id
        if(renderedChats!=chatSignature){renderedChats=chatSignature;chatIds=listOf("")+cloud.chats.map{it.id}
            conversations.adapter=adapter(listOf("Choose conversation")+cloud.chats.map{it.title});conversations.setSelection(chatIds.indexOf(cloud.currentChat?.id).coerceAtLeast(0))}
        updating=false
        if(renderedConversation!=cloud.chatVersion){renderedConversation=cloud.chatVersion
            val chat=cloud.currentChat;chosen.clear();chosen.addAll(chat?.selected?:emptyList());question.setText(chat?.draft?:"")
            messages.removeAllViews()
            if(chat==null)messages.addView(label("Create a new chat to get started.",16f))
            else if(chat.turns.isEmpty())messages.addView(label("Ask a question, or include recordings to discuss their transcripts.",16f))
            else chat.turns.forEach {turn->
                bubble("You",turn.prompt,true)
                if(turn.sources.isNotEmpty())messages.addView(label(turn.sources.mapIndexed{i,s->"[${i+1}] ${s.label}"}.joinToString(" · "),12f))
                when(turn.state){"complete"->bubble(turn.model,turn.reply,false);"failed","cancelled"->bubble(if(turn.state=="cancelled")"Stopped" else "Could not finish",turn.reply,false)}
            }
            history.post{history.fullScroll(View.FOCUS_DOWN)}
        }
        stable(contextLabel,if(chosen.isEmpty())"No recordings included" else chosen.map{cloud.texts[it]?.label?:"Unavailable recording"}.joinToString(" · "))
        newChat.isEnabled=cloud.signedIn&&cloud.canEdit;conversations.isEnabled=newChat.isEnabled
        manageChat.isEnabled=cloud.currentChat!=null&&cloud.canEdit;chooseContext.isEnabled=cloud.currentChat!=null&&cloud.canEdit
        question.isEnabled=cloud.currentChat!=null&&!cloud.chatting
        send.isEnabled=cloud.currentChat!=null&&cloud.signedIn&&cloud.canEdit&&cloud.host.isNotBlank()&&cloud.model.isNotBlank()
        thinking.visibility=if(cloud.chatting)View.VISIBLE else View.GONE;stop.visibility=thinking.visibility
        if(cloud.chatting)stable(thinkingLabel,"${cloud.chatStage} · ${((android.os.SystemClock.elapsedRealtime()-cloud.chatStartedAt)/1000).coerceAtLeast(0)}s")
    }
    private fun pickRecordings(){
        val docs=cloud.texts.values.toList();if(docs.isEmpty()){toast("Transcribe a saved recording first. Its transcript will appear here.");return}
        val picked=chosen.toMutableSet()
        val dialog=AlertDialog.Builder(activity).setTitle("Include transcripts · earlier messages are kept")
            .setMultiChoiceItems(docs.map{it.label}.toTypedArray(),BooleanArray(docs.size){docs[it].key in picked}){dialog,index,on->
                if(on&&picked.size>=20){(dialog as AlertDialog).listView.setItemChecked(index,false);toast("Include up to 20 recordings.")}
                else if(on)picked.add(docs[index].key)else picked.remove(docs[index].key)}
            .setNegativeButton("Cancel",null).setNeutralButton("Clear selection"){_,_->chosen.clear();persistDraft();render()}
            .setPositiveButton("Include"){_,_->chosen.clear();chosen.addAll(picked);persistDraft();render()}.create()
        dialog.show()
    }
    private fun showModels(){
        (modelPanel.parent as? ViewGroup)?.removeView(modelPanel);modelPanel.visibility=View.VISIBLE
        AlertDialog.Builder(activity).setTitle("AI model").setView(ScrollView(activity).apply{setPadding(dp(16),dp(8),dp(16),dp(8));addView(modelPanel)})
            .setPositiveButton("Done",null).show()
    }
    private fun manageChat(){val chat=cloud.currentChat?:return
        AlertDialog.Builder(activity).setTitle(chat.title).setItems(arrayOf("Rename chat","Delete chat from phone")){_,index->
            if(index==0)editName("Chat name",chat.title){cloud.renameChat(it,question.text.toString(),chosen.toSet())}
            else AlertDialog.Builder(activity).setTitle("Delete this chat?").setMessage("Only this conversation on the phone is removed. Recordings, transcripts and PC jobs are kept.")
                .setNegativeButton("Keep",null).setPositiveButton("Delete chat"){_,_->cloud.deleteChat()}.show()
        }.show()
    }
    fun recordingActions(parent:LinearLayout,row:RecordingSyncSnapshot,allowed:Boolean){
        parent.addView(button("Rename recording…"){editName("Recording name",cloud.recordingLabel(row)){cloud.renameRecording(row,it)}}.apply{isEnabled=cloud.canEdit})
        val transcript=cloud.transcript(row)
        cloud.jobs[MindyLinkStore.recordingKey(row.recording)]?.let{parent.addView(label(it))}
        parent.addView(button(if(transcript==null)"Transcribe on PC / resume" else "View transcript"){
            if(transcript!=null)cloud.preview(row){text->if(!activity.isFinishing&&!activity.isDestroyed)
                AlertDialog.Builder(activity).setTitle(transcript.label).setMessage(text+"\n\nModel: "+transcript.model+"\n"+transcript.warnings).setPositiveButton("Close",null).show()}
            else if(!cloud.signedIn||cloud.pc.isBlank())toast("Sign in and choose a transcription PC in Settings → MindyLink.")
            else AlertDialog.Builder(activity).setTitle("Transcribe on selected PC?")
                .setMessage("Send this phone recording to ${cloud.devices.find{it.id==cloud.pc}?.name?:"the saved PC"}? Audio is encrypted in transit; the PC processes it locally. Phone and pendant copies are kept. Your recovery key stays here.")
                .setNegativeButton("Cancel",null).setPositiveButton("Send recording"){_,_->cloud.transcribe(row)}.show()
        }.apply{isEnabled=transcript!=null||(allowed&&!cloud.busy)})
        parent.addView(button("Cancel / remove PC job"){
            AlertDialog.Builder(activity).setTitle("Remove this recording's PC job?")
                .setMessage("Cancel transcription and remove staged audio and transcript on the PC. Phone recordings and saved phone transcript are kept. The PC must be online.")
                .setNegativeButton("Keep",null).setPositiveButton("Remove PC job"){_,_->cloud.cancelRemote(row)}.show()
        }.apply{isEnabled=cloud.signedIn&&!cloud.busy})
    }
    private fun editName(title:String,current:String,save:(String)->Unit){val input=field(title,current).apply{filters=arrayOf(InputFilter.LengthFilter(120))}
        val dialog=AlertDialog.Builder(activity).setTitle(title).setView(input).setNegativeButton("Cancel",null).setPositiveButton("Save",null).create();dialog.show()
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener{try{save(PendantChatRules.name(input.text.toString()));dialog.dismiss()}catch(e:IllegalArgumentException){input.error=e.message}}
    }
    private fun bubble(title:String,value:String,user:Boolean){
        val block=column().apply{setPadding(dp(14),dp(8),dp(14),dp(12));background=GradientDrawable().apply{setColor(if(user)Color.rgb(32,65,58) else surface);cornerRadius=dp(14).toFloat()}}
        block.addView(label(title,12f).apply{setTextColor(if(user)green else muted)})
        block.addView(label(value,16f).apply{setTextIsSelectable(true)})
        messages.addView(block,LinearLayout.LayoutParams(-1,-2).apply{topMargin=dp(8);if(user)marginStart=dp(24)else marginEnd=dp(24)})
    }
    private fun dp(value:Int)=(value*activity.resources.displayMetrics.density).toInt()
    private fun stable(view:TextView,text:String){if(view.text.toString()!=text)view.text=text}
    private fun adapter(items:List<String>)=ArrayAdapter(activity,android.R.layout.simple_spinner_dropdown_item,items)
    private fun column()=LinearLayout(activity).apply{orientation=LinearLayout.VERTICAL}
    private fun label(value:String,size:Float=14f)=TextView(activity).apply{text=value;textSize=size;setTextColor(ink);setPadding(0,dp(8),0,dp(8));if(size>=20)setTypeface(null,Typeface.BOLD)}
    private fun field(hint:String,value:String="")=EditText(activity).apply{this.hint=hint;setText(value);setTextColor(ink);setHintTextColor(muted);minimumHeight=dp(48)}
    private fun button(text:String,action:()->Unit)=Button(activity).apply{this.text=text;isAllCaps=false;minHeight=dp(48);setOnClickListener{action()}}
    private fun toast(text:String)=Toast.makeText(activity,text,Toast.LENGTH_LONG).show()
}
