package org.openpendant.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.graphics.drawable.StateListDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.widget.*
import java.text.DateFormat
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : Activity(), PendantClient.Listener {
    private lateinit var client: PendantClient
    private lateinit var pendantSession: PendantSession
    private lateinit var repository: RecordingRepository
    private lateinit var player: ClipPlayer
    private lateinit var models: ModelRepository
    private lateinit var transcripts: TranscriptionRepository
    private lateinit var transcription: TranscriptionController
    private lateinit var durableLibrary: AndroidDurableLibrary
    private lateinit var durableSection: DurableLibrarySection
    private lateinit var deviceSettings: DeviceSettingsSection
    private lateinit var storageSettings: StorageSettingsSection
    private lateinit var pendantStorageValue: TextView
    private lateinit var pendantCapacityValue: TextView
    private lateinit var pendantCapacityDetail: TextView
    private lateinit var pendantCapacityProgress: ProgressBar
    private lateinit var modelStatus: TextView
    private lateinit var importModel: Button
    private val mainHandler = Handler(Looper.getMainLooper())
    private val modelWorker = Executors.newSingleThreadExecutor()
    private val importCancelled = AtomicBoolean(false)
    private var importingModel = false
    private var transcriptState = TranscriptionState()
    private var modelMessage: String? = null
    private lateinit var status: TextView
    private lateinit var connectionValue: TextView
    private lateinit var captureValue: TextView
    private lateinit var recordingHint: TextView
    private lateinit var securityStatus: TextView
    private lateinit var pendantName: TextView
    private lateinit var quickConnect: Button
    private lateinit var deviceSummary: TextView
    private lateinit var modeValue: TextView
    private lateinit var syncSummary: TextView
    private lateinit var storageTechnical: TextView
    private lateinit var ledValue: TextView
    private lateinit var batteryValue: TextView
    private lateinit var telemetryDetail: TextView
    private lateinit var refreshDevice: Button
    private lateinit var phoneStorageValue: TextView
    private lateinit var librarySummary: TextView
    private lateinit var libraryEmptyState: TextView
    private lateinit var recordPrompt: TextView
    private lateinit var longStatus: TextView
    private lateinit var longStart: Button
    private lateinit var longArm: Button
    private lateinit var longStop: Button
    private lateinit var longCheck: Button
    private lateinit var scan: Button
    private lateinit var pair: Button
    private lateinit var connect: Button
    private lateinit var pairingHelpButton: Button
    private lateinit var record: Button
    private lateinit var cancel: Button
    private lateinit var dashboardTab: Button
    private lateinit var recordingsTab: Button
    private lateinit var settingsTab: Button
    private lateinit var dashboardPage: ScrollView
    private lateinit var recordingsPage: ScrollView
    private lateinit var settingsPage: ScrollView
    private lateinit var devices: LinearLayout
    private lateinit var recordingsList: LinearLayout
    private lateinit var progress: ProgressBar
    private lateinit var operationBar: LinearLayout
    private lateinit var operationLabel: TextView
    private lateinit var operationStop: Button
    private var uiReady = false
    private var foreground = false
    private val autoTransferCheck = Runnable {
        if (uiReady && foreground) {
            val now = android.os.SystemClock.elapsedRealtime()
            val peer = client.durablePeer()
            val prefs = durableLibrary.transferPreferences.read()
            val telemetry = client.telemetry
            val enrolled = peer?.takeIf { it.bondAddress == durableLibrary.state.binding?.bondAddress }
            durableLibrary.transferPolicy.observeLow(prefs, enrolled?.bondAddress,
                telemetry?.freshBattery(now, client.connected)?.percent)
            val device = telemetry?.takeIf { it.fresh(now, client.connected) }?.value
            val safe = !durableLibrary.busy && !client.recording && !transcriptState.busy && !importingModel &&
                playingRecordingId == null && !pendantSession.longRecording.busy && !pendantSession.longRecording.ownsRadio &&
                durableLibrary.syncRefusal(peer) == null && device != null && !device.microphonePower &&
                !device.resourceBusy && device.faults == 0L && device.recorder?.let {
                    !it.busy && !it.recording && !it.fault
                } == true
            if (durableLibrary.transferPolicy.claim(prefs, enrolled, safe, foreground, now)) durableLibrary.sync(enrolled!!)
        }
    }
    private var renderedDevices = ""
    private var renderedLibrary = ""
    private var localRecordings = emptyList<LocalRecording>()
    private var libraryError: String? = null
    private var playingRecordingId: String? = null
    private var activeTab = 0
    private var visibleRecordings = 30
    private var libraryQuery = ""
    private val transcriptCache = mutableMapOf<String, TranscriptDocument?>()
    private val transcriptReadErrors = mutableSetOf<String>()
    private val backgroundColor = Color.rgb(8, 10, 12)
    private val panel = Color.rgb(16, 19, 24)
    private val raisedPanel = Color.rgb(23, 27, 33)
    private val line = Color.rgb(39, 45, 53)
    private val ink = Color.rgb(246, 247, 242)
    private val green = Color.rgb(217, 255, 83)
    private val cyan = Color.rgb(112, 229, 255)
    private val coral = Color.rgb(255, 118, 87)
    private val muted = Color.rgb(142, 152, 166)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        repository = RecordingRepository(filesDir)
        val modelDir = File(filesDir, "models").apply { mkdirs() }
        val transcriptDir = File(filesDir, "transcripts").apply { mkdirs() }
        models = ModelRepository(modelDir, SpeechModel.spec)
        transcripts = TranscriptionRepository(transcriptDir)
        transcription = TranscriptionController(repository, transcripts, models, LocalTranscriptionEngine(),
            dispatch = { task -> mainHandler.post { task() } }, onState = { state ->
                if (state.phase == TranscriptionPhase.COMPLETE && state != transcriptState) {
                    state.recordingId?.let { id -> transcriptCache.remove(id); transcriptReadErrors.remove(id) }
                }
                transcriptState = state
                if (uiReady) changed()
            })
        pendantSession = PendantSession.get(applicationContext)
        client = pendantSession.client
        pendantSession.attach(this)
        player = ClipPlayer { playing, error ->
            if (!playing) playingRecordingId = null
            if (uiReady) { error?.let(::showError); changed() }
        }
        durableLibrary = pendantSession.library
        activeTab = savedInstanceState?.getInt("tab", 0)?.coerceIn(0, 2) ?: 0
        libraryQuery = savedInstanceState?.getString("libraryQuery")?.take(120) ?: ""
        refreshLibrary()
        buildUi()
        uiReady = true
        changed()
        showTab(activeTab)
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun column() = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
    private fun label(text: String, size: Float = 16f, color: Int = ink, bold: Boolean = false) = TextView(this).apply {
        this.text = text; textSize = size; setTextColor(color)
        typeface = Typeface.create("sans-serif", if (bold) Typeface.BOLD else Typeface.NORMAL)
        includeFontPadding = false
        setLineSpacing(dp(3).toFloat(), 1f)
        setPadding(0, dp(3), 0, dp(3))
    }
    private fun shape(fill: Int, radius: Int = 22, stroke: Int = line) = GradientDrawable().apply {
        setColor(fill); cornerRadius = dp(radius).toFloat(); setStroke(dp(1), stroke)
    }
    private fun button(text: String, primary: Boolean = false, action: () -> Unit) = Button(this).apply {
        this.text = text; isAllCaps = false; minHeight = dp(48); minimumHeight = dp(48); textSize = 14f
        minimumWidth = 0; minWidth = 0; includeFontPadding = false
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        setPadding(dp(12), dp(11), dp(12), dp(11))
        setTextColor(ColorStateList(arrayOf(intArrayOf(-android.R.attr.state_enabled), intArrayOf()),
            intArrayOf(muted, if (primary) backgroundColor else ink)))
        backgroundTintList = null
        background = StateListDrawable().apply {
            addState(intArrayOf(-android.R.attr.state_enabled), shape(panel, 15))
            addState(intArrayOf(), RippleDrawable(ColorStateList.valueOf(0x26ffffff),
                shape(if (primary) green else raisedPanel, 15, if (primary) green else line), null))
        }
        setOnClickListener { action() }
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(8)
        }
    }
    private fun card(parent: LinearLayout, radius: Int = 22): LinearLayout = column().apply {
        setPadding(dp(16), dp(16), dp(16), dp(16))
        background = shape(panel, radius)
        parent.addView(this, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(10)
        })
    }
    private fun eyebrow(text: String) = label(text.uppercase(Locale.getDefault()), 11f, muted, true).apply {
        letterSpacing = .11f
    }
    private fun section(parent: LinearLayout, title: String) {
        parent.addView(label(title, 17f, bold = true), LinearLayout.LayoutParams(-1, -2).apply {
            topMargin = dp(22); bottomMargin = dp(3)
        })
    }
    private fun metric(row: LinearLayout, title: String, value: String, detail: String, accent: Int = ink): TextView {
        val tile = column().apply { setPadding(dp(14), dp(15), dp(14), dp(15)); background = shape(panel, 20) }
        row.addView(tile, LinearLayout.LayoutParams(0, -1, 1f).apply { if (row.childCount > 0) marginStart = dp(10) })
        tile.addView(label(title, 12f, muted, true))
        val result = label(value, 18f, accent, true).apply { setPadding(0, dp(12), 0, dp(5)) }
        tile.addView(result)
        if (detail.isNotEmpty()) tile.addView(label(detail, 12f, muted))
        return result
    }
    private fun metricRow(parent: LinearLayout) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL; isBaselineAligned = false
        parent.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(10) })
    }
    private fun divider(parent: LinearLayout) {
        parent.addView(View(this).apply { setBackgroundColor(line) }, LinearLayout.LayoutParams(-1, dp(1)).apply {
            topMargin = dp(13); bottomMargin = dp(10)
        })
    }
    private fun disclosure(parent: LinearLayout, title: String, text: String) {
        val body = label(text, 13f, muted).apply { visibility = View.GONE }
        parent.addView(button(title) {
            body.visibility = if (body.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        })
        parent.addView(body)
    }
    /** Local vector assets in the reference style; no icon dependency or network. */
    private fun referenceIcon(resource: Int, color: Int): Drawable = getDrawable(resource)!!.mutate().apply {
        setTint(color); setBounds(0, 0, dp(20), dp(20))
    }
    private fun page(): Pair<ScrollView, LinearLayout> {
        val scroll = ScrollView(this).apply { isFillViewport = true; isVerticalScrollBarEnabled = false }
        val body = column().apply { setPadding(dp(18), dp(18), dp(18), dp(24)) }
        scroll.addView(body)
        return scroll to body
    }

    private fun buildUi() {
        val root = column().apply { setBackgroundColor(backgroundColor) }
        root.setOnApplyWindowInsetsListener { view, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or
                    WindowInsets.Type.displayCutout() or WindowInsets.Type.ime())
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                view.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                    insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        val host = FrameLayout(this)
        root.addView(host, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        val (dashboard, dashboardBody) = page()
        dashboardPage = dashboard; host.addView(dashboard)
        val (library, libraryBody) = page()
        recordingsPage = library; host.addView(library)
        val (settings, settingsBody) = page()
        settingsPage = settings; host.addView(settings)

        val heading = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        val greeting = column()
        greeting.addView(eyebrow(SimpleDateFormat("EEEE · d MMMM", Locale.getDefault()).format(Date())))
        greeting.addView(label("Your pendant", 28f, bold = true).apply { letterSpacing = -.04f })
        heading.addView(greeting, LinearLayout.LayoutParams(0, -2, 1f))
        dashboardBody.addView(heading)

        val overview = card(dashboardBody, 28).apply { setPadding(dp(18), dp(18), dp(18), dp(18)); background = shape(raisedPanel, 28) }
        pendantName = label("OpenPendant", 20f, bold = true); overview.addView(pendantName)
        connectionValue = label("Disconnected", 12f, green, true).apply {
            background = shape(0xff242d17.toInt(), 20, 0xff394522.toInt())
            setPadding(dp(11), dp(7), dp(11), dp(7))
        }
        overview.addView(connectionValue, LinearLayout.LayoutParams(-2, -2).apply { topMargin = dp(10) })
        quickConnect = button("Connect", true) { connectFromDashboard() }; overview.addView(quickConnect)
        deviceSummary = label("Connect to check your pendant.", 12f, muted); overview.addView(deviceSummary)
        // Recording controls are placed directly after the connection card.
        val longCard = card(dashboardBody)

        section(dashboardBody, "At a glance")
        val metricsOne = metricRow(dashboardBody)
        batteryValue = metric(metricsOne, "Battery", "—", "")
        ledValue = metric(metricsOne, "LED color", "—", "", cyan)
        val capacity = card(dashboardBody)
        capacity.addView(eyebrow("Pendant storage"))
        pendantCapacityValue = label("Capacity not yet reported", 19f, cyan, true)
        capacity.addView(pendantCapacityValue)
        pendantCapacityProgress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100; progress = 0; visibility = View.GONE
            progressTintList = ColorStateList.valueOf(cyan)
            contentDescription = "Occupied share of the enabled recording area"
        }
        capacity.addView(pendantCapacityProgress, LinearLayout.LayoutParams(-1, dp(8)).apply {
            topMargin = dp(12); bottomMargin = dp(12)
        })
        pendantCapacityDetail = label("Connect to check available space.", 13f, muted)
        capacity.addView(pendantCapacityDetail)
        pendantStorageValue = label("", 12f, muted); capacity.addView(pendantStorageValue)
        capacity.addView(button("Storage details") {
            val current = PendantStoragePresentation.from(client.telemetry?.value)
            AlertDialog.Builder(this).setTitle("Recording storage")
                .setMessage(current?.detail ?: "Connect and refresh device status to check recording capacity. Unknown space is not counted as free.")
                .setPositiveButton("Close", null).show()
        })
        val mode = card(dashboardBody)
        mode.addView(eyebrow("Recording mode"))
        modeValue = label("Connect to check", 16f, bold = true); mode.addView(modeValue)
        val phone = card(dashboardBody)
        phone.addView(eyebrow("Your library"))
        syncSummary = label("", 18f, cyan, true); phone.addView(syncSummary)
        phoneStorageValue = label("", 13f, muted); phone.addView(phoneStorageValue)
        phone.addView(button("View recordings") { showTab(1) })
        refreshDevice = button("Refresh status") { client.refreshTelemetry() }; dashboardBody.addView(refreshDevice)

        settingsBody.addView(eyebrow("Make it yours"))
        settingsBody.addView(label("Settings", 30f, bold = true))
        val deviceStatus = card(settingsBody)
        deviceStatus.addView(eyebrow("Technical details"))
        telemetryDetail = label("Connect to check firmware and device activity.", 14f, muted)
        storageTechnical = label("", 13f, muted)
        longStatus = label("", 13f, muted)
        val technicalBody = column().apply { visibility = View.GONE }
        technicalBody.addView(telemetryDetail); technicalBody.addView(storageTechnical); technicalBody.addView(longStatus)
        deviceStatus.addView(button("Device diagnostics") {
            technicalBody.visibility = if (technicalBody.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }); deviceStatus.addView(technicalBody)
        durableSection = DurableLibrarySection(this, durableLibrary, { client.durablePeer() },
            { client.recording || transcriptState.busy || importingModel || pendantSession.longRecording.busy || pendantSession.longRecording.ownsRadio }, { player.stop() },
            { startActivity(Intent(this, RecoveryActivity::class.java)) })
        disclosure(deviceStatus, "About device readings", "Supported firmware reports software observations while connected and idle. Cached readings expire after disconnect or ten seconds; battery freshness also includes the gauge sample age, up to twenty seconds total. Battery percent is a gauge estimate, not measured remaining runtime. Charging state is not reported. LED values are commanded levels, not optical measurements. Storage counts cover the enabled recording allocation, not raw NAND. " +
            "Portable recording requires supported firmware and a fresh battery check. The pendant stops and attempts to save if battery data becomes low or unavailable; abrupt power loss can still lose the last audio. Supported firmware can sync on battery after fresh power checks. Recovery backup and explicit enrollment remain required. Encrypted-recording transcription is not yet enabled.")

        val connection = card(settingsBody)
        // Connection setup comes first; diagnostics follow the everyday settings.
        settingsBody.removeView(connection); settingsBody.addView(connection, 2)
        connection.addView(eyebrow("Connection"))
        connection.addView(label("Bluetooth", 21f, bold = true))
        status = label("", 16f).apply { accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE }
        connection.addView(status)
        scan = button("Scan for pendant") { requestScan() }; connection.addView(scan)
        devices = column(); connection.addView(devices)
        pair = button("Pair using USB") {
            if (!client.connected && !client.connecting && !client.recording && !client.pairing && !durableLibrary.busy) {
                @Suppress("DEPRECATION")
                startActivityForResult(Intent(this, UsbPairingActivity::class.java), 103)
            }
        }; connection.addView(pair)
        connect = button("Connect") {
            player.stop()
            if (client.connected) client.disconnect() else client.connect()
        }; connection.addView(connect)
        pairingHelpButton = button("Pairing help") { pairingHelp() }; connection.addView(pairingHelpButton)

        longCard.addView(eyebrow("Record on pendant"))
        captureValue=label("Connect to check",21f,bold=true).apply { accessibilityLiveRegion=View.ACCESSIBILITY_LIVE_REGION_POLITE }
        longCard.addView(captureValue)
        recordingHint = label("Keeps recording without your phone.",13f,muted); longCard.addView(recordingHint)
        longStart=button("Start recording",true){
            AlertDialog.Builder(this).setTitle("Start recording on the pendant?")
                .setMessage("The microphone stays on until you stop, storage fills, or a power/safety check stops it. Portable firmware checks battery power; older firmware needs USB throughout. Bluetooth disconnection does not stop recording. Audio stays encrypted on the pendant. Make sure people nearby are aware.")
                .setNegativeButton("Cancel",null).setPositiveButton("Start recording"){_,_->
                    if(!durableLibrary.busy&&!client.recording&&!transcriptState.busy&&!importingModel){
                        player.stop();pendantSession.longRecording.startExplicit()
                    }
                }.show()
        };longCard.addView(longStart)
        longArm=button("Use pendant button"){
            AlertDialog.Builder(this).setTitle("Arm one button-start?")
                .setMessage("Within two minutes, briefly tap and release the pendant button to start recording. A second short tap stops and saves it. Do not hold the button. Portable firmware checks battery power; older firmware needs USB throughout. Arming expires after two minutes and never survives a reboot. Make sure people nearby are aware.")
                .setNegativeButton("Cancel",null).setPositiveButton("Arm button"){_,_->
                    if(!durableLibrary.busy&&!client.recording&&!transcriptState.busy&&!importingModel){
                        player.stop();pendantSession.longRecording.armButtonExplicit()
                    }
                }.show()
        };longCard.addView(longArm)
        longStop=button("Stop and save"){pendantSession.longRecording.stopExplicit()};longCard.addView(longStop)
        longCheck=button("Check recording status"){pendantSession.longRecording.checkStatus()};longCard.addView(longCheck)

        val recording = card(settingsBody)
        recording.addView(eyebrow("Diagnostics"))
        recording.addView(label("Microphone test", 21f, bold = true))
        val shortTest = column().apply { visibility = View.GONE }
        recording.addView(button("Show short-clip test") {
            shortTest.visibility = if (shortTest.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }); recording.addView(shortTest)
        shortTest.addView(label("Tap Record, then briefly tap and release the pendant button. " +
            "Speak for the two-second red light. Only the last ~1 second is kept; the first second is startup warm-up.", 15f, muted))
        recordPrompt = label("", 16f, bold = true); shortTest.addView(recordPrompt)
        record = button("Record short clip", true) { requestRecording() }; shortTest.addView(record)
        progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { max = 100; visibility = View.GONE }
        progress.progressTintList = ColorStateList.valueOf(green)
        shortTest.addView(progress, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(16)))
        cancel = button("Cancel request / recording") { client.cancel() }; shortTest.addView(cancel)

        val security = card(settingsBody)
        security.addView(eyebrow("Private by design"))
        security.addView(label("Recording security", 21f, bold = true))
        security.addView(button("Move pendant / change recording key…") {
            if (client.recording || transcriptState.busy || importingModel || durableLibrary.busy || pendantSession.longRecording.busy || pendantSession.longRecording.ownsRadio)
                showError("Finish the current recording, transfer or playback, then disconnect Bluetooth first.")
            else startActivityForResult(Intent(this, PhoneMigrationActivity::class.java), 103)
        })
        securityStatus = label("Keep your recovery backup somewhere safe.", 14f, muted); security.addView(securityStatus)
        security.addView(button("Recording key and recovery…") {
            if (client.recording || transcriptState.busy || importingModel || durableLibrary.busy) {
                showError("Finish or cancel the current recording, transcription or model import first.")
            } else startActivity(Intent(this, RecoveryActivity::class.java))
        })

        libraryBody.addView(eyebrow("Your private library"))
        libraryBody.addView(label("Recordings", 32f, bold = true).apply { letterSpacing = -.04f })
        librarySummary = label("", 13f, muted); libraryBody.addView(librarySummary)
        libraryBody.addView(durableSection.dashboard)
        val search = EditText(this).apply {
            hint = "Search titles or dates"; textSize = 14f; setTextColor(ink); setHintTextColor(muted)
            background = shape(panel, 16); setPadding(dp(15), dp(13), dp(15), dp(13))
            minHeight = dp(48); minimumHeight = dp(48)
            isSingleLine = true; maxLines = 1; inputType = android.text.InputType.TYPE_CLASS_TEXT
            filters = arrayOf(android.text.InputFilter.LengthFilter(120))
            contentDescription = "Search recording titles or dates; transcripts and audio are not searched"
            setText(libraryQuery)
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                    libraryQuery = s?.toString().orEmpty(); visibleRecordings = 30; renderedLibrary = ""
                    if (uiReady) { renderLibrary(readTranscripts = false); durableSection.render(libraryQuery) }
                }
                override fun afterTextChanged(s: Editable?) = Unit
            })
        }
        libraryBody.addView(search, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(17); bottomMargin = dp(5) })
        libraryBody.addView(durableSection.library)
        val testClips = column().apply { visibility = View.GONE }
        libraryBody.addView(button("Short test clips on this phone") {
            testClips.visibility = if (testClips.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }); libraryBody.addView(testClips)
        recordingsList = column(); testClips.addView(recordingsList)
        val speech = card(settingsBody)
        speech.addView(eyebrow("On-device intelligence"))
        speech.addView(label("Lithuanian transcription", 20f, bold = true))
        speech.addView(label("Only when you tap Transcribe. No upload.", 13f, cyan))
        speech.addView(label("Available for short test clips. Long recordings are not supported yet.", 13f, muted))
        modelStatus = label("", 13f, muted); speech.addView(modelStatus)
        importModel = button("Import speech model…") { chooseModel() }; speech.addView(importModel)
        disclosure(speech, "Playback & transcription notes", "Playback is volume-adjusted: 100 Hz high-pass and peak normalization. Saved WAVs stay raw. Start with a comfortable phone volume.\n\n" +
            "Lithuanian transcription runs only after you tap Transcribe. Keep the app open. This first version uses Whisper base; very short clips may be inaccurate. Check important text against the audio.")
        // Common settings precede diagnostics. No action is triggered by navigation.
        settingsBody.removeView(security); settingsBody.addView(security, 3)
        settingsBody.removeView(speech); settingsBody.addView(speech, 4)
        deviceSettings=DeviceSettingsSection(this,client) {
            durableLibrary.busy||client.recording||transcriptState.busy||importingModel||
                pendantSession.longRecording.busy||pendantSession.longRecording.ownsRadio
        }
        settingsBody.addView(deviceSettings.view,3,LinearLayout.LayoutParams(-1,-2).apply { topMargin=dp(12);bottomMargin=dp(12) })
        storageSettings=StorageSettingsSection(this,durableLibrary,client, {
            client.preferencesSave.busy||client.recording||transcriptState.busy||importingModel||pendantSession.longRecording.busy||pendantSession.longRecording.ownsRadio
        }, ::connectFromDashboard)
        settingsBody.addView(storageSettings.view,4,LinearLayout.LayoutParams(-1,-2).apply { topMargin=dp(12);bottomMargin=dp(12) })
        settingsBody.addView(button("About OpenPendant") { showAbout() })
        settingsBody.addView(label("Private by design · no account or cloud", 12f, muted))
        operationBar = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL; setPadding(dp(14), dp(6), dp(14), dp(6))
            background = shape(raisedPanel, 18); visibility = View.GONE
        }
        operationLabel = label("", 13f, cyan, true)
        operationBar.addView(operationLabel, LinearLayout.LayoutParams(0, -2, 1f))
        operationStop = button("Stop") {
            if (durableLibrary.state.work in setOf(DurableLibraryWork.PLAY, DurableLibraryWork.SYNC)) durableLibrary.cancel()
        }
        operationBar.addView(operationStop, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8) })
        root.addView(operationBar, LinearLayout.LayoutParams(-1,-2).apply { setMargins(dp(12),dp(6),dp(12),0) })
        val navigation = LinearLayout(this).apply { setPadding(dp(8), dp(6), dp(8), dp(6)); background = shape(panel, 24) }
        dashboardTab = button("Dashboard") { showTab(0) }
        recordingsTab = button("Recordings") { showTab(1) }
        settingsTab = button("Settings") { showTab(2) }
        navigation.addView(dashboardTab, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        navigation.addView(recordingsTab, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        navigation.addView(settingsTab, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(navigation, LinearLayout.LayoutParams(-1, -2).apply { setMargins(dp(12), dp(6), dp(12), dp(10)) })
        setContentView(root); root.requestApplyInsets()
    }

    private fun showTab(tab: Int) {
        activeTab = tab
        dashboardPage.visibility = if (tab == 0) View.VISIBLE else View.GONE
        recordingsPage.visibility = if (tab == 1) View.VISIBLE else View.GONE
        settingsPage.visibility = if (tab == 2) View.VISIBLE else View.GONE
        listOf(dashboardTab, recordingsTab, settingsTab).forEachIndexed { index, view ->
            view.isSelected = index == tab
            val selectedColor = if (index == tab) green else muted
            view.setTextColor(selectedColor); view.textSize = 12f
            view.backgroundTintList = null
            view.background = shape(if (index == tab) raisedPanel else panel, 17, if (index == tab) line else panel)
            view.setCompoundDrawables(null, referenceIcon(listOf(R.drawable.ic_dashboard, R.drawable.ic_recordings, R.drawable.ic_settings)[index], selectedColor), null, null)
            view.compoundDrawablePadding = dp(5)
            view.contentDescription = listOf("Dashboard", "Recordings", "Settings")[index] + if (index == tab) ", selected" else ""
        }
    }

    private fun refreshLibrary() {
        try { localRecordings = repository.entries(); libraryError = null }
        catch (_: Exception) { libraryError = "Could not list local recordings. No files were changed." }
        renderedLibrary = ""
        transcriptCache.clear(); transcriptReadErrors.clear()
    }

    /** An explicit tap may reconnect the enrolled bond without a new scan.
     * Merely opening the app or receiving a metadata update never connects. */
    private fun connectFromDashboard() {
        if (client.connected) { showTab(2); return }
        if (client.connecting || client.recording || client.pairing || durableLibrary.busy) return
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            showTab(2); return
        }
        if (client.selected == null) {
            val address = durableLibrary.state.binding?.bondAddress
            try {
                val adapter = getSystemService(android.bluetooth.BluetoothManager::class.java)?.adapter
                adapter?.bondedDevices?.singleOrNull { it.address == address }?.let(client::select)
            } catch (_: SecurityException) { showTab(2); return }
        }
        if (client.selected != null && client.isBonded) { player.stop(); client.connect() } else showTab(2)
    }

    private fun requestRecording() {
        if (!client.ready || client.recording || libraryError != null || transcriptState.busy || importingModel || durableLibrary.busy || pendantSession.longRecording.ownsRadio || pendantSession.longRecording.activeRecording) return
        // Preflight only; publication never replaces an existing recording.
        if (filesDir.usableSpace < 1024 * 1024) { showError("Not enough free phone storage for a new clip. Existing recordings are unchanged."); return }
        AlertDialog.Builder(this).setTitle("Record with the pendant?")
            .setMessage("This requests up to two seconds of microphone capture. A fresh short button tap on the pendant is still required. " +
                "The final ~1 second will be added to your phone library. Existing recordings are kept. No upload. Make sure anyone nearby is aware.")
            .setNegativeButton("Not now", null).setPositiveButton("Request clip") { _, _ ->
                if (!client.ready || client.recording || !client.channelIdle || libraryError != null ||
                    transcriptState.busy || importingModel || durableLibrary.busy) {
                    showError("The connection is busy or changed while this dialog was open. No recording was requested. Please try again when ready.")
                } else { player.stop(); client.record() }
            }.show()
    }

    override fun changed() {
        if (!uiReady || isFinishing) return
        val longControl=pendantSession.longRecording
        val longObservation=longControl.observation
        val longState=longObservation?.state
        val longFresh=client.connected&&longControl.ownsRadio&&
            android.os.SystemClock.elapsedRealtime()-longControl.observedAt in 0..10_000&&longObservation?.outcome==LongRecordingOutcome.OBSERVED
        fun clockText(frames:Int):String {val seconds=frames/50;return "%d:%02d".format(seconds/60,seconds%60)}
        longStatus.stableText=longControl.message+(longState?.let {
            "\n${if(longFresh)"" else "Last observed: "}captured ${clockText(it.accepted)} · stored ${clockText(it.committed)}"
        }?:"")
        val canLong=durableLibrary.state.recipientReady&&client.longPeer()!=null&&!client.preferencesSave.busy&&!longControl.busy&&!durableLibrary.busy&&!client.recording&&!transcriptState.busy&&!importingModel
        val powerBlock=RecordingPowerStatus.blocked(client.telemetry,android.os.SystemClock.elapsedRealtime(),client.connected)
        longStart.isEnabled=canLong&&powerBlock==null&&!longControl.activeRecording&&longObservation?.outcome!=LongRecordingOutcome.BOOT_CHANGED
        longArm.isEnabled=longStart.isEnabled
        longStop.isEnabled=canLong&&longControl.activeRecording
        longCheck.isEnabled=canLong
        val state = DashboardState.from(client.connected, client.connecting, client.ready,
            client.recording, client.currentClipState, client.cancelling)
        connectionValue.stableText = state.connection
        val captureLedRequested = state.led.startsWith("Red requested")
        if(captureLedRequested || client.telemetry==null) {
            ledValue.stableText = if (captureLedRequested) "Red requested" else "Not reported"
            ledValue.setTextColor(if (captureLedRequested) coral else ink)
            ledValue.contentDescription = state.led
        }
        val telemetry = client.telemetry
        val now = android.os.SystemClock.elapsedRealtime()
        captureValue.stableText = DashboardReadings.recording(client.connected, telemetry, now, longObservation, longControl.observedAt, client.recording)
        recordingHint.stableText = when {
            !durableLibrary.state.recipientReady -> "Finish recording setup in Settings → Move pendant / change recording key. Bluetooth pairing alone is not enough."
            longObservation?.outcome in setOf(LongRecordingOutcome.UNKNOWN, LongRecordingOutcome.BOOT_CHANGED) -> longControl.message
            longControl.message.startsWith("A fresh,") -> "Refresh the battery reading before starting."
            !client.connected -> "Connect above to see whether the pendant is recording."
            powerBlock!=null&&!longControl.activeRecording -> powerBlock
            longControl.busy -> "Checking with your pendant…"
            longState?.has(LongRecordingControlCodec.BUTTON_ARMED) == true && longFresh -> "Tap once within two minutes to start. Tap again to stop; don't hold."
            longState?.phase == LongRecordingControlCodec.Phase.STOPPED && now-longControl.observedAt in 0..10_000 -> "Sync in Recordings to save a phone copy."
            client.longPeer()?.let { it.capabilityBits and LongRecordingControlCodec.STANDALONE_CAPABILITY != 0L } == true ->
                "Short tap: start / stop. On firmware 0.4.60, wait briefly after one tap; five quick taps while idle replace phone pairing. Do not hold."
            else -> "Keeps recording without your phone."
        }
        securityStatus.stableText = if (durableLibrary.state.recipientReady) "Recovery backup verified"
            else "Recovery setup needed before recording sync."
        val observationCurrent = telemetry?.fresh(now, client.connected) == true && !client.recording
        val battery = telemetry?.freshBattery(now, client.connected)
        val readings = DashboardReadings.from(telemetry, now, client.connected && !client.recording && durableLibrary.state.work != DurableLibraryWork.SYNC)
        batteryValue.stableText = readings.battery
        modeValue.stableText = readings.mode
        deviceSummary.stableText = readings.device
        batteryValue.contentDescription = "${batteryValue.text}. Gauge estimate, not remaining runtime. Charging state not reported."
        if (telemetry != null) {
            val device = telemetry.value
            telemetryDetail.stableText = "Firmware ${device.firmware} · uptime at update ${device.uptimeMs / 1000}s\n" +
                "Last observed microphone power ${if (device.microphonePower) "on" else "off"} · " +
                (if (device.resourceBusy) "device busy" else "device idle") +
                (if (device.faults != 0L) " · hardware fault ${device.faults}" else "") + "\n" +
                telemetry.description(now, client.connected) + (if (client.recording) " · polling paused during capture" else "") +
                (device.recorder?.let { "\n\n${it.details}" } ?: "") +
                (battery?.let { "\nBattery: ${it.percent}% · ${it.millivolts} mV (charging state not reported)" } ?: "")
            if (!client.recording) {
                ledValue.stableText = readings.led
                ledValue.setTextColor(when {
                    !observationCurrent -> muted
                    device.ledSummary == "Red commanded" -> coral
                    device.ledSummary == "Green commanded" -> green
                    device.ledSummary == "Blue commanded" -> cyan
                    else -> ink
                })
                ledValue.contentDescription = "${ledValue.text}. ${telemetry.description(now, client.connected)}. Last software command; no optical readback."
            }
        } else {
            telemetryDetail.stableText = when {
                client.connected && !client.ready && !client.recording -> "Checking device status support…"
                client.connected && !client.telemetrySupported ->
                    "Device status needs newer pendant firmware. Battery and storage remain unavailable."
                else -> "Connect to check firmware and device activity."
            }
        }
        refreshDevice.isEnabled = client.canRefreshTelemetry
        val authenticated = client.connected && (client.ready || client.recording)
        connectionValue.setTextColor(if (authenticated) green else muted)
        if(connectionValue.tag!=authenticated) {
            connectionValue.tag=authenticated
            connectionValue.background = if (authenticated) shape(0xff242d17.toInt(), 20, 0xff394522.toInt()) else shape(panel, 20)
        }
        pendantName.stableText = client.found.values.firstOrNull { it.device == client.selected }?.title ?: "OpenPendant"
        val rows = durableLibrary.state.recordings
        val work = durableLibrary.state.work
        operationBar.visibility = if (work in setOf(DurableLibraryWork.PLAY, DurableLibraryWork.SYNC)) View.VISIBLE else View.GONE
        operationLabel.stableText = if (work == DurableLibraryWork.PLAY)
            if (durableLibrary.player?.snapshot()?.playing == false) "Playback paused" else "Playing recording"
            else durableLibrary.state.transfer?.let { "Syncing · ${it.percent}%" } ?: "Preparing sync…"
        operationStop.contentDescription = if (work == DurableLibraryWork.PLAY) "Stop recording playback" else "Stop storage sync"
        val awaiting = rows.count(RecordingListPresentation::needsSync)
        syncSummary.stableText = if (durableLibrary.state.work == DurableLibraryWork.SYNC) "Syncing to phone…"
            else if (awaiting > 0) "$awaiting need sync" else "${rows.count(RecordingListPresentation::phoneComplete)} saved on phone"
        phoneStorageValue.stableText = libraryError ?: "${rows.count { RecordingListPresentation.matches(it, RecordingListFilter.PENDANT) }} on pendant · last sync"
        status.accessibilityLiveRegion = if (client.recording) View.ACCESSIBILITY_LIVE_REGION_NONE else View.ACCESSIBILITY_LIVE_REGION_POLITE
        recordPrompt.accessibilityLiveRegion = if (client.recording) View.ACCESSIBILITY_LIVE_REGION_POLITE else View.ACCESSIBILITY_LIVE_REGION_NONE
        if (status.text.toString() != client.message) status.stableText = client.message
        val prompt = if (client.recording) client.message else "After Record, briefly tap and release the pendant button when prompted."
        if (recordPrompt.text.toString() != prompt) recordPrompt.stableText = prompt
        scan.isEnabled = !client.connected && !client.connecting && !client.recording && !client.pairing && !durableLibrary.busy
        scan.stableText = if (client.scanning) "Restart scan" else "Scan for pendant"
        scan.visibility = if (client.connected) View.GONE else View.VISIBLE
        devices.visibility = scan.visibility
        pairingHelpButton.visibility = scan.visibility
        pair.isEnabled = !client.connected && !client.connecting && !client.recording && !client.pairing && !durableLibrary.busy
        pair.visibility = if (!client.connected) View.VISIBLE else View.GONE
        connect.isEnabled = client.selected != null && client.isBonded && !client.pairing && !client.recording && !client.connecting && !durableLibrary.busy
        connect.stableText = if (client.connected) "Disconnect" else if (client.connecting) "Connecting…" else "Connect"
        quickConnect.stableText = if (client.connected) "Connection settings" else if (client.connecting) "Connecting…"
            else if (client.selected != null && client.isBonded || durableLibrary.state.binding != null) "Connect pendant" else "Set up connection"
        quickConnect.isEnabled = !client.connecting && !client.recording && !client.pairing && !durableLibrary.busy
        quickConnect.visibility = if (client.connected) View.GONE else View.VISIBLE
        record.isEnabled = client.ready && client.channelIdle && !client.recording && libraryError == null && !transcriptState.busy && !importingModel && !durableLibrary.busy && !longControl.activeRecording && !longControl.busy
        val remoteRecording = observationCurrent && (telemetry?.value?.recorder?.recording == true || telemetry?.value?.microphonePower == true)
        longStart.visibility = if (longControl.activeRecording || remoteRecording) View.GONE else View.VISIBLE
        val standaloneButton=client.longPeer()?.capabilityBits?.and(LongRecordingControlCodec.STANDALONE_CAPABILITY)!=0L&&client.longPeer()!=null
        longArm.visibility = if(standaloneButton)View.GONE else longStart.visibility
        longStop.visibility = if (longControl.activeRecording || remoteRecording) View.VISIBLE else View.GONE
        longCheck.visibility = if (client.connected && ((!observationCurrent && !longFresh) ||
            longObservation?.outcome in setOf(LongRecordingOutcome.UNKNOWN,LongRecordingOutcome.BOOT_CHANGED) ||
            remoteRecording && !longControl.activeRecording)) View.VISIBLE else View.GONE
        if (remoteRecording && !longControl.activeRecording) recordingHint.stableText = "Check recording status to load Stop and save."
        cancel.isEnabled = client.recording && !client.cancelling
        progress.visibility = if (client.recording) View.VISIBLE else View.GONE; progress.progress = client.progress
        librarySummary.stableText = libraryError ?: "${rows.count { RecordingListPresentation.matches(it, RecordingListFilter.ALL) }} recordings · stored privately"
        modelStatus.stableText = if (importingModel) "Checking and importing model…" else modelMessage ?: if (modelAvailable())
            "Offline model ready · Lithuanian"
            else "Speech model needed · import ggml-base-q5_1.bin (59.7 MB). No model downloads happen in this app."
        importModel.isEnabled = !client.recording && !transcriptState.busy && !importingModel && !durableLibrary.busy
        pendantStorageValue.stableText = readings.entries
        val capacity = PendantStoragePresentation.from(telemetry?.value)
        val capacityStale = !observationCurrent || durableLibrary.state.work == DurableLibraryWork.SYNC
        pendantCapacityValue.stableText = readings.storageTitle
        pendantCapacityDetail.stableText = readings.storageDetail
        storageTechnical.stableText = capacity?.detail ?: "Storage capacity has not been reported."
        pendantCapacityProgress.visibility = if (capacity == null) View.GONE else View.VISIBLE
        pendantCapacityProgress.progress = capacity?.percentUsed ?: 0
        pendantCapacityProgress.contentDescription = if (capacity == null) "Capacity unavailable"
            else "${if (capacityStale) "Last observation: " else ""}${capacity.percentUsed}% of enabled recording space occupied"
        durableSection.render(libraryQuery)
        deviceSettings.render()
        storageSettings.render()
        mainHandler.removeCallbacks(autoTransferCheck)
        if (foreground) mainHandler.postDelayed(autoTransferCheck, 300)
        renderLibrary()
        val devicesKey = client.found.values.joinToString("|") { "${it.title}:${it.device == client.selected}" } +
            ":${client.connected}:${client.connecting}:${client.recording}:${client.pairing}:${durableLibrary.busy}"
        if (devicesKey != renderedDevices) {
            renderedDevices = devicesKey; devices.removeAllViews()
            client.found.values.forEach { found ->
                devices.addView(button(if (found.device == client.selected) "Selected · ${found.title}" else found.title) { client.select(found.device) }
                    .apply { isEnabled = !client.connected && !client.connecting && !client.recording && !client.pairing && !durableLibrary.busy })
            }
        }
    }

    private fun recordingDate(entry: LocalRecording): String = if (entry.createdAtMillis > 0)
        DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(entry.createdAtMillis)) else "Date unknown"

    private fun recordingGroup(entry: LocalRecording): String {
        if (entry.createdAtMillis <= 0) return "Date unknown"
        val day = SimpleDateFormat("yyyy-MM-dd", Locale.ROOT)
        val date = day.format(Date(entry.createdAtMillis))
        val calendar = Calendar.getInstance()
        if (date == day.format(calendar.time)) return "Today"
        calendar.add(Calendar.DAY_OF_YEAR, -1)
        return if (date == day.format(calendar.time)) "Yesterday" else recordingDate(entry)
    }

    private fun readTranscriptForDisplay(entry: LocalRecording) {
        if (transcriptCache.containsKey(entry.id) || transcriptReadErrors.contains(entry.id)) return
        try { transcriptCache[entry.id] = transcripts.read(entry.id) }
        catch (_: Exception) { transcriptReadErrors.add(entry.id) }
    }

    private fun renderLibrary(readTranscripts: Boolean = true) {
        val key = localRecordings.joinToString { "${it.id}:${it.byteCount}:${it.createdAtMillis}" } +
            ":${client.recording}:${player.playing}:$playingRecordingId:$visibleRecordings:$libraryError:$transcriptState:$importingModel:${modelAvailable()}:$libraryQuery:${durableLibrary.busy}"
        if (key == renderedLibrary) return
        renderedLibrary = key; recordingsList.removeAllViews()
        // Search only already-listed title/date metadata. It never opens audio,
        // reads a sidecar or searches transcript contents.
        val query = libraryQuery.trim()
        val matches = localRecordings.filter { query.isEmpty() ||
            (it.title + " " + recordingDate(it) + " " + recordingGroup(it)).contains(query, ignoreCase = true) }
        if (matches.isEmpty()) {
            val empty = card(recordingsList)
            libraryEmptyState = label(if (query.isEmpty()) "Your clips will live here" else "No matching recordings", 20f, bold = true)
            empty.addView(libraryEmptyState)
            empty.addView(label(if (query.isEmpty()) "Microphone test clips appear here. Find the short-clip test in Settings."
                else "Try another title or date. Audio and transcripts are not searched.", 14f, muted))
        }
        var previousGroup = ""
        matches.take(visibleRecordings).forEach { entry ->
            val group = recordingGroup(entry)
            if (group != previousGroup) {
                recordingsList.addView(eyebrow(group), LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(20); bottomMargin = dp(1) })
                previousGroup = group
            }
            val item = card(recordingsList)
            val selectedPlaying = player.playing && playingRecordingId == entry.id
            val top = LinearLayout(this).apply { gravity = Gravity.TOP }
            val play = button(if (selectedPlaying) "Stop" else "") {
                if (durableLibrary.busy) return@button
                if (player.playing && playingRecordingId == entry.id) player.stop()
                else { player.stop(); playingRecordingId = entry.id; player.play { repository.readPcm(entry.id) } }
            }.apply {
                isEnabled = !client.recording && !transcriptState.busy && !durableLibrary.busy
                contentDescription = (if (selectedPlaying) "Stop playback" else "Play volume-adjusted clip") + ": " + entry.title
                setPadding(dp(if (selectedPlaying) 8 else 12), dp(12), dp(if (selectedPlaying) 8 else 12), dp(12)); textSize = 11f
                minimumWidth = dp(48); minWidth = dp(48)
                background = shape(if (selectedPlaying) green else raisedPanel, 14)
                setTextColor(backgroundColor)
                if (!selectedPlaying) setCompoundDrawables(referenceIcon(R.drawable.ic_play, green), null, null, null)
            }
            top.addView(play, LinearLayout.LayoutParams(if (selectedPlaying) -2 else dp(48), -2))
            val copy = column()
            copy.addView(label(entry.title, 16f, bold = true))
            val time = if (entry.createdAtMillis > 0) DateFormat.getTimeInstance(DateFormat.SHORT).format(Date(entry.createdAtMillis)) else "Time unknown"
            copy.addView(label("%s · %.2f sec · %s".format(Locale.getDefault(), time, entry.durationSeconds, formatBytes(entry.byteCount)), 12f, muted))
            copy.addView(label("PHONE COPY", 10f, cyan, true).apply {
                letterSpacing = .07f; background = shape(0xff17262c.toInt(), 7, 0xff17262c.toInt())
                setPadding(dp(7), dp(5), dp(7), dp(5))
                layoutParams = LinearLayout.LayoutParams(-2, -2).apply { topMargin = dp(6) }
            })
            top.addView(copy, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(12) })
            item.addView(top)
            if (readTranscripts && query.isEmpty()) readTranscriptForDisplay(entry)
            val transcript = transcriptCache[entry.id]
            if (transcriptReadErrors.contains(entry.id)) item.addView(label("Saved transcript could not be read. Audio is unchanged.", 13f, muted))
            if (transcript != null) {
                val text = label(transcript.text.ifBlank { "No speech text returned." }, 15f).apply { setTextIsSelectable(true); visibility = View.GONE }
                item.addView(button("View Lithuanian transcript") {
                    text.visibility = if (text.visibility == View.VISIBLE) View.GONE else View.VISIBLE
                }.apply { setTextColor(cyan) })
                item.addView(text)
            } else if (!transcriptCache.containsKey(entry.id) && !transcriptReadErrors.contains(entry.id)) {
                item.addView(button("Check saved transcript") {
                    readTranscriptForDisplay(entry); renderedLibrary = ""; renderLibrary(readTranscripts = false)
                }.apply { isEnabled = !client.recording && !transcriptState.busy && !durableLibrary.busy })
            }
            if (transcriptState.recordingId == entry.id) {
                item.addView(label(transcriptState.message ?: "", 14f, muted))
                if (transcriptState.busy) {
                    item.addView(ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
                        max = 100; progress = transcriptState.progress
                        progressTintList = ColorStateList.valueOf(green)
                        contentDescription = "Transcription ${transcriptState.progress} percent"
                    })
                    item.addView(button(if (transcriptState.phase == TranscriptionPhase.CANCELLING) "Stopping…" else "Cancel transcription") {
                        transcription.cancel()
                    }.apply { isEnabled = transcriptState.phase == TranscriptionPhase.RUNNING })
                }
            }
            divider(item)
            val actions = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL; isBaselineAligned = false }
            actions.addView(button(if (transcript == null) "Transcribe\nLithuanian" else "Transcribe again\nLithuanian") {
                requestTranscription(entry)
            }.apply { textSize = 12f; isEnabled = !client.recording && !transcriptState.busy && !importingModel && modelAvailable() && !durableLibrary.busy },
                LinearLayout.LayoutParams(0, -2, 1f))
            actions.addView(button("Delete…") { confirmDelete(entry) }.apply {
                textSize = 12f; setTextColor(coral); isEnabled = !client.recording && !durableLibrary.busy
            }, LinearLayout.LayoutParams(0, -2, .65f).apply { marginStart = dp(8) })
            item.addView(actions)
        }
        if (matches.size > visibleRecordings) recordingsList.addView(button("Show more recordings") {
            visibleRecordings += 30; changed()
        })
    }

    private fun showAbout() {
        @Suppress("DEPRECATION") val version = packageManager.getPackageInfo(packageName, 0).versionName ?: ""
        AlertDialog.Builder(this).setTitle("OpenPendant · $version")
            .setMessage("Your pendant. Your recordings.\n\nNo account. No Internet access. No phone microphone.\n\n" +
                "Started storage sync continues in the background with a Stop notification; saved checkpoints survive interruptions. Leaving the app stops short-clip capture, playback and transcription. Changing tabs does not disconnect. Optional automatic sync is configured under Recordings → Transfer.\n\n" +
                "Record on the pendant from Dashboard. Sync saved recordings from Recordings while the pendant has USB power. Recovery backup is required. Lithuanian transcription currently supports short phone clips only; long-recording transcription is not yet available.")
            .setPositiveButton("Got it", null).show()
    }

    private fun confirmDelete(entry: LocalRecording) {
        val body = column().apply { setPadding(dp(22), dp(10), dp(22), 0) }
        body.addView(label("This recording is saved on this phone only. Deleting it removes the audio and its transcript, and stops transcription for it. There is no cloud backup.", 15f))
        AlertDialog.Builder(this).setTitle("Delete ${entry.title.lowercase()}?").setView(body)
            .setNegativeButton("Keep", null).setPositiveButton("Delete from phone") { _, _ ->
                if (client.recording || durableLibrary.busy) { showError("Finish or cancel the current operation first."); return@setPositiveButton }
                player.stop()
                try { transcription.deletePhone(entry.id); refreshLibrary(); changed() }
                catch (_: Exception) { refreshLibrary(); changed(); showError("Deletion could not be fully confirmed. Check the library; other recordings were not changed.") }
            }.show()
    }

    private fun formatBytes(bytes: Long): String = if (bytes < 1024) "$bytes B" else "%.1f KB".format(Locale.getDefault(), bytes / 1024.0)

    private fun modelAvailable() = try { models.available() } catch (_: Exception) { false }

    private fun requestTranscription(entry: LocalRecording) {
        if (client.recording || transcriptState.busy || importingModel || !modelAvailable() || durableLibrary.busy) return
        AlertDialog.Builder(this).setTitle("Transcribe on this phone?")
            .setMessage("Read this saved recording with the local Lithuanian model? Audio and text stay on this phone. Keep OpenPendant open until finished. Any previous transcript is replaced only after success.")
            .setNegativeButton("Not now", null).setPositiveButton("Transcribe") { _, _ ->
                if (client.recording || importingModel || transcription.snapshot().busy || durableLibrary.busy) return@setPositiveButton
                player.stop()
                try { transcription.start(entry.id) }
                catch (_: Exception) { showError("Transcription could not start. The recording is unchanged.") }
            }.show()
    }

    private fun chooseModel() {
        if (client.recording || transcription.snapshot().busy || importingModel || durableLibrary.busy) return
        AlertDialog.Builder(this).setTitle("Import the offline speech model")
            .setMessage("Choose ggml-base-q5_1.bin copied from this project's models folder. Only the exact trusted multilingual model is accepted. The file is copied into private app storage; no audio is read.")
            .setNegativeButton("Cancel", null).setPositiveButton("Choose file") { _, _ ->
                try {
                    @Suppress("DEPRECATION")
                    startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                        addCategory(Intent.CATEGORY_OPENABLE); type = "*/*"
                    }, 102)
                } catch (_: Exception) { showError("No file picker is available on this phone.") }
            }.show()
    }

    @Deprecated("Uses the platform document picker without an additional AndroidX dependency")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == 103) {
            client.endUsbPairing()
            val address = data?.getStringExtra("pendantAddress")
            if (resultCode == RESULT_OK && address != null && BluetoothAdapter.checkBluetoothAddress(address)) {
                try {
                    val adapter = getSystemService(android.bluetooth.BluetoothManager::class.java)?.adapter
                    adapter?.bondedDevices?.firstOrNull { it.address == address }?.let(client::select)
                } catch (_: SecurityException) { showError("Allow Nearby devices to select the paired pendant.") }
            }
            return
        }
        if (requestCode != 102 || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        if (client.recording || transcription.snapshot().busy || importingModel || durableLibrary.busy) return
        importingModel = true; importCancelled.set(false); changed()
        modelWorker.execute {
            val message = try {
                contentResolver.openInputStream(uri)?.use { models.importVerified(it) { importCancelled.get() } }
                    ?: throw IllegalArgumentException("No model stream")
                "Model imported and checksum verified · ready for Tap Transcribe"
            } catch (_: Exception) { "Import did not finish or the file was not the trusted model. Any previous model is unchanged." }
            mainHandler.post {
                importingModel = false; modelMessage = message
                if (uiReady && !isFinishing) changed()
            }
        }
    }

    override fun recordingComplete(pcm: ByteArray) {
        try { repository.save(pcm) }
        catch (_: Exception) {
            // A failure after publication can still leave a valid new file. Refresh
            // without reading audio and do not mislabel this as a Bluetooth failure.
            refreshLibrary(); changed(); showTab(1)
            throw ProtocolException("Phone storage could not confirm saving the new clip. Existing recordings are unchanged. Check the library; this pendant transfer cannot be retried")
        }
        refreshLibrary(); changed(); showTab(1)
    }

    private fun pairingHelp() {
        AlertDialog.Builder(this).setTitle("Pair using USB").setMessage(
            "First setup: connect the pendant directly to this phone using a USB data cable, then tap Pair using USB. Allow USB and Nearby devices. The app passes the temporary code to Android automatically; Android may still show its own pairing prompt. Firmware 0.4.61 or later is required.\n\n" +
            "Normal recording and sync use Bluetooth without the cable. USB setup never erases an existing pairing.\n\n" +
            "To replace a phone: while idle, press the pendant button five times quickly within 3.5 seconds. Blue blinking means pairing is open for 60 seconds. On a previously paired phone, Forget the old pendant in Android Bluetooth settings first. Then use USB setup.\n\n" +
            "Replacing pairing keeps recordings and recording keys. A new phone needs your recovery backup to read existing recordings.")
            .setPositiveButton("Got it", null).show()
    }

    private fun permissions(): Array<String> = if (Build.VERSION.SDK_INT >= 31)
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    private fun requestScan() {
        val missing = permissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) { requestPermissions(missing.toTypedArray(), 100); return }
        if (!client.bluetoothEnabled) {
            try { @Suppress("DEPRECATION") startActivityForResult(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), 101) }
            catch (_: Exception) { showError("Enable Bluetooth in Android settings, then tap Scan.") }
            return
        }
        client.scan()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 100) {
            if (grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) requestScan()
            else showError("Nearby-device permission is needed for Bluetooth. Nothing was connected or recorded.")
        }
    }

    private fun showError(text: String) {
        if (!isFinishing) AlertDialog.Builder(this).setTitle("OpenPendant").setMessage(text).setPositiveButton("OK", null).show()
    }
    override fun onSaveInstanceState(outState: Bundle) {
        outState.putInt("tab", activeTab); outState.putString("libraryQuery", libraryQuery)
        super.onSaveInstanceState(outState)
    }
    override fun onResume() {
        super.onResume()
        foreground = true
        pendantSession.foreground()
        if (uiReady && !client.recording && !transcriptState.busy && !importingModel) durableLibrary.refresh()
    }
    override fun onStop() { foreground=false; mainHandler.removeCallbacks(autoTransferCheck); player.stop(); transcription.cancel(); importCancelled.set(true); pendantSession.background(); super.onStop() }
    override fun onDestroy() { uiReady = false; pendantSession.detach(this); transcription.close(); importCancelled.set(true); modelWorker.shutdownNow(); player.close(); super.onDestroy() }
}
