package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.nio.file.StandardOpenOption.*
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest
import java.util.UUID

internal data class LongRecordingIntent(val connection: UUID, val start: LongRecordingControlCodec.Request,
    val stop: LongRecordingControlCodec.Request? = null, val latest: LongRecordingControlCodec.State? = null) {
    val unresolved get() = latest == null || !latest.terminal || latest.phase == LongRecordingControlCodec.Phase.FAULT
}
internal interface LongRecordingIntentStore {
    val binding: DurablePublicBinding
    fun read(): Map<UUID,LongRecordingIntent>
    fun begin(connection: UUID, request: LongRecordingControlCodec.Request)
    fun stop(connection: UUID, request: LongRecordingControlCodec.Request)
    fun observe(request: LongRecordingControlCodec.Request, response: ByteArray): LongRecordingControlCodec.State
}

/** App-private public metadata only, never PCM/keys/transcripts. Explicit creation,
 * bounded append-only chain, no truncation/reset/adoption API. A partial/unknown
 * write remains a fence after restart. Every accepted mutation forces the file,
 * performs a directory barrier and reads the exact bytes back BEFORE return.
 * All cooperating writers use this same OS file lock; no lock spans radio waits.
 * SHA detects corruption, not hostile same-account rollback/cryptographic proof. */
internal class LongRecordingIntentJournal private constructor(private val root: Path,
    override val binding: DurablePublicBinding, private val directorySync: (Path) -> Unit,
    private val event: (String) -> Unit) : LongRecordingIntentStore {
    companion object {
        const val ENTRY_BYTES = 320
        const val MAX_ENTRIES = 8192
        const val MAX_OPERATIONS = 128
        private val magic = byteArrayOf(79,80,78,76,67,74,49,0)
        fun createExplicit(root: Path, binding: DurablePublicBinding, directorySync: (Path) -> Unit,
            event: (String) -> Unit = {}): LongRecordingIntentJournal {
            require(root.isAbsolute && root.normalize() == root && root.parent.toRealPath() == root.parent)
            Files.createDirectory(root); directorySync(root.parent)
            FileChannel.open(root.resolve("intent.bin"),CREATE_NEW,WRITE,NOFOLLOW_LINKS).use { file ->
                val bytes = ByteBuffer.wrap(DurablePublicBindingCodec.encode(binding))
                while(bytes.hasRemaining()) check(file.write(bytes) > 0)
                file.force(true)
            }
            directorySync(root)
            return openExisting(root,binding,directorySync,event)
        }
        fun openExisting(root: Path, binding: DurablePublicBinding, directorySync: (Path) -> Unit,
            event: (String) -> Unit = {}): LongRecordingIntentJournal = LongRecordingIntentJournal(root,binding,directorySync,event).also { it.read() }
    }
    private data class Loaded(val states: LinkedHashMap<UUID,LongRecordingIntent>, val count: Int, val digest: ByteArray)
    override fun read(): Map<UUID,LongRecordingIntent> = locked { load(it).states.toMap() }
    override fun begin(connection: UUID, request: LongRecordingControlCodec.Request) = append(1,LongRecordingControlCodec.uuid(connection)+request.frame())
    override fun stop(connection: UUID, request: LongRecordingControlCodec.Request) = append(2,LongRecordingControlCodec.uuid(connection)+request.frame())
    override fun observe(request: LongRecordingControlCodec.Request, response: ByteArray): LongRecordingControlCodec.State {
        val copy=response.copyOf();val state=LongRecordingControlCodec.parse(copy,request)
        append(3,request.frame()+copy);return state
    }
    private fun append(kind: Int, payload: ByteArray) = locked { file ->
        val loaded=load(file)
        val previous=if(kind==3) {
            val request=LongRecordingControlCodec.parseRequest(payload.copyOfRange(0,80),binding)
            loaded.states[request.operation]?.latest
        } else null
        apply(loaded.states,kind,payload) // Validate before touching disk.
        if(previous!=null) {
            val request=LongRecordingControlCodec.parseRequest(payload.copyOfRange(0,80),binding)
            val next=checkNotNull(loaded.states[request.operation]?.latest)
            // Progress counters are live observations, not commands or durable
            // completion. Persist phase/identity/ownership changes and every
            // terminal result, not two-second counter updates for hours. The
            // session separately enforces monotonicity between live replies.
            if(!next.terminal && previous.copy(accepted=next.accepted,committed=next.committed,
                    flags=previous.flags and LongRecordingControlCodec.USB.inv())==
                next.copy(flags=next.flags and LongRecordingControlCodec.USB.inv())) return@locked Unit
            if(previous==next) return@locked Unit
        }
        check(loaded.count < MAX_ENTRIES)
        val bytes=ByteBuffer.allocate(ENTRY_BYTES).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(magic);putInt(kind);putInt(loaded.count);putInt(payload.size);position(32);put(loaded.digest);put(payload)
        }.array()
        digest(bytes.copyOf(288)).copyInto(bytes,288)
        val offset=file.size();file.position(offset);event("before_write")
        val data=ByteBuffer.wrap(bytes)
        while(data.hasRemaining()) check(file.write(data)>0)
        event("written");file.force(true);event("file_forced");directorySync(root);event("directory_forced")
        check(file.size()==offset+ENTRY_BYTES && readAt(file,offset,ENTRY_BYTES).contentEquals(bytes));event("readback")
        Unit
    }
    private fun load(file: FileChannel): Loaded {
        val size=file.size();check(size>=128 && size<=128L+MAX_ENTRIES.toLong()*ENTRY_BYTES && (size-128)%ENTRY_BYTES==0L)
        check(DurablePublicBindingCodec.decode(readAt(file,0,128))==binding)
        val states=linkedMapOf<UUID,LongRecordingIntent>();var previous=ByteArray(32);val count=((size-128)/ENTRY_BYTES).toInt()
        for(index in 0 until count) {
            val bytes=readAt(file,128L+index.toLong()*ENTRY_BYTES,ENTRY_BYTES);val p=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            check(bytes.copyOfRange(0,8).contentEquals(magic) && p.getInt(12)==index && (20..31).all { bytes[it]==0.toByte() })
            check(bytes.copyOfRange(32,64).contentEquals(previous));check(MessageDigest.isEqual(digest(bytes.copyOf(288)),bytes.copyOfRange(288,320)))
            val kind=p.getInt(8);val length=p.getInt(16);check(length==if(kind==3)161 else 96)
            check((64+length until 288).all { bytes[it]==0.toByte() })
            apply(states,kind,bytes.copyOfRange(64,64+length));previous=bytes.copyOfRange(288,320)
        }
        check(file.size()==size);return Loaded(states,count,previous)
    }
    private fun apply(states: LinkedHashMap<UUID,LongRecordingIntent>, kind: Int, payload: ByteArray) {
        check(kind in 1..3)
        val offset=if(kind==3)0 else 16
        val request=LongRecordingControlCodec.parseRequest(payload.copyOfRange(offset,offset+80),binding)
        val operation=requireNotNull(request.operation);check(request.boot!=null)
        if(kind==1) {
            val connection=requireNotNull(LongRecordingControlCodec.readUuid(payload,0))
            check(request.command==LongRecordingControlCodec.START && operation !in states && states.size<MAX_OPERATIONS && states.values.none { it.unresolved })
            states[operation]=LongRecordingIntent(connection,request);return
        }
        val old=checkNotNull(states[operation]);check(old.start.boot==request.boot)
        if(kind==2) {
            requireNotNull(LongRecordingControlCodec.readUuid(payload,0))
            // An explicit Stop may be repeated after STATUS proves the exact
            // operation still runs without its stop latch. START never repeats.
            check(request.command==LongRecordingControlCodec.STOP && old.unresolved && old.latest?.phase!=LongRecordingControlCodec.Phase.FAULT)
            states[operation]=old.copy(stop=request);return
        }
        if(request.command==LongRecordingControlCodec.START)check(request.frame().contentEquals(old.start.frame()))
        if(request.command==LongRecordingControlCodec.STOP)check(old.stop?.frame()?.contentEquals(request.frame())==true)
        val state=LongRecordingControlCodec.parse(payload.copyOfRange(80,161),request)
        old.latest?.let { LongRecordingControlCodec.progress(it,state) }
        states[operation]=old.copy(latest=state)
    }
    private fun digest(bytes: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").apply {
        update(DurablePublicBindingCodec.encode(binding))
    }.digest(bytes)
    private fun readAt(file: FileChannel, at: Long, length: Int): ByteArray {
        val result=ByteArray(length);val b=ByteBuffer.wrap(result);file.position(at)
        while(b.hasRemaining()) check(file.read(b)>0)
        return result
    }
    private fun <T> locked(action: (FileChannel) -> T): T {
        check(root.isAbsolute && root.normalize()==root && root.toRealPath()==root)
        check(Files.readAttributes(root,BasicFileAttributes::class.java,NOFOLLOW_LINKS).isDirectory)
        Files.newDirectoryStream(root).use { entries -> check(entries.map { it.fileName.toString() }.take(2)==listOf("intent.bin")) }
        val path=root.resolve("intent.bin");check(Files.readAttributes(path,BasicFileAttributes::class.java,NOFOLLOW_LINKS).isRegularFile)
        FileChannel.open(path,READ,WRITE,NOFOLLOW_LINKS).use { file ->
            val lock=checkNotNull(file.tryLock());lock.use { return action(file) }
        }
    }
}
