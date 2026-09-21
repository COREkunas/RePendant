package org.openpendant.app

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.PendingIntent
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.*
import android.content.*
import android.content.pm.PackageManager
import android.hardware.usb.*
import android.os.*
import android.view.WindowManager
import android.widget.*
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/** Foreground, explicitly consented USB bootstrap. Native SMP still owns every
 * long-term key. No hidden Android APIs, silent unpair, key backup, or logging. */
@SuppressLint("MissingPermission") // Each entry checks runtime permissions; revocation fails closed.
class UsbPairingActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val cancelled = AtomicBoolean(false)
    private val running = AtomicBoolean(false)
    private val pin = AtomicReference<ByteArray?>(null)
    private val codeSubmitted = AtomicBoolean(false)
    @Volatile private var console: UsbPairingConsole? = null
    @Volatile private var attempt: UsbPairingAttempt? = null
    @Volatile private var peer: BluetoothDevice? = null
    private lateinit var status: TextView
    private lateinit var start: Button
    private lateinit var session: PendantSession
    private lateinit var manager: UsbManager
    private var claimed = false
    private var registered = false
    private var permissionDevice: UsbDevice? = null
    private var awaitingPermission = false
    private var terminal = false
    private val usbPermission get() = "$packageName.USB_PAIR_PERMISSION"
    private val adapter get() = getSystemService(BluetoothManager::class.java)?.adapter

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (terminal || cancelled.get()) return
            when (intent.action) {
                usbPermission -> {
                    if (!awaitingPermission) return
                    awaitingPermission = false
                    // Never trust supplied permission/device extras. Look up
                    // exactly the user-selected live USB device and OS grant.
                    val expected = permissionDevice ?: return
                    val live = manager.deviceList.values.singleOrNull { it.deviceId == expected.deviceId && it.deviceName == expected.deviceName }
                    if (live != null && manager.hasPermission(live)) launch(live)
                    else finishAttempt("USB access was not granted. Nothing was paired.")
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val expected = permissionDevice ?: return
                    if (manager.deviceList.values.none { it.deviceId == expected.deviceId }) {
                        cancelled.set(true); attempt?.cancel(); console?.close()
                        finishAttempt("USB disconnected. Pairing was not confirmed. Check Bluetooth status before trying again.")
                    }
                }
                BluetoothDevice.ACTION_PAIRING_REQUEST -> {
                    @Suppress("DEPRECATION")
                    val device = intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE) ?: return
                    val current = attempt ?: return
                    if (device.address != current.address) return
                    val variant = intent.getIntExtra(BluetoothDevice.EXTRA_PAIRING_VARIANT, -1)
                    val accepted = try {
                        current.pairingRequest(device.address, variant,
                            device.bondState == BluetoothDevice.BOND_BONDING, SystemClock.elapsedRealtime())
                    } catch (_: Exception) { false }
                    if (accepted) submitCode()
                    // Do not abort the system broadcast or auto-confirm a
                    // comparison/consent request. OEM dialogs remain visible.
                }
            }
        }
    }

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
        session = PendantSession.get(applicationContext)
        manager = getSystemService(UsbManager::class.java)
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val p = (24 * resources.displayMetrics.density).toInt(); setPadding(p, p, p, p)
        }
        column.addView(TextView(this).apply { setText(R.string.usb_pair_title); textSize = 25f })
        column.addView(TextView(this).apply {
            setText(R.string.usb_pair_instructions)
            textSize = 16f; setPadding(0, 24, 0, 24)
        })
        status = TextView(this).apply { textSize = 16f; setText(R.string.usb_pair_ready) }
        column.addView(status)
        start = Button(this).apply { setText(R.string.usb_pair_start); setOnClickListener { prepare() } }
        column.addView(start)
        column.addView(Button(this).apply { setText(R.string.usb_pair_back); setOnClickListener { finish() } })
        setContentView(ScrollView(this).apply { isFillViewport = true; fitsSystemWindows = true; addView(column) })
        claimed = !session.library.busy && !session.longRecording.busy && !session.longRecording.ownsRadio && session.client.beginUsbPairing()
        if (!claimed) { finishAttempt("Disconnect Bluetooth and finish any recording or transfer before USB pairing."); return }
        val filter = IntentFilter().apply {
            addAction(usbPermission); addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(BluetoothDevice.ACTION_PAIRING_REQUEST)
        }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, RECEIVER_EXPORTED)
        else @Suppress("DEPRECATION") registerReceiver(receiver, filter)
        registered = true
    }

    private fun permissions() = if (Build.VERSION.SDK_INT >= 31)
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    private fun prepare() {
        if (terminal || running.get() || awaitingPermission || !claimed) return
        val missing = permissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) { requestPermissions(missing.toTypedArray(), 201); return }
        if (adapter?.isEnabled != true) {
            status.setText(R.string.usb_pair_enable_bluetooth)
            try { startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) } catch (_: Exception) { }
            return
        }
        val devices = manager.deviceList.values.filter {
            it.vendorId == UsbPairingProtocol.VID && it.productId == UsbPairingProtocol.PID
        }
        if (devices.size != 1) {
            status.setText(R.string.usb_pair_connect_one)
            return
        }
        permissionDevice = devices.single()
        if (manager.hasPermission(devices.single())) launch(devices.single())
        else {
            awaitingPermission = true; start.isEnabled = false
            status.setText(R.string.usb_pair_permission)
            val permission = PendingIntent.getBroadcast(this, 61,
                Intent(usbPermission).setPackage(packageName), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_CANCEL_CURRENT)
            try { manager.requestPermission(devices.single(), permission) }
            catch (_: Exception) { finishAttempt("Android could not request USB access.") }
            main.postDelayed({ if (awaitingPermission) finishAttempt("USB permission timed out. Nothing was paired.") }, 120000)
        }
    }
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, results)
        if (requestCode == 201 && results.isNotEmpty() && results.all { it == PackageManager.PERMISSION_GRANTED }) prepare()
        else finishAttempt("Nearby-device permission is required for secure Bluetooth pairing.")
    }
    private class SetupFailure(val explanation: String) : Exception()
    private fun ensureActive() { if (cancelled.get()) throw SetupFailure("Pairing cancelled; no automatic retry.") }
    private fun progress(message: String) { main.post { if (!terminal && !isFinishing) status.text = message } }

    private fun launch(device: UsbDevice) {
        if (!running.compareAndSet(false, true) || terminal) return
        start.isEnabled = false
        worker.execute {
            var ownsWindow = false
            var resultAddress: String? = null
            var resultText = "Pairing was not confirmed. Check the cable, pendant firmware and Android Bluetooth status before retrying."
            try {
                ensureActive(); progress("Identifying the wired pendant…")
                val port = UsbPairingConsole(manager, device)
                console = port; ensureActive()
                val identity = UsbPairingProtocol.identity(port.request(UsbPairingProtocol.Command.IDENTITY))
                UsbPairingProtocol.status(port.request(UsbPairingProtocol.Command.STATUS)).use { state ->
                    if (state.bonds == 1) {
                        val known = adapter?.bondedDevices?.any { it.address == identity.address } == true
                        if (known) { resultAddress = identity.address; throw SetupFailure("Both devices have saved pairing information. Unplug USB and tap Connect to verify it. If connection fails, Forget the old Android pairing and use five quick pendant taps before trying USB setup again.") }
                        throw SetupFailure("The pendant is paired with another phone. While idle, press its button five times quickly to replace that pairing, then return here and try again. Recordings and recovery keys are kept.")
                    }
                    if (state.pending || state.codeReady) throw SetupFailure("Another pairing attempt is active. Let its 60-second window finish before trying again.")
                }
                if (adapter?.bondedDevices?.any { it.address == identity.address } == true)
                    throw SetupFailure("Android still remembers an old pairing. Forget this pendant in Android Bluetooth settings, then try USB pairing again.")
                progress("Finding this pendant over Bluetooth…")
                val devicePeer = findPeer(identity.address)
                peer = devicePeer; ensureActive()
                UsbPairingProtocol.status(port.request(UsbPairingProtocol.Command.STATUS)).use { state ->
                    if (state.bonds != 0 || state.pending || state.codeReady)
                        throw SetupFailure("Pairing state changed. No credentials were sent; retry after the window closes.")
                    if (!state.open) {
                        if (!UsbPairingProtocol.opened(port.request(UsbPairingProtocol.Command.OPEN)))
                            throw SetupFailure("The pendant could not open pairing. Leave it idle and retry.")
                    }
                    ownsWindow = true
                }
                val remaining = UsbPairingProtocol.status(port.request(UsbPairingProtocol.Command.STATUS)).use { state ->
                    if (!state.open || state.remaining < 15000 || state.pending || state.bonds != 0)
                        throw SetupFailure("Too little time remains. Wait for the blue light to stop, then try again.")
                    state.remaining
                }
                val deadline = SystemClock.elapsedRealtime() + remaining - 500
                attempt = UsbPairingAttempt(identity.address, deadline)
                progress("Pairing securely… Allow Android’s prompt if shown. No code needs to be typed.")
                ensureActive()
                if (devicePeer.bondState != BluetoothDevice.BOND_NONE || !devicePeer.createBond())
                    throw SetupFailure("Android could not start a fresh pairing. Check its Bluetooth settings before retrying.")
                while (SystemClock.elapsedRealtime() < deadline) {
                    ensureActive()
                    UsbPairingProtocol.status(port.request(UsbPairingProtocol.Command.STATUS)).use { state ->
                        if (UsbPairingProtocol.complete(devicePeer.bondState == BluetoothDevice.BOND_BONDED, codeSubmitted.get(), state)) {
                            ownsWindow = false; resultAddress = identity.address
                        } else if (!state.open && state.bonds == 0) {
                            throw SetupFailure("Pairing ended without a bond. No automatic retry or pairing replacement was performed.")
                        } else if (state.codeReady && !codeSubmitted.get() && pin.get() == null) {
                            val code = state.takeCode()
                            if (!pin.compareAndSet(null, code)) code?.fill(0)
                            main.post { submitCode() }
                        }
                    }
                    if (resultAddress != null) break
                    Thread.sleep(200)
                }
                if (resultAddress == null) throw SetupFailure("Pairing was not confirmed within 60 seconds. Android may not support automatic code entry on this phone. No security downgrade or automatic retry was made.")
                resultText = "Bluetooth paired securely. On a new phone, go Back → Settings → Move pendant / change recording key to complete recording setup using your old backup. Keep the cable connected for setup."
            } catch (e: SetupFailure) { resultText = e.explanation }
            catch (_: Exception) { /* Never expose exception text or a serial reply containing a passkey. */ }
            finally {
                attempt?.cancel(); pin.getAndSet(null)?.fill(0)
                if (ownsWindow) try { console?.request(UsbPairingProtocol.Command.CLOSE) } catch (_: Exception) { }
                console?.close(); console = null
                main.post { if (!terminal && !isFinishing) finishAttempt(resultText, resultAddress) }
            }
        }
    }
    private fun findPeer(address: String): BluetoothDevice {
        val scanner = adapter?.bluetoothLeScanner ?: throw SetupFailure("Bluetooth scanning is unavailable.")
        val found = AtomicReference<BluetoothDevice?>()
        val done = CountDownLatch(1)
        val callback = object : ScanCallback() {
            override fun onScanResult(type: Int, result: ScanResult) {
                if (result.device.address == address) { found.compareAndSet(null, result.device); done.countDown() }
            }
            override fun onScanFailed(errorCode: Int) { done.countDown() }
        }
        try {
            // Exact address sourced over cable; names and other advertisements
            // cannot select a different pendant. Scan caches LE transport type.
            scanner.startScan(listOf(ScanFilter.Builder().setDeviceAddress(address).build()),
                ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), callback)
            val deadline = SystemClock.elapsedRealtime() + 10000
            while (!done.await(200, TimeUnit.MILLISECONDS) && SystemClock.elapsedRealtime() < deadline) ensureActive()
            ensureActive()
            return found.get() ?: throw SetupFailure("The wired pendant was not found over Bluetooth. Disconnect other phone connections and try again.")
        } finally { try { scanner.stopScan(callback) } catch (_: Exception) { } }
    }
    private fun submitCode() {
        if (terminal || cancelled.get() || codeSubmitted.get()) return
        val current = attempt ?: return
        val device = peer ?: return
        val code = pin.get() ?: return
        try {
            if (!current.claimCode(device.address, device.bondState == BluetoothDevice.BOND_BONDING, SystemClock.elapsedRealtime())) return
            if (!pin.compareAndSet(code, null)) return
            // Public Android API. AOSP btif_dm_pin_reply converts exactly six
            // ASCII digits to the native LE SMP passkey for an LE-only device.
            // Do not use hidden setPasskey/setPairingConfirmation or reflection.
            if (!device.setPin(code)) throw SetupFailure("This phone did not accept automatic code entry. Pairing was not confirmed.")
            codeSubmitted.set(true)
        } catch (_: Exception) {
            cancelled.set(true)
            finishAttempt("Android refused automatic code entry. Pairing was not confirmed; no weaker pairing method was used.")
        } finally { if (pin.get() !== code) code.fill(0) }
    }
    private fun finishAttempt(message: String, address: String? = null) {
        if (terminal) return
        terminal = true; awaitingPermission = false
        cancelled.set(true); attempt?.cancel(); pin.getAndSet(null)?.fill(0)
        start.isEnabled = false; status.text = message
        if (address != null) setResult(RESULT_OK, Intent().putExtra("pendantAddress", address))
    }
    override fun onDestroy() {
        cancelled.set(true); attempt?.cancel(); pin.getAndSet(null)?.fill(0)
        main.removeCallbacksAndMessages(null)
        if (registered) try { unregisterReceiver(receiver) } catch (_: Exception) { }
        console?.close(); worker.shutdownNow()
        if (claimed) session.client.endUsbPairing()
        super.onDestroy()
    }
}
