package org.openpendant.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.*
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock

/** Single-owner GATT state machine. Foreground recovery opens a new connection
 * only after closure proof; it never replays a recording or settings command. */
@SuppressLint("MissingPermission")
class PendantClient(private val context: Context, private val listener: Listener) {
    private companion object {
        // Activity recreation must not forget an earlier client's pending or
        // failed-close fence. There is deliberately no force-release/reset API.
        val processRadio = PendantRadioOwnership()
    }
    interface Listener {
        fun changed()
        fun recordingComplete(pcm: ByteArray)
    }
    data class Found(val device: BluetoothDevice, val title: String)
    private data class Transaction(val command: Int, val sequence: Int,
        val completion: (ByteArray) -> Unit, val gate: PendantRadioOwnership.Exchange,
        val failure: (() -> Unit)? = null)

    private val main = Handler(Looper.getMainLooper())
    private val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
    private var gatt: BluetoothGatt? = null
    private val radio = processRadio
    private val durablePriority = DurableTransferPriority()
    private val recovery = ConnectionRecovery()
    private val storageSubscriptionReset = StorageSubscriptionReset()
    internal var recoveredConnection = false
        private set
    internal fun recoverForegroundConnection(foreground: Boolean, available: Boolean) {
        check(Looper.myLooper() === main.looper)
        if(recovery.claim(selected?.address,SystemClock.elapsedRealtime(),foreground,
                available&&alive&&gatt==null&&radio.idle()&&isBonded&&bluetoothEnabled&&!pairing&&!recording))
            connect(false, recovering=true)
    }
    @Volatile private var radioConnection: PendantRadioOwnership.Connection? = null
    @Volatile private var observedLinkLoss: java.util.UUID? = null
    internal fun linkWasLost(epoch: java.util.UUID): Boolean = observedLinkLoss == epoch
    private data class Negotiated(val epoch: java.util.UUID, val capabilities: Long, val mtu: Int)
    @Volatile private var negotiated: Negotiated? = null
    var observedPhy: BlePhyObservation? = null
        private set
    private var writeCharacteristic: BluetoothGattCharacteristic? = null
    private var notifyCharacteristic: BluetoothGattCharacteristic? = null
    private var ccc: BluetoothGattDescriptor? = null
    private var transaction: Transaction? = null
    private var operation: String? = null
    private var timeout: Runnable? = null
    private var poll: Runnable? = null
    private var telemetryPoll: Runnable? = null
    private var capabilities = 0L
    var telemetry: TimedDeviceTelemetry? = null
        private set
    val telemetrySupported: Boolean get() = capabilities and DeviceTelemetry.CAPABILITY != 0L
    val canRefreshTelemetry: Boolean get() = ready && !recording && channelIdle && telemetrySupported
    var devicePreferences:DevicePreferences?=null
        private set
    internal val preferencesSave=DeviceSettingsSave()
    internal val preferencesDraft=DeviceSettingsDraft()
    internal val preferencesPeer:String? get()=selected?.address
    val preferencesSupported:Boolean get() = ready && capabilities and DevicePreferences.CAPABILITY != 0L
    val canReadPreferences:Boolean get() = preferencesSupported && !recording && channelIdle
    val preferencesSaveRefusal:String? get() = when {
        !preferencesSupported -> "Connect to a pendant that supports settings."
        !canReadPreferences -> "Wait for the current pendant operation to finish."
        else -> DeviceSettingsPower.refusal(telemetry,connected,SystemClock.elapsedRealtime(),capabilities)
    }
    val canSavePreferences:Boolean get() = preferencesSaveRefusal==null
    fun readPreferences() {
        if(!canReadPreferences)return
        try {
            request(OpProtocol.GET_PREFERENCES) { body ->
                devicePreferences=DevicePreferences.decode(body)
                preferencesSave.readObserved(checkNotNull(devicePreferences))
                update("Pendant settings loaded.")
            }
            listener.changed()
        } catch(_:Exception){fail("Pendant settings could not be read. Reconnect manually.")}
    }
    fun savePreferences(value:DevicePreferences):Boolean {
        check(Looper.myLooper()===main.looper)
        if(preferencesSave.busy||preferencesSave.phase==DeviceSettingsSave.Phase.UNKNOWN){listener.changed();return false}
        // Only a read-only dashboard query may already own the radio. Do not
        // queue behind recording/storage or carry an intent into a new epoch.
        val statusRead=transaction?.command==OpProtocol.DEVICE_STATUS && operation==null
        if(!preferencesSupported||recording||(!transportIdle&&!statusRead)){
            preferencesSave.refuse("Finish the current pendant operation, then try again.")
            listener.changed();return false
        }
        if(!preferencesSave.begin(value,devicePreferences,currentRadioEpoch(),SystemClock.elapsedRealtime())){
            listener.changed();return false
        }
        try {
            // An in-flight status reply will continue this request; otherwise
            // obtain a fresh one now. Keep all existing firmware power checks.
            if(!statusRead)readTelemetry()
            listener.changed()
        } catch(_:Exception){fail("Settings check failed. Nothing was retried.")}
        return true
    }
    private fun savePreferencesAfterStatus() {
        if(preferencesSave.phase!=DeviceSettingsSave.Phase.CHECKING)return
        val reason=if(!transportIdle)"Another pendant operation is active. Try again when it finishes."
            else DeviceSettingsPower.refusal(telemetry,connected,SystemClock.elapsedRealtime(),capabilities)
        if(!preferencesSave.admit(currentRadioEpoch(),devicePreferences,SystemClock.elapsedRealtime(),reason))return
        val value=checkNotNull(preferencesSave.request)
        try {
            request(OpProtocol.SET_PREFERENCES,value.encode()) { body ->
                val acknowledged=preferencesSave.acknowledged(body)
                request(OpProtocol.GET_PREFERENCES) { saved ->
                    val readback=DevicePreferences.decode(saved)
                    check(readback==acknowledged)
                    preferencesSave.verified(readback)
                    devicePreferences=readback
                    update("Settings saved on the pendant and verified.")
                }
                listener.changed()
            }
        }catch(_:Exception){fail("Settings were not confirmed. Reconnect and read them before trying again.")}
    }
    private val transportIdle: Boolean get() = transaction == null && operation == null && radio.idle()
    val channelIdle: Boolean get() = !preferencesSave.busy && transportIdle
    private var clipDeadline: Runnable? = null
    private var scanDeadline: Runnable? = null
    private var pairingDeadline: Runnable? = null
    private var nextSequence = 1
    private var storageContinuation = false
    @Volatile internal var lastWriteFailureStatus: Int = -1
        private set
    private var clipId = 0L
    private var assembler: ClipAssembler? = null
    private var cancellationRequested = false
    private var cancelSent = false
    private var recordDeadlineMs = 0L
    private var lastClipState = ClipState.WAITING
    private var alive = true
    val found = linkedMapOf<String, Found>()
    var selected: BluetoothDevice? = null
        private set
    var message = "Scan when you are ready. Nothing connects or records automatically."
        private set
    var scanning = false
        private set
    var connected = false
        private set
    var ready = false
        private set
    var recording = false
        private set
    var pairing = false
        private set
    private var usbPairing = false
    internal fun beginUsbPairing(): Boolean {
        check(Looper.myLooper() === main.looper)
        if (!alive || gatt != null || !radio.idle() || recording || pairing || usbPairing) return false
        stopScan(); recovery.stop(); usbPairing = true; pairing = true
        update("USB pairing in progress. No recording or transfer starts here.")
        return true
    }
    internal fun endUsbPairing() {
        check(Looper.myLooper() === main.looper)
        if (!usbPairing) return
        usbPairing = false; pairing = false
        update("USB setup closed. Select the paired pendant and tap Connect when ready.")
    }
    var progress = 0
        private set
    private var mtuNegotiation = MtuNegotiation()
    val mtu: Int get() = mtuNegotiation.mtu
    val connecting: Boolean get() = gatt != null && !connected

    val isBonded: Boolean get() = hasConnectPermission() && selected?.bondState == BluetoothDevice.BOND_BONDED
    val bluetoothEnabled: Boolean get() = hasConnectPermission() && adapter?.isEnabled == true
    val cancelling: Boolean get() = cancellationRequested
    val currentClipState: Int? get() = if (recording) lastClipState else null

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            if (usbPairing) return // The bounded wired setup owns this enrollment.
            @Suppress("DEPRECATION")
            val device = intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
            if (device != selected) return
            val bond = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)
            pairing = bond == BluetoothDevice.BOND_BONDING
            if (!pairing) { pairingDeadline?.let(main::removeCallbacks); pairingDeadline = null }
            when (bond) {
                BluetoothDevice.BOND_BONDED -> update("Paired. Tap Connect to verify the pendant channel.")
                BluetoothDevice.BOND_BONDING -> update("Complete Android's pairing dialog using the passkey shown over pendant USB.")
                BluetoothDevice.BOND_NONE -> {
                    if (gatt != null) disconnect("Pairing was removed; connection closed.")
                    else update("Pairing not completed. Open the USB pairing window and try Pair again.")
                }
            }
        }
    }

    init {
        val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(bondReceiver, filter, Context.RECEIVER_EXPORTED)
        else @Suppress("DEPRECATION") context.registerReceiver(bondReceiver, filter)
    }

    private fun hasConnectPermission() = Build.VERSION.SDK_INT < 31 ||
        context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    private fun hasScanPermission() = if (Build.VERSION.SDK_INT >= 31)
        context.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
        else context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    private fun update(text: String) { message = text; listener.changed() }
    private fun dispatch(source: BluetoothGatt, epoch: PendantRadioOwnership.Connection, action: () -> Unit) {
        main.post {
            if (!alive || source !== gatt || !radio.accepts(epoch, source)) return@post
            try { action() }
            catch (error: ProtocolException) { fail(error.message ?: "Protocol validation failed") }
            catch (_: SecurityException) { fail("Bluetooth permission was removed.") }
            catch (_: Exception) { fail("Bluetooth operation failed; no automatic retry.") }
        }
    }

    private val scannerCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            main.post {
                if (!alive || !scanning || !hasConnectPermission()) return@post
                val address = result.device.address
                if (found.size >= 20 && !found.containsKey(address)) return@post
                // Names are display-only. Selection is authenticated by Android pairing
                // and the firmware's secured CCC/control attributes, not by a name.
                val advertised = result.scanRecord?.deviceName ?: result.device.name ?: "OpenPendant"
                val safeTitle = advertised.filter { !it.isISOControl() }.take(40)
                found[address] = Found(result.device, "$safeTitle · ${address.takeLast(5)}")
                listener.changed()
            }
        }
        override fun onScanFailed(errorCode: Int) { main.post { stopScan(); update("Bluetooth scan failed. No automatic retry.") } }
    }

    fun scan() {
        if (!hasScanPermission() || !hasConnectPermission()) { update("Allow Nearby devices to scan."); return }
        if (!bluetoothEnabled) { update("Turn Bluetooth on, then tap Scan again."); return }
        if (gatt != null || recording || pairing) { update("Disconnect or finish pairing before scanning."); return }
        stopScan()
        found.clear()
        scanning = true
        try {
            adapter?.bluetoothLeScanner?.startScan(
                listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(OpProtocol.SERVICE)).build()),
                ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scannerCallback)
            scanDeadline = Runnable { stopScan(); update(if (found.isEmpty()) "No pendant found. Check its power and try Scan again." else "Choose your pendant below.") }
            main.postDelayed(scanDeadline!!, 10000)
            update("Scanning for OpenPendant… select a device below.")
        } catch (_: Exception) { stopScan(); update("Could not start Bluetooth scan.") }
    }

    private fun stopScan() {
        scanDeadline?.let(main::removeCallbacks); scanDeadline = null
        if (scanning && hasScanPermission()) try { adapter?.bluetoothLeScanner?.stopScan(scannerCallback) } catch (_: Exception) { }
        scanning = false
    }

    fun select(device: BluetoothDevice) {
        if (gatt != null || recording || pairing) return
        stopScan()
        if (device != selected) { recovery.stop(); telemetry = null; capabilities = 0; preferencesSave.reset() }
        selected = device
        update(if (isBonded) "Pendant selected. Tap Connect." else "Pendant selected. First open its USB pairing window, then tap Pair.")
    }

    fun pair() {
        val device = selected ?: return
        if (!hasConnectPermission() || gatt != null || recording || pairing) return
        if (device.bondState == BluetoothDevice.BOND_BONDED) { update("Already paired. Tap Connect."); return }
        if (device.bondState == BluetoothDevice.BOND_BONDING) {
            update("Android is still pairing. Complete or cancel its native dialog before trying again."); return
        }
        stopScan()
        try {
            if (!device.createBond()) { update("Android could not start pairing. Check the pendant's USB pairing window."); return }
            pairing = true
            pairingDeadline?.let(main::removeCallbacks)
            pairingDeadline = Runnable {
                pairing = false
                update("Pairing has not completed within 60 seconds. Complete or cancel Android's dialog; no automatic retry or unpairing.")
            }
            main.postDelayed(pairingDeadline!!, 60000)
            update("Use Android's secure pairing dialog. Enter only the fresh passkey displayed over pendant USB.")
        } catch (_: Exception) { update("Pairing could not start. Nothing was recorded.") }
    }

    fun connect() {
        if(gatt!=null)return
        selected?.address?.let { recovery.explicitConnect(it,SystemClock.elapsedRealtime()) }
        connect(false)
    }
    private fun connect(continuation: Boolean, recovering: Boolean = false) {
        val device = selected ?: return
        if (!isBonded || gatt != null || pairing || !bluetoothEnabled) {
            update("Pair the selected pendant first, with Bluetooth enabled."); return
        }
        stopScan()
        recoveredConnection = recovering || continuation
        storageContinuation = continuation
        lastWriteFailureStatus = -1
        nextSequence = 1; mtuNegotiation = MtuNegotiation()
        // Keep last values visible, but permanently stale until a new reply.
        telemetry = telemetry?.copy(receivedMs = Long.MIN_VALUE)
        capabilities = 0; observedPhy = null
        try {
            val epoch = radio.open(); radioConnection = epoch
            operation = "connect"
            armTimeout(10000, "Connection timed out. No automatic reconnect.")
            gatt = device.connectGatt(context, false, callbacks(epoch), BluetoothDevice.TRANSPORT_LE)
            if (gatt == null) fail("Android could not open the Bluetooth connection.")
            else { radio.attach(epoch, gatt!!); update("Connecting to the paired pendant…") }
        } catch (_: Exception) { fail("Connection could not start.") }
    }

    private fun callbacks(epoch: PendantRadioOwnership.Connection) = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(source: BluetoothGatt, status: Int, newState: Int) = dispatch(source, epoch) {
            if (status != BluetoothGatt.GATT_SUCCESS || newState == BluetoothProfile.STATE_DISCONNECTED) {
                if (ready && connected) observedLinkLoss = epoch.epoch
                disconnectInternal("Bluetooth interrupted. Saved progress is retained.", recover = ready && connected, planned=false)
                return@dispatch
            }
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                ensure(operation == "connect" && isBonded, "Unexpected or unpaired connection")
                cancelTimeout(); connected = true; operation = "services"
                armTimeout(8000, "Service discovery timed out.")
                ensure(source.discoverServices(), "Could not discover pendant services")
                update("Checking pendant services…")
            }
        }

        override fun onServicesDiscovered(source: BluetoothGatt, status: Int) = dispatch(source, epoch) {
            ensure(operation == "services" && status == BluetoothGatt.GATT_SUCCESS, "Service discovery failed")
            val service = source.getService(OpProtocol.SERVICE) ?: throw ProtocolException("OpenPendant service is missing")
            val write = service.getCharacteristic(OpProtocol.WRITE) ?: throw ProtocolException("Control characteristic is missing")
            val notify = service.getCharacteristic(OpProtocol.NOTIFY) ?: throw ProtocolException("Response characteristic is missing")
            ensure(write.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 &&
                notify.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0, "Unexpected GATT characteristic properties")
            writeCharacteristic = write; notifyCharacteristic = notify
            ccc = notify.getDescriptor(OpProtocol.CCC) ?: throw ProtocolException("Response subscription descriptor is missing")
            cancelTimeout(); operation = "mtu"
            when (mtuNegotiation.servicesDiscovered()) {
                MtuNegotiation.Action.SUBSCRIBE -> subscribeSecurely(source)
                MtuNegotiation.Action.REQUEST_MTU -> {
                    armTimeout(5000, "Bluetooth MTU negotiation timed out.")
                    ensure(source.requestMtu(247), "Could not negotiate Bluetooth packet size")
                }
                MtuNegotiation.Action.NONE -> throw ProtocolException("Missing packet-size setup action")
            }
        }

        override fun onMtuChanged(source: BluetoothGatt, value: Int, status: Int) = dispatch(source, epoch) {
            // A connection event can deliver this before discoverServices, or
            // repeat it later. Cache it without disturbing another stage's timer.
            if (mtuNegotiation.changed(value, status == BluetoothGatt.GATT_SUCCESS) == MtuNegotiation.Action.SUBSCRIBE) {
                subscribeSecurely(source)
            }
        }

        override fun onPhyUpdate(source: BluetoothGatt, txPhy: Int, rxPhy: Int, status: Int) = dispatch(source, epoch) {
            // The common dispatch rejects old/revoked GATT epochs. This is
            // observation only: no GATT completion, timer change or authority.
            observedPhy = BlePhyObservation.fromCallback(status == BluetoothGatt.GATT_SUCCESS, txPhy, rxPhy)
        }

        override fun onDescriptorWrite(source: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) = dispatch(source, epoch) {
            if (operation == "ccc-retire") {
                ensure(descriptor === ccc && status == BluetoothGatt.GATT_SUCCESS,
                    "Previous storage subscription could not be closed")
                writeSubscription(source, true)
                return@dispatch
            }
            ensure(operation == "ccc" && descriptor === ccc && status == BluetoothGatt.GATT_SUCCESS,
                "Secure subscription failed. Check the authenticated Android pairing.")
            cancelTimeout(); operation = null
            radio.setupTransportCompleted(epoch)
            val echo = "Android".toByteArray(Charsets.US_ASCII)
            request(OpProtocol.PING, echo) { body ->
                ensure(body.contentEquals(echo), "Pendant PING reply was not exact")
                finishSetup(source, epoch)
            }
        }

        override fun onCharacteristicWrite(source: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) = dispatch(source, epoch) {
            val pending = transaction ?: throw ProtocolException("Unexpected GATT write completion")
            ensure(characteristic === writeCharacteristic, "Unexpected write characteristic")
            if (status != BluetoothGatt.GATT_SUCCESS) lastWriteFailureStatus = status
            pending.gate.written(status == BluetoothGatt.GATT_SUCCESS)
            finishRequestIfReady()
        }

        @Deprecated("Used only on Android 12 and below")
        override fun onCharacteristicChanged(source: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) {
                @Suppress("DEPRECATION") val packet = characteristic.value?.copyOf() ?: byteArrayOf()
                receive(source, epoch, characteristic, packet)
            }
        }
        override fun onCharacteristicChanged(source: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            receive(source, epoch, characteristic, value.copyOf())
        }
    }

    private fun subscribeSecurely(source: BluetoothGatt) {
        ensure(operation == "mtu" && connected && isBonded && mtu >= OpProtocol.MIN_MTU,
            "Unexpected secure subscription stage")
        cancelTimeout()
        ensure(source.setCharacteristicNotification(notifyCharacteristic!!, true), "Could not enable secured notifications")
        // Explicitly revoke any prior catalog on a retained physical link.
        // The existing pendant CCC callback drains its admitted storage worker.
        writeSubscription(source, !storageContinuation && !storageSubscriptionReset.required(selected?.address))
    }
    private fun writeSubscription(source: BluetoothGatt, enable: Boolean) {
        cancelTimeout()
        operation = if (enable) "ccc" else "ccc-retire"
        armTimeout(5000, "Secure notification subscription timed out.")
        val descriptor = ccc!!
        val value = if (enable) BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE else BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
        val sent = if (Build.VERSION.SDK_INT >= 33)
            source.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS
        else {
            @Suppress("DEPRECATION") descriptor.value = value
            @Suppress("DEPRECATION") source.writeDescriptor(descriptor)
        }
        ensure(sent, "Could not write secured notification subscription")
        update("Authenticating the response channel…")
    }

    private fun finishSetup(source: BluetoothGatt, epoch: PendantRadioOwnership.Connection) {
        request(OpProtocol.INFO) { info ->
            capabilities = DurableBleCodec.validatedCapabilities(info)
            fun complete() {
                radio.setupValidated(epoch)
                ready = true
                negotiated = Negotiated(epoch.epoch, capabilities, mtu)
                update(if (info[6].toInt() == 1) "Connected securely · pendant microphone is active. Check long-recording status."
                    else "Connected securely · microphone off.")
                if (telemetrySupported) refreshTelemetry()
            }
            if (!storageContinuation) complete()
            else {
                ensure(telemetrySupported, "Storage cleanup status is unavailable")
                request(OpProtocol.DEVICE_STATUS) { bytes ->
                    val status = DeviceTelemetry.parse(bytes)
                    telemetry = TimedDeviceTelemetry(status, SystemClock.elapsedRealtime())
                    if (StorageContinuationCheck.retired(status,capabilities)) {
                        storageSubscriptionReset.observed(selected?.address,true)
                        complete()
                    }
                    else {
                        // The ORIGINAL 45s continuation deadline still applies.
                        // Cached status/INFO only; no storage command or reset.
                        operation = "storage-retirement"
                        main.postDelayed({
                            if (alive && source === gatt && radio.accepts(epoch, source) && operation == "storage-retirement") {
                                operation = null
                                try { finishSetup(source, epoch) }
                                catch (_: Exception) { fail("Previous storage cleanup could not be confirmed") }
                            }
                        }, 250)
                    }
                }
            }
        }
    }

    private fun receive(source: BluetoothGatt, epoch: PendantRadioOwnership.Connection, characteristic: BluetoothGattCharacteristic, packet: ByteArray) {
        main.post {
            try {
                if (!alive || source !== gatt || !radio.accepts(epoch, source)) return@post
                val pending = transaction ?: throw ProtocolException("Unexpected unsolicited response")
                ensure(characteristic === notifyCharacteristic, "Unexpected response characteristic")
                pending.gate.notified(packet)
                finishRequestIfReady()
            } catch (error: ProtocolException) { fail(error.message ?: "Response validation failed") }
            catch (_: Exception) { fail("Bluetooth response failed validation.") }
            finally { packet.fill(0) }
        }
    }

    private fun request(command: Int, payload: ByteArray = byteArrayOf(), completion: (ByteArray) -> Unit) =
        submit(command, payload, completion)

    /** Dormant adapter seam. Must be called on main after the typed
     * transport marshals its worker request here without holding file/coordinator
     * locks. Durable commands require the actual INFO capability, not UI state.
     * Response body is borrowed until callback returns, then wiped. Failure
     * callback means local channel closed/failed, never remote cancellation.
     * The logical call is completed by its adapter only after final validation. */
    internal fun exchangeBorrowed(lease: DurableTransportLease, call: DurableSyncCall,
        command: Int, payload: ByteArray, completion: (ByteArray) -> Unit, failure: () -> Unit) {
        check(Looper.myLooper() === main.looper)
        check(ready && connected && isBonded && !recording && operation == null && transaction == null)
        val epoch = checkNotNull(radioConnection)
        // Reject stale/foreign producers BEFORE a catch that may close a client.
        // Such a producer must never disrupt a new connection/capture session.
        radio.checkBorrowAdmission(epoch, lease, call, command)
        val validation = encodeRequest(command, nextSequence, payload, lease)
        try { ensure(validation.size <= mtu - 3, "Request exceeds negotiated Bluetooth packet size") }
        finally { validation.fill(0) }
        ensure(call.deadlineMillis > System.nanoTime() / 1_000_000L, "Bluetooth request deadline expired")
        try {
            call.checkActive()
            if (DurableBleCodec.isDurable(command) && durablePriority.begin(lease)) {
                connectionPriority(true)
                preferFastPhy()
            }
            if(DurableBleCodec.isDurable(command))storageSubscriptionReset.touched(checkNotNull(selected?.address))
            submit(command, payload, completion, lease, call, failure)
        }
        catch (_: Exception) {
            // Includes a synchronous send rejection: close before reporting.
            val wasSubmitted = transaction?.failure === failure
            disconnect("Durable transport request failed. No automatic retry.")
            if (!wasSubmitted) notifyTransportFailure(failure)
        }
    }

    /** Pure worker-safe admission from the owned ready epoch; no Android call,
     * posting or wait. Main-thread submission rechecks bond/GATT/CCC/ready.
     * Caller retires after its complete logical session, not each frame. */
    internal fun acquireDurableSession(expectedEpoch: java.util.UUID): DurableTransportLease {
        check(!preferencesSave.busy)
        val epoch = checkNotNull(radioConnection)
        check(epoch.epoch == expectedEpoch && radio.currentEpoch() == expectedEpoch)
        check(durableCapabilities(expectedEpoch)?.catalog == true)
        return radio.acquireDurable(epoch)
    }
    internal fun longPeer(): LongRecordingPeer? {
        check(Looper.myLooper() === main.looper)
        val n = negotiated ?: return null
        if (!ready || !connected || !isBonded || !isCurrentRadioEpoch(n.epoch) ||
            n.capabilities and LongRecordingControlCodec.CAPABILITY == 0L) return null
        return LongRecordingPeer(n.epoch, selected?.address ?: return null, n.capabilities)
    }
    internal fun acquireLong(peer: LongRecordingPeer): DurableTransportLease {
        check(!preferencesSave.busy)
        // Worker-safe: negotiated is immutable/volatile; radio verifies actual
        // setup completion and sole ownership under its short monitor.
        val n = checkNotNull(negotiated);val epoch = checkNotNull(radioConnection)
        check(n.epoch == peer.epoch && n.capabilities == peer.capabilityBits && epoch.epoch == peer.epoch &&
            n.capabilities and LongRecordingControlCodec.CAPABILITY != 0L && isCurrentRadioEpoch(peer.epoch))
        return radio.acquireLong(epoch)
    }
    internal fun closeLong(lease: DurableTransportLease) {
        check(Looper.myLooper() === main.looper)
        val epoch = radioConnection ?: return
        if (radio.ownsLong(epoch, lease)) disconnectInternal("Recording control finished. Restoring the status connection…", recover=true)
    }
    internal fun exchangeLong(lease: DurableTransportLease, ticket: DurableTransportRequest,
        request: LongRecordingControlCodec.Request, deadline: Long, cancelled: () -> Boolean,
        success: (ByteArray) -> Unit, failure: () -> Unit) {
        check(Looper.myLooper() === main.looper)
        ticket.requirePending()
        check(ticket.belongsTo(lease) && !cancelled() && !recording && ready && connected && isBonded)
        check(longPeer()?.epoch == lease.epoch && operation == null && transaction == null)
        val remaining = deadline - System.nanoTime() / 1_000_000
        check(remaining in 1..4000)
        val epoch = checkNotNull(radioConnection)
        val packet = request.frame();check(packet.size == 80 && packet.size <= mtu - 3)
        val source = checkNotNull(gatt); val characteristic = checkNotNull(writeCharacteristic)
        val gate = radio.borrowLong(epoch, lease, ticket, request.command, request.sequence)
        transaction = Transaction(request.command, request.sequence, { body ->
            // ResponseGate already checked exact OP command/sequence/status and
            // both callbacks. Restore the canonical frame for the domain parser.
            check(body.size == 72)
            val frame = ByteArray(81)
            frame[0]=79;frame[1]=80;frame[2]=1;frame[3]=(request.command or 128).toByte()
            frame[4]=request.sequence.toByte();frame[5]=(request.sequence shr 8).toByte();frame[6]=73
            body.copyInto(frame,9)
            try { success(frame) } finally { frame.fill(0) }
        }, gate, failure)
        armTimeout(remaining, "Recording command outcome unknown. Reconnect and check status; do not repeat Start.")
        check(!cancelled() && System.nanoTime()/1_000_000 < deadline)
        val sent = if (Build.VERSION.SDK_INT >= 33)
            source.writeCharacteristic(characteristic,packet,BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)==BluetoothStatusCodes.SUCCESS
        else {
            @Suppress("DEPRECATION") characteristic.value=packet
            characteristic.writeType=BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION") source.writeCharacteristic(characteristic)
        }
        check(sent)
    }
    internal fun durableCapabilities(epoch: java.util.UUID): DurableSyncCapabilities? {
        val value = negotiated ?: return null
        if (value.epoch != epoch || !isCurrentRadioEpoch(epoch)) return null
        val full = value.capabilities and DurableBleCodec.FULL_CAPABILITY != 0L
        val enabled = full || value.capabilities and DurableBleCodec.CAPABILITY != 0L
        return DurableSyncCapabilities(enabled, enabled, enabled, enabled, full,
            DurableBleCodec.wideEnabled(value.capabilities, value.mtu), DurableBleCodec.streamEnabled(value.capabilities, value.mtu),
            full && value.capabilities and DurableBleCodec.RECEIPT_BATCH_CAPABILITY != 0L)
    }
    /** UI captures this public peer selector on main; a worker must still check
     * the exact epoch on every callback. It is NOT enrollment authority. */
    internal fun durablePeer(): DurableConnectedPeer? {
        check(Looper.myLooper() === main.looper)
        if (!ready || !connected || !isBonded) return null
        val epoch = currentRadioEpoch() ?: return null
        return DurableConnectedPeer(epoch, selected?.address ?: return null, durableCapabilities(epoch) ?: return null)
    }
    /** Exactly one continuation after a SUCCESSFUL checkpointed full-storage
     * batch. Never called to recover a failed exchange. Wait for proven local
     * closure, select the same bond, then require a fresh authenticated epoch. */
    internal fun continueDurableBatch(previous: DurableConnectedPeer, cancelled: () -> Boolean,
        complete: (DurableConnectedPeer?) -> Unit) {
        openNextDurableConnection(previous, cancelled, complete)
    }
    /** A new independently authenticated session after an observed remote link
     * loss. No replay on an old epoch and no bypass of a failed-close fence. */
    internal fun resumeDurableAfterLinkLoss(previous: DurableConnectedPeer, cancelled: () -> Boolean,
        complete: (DurableConnectedPeer?) -> Unit) {
        check(Looper.myLooper() === main.looper)
        if (!linkWasLost(previous.epoch)) { complete(null); return }
        openNextDurableConnection(previous, cancelled, complete)
    }
    private fun openNextDurableConnection(previous: DurableConnectedPeer, cancelled: () -> Boolean,
        complete: (DurableConnectedPeer?) -> Unit) {
        check(Looper.myLooper() === main.looper)
        val deadline = SystemClock.elapsedRealtime() + 45_000L
        var opened: java.util.UUID? = null
        fun stop() {
            if (opened != null && radioConnection?.epoch == opened)
                disconnect("Storage continuation stopped; saved progress is retained.")
            complete(null)
        }
        val poll = object : Runnable {
            override fun run() {
                if (cancelled() || !alive || SystemClock.elapsedRealtime() >= deadline ||
                    selected?.address != previous.bondAddress || !isBonded || !bluetoothEnabled || pairing || recording) {
                    stop(); return
                }
                val current = radioConnection?.epoch
                if (opened == null) {
                    if (current != null && current != previous.epoch) { stop(); return }
                    if (current == null && gatt == null && radio.idle()) {
                        connect(true); opened = radioConnection?.epoch
                        if (opened == null || opened == previous.epoch) { stop(); return }
                    }
                } else {
                    if (current != opened) { stop(); return }
                    // INFO makes the peer ready before its automatic dashboard
                    // query has finished. Wait for that CONTROL owner to retire;
                    // a ready peer alone is not permission to acquire DURABLE.
                    (if (channelIdle) durablePeer() else null)?.let { peer ->
                        if (peer.epoch != opened || peer.capabilities != previous.capabilities) { stop(); return }
                        complete(peer); return
                    }
                }
                main.postDelayed(this, 50)
            }
        }
        poll.run()
    }
    /** Pure thread-safe epoch check; not owner/volume/capability authentication. */
    internal fun isCurrentRadioEpoch(epoch: java.util.UUID): Boolean = currentRadioEpoch() == epoch
    internal fun currentRadioEpoch(): java.util.UUID? {
        val own = radioConnection ?: return null
        return radio.currentEpoch()?.takeIf { it == own.epoch }
    }
    /** Explicit local-channel cancellation, never proof that remote NAND work
     * stopped. Stale owners cannot close a later connection. */
    internal fun cancelDurableSession(lease: DurableTransportLease) {
        check(Looper.myLooper() === main.looper)
        val epoch = radioConnection ?: return
        if (radio.ownsDurable(epoch, lease)) disconnectInternal("Storage channel closed. Restoring the status connection; no storage action is repeated.", recover=true)
    }

    private fun submit(command: Int, payload: ByteArray, completion: (ByteArray) -> Unit,
        lease: DurableTransportLease? = null, call: DurableSyncCall? = null, failure: (() -> Unit)? = null) {
        ensure(operation == null && transaction == null && connected && isBonded,
            "Another Bluetooth operation is active or secure connection was lost")
        ensure(nextSequence <= 65535, "Connection sequence exhausted; reconnect manually")
        val source = gatt ?: throw ProtocolException("No Bluetooth connection")
        val characteristic = writeCharacteristic ?: throw ProtocolException("Missing control channel")
        val sequence = nextSequence++
        val packet = encodeRequest(command, sequence, payload, lease)
        ensure(packet.size <= mtu - 3, "Request exceeds negotiated Bluetooth packet size")
        val epoch = radioConnection ?: throw ProtocolException("Bluetooth epoch is unavailable")
        val gate = if (lease == null) radio.legacy(epoch, command, sequence)
            else radio.borrow(epoch, lease, checkNotNull(call), command, sequence,
                if (command == DurableBleCodec.FULL_STREAM) DurableBleStreamAssembler(payload) else null)
        val frameBudget = if (command == DurableBleCodec.FULL_GET_CATALOG && payload.size == 22 && OpProtocol.u32(payload, 16) == 0L)
            DurableRecordingSyncSession.FULL_METADATA_MILLIS else if (command == DurableBleCodec.FULL_RECEIVE_RANGE)
            DurableRecordingSyncSession.MAX_CALL_MILLIS else 4000L
        val remaining = if (call == null) 4000L else minOf(frameBudget, call.deadlineMillis - System.nanoTime() / 1_000_000L)
        ensure(remaining > 0, "Bluetooth request deadline expired")
        transaction = Transaction(command, sequence, completion, gate, failure)
        armTimeout(remaining, "Bluetooth command timed out. Partial audio discarded; no retry.")
        call?.checkActive() // Original absolute deadline also covers submission work.
        val sent = if (Build.VERSION.SDK_INT >= 33)
            source.writeCharacteristic(characteristic, packet, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        else {
            @Suppress("DEPRECATION") characteristic.value = packet
            characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION") source.writeCharacteristic(characteristic)
        }
        ensure(sent, "Could not send Bluetooth control request")
    }

    private fun encodeRequest(command: Int, sequence: Int, payload: ByteArray,
        lease: DurableTransportLease?): ByteArray {
        if (!DurableBleCodec.isDurable(command))
            return OpProtocol.encode(command, sequence, payload)
        check(lease != null && durableCapabilities(lease.epoch)?.catalog == true)
        check(durableCapabilities(lease.epoch)?.fullStorage == DurableBleCodec.isFull(command))
        val packet = DurableBleCodec.encodeRequest(command, sequence, payload)
        if (command == DurableBleCodec.FULL_RECEIVE_RANGE) ensure(durableCapabilities(lease.epoch)?.receiptBatch == true,
            "Receipt batches were not negotiated")
        if (command == DurableBleCodec.FULL_STREAM) ensure(durableCapabilities(lease.epoch)?.rangeStream == true && mtu >= DurableBleCodec.WIDE_MIN_MTU,
            "Storage streaming was not negotiated")
        if (command in DurableBleCodec.FULL_GET_CATALOG..DurableBleCodec.FULL_GET_SEGMENT) {
            val offset = when (command) { DurableBleCodec.FULL_GET_CATALOG -> 20; DurableBleCodec.FULL_GET_MANIFEST -> 72; else -> 40 }
            val maximum = payload[offset].toInt() and 255
            ensure(maximum <= DurableBleCodec.FULL_MAX_FRAGMENT || durableCapabilities(lease.epoch)?.wideFragments == true,
                "Large storage packets were not negotiated")
            ensure(33 + maximum <= mtu - 3, "Storage reply exceeds negotiated Bluetooth packet size")
        }
        return packet
    }

    private fun finishRequestIfReady() {
        val pending = transaction ?: return
        if (!pending.gate.ready) return
        val body = pending.gate.take()
        cancelTimeout(); transaction = null
        try {
            // BEGIN must first provide the fresh identifier before cancellation.
            if (cancellationRequested && !cancelSent && pending.command != OpProtocol.BEGIN && pending.command != OpProtocol.CANCEL) sendCancel()
            else pending.completion(body)
        } catch (error: Exception) {
            if (pending.failure == null) throw error
            disconnect("Durable response validation failed. No automatic retry.")
            notifyTransportFailure(pending.failure)
        } finally { body.fill(0) }
    }

    /** Read-only, serialized status; never schedules capture, key setup or NAND access. */
    fun refreshTelemetry() {
        if (!canRefreshTelemetry) return
        readTelemetry()
    }
    private fun readTelemetry() {
        telemetryPoll?.let(main::removeCallbacks); telemetryPoll = null
        try {
            request(OpProtocol.DEVICE_STATUS) { body ->
                telemetry = TimedDeviceTelemetry(DeviceTelemetry.parse(body), SystemClock.elapsedRealtime())
                storageSubscriptionReset.observed(selected?.address,telemetry!!.value,capabilities)
                savePreferencesAfterStatus()
                listener.changed()
                scheduleTelemetry()
            }
        } catch (_: Exception) { fail("Device status could not be verified. Reconnect manually.") }
    }

    private fun scheduleTelemetry() {
        telemetryPoll?.let(main::removeCallbacks)
        if (!connected || !telemetrySupported) { telemetryPoll = null; return }
        telemetryPoll = Runnable {
            if (canRefreshTelemetry) refreshTelemetry()
            else { listener.changed(); scheduleTelemetry() }
        }
        main.postDelayed(telemetryPoll!!, 5000)
    }

    fun record() {
        if (!ready || recording || !channelIdle) return
        try { radio.startCapture(checkNotNull(radioConnection)) }
        catch (_: Exception) { update("Another pendant operation owns the connection."); return }
        connectionPriority(true)
        recording = true; ready = false; progress = 0; cancellationRequested = false; cancelSent = false
        clipId = 0; lastClipState = ClipState.WAITING
        recordDeadlineMs = SystemClock.elapsedRealtime() + 45000
        clipDeadline = Runnable { fail("Recording/transfer deadline reached. Partial audio discarded.") }
        main.postDelayed(clipDeadline!!, 45000)
        try {
            request(OpProtocol.BEGIN) { body ->
                clipId = OpProtocol.begin(body)
                if (cancellationRequested) sendCancel()
                else {
                    update("Now tap and RELEASE the pendant button briefly (under 1 second). You have 15 seconds. Do not hold it.")
                    pollStatus()
                }
            }
        } catch (error: Exception) { fail(if (error is ProtocolException) error.message ?: "Recording request failed" else "Recording request failed") }
    }

    private fun pollStatus() {
        if (!recording || cancellationRequested) { if (cancellationRequested) sendCancel(); return }
        ensure(SystemClock.elapsedRealtime() < recordDeadlineMs, "Recording transfer expired")
        request(OpProtocol.STATUS, OpProtocol.idPayload(clipId)) { body ->
            val status = ClipStatus.parse(body, clipId)
            when (status.state) {
                ClipState.WAITING -> {
                    ensure(lastClipState == ClipState.WAITING, "Recording state regressed")
                    schedulePoll()
                }
                ClipState.RECORDING -> {
                    ensure(lastClipState in listOf(ClipState.WAITING, ClipState.RECORDING), "Recording state regressed")
                    lastClipState = ClipState.RECORDING
                    update("Recording · red pendant light · keep speaking for two seconds.")
                    schedulePoll()
                }
                ClipState.READY -> {
                    assembler = ClipAssembler(status)
                    lastClipState = ClipState.READY
                    // Device owns the authoritative 30 s expiry from capture start.
                    // Polling is foreground/150 ms; cap transfer to another 25 s too.
                    recordDeadlineMs = minOf(recordDeadlineMs, SystemClock.elapsedRealtime() + 25000)
                    clipDeadline?.let(main::removeCallbacks)
                    clipDeadline = Runnable { fail("Bluetooth clip transfer expired. Partial audio discarded.") }
                    main.postDelayed(clipDeadline!!, maxOf(1, recordDeadlineMs - SystemClock.elapsedRealtime()))
                    update("Microphone off. Receiving and verifying the local clip…")
                    pullChunk()
                }
                ClipState.EXPIRED -> fail("Button confirmation or clip retention expired. Tap Record to start a new request.")
                ClipState.CANCELLED -> finishCancellation()
                ClipState.ERROR -> fail("Pendant could not complete recording. No clip was saved.")
                else -> throw ProtocolException("Unexpected clip state before transfer")
            }
        }
    }

    private fun schedulePoll() {
        poll = Runnable {
            try { pollStatus() } catch (error: Exception) { fail(if (error is ProtocolException) error.message ?: "Clip status failed" else "Clip status failed") }
        }
        main.postDelayed(poll!!, 150)
    }

    private fun pullChunk() {
        ensure(SystemClock.elapsedRealtime() < recordDeadlineMs, "Clip transfer deadline reached")
        val transfer = assembler ?: throw ProtocolException("No active clip transfer")
        if (transfer.received == transfer.metadata.total) {
            request(OpProtocol.STATUS, OpProtocol.idPayload(clipId)) { body ->
                val status = ClipStatus.parse(body, clipId)
                val pcm = transfer.finish(status)
                assembler = null
                finishClipState()
                try {
                    listener.recordingComplete(pcm)
                    update("Clip verified and saved on this phone. Pendant microphone off; retained clip store drained. Tap Play when ready.")
                } finally { pcm.fill(0) }
            }
        } else {
            request(OpProtocol.CHUNK, OpProtocol.chunkPayload(clipId, transfer.received)) { body ->
                transfer.accept(body)
                val percentage = transfer.received * 100 / transfer.metadata.total
                if (progress != percentage) { progress = percentage; listener.changed() }
                pullChunk()
            }
        }
    }

    fun cancel() {
        if (!recording) { disconnect("Bluetooth disconnected. A pendant long recording continues until explicitly stopped."); return }
        cancellationRequested = true
        poll?.let(main::removeCallbacks); poll = null
        assembler?.discard(); assembler = null
        update("Cancelling. No partial audio will be kept.")
        if (transaction == null) try { sendCancel() } catch (_: Exception) { disconnect("Disconnected; partial audio discarded.") }
    }

    private fun sendCancel() {
        if (clipId == 0L) { disconnect("Disconnected before a recording identifier was received."); return }
        ensure(!cancelSent, "Cancellation has already been sent")
        cancelSent = true
        clipDeadline?.let(main::removeCallbacks)
        clipDeadline = Runnable { disconnect("Cancellation cleanup timed out. Bluetooth closed; partial audio discarded.") }
        main.postDelayed(clipDeadline!!, 4000)
        request(OpProtocol.CANCEL, OpProtocol.idPayload(clipId)) { body ->
            when (OpProtocol.cancellation(body, clipId)) {
                ClipState.CANCELLED -> finishCancellation()
                ClipState.WAITING, ClipState.RECORDING -> scheduleCancelPoll()
                else -> disconnect("Clip already stopped or expired. Partial audio discarded; reconnect manually.")
            }
        }
    }

    private fun scheduleCancelPoll() {
        poll = Runnable {
            try {
                request(OpProtocol.STATUS, OpProtocol.idPayload(clipId)) { body ->
                    val status = ClipStatus.parse(body, clipId)
                    when (status.state) {
                        ClipState.CANCELLED -> finishCancellation()
                        ClipState.WAITING, ClipState.RECORDING -> scheduleCancelPoll()
                        else -> disconnect("Recording stopped or expired. Partial audio discarded; reconnect manually.")
                    }
                }
            } catch (_: Exception) { disconnect("Could not confirm cancellation. Bluetooth closed; partial audio discarded.") }
        }
        main.postDelayed(poll!!, 150)
    }

    private fun finishCancellation() { finishClipState(); update("Cancelled. Partial audio discarded; no playback started.") }
    private fun finishClipState(releaseRadio: Boolean = true) {
        if (recording && releaseRadio) radio.finishCapture(checkNotNull(radioConnection))
        assembler?.discard(); assembler = null
        clipDeadline?.let(main::removeCallbacks); clipDeadline = null
        poll?.let(main::removeCallbacks); poll = null
        clipId = 0; recording = false; cancellationRequested = false; cancelSent = false; ready = connected
        connectionPriority(false)
    }

    private fun connectionPriority(high: Boolean) {
        // A best-effort link-parameter hint, not a GATT transaction. Android and
        // the peer choose the actual interval; this app never claims to force it.
        if (connected && hasConnectPermission()) try {
            gatt?.requestConnectionPriority(if (high) BluetoothGatt.CONNECTION_PRIORITY_HIGH
                else BluetoothGatt.CONNECTION_PRIORITY_BALANCED)
        } catch (_: Exception) { }
    }

    private fun preferFastPhy() {
        // Once per already-admitted durable lease. This is a preference, not a
        // requirement; keep the negotiated fallback if either side rejects it.
        // Do not wait, retry, touch GATT transaction state or renew deadlines.
        if (connected && hasConnectPermission()) try {
            if (adapter?.isLe2MPhySupported == true)
                gatt?.setPreferredPhy(BluetoothDevice.PHY_LE_2M_MASK,
                    BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED)
        } catch (_: Exception) { }
    }

    private fun armTimeout(milliseconds: Long, text: String) {
        cancelTimeout(); timeout = Runnable { fail(text) }; main.postDelayed(timeout!!, milliseconds)
    }
    private fun cancelTimeout() { timeout?.let(main::removeCallbacks); timeout = null }
    private fun fail(text: String) { disconnect(text) }

    fun disconnect(reason: String = "Disconnected. Tap Connect when ready.") {
        disconnectInternal(reason, recover=false)
    }
    private fun disconnectInternal(reason: String, recover: Boolean, planned: Boolean = true) {
        if(!recover) recovery.stop()
        preferencesSave.disconnected()
        // Revoke the source AND token before any external platform/listener
        // callback. A later queued callback from this GATT is permanently inert.
        val previous = gatt; gatt = null
        val balanceDurable = durablePriority.clear()
        negotiated = null; observedPhy = null; devicePreferences = null
        val epoch = radioConnection; radioConnection = null
        val closedProof = epoch?.let(radio::revoke)
        val failedRequest = transaction?.failure
        telemetryPoll?.let(main::removeCallbacks); telemetryPoll = null
        stopScan(); cancelTimeout(); finishClipState(releaseRadio = false)
        transaction?.gate?.discard(); transaction = null; operation = null
        ready = false; connected = false; progress = 0
        mtuNegotiation = MtuNegotiation()
        writeCharacteristic = null; notifyCharacteristic = null; ccc = null
        var localCloseConfirmed = false
        if (previous != null) {
            // Source/epoch already revoked. Best effort only; failure does not
            // skip disconnect/close, renew a deadline or release a radio fence.
            if (balanceDurable && hasConnectPermission()) try {
                previous.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_BALANCED)
            } catch (_: Exception) { }
            try { if (hasConnectPermission()) previous.disconnect() } catch (_: Exception) { }
            try {
                previous.close()
                if (closedProof != null) radio.closed(closedProof)
                localCloseConfirmed = true
            } catch (_: Exception) { /* No close proof: the radio gate stays fenced. */ }
        }
        val text = if (closedProof != null && !localCloseConfirmed)
            "$reason Local Bluetooth cleanup is unconfirmed; this app session is locked."
            else reason
        if(recover && localCloseConfirmed) recovery.closed(selected?.address,SystemClock.elapsedRealtime(),planned)
        else if(closedProof != null && !localCloseConfirmed) recovery.stop()
        try { update(text) } finally { failedRequest?.let(::notifyTransportFailure) }
    }

    private fun notifyTransportFailure(callback: () -> Unit) {
        try { callback() } catch (_: Throwable) { /* The revoked client stays closed/fenced. */ }
    }

    fun background() {
        // Called twice during navigation/rotation: don't erase an existing
        // pending foreground recovery when there is no GATT left to close.
        if(gatt!=null) disconnectInternal("App paused. Recording on the pendant is unchanged.", recover=true)
    }
    fun close() {
        if (!alive) return
        disconnect("Closed."); alive = false
        pairingDeadline?.let(main::removeCallbacks); pairingDeadline = null
        try { context.unregisterReceiver(bondReceiver) } catch (_: Exception) { }
    }
}
