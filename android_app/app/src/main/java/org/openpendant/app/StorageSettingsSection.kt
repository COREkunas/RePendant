package org.openpendant.app

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.View
import android.widget.*

/** Separate inventory/review/confirmation. No action on merely opening Settings. */
internal class StorageSettingsSection(private val activity:Activity,private val controller:AndroidDurableLibrary,
    private val client:PendantClient,private val otherBusy:()->Boolean,private val connect:()->Unit) {
    private val ink=Color.rgb(246,247,242)
    private val muted=Color.rgb(162,174,190)
    private var operationRequested=false
    val view=column()
    private val status=text("",14,muted)
    private val connectButton=button("Connect pendant",connect)
    private val checkButton=button("Check recordings") { client.durablePeer()?.let { operationRequested=true;controller.reviewQuickClear(it) } }
    private val clearButton=button("Clear recordings…") { controller.clearReview?.let(::choose) }
    private val resume=button("Finish pending deletions") { client.durablePeer()?.let { operationRequested=true;controller.finishPendingClear(it) } }
    private var dialog:AlertDialog?=null
    init {
        view.setPadding(dp(18),dp(18),dp(18),dp(18))
        view.background=GradientDrawable().apply { setColor(Color.rgb(23,31,41));cornerRadius=dp(22).toFloat() }
        view.addView(text("Storage cleanup",21,ink,true))
        view.addView(text("Quick clear · reuse storage, keep pairing and recording keys. Not a secure erase.",13,muted))
        view.addView(status);view.addView(connectButton);view.addView(checkButton);view.addView(clearButton);view.addView(resume)
        view.addView(text("Works on battery with supported firmware and sufficient charge. Check reads the full recording list without downloading audio. Phone copies stay unless you select their removal.",13,muted))
        render()
    }
    fun render() {
        val state=controller.state
        val pending=state.recordings.count { it.recording.volume==state.binding?.volume && RecordingListPresentation.pending(it) }
        val review=controller.clearReview
        val blocked=controller.busy||otherBusy()||state.needsAttention
        val peer=client.durablePeer()
        val usable=!blocked && controller.syncRefusal(peer)==null
        connectButton.visibility=if(peer==null && review==null && !controller.busy)View.VISIBLE else View.GONE
        connectButton.isEnabled=!blocked&&!client.connecting
        checkButton.isEnabled=usable
        clearButton.isEnabled=!blocked && review!=null && (review.pendantCount>0 || review.phoneCount>0)
        clearButton.visibility=if(review!=null)View.VISIBLE else View.GONE
        resume.visibility=if(pending>0)View.VISIBLE else View.GONE;resume.isEnabled=usable&&pending>0
        status.stableText=when {
            controller.busy -> state.message
            pending>0 -> "$pending pending deletion requests. Connect to finish their original scope; no new recordings will be selected."
            review!=null -> "${review.pendantCount} on pendant · ${review.phoneCount} phone copies"
            operationRequested -> state.message
            state.message.startsWith("Pendant recordings cleared") || state.message.startsWith("Selected removals confirmed") -> state.message
            else -> controller.syncRefusal(peer) ?: "Ready · check recordings before clearing."
        }
    }
    private fun choose(review:QuickClearReview) {
        if(controller.busy||otherBusy()||controller.clearReview!==review)return
        val body=column().apply { setPadding(dp(22),dp(8),dp(22),dp(8)) }
        body.addView(text("Clear all ${review.pendantCount} reviewed pendant recordings. This includes every page, regardless of list filters.",14,ink))
        val phone=CheckBox(activity).apply { tag="clear-phone";text="Also delete phone copies (${review.phoneCount})";setTextColor(ink);minHeight=dp(48);isChecked=false }
        body.addView(phone)
        body.addView(text("Only this pendant’s current storage is included. Other devices, old storage archives, pairing and recovery keys are kept.",13,muted))
        val preview=AlertDialog.Builder(activity).setTitle("Clear pendant recordings").setView(body)
            .setNegativeButton("Cancel",null).setPositiveButton("Review",null).create()
        dialog=preview;preview.show()
        fun enabled(){preview.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled=review.pendantCount>0 || phone.isChecked&&review.phoneCount>0}
        phone.setOnCheckedChangeListener{_,_->enabled()};enabled()
        preview.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            val removePhone=phone.isChecked
            if(controller.busy||otherBusy()||controller.clearReview!==review)return@setOnClickListener
            val plan=review.plan(removePhone)
            if(plan.targets.isEmpty())return@setOnClickListener
            preview.dismiss()
            dialog=AlertDialog.Builder(activity).setTitle("Confirm recording cleanup")
                .setMessage("Pendant: ${review.pendantCount} recordings removed.\nPhone: ${if(removePhone)"${review.phoneCount} copies removed" else "all copies kept"}.\n\nThis cannot be undone. Pairing and recording keys stay. Bluetooth reconnects to confirm removal; if interrupted, pending requests remain for you to finish. New recordings outside this review are not deleted.")
                .setNegativeButton("Cancel",null).setPositiveButton("Clear recordings") { _,_->
                    if(!controller.busy&&!otherBusy()&&controller.clearReview===review){operationRequested=true;controller.quickClear(review,removePhone)}
                }.show()
        }
    }
    private fun dp(n:Int)=(n*activity.resources.displayMetrics.density).toInt()
    private fun column()=LinearLayout(activity).apply { orientation=LinearLayout.VERTICAL }
    private fun text(value:String,size:Int,color:Int,bold:Boolean=false)=TextView(activity).apply {
        text=value;textSize=size.toFloat();setTextColor(color);setPadding(0,dp(6),0,dp(6));if(bold)setTypeface(typeface,Typeface.BOLD)
    }
    private fun button(value:String,action:()->Unit)=Button(activity).apply {
        text=value;isAllCaps=false;setTextColor(ink);textSize=14f;minHeight=dp(48);setOnClickListener{action()}
    }
}
