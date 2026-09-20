package org.openpendant.app

import android.content.Context
import android.database.DatabaseErrorHandler
import android.database.sqlite.SQLiteDatabase
import android.os.Looper
import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.nio.file.StandardOpenOption.*
import java.security.MessageDigest
import java.util.UUID

class RecordingMetadataException : IllegalStateException("Recording metadata requires reconciliation")
class RecordingMetadataCapacityException : IllegalStateException("Recording metadata capacity is full; existing records are unchanged")

/** Durable CAS implementation for the existing coordinator, not a wire protocol.
 * One instance owns this directory/process lock for its entire lifetime. Close
 * all returned coordinators BEFORE closing the store. FULL synchronous rollback
 * journal transactions commit before callbacks return; no default corruption
 * handler may delete/recreate the database. Unsupported schema/corruption or any
 * ambiguous commit sticks a fault marker: no auto-reset/replacement is provided.
 * All entry points are explicit I/O-worker operations. No startup audio scan.
 */
class AndroidRecordingSyncStore private constructor(private val directory:Path,
    private val diagnostic:((String,String,Int)->Unit)?=null) : Closeable, DurableRecordingMetadata {
    private val monitor=Any()
    private val ownership=RecordingSyncOwnership()
    private val fault=directory.resolve("metadata.fault")
    private val lockChannel:FileChannel
    private val lock:FileLock
    private val database:SQLiteDatabase
    private var closed=false
    private var fenced=false

    init {
        checkWorker()
        require(Files.isDirectory(directory,NOFOLLOW_LINKS) && directory.toRealPath()==directory)
        lockChannel=FileChannel.open(directory.resolve("metadata.lock"),CREATE,WRITE,NOFOLLOW_LINKS)
        lock=try { lockChannel.tryLock() ?: throw RecordingSyncBusyException() }
            catch (_:OverlappingFileLockException) { lockChannel.close(); throw RecordingSyncBusyException() }
            catch (failure:RecordingSyncBusyException) { lockChannel.close(); throw failure }
            catch (_:Exception) { lockChannel.close(); throw RecordingMetadataException() }
        var opened:SQLiteDatabase?=null
        var initStage="initialization_admission"
        try {
            if(Files.exists(fault,NOFOLLOW_LINKS)) throw RecordingMetadataException()
            val file=directory.resolve("recordings.sqlite")
            val existed=Files.exists(file,NOFOLLOW_LINKS)
            val creation=directory.resolve("metadata.creation")
            val initialized=directory.resolve("metadata.initialized")
            val namespace=MessageDigest.getInstance("SHA-256").digest(directory.fileName.toString().toByteArray(Charsets.UTF_8))
            fun marker(magic:String):ByteArray {
                val body=magic.toByteArray(Charsets.US_ASCII)+byteArrayOf(0,0,0,1)+namespace
                return body+MessageDigest.getInstance("SHA-256").digest(body)
            }
            fun markerMatches(path:Path,bytes:ByteArray):Boolean {
                if(!Files.isRegularFile(path,NOFOLLOW_LINKS)) return false
                return FileChannel.open(path,READ,NOFOLLOW_LINKS).use { channel ->
                    if(channel.size()!=bytes.size.toLong()) return@use false
                    val actual=ByteArray(bytes.size);val buffer=ByteBuffer.wrap(actual)
                    while(buffer.hasRemaining()) if(channel.read(buffer)<=0) return@use false
                    actual.contentEquals(bytes) && channel.read(ByteBuffer.allocate(1)) == -1
                }
            }
            if(existed) {
                initStage="existing_marker_validation"
                check(markerMatches(creation,marker("OPNDSC1\u0000")) && markerMatches(initialized,marker("OPNDSI1\u0000")))
            } else {
                initStage="new_creation_marker"
                // Missing previously owned DB is lost metadata, never a blank
                // phone. Retain suppression/tombstone evidence and refuse reset.
                Files.newDirectoryStream(directory).use { entries ->
                    check(entries.all { it.fileName.toString()=="metadata.lock" })
                }
                writeMarker(creation,marker("OPNDSC1\u0000"))
            }
            for(name in listOf("recordings.sqlite","recordings.sqlite-journal","recordings.sqlite-wal","recordings.sqlite-shm")) {
                val path=directory.resolve(name)
                if(Files.exists(path,NOFOLLOW_LINKS)) {
                    require(Files.isRegularFile(path,NOFOLLOW_LINKS) && Files.size(path)<=MAX_DB_BYTES)
                    if(!existed) throw RecordingMetadataException()
                }
            }
            initStage="sqlite_open"
            opened=SQLiteDatabase.openDatabase(file.toString(),null,
                SQLiteDatabase.OPEN_READWRITE or (if(existed)0 else SQLiteDatabase.CREATE_IF_NECESSARY) or SQLiteDatabase.NO_LOCALIZED_COLLATORS,
                DatabaseErrorHandler { throw RecordingMetadataException() })
            val db=opened
            initStage="journal_mode"
            db.disableWriteAheadLogging()
            db.rawQuery("PRAGMA journal_mode=DELETE",null).use { check(it.moveToFirst() && it.getString(0).equals("delete",true)) }
            initStage="synchronous_mode"
            db.execSQL("PRAGMA synchronous=FULL")
            db.rawQuery("PRAGMA synchronous",null).use { check(it.moveToFirst() && it.getInt(0)==2) }
            initStage="database_size_limit"
            db.rawQuery("PRAGMA busy_timeout=2500",null).use {
                check(it.moveToFirst() && it.getInt(0)==2500 && !it.moveToNext())
            }
            check(db.setMaximumSize(MAX_DB_BYTES)<=MAX_DB_BYTES+4096)
            if(!existed) {
                initStage="create_schema_transaction"
                db.beginTransaction()
                try {
                    requireDurability(db)
                    db.execSQL(SCHEMA)
                    db.version=1
                    db.setTransactionSuccessful()
                } finally { db.endTransaction() }
                initStage="schema_directory_sync"
                AndroidDurableSegments.syncDirectory(directory)
            }
            initStage="schema_version"
            check(db.version==1)
            initStage="exact_schema"
            db.rawQuery("SELECT type,name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY name",null).use {
                check(it.moveToFirst() && it.getString(0)=="table" && it.getString(1)=="snapshots" && it.getString(2)==SCHEMA && !it.moveToNext())
            }
            initStage="integrity_check"
            db.rawQuery("PRAGMA quick_check(1)",null).use { check(it.moveToFirst() && it.getString(0)=="ok" && !it.moveToNext()) }
            initStage="record_count"
            db.rawQuery("SELECT count(*) FROM snapshots",null).use { check(it.moveToFirst() && it.getLong(0)<=MAX_RECORDINGS) }
            initStage="initialized_marker"
            if(!existed) writeMarker(initialized,marker("OPNDSI1\u0000"))
            database=db
        } catch (failure:Exception) {
            reportDiagnostic(initStage,failure)
            markFault()
            try { opened?.close() } catch (_:Exception) { }
            try { lock.release() } finally { lockChannel.close() }
            throw RecordingMetadataException()
        }
    }

    /** Explicit catalog admission creates ONLY an empty metadata row. */
    override fun createRecording(recording:DurableRecordingId) = synchronized(monitor) {
        guarded(admission=true) {
            transaction {
                if(load(recording)!=null) return@transaction //Authenticated catalog replay never resets metadata.
                database.rawQuery("SELECT count(*) FROM snapshots",null).use {
                    check(it.moveToFirst())
                    val count=it.getLong(0)
                    check(count in 0..MAX_RECORDINGS.toLong() && !it.moveToNext())
                    if(count==MAX_RECORDINGS.toLong()) throw RecordingMetadataCapacityException()
                }
                database.compileStatement("INSERT INTO snapshots(identity,revision,payload) VALUES(?,0,?)").use {
                    it.bindString(1,key(recording));it.bindBlob(2,RecordingSyncSnapshotCodec.encode(RecordingSyncSnapshot(recording)))
                    check(it.executeInsert()!=-1L)
                }
            }
        }
    }

    /** Revalidates every committed local segment before receipts can be exposed.
     * The verifier must use trusted catalog/key binding + DurableSegmentStore.
     * An orphan ciphertext from an interrupted publication is not a receipt:
     * a new DOWNLOAD ticket reopens/verifies it through SegmentDownloadEngine.
     */
    override fun coordinator(recording:DurableRecordingId, verifyPhoneSegment:(SegmentIdentity)->Boolean):RecordingSyncContract = synchronized(monitor) {
        guarded(admission=true) {
            val snapshot=load(recording) ?: throw RecordingMetadataException()
            // Do not run download recovery for intentionally removed files.
            // Persisted pending phone deletion has a separate restricted path.
            if(snapshot.deletions.any { it.phonePending }) throw DeletionRecoveryRequiredException()
            check(snapshot.phoneSegments.all(verifyPhoneSegment))
            RecordingSyncContract(snapshot,ownership) { revision,next -> commit(revision,next) }
        }
    }

    /** Bounded validated metadata only, including suppression and offline delete
     * outboxes. Does not open/recover/read audio or infer deletion from absence. */
    override fun snapshots(deviceId:UUID):List<RecordingSyncSnapshot> = synchronized(monitor) {
        require(validOwnedUuid(deviceId)) // Caller admission is not metadata corruption.
        guarded {
            val result=ArrayList<RecordingSyncSnapshot>()
            database.rawQuery("SELECT identity,revision,length(payload),payload FROM snapshots ORDER BY identity",null).use { cursor ->
                var count=0
                while(cursor.moveToNext()) {
                    check(++count<=MAX_RECORDINGS && cursor.getInt(2) in 126..RecordingSyncSnapshotCodec.MAX_BYTES)
                    val row=RecordingSyncSnapshotCodec.decode(cursor.getBlob(3))
                    check(cursor.getString(0)==key(row.recording) && cursor.getLong(1)==row.revision)
                    if(row.recording.volume.deviceId==deviceId) result.add(row)
                }
            }
            result.toList()
        }
    }

    /** Restricted metadata-only stale-volume transition. Never returns an
     * unverified file-owning coordinator. It preserves phone data/suppression,
     * fences old remote intents and cannot authorize download, receipt or erase.
     * A cooperating active owner causes ordinary Busy, not a corruption fault. */
    override fun invalidateOtherVolumes(current:RecordingVolume) = synchronized(monitor) {
        guarded(admission=true) {
            for(row in snapshots(current.deviceId)) {
                if(row.recording.volume==current || row.staleVolume) continue
                val coordinator=RecordingSyncContract(row,ownership) { revision,next -> commit(revision,next) }
                try { coordinator.authenticatedConnection(current,true) }
                finally { coordinator.close() }
            }
        }
    }

    /** Metadata-only recovery admission. Missing covered files are expected;
     * no normal verifier/download recovery runs. Wrong caller binding is a
     * non-fatal refusal, but corrupt metadata or an existing fault stays fenced. */
    fun openPendingPhoneDeletion(recording:DurableRecordingId, operationId:UUID,
        manifestSha256:String):PendingPhoneDeletion = synchronized(monitor) {
        guarded(admission=true) {
            val snapshot=load(recording) ?: throw PhoneDeletionAdmissionException()
            PendingPhoneDeletion(snapshot,operationId,manifestSha256,ownership,::whileDeletionStoreOwned) { revision,next -> commit(revision,next) }
        }
    }

    private fun whileDeletionStoreOwned(action:()->Boolean):Boolean = synchronized(monitor) {
        checkWorker()
        // Hold the metadata lifetime monitor through file work and completion
        // CAS. Store.close cannot release its OS ownership during removal, and
        // a retained handle cannot mutate files after its store was closed.
        if(closed || fenced || !lock.isValid || Files.exists(fault,NOFOLLOW_LINKS)) throw RecordingMetadataException()
        action() //File failures fence only the handle; persisted intent can resume.
    }

    private fun commit(expectedRevision:Long,next:RecordingSyncSnapshot) = synchronized(monitor) {
        guarded {
            val encoded=RecordingSyncSnapshotCodec.encode(next)
            check(expectedRevision in 0 until Long.MAX_VALUE && next.revision==expectedRevision+1)
            transaction {
                val previous=load(next.recording) ?: throw RecordingMetadataException()
                check(previous.revision==expectedRevision && next.workGeneration>=previous.workGeneration)
                database.compileStatement("UPDATE snapshots SET revision=?,payload=? WHERE identity=? AND revision=?").use {
                    it.bindLong(1,next.revision);it.bindBlob(2,encoded);it.bindString(3,key(next.recording));it.bindLong(4,expectedRevision)
                    check(it.executeUpdateDelete()==1)
                }
            }
            check(load(next.recording)?.let { RecordingSyncSnapshotCodec.encode(it).contentEquals(encoded) }==true)
        }
    }

    private fun load(recording:DurableRecordingId):RecordingSyncSnapshot? {
        var revision:Long
        database.rawQuery("SELECT revision,length(payload) FROM snapshots WHERE identity=?",arrayOf(key(recording))).use {
            if(!it.moveToFirst()) return null
            revision=it.getLong(0);check(revision>=0 && it.getInt(1) in 126..RecordingSyncSnapshotCodec.MAX_BYTES && !it.moveToNext())
        }
        database.rawQuery("SELECT payload FROM snapshots WHERE identity=? AND length(payload)<=262144",arrayOf(key(recording))).use {
            check(it.moveToFirst())
            val snapshot=RecordingSyncSnapshotCodec.decode(it.getBlob(0))
            check(snapshot.recording==recording && snapshot.revision==revision && !it.moveToNext())
            return snapshot
        }
    }
    private fun key(recording:DurableRecordingId)=canonicalHex(RecordingSyncSnapshotCodec.identityBytes(recording))
    private fun <T> transaction(action:()->T):T {
        database.beginTransaction()
        var successful=false
        try {
            requireDurability(database)
            return action().also { database.setTransactionSuccessful();successful=true }
        }
        finally {
            database.endTransaction()
            // FULL flushes DB/journal contents; explicitly persist rollback
            // journal removal before returning an ACK-eligible metadata commit.
            if(successful) AndroidDurableSegments.syncDirectory(directory)
        }
    }
    private fun requireDurability(db:SQLiteDatabase) {
        // The begun transaction pins one pooled connection on this worker.
        // Re-check it before writes; an OEM/default/idle replacement connection
        // may not inherit raw per-connection PRAGMAs set at initial open.
        db.rawQuery("PRAGMA synchronous",null).use {
            check(it.moveToFirst() && it.getInt(0)==2 && !it.moveToNext())
        }
        db.rawQuery("PRAGMA journal_mode",null).use {
            check(it.moveToFirst() && it.getString(0).equals("delete",true) && !it.moveToNext())
        }
    }
    private fun <T> guarded(admission:Boolean=false,action:()->T):T {
        checkWorker()
        if(closed || fenced || !lock.isValid || Files.exists(fault,NOFOLLOW_LINKS)) throw RecordingMetadataException()
        try { return action() } catch (failure:Exception) {
            // These exact admission refusals occur before a mutation (or after
            // a successfully rolled-back read-only capacity transaction). Never
            // exempt a commit/SQLite/durability failure from the sticky fence.
            if(admission && (failure is RecordingSyncBusyException ||
                failure is SegmentStorageBusyException || failure is RecordingMetadataCapacityException ||
                failure is DeletionRecoveryRequiredException || failure is PhoneDeletionAdmissionException)) throw failure
            reportDiagnostic("metadata_operation",failure)
            markFault();throw RecordingMetadataException()
        }
    }
    private fun reportDiagnostic(stage:String,failure:Exception) {
        try {
            diagnostic?.invoke(stage,failure.javaClass.simpleName.take(80),
                failure.stackTrace.firstOrNull { it.className==AndroidRecordingSyncStore::class.java.name }?.lineNumber ?: -1)
        } catch (_:Throwable) { /* A synthetic observer must never skip fencing or cleanup. */ }
    }
    private fun markFault() {
        fenced=true
        try {
            if(!Files.exists(fault,NOFOLLOW_LINKS)) FileChannel.open(fault,CREATE_NEW,WRITE,NOFOLLOW_LINKS).use {
                val data=ByteBuffer.wrap("OPNDSYNCFAULT1".toByteArray(Charsets.US_ASCII))
                while(data.hasRemaining()) check(it.write(data)>0)
                it.force(true)
            }
            AndroidDurableSegments.syncDirectory(directory)
        } catch (_:Exception) { /* Current instance remains fenced even when disk is unavailable. */ }
    }
    private fun writeMarker(path:Path,bytes:ByteArray) {
        FileChannel.open(path,CREATE_NEW,WRITE,NOFOLLOW_LINKS).use {
            val data=ByteBuffer.wrap(bytes)
            while(data.hasRemaining()) check(it.write(data)>0)
            it.force(true)
        }
        AndroidDurableSegments.syncDirectory(directory)
    }
    override fun close() = synchronized(monitor) {
        if(!closed) {
            closed=true
            try { database.close() } finally { try { lock.release() } finally { lockChannel.close() } }
        }
    }
    companion object {
        const val MAX_RECORDINGS=128
        const val MAX_DB_BYTES=64L*1024*1024
        private const val SCHEMA="CREATE TABLE snapshots (identity TEXT PRIMARY KEY NOT NULL CHECK(length(identity)=112), revision INTEGER NOT NULL CHECK(revision>=0), payload BLOB NOT NULL CHECK(length(payload)<=262144))"
        fun open(context:Context)=openAt(context,"recording-sync-v1")
        /** Library inspection never creates a new database/namespace. */
        fun openExisting(context:Context):AndroidRecordingSyncStore? {
            checkWorker()
            val directory=context.noBackupFilesDir.canonicalFile.toPath().resolve("recording-sync-v1")
            val found=AndroidDurableBinding.attributes(directory) ?: return null
            check(found.isDirectory)
            check(AndroidDurableBinding.attributes(directory.resolve("recordings.sqlite"))?.isRegularFile == true)
            return AndroidRecordingSyncStore(directory)
        }
        fun forSyntheticTests(context:Context,id:UUID, diagnostic:((String,String,Int)->Unit)?=null):AndroidRecordingSyncStore {
            require(validOwnedUuid(id));return openAt(context,"recording-sync-test-$id",diagnostic)
        }
        private fun checkWorker() { check(Looper.myLooper()!=Looper.getMainLooper()) { "Recording metadata requires an I/O worker" } }
        private fun openAt(context:Context,name:String,diagnostic:((String,String,Int)->Unit)?=null):AndroidRecordingSyncStore {
            checkWorker()
            val parent=context.noBackupFilesDir.canonicalFile.toPath();val directory=parent.resolve(name)
            if(!Files.exists(directory,NOFOLLOW_LINKS)) {
                Files.createDirectory(directory);AndroidDurableSegments.syncDirectory(parent)
            }
            return AndroidRecordingSyncStore(directory,diagnostic)
        }
    }
}
