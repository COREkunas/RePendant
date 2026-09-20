/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include "ble_security.h"
#include "mic_commands.h"
#ifdef OPENPENDANT_LONG_CONTROL
/* Small maintenance interface; avoid importing recorder internals here. */
int recording_runtime_pairing_claim(void);
int recording_runtime_pairing_ready(void);
void recording_runtime_pairing_release(void);
#endif

/* Only the native settings backend writes bond metadata. No key export or
 * arbitrary flash addresses. Explicit physical replacement uses bt_unpair;
 * USB/remote commands cannot replace the owner.
 * The existing 32 KiB storage partition is selected in the application DTS.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_SMP_SC_ONLY));
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_SMP_APP_PAIRING_ACCEPT));
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_SMP_ENFORCE_MITM));
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_BONDING_REQUIRED));
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_SETTINGS));
BUILD_ASSERT(IS_ENABLED(CONFIG_SETTINGS_NVS));
BUILD_ASSERT(CONFIG_BT_MAX_PAIRED == 1 && CONFIG_BT_MAX_CONN == 1);
BUILD_ASSERT(CONFIG_BT_SMP_MIN_ENC_KEY_SIZE == 16);
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_KEYS_OVERWRITE_OLDEST));
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE));
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_USE_DEBUG_KEYS));
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_STORE_DEBUG_KEYS));
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_LOG_SNIFFER_INFO));
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_FIXED_PASSKEY));
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT_APP_PASSKEY));
#define SETTINGS_PARTITION DT_CHOSEN(zephyr_settings_partition)
BUILD_ASSERT(DT_SAME_NODE(SETTINGS_PARTITION, DT_NODELABEL(storage_partition)));
BUILD_ASSERT(DT_REG_ADDR(SETTINGS_PARTITION) == 0xf8000);
BUILD_ASSERT(DT_REG_SIZE(SETTINGS_PARTITION) == 0x8000);
BUILD_ASSERT(DT_SAME_NODE(DT_MEM_FROM_FIXED_PARTITION(SETTINGS_PARTITION),
			  DT_NODELABEL(flash0)));
BUILD_ASSERT(DT_SAME_NODE(DT_MTD_FROM_FIXED_PARTITION(SETTINGS_PARTITION),
			  DT_PARENT(DT_NODELABEL(flash0))));
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(flash0)) == 0);
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(flash0)) == 0x100000);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(flash0), erase_block_size) == 4096);
BUILD_ASSERT(DT_PROP(DT_NODELABEL(flash0), write_block_size) == 4);
BUILD_ASSERT(CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT == 1);
BUILD_ASSERT(CONFIG_SETTINGS_NVS_SECTOR_COUNT == 8);

#define PAIRING_WINDOW_MS 60000
/* Core Specification, Vol 3, Part H: AuthReq bonding field and SC bit. */
#define AUTH_BOND_MASK 0x03u
#define AUTH_BOND_REQUIRED 0x01u
#define AUTH_SC 0x08u
#define IO_KEYBOARD_ONLY 0x02u
#define IO_KEYBOARD_DISPLAY 0x04u

static struct k_spinlock enrollment_lock;
static atomic_t init_result = ATOMIC_INIT(-EAGAIN);
static atomic_t init_called;
static atomic_t owners;
static bool window_open;
static bool closing;
static int64_t window_deadline;
/* One owned reference. Bluetooth calls and reference release occur outside the
 * spinlock; they may immediately invoke another authentication callback. */
static struct bt_conn *pending;
static bool code_valid;
static volatile uint32_t displayed_code;
static atomic_t replacement_result;
static void expire_window(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(expiry_work, expire_window);

static void count_bond(const struct bt_bond_info *info, void *data)
{
	ARG_UNUSED(info);
	(*(unsigned int *)data)++;
}

static unsigned int bond_count(void)
{
	unsigned int count = 0;
	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &count);
	return count;
}

static bool basic_connection(struct bt_conn *conn, struct bt_conn_info *info)
{
	return conn != NULL && bt_conn_get_info(conn, info) == 0 &&
		info->type == BT_CONN_TYPE_LE && info->id == BT_ID_DEFAULT &&
		info->role == BT_CONN_ROLE_PERIPHERAL &&
		info->state == BT_CONN_STATE_CONNECTED;
}

bool pendant_ble_pairing_busy(void)
{
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	/* Even an elapsed window stays busy until its cancellation is processed. */
	bool busy = window_open || pending != NULL || closing;
	k_spin_unlock(&enrollment_lock, key);
	return busy;
}

unsigned int pendant_ble_pairing_remaining_ms(void)
{
	k_spinlock_key_t key=k_spin_lock(&enrollment_lock);
	int64_t left=window_open&&!closing?window_deadline-k_uptime_get():0;
	k_spin_unlock(&enrollment_lock,key);
	return left>0?(unsigned int)left:0;
}

bool pendant_ble_authorized(struct bt_conn *conn)
{
	struct bt_conn_info info;
	if (atomic_get(&init_result) != 0 || atomic_get(&owners) != 1 ||
	    pendant_ble_pairing_busy() || !basic_connection(conn, &info)) {
		return false;
	}
	return info.security.level == BT_SECURITY_L4 &&
		info.security.enc_key_size == 16 &&
		(info.security.flags & BT_SECURITY_FLAG_SC) != 0 &&
		bt_le_bond_exists(BT_ID_DEFAULT, bt_conn_get_dst(conn));
}

/* Called with enrollment_lock held. Keep closing set until all out-of-lock
 * cancellation/ref-release work ends; local open cannot race that tail. */
static struct bt_conn *detach_enrollment_locked(void)
{
	closing = true;
	window_open = false;
	window_deadline = 0;
	code_valid = false;
	displayed_code = 0;
	struct bt_conn *release = pending;
	pending = NULL;
	return release;
}

static void release_enrollment(struct bt_conn *release, bool abort_auth)
{
	(void)k_work_cancel_delayable(&expiry_work);
	if (release != NULL) {
		if (abort_auth) {
			(void)bt_conn_auth_cancel(release);
			(void)bt_conn_disconnect(release, BT_HCI_ERR_AUTH_FAIL);
		}
		bt_conn_unref(release);
	}
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	closing = false;
	k_spin_unlock(&enrollment_lock, key);
}

/* expected == NULL is the explicit local close/expiry path. An old callback
 * cannot close a different pending connection. Native cancellation is bounded
 * and nonblocking; no shell output, waits or settings operations in callbacks.
 */
static void finish_enrollment(struct bt_conn *expected, bool abort_auth,
			      int64_t expired_deadline)
{
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	if ((expected != NULL && pending != expected) || closing ||
	    (expired_deadline != 0 && (window_deadline != expired_deadline ||
				      k_uptime_get() < window_deadline)) ||
	    (!window_open && pending == NULL)) {
		k_spin_unlock(&enrollment_lock, key);
		return;
	}
	struct bt_conn *release = detach_enrollment_locked();
	k_spin_unlock(&enrollment_lock, key);
	release_enrollment(release, abort_auth);
}

static void close_enrollment(struct bt_conn *expected, bool abort_auth)
{
	finish_enrollment(expected, abort_auth, 0);
}

static void expire_window(struct k_work *work)
{
	ARG_UNUSED(work);
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	int64_t deadline = window_deadline;
	int64_t remaining = deadline - k_uptime_get();
	bool active = window_open;
	if (active && remaining > 0) {
		/* Reschedule while holding the state lock, so this wakeup cannot
		 * overwrite the timer belonging to a concurrently reopened window. */
		(void)k_work_reschedule(&expiry_work, K_MSEC(remaining));
	}
	k_spin_unlock(&enrollment_lock, key);
	if (active && remaining <= 0) {
		finish_enrollment(NULL, true, deadline);
	}
}

static enum bt_security_err accept_pairing(struct bt_conn *conn,
				 const struct bt_conn_pairing_feat *const feat)
{
	struct bt_conn_info info;
	if (!basic_connection(conn, &info) || feat == NULL ||
	    (feat->auth_req & AUTH_BOND_MASK) != AUTH_BOND_REQUIRED ||
	    (feat->auth_req & AUTH_SC) == 0 || feat->max_enc_key_size != 16 ||
	    (feat->io_capability != IO_KEYBOARD_ONLY &&
	     feat->io_capability != IO_KEYBOARD_DISPLAY)) {
		return BT_SECURITY_ERR_AUTH_REQUIREMENT;
	}
	/* A known bond may reconnect using its key, but cannot be replaced or
	 * repaired remotely, even by a peer spoofing the same Bluetooth address. */
	if (atomic_get(&init_result) != 0 || atomic_get(&owners) != 0 || bond_count() != 0) {
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	bool allow = window_open && !closing && pending == conn &&
		k_uptime_get() < window_deadline &&
		!mic_commands_busy() && !pendant_recovery_is_pending();
	k_spin_unlock(&enrollment_lock, key);
	return allow ? BT_SECURITY_ERR_SUCCESS : BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

static void show_passkey(struct bt_conn *conn, unsigned int passkey)
{
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	bool allow = atomic_get(&init_result) == 0 && window_open && !closing &&
		pending == conn && k_uptime_get() < window_deadline &&
		!mic_commands_busy() && !pendant_recovery_is_pending() && passkey <= 999999;
	if (allow) {
		displayed_code = passkey;
		code_valid = true;
	}
	k_spin_unlock(&enrollment_lock, key);
	if (!allow) {
		close_enrollment(conn, true);
		(void)bt_conn_auth_cancel(conn);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}
	/* Intentionally no LOG/printk/shell output here. Only a local status request
	 * may reveal the native random passkey. No fixed application passkey. */
}

static void authentication_cancelled(struct bt_conn *conn)
{
	close_enrollment(conn, false);
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	unsigned int count = bond_count();
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	bool expected = window_open && !closing && pending == conn && code_valid &&
		k_uptime_get() < window_deadline && bonded && count == 1;
	struct bt_conn *release = NULL;
	if (expected) {
		/* Success and timeout compete for this same lock. Commit the owner
		 * and close/detach the window as ONE state transition. */
		atomic_set(&owners, 1);
		release = detach_enrollment_locked();
	} else {
		atomic_set(&init_result, -EACCES);
	}
	k_spin_unlock(&enrollment_lock, key);
	/* Native bonded=true means negotiated bonding, not a verified durable NVS
	 * commit: this SDK does not propagate bt_keys_store() errors here. A later
	 * reboot/reconnect test is required to validate actual key persistence. */
	if (expected) {
		release_enrollment(release, false);
		if (!pendant_ble_authorized(conn)) {
			(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		}
	} else {
		/* Do not erase a key behind the user's back if completion raced timeout.
		 * Fail closed for this boot; local intervention is required. */
		close_enrollment(conn, false);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(reason);
	close_enrollment(conn, false);
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static const struct bt_conn_auth_cb authentication_callbacks = {
	.pairing_accept = accept_pairing,
	.passkey_display = show_passkey,
	.cancel = authentication_cancelled,
};
static struct bt_conn_auth_info_cb authentication_info = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static void connected(struct bt_conn *conn, uint8_t error)
{
	struct bt_conn_info info;
	if (error != 0) { return; }
	if (atomic_get(&init_result) != 0 || !basic_connection(conn, &info)) {
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}
	bool known = atomic_get(&owners) == 1 &&
		bt_le_bond_exists(BT_ID_DEFAULT, bt_conn_get_dst(conn));
	if (!known) {
		struct bt_conn *reference = bt_conn_ref(conn);
		k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
		bool allow = atomic_get(&owners) == 0 && window_open && !closing &&
			pending == NULL && k_uptime_get() < window_deadline &&
			!mic_commands_busy() && !pendant_recovery_is_pending();
		if (allow) { pending = reference; }
		k_spin_unlock(&enrollment_lock, key);
		if (!allow) {
			bt_conn_unref(reference);
			(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
			return;
		}
	}
	/* Never FORCE_PAIR: a missing key must not silently replace the owner. */
	int err = bt_conn_set_security(conn, BT_SECURITY_L4);
	if (err != 0 && err != -EALREADY) {
		close_enrollment(conn, true);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	close_enrollment(conn, false);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err error)
{
	if (error != BT_SECURITY_ERR_SUCCESS || level != BT_SECURITY_L4) {
		close_enrollment(conn, true);
		(void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
	}
}

BT_CONN_CB_DEFINE(security_connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

int pendant_ble_security_init(void)
{
	if (!atomic_cas(&init_called, 0, 1)) { return -EALREADY; }
	int err = bt_conn_auth_cb_register(&authentication_callbacks);
	if (err == 0) { err = bt_conn_auth_info_cb_register(&authentication_info); }
	if (err == 0) { err = settings_load(); }
	if (err == 0) {
		unsigned int count = bond_count();
		if (count > 1) { err = -EOVERFLOW; }
		else { atomic_set(&owners, count); }
	}
	atomic_set(&init_result, err);
	return err;
}

#ifdef OPENPENDANT_LONG_CONTROL
static int count_saved_bond(const char *name,size_t len,settings_read_cb reader,
			    void *arg,void *user)
{
	/* Enumerate metadata only. Never request/read/log any secret bond bytes. */
	ARG_UNUSED(name);ARG_UNUSED(reader);ARG_UNUSED(arg);
	if(len)(*(unsigned int *)user)++;
	return 0;
}
#endif

int pendant_ble_pairing_replace_local(void)
{
#ifndef OPENPENDANT_LONG_CONTROL
	return -ENOTSUP;
#else
	if(atomic_get(&init_result)!=0)return -EACCES;
	k_spinlock_key_t key=k_spin_lock(&enrollment_lock);
	if(window_open||pending||closing||mic_commands_busy()||pendant_recovery_is_pending()){
		k_spin_unlock(&enrollment_lock,key);return -EBUSY;
	}
	/* Publish refusal before claiming the recorder gate. This two-way barrier
	 * excludes new mic/storage work and authorization while old links retire. */
	closing=true;
	k_spin_unlock(&enrollment_lock,key);
	int rc=recording_runtime_pairing_claim();
	if(rc){
		key=k_spin_lock(&enrollment_lock);closing=false;atomic_set(&replacement_result,rc);
		k_spin_unlock(&enrollment_lock,key);return rc;
	}
	/* Zephyr disconnects the old peer and deletes only native BT metadata.
	 * Its return does NOT report every settings deletion failure, so verify
	 * both RAM and persistent key-entry absence before opening enrollment. */
	if(!recording_runtime_pairing_ready()){
		key=k_spin_lock(&enrollment_lock);closing=false;atomic_set(&replacement_result,-EPERM);
		k_spin_unlock(&enrollment_lock,key);recording_runtime_pairing_release();return -EPERM;
	}
	rc=bt_unpair(BT_ID_DEFAULT,NULL);
	unsigned int saved=0;
	if(!rc)rc=settings_load_subtree_direct("bt/keys",count_saved_bond,&saved);
	if(!rc&&(saved||bond_count()))rc=-EIO;
	key=k_spin_lock(&enrollment_lock);
	if(!rc){
		atomic_set(&owners,0);
		window_open=true;window_deadline=k_uptime_get()+PAIRING_WINDOW_MS;
		code_valid=false;displayed_code=0;
		rc=k_work_reschedule(&expiry_work,K_MSEC(PAIRING_WINDOW_MS));
		if(rc>=0)rc=0;
	}
	if(rc){
		/* Ambiguous deletion/scheduling is terminal for this boot. Never
		 * automatically repeat a destructive operation or trust an old key. */
		atomic_set(&init_result,rc);window_open=false;window_deadline=0;
	}
	atomic_set(&replacement_result,rc);closing=false;
	k_spin_unlock(&enrollment_lock,key);
	recording_runtime_pairing_release();
	return rc;
#endif
}

static int command_open(const struct shell *sh, size_t argc, char **argv)
{
	if (sh != shell_backend_uart_get_ptr() || argc != 2 ||
	    strcmp(argv[1], "confirm") != 0) { return -EINVAL; }
	if (atomic_get(&init_result) != 0) { return -EACCES; }
	if (atomic_get(&owners) != 0 || bond_count() != 0) {
		shell_print(sh, "PAIRING_REFUSED owner_already_bonded; remote replacement disabled");
		return -EALREADY;
	}
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	if (window_open || pending != NULL || closing || mic_commands_busy() ||
	    pendant_recovery_is_pending()) {
		k_spin_unlock(&enrollment_lock, key);
		return -EBUSY;
	}
	window_open = true;
	window_deadline = k_uptime_get() + PAIRING_WINDOW_MS;
	code_valid = false;
	displayed_code = 0;
	int err = k_work_reschedule(&expiry_work, K_MSEC(PAIRING_WINDOW_MS));
	k_spin_unlock(&enrollment_lock, key);
	if (err < 0) { close_enrollment(NULL, true); return err; }
	shell_print(sh, "PAIRING_OPEN seconds=60; connect phone, then request pairing status for the passkey; microphone remains off");
	return 0;
}

static int command_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);
	if (sh != shell_backend_uart_get_ptr() || argc != 1) { return -EINVAL; }
	expire_window(NULL);
	k_spinlock_key_t key = k_spin_lock(&enrollment_lock);
	int64_t remaining = window_open ? MAX(0, window_deadline - k_uptime_get()) : 0;
	bool active = window_open && remaining > 0;
	bool available = active && code_valid;
	bool connected_pending = pending != NULL;
	volatile uint32_t code = available ? displayed_code : 0;
	k_spin_unlock(&enrollment_lock, key);
	shell_print(sh, "PAIRING_STATUS rc=%d bonds=%u open=%u remaining_ms=%u pending=%u code_ready=%u; native bond durability requires reboot/reconnect validation",
		(int)atomic_get(&init_result), (unsigned int)atomic_get(&owners), active,
		(unsigned int)remaining, connected_pending, available);
	if (available) {
		shell_print(sh, "PAIRING_PASSKEY %06u; enter only in your phone's system pairing dialog", (unsigned int)code);
	}
	code = 0;
	return 0;
}

static int command_close(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argv);
	if (sh != shell_backend_uart_get_ptr() || argc != 1) { return -EINVAL; }
	close_enrollment(NULL, true);
	shell_print(sh, "PAIRING_CLOSED; pending authentication cancelled; existing bond preserved");
	return 0;
}

static int command_replacement(const struct shell *sh,size_t argc,char **argv)
{
	ARG_UNUSED(argv);
	if(sh!=shell_backend_uart_get_ptr()||argc!=1)return -EINVAL;
	/* Separate cached diagnostic keeps existing status/recovery parsers intact. */
	shell_print(sh,"PAIRING_REPLACEMENT rc=%d; physical five-tap only",(int)atomic_get(&replacement_result));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(pairing_subcommands,
	SHELL_CMD_ARG(open, NULL, "Open one local 60-second pairing window: open confirm", command_open, 2, 0),
	SHELL_CMD_ARG(status, NULL, "Show pairing state and active passkey locally", command_status, 1, 0),
	SHELL_CMD_ARG(close, NULL, "Cancel pairing without deleting an existing bond", command_close, 1, 0),
	SHELL_CMD_ARG(replacement, NULL, "Read last physical replacement result; no changes", command_replacement, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pairing, &pairing_subcommands, "Local authenticated phone enrollment", NULL);
