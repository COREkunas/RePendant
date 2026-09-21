package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test

class RecordingSyncContentTest {
    @Test fun existingInstallDefaultsToAudioAndExplicitChoicesRoundTrip() {
        assertEquals(RecordingSyncContent.RECORDINGS_AND_AUDIO,TransferPreferences().content)
        assertEquals(RecordingSyncContent.RECORDINGS_AND_AUDIO,RecordingSyncContent.fromStored(null))
        for(content in RecordingSyncContent.entries)assertEquals(content,RecordingSyncContent.fromStored(content.storedValue))
    }
    @Test fun unknownSavedChoiceCannotAuthorizeAudioOrDeletion() {
        for(value in listOf("", "invalid", "AUDIO", "future-mode"))
            assertEquals(DurableSyncMode.INVENTORY_ONLY,RecordingSyncContent.fromStored(value).mode)
    }
    @Test fun contentSelectionPreservesOtherPreferencesAndNeverMeansDeletionOnly() {
        val full=TransferPreferences(automatic=true,removeAfterSync=true,lowBattery=true,lowBatteryPercent=35)
        val details=full.copy(content=RecordingSyncContent.DETAILS_ONLY)
        assertTrue(details.automatic&&details.removeAfterSync&&details.lowBattery)
        assertEquals(35,details.lowBatteryPercent)
        assertEquals(DurableSyncMode.INVENTORY_ONLY,details.content.mode)
        assertEquals(full,details.copy(content=RecordingSyncContent.RECORDINGS_AND_AUDIO))
    }
}
