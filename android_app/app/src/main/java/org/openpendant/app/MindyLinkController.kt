package org.openpendant.app

import android.content.Context
import android.os.Handler
import android.os.Looper
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

internal data class RemoteTranscript(val key:String,val label:String,val text:String,val source:String,val model:String,val warnings:String)
internal class MindyLinkController(context:Context,private val library:AndroidDurableLibrary,private val changed:()->Unit) {
    private val store=MindyLinkStore(context.applicationContext)
    private val worker=Executors.newSingleThreadExecutor()
    private val main=Handler(Looper.getMainLooper())
    private val working=AtomicBoolean()
    @Volatile private var account:MindyLinkAccount?=null
    @Volatile private var client:MindyLinkClient?=null
    @Volatile private var prefs=JSONObject()
    @Volatile var devices=emptyList<MindyLinkDevice>();private set
    @Volatile var models=emptyList<String>();private set
    private var modelsHost=""
    @Volatile var texts=emptyMap<String,RemoteTranscript>();private set
    @Volatile var jobs=emptyMap<String,String>();private set
    @Volatile var message="MindyLink is optional. Recording and phone sync work without an account.";private set
    @Volatile var answer="";private set
    @Volatile var email="";private set
    @Volatile var version=0;private set
    @Volatile var names=emptyMap<String,String>();private set
    @Volatile var chats=emptyList<PendantChatSummary>();private set
    @Volatile var currentChat:PendantChat?=null;private set
    @Volatile var chatVersion=0;private set
    @Volatile var chatStage="";private set
    @Volatile var chatStartedAt=0L;private set
    private val chatCancelled=AtomicBoolean()
    val chatting get()=chatStartedAt!=0L
    private var autoSeen=emptySet<String>()
    private var localSignature=""
    private var lastPoll=0L
    private var foreground=false
    private val monitor=object:Runnable {override fun run(){if(!foreground)return;observeForeground();main.postDelayed(this,5000)}}
    fun foreground(){foreground=true;main.removeCallbacks(monitor);main.post(monitor)}
    fun background(){foreground=false;main.removeCallbacks(monitor)}
    val busy get()=working.get()
    val canEdit get()=!busy&&!library.busy
    val signedIn get()=client!=null
    val pc get()=prefs.optString("pc")
    val host get()=prefs.optString("host")
    val model get()=prefs.optString("model")
    val language get()=prefs.optString("language","lt")
    val automatic get()=prefs.optBoolean("automatic",false)
    private fun tell(value:String){message=value;main.post(changed)}
    init { run("Loading MindyLink settings…") {
        prefs=store.read("preferences")?:JSONObject()
        val saved=store.read("account")
        if(saved!=null){account=MindyLinkAccount.read(saved);email=account!!.email;client=MindyLinkClient(account!!)}
        val baseline=prefs.optJSONArray("autoSeen")?:JSONArray();autoSeen=(0 until baseline.length()).map {baseline.getString(it)}.toSet()
        loadTexts();if(signedIn)loadChats();tell(if(signedIn)"Signed in as $email. Refresh to find available PCs." else "Sign in in Settings to use PC transcription and AI chat.")
    } }
    private fun run(progress:String,action:()->Unit){
        if(!working.compareAndSet(false,true))return
        tell(progress)
        worker.execute {try{action()}catch(e:Exception){tell(e.message?.take(240)?:"MindyLink could not finish. Retry when connected.")}
            finally{working.set(false);main.post(changed)}}
    }
    private fun scope(a:MindyLinkAccount)=MindyLinkProtocol.sha("${a.server}\u0000${a.email}\u0000${a.device}".toByteArray())
    fun login(server:String,email:String,password:CharArray,signup:Boolean){
        if(busy||library.busy){password.fill('\u0000');return}
        run(if(signup)"Creating MindyLink account…" else "Signing in…") {
            try {
                val identity=MindyLinkProtocol.sha("${MindyLinkProtocol.server(server)}\u0000${MindyLinkProtocol.email(email)}".toByteArray())
                val ids=store.read("device-identities")?:JSONObject()
                val id=ids.optString(identity).ifBlank {UUID.randomUUID().toString().also {ids.put(identity,it);store.save("device-identities",ids)}}
                val next=MindyLinkClient.authenticate(server,email,password,signup,id,"OpenPendant · ${android.os.Build.MODEL}")
                store.save("account",next.json());client?.close();account=next;client=MindyLinkClient(next);this.email=next.email
                prefs=JSONObject().put("language","lt");store.save("preferences",prefs);models=emptyList();answer=""
                loadChats()
                devices=client!!.devices();tell("Signed in. Choose an approved online PC. A new phone may need approval in MindyLink.")
            }finally{password.fill('\u0000')}
        }
    }
    fun logout(){if(library.busy||busy)return;run("Signing out…"){
        client?.close();client=null;account=null;email="";devices=emptyList();models=emptyList();answer=""
        currentChat=null;chats=emptyList();chatVersion++
        store.remove("account");prefs=JSONObject().put("automatic",false);store.save("preferences",prefs)
        tell("Signed out. Phone recordings and transcripts remain. Uploaded PC jobs are not erased by signing out.")
    }}
    fun configure(pc:String,host:String,model:String,language:String){if(busy||library.busy)return
        require(language in listOf("lt","en","auto"))
        run("Saving MindyLink choices…") {
            require(model.isBlank()||(host==modelsHost&&model in models)||(host==this.host&&model==this.model)) {"Load models from the chosen host before selecting one."}
            val next=JSONObject(prefs.toString()).put("pc",pc).put("host",host).put("model",model).put("language",language)
            if(pc!=this.pc)next.put("automatic",false)
            store.save("preferences",next);prefs=next;tell("Choices saved. Audio goes only to the selected PC.")
        }
    }
    fun setAutomatic(enabled:Boolean){if(busy||library.busy)return
        val baseline=library.state.recordings.filter(RecordingListPresentation::phoneComplete).map { MindyLinkStore.recordingKey(it.recording) }.toSet()
        run("Saving automatic transcription…"){
            check(!enabled||pc.isNotBlank()&&signedIn) {"Choose a transcription PC first."}
            val next=JSONObject(prefs.toString()).put("automatic",enabled).put("autoSeen",JSONArray(baseline.toList()))
            store.save("preferences",next);prefs=next;autoSeen=baseline
            tell(if(enabled)"Auto transcription enabled for new complete phone copies while the app is open. Existing recordings are not uploaded." else "Automatic transcription off.")
        }
    }
    fun refresh(){if(library.busy)return;val rows=library.state.recordings.toList();run("Checking MindyLink PCs and jobs…") {
        val c=checkNotNull(client){"Sign in first."};devices=c.devices()
        for(row in rows){val key=MindyLinkStore.recordingKey(row.recording);val job=store.read("job-$key")?:continue
            if(!job.optBoolean("suppressed")&&job.optString("phase")!="completed"&&job.optString("scope")==scope(checkNotNull(account))&&
                devices.any {it.id==job.optString("pc")&&it.approved&&it.online})checkResult(c,key,job)
        }
        loadTexts();tell("PC list and transcript jobs updated. ${texts.size} transcripts on this phone.")
    }}
    fun loadModels(selectedHost:String){run("Loading models from selected PC…") {
        val c=checkNotNull(client){"Sign in first."}
        require(devices.any {it.id==selectedHost&&it.approved&&it.online&&it.role=="model-host"}) {"Choose an online model host."}
        val data=c.rpc(selectedHost,"models.list",JSONObject().put("api","openai"),true).getJSONArray("data")
        require(data.length()<=1000);models=(0 until data.length()).map {data.getJSONObject(it).getString("id")}.distinct()
        modelsHost=selectedHost
        // Choosing a host/model is still explicit; no fallback to another device.
        tell("${models.size} models available. Select one and save choices.")
    }}
    private fun chatStore()=MindyLinkChats(scope(checkNotNull(account)),store::read,store::save,store::remove)
    private fun publishChat(chat:PendantChat?) {currentChat=chat;chats=chatStore().list();chatVersion++;main.post(changed)}
    private fun loadChats() {
        val repo=chatStore();val chat=repo.active()
        // Process death never silently repeats a model request.
        val restored=chat?.copy(turns=chat.turns.map {if(it.state=="pending")it.copy(state="failed",reply="Interrupted. Send a new message to try again.") else it})
        if(restored!=null&&restored!=chat)repo.save(restored)
        publishChat(restored)
    }
    private fun saveDraft(draft:String,selected:Set<String>) {currentChat?.let {chatStore().save(it.copy(draft=draft.take(12000),selected=selected.toList()))}}
    fun persistChatDraft(draft:String,selected:Set<String>) {
        val id=currentChat?.id?:return;val a=account?:return;if(chatting)return
        worker.execute {
            if(account!==a||currentChat?.id!=id||chatting)return@execute
            try {val repo=chatStore();val next=repo.load(id).copy(draft=draft.take(12000),selected=selected.toList());repo.save(next);currentChat=next}
            catch(_:Exception){tell("Chat draft could not be saved. Keep this screen open and try again.")}
        }
    }
    fun newChat(draft:String,selected:Set<String>) {if(busy||library.busy||!signedIn)return
        run("Creating chat…"){saveDraft(draft,selected);publishChat(chatStore().create());tell("New chat. Choose recordings to include, or ask a question without recordings.")}}
    fun selectChat(id:String,draft:String,selected:Set<String>){if(busy||library.busy||!signedIn||currentChat?.id==id)return
        run("Opening chat…"){saveDraft(draft,selected);val next=chatStore().load(id);chatStore().save(next);publishChat(next);tell("Chat opened.")}}
    fun renameChat(name:String,draft:String,selected:Set<String>){if(busy||library.busy||!signedIn)return
        val chat=currentChat?:return
        run("Renaming chat…"){val next=chat.copy(title=PendantChatRules.name(name),draft=draft.take(12000),selected=selected.toList());chatStore().save(next);publishChat(next);tell("Chat renamed.")}}
    fun deleteChat(){if(busy||library.busy||!signedIn)return;val chat=currentChat?:return
        run("Deleting local chat…"){chatStore().delete(chat.id);publishChat(chatStore().active());tell("Chat deleted from this phone. Recordings and transcripts were kept.")}}
    fun stopChat(){if(!chatting)return;chatCancelled.set(true);client?.cancelModelRequest();chatStage="Stopping…";main.post(changed)}
    fun chat(prompt:String,selected:Set<String>):Boolean {
        if(library.busy||busy)return false
        val a=account?:return false;val c=client?:return false;val original=currentChat?:return false
        val selectedHost=host;val selectedModel=model
        if(selectedHost.isBlank()||selectedModel.isBlank()){tell("Choose a model host and model in AI chat first.");return false}
        if(!working.compareAndSet(false,true))return false
        chatCancelled.set(false);chatStartedAt=android.os.SystemClock.elapsedRealtime();chatStage="Preparing selected recordings…";main.post(changed)
        worker.execute {
            var saved:PendantChat?=null
            try {
            require(selected.size<=20) {"Select at most 20 transcripts."}
            var characters=0
            val context=selected.map {key->val cached=texts[key]?:error("Selected transcript is no longer available")
                val latest=store.read("text-$key")?:error("Selected transcript was deleted")
                check(latest.getString("source")==cached.source)
                val text=latest.getString("text");characters+=text.length+cached.label.length
                require(characters<=120000){"Select shorter or fewer transcripts (120,000-character context limit)."}
                cached.copy(text=text)}
            check(account===a&&selectedHost.isNotBlank()&&selectedModel.isNotBlank()) {"Choose a model host and model first."}
            val body=PendantChatRules.request(original,prompt,context.map {it.label to it.text}).put("model",selectedModel).put("stream",false).put("max_tokens",2048)
            val turn=ChatTurn(UUID.randomUUID().toString(),prompt,sources=context.map{ChatSource(it.key,it.label,it.source)},host=selectedHost,model=selectedModel)
            val next=original.copy(title=if(original.turns.isEmpty()&&original.title=="New chat")prompt.replace(Regex("\\s+")," ").trim().take(70) else original.title,
                selected=selected.toList(),draft="",turns=original.turns+turn,updatedAt=System.currentTimeMillis())
            chatStore().save(next);saved=next;publishChat(next)
            check(!chatCancelled.get()) {"Stopped waiting. The model may still finish on its PC."}
            val result=c.rpc(selectedHost,"chat.openai",body,true) {stage->check(!chatCancelled.get()){"Stopped waiting"};if(chatStage!=stage){chatStage=stage;main.post(changed)}}
            val text=result.getJSONArray("choices").getJSONObject(0).getJSONObject("message").getString("content")
            require(text.length<=120000)
            check(!chatCancelled.get()&&account===a) {"Stopped waiting. The model may still finish on its PC."}
            val completed=next.copy(turns=next.turns.dropLast(1)+turn.copy(state="complete",reply=text),updatedAt=System.currentTimeMillis())
            chatStore().save(completed);publishChat(completed)
            tell("Answer received from $selectedModel. No tools were enabled.")
            }catch(e:Exception){
                val error=if(chatCancelled.get())"Stopped waiting. The model may still finish on its PC." else e.message?.take(240)?:"The model request failed. Send a new message to retry."
                saved?.let {s->try{val failed=s.copy(turns=s.turns.dropLast(1)+s.turns.last().copy(state=if(chatCancelled.get())"cancelled" else "failed",reply=error),updatedAt=System.currentTimeMillis())
                    chatStore().save(failed);publishChat(failed)}catch(_:Exception){ /* Keep the durable pending turn; restore marks it interrupted. */ }}
                tell(error)
            }finally{chatStartedAt=0;chatStage="";working.set(false);main.post(changed)}
        }
        return true
    }
    fun transcribe(row:RecordingSyncSnapshot){
        if(busy||library.busy||!library.state.playable(row))return
        val a=account?:return;val c=client?:return;val selectedPc=pc;val lang=language
        if(selectedPc.isBlank()){tell("Choose a transcription PC in MindyLink first.");return}
        if(!working.compareAndSet(false,true))return
        val key=MindyLinkStore.recordingKey(row.recording)
        tell("Authenticating the selected phone recording…")
        library.export(row.recording,{export,current ->
            try {
                val caps=c.rpc(selectedPc,"transcription.capabilities")
                check(caps.getInt("version")==1&&caps.getString("format")=="pcm16le-mono-16000"&&caps.getInt("chunkBytes")==MindyLinkProtocol.CHUNK)
                val bytes=export.measure();current();check(bytes<=caps.getLong("maxBytes"))
                val saved=store.read("job-$key")
                val job=if(saved!=null&&!saved.optBoolean("suppressed")&&saved.optString("scope")==scope(a)&&
                    saved.optString("source")==export.manifest.sha256&&saved.optString("pc")==selectedPc&&saved.optString("language")==lang&&
                    saved.optString("phase")!="cancelled")saved else JSONObject().put("jobId",UUID.randomUUID().toString())
                        .put("source",export.manifest.sha256).put("scope",scope(a)).put("pc",selectedPc).put("language",lang)
                        .put("label",recordingLabel(row)).put("phase","uploading")
                store.save("job-$key",job)
                val id=job.getString("jobId")
                val status=c.rpc(selectedPc,"transcription.begin",JSONObject().put("jobId",id).put("source",export.manifest.sha256).put("expectedBytes",bytes).put("language",lang).put("label",recordingLabel(row)))
                validateStatus(status,job,bytes)
                if(status.getString("phase")=="completed")checkResult(c,key,job,current)
                else if(status.getString("phase")=="processing"){job.put("phase","processing");store.save("job-$key",job)}
                else {
                    check(status.getString("phase") in listOf("uploading","interrupted","failed"))
                    var sent=status.getLong("receivedBytes");val digest=MessageDigest.getInstance("SHA-256")
                    export.stream {offset,pcm ->
                        current();check(account===a);digest.update(pcm)
                        var at=(sent-offset).coerceIn(0,pcm.size.toLong()).toInt()
                        while(at<pcm.size){current();val end=minOf(pcm.size,at+MindyLinkProtocol.CHUNK);val chunk=pcm.copyOfRange(at,end)
                            try {
                                val ack=c.rpc(selectedPc,"transcription.chunk",JSONObject().put("jobId",id).put("offset",offset+at)
                                    .put("dataBase64",MindyLinkProtocol.base64(chunk)).put("sha256",MindyLinkProtocol.sha(chunk)))
                                validateStatus(ack,job,bytes);check(ack.getLong("receivedBytes")==offset+end);sent=offset+end
                                tell("Sending to PC · ${sent*100/bytes}%")
                            }finally{chunk.fill(0)};at=end
                        }
                    }
                    current();check(sent==bytes)
                    val checksum=digest.digest().joinToString(""){"%02x".format(it)}
                    c.rpc(selectedPc,"transcription.commit",JSONObject().put("jobId",id).put("sha256",checksum))
                    job.put("phase","processing");store.save("job-$key",job)
                }
                loadTexts();tell("Audio delivered. The PC is transcribing; use Refresh PCs / jobs to retrieve the result.")
                "Selected audio delivered to the chosen PC. Phone and pendant copies kept."
            }catch(e:Exception){tell("Upload paused. ${e.message?.take(150)?:"Retry to resume."}");throw e}
        },{working.set(false);main.post(changed)}, progress={message}, interrupt={c.cancelTransferRequest()})
    }
    private fun validateStatus(status:JSONObject,job:JSONObject,bytes:Long){
        require(status.getInt("version")==1&&status.getString("jobId")==job.getString("jobId")&&
            status.getString("source")==job.getString("source")&&status.getString("language")==job.getString("language")&&
            status.getLong("expectedBytes")==bytes&&status.getLong("receivedBytes") in 0..bytes&&status.getLong("receivedBytes")%2L==0L)
    }
    private fun checkResult(c:MindyLinkClient,key:String,job:JSONObject,current:()->Unit = {}){
        current()
        val id=job.getString("jobId");val pc=job.getString("pc")
        val status=c.rpc(pc,"transcription.status",JSONObject().put("jobId",id))
        check(status.getString("jobId")==id&&status.getString("source")==job.getString("source"))
        job.put("phase",if(status.getString("phase")=="completed")"retrieving" else status.getString("phase"))
            .put("progress",status.optInt("progress",0).coerceIn(0,100));if(!store.updateJob(key,job))return
        if(status.getString("phase")!="completed")return
        val total=status.getInt("resultBytes");require(total in 1..MindyLinkProtocol.MAX_RESULT)
        val data=ByteArray(total);var at=0
        try {
            while(at<total){current();val chunk=c.rpc(pc,"transcription.result",JSONObject().put("jobId",id).put("offset",at))
                require(chunk.getString("jobId")==id&&chunk.getString("source")==job.getString("source")&&chunk.getInt("offset")==at&&chunk.getInt("totalBytes")==total&&chunk.getString("sha256")==status.getString("resultSha256"))
                val part=MindyLinkProtocol.decode(chunk.getString("dataBase64"),MindyLinkProtocol.CHUNK)
                try{require(part.isNotEmpty()&&at+part.size<=total);part.copyInto(data,at);at+=part.size}finally{part.fill(0)}
            }
            current();require(MindyLinkProtocol.sha(data)==status.getString("resultSha256"))
            val text=Charsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(data)).toString()
            store.finish(key,id,JSONObject().put("text",text).put("label",job.getString("label")).put("source",job.getString("source"))
                .put("model",status.optString("model")).put("language",job.getString("language")).put("pc",pc).put("jobId",id)
                .put("warnings",status.optJSONArray("warnings")?:JSONArray()).put("receivedAt",System.currentTimeMillis()))
        }finally{data.fill(0)}
    }
    fun cancelRemote(row:RecordingSyncSnapshot){if(library.busy){library.cancel();tell("Upload stopping. Resume later or cancel its PC job after it stops.");return}
        run("Cancelling PC job…"){
            val key=MindyLinkStore.recordingKey(row.recording);val job=store.read("job-$key")?:return@run
            check(job.getString("scope")==scope(checkNotNull(account)))
            val c=checkNotNull(client);val id=job.getString("jobId");val pc=job.getString("pc")
            val status=c.rpc(pc,"transcription.status",JSONObject().put("jobId",id))
            if(status.getString("phase")!="completed")c.rpc(pc,"transcription.cancel",JSONObject().put("jobId",id))
            c.rpc(pc,"transcription.forget",JSONObject().put("jobId",id))
            store.save("job-$key",job.put("phase","cancelled").put("suppressed",true));tell("PC job removed. Phone audio and saved transcript kept.")
        }
    }
    fun transcript(row:RecordingSyncSnapshot)=texts[MindyLinkStore.recordingKey(row.recording)]?.takeIf {it.source==row.manifest?.sha256}
    fun preview(row:RecordingSyncSnapshot,show:(String)->Unit){run("Opening saved transcript…") {
        val doc=store.read("text-${MindyLinkStore.recordingKey(row.recording)}")?:error("Transcript was deleted.")
        check(doc.getString("source")==row.manifest?.sha256)
        val text=doc.getString("text");val display=text.take(120000)+(if(text.length>120000)"\n\nPreview limited to 120,000 characters; full transcript remains saved." else "")
        main.post{show(display)};tell("Transcript opened locally.")
    }}
    private fun loadTexts(){val next=mutableMapOf<String,RemoteTranscript>()
        val nextJobs=mutableMapOf<String,String>()
        val nextNames=mutableMapOf<String,String>()
        for(row in library.state.recordings){val key=MindyLinkStore.recordingKey(row.recording)
            store.read("name-$key")?.optString("name")?.takeIf{it.isNotBlank()}?.let{nextNames[key]=PendantChatRules.name(it)}
            store.read("job-$key")?.takeIf{!it.optBoolean("suppressed")}?.let{nextJobs[key]="PC job: ${it.optString("phase")} · ${it.optInt("progress",0)}%"}
            val doc=store.read("meta-$key")?:continue
            next[key]=RemoteTranscript(key,nextNames[key]?:doc.getString("label"),"",doc.getString("source"),doc.optString("model"),doc.optJSONArray("warnings")?.join("\n")?:"")}
        if(texts!=next||jobs!=nextJobs||names!=nextNames){texts=next;jobs=nextJobs;names=nextNames;version++;main.post(changed)}
    }
    fun observeForeground(){
        if(!foreground)return
        if(!busy&&!library.busy){
            val signature=library.state.recordings.joinToString {"${it.recording}:${it.revision}"}
            if(signature!=localSignature){localSignature=signature;run(message){loadTexts()};return}
        }
        if(!busy&&!library.busy&&signedIn&&System.currentTimeMillis()-lastPoll>15000){
            lastPoll=System.currentTimeMillis()
            // Only already submitted jobs, never audio, may be polled automatically.
            val rows=library.state.recordings.toList()
            run(message){val c=client?:return@run;val a=account?:return@run
                var updated=false
                for(row in rows){val key=MindyLinkStore.recordingKey(row.recording);val job=store.read("job-$key")?:continue
                    if(!job.optBoolean("suppressed")&&job.optString("phase") in listOf("processing","retrieving")&&job.optString("scope")==scope(a)){
                        checkResult(c,key,job);updated=true
                    }
                }
                if(updated){loadTexts();tell("PC transcription status updated. ${texts.size} saved phone transcripts.")}
            };return
        }
        if(!automatic||!signedIn||busy||library.busy)return
        val row=library.state.recordings.firstOrNull {library.state.playable(it)&&MindyLinkStore.recordingKey(it.recording) !in autoSeen}?:return
        val key=MindyLinkStore.recordingKey(row.recording);autoSeen=autoSeen+key
        // Baseline is persisted before an automatic upload. Failure never becomes a retry loop.
        val next=JSONObject(prefs.toString()).put("autoSeen",JSONArray(autoSeen.toList()))
        run("Preparing optional automatic transcription…") {store.save("preferences",next);prefs=next
            main.post {if(foreground&&automatic&&!busy&&!library.busy)transcribe(row)
                else main.postDelayed({if(foreground&&automatic&&!busy&&!library.busy)transcribe(row)},250)} }
    }
    fun recordingLabel(row:RecordingSyncSnapshot):String {
        names[MindyLinkStore.recordingKey(row.recording)]?.let{return it}
        val time=library.state.firstSyncedTimes[row.recording]
        return (time?.let {"Synced "+java.text.DateFormat.getDateTimeInstance().format(java.util.Date(it))}?:"Recording")+" · "+row.recording.recordingId.toString().take(8)
    }
    fun renameRecording(row:RecordingSyncSnapshot,name:String) {if(busy||library.busy)return
        run("Renaming recording…"){val value=PendantChatRules.name(name);val key=MindyLinkStore.recordingKey(row.recording)
            store.save("name-$key",JSONObject().put("name",value));loadTexts();tell("Recording renamed on this phone. Future PC uploads use this name; existing PC jobs and chat citations keep their original labels.")}}
}
