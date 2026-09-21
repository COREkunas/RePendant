package org.openpendant.app

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.*
import android.content.pm.PackageManager
import android.hardware.usb.*
import android.os.*
import android.view.View
import android.view.WindowManager
import android.widget.*
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** Explicit replacement-phone setup. USB is identity-only; private recovery
 * material is imported locally into a separate preserved recipient profile.
 * No audio, reset, pairing changes, formatting or deletion commands exist here. */
class PhoneMigrationActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val cancelled = AtomicBoolean()
    private lateinit var session: PendantSession
    private lateinit var manager: UsbManager
    private lateinit var status: TextView
    private lateinit var read: Button
    private lateinit var restore: Button
    private lateinit var activate: Button
    private lateinit var choices: RadioGroup
    private var choice = PhoneMigrationChoice.KEEP_KEY
    private var binding: DurablePublicBinding? = null
    private var keyReady = false
    private var busy = false
    private var picker = false
    private var claimed = false
    private var registered = false
    private var permissionDevice: UsbDevice? = null
    private var awaitingPermission = false
    @Volatile private var console: UsbPairingConsole? = null
    private val permissionAction get() = "$packageName.MIGRATION_USB_PERMISSION"
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != permissionAction || !awaitingPermission || cancelled.get()) return
            awaitingPermission = false
            val expected = permissionDevice
            val live = expected?.let { selected -> manager.deviceList.values.singleOrNull { it.deviceId == selected.deviceId && it.deviceName == selected.deviceName } }
            if (live == null || !manager.hasPermission(live)) {
                status.text = "USB access was not granted. Nothing changed."; render()
            } else readIdentity()
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
        fun label(text: String, size: Float = 16f) = TextView(this).also {
            it.text = text; it.textSize = size; it.setPadding(0, 12, 0, 12); column.addView(it)
        }
        label("Move pendant to this phone", 25f)
        label("Connect the pendant directly to this phone with a USB data cable. Pair using USB first if needed. Choose what happens to existing recordings.")
        choices = RadioGroup(this)
        PhoneMigrationChoice.entries.forEach { item ->
            choices.addView(RadioButton(this).apply {
                id = View.generateViewId(); tag = item; text = item.title; textSize = 16f
                contentDescription = "${item.title}. ${item.explanation}"
            })
        }
        choices.check(choices.getChildAt(0).id); column.addView(choices)
        val explanation = label(choice.explanation)
        choices.setOnCheckedChangeListener { _, id ->
            choice = choices.findViewById<RadioButton>(id)?.tag as? PhoneMigrationChoice ?: return@setOnCheckedChangeListener
            explanation.text = choice.explanation
            status.text = if (choice == PhoneMigrationChoice.KEEP_KEY)
                "Read the wired pendant, then choose its old recovery backup. Your other key is kept separately."
            else if(choice == PhoneMigrationChoice.NEW_KEY_ERASE) "Open the new-key reset wizard. It checks the new backup before a separate pendant-only deletion confirmation."
            else PhoneMigrationPolicy.rotationRefusal(choice, false, false, null, "", false, false)
            render()
        }
        status = label("Nothing changes until you explicitly restore a backup and finish setup.")
        fun button(text: String, action: () -> Unit) = Button(this).also {
            it.text = text; it.isAllCaps = false; it.setOnClickListener { action() }; column.addView(it)
        }
        read = button("1 · Read wired pendant") {
            if(choice == PhoneMigrationChoice.NEW_KEY_ERASE){
                if(claimed){session.client.endUsbPairing();claimed=false}
                startActivity(Intent(this,KeyResetActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_FORWARD_RESULT));finish()
            } else prepare()
        }
        restore = button("2 · Choose old recovery backup…") {
            AlertDialog.Builder(this).setTitle("Restore the pendant's existing key?")
                .setMessage("Choose the old private backup locally. The app will verify that it matches this pendant and protect it in a separate profile. Your newly created key and all recordings are kept. Nothing is uploaded.")
                .setNegativeButton("Cancel", null).setPositiveButton("Choose local backup") { _, _ -> chooseBackup() }.show()
        }
        activate = button("3 · Finish recording setup") { finishSetup() }
        button("Back") { finish() }
        setContentView(ScrollView(this).apply { isFillViewport = true; fitsSystemWindows = true; addView(column) })
        claimed = !session.library.busy && !session.longRecording.busy && !session.longRecording.ownsRadio && session.client.beginUsbPairing()
        if (!claimed) status.text = "Finish recording or transfer and disconnect Bluetooth before opening setup again."
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, IntentFilter(permissionAction), RECEIVER_NOT_EXPORTED)
        else @Suppress("DEPRECATION") registerReceiver(receiver, IntentFilter(permissionAction))
        registered = true
        render()
    }
    private fun render() {
        val available = claimed && !busy && !picker && !awaitingPermission && !cancelled.get()
        for (i in 0 until choices.childCount) choices.getChildAt(i).isEnabled = available
        val keep = choice == PhoneMigrationChoice.KEEP_KEY
        read.text = if(choice == PhoneMigrationChoice.NEW_KEY_ERASE) "Open new-key reset…" else "1 · Read wired pendant"
        read.isEnabled = available && choice != PhoneMigrationChoice.NEW_KEY_ARCHIVE
        restore.isEnabled = available && keep && binding != null
        activate.isEnabled = available && keep && binding != null && keyReady
    }
    private fun prepare() {
        if (busy || picker || awaitingPermission || !claimed || choice != PhoneMigrationChoice.KEEP_KEY) return
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_CONNECT), 1); return
        }
        val target = manager.deviceList.values.filter { it.vendorId == UsbPairingProtocol.VID && it.productId == UsbPairingProtocol.PID }.singleOrNull()
        if (target == null) { status.text = "Connect exactly one pendant directly to this phone using a USB data cable."; return }
        permissionDevice = target
        if (manager.hasPermission(target)) readIdentity()
        else {
            awaitingPermission = true; render()
            val pending = PendingIntent.getBroadcast(this, 62, Intent(permissionAction).setPackage(packageName),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_CANCEL_CURRENT)
            try { manager.requestPermission(target, pending) }
            catch (_: Exception) { awaitingPermission = false; status.text = "USB permission could not be requested."; render() }
            main.postDelayed({ if (awaitingPermission) { awaitingPermission = false; status.text = "USB permission timed out. Nothing changed."; render() } }, 120000)
        }
    }
    override fun onRequestPermissionsResult(code: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, permissions, results)
        if (code == 1 && results.isNotEmpty() && results.all { it == PackageManager.PERMISSION_GRANTED }) prepare()
        else { status.text = "Nearby-device access is needed to verify the saved Bluetooth pairing."; render() }
    }
    private fun checkActive() { check(!cancelled.get()) }
    @SuppressLint("MissingPermission")
    private fun wiredBinding(): DurablePublicBinding {
        checkActive()
        val expected = checkNotNull(permissionDevice)
        val device = manager.deviceList.values.single { it.deviceId == expected.deviceId && it.deviceName == expected.deviceName }
        return UsbPairingConsole(manager, device).use { channel ->
            console = channel
            try {
                val identity = UsbPairingProtocol.identity(channel.request(UsbPairingProtocol.Command.IDENTITY))
                UsbPairingProtocol.status(channel.request(UsbPairingProtocol.Command.STATUS)).use {
                    check(it.bonds == 1 && !it.open && !it.pending && !it.codeReady)
                }
                check(getSystemService(BluetoothManager::class.java).adapter.bondedDevices.any {
                    it.address == identity.address && it.bondState == BluetoothDevice.BOND_BONDED
                })
                PhoneMigrationProtocol.binding(identity.address, channel.request(UsbPairingProtocol.Command.STORAGE_IDENTITY))
                    .also { checkActive() }
            } finally { console = null }
        }
    }
    private fun job(progress: String, action: () -> (() -> Unit)) {
        if (busy || cancelled.get() || !claimed) return
        busy = true; status.text = progress; render()
        worker.execute {
            val result: () -> Unit = try { checkActive(); action() } catch (_: Exception) {
                { status.text = "Setup could not be confirmed. Nothing on the pendant was changed. Check the cable, pairing and matching old backup. Existing keys were kept; no automatic retry." }
            }
            main.post { if (!cancelled.get()) { busy = false; result(); render() } }
        }
    }
    private fun readIdentity() = job("Checking the wired pendant's public storage identity…") {
        val observed = wiredBinding()
        val existing = AndroidDurableBinding.read(applicationContext)
        check(existing == null || existing == observed)
        val summary = AndroidRecipientProfiles.selected(applicationContext, observed.recipientFingerprint).summary()
        val ready = summary.state == RecipientVaultState.READY && summary.backupVerified && summary.fingerprintHex == observed.recipientFingerprint
        return@job { binding = observed; keyReady = ready; status.text = if (ready) "Matching verified key found. Finish setup to enable recording controls."
            else "Pendant identified. Choose its OLD recovery backup. The new phone key will be preserved separately." }
    }
    @Suppress("DEPRECATION")
    private fun chooseBackup() {
        if (busy || binding == null || cancelled.get() || picker) return
        picker = true; render()
        try { startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE); type = "*/*"; putExtra(Intent.EXTRA_LOCAL_ONLY, true)
        }, 2) } catch (_: Exception) { picker = false; status.text = "The local file picker is unavailable."; render() }
    }
    @Deprecated("Platform document picker")
    override fun onActivityResult(code: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(code, resultCode, data)
        if (code != 2) return
        picker = false
        val selected = binding; val uri = data?.data
        if (resultCode != RESULT_OK || selected == null || uri?.scheme != "content") { render(); return }
        job("Checking the old backup against this pendant…") {
            checkNotNull(contentResolver.openInputStream(uri)).use { input ->
                RecoveryDocumentIO.readBackup(input).use { backup ->
                    val bytes = backup.copyForExplicitExport()
                    try { checkActive(); AndroidRecipientProfiles.importMatching(applicationContext, selected, bytes) }
                    finally { bytes.fill(0) }
                }
            }
            return@job { keyReady = true; status.text = "Old key restored and verified. Your other key is kept. Finish setup with the pendant still connected by USB." }
        }
    }
    private fun finishSetup() {
        val selected = binding ?: return
        if (!keyReady) return
        job("Rechecking the cable identity and saving this phone's setup…") {
            check(wiredBinding() == selected); checkActive()
            AndroidDurableBinding.enrollFromUsbExplicit(applicationContext, selected)
            return@job { status.text = "Recording setup complete. Go Back, disconnect the USB cable from the phone and connect securely over Bluetooth. Nothing was recorded or erased."
                keyReady = false; binding = null; setResult(RESULT_OK, Intent().putExtra("pendantAddress", selected.bondAddress)) }
        }
    }
    override fun onDestroy() {
        cancelled.set(true); console?.close(); worker.shutdown(); main.removeCallbacksAndMessages(null)
        if (registered) unregisterReceiver(receiver)
        if (claimed) session.client.endUsbPairing()
        super.onDestroy()
    }
}
