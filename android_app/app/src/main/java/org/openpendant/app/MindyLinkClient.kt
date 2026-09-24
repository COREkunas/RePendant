package org.openpendant.app

import okhttp3.*
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit

internal data class MindyLinkDevice(val id:String,val name:String,val role:String,val online:Boolean,val approved:Boolean)
internal class MindyLinkAccount(val server:String,val email:String,val token:String,val device:String,val key:ByteArray) {
    fun json()=JSONObject().put("server",server).put("email",email).put("token",token).put("device",device).put("key",MindyLinkProtocol.base64(key))
    companion object { fun read(j:JSONObject)=MindyLinkAccount(MindyLinkProtocol.server(j.getString("server")),
        MindyLinkProtocol.email(j.getString("email")),j.getString("token"),j.getString("device"),MindyLinkProtocol.decode(j.getString("key"),32).also { require(it.size==32) }) }
}

/** One persistent socket per phone identity; all RPCs serialized. No retry can
 * silently select another PC, submit a new recording, or switch an account. */
internal class MindyLinkClient(private val account:MindyLinkAccount):AutoCloseable {
    private val http=client()
    private var socket:WebSocket?=null
    private var opening:CompletableFuture<Unit>?=null
    @Volatile private var pending:Pending?=null
    @Volatile private var closed=false
    private val seen=LinkedHashSet<String>()
    private class Pending(val request:String,val host:String,val channel:String,val envelope:String,val progress:((String)->Unit)?) {
        val result=CompletableFuture<JSONObject>();val bytes=ByteArrayOutputStream()
        var started=false;var sequence=0;var status=0
    }
    fun devices():List<MindyLinkDevice> {
        val response=json(http,account.server,"/api/v1/devices",null,account.token).getJSONArray("devices")
        require(response.length()<=1000)
        return (0 until response.length()).map { response.getJSONObject(it) }.map {
            MindyLinkDevice(it.getString("id"),it.optString("name","PC"),it.getString("role"),it.optBoolean("online"),it.optBoolean("approved")) }
    }
    @Synchronized fun rpc(host:String,operation:String,body:JSONObject=JSONObject(),model:Boolean=false,progress:((String)->Unit)?=null):JSONObject {
        check(!closed);require(host.isNotBlank()&&host!=account.device)
        val channel=if(model)"model" else "forge-control"
        progress?.invoke("Connecting to selected model…");connect()
        progress?.invoke("Sending to selected model…")
        val request=UUID.randomUUID().toString()
        val frame=MindyLinkProtocol.seal(account.key,account.device,host,channel,
            JSONObject().put("kind",if(model)"model.request" else "forge.request").put("requestId",request).put("operation",operation).put("body",body))
        val p=Pending(request,host,channel,frame.getString("id"),progress);pending=p
        try {
            check(socket?.send(frame.toString())==true) { "MindyLink disconnected. Retry to resume." }
            progress?.invoke("Waiting for model response…")
            return p.result.get(if(operation.startsWith("chat."))180 else 45,TimeUnit.SECONDS)
        } catch (e:Exception) {
            socket?.cancel();socket=null;opening=null
            throw IllegalStateException("MindyLink request did not finish. Check connection and PC; saved jobs can resume.",e)
        } finally { pending=null }
    }
    fun cancelModelRequest(){val p=pending?:return;if(p.channel!="model")return
        p.result.completeExceptionally(IllegalStateException("Stopped waiting"));socket?.cancel();socket=null;opening=null}
    fun cancelTransferRequest(){val p=pending
        if(p!=null&&p.channel!="forge-control")return
        p?.result?.completeExceptionally(IllegalStateException("Transfer stopped; saved progress is kept"))
        opening?.completeExceptionally(IllegalStateException("Transfer stopped"))
        socket?.cancel();socket=null;opening=null
    }
    private fun connect() {
        if(socket!=null&&opening?.isDone==true&&!opening!!.isCompletedExceptionally)return
        val ready=CompletableFuture<Unit>();opening=ready
        val request=Request.Builder().url(account.server.replaceFirst("https://","wss://")+"/relay")
            .header("Authorization","Bearer ${account.token}").header("X-MindyLink-Device-Id",account.device)
            .header("X-MindyLink-Protocol-Version","2").build()
        socket=http.newWebSocket(request,object:WebSocketListener(){
            override fun onOpen(webSocket:WebSocket,response:Response){ready.complete(Unit)}
            override fun onFailure(webSocket:WebSocket,t:Throwable,response:Response?){
                ready.completeExceptionally(IllegalStateException("MindyLink unavailable"));pending?.result?.completeExceptionally(IllegalStateException("Relay disconnected"))
            }
            override fun onClosed(webSocket:WebSocket,code:Int,reason:String){ pending?.result?.completeExceptionally(IllegalStateException("Relay closed")) }
            override fun onMessage(webSocket:WebSocket,text:String){
                val p=pending?:return
                try {
                    require(text.length<=800000)
                    val frame=JSONObject(text)
                    if(frame.optString("type")=="relay.error") {
                        if(MindyLinkProtocol.routingErrorFor(frame,p.envelope))p.result.completeExceptionally(IllegalStateException("PC is unavailable or not approved."))
                        return
                    }
                    if(frame.optString("type")!="relay")return
                    if(frame.optString("fromDeviceId")!=p.host||frame.optString("channel")!=p.channel)return
                    val body=MindyLinkProtocol.open(account.key,frame,p.host,account.device,p.channel)
                    if(body.optString("requestId")!=p.request)return
                    check(seen.add(frame.getString("id"))) { "Repeated response" };while(seen.size>4096)seen.remove(seen.first())
                    if(p.channel=="forge-control") {
                        check(body.getString("kind")=="forge.event")
                        if(body.getString("event")=="error")p.result.completeExceptionally(IllegalStateException(body.optJSONObject("body")?.optString("message")?.take(250)?:"PC refused the request."))
                        else {check(body.getString("event")=="completed");p.result.complete(body.getJSONObject("body"))}
                    } else when(body.getString("kind")) {
                        "model.response.start"->{check(!p.started);p.started=true;p.status=body.getInt("status");p.progress?.invoke("Model responding…")}
                        "model.response.chunk"->{check(p.started&&body.getInt("sequence")==p.sequence++);val bytes=MindyLinkProtocol.decode(body.getString("dataBase64"),512*1024)
                            try { check(p.bytes.size()+bytes.size<=MindyLinkProtocol.MAX_RESULT);p.bytes.write(bytes) }finally{bytes.fill(0)}}
                        "model.response.end"->{check(p.started&&p.status in 200..299);p.result.complete(JSONObject(p.bytes.toString("UTF-8")))}
                        "model.response.error"->p.result.completeExceptionally(IllegalStateException("Selected model could not finish the request."))
                        else->error("Unexpected model response")
                    }
                }catch(_:Exception){p.result.completeExceptionally(IllegalStateException("Invalid or unauthenticated MindyLink response."))}
            }
        })
        ready.get(20,TimeUnit.SECONDS)
    }
    override fun close(){closed=true;socket?.cancel();pending?.result?.completeExceptionally(IllegalStateException("Signed out"));http.dispatcher.executorService.shutdown();http.connectionPool.evictAll();account.key.fill(0)}
    companion object {
        private fun client()=OkHttpClient.Builder().followRedirects(false).followSslRedirects(false)
            .connectTimeout(15,TimeUnit.SECONDS).readTimeout(30,TimeUnit.SECONDS).callTimeout(45,TimeUnit.SECONDS).pingInterval(20,TimeUnit.SECONDS).build()
        private fun json(http:OkHttpClient,server:String,path:String,body:JSONObject?,token:String?=null):JSONObject {
            val builder=Request.Builder().url(MindyLinkProtocol.server(server)+path).header("X-MindyLink-Protocol-Version","2").header("Accept","application/json")
            token?.let { builder.header("Authorization","Bearer $it") }
            body?.let { builder.post(it.toString().toRequestBody("application/json".toMediaType())) }
            return http.newCall(builder.build()).execute().use { response ->
                check(response.isSuccessful) { "MindyLink HTTP ${response.code}. Check account, device approval and registration availability." }
                val input=checkNotNull(response.body).byteStream();val output=ByteArrayOutputStream();val buffer=ByteArray(8192)
                while(true){val n=input.read(buffer);if(n<0)break;check(output.size()+n<=1024*1024);output.write(buffer,0,n)}
                JSONObject(output.toString("UTF-8"))
            }
        }
        fun authenticate(server:String,email:String,password:CharArray,signup:Boolean,deviceId:String,deviceName:String):MindyLinkAccount {
            val base=MindyLinkProtocol.server(server);val address=MindyLinkProtocol.email(email)
            val http=client();var key:ByteArray?=null;var auth:ByteArray?=null
            try {
                auth=MindyLinkProtocol.derive(password,address,"auth");key=MindyLinkProtocol.derive(password,address,"e2e")
                val response=json(http,base,"/api/v1/auth/${if(signup)"register" else "login"}",JSONObject()
                    .put("email",address).put("password",MindyLinkProtocol.base64(auth)).put("displayName","OpenPendant"))
                val token=response.getString("token")
                val device=json(http,base,"/api/v1/devices/register",JSONObject().put("deviceId",deviceId).put("name",deviceName.take(100))
                    .put("role","android").put("capabilities",org.json.JSONArray(listOf("model.client","forge-control.foundation","pendant.transcription.v1"))),token).getJSONObject("device")
                return MindyLinkAccount(base,address,token,device.getString("id"),key).also { key=null }
            } finally { auth?.fill(0);key?.fill(0);password.fill('\u0000');http.dispatcher.executorService.shutdown();http.connectionPool.evictAll() }
        }
    }
}
