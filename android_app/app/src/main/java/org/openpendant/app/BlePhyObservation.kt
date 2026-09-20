package org.openpendant.app

/** Observed phone-side PHY, not the requested preference or application speed.
 * Null means unknown/failed; it must never be presented as negotiated 2M. */
data class BlePhyObservation(val transmit: Int, val receive: Int) {
    companion object {
        fun fromCallback(success: Boolean, transmit: Int, receive: Int): BlePhyObservation? =
            if (success && transmit in 1..3 && receive in 1..3) BlePhyObservation(transmit, receive) else null
    }
}
