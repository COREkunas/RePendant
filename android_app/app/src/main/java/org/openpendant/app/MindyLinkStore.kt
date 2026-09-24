package org.openpendant.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** OS-keystore-wrapped private files, excluded from backup. Not pendant keys.
 * Every transcript publication compares its saved job identity under the same
 * process lock used by deletion. No late response can resurrect a deleted item. */
internal class MindyLinkStore private constructor(context:Context,private val alias:String) {
    constructor(context:Context):this(context,"openpendant-mindylink-v1")
    private val root=File(context.noBackupFilesDir,"mindylink-v1")
    companion object {
        private val lock=Any()
        fun recordingKey(id:DurableRecordingId)=MindyLinkProtocol.sha(id.toString().toByteArray())
        fun forSyntheticTests(context:Context,id:java.util.UUID)=MindyLinkStore(context,"openpendant-mindylink-test-$id")
    }
    private fun key():SecretKey {
        val store=KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias,null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build())
        }.generateKey()
    }
    private fun file(name:String):AtomicFile {
        require(name.matches(Regex("[a-z0-9-]{1,100}")))
        return AtomicFile(File(root,name))
    }
    fun read(name:String):JSONObject?=synchronized(lock) {
        val target=file(name)
        if(!target.baseFile.exists()&&!File(target.baseFile.path+".bak").exists())return@synchronized null
        require(target.baseFile.length()<=12*1024*1024)
        val bytes=target.readFully();require(bytes.size>=29)
        val cipher=Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE,key(),GCMParameterSpec(128,bytes.copyOfRange(0,12)));cipher.updateAAD(name.toByteArray())
        val plain=cipher.doFinal(bytes,12,bytes.size-12)
        try { JSONObject(plain.toString(Charsets.UTF_8)) }finally{plain.fill(0);bytes.fill(0)}
    }
    fun save(name:String,value:JSONObject)=synchronized(lock) {
        check(root.isDirectory||root.mkdir());val target=file(name)
        val plain=value.toString().toByteArray();require(plain.size<=10*1024*1024)
        try {
            val cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.ENCRYPT_MODE,key());cipher.updateAAD(name.toByteArray())
            val out=target.startWrite()
            try{out.write(cipher.iv);out.write(cipher.doFinal(plain));target.finishWrite(out)}catch(e:Throwable){target.failWrite(out);throw e}
        }finally{plain.fill(0)}
    }
    fun remove(name:String)=synchronized(lock){file(name).delete();check(!file(name).baseFile.exists())}
    fun updateJob(recording:String,job:JSONObject):Boolean=synchronized(lock) {
        val current=read("job-$recording")?:return@synchronized false
        if(current.optBoolean("suppressed")||current.optString("jobId")!=job.optString("jobId"))return@synchronized false
        save("job-$recording",job);true
    }
    fun finish(recording:String,jobId:String,transcript:JSONObject):Boolean=synchronized(lock) {
        val job=read("job-$recording")?:return@synchronized false
        if(job.optString("jobId")!=jobId||job.optBoolean("suppressed"))return@synchronized false
        save("text-$recording",transcript)
        val metadata=JSONObject(transcript.toString()).apply{remove("text")}
        save("meta-$recording",metadata)
        save("job-$recording",job.put("phase","completed"));true
    }
    fun deleteRecording(id:DurableRecordingId,keepTranscript:Boolean)=synchronized(lock) {
        val idKey=recordingKey(id)
        read("job-$idKey")?.let { save("job-$idKey",it.put("suppressed",true)) }
        if(!keepTranscript){remove("text-$idKey");remove("meta-$idKey")}
    }
}
