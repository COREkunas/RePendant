package org.openpendant.app

import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/** Conversation/session pattern adapted from MindyLab MindyLink with owner permission.
 * Pure, bounded model; Android stores every file through its existing Keystore wrapper.
 * Source references retain provenance, never duplicate recording audio or transcript text. */
internal data class ChatSource(val key:String,val label:String,val source:String)
internal data class ChatTurn(val id:String,val prompt:String,val reply:String="",val state:String="pending",
    val sources:List<ChatSource> = emptyList(),val host:String,val model:String,val createdAt:Long=System.currentTimeMillis())
internal data class PendantChat(val id:String=UUID.randomUUID().toString(),val title:String="New chat",
    val selected:List<String> = emptyList(),val draft:String="",val turns:List<ChatTurn> = emptyList(),
    val updatedAt:Long=System.currentTimeMillis())
internal data class PendantChatSummary(val id:String,val title:String,val updatedAt:Long)

internal object PendantChatRules {
    fun name(value:String):String=value.trim().also {
        require(it.length in 1..120 && it.none { c->c<' '||c=='\u007f' }) {"Use a name of 1–120 characters without line breaks."}
    }
    fun validate(chat:PendantChat) {
        require(UUID.fromString(chat.id).toString()==chat.id)
        name(chat.title);require(chat.draft.length<=12000&&chat.turns.size<=50&&chat.selected.distinct().size==chat.selected.size&&chat.selected.size<=20)
        require(chat.selected.all {it.matches(Regex("[a-f0-9]{64}"))})
        require(chat.turns.map{it.id}.distinct().size==chat.turns.size)
        require(chat.turns.sumOf {it.prompt.length+it.reply.length}<=600000) {"This chat is full. Create a new chat."}
        for(turn in chat.turns){
            require(UUID.fromString(turn.id).toString()==turn.id)
            require(turn.prompt.isNotBlank()&&turn.prompt.length<=12000&&turn.reply.length<=120000)
            require(turn.state in listOf("pending","complete","failed","cancelled"))
            require(turn.sources.size<=20&&turn.host.length in 1..200&&turn.model.length in 1..500)
            for(s in turn.sources){require(s.key.matches(Regex("[a-f0-9]{64}"))&&s.source.matches(Regex("[a-f0-9]{64}")));name(s.label)}
        }
    }
    fun request(chat:PendantChat,prompt:String,context:List<Pair<String,String>>):JSONObject {
        validate(chat)
        val complete=chat.turns.filter {it.state=="complete"}
        require(complete.sumOf{it.prompt.length+it.reply.length}<=48000) {"Conversation history is too long for another request. Start a new chat."}
        val body=MindyLinkProtocol.context(prompt,context)
        val base=body.getJSONArray("messages");val messages=JSONArray().put(base.getJSONObject(0))
        complete.forEach {messages.put(JSONObject().put("role","user").put("content",it.prompt))
            messages.put(JSONObject().put("role","assistant").put("content",it.reply))}
        messages.put(base.getJSONObject(1));return body.put("messages",messages)
    }
}

internal class MindyLinkChats(private val scope:String,private val read:(String)->JSONObject?,
    private val write:(String,JSONObject)->Unit,private val remove:(String)->Unit) {
    init{require(scope.matches(Regex("[a-f0-9]{64}")))}
    private val indexName="chats-$scope"
    private fun index()=read(indexName)?:JSONObject().put("items",JSONArray()).put("active","")
    private fun file(id:String):String {require(UUID.fromString(id).toString()==id);return "chat-$id"}
    fun list():List<PendantChatSummary> {val rows=index().getJSONArray("items");require(rows.length()<=60)
        return (0 until rows.length()).map {rows.getJSONObject(it).let {j->
            file(j.getString("id"));PendantChatSummary(j.getString("id"),PendantChatRules.name(j.getString("title")),j.getLong("updatedAt"))}}
    }
    fun active():PendantChat?=index().optString("active").takeIf{it.isNotBlank()}?.let(::load)
    fun create():PendantChat {
        require(list().size<60){"You have 60 chats. Delete an old chat before creating another."}
        return PendantChat().also(::save)
    }
    fun load(id:String):PendantChat {
        check(list().any {it.id==id}) {"Chat not found for this account."}
        val j=checkNotNull(read(file(id)));check(j.getString("scope")==scope)
        val ts=j.getJSONArray("turns")
        require(ts.length()<=50)
        val turns=(0 until ts.length()).map {ts.getJSONObject(it).let {t->
            val sources=t.getJSONArray("sources");require(sources.length()<=20)
            ChatTurn(t.getString("id"),t.getString("prompt"),t.getString("reply"),t.getString("state"),
                (0 until sources.length()).map {sources.getJSONObject(it).let {s->ChatSource(s.getString("key"),s.getString("label"),s.getString("source"))}},
                t.getString("host"),t.getString("model"),t.getLong("createdAt"))}}
        val selected=j.getJSONArray("selected");require(selected.length()<=20)
        return PendantChat(j.getString("id"),j.getString("title"),(0 until selected.length()).map {selected.getString(it)},
            j.getString("draft"),turns,j.getLong("updatedAt")).also {check(it.id==id);PendantChatRules.validate(it)}
    }
    fun save(chat:PendantChat) {
        PendantChatRules.validate(chat)
        val summaries=list();require(summaries.any{it.id==chat.id}||summaries.size<60)
        read(file(chat.id))?.let {check(it.getString("scope")==scope){"Chat belongs to another account."}}
        val ts=JSONArray();chat.turns.forEach {t->val sources=JSONArray();t.sources.forEach{s->sources.put(JSONObject()
            .put("key",s.key).put("label",s.label).put("source",s.source))}
            ts.put(JSONObject().put("id",t.id).put("prompt",t.prompt).put("reply",t.reply).put("state",t.state)
                .put("host",t.host).put("model",t.model).put("createdAt",t.createdAt).put("sources",sources))}
        write(file(chat.id),JSONObject().put("scope",scope).put("id",chat.id).put("title",chat.title)
            .put("draft",chat.draft).put("selected",JSONArray(chat.selected)).put("updatedAt",chat.updatedAt).put("turns",ts))
        val rows=JSONArray();(listOf(PendantChatSummary(chat.id,chat.title,chat.updatedAt))+summaries.filter{it.id!=chat.id}).forEach {
            rows.put(JSONObject().put("id",it.id).put("title",it.title).put("updatedAt",it.updatedAt))}
        write(indexName,JSONObject().put("active",chat.id).put("items",rows))
    }
    fun delete(id:String) {
        val previous=list();check(previous.any{it.id==id})
        val rows=JSONArray();previous.filter{it.id!=id}.forEach {rows.put(JSONObject().put("id",it.id).put("title",it.title).put("updatedAt",it.updatedAt))}
        write(indexName,JSONObject().put("active",previous.firstOrNull{it.id!=id}?.id?:"").put("items",rows))
        remove(file(id))
    }
}
