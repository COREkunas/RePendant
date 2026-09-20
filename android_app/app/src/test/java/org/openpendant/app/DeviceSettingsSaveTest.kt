package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class DeviceSettingsSaveTest {
    private val epoch=UUID(1,2)
    private val base=DevicePreferences(revision=9)
    private val wanted=base.copy(profile=1,connected=2)
    private val accepted=wanted.copy(revision=10)
    private fun started()=DeviceSettingsSave().also { assertTrue(it.begin(wanted,base,epoch,1000)) }
    @Test fun statusReadThenOneWriteThenExactReadback() {
        val s=started();assertTrue(s.busy);assertEquals(DeviceSettingsSave.Phase.CHECKING,s.phase)
        assertFalse(s.begin(wanted,base,epoch,2000))
        assertTrue(s.admit(epoch,base,6999,null));assertFalse(s.admit(epoch,base,6999,null))
        assertEquals(DeviceSettingsSave.Phase.WRITING,s.phase)
        assertEquals(accepted,s.acknowledged(accepted.encode()))
        assertEquals(DeviceSettingsSave.Phase.VERIFYING,s.phase)
        assertTrue(s.busy);s.verified(accepted)
        assertFalse(s.busy);assertEquals(DeviceSettingsSave.Phase.SAVED,s.phase)
        assertTrue(s.message!!.contains("verified"))
    }
    @Test fun changedConnectionDeadlineRevisionOrPowerNeverAdmits() {
        for(i in 0..4){val s=started()
            assertFalse(s.admit(if(i==0)null else if(i==1)UUID(3,4) else epoch,
                if(i==2)base.copy(revision=10) else base,if(i==3)7000 else 1001,if(i==4)"Charge the pendant." else null))
            assertFalse(s.busy);assertEquals(DeviceSettingsSave.Phase.NOT_SENT,s.phase)
            assertTrue(s.message!!.startsWith("Not saved."))
            assertFalse(s.admit(epoch,base,1001,null))
        }
    }
    @Test fun disconnectBeforeSendCannotBecomeDeferredWrite() {
        val s=started();s.disconnected();assertEquals(DeviceSettingsSave.Phase.NOT_SENT,s.phase)
        assertFalse(s.admit(epoch,base,1001,null));s.readObserved(base)
        assertEquals(DeviceSettingsSave.Phase.NOT_SENT,s.phase)
    }
    @Test fun uncertainWriteCannotRepeatBeforeExplicitRead() {
        for(ack in listOf(false,true)){
            val s=started();s.admit(epoch,base,1001,null);if(ack)s.acknowledged(accepted.encode())
            s.disconnected();assertEquals(DeviceSettingsSave.Phase.UNKNOWN,s.phase)
            assertFalse(s.begin(wanted,base,epoch,1002));assertFalse(s.refuse("busy"))
            assertEquals(DeviceSettingsSave.Phase.UNKNOWN,s.phase)
            s.readObserved(accepted);assertEquals(DeviceSettingsSave.Phase.SAVED,s.phase)
        }
    }
    @Test fun explicitReadCanShowChangeWasNotPresentWithoutRetry() {
        val s=started();s.admit(epoch,base,1001,null);s.disconnected();s.readObserved(base)
        assertEquals(DeviceSettingsSave.Phase.NOT_SENT,s.phase)
        assertFalse(s.admit(epoch,base,1002,null));assertEquals(wanted,s.request)
    }
    @Test fun invalidRevisionOrValuesRejectedWithFeedback() {
        for(v in listOf(wanted.copy(revision=8),wanted.copy(brightness=0),wanted.copy(profile=3),wanted.copy(revision=0xffffffffL))){
            val s=DeviceSettingsSave();assertFalse(s.begin(v,base,epoch,1));assertFalse(s.busy)
            assertEquals(DeviceSettingsSave.Phase.NOT_SENT,s.phase);assertNotNull(s.message)
        }
        assertFalse(DeviceSettingsSave().begin(wanted,null,epoch,1))
        assertFalse(DeviceSettingsSave().begin(wanted,base,null,1))
    }
    @Test fun mismatchedAcknowledgementAndReadbackNeverClaimSuccess() {
        val s=started();s.admit(epoch,base,1001,null)
        assertThrows(IllegalArgumentException::class.java){s.acknowledged(base.encode())}
        assertEquals(DeviceSettingsSave.Phase.WRITING,s.phase)
        s.acknowledged(accepted.encode())
        assertThrows(IllegalStateException::class.java){s.verified(accepted.copy(connected=3))}
        assertEquals(DeviceSettingsSave.Phase.VERIFYING,s.phase)
        s.disconnected();assertEquals(DeviceSettingsSave.Phase.UNKNOWN,s.phase)
    }
    @Test fun draftSurvivesReadRefreshAndDisconnectWithoutViewRebuild() {
        val d=DeviceSettingsDraft();d.observe("A",base);val generation=d.generation
        d.edit{copy(profile=1)};val draft=d.draft
        repeat(5){d.observe("A",base);assertEquals(draft,d.draft);assertEquals(generation,d.generation)}
        d.observe("A",null);assertEquals(draft,d.draft);assertTrue(d.dirty);assertNull(d.loaded)
        d.observe("A",base);assertEquals(draft,d.draft);assertFalse(d.conflict)
    }
    @Test fun changedPendantRevisionNeedsReviewNotSilentOverwrite() {
        val d=DeviceSettingsDraft();d.observe("A",base);d.edit{copy(profile=1)}
        d.observe("A",base.copy(revision=10,brightness=20))
        assertTrue(d.conflict);assertEquals(32,d.draft!!.brightness)
        d.discard();assertFalse(d.dirty);assertFalse(d.conflict);assertEquals(20,d.draft!!.brightness)
    }
    @Test fun verifiedOwnSaveClearsDraftAndUpdatesRevision() {
        val d=DeviceSettingsDraft();d.observe("A",base);d.edit{wanted}
        d.observe("A",accepted);assertFalse(d.dirty);assertEquals(accepted,d.draft)
        assertEquals(accepted,d.loaded)
    }
    @Test fun draftNeverMovesToAnotherPendant() {
        val d=DeviceSettingsDraft();d.observe("A",base);d.edit{wanted}
        d.observe("B",null);assertNull(d.draft);assertNull(d.loaded);assertFalse(d.dirty)
        d.observe("B",base);assertEquals(base,d.draft)
    }
}
