package org.openpendant.app

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class MindyLinkChatsTest {
    private val files=mutableMapOf<String,String>()
    private fun repo(scope:String="a".repeat(64))=MindyLinkChats(scope,{files[it]?.let(::JSONObject)},{k,j->files[k]=j.toString()},{files.remove(it);Unit})
    private fun turn(prompt:String="Hello",reply:String="Labas",state:String="complete")=ChatTurn(UUID.randomUUID().toString(),prompt,reply,state,host="host",model="model")
    @Test fun conversationsPersistIndependentlyWithDraftAndSourceSelection(){
        val store=repo();val first=store.create();store.save(first.copy(title="Meeting one",draft="Unsent",selected=listOf("f".repeat(64)),turns=listOf(turn())))
        val second=store.create();assertTrue(second.turns.isEmpty());assertTrue(second.selected.isEmpty())
        val reopened=repo().load(first.id);assertEquals("Unsent",reopened.draft);assertEquals(listOf("f".repeat(64)),reopened.selected)
        assertEquals(1,reopened.turns.size);assertEquals(second.id,repo().active()!!.id)
    }
    @Test fun accountsCannotLoadOverwriteOrDeleteAnotherAccountsChat(){
        val first=repo().create();val other=repo("b".repeat(64));assertTrue(other.list().isEmpty())
        assertThrows(Exception::class.java){other.load(first.id)}
        assertThrows(Exception::class.java){other.save(first)}
        assertThrows(Exception::class.java){other.delete(first.id)}
        assertEquals(first,repo().load(first.id))
    }
    @Test fun deletionRemovesOnlyTheChosenChatAndKeepsRecordings(){
        val store=repo();val one=store.create();val two=store.create();files["text-private"]="unchanged"
        store.delete(two.id);assertEquals(one.id,store.active()!!.id);assertFalse(files.containsKey("chat-${two.id}"));assertEquals("unchanged",files["text-private"])
    }
    @Test fun requestsIncludeOnlyThisChatCompletedHistoryAndExplicitCurrentContext(){
        val chat=PendantChat(turns=listOf(turn(),turn("failed question","internal error","failed"),turn("pending question","","pending")))
        val body=PendantChatRules.request(chat,"Follow up",listOf("Selected meeting" to "PUBLIC TEXT"))
        val messages=body.getJSONArray("messages");assertEquals(4,messages.length())
        assertEquals("Hello",messages.getJSONObject(1).getString("content"));assertEquals("Labas",messages.getJSONObject(2).getString("content"))
        assertFalse(body.toString().contains("failed question"));assertFalse(body.toString().contains("pending question"));assertFalse(body.has("tools"))
        assertTrue(body.toString().contains("PUBLIC TEXT"));assertTrue(messages.getJSONObject(0).getString("content").contains("untrusted"))
    }
    @Test fun namesAndHistoryAreBoundedAndNeverSilentlyTruncated(){
        assertEquals("Susitikimas",PendantChatRules.name(" Susitikimas "))
        listOf("","a\nb","a\u0000b","x".repeat(121)).forEach{assertThrows(Exception::class.java){PendantChatRules.name(it)}}
        assertThrows(Exception::class.java){PendantChatRules.request(PendantChat(turns=listOf(turn(reply="x".repeat(48001)))),"Next",emptyList())}
        assertThrows(Exception::class.java){repo().save(PendantChat(selected=List(21){it.toString().padStart(64,'0')}))}
        assertThrows(Exception::class.java){repo().save(PendantChat(turns=List(51){turn()}))}
    }
    @Test fun pendingStateAndCitationsSurviveRestartWithoutAutomaticExecution(){
        val store=repo();val chat=store.create();val pending=turn(reply="",state="pending").copy(sources=listOf(ChatSource("a".repeat(64),"Original name","b".repeat(64))))
        store.save(chat.copy(turns=listOf(pending)));val restored=repo().active()!!
        assertEquals("pending",restored.turns.single().state);assertEquals("Original name",restored.turns.single().sources.single().label)
        assertEquals(2,PendantChatRules.request(restored,"Retry",emptyList()).getJSONArray("messages").length())
    }
    @Test fun invalidOrCrossAccountStoredContentFailsClosed(){
        val store=repo();val chat=store.create();val key="chat-${chat.id}";files[key]=JSONObject(files[key]!!).put("scope","b".repeat(64)).toString()
        assertThrows(Exception::class.java){store.load(chat.id)}
        assertThrows(Exception::class.java){store.load("../outside")}
    }
}
