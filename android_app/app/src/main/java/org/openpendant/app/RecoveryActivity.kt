package org.openpendant.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.graphics.drawable.StateListDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.concurrent.Executors

/** Explicit, local key setup only. No BLE, recording, audio or provisioning.
 * Private bytes never enter text, saved state, clipboard, logs or Intent extras.
 * Leaving during an accepted atomic operation lets it finish/reconcile; the
 * destroyed Activity never publishes its result. Picker grants are not retained.
 */
class RecoveryActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var vault: RecipientKeyVault
    private lateinit var stateLabel: TextView
    private lateinit var fingerprint: TextView
    private lateinit var notice: TextView
    private lateinit var createButton: Button
    private lateinit var exportButton: Button
    private lateinit var verifyButton: Button
    private lateinit var restoreButton: Button
    private lateinit var refreshButton: Button
    private var summary: RecipientVaultSummary? = null
    private var busy = false
    private var alive = true
    private var pendingPicker = 0
    private var pendingFingerprint: String? = null
    private val pageColor = Color.rgb(8, 10, 12)
    private val cardColor = Color.rgb(16, 19, 24)
    private val raisedColor = Color.rgb(23, 27, 33)
    private val borderColor = Color.rgb(39, 45, 53)
    private val textColor = Color.rgb(246, 247, 242)
    private val mutedColor = Color.rgb(142, 152, 166)
    private val accentColor = Color.rgb(217, 255, 83)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
        val monitor = AndroidRecipientVaultOwner.monitor
        vault = RecipientKeyVault(AndroidRecipientVaultStorage(applicationContext, monitor),
            AndroidRecipientVaultWrapper(monitor), JcaRecipientVaultGenerator(), monitor)
        pendingPicker = savedInstanceState?.getInt("recoveryPicker", 0)?.takeIf { it in EXPORT..RESTORE } ?: 0
        pendingFingerprint = savedInstanceState?.getString("recoveryFingerprint")?.takeIf { it.matches(Regex("[0-9a-f]{64}")) }
        buildUi()
        // A returning picker result must not race a background status refresh.
        if (pendingPicker == 0) refresh()
        else notice.setText(R.string.recovery_waiting_picker)
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private fun rounded(fill: Int, radius: Int, stroke: Int = borderColor) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(radius).toFloat()
        setStroke(dp(1).coerceAtLeast(1), stroke)
    }

    private fun card(parent: LinearLayout, raised: Boolean = false) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(18), dp(16), dp(18), dp(18))
        background = rounded(if (raised) raisedColor else cardColor, 22)
        parent.addView(this, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ).apply { bottomMargin = dp(16) })
    }

    private fun label(parent: LinearLayout, text: String, size: Float = 16f, bold: Boolean = false,
                      muted: Boolean = false): TextView =
        TextView(this).apply {
            this.text = text; textSize = size; setTextColor(if (muted) mutedColor else textColor)
            setPadding(0, dp(5), 0, dp(7))
            setLineSpacing(dp(2).toFloat(), 1.08f)
            if (bold) setTypeface(typeface, Typeface.BOLD)
            parent.addView(this, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }

    private fun eyebrow(parent: LinearLayout, text: String) = label(parent, text, 11f, true, true).apply {
        letterSpacing = .13f
    }

    private fun button(parent: LinearLayout, text: String, primary: Boolean = false,
                       action: () -> Unit) = Button(this).apply {
        this.text = text; isAllCaps = false; textSize = 15f; minHeight = dp(52)
        minimumHeight = dp(52)
        setTypeface(typeface, Typeface.BOLD)
        setPadding(dp(16), dp(12), dp(16), dp(12))
        backgroundTintList = null
        val states = StateListDrawable().apply {
            addState(intArrayOf(-android.R.attr.state_enabled), rounded(raisedColor, 14))
            addState(intArrayOf(android.R.attr.state_pressed),
                rounded(if (primary) Color.rgb(197, 235, 63) else Color.rgb(35, 42, 50), 14,
                    if (primary) accentColor else mutedColor))
            addState(intArrayOf(android.R.attr.state_focused),
                rounded(if (primary) accentColor else raisedColor, 14, accentColor))
            addState(intArrayOf(), rounded(if (primary) accentColor else raisedColor, 14,
                if (primary) accentColor else borderColor))
        }
        background = RippleDrawable(ColorStateList.valueOf(
            if (primary) Color.argb(32, 8, 10, 12) else Color.argb(28, 246, 247, 242)
        ), states, rounded(Color.WHITE, 14, Color.WHITE))
        setTextColor(ColorStateList(arrayOf(intArrayOf(-android.R.attr.state_enabled), intArrayOf()),
            intArrayOf(mutedColor, if (primary) pageColor else textColor)))
        setOnClickListener { if (!busy && pendingPicker == 0) action() }
        parent.addView(this, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = dp(8) })
    }

    private fun buildUi() {
        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(18), dp(18), dp(28))
            setBackgroundColor(pageColor)
        }
        eyebrow(body, "PRIVACY & RECOVERY")
        label(body, "Recording security", 28f, true).apply {
            setPadding(0, dp(2), 0, dp(20))
        }
        val information = card(body)
        label(information, "Preparation only — pendant encryption is not enabled", 17f, true)
        label(information, "These controls prepare a key for future encrypted recordings. They do not change your pendant, record audio, or encrypt existing phone clips.", 14f, muted = true)
        val keyStatus = card(body, raised = true)
        eyebrow(keyStatus, "PROTECTED STORAGE")
        stateLabel = label(keyStatus, "Checking protected key storage…", 19f, true).apply {
            accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
        }
        fingerprint = label(keyStatus, "", 13f, muted = true).apply {
            typeface = Typeface.MONOSPACE
        }
        notice = label(keyStatus, "", 15f)
        refreshButton = button(keyStatus, "Check again") { refresh() }
        val setup = card(body)
        label(setup, "Set up recovery", 18f, true)
        label(setup, "Create a key, then save and verify its recovery backup.", 14f, muted = true)
        createButton = button(setup, "Create recording key", primary = true) {
            confirm("Create a recording key?", "A new key will be protected in this app using Android Keystore. " +
                "You must save and verify a separate recovery backup before relying on it. Nothing is sent to the pendant.", "Create key") {
                operation("Key created. Save its recovery backup, then verify the saved file.") { vault.create() }
            }
        }
        exportButton = button(setup, "Save recovery backup…") {
            confirm("Save a secret recovery key?", "Anyone with this file could decrypt recordings made for this key. " +
                "The exported file is NOT password-protected. Choose a private local folder, not a cloud provider or shared folder. " +
                "Keep a separate safe offline copy; losing both this phone and every backup means losing access.", "Choose local location") {
                launchPicker(EXPORT)
            }
        }
        verifyButton = button(setup, "Verify saved backup…") {
            confirm("Verify your backup", "Select the recovery file you saved. The app will read it back and check that it restores " +
                "this exact recording key. Nothing is uploaded or sent to the pendant.", "Choose backup") { launchPicker(VERIFY) }
        }
        val restore = card(body)
        label(restore, "Restore access", 18f, true)
        restoreButton = button(restore, "Restore from backup…") {
            confirm("Restore a recording key?", "Choose your private local recovery file. The imported key will be protected " +
                "on this phone. Existing encrypted key files are preserved; this does not erase recordings or change the pendant. " +
                "A different key cannot replace a readable active key. This screen does not support key rotation.", "Choose backup") { launchPicker(RESTORE) }
        }
        label(restore, "A verified backup was readable at the time of the check. It is not proof that an offline copy exists " +
            "or that your chosen storage location will keep it. The app does not automatically copy or upload keys.", 14f, muted = true)
        button(body, "Back to dashboard") { finish() }
        val scroll = ScrollView(this).apply {
            isFillViewport = true
            setBackgroundColor(pageColor)
            addView(body)
        }
        scroll.setOnApplyWindowInsetsListener { view, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                view.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                    insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        setContentView(scroll); scroll.requestApplyInsets(); render()
    }

    private fun confirm(title: String, message: String, positive: String, action: () -> Unit) {
        if (!alive || isFinishing || busy || pendingPicker != 0) return
        AlertDialog.Builder(this).setTitle(title).setMessage(message).setNegativeButton("Cancel", null)
            .setPositiveButton(positive) { _, _ -> if (alive && !busy && pendingPicker == 0) action() }.show()
    }

    private fun render() {
        val value = summary
        val state = value?.state
        val ready = state == RecipientVaultState.READY
        val empty = state == RecipientVaultState.EMPTY
        stateLabel.text = if (busy) "Checking protected key storage…" else when {
            empty -> "No recording key created"
            ready && value?.backupVerified == true -> "Key protected · backup verified"
            ready -> "Key protected · backup still needs verification"
            else -> "Recovery needs attention · existing data kept"
        }
        fingerprint.text = value?.fingerprintHex?.let { "Public key fingerprint\n${it.chunked(8).joinToString(" ")}" } ?: ""
        val enabled = !busy && pendingPicker == 0
        createButton.isEnabled = enabled && empty
        exportButton.isEnabled = enabled && ready
        verifyButton.isEnabled = enabled && ready
        restoreButton.isEnabled = enabled && value != null && !ready
        refreshButton.isEnabled = enabled
    }

    private fun refresh() = operation(null) { vault.summary() }

    private fun operation(message: String?, failureMessage: String? = null, action: () -> RecipientVaultSummary) {
        if (!alive || busy) return
        busy = true; render()
        worker.execute {
            var result: RecipientVaultSummary? = null
            var success = false
            var explanation: String? = null
            try { result = action(); success = true } catch (error: Exception) {
                explanation = when ((error as? RecipientVaultException)?.failure) {
                    RecipientVaultFailure.INVALID_BACKUP -> "That file is not a valid recovery backup, or it is damaged. The active key was not replaced."
                    RecipientVaultFailure.DIFFERENT_RECIPIENT -> "That backup belongs to a different recording key. The active key was not replaced."
                    RecipientVaultFailure.NOT_EMPTY -> "A recording key already exists. It was not replaced. Save and verify its backup instead."
                    RecipientVaultFailure.NO_ACTIVE_KEY -> "There is no readable active key to export or verify. Restore your backup or check key status."
                    else -> null
                }
                // A disk operation might have committed before reporting failure.
                // Re-read through the fail-closed service; never retry the write.
                try { result = vault.summary() } catch (_: Exception) { }
            }
            handler.post {
                if (!alive || isFinishing || isDestroyed) return@post
                busy = false; summary = result
                if (!success) notice.text = failureMessage ?: explanation ?: ("The operation could not be confirmed. No automatic retry or key replacement was attempted. " +
                    "Existing encrypted key files were kept. Check the status before trying again."
                    )
                else if (message != null) notice.text = message
                render()
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun launchPicker(action: Int) {
        if (!alive || busy || pendingPicker != 0) return
        pendingPicker = action
        pendingFingerprint = summary?.fingerprintHex
        try {
            startActivityForResult(Intent(if (action == EXPORT) Intent.ACTION_CREATE_DOCUMENT else Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = if (action == EXPORT) "application/octet-stream" else "*/*"
                putExtra(Intent.EXTRA_LOCAL_ONLY, true)
                if (action == EXPORT) putExtra(Intent.EXTRA_TITLE, "OpenPendant-recovery.opnd-key")
            }, action)
        } catch (_: Exception) {
            pendingPicker = 0; pendingFingerprint = null
            notice.setText(R.string.recovery_picker_unavailable)
        }
        render()
    }

    @Deprecated("Platform document picker; no additional dependency or broad storage permission")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != pendingPicker || requestCode !in EXPORT..RESTORE) return
        val expectedFingerprint = pendingFingerprint
        pendingPicker = 0; pendingFingerprint = null
        if (resultCode != RESULT_OK) { notice.setText(R.string.recovery_picker_cancelled); refresh(); return }
        val uri = data?.data
        if (uri == null || uri.scheme != "content") { notice.setText(R.string.recovery_picker_unsupported); render(); return }
        val success = when (requestCode) {
            EXPORT -> "Backup file written. Now tap Verify saved backup and choose that file. Verification is not automatic."
            VERIFY -> "Saved backup verified against this key. Keep a separate safe offline copy."
            else -> "Recovery key imported and checked. Pendant encryption is still not enabled."
        }
        val failure = if (requestCode == EXPORT) "The export could not be confirmed. The selected file may be incomplete or may contain " +
            "the secret key: keep it private. This export did not mark a backup as verified. Check key status before trying again." else null
        operation(success, failure) {
            if (requestCode == EXPORT) {
                // Bind the user's original selection and take the owned export
                // under ONE lock, but never hold it while a document provider runs.
                synchronized(AndroidRecipientVaultOwner.monitor) {
                    requireCurrentFingerprint(expectedFingerprint)
                    vault.export()
                }.use { backup ->
                    // Only the URI returned from the explicit create-document action.
                    contentResolver.openOutputStream(uri, "wt")?.use { RecoveryDocumentIO.writeBackup(it, backup) }
                        ?: throw RecoveryDocumentException()
                }
                vault.summary()
            } else {
                val input = contentResolver.openInputStream(uri) ?: throw RecoveryDocumentException()
                input.use { RecoveryDocumentIO.readBackup(it) }.use { backup ->
                    val bytes = backup.copyForExplicitExport()
                    try {
                        if (requestCode == VERIFY) synchronized(AndroidRecipientVaultOwner.monitor) {
                            requireCurrentFingerprint(expectedFingerprint)
                            vault.verifyBackup(bytes)
                        } else vault.restore(bytes)
                    }
                    finally { bytes.fill(0) }
                }
            }
        }
    }

    private fun requireCurrentFingerprint(expected: String?) {
        if (expected == null || vault.summary().fingerprintHex != expected) throw RecoveryDocumentException()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putInt("recoveryPicker", pendingPicker)
        outState.putString("recoveryFingerprint", pendingFingerprint) // Public metadata only.
        super.onSaveInstanceState(outState)
    }

    override fun onDestroy() {
        alive = false
        worker.shutdown() // Do not interrupt a committed key write or auto-retry it.
        super.onDestroy()
    }

    companion object { private const val EXPORT = 201; private const val VERIFY = 202; private const val RESTORE = 203 }
}
