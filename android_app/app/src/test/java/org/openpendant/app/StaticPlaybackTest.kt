package org.openpendant.app

import android.media.AudioTrack
import org.junit.Assert.*
import org.junit.Test

class StaticPlaybackTest {
    private val pcm = ByteArray(640) { (it % 127).toByte() }

    private fun rejected(action: () -> Unit) {
        try { action(); fail("Invalid static playback setup accepted") } catch (_: ProtocolException) { }
    }

    @Test fun emptyStaticTrackIsLoadedBeforeReadyStateIsRequired() {
        var state = AudioTrack.STATE_NO_STATIC_DATA
        val calls = mutableListOf<String>()
        val original = pcm.copyOf()
        loadStaticPlayback(pcm, { calls.add("state:$state"); state }) { data ->
            calls.add("write")
            assertSame(pcm, data)
            state = AudioTrack.STATE_INITIALIZED
            data.size
        }
        assertEquals(listOf("state:2", "write", "state:1"), calls)
        assertArrayEquals(original, pcm)
    }

    @Test fun initializedStaticTrackAlsoRequiresExactlyOneFullWrite() {
        var writes = 0
        loadStaticPlayback(pcm, { AudioTrack.STATE_INITIALIZED }) { writes++; it.size }
        assertEquals(1, writes)
    }

    @Test fun invalidInitialStateDoesNotWrite() {
        for (value in listOf(AudioTrack.STATE_UNINITIALIZED, -1, 3)) {
            var writes = 0
            rejected { loadStaticPlayback(pcm, { value }) { writes++; it.size } }
            assertEquals(0, writes)
        }
    }

    @Test fun shortFailedOrExcessiveWritesAreRejectedWithoutRetry() {
        for (count in listOf(-6, -3, -2, -1, 0, 1, pcm.size - 1, pcm.size + 1)) {
            var writes = 0
            rejected {
                loadStaticPlayback(pcm, { AudioTrack.STATE_NO_STATIC_DATA }) { writes++; count }
            }
            assertEquals(1, writes)
        }
    }

    @Test fun fullWriteMustTransitionToInitialized() {
        for (after in listOf(AudioTrack.STATE_UNINITIALIZED, AudioTrack.STATE_NO_STATIC_DATA, -1, 3)) {
            var stateReads = 0
            var writes = 0
            rejected {
                loadStaticPlayback(pcm, {
                    if (stateReads++ == 0) AudioTrack.STATE_NO_STATIC_DATA else after
                }) { writes++; it.size }
            }
            assertEquals(2, stateReads)
            assertEquals(1, writes)
        }
    }

    @Test fun invalidAudioLengthDoesNotTouchTrack() {
        for (count in listOf(0, 1, 638, 642, 32002, 32640)) {
            rejected {
                loadStaticPlayback(ByteArray(count), { fail("State read for invalid PCM"); 0 }) {
                    fail("Write for invalid PCM"); 0
                }
            }
        }
    }

    @Test fun writeExceptionPropagatesToPlayerCleanupWithoutRetry() {
        var writes = 0
        val failure = IllegalStateException("fake output failure")
        try {
            loadStaticPlayback(pcm, { AudioTrack.STATE_NO_STATIC_DATA }) { writes++; throw failure }
            fail("Write exception swallowed")
        } catch (error: IllegalStateException) { assertSame(failure, error) }
        assertEquals(1, writes)
    }
}
