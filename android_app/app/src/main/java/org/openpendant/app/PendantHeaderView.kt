package org.openpendant.app

import android.app.AlertDialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

/** Compact reference-matched strip. Display-only changes; actions remain gated by the host. */
internal class PendantHeaderView(private val activity: Context, connect: () -> Unit, sync: () -> Unit) {
    private val ink = Color.rgb(246,247,242)
    private val muted = Color.rgb(142,152,166)
    private val green = Color.rgb(217,255,83)
    private val cyan = Color.rgb(112,229,255)
    private val warning = Color.rgb(255,118,87)
    private val meterHeight = dp(maxOf(28, (18 * activity.resources.configuration.fontScale).toInt()))
    val view = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL; gravity = Gravity.TOP; tag = "pendant-header"
        setPadding(dp(8),dp(4),dp(8),dp(4)); setBackgroundColor(Color.rgb(16,19,24))
    }
    private val state = text(10f).apply { gravity=Gravity.CENTER }
    private val stateIcon = image(R.drawable.ic_status_dot)
    private val date = text(9f).apply {
        tag="header-date"
        setCompoundDrawablesRelative(activity.getDrawable(R.drawable.ic_sync)?.mutate()?.apply {
            setTint(muted);setBounds(0,0,dp(10),dp(10))
        },null,null,null)
        compoundDrawablePadding=dp(3)
    }
    private val connectButton = icon(R.drawable.ic_link, "Connect pendant", connect)
    private val syncButton = icon(R.drawable.ic_sync, "Sync recordings", sync)
    private val batteryIcon = BatteryMeter()
    private val storageIcon = image(R.drawable.ic_storage_database)
    private val battery = text(14f, true)
    private val storage = text(14f, true)
    private val storageHint = text(9f).apply { gravity=Gravity.CENTER }
    private var current: PendantHeaderPresentation? = null
    private var currentState = "State unavailable"
    private var currentStateLive = false
    private var linked: Boolean? = null
    private val statusColumn = column().apply {
        addView(LinearLayout(activity).apply {
            gravity=Gravity.CENTER;minimumHeight=meterHeight
            addView(stateIcon,LinearLayout.LayoutParams(dp(18),dp(18)))
        },LinearLayout.LayoutParams(-1,-2))
        addView(state,LinearLayout.LayoutParams(-1,-2))
    }
    private val batteryColumn = column().apply { addView(metric(batteryIcon,battery,false));addView(date,LinearLayout.LayoutParams(-1,-2)) }
    private val storageColumn = column().apply { addView(metric(storageIcon,storage));addView(storageHint,LinearLayout.LayoutParams(-1,-2)) }
    init {
        view.addView(statusColumn,LinearLayout.LayoutParams(dp(36),-2).apply { marginEnd=dp(4) })
        view.addView(batteryColumn,LinearLayout.LayoutParams(0,-2,1.4f).apply { marginEnd=dp(4) })
        view.addView(storageColumn,LinearLayout.LayoutParams(0,-2,1f).apply { marginEnd=dp(4) })
        view.addView(connectButton);view.addView(syncButton)
        for(column in listOf(statusColumn,batteryColumn,storageColumn)) {
            column.minimumHeight=dp(48);column.isClickable=true;column.isFocusable=true
            column.setOnClickListener { showDetails() }
        }
    }
    private fun metric(icon:View,value:TextView,center:Boolean=true) = LinearLayout(activity).apply {
        gravity=Gravity.CENTER_VERTICAL or if(center)Gravity.CENTER_HORIZONTAL else Gravity.START;minimumHeight=meterHeight
        addView(icon,LinearLayout.LayoutParams(dp(26),dp(26)).apply { marginEnd=dp(3) })
        addView(value,LinearLayout.LayoutParams(-2,-2))
    }
    fun render(value: PendantHeaderPresentation, connected: Boolean, connecting: Boolean,
        canConnect: Boolean, canSync: Boolean, detailsOnly: Boolean, syncing: Boolean, activeState: String? = null) {
        current=value;currentState=activeState ?: value.activity;currentStateLive=activeState!=null || value.live
        val stateLabel=when {
            connecting -> "Link…"
            !connected -> "Offline"
            !value.live && activeState==null -> "Last"
            syncing -> "Sync"
            currentState.startsWith("Recording") && !currentState.contains("needs attention") -> "Rec"
            currentState.startsWith("Preparing") -> "Start"
            currentState.startsWith("Saving") -> "Save"
            currentState in setOf("Idle","Ready to record","Saved on pendant") -> "Idle"
            currentState=="Storage full" -> "Full"
            currentState=="Busy" -> "Busy"
            else -> "Check"
        }
        state.stableText=stateLabel
        stateIcon.imageTintList=ColorStateList.valueOf(when {
            !connected -> muted
            connecting || !currentStateLive -> cyan
            currentState.contains("attention") || currentState=="Storage full" -> warning
            else -> green
        })
        statusColumn.contentDescription="${value.connection}. ${if(currentStateLive)"" else "Last known state: "}$currentState. Tap for details."
        statusColumn.tooltipText=statusColumn.contentDescription
        date.stableText=value.readAt?.let(HeaderDateFormat::format) ?: "Not read yet"
        date.contentDescription=value.readAt?.let { (if(value.live)"Read " else "Last read ")+HeaderDateFormat.format(it) } ?: "No saved reading"
        date.tooltipText=date.contentDescription
        battery.stableText=value.batteryText
        storage.stableText=value.storage?.let { "${it.percent}%" } ?: "—"
        storageHint.stableText=if(value.storage==null)"Not read yet" else value.storageSize
        battery.setTextColor(if(value.batteryLive)green else muted)
        val storageColor=if(value.storageLive)cyan else muted
        storage.setTextColor(storageColor);storageIcon.imageTintList=ColorStateList.valueOf(storageColor)
        batteryIcon.update(value.battery?.percent,if(!value.batteryLive)muted else if((value.battery?.percent ?: 100)<25)warning else green)
        batteryColumn.contentDescription="Battery ${value.batteryText}. ${if(value.batteryLive)"Current estimate" else "Last known estimate"}. ${date.contentDescription}. Tap for details."
        storageColumn.contentDescription="Storage ${value.storageText}, ${value.storageSize}. ${if(value.storageLive)"Current reading" else "Last known reading"}. Tap for details."
        batteryColumn.tooltipText=batteryColumn.contentDescription;storageColumn.tooltipText=storageColumn.contentDescription
        val hasLink=connected||connecting
        if(linked!=hasLink){linked=hasLink;connectButton.setImageResource(if(hasLink)R.drawable.ic_unlink else R.drawable.ic_link)}
        connectButton.contentDescription=if(connecting)"Cancel pendant connection" else if(connected)"Disconnect pendant" else "Connect pendant"
        connectButton.tooltipText=connectButton.contentDescription
        connectButton.isEnabled=canConnect;connectButton.alpha=if(canConnect)1f else .35f
        syncButton.contentDescription=if(syncing)"Pendant sync in progress" else if(detailsOnly)"Sync recording details" else "Sync recordings"
        syncButton.tooltipText=syncButton.contentDescription
        syncButton.isEnabled=canSync;syncButton.alpha=if(canSync)1f else .35f
    }
    private fun showDetails() {
        val value=current ?: return
        AlertDialog.Builder(activity).setTitle("Pendant readings").setMessage(buildString {
            append(value.connection).append("\n")
            append(if(currentStateLive)"" else "Last known state: ").append(currentState)
            value.readAt?.let { append("\nLast telemetry: ").append(HeaderDateFormat.format(it)) }
            append("\n\nBattery: ").append(value.batteryText)
            value.battery?.let { append("\nGauge sample: ").append(HeaderDateFormat.format(it.readAt)) }
            append("\nBattery percentage is an estimate. Charging state is not reported.")
            append("\n\nStorage: ").append(value.storageText).append("\n").append(value.storageSize)
            value.storage?.let {
                append("\nRecording spaces: ${it.entriesUsed} / ${it.entriesTotal}\nRead: ${HeaderDateFormat.format(it.readAt)}")
                if(it.entriesUsed==it.entriesTotal)append("\nRecording entry limit reached, even if byte space remains.")
            }
            append("\nEnabled recording allocation, not raw chip capacity. Cached values are not live measurements.")
        }).setPositiveButton("Close",null).show()
    }
    private fun column()=LinearLayout(activity).apply { orientation=LinearLayout.VERTICAL;gravity=Gravity.TOP }
    private fun dp(n:Int)=(n*activity.resources.displayMetrics.density).toInt()
    private fun text(size:Float,bold:Boolean=false)=TextView(activity).apply {
        textSize=size;setTextColor(if(bold)ink else muted);includeFontPadding=false
        typeface=Typeface.create("sans-serif",if(bold)Typeface.BOLD else Typeface.NORMAL)
        setPadding(0,dp(1),0,dp(1))
    }
    private fun image(resource:Int)=ImageView(activity).apply {
        setImageResource(resource);scaleType=ImageView.ScaleType.FIT_CENTER
        importantForAccessibility=View.IMPORTANT_FOR_ACCESSIBILITY_NO
    }
    private fun icon(resource:Int,description:String,action:()->Unit)=ImageButton(activity).apply {
        setImageResource(resource);imageTintList=ColorStateList.valueOf(ink)
        contentDescription=description;tooltipText=description
        background=GradientDrawable().apply { setColor(Color.rgb(23,27,33));cornerRadius=dp(9).toFloat() }
        setPadding(dp(12),dp(12),dp(12),dp(12));setOnClickListener { action() }
        layoutParams=LinearLayout.LayoutParams(dp(48),dp(48)).apply { marginStart=dp(2) }
    }
    /** Official battery outline with a data-driven charge fill inside its cavity. */
    private inner class BatteryMeter:View(activity) {
        private val outline=checkNotNull(activity.getDrawable(R.drawable.ic_header_battery)).mutate()
        private val paint=Paint(Paint.ANTI_ALIAS_FLAG)
        private var percent:Int?=null;private var tint=muted
        init { importantForAccessibility=View.IMPORTANT_FOR_ACCESSIBILITY_NO;outline.setTint(muted) }
        fun update(value:Int?,color:Int){if(percent!=value||tint!=color){percent=value;tint=color;outline.setTint(color);invalidate()}}
        override fun onDraw(canvas:Canvas){
            super.onDraw(canvas);val save=canvas.save();canvas.scale(width/24f,height/24f)
            paint.color=tint;percent?.let { canvas.drawRect(4f,8f,4f+12f*it/100f,16f,paint) }
            outline.setBounds(0,0,24,24);outline.draw(canvas);canvas.restoreToCount(save)
        }
    }
}
