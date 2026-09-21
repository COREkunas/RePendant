package org.openpendant.app

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** Explicit foreground controls. The pendant, not this Activity/connection,
 * owns an accepted recording. Leaving the app closes monitoring, never STOPs.
 * No startup microphone/connection, private-key export, sync or deletion. */
internal class AndroidLongRecordingController(context: Context,private val client: PendantClient,
    private val changed: () -> Unit) {
    private val context=context.applicationContext
    private val main=Handler(Looper.getMainLooper())
    private val worker=Executors.newSingleThreadExecutor()
    private val working=AtomicBoolean()
    private val visible=AtomicBoolean()
    @Volatile private var session: LongRecordingControl.Session?=null
    private var store: LongRecordingIntentJournal?=null // worker-only
    private var peer: LongRecordingPeer?=null // worker-only
    private var polling: Runnable?=null
    @Volatile var observation: LongRecordingObservation?=null
        private set
    @Volatile var observedAt=0L
        private set
    @Volatile var message="Long recording. Check status for power capability; nothing starts automatically."
        private set
    val busy get()=working.get()
    val ownsRadio get()=session!=null
    val activeRecording get()=observation?.state?.phase in setOf(LongRecordingControlCodec.Phase.STARTING,
        LongRecordingControlCodec.Phase.RUNNING,LongRecordingControlCodec.Phase.STOPPING,LongRecordingControlCodec.Phase.DRAINING)
    fun foreground(){visible.set(true)}
    fun background(){
        visible.set(false);polling?.let(main::removeCallbacks);polling=null
        session?.close();session=null
        if(activeRecording)message="Monitoring paused. The pendant may still be recording. Reconnect and check status."
    }
    fun checkStatus()=run(Action.STATUS)
    fun startExplicit()=run(Action.START)
    fun armButtonExplicit()=run(Action.ARM)
    fun stopExplicit()=run(Action.STOP)
    private enum class Action { STATUS,START,ARM,STOP }
    private fun publish(value: LongRecordingObservation) {
        observation=value;observedAt=SystemClock.elapsedRealtime()
        message=when {
            value.outcome==LongRecordingOutcome.BOOT_CHANGED -> "Pendant restarted. Previous recording outcome needs recovery verification; no new Start was sent."
            value.state?.phase==LongRecordingControlCodec.Phase.STOPPED ->
                (if(value.state.reason==2) "Saved after a battery/power check stop. " else "Saved on pendant. ")+
                "Sync recordings to copy it to your phone when power checks allow."
            value.state?.phase==LongRecordingControlCodec.Phase.NO_CAPACITY -> "No recording space available. Nothing was recorded."
            value.state?.phase==LongRecordingControlCodec.Phase.CANCELLED_BEFORE_START -> "Cancelled before microphone capture."
            value.state?.phase==LongRecordingControlCodec.Phase.FAULT -> "Recording fault. Do not assume all audio was saved."
            value.state?.has(LongRecordingControlCodec.STOP_REQUESTED)==true -> "Stop requested. Waiting for capture and storage to finish."
            value.state?.has(LongRecordingControlCodec.BUTTON_ARMED)==true -> "Armed for one start. Briefly tap the pendant within two minutes. Microphone is still off."
            value.state?.phase==LongRecordingControlCodec.Phase.STARTING -> "Preparing recording storage. "+
                (if(value.state.has(LongRecordingControlCodec.PORTABLE)) "Checking battery power." else "Keep USB power connected.")
            value.state?.phase==LongRecordingControlCodec.Phase.RUNNING -> "Recording on pendant. Bluetooth may disconnect. "+
                (if(value.state.has(LongRecordingControlCodec.PORTABLE)) "Battery-powered recording supported." else "Keep USB power connected.")
            value.state?.phase in setOf(LongRecordingControlCodec.Phase.STOPPING,LongRecordingControlCodec.Phase.DRAINING) -> "Saving the remaining audio…"
            else -> if(value.state?.has(LongRecordingControlCodec.PORTABLE)==true)
                "Portable recording available. Start and sync require their fresh power checks."
                else "Ready for an explicit Start. USB power must remain connected."
        }
    }
    private fun run(action: Action) {
        check(Looper.myLooper()===main.looper)
        if(client.preferencesSave.busy){message="Wait for pendant settings to finish saving.";changed();return}
        if(!visible.get()||!working.compareAndSet(false,true))return
        val powerBlock=RecordingPowerStatus.blocked(client.telemetry,SystemClock.elapsedRealtime(),client.connected)
        if(action in setOf(Action.START,Action.ARM) && powerBlock!=null){
            working.set(false);message="$powerBlock. Nothing was sent.";changed();return
        }
        val selected=client.longPeer()
        if(selected==null){
            // A dead monitoring session must not block connection-only recovery.
            // This only retires its old epoch; never STOPs or replays a Start.
            polling?.let(main::removeCallbacks);polling=null
            session?.close();session=null
            working.set(false);message="Connection interrupted. Recording on the pendant is unchanged; check status after reconnecting.";changed();return
        }
        val stopSelection=observation?.takeIf { it.outcome==LongRecordingOutcome.OBSERVED }?.state
        polling?.let(main::removeCallbacks);polling=null
        changed()
        worker.execute {
            var setupChecked = false
            try {
                check(visible.get())
                val binding=checkNotNull(AndroidDurableBinding.read(context))
                check(selected.bondAddress==binding.bondAddress)
                binding.requireVerifiedRecipient(AndroidDurableBinding.vault(context).summary())
                setupChecked = true
                if(session==null||peer!=selected){
                    session?.close();session=null
                    val intents=AndroidLongRecordingIntents.openOrCreateExplicit(context,binding);store=intents
                    val transport=PendantLongRecordingTransport(client,selected)
                    val core=LongRecordingControl(binding,intents,transport,{
                        check(visible.get()&&AndroidDurableBinding.read(context)==binding)
                        binding.requireVerifiedRecipient(AndroidDurableBinding.vault(context).summary())
                    })
                    session=core.openExplicit();peer=selected
                    publish(checkNotNull(session).discover { !visible.get() })
                }
                val control=checkNotNull(session);val intents=checkNotNull(store)
                val unresolved=intents.read().filterValues { it.unresolved }.keys
                check(unresolved.size<=1)
                when(action){
                    Action.START,Action.ARM -> {
                        if(unresolved.isNotEmpty())publish(control.reconcile(unresolved.single()){!visible.get()})
                        else publish(control.startExplicit(UUID.randomUUID(),action==Action.ARM){!visible.get()})
                    }
                    Action.STATUS -> publish(if(unresolved.isEmpty())control.discover{!visible.get()}
                        else control.reconcile(unresolved.single()){!visible.get()})
                    Action.STOP -> {
                        val id=unresolved.singleOrNull()
                        if(id==null)publish(control.stopObservedExplicit(checkNotNull(stopSelection)){!visible.get()})
                        else {
                            val observed=control.reconcile(id){!visible.get()};publish(observed)
                            if(observed.outcome==LongRecordingOutcome.OBSERVED&&observed.state?.terminal==false&&
                                observed.state.reason==0&&!observed.state.has(LongRecordingControlCodec.STOP_REQUESTED))
                                publish(control.stopExplicit(id){!visible.get()})
                        }
                    }
                }
                if(observation?.outcome==LongRecordingOutcome.BOOT_CHANGED||observation?.state?.terminal==true||
                    observation?.state?.phase==LongRecordingControlCodec.Phase.IDLE){session?.close();session=null}
            }catch(_:Throwable){
                session?.close();session=null
                if (!setupChecked) {
                    message="Recording setup is incomplete or needs attention. Open Settings → Move pendant / change recording key. No recording command was sent; any existing pendant recording is unchanged."
                } else {
                    message="Recording outcome is unknown. Reconnect and check status; Start was not retried. Closing Bluetooth does not stop the pendant."
                    observation=LongRecordingObservation(LongRecordingOutcome.UNKNOWN,observation?.state)
                }
            }finally{
                working.set(false)
                main.post {
                    changed()
                    if(visible.get()&&session!=null){
                        polling=Runnable { run(Action.STATUS) };main.postDelayed(checkNotNull(polling),2000)
                    }
                }
            }
        }
    }
}
