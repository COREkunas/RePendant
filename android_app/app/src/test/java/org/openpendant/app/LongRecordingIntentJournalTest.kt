package org.openpendant.app

import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.StandardOpenOption.*
import java.util.UUID
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class LongRecordingIntentJournalTest {
    @get:Rule val folder=TemporaryFolder()
    private val data=LongControlTestData
    private fun path()=folder.root.toPath().toRealPath().resolve("owned")
    private fun create(event:(String)->Unit={})=LongRecordingIntentJournal.createExplicit(path(),data.binding,{ },event)
    private fun reopen()=LongRecordingIntentJournal.openExisting(path(),data.binding,{ })
    @Test fun exactStartStopObservationSurviveFreshInstanceAndCannotReplay() {
        val store=create();val start=data.request("starting");store.begin(UUID(5,6),start)
        assertTrue(reopen().read().getValue(data.operation).unresolved)
        assertThrows(IllegalStateException::class.java){reopen().begin(UUID(5,6),start)}
        store.observe(start,data.reply("starting",start))
        val stop=data.request("stopping");store.stop(UUID(7,8),stop);store.observe(stop,data.reply("stopping",stop))
        val status=data.request("stopped");store.observe(status,data.reply("stopped",status))
        val restored=reopen().read().getValue(data.operation)
        assertFalse(restored.unresolved);assertNotNull(restored.stop);assertEquals(499,restored.latest?.committed)
        assertThrows(IllegalStateException::class.java){store.stop(UUID(5,6),stop)}
    }
    @Test fun everyAppendCutRetainsIntentOrPartialFenceNeverBecomesVirgin() {
        for(stage in listOf("before_write","written","file_forced","directory_forced","readback")) {
            val root=folder.root.toPath().toRealPath().resolve(stage)
            val store=LongRecordingIntentJournal.createExplicit(root,data.binding,{ }) { if(it==stage)error("cut") }
            assertThrows(IllegalStateException::class.java){store.begin(UUID(5,6),data.request("starting"))}
            assertThrows(java.nio.file.FileAlreadyExistsException::class.java){LongRecordingIntentJournal.createExplicit(root,data.binding,{ })}
            val recovered=LongRecordingIntentJournal.openExisting(root,data.binding,{ }).read()
            assertEquals(if(stage=="before_write")0 else 1,recovered.size)
        }
    }
    @Test fun truncationTamperWrongBindingAndMissingFileFailClosed() {
        val store=create();store.begin(UUID(5,6),data.request("starting"));val file=path().resolve("intent.bin");val original=Files.readAllBytes(file)
        for(size in listOf(0,127,129,447)) {
            Files.write(file,original.copyOf(size));assertThrows(Exception::class.java){reopen()}
        }
        for(offset in listOf(0,127,128,150,190,300,447)) {
            val changed=original.copyOf();changed[offset]=(changed[offset].toInt() xor 1).toByte();Files.write(file,changed)
            assertThrows(Exception::class.java){reopen()}
        }
        Files.write(file,original)
        assertThrows(Exception::class.java){LongRecordingIntentJournal.openExisting(path(),data.binding.copy(bondAddress="12:34:56:78:9A:BD"),{ })}
        Files.delete(file);assertThrows(Exception::class.java){reopen()}
    }
    @Test fun staleObservationAndUnrelatedStopLeaveExactBytesUnchanged() {
        val store=create();val start=data.request("starting");store.begin(UUID(5,6),start)
        val run=data.request("running");store.observe(run,data.reply("running",run));val before=Files.readAllBytes(path().resolve("intent.bin"))
        assertThrows(IllegalArgumentException::class.java){store.observe(start,data.reply("starting",start))}
        assertThrows(IllegalStateException::class.java){store.stop(UUID(5,6),LongRecordingControlCodec.request(0x42,2,data.boot,UUID(99,1),data.binding))}
        assertArrayEquals(before,Files.readAllBytes(path().resolve("intent.bin")))
    }
    @Test fun directoryBarrierFailureNoCompletionAndParallelWriterRejected() {
        var fail=false
        val store=LongRecordingIntentJournal.createExplicit(path(),data.binding,{ if(fail)error("barrier") });fail=true
        assertThrows(IllegalStateException::class.java){store.begin(UUID(5,6),data.request("starting"))}
        assertTrue(reopen().read().containsKey(data.operation))
        FileChannel.open(path().resolve("intent.bin"),READ,WRITE).use { file ->file.lock().use {
            assertThrows(java.nio.channels.OverlappingFileLockException::class.java){reopen()}
        } }
    }
    @Test fun hourOfLiveProgressDoesNotAppendEveryPollAndTerminalIsDurable() {
        val store=create();val start=data.request("starting");store.begin(UUID(5,6),start)
        val status=data.request("running");val frame=data.reply("running",status)
        val b=ByteBuffer.wrap(frame).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        b.putInt(65,500);b.putInt(69,0);b.putInt(73,LongRecordingControlCodec.MAX_FRAMES)
        store.observe(status,frame)
        val file=path().resolve("intent.bin");val before=Files.size(file)
        for(i in 1..1800) {
            b.putInt(65,500+i*100);b.putInt(69,i*100)
            val observed=store.observe(status,frame);assertEquals(500+i*100,observed.accepted)
        }
        assertEquals(before,Files.size(file))
        val done=data.reply("stopped",status)
        val end=ByteBuffer.wrap(done).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        end.putInt(65,180500);end.putInt(69,180500);end.putInt(73,LongRecordingControlCodec.MAX_FRAMES)
        store.observe(status,done)
        assertEquals(before+LongRecordingIntentJournal.ENTRY_BYTES,Files.size(file))
        assertFalse(reopen().read().getValue(data.operation).unresolved)
        assertEquals(180500,reopen().read().getValue(data.operation).latest?.committed)
        store.observe(status,done);assertEquals(before+LongRecordingIntentJournal.ENTRY_BYTES,Files.size(file))
    }
}
