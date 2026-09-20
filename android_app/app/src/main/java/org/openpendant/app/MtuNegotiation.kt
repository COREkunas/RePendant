package org.openpendant.app

/** Android can report an existing MTU before service discovery completes. */
internal class MtuNegotiation {
    enum class Action { NONE, REQUEST_MTU, SUBSCRIBE }

    var mtu = 23
        private set
    private var servicesReady = false
    private var subscriptionStarted = false

    fun servicesDiscovered(): Action {
        ensure(!servicesReady, "Duplicate service discovery completion")
        servicesReady = true
        return if (mtu >= OpProtocol.MIN_MTU) startSubscription() else Action.REQUEST_MTU
    }

    fun changed(value: Int, success: Boolean): Action {
        ensure(success, "Bluetooth packet-size negotiation failed")
        ensure(value in 23..517, "Invalid Bluetooth packet size")
        // An initial default MTU is allowed before we request a larger one.
        // Once services are ready, a too-small result cannot carry our protocol.
        ensure(!servicesReady || value >= OpProtocol.MIN_MTU,
            "Bluetooth MTU must be at least 96 bytes")
        mtu = value
        return if (servicesReady && !subscriptionStarted) startSubscription() else Action.NONE
    }

    private fun startSubscription(): Action {
        ensure(servicesReady && !subscriptionStarted && mtu >= OpProtocol.MIN_MTU,
            "Secure subscription prerequisites are not ready")
        subscriptionStarted = true
        return Action.SUBSCRIBE
    }
}
