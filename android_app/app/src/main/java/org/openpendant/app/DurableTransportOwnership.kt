package org.openpendant.app

import java.util.UUID

class DurableTransportBusyException : IllegalStateException("The shared pendant transport is already owned")

/** One gate per actual GATT/control channel, shared by durable sync, capture,
 * telemetry and every other request producer. No Android/BLE call or wait is
 * performed under this short monitor. A lease owns RADIO admission only, never
 * a recording coordinator, key or file. Separate gate instances do not exclude
 * each other: the actual PendantClient integration must inject this same gate.
 */
class DurableTransportOwnership {
    private val monitor=Any()
    private var owner:DurableTransportLease?=null

    fun acquire(epoch:UUID):DurableTransportLease = synchronized(monitor) {
        require(validOwnedUuid(epoch))
        if(owner!=null) throw DurableTransportBusyException()
        DurableTransportLease(this,epoch).also { owner=it }
    }

    internal fun <T> locked(action:()->T):T = synchronized(monitor) { action() }
    internal fun isIdle():Boolean=locked { owner==null }
    internal fun owns(lease:DurableTransportLease)=owner===lease
    internal fun release(lease:DurableTransportLease) { check(owner===lease);owner=null }
}

/** Retiring prohibits new requests immediately. A pending radio request keeps
 * exclusive ownership until its adapter explicitly proves all response/write
 * callbacks finished OR the cancellation/disconnect lifecycle is quiescent.
 * Catching an exception, timing out or setting a cancel flag is NOT that proof.
 */
class DurableTransportLease internal constructor(private val owner:DurableTransportOwnership,val epoch:UUID) {
    private var retiring=false
    private var waitingForClose=false
    private var pending:DurableTransportRequest?=null
    private var completed:DurableTransportRequest?=null
    fun isActive():Boolean=owner.locked { owner.owns(this) && !retiring }
    fun beginRequest():DurableTransportRequest=owner.locked {
        check(owner.owns(this) && !retiring && pending==null) { "Transport lease is unavailable or has pending callbacks" }
        completed=null
        DurableTransportRequest(this).also { pending=it }
    }
    /** Nonblocking; false means still quarantined by pending radio lifecycle. */
    fun retire():Boolean=owner.locked {
        retiring=true
        if(!owner.owns(this)) return@locked true // Never releases a newer owner.
        if(pending!=null || waitingForClose) return@locked false
        owner.release(this);true
    }
    internal fun complete(request:DurableTransportRequest,success:Boolean):Boolean=owner.locked {
        if(!owner.owns(this) || pending!==request) return@locked false
        request.result=if(success)1 else 2
        pending=null
        completed=request
        if(retiring && !waitingForClose) owner.release(this)
        true
    }
    internal fun successful(request:DurableTransportRequest):Boolean=owner.locked {
        owner.owns(this) && !retiring && pending==null && completed===request && request.result==1
    }
    internal fun pending(request:DurableTransportRequest):Boolean=owner.locked {
        owner.owns(this) && !retiring && pending===request && request.result==0
    }
    /** Fence admission before queueing closure or after irrevocable source/epoch
     * revocation. This alone is NOT revocation/closure proof. Even an idle lease
     * stays fenced until the actual client revokes its source and close succeeds. */
    internal fun revokeForTransportClose():Boolean=owner.locked {
        if(!owner.owns(this)) return@locked false
        retiring=true;waitingForClose=true;true
    }
    /** Adapter-only local-client closure proof, AFTER revoking its source/epoch.
     * This is NOT proof of remote operation cancellation or durable receipt.
     * Includes a request admitted by a worker but not yet submitted to main. */
    internal fun cancelAfterTransportClosed():Boolean=owner.locked {
        if(!owner.owns(this) || !waitingForClose) return@locked false
        pending?.result=2;pending=null;completed=null
        waitingForClose=false;retiring=true;owner.release(this);true
    }
}

class DurableTransportRequest internal constructor(private val lease:DurableTransportLease) {
    internal var result=0 // Guarded by this request's lease owner monitor.
    /** Adapter only: BOTH successful write and fully validated response have
     * completed, with no outstanding fragments/callbacks. Duplicate/old proof
     * returns false and cannot release or authorize a newer session. */
    fun completed():Boolean=lease.complete(this,true)
    /** Adapter only: cancellation/disconnect actually completed and no callback
     * can still issue a write. Not a request to initiate cancellation. */
    fun cancelled():Boolean=lease.complete(this,false)
    internal fun requirePending() { check(lease.pending(this)) { "Radio request is no longer permitted to submit work" } }
    internal fun requireSuccessful() { check(lease.successful(this)) { "Radio lifecycle has not completed successfully" } }
    internal fun belongsTo(expected:DurableTransportLease):Boolean=lease===expected
}
