package org.openpendant.app

import android.Manifest
import android.annotation.SuppressLint
import android.app.*
import android.bluetooth.*
import android.content.*
import android.content.pm.PackageManager
import android.hardware.usb.*
import android.os.*
import android.view.WindowManager
import android.widget.*
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** Explicit cable-only new-key/reset wizard. Every mutation follows a fresh
 * identity check; power loss or a lost ACK stops for read-only reconciliation.
 * No private key leaves the phone, no phone copy is deleted, no mic is started. */
class KeyResetActivity : Activity() {
    private val main=Handler(Looper.getMainLooper())
    private val worker=Executors.newSingleThreadExecutor()
    private val cancelled=AtomicBoolean()
    private lateinit var session:PendantSession
    private lateinit var manager:UsbManager
    private lateinit var status:TextView
    private lateinit var read:Button
    private lateinit var backup:Button
    private lateinit var restart:Button
    private lateinit var action:Button
    private var claimed=false
    private var busy=false
    private var awaiting=false
    private var registered=false
    private var device:UsbDevice?=null
    private var observed:Observed?=null
    private var plan:KeyResetProtocol.Plan?=null
    private var keyReady=false
    @Volatile private var console:UsbPairingConsole?=null
    private data class Observed(val storage:PhoneMigrationProtocol.Storage,val state:KeyResetProtocol.State)
    private val permissionAction get()="$packageName.KEY_RESET_USB_PERMISSION"
    private val receiver=object:BroadcastReceiver(){
        override fun onReceive(context:Context,intent:Intent){
            if(intent.action!=permissionAction||!awaiting||cancelled.get())return
            awaiting=false
            val live=device?.let { d->manager.deviceList.values.singleOrNull{it.deviceName==d.deviceName&&it.deviceId==d.deviceId} }
            if(live!=null&&manager.hasPermission(live))readState()
            else { status.text="USB access was not granted. Nothing was sent.";render() }
        }
    }
    override fun onCreate(savedInstanceState:Bundle?){
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_SECURE or WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        session=PendantSession.get(applicationContext);manager=getSystemService(UsbManager::class.java)
        val column=LinearLayout(this).apply{orientation=LinearLayout.VERTICAL;val p=(24*resources.displayMetrics.density).toInt();setPadding(p,p,p,p)}
        fun label(text:String,size:Float=16f)=TextView(this).also{it.text=text;it.textSize=size;it.setPadding(0,12,0,12);column.addView(it)}
        fun button(text:String,run:()->Unit)=Button(this).also{it.text=text;it.isAllCaps=false;it.setOnClickListener{run()};column.addView(it)}
        label("New key · delete pendant recordings",24f)
        label("Connect the pendant directly to this phone by USB. This removes recordings from the pendant only. Phone copies, Bluetooth pairing and old backups are kept. Keep the cable connected while erasing.")
        status=label("Read the wired pendant first. Nothing is erased by reading or preparing a backup.")
        read=button("Read wired pendant / refresh"){prepareRead()}
        backup=button("Prepare and verify new key backup…"){
            val old=plan?.old?.recipientFingerprint?:observed?.storage?.binding?.recipientFingerprint?:return@button
            startActivity(Intent(this,RecoveryActivity::class.java).putExtra("keyResetOldFingerprint",old))
        }
        restart=button("Restart pendant, then read again"){
            job("Restarting without recording or changing Bluetooth pairing…"){
                withConsole { ch->val live=snapshot(ch);check(!live.state.busy&&!live.state.fault);ch.request(KeyResetProtocol.Command.Restart) }
                return@job {observed=null;status.text="Restart requested. Wait about 20 seconds, then tap Read wired pendant. If USB permission appears, allow it. No retry was sent."}
            }
        }
        action=button("Continue"){review()}
        button("Back"){finish()}
        setContentView(ScrollView(this).apply{isFillViewport=true;fitsSystemWindows=true;addView(column)})
        claimed=!session.library.busy&&!session.longRecording.busy&&!session.longRecording.ownsRadio&&session.client.beginUsbPairing()
        if(!claimed)status.text="Finish transfer/recording and disconnect Bluetooth before reopening this screen."
        if(Build.VERSION.SDK_INT>=33)registerReceiver(receiver,IntentFilter(permissionAction),RECEIVER_NOT_EXPORTED)
        else @Suppress("DEPRECATION") registerReceiver(receiver,IntentFilter(permissionAction))
        registered=true;render()
    }
    private fun render(){
        val available=claimed&&!busy&&!awaiting&&!cancelled.get()
        val o=observed;val p=plan;val safe=o!=null&&!o.state.busy&&!o.state.fault
        read.isEnabled=available;backup.isEnabled=available&&safe
        restart.isEnabled=available&&safe
        action.text=when{
            o==null->"Read pendant first"
            p!=null&&o.storage.binding==p.next&&o.storage.phase==3->"Finish new-key setup"
            o.storage.phase!=3->"Erase pendant recordings"
            else->"Use new key and delete pendant recordings…"
        }
        action.isEnabled=available&&safe&&keyReady&&o!!.state.fresh
    }
    private fun prepareRead(){
        if(!claimed||busy||awaiting)return
        if(Build.VERSION.SDK_INT>=31&&checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)!=PackageManager.PERMISSION_GRANTED){
            requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_CONNECT),1);return
        }
        val target=manager.deviceList.values.filter{it.vendorId==UsbPairingProtocol.VID&&it.productId==UsbPairingProtocol.PID}.singleOrNull()
        if(target==null){status.text="Connect exactly one pendant directly to this phone with a USB data cable.";return}
        device=target
        if(manager.hasPermission(target))readState()
        else{
            awaiting=true;render()
            try{manager.requestPermission(target,PendingIntent.getBroadcast(this,63,Intent(permissionAction).setPackage(packageName),PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_CANCEL_CURRENT))}
            catch(_:Exception){awaiting=false;status.text="Could not request USB access.";render()}
            main.postDelayed({if(awaiting){awaiting=false;status.text="USB permission timed out. Read again when ready.";render()}},120000)
        }
    }
    override fun onRequestPermissionsResult(code:Int,permissions:Array<out String>,results:IntArray){
        super.onRequestPermissionsResult(code,permissions,results)
        if(code==1&&results.isNotEmpty()&&results.all{it==PackageManager.PERMISSION_GRANTED})prepareRead()
    }
    private fun <T> withConsole(action:(UsbPairingConsole)->T):T{
        check(!cancelled.get());val expected=checkNotNull(device)
        val live=manager.deviceList.values.single{it.deviceName==expected.deviceName&&it.deviceId==expected.deviceId}
        return UsbPairingConsole(manager,live).use{ch->console=ch;try{action(ch)}finally{console=null}}
    }
    @SuppressLint("MissingPermission")
    private fun snapshot(ch:UsbPairingConsole):Observed{
        check(!cancelled.get())
        val identity=UsbPairingProtocol.identity(ch.request(UsbPairingProtocol.Command.IDENTITY))
        UsbPairingProtocol.status(ch.request(UsbPairingProtocol.Command.STATUS)).use{check(it.bonds==1&&!it.open&&!it.pending&&!it.codeReady)}
        check(getSystemService(BluetoothManager::class.java).adapter.bondedDevices.any{it.address==identity.address&&it.bondState==BluetoothDevice.BOND_BONDED})
        val state=KeyResetProtocol.state(ch.request(KeyResetProtocol.Command.Info))
        val storage=PhoneMigrationProtocol.storage(identity.address,ch.request(UsbPairingProtocol.Command.STORAGE_IDENTITY))
        check(state.phase==storage.phase);return Observed(storage,state)
    }
    private fun job(progress:String,run:()->(()->Unit)){
        if(!claimed||busy||cancelled.get())return
        busy=true;status.text=progress;render()
        worker.execute{
            val result=try{check(!cancelled.get());run()}catch(_:Exception){
                {observed=null;status.text="Outcome not confirmed. Do not repeat a reset blindly. Keep USB connected and use Read wired pendant to check its saved state. Existing phone copies and keys are kept. Firmware 0.4.62 or newer is required."}
            }
            main.post{if(!cancelled.get()){busy=false;result();render()}}
        }
    }
    private fun readState()=job("Reading public identity and the saved reset state…"){
        val o=withConsole(::snapshot);check(!o.state.fault)
        val current=AndroidDurableBinding.read(applicationContext)
        val matches=AndroidKeyReset.plans(applicationContext).filter{it.old==o.storage.binding||it.next==o.storage.binding}
        // Prefer a pending child of this ACTIVE parent over the already completed prior reset.
        val p=matches.filter{it.old==o.storage.binding}.singleOrNull()?:matches.singleOrNull()
        check(matches.size<=2 && (matches.size<2||p?.old==o.storage.binding))
        check(current==null||current==o.storage.binding||current==p?.old)
        if(o.storage.phase!=3){checkNotNull(p).checkChild(o.storage,o.state)}
        val old=p?.old?.recipientFingerprint?:o.storage.binding.recipientFingerprint
        val summary=if(p!=null)AndroidRecipientProfiles.selected(applicationContext,p.next.recipientFingerprint).summary()
            else AndroidKeyReset.candidate(applicationContext,old).summary()
        val ready=summary.state==RecipientVaultState.READY&&summary.backupVerified&&summary.fingerprintHex!=old&&
            (p==null||summary.fingerprintHex==p.next.recipientFingerprint)
        val message=when{
            o.state.busy->"An operation is running. Keep USB connected; read again later. Do not restart or erase again."
            !ready->"Prepare the new key's backup, save it and verify it. Then return here and tap Read wired pendant."
            !o.state.fresh->"Restart the pendant once, then tap Read wired pendant. This safely releases its old storage session."
            p!=null&&o.storage.binding==p.next&&o.storage.phase==3->"New storage is ready. Finish setup to enable recording with the new key."
            o.storage.phase==2->"An earlier erase was interrupted. No recording is enabled. You may explicitly erase this same inactive volume again."
            o.storage.phase==1->"New key is prepared. Erase the pendant recordings to finish creating empty storage."
            else->"New key backup verified. The next confirmation removes pendant recordings only; phone copies stay."
        }
        return@job {observed=o;plan=p;keyReady=ready;status.text=message}
    }
    private fun review(){
        val o=observed?:return
        if(!keyReady||!o.state.fresh||o.state.busy||o.state.fault)return
        val p=plan
        if(p!=null&&o.storage.binding==p.next&&o.storage.phase==3){finishSetup(p);return}
        AlertDialog.Builder(this).setTitle("Delete ALL pendant recordings?")
            .setMessage("Use the new key for future recordings. ALL recordings currently on the pendant will be removed. Phone copies and both key backups are kept. This does not record audio. Keep USB connected; the erase may take several minutes.")
            .setNegativeButton("Cancel",null).setPositiveButton("Delete pendant recordings"){_,_->
                if(o.storage.phase==3)prepareReset(o)else erase(checkNotNull(p),o)
            }.show()
    }
    private fun prepareReset(expected:Observed)=job("Saving the new-key reset intent; phone copies stay untouched…"){
        val p=withConsole{ch->
            val live=snapshot(ch);check(live==expected&&live.state.fresh&&!live.state.busy&&!live.state.fault)
            pauseBattery(ch)
            val existing=plan
            val chosen=if(existing!=null){check(existing.old==live.storage.binding);existing}else{
                val recipient=AndroidKeyReset.publicRecipient(applicationContext,live.storage.binding.recipientFingerprint)
                val next=live.storage.binding.copy(volume=live.storage.binding.volume.copy(volumeId=UUID.randomUUID(),generation=live.storage.binding.volume.generation+1),recipientFingerprint=recipient.first)
                KeyResetProtocol.Plan(live.storage.binding,live.state.descriptor,next,recipient.second)
            }
            chosen.next.requireVerifiedRecipient(AndroidRecipientProfiles.selected(applicationContext,chosen.next.recipientFingerprint).summary())
            AndroidKeyReset.save(applicationContext,chosen);check(!cancelled.get())
            val reply=ch.request(KeyResetProtocol.Command.Prepare(chosen));check("RECORDER_QUEUED task=18" in reply)
            chosen
        }
        return@job {plan=p;observed=null;status.text="Preparation sent. Tap Read wired pendant to confirm it, then Restart pendant and read again before erasing. No automatic retry."}
    }
    private fun erase(p:KeyResetProtocol.Plan,expected:Observed)=job("Erasing pendant storage. Keep this screen open and USB connected…"){
        withConsole{ch->
            val live=snapshot(ch);check(live==expected&&live.state.fresh);p.checkChild(live.storage,live.state);check(live.storage.phase in 1..2)
            pauseBattery(ch,allowNeverStarted=true)
            p.next.requireVerifiedRecipient(AndroidRecipientProfiles.selected(applicationContext,p.next.recipientFingerprint).summary())
            check(p in AndroidKeyReset.plans(applicationContext)&&!cancelled.get())
            check("RECORDER_QUEUED task=19" in ch.request(KeyResetProtocol.Command.Erase(live.storage.confirmation)))
            val deadline=SystemClock.elapsedRealtime()+3605000L
            var completed=false
            while(!cancelled.get()&&SystemClock.elapsedRealtime()<deadline){
                Thread.sleep(1000)
                val state=KeyResetProtocol.state(ch.request(KeyResetProtocol.Command.Info))
                check(!state.fault&&state.reset&&state.parent==p.oldDescriptor)
                if(!state.busy){val after=snapshot(ch);p.checkChild(after.storage,after.state);check(after.storage.phase==3);completed=true;break}
            }
            check(completed)
        }
        return@job {observed=null;status.text="Pendant recordings erased and new storage verified. Tap Read, Restart pendant, then Read again and Finish new-key setup. Phone copies and backups were kept."}
    }
    private fun finishSetup(p:KeyResetProtocol.Plan)=job("Verifying empty new-key storage and saving this phone's setup…"){
        withConsole{ch->val live=snapshot(ch);p.checkChild(live.storage,live.state);check(live.storage.phase==3&&live.state.fresh)}
        check(!cancelled.get());AndroidDurableBinding.finishKeyResetExplicit(applicationContext,p.old,p.next)
        return@job {observed=null;status.text="New-key setup complete. Pendant recordings were removed; phone copies and backups remain. Go Back and connect securely over Bluetooth. No audio was recorded."
            setResult(RESULT_OK,Intent().putExtra("pendantAddress",p.next.bondAddress))}
    }
    private fun pauseBattery(ch:UsbPairingConsole,allowNeverStarted:Boolean=false){
        fun stopped():Boolean{
            val lines=ch.request(KeyResetProtocol.Command.Battery).lineSequence().map{it.trimEnd('\r')}.filter{it.startsWith("BATTERY_WATCH ")}.toList()
            check(lines.size==1)
            val fields=lines.single().removePrefix("BATTERY_WATCH ").split(' ').map{it.split('=')}
            check(fields.size==19&&fields.all{it.size==2}&&fields.map{it[0]}.distinct().size==19)
            val values=fields.associate{it[0] to it[1].toLong()}
            check(values["usb"]==1L)
            // An inactive child has no long-control owner, so firmware never
            // auto-starts the gauge. It also cannot start recording on battery.
            if(allowNeverStarted&&values["requested"]==0L){
                check(values["initialized"]==0L&&values["done"]==0L&&values["sequence"]==0L);return true
            }
            check(values["requested"]==1L&&values["initialized"]==1L)
            if(values["done"]==1L){check(values["rc"]==-140L&&values["valid"]==0L);return true}
            check(values["done"]==0L&&values["rc"]==0L&&values["valid"]==1L&&checkNotNull(values["mv"])>=3500)
            return false
        }
        if(stopped())return
        check("BATTERY_WATCH_STOP requested=1" in ch.request(KeyResetProtocol.Command.PauseBattery))
        val deadline=SystemClock.elapsedRealtime()+10000
        while(!cancelled.get()&&SystemClock.elapsedRealtime()<deadline){Thread.sleep(300);if(stopped())return}
        error("Battery monitor did not stop; no destructive command sent")
    }
    override fun onDestroy(){
        cancelled.set(true);console?.close();worker.shutdown();main.removeCallbacksAndMessages(null)
        if(registered)unregisterReceiver(receiver)
        if(claimed)session.client.endUsbPairing()
        super.onDestroy()
    }
}
