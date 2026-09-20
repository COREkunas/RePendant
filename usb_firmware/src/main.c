/*
 * USB-update development firmware for the Limitless Pendant.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/app_version.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include "mic_commands.h"
#include "mic_diagnostics.h"
#include "ble_audio.h"
#include "ble_security.h"
#ifdef OPENPENDANT_RECORDING_PROFILE
#include "recording_runtime.h"
#include "recording_ble.h"
#include "device_preferences.h"
#ifdef OPENPENDANT_LONG_CONTROL
#include "recording_control_ble.h"
#endif
#else
#include "nand_backup_commands.h"
#include "nand_qualification_commands.h"
#include "crypto_commands.h"
#include "opus_commands.h"
#endif
#include "device_telemetry.h"
#ifndef OPENPENDANT_RECORDING_PROFILE
#include "nand_public_object_commands.h"
#include "nand_read_rate_commands.h"
#include "nand_write_rate_commands.h"
#endif

#define PENDANT_SERVICE_UUID_VAL \
	BT_UUID_128_ENCODE(0x632de001, 0x604c, 0x446b, 0xa80f, 0x7963e950f3fb)
#define PENDANT_WRITE_UUID_VAL \
	BT_UUID_128_ENCODE(0x632de002, 0x604c, 0x446b, 0xa80f, 0x7963e950f3fb)
#define PENDANT_NOTIFY_UUID_VAL \
	BT_UUID_128_ENCODE(0x632de003, 0x604c, 0x446b, 0xa80f, 0x7963e950f3fb)

#define OP_MAGIC_0 'O'
#define OP_MAGIC_1 'P'
#define OP_VERSION 1U
#define OP_HEADER_SIZE 8U
#define OP_RESPONSE_BIT 0x80U

enum op_command {
	OP_CMD_PING = 0x01,
	OP_CMD_GET_INFO = 0x02,
	OP_CMD_GET_BUTTON = 0x03,
	OP_CMD_SET_RGB = 0x04,
};

enum op_status {
	OP_STATUS_OK = 0,
	OP_STATUS_BAD_PACKET = 1,
	OP_STATUS_BAD_VERSION = 2,
	OP_STATUS_UNKNOWN_COMMAND = 3,
	OP_STATUS_BAD_LENGTH = 4,
	OP_STATUS_HARDWARE_ERROR = 5,
};

enum op_capability {
	OP_CAP_BUTTON = BIT(0),
	OP_CAP_RGB = BIT(1),
	OP_CAP_AUDIO = BIT(2),
	OP_CAP_PHYSICAL_CONFIRM = BIT(3),
};

static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(DT_NODELABEL(pendant_button), gpios);
static const struct pwm_dt_spec led_red =
	PWM_DT_SPEC_GET(DT_NODELABEL(pendant_red));
static const struct pwm_dt_spec led_green =
	PWM_DT_SPEC_GET(DT_NODELABEL(pendant_green));
static const struct pwm_dt_spec led_blue =
	PWM_DT_SPEC_GET(DT_NODELABEL(pendant_blue));

static const struct bt_uuid_128 pendant_service_uuid =
	BT_UUID_INIT_128(PENDANT_SERVICE_UUID_VAL);
static const struct bt_uuid_128 pendant_write_uuid =
	BT_UUID_INIT_128(PENDANT_WRITE_UUID_VAL);
static const struct bt_uuid_128 pendant_notify_uuid =
	BT_UUID_INIT_128(PENDANT_NOTIFY_UUID_VAL);

static atomic_t hardware_ready;
static atomic_t hardware_result = ATOMIC_INIT(-EAGAIN);
static atomic_t bluetooth_result = ATOMIC_INIT(-EAGAIN);
static atomic_t usb_result = ATOMIC_INIT(-EAGAIN);
static atomic_t recovery_pending;
#ifdef OPENPENDANT_STANDALONE_CONTROL
K_SEM_DEFINE(button_wake,0,1);
static struct gpio_callback button_callback;
static bool dp_standby_requested;
static void button_edge(const struct device *port,struct gpio_callback *cb,uint32_t pins)
{ARG_UNUSED(port);ARG_UNUSED(cb);ARG_UNUSED(pins);k_sem_give(&button_wake);}
#endif
K_MUTEX_DEFINE(rgb_lock);
static bool rgb_known;
static uint8_t rgb_applied[3];
#ifdef OPENPENDANT_RECORDING_PROFILE
static atomic_t dp_ready,dp_failed,dp_recording,dp_connected,dp_usb,dp_manual_until;
static void dp_copy(uint8_t out[DP_BYTES]);
static int dp_indicator(bool enabled);
static int dp_queue(struct bt_conn *,uint16_t,const uint8_t *,size_t);
#endif

bool pendant_recovery_is_pending(void)
{
	return atomic_get(&recovery_pending) != 0;
}

int pendant_button_read(void)
{
	return atomic_get(&hardware_ready) ? gpio_pin_get_dt(&button) : -ENODEV;
}

static ssize_t write_request(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags);

static int set_channel(const struct pwm_dt_spec *channel, uint8_t level)
{
	uint32_t pulse = (uint32_t)(((uint64_t)channel->period * level) / 255U);

	return pwm_set_pulse_dt(channel, pulse);
}

static int set_rgb_channels(uint8_t red, uint8_t green, uint8_t blue)
{
	int err;

	err = set_channel(&led_red, red);
	if (err != 0) {
		return err;
	}

	err = set_channel(&led_green, green);
	if (err != 0) {
		(void)set_channel(&led_red, 0U);
		return err;
	}

	err = set_channel(&led_blue, blue);
	if (err != 0) {
		(void)set_channel(&led_red, 0U);
		(void)set_channel(&led_green, 0U);
		return err;
	}

	return 0;
}

static int set_rgb_guarded(uint8_t red, uint8_t green, uint8_t blue, bool diagnostic)
{
	int err = k_mutex_lock(&rgb_lock, K_NO_WAIT);
	if (err != 0) { return err; }
	if (!diagnostic && mic_commands_busy()) {
		err = -EBUSY;
	} else {
		err = set_rgb_channels(red, green, blue);
		rgb_known = err == 0;
		if (rgb_known) {
			rgb_applied[0] = red; rgb_applied[1] = green; rgb_applied[2] = blue;
		}
	}
	k_mutex_unlock(&rgb_lock);
	return err;
}

static int set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
	return set_rgb_guarded(red, green, blue, false);
}

int pendant_audio_indicator(bool enabled)
{
#ifdef OPENPENDANT_RECORDING_PROFILE
	return dp_indicator(enabled);
#else
	return set_rgb_guarded(enabled ? 32U : 0U, 0U, 0U, true);
#endif
}

/* Cached software status only. No gauge/NAND read, LED write or mic activation. */
static int read_device_telemetry(uint8_t *reply, size_t size)
{
	struct mic_diagnostics_state mic;
	mic_diagnostics_get_state(&mic);
#ifdef OPENPENDANT_RECORDING_PROFILE
	mic.powered = mic.powered || recording_runtime_microphone_power();
	mic.fault_latched = mic.fault_latched || recording_runtime_faulted();
#endif
	struct device_telemetry state = {
		.uptime_ms = (uint64_t)k_uptime_get(),
		.major = APP_VERSION_MAJOR, .minor = APP_VERSION_MINOR,
		.patch = APP_PATCHLEVEL, .microphone_power = mic.powered,
		.resource_busy = mic_commands_busy(),
		.faults = (atomic_get(&hardware_ready) ? 0U : 1U) |
			(mic.fault_latched ? 2U : 0U),
	};
	if (k_mutex_lock(&rgb_lock, K_NO_WAIT) == 0) {
		state.led_known = rgb_known;
		state.red = rgb_applied[0]; state.green = rgb_applied[1]; state.blue = rgb_applied[2];
		k_mutex_unlock(&rgb_lock);
	}
#ifdef OPENPENDANT_RECORDING_PROFILE
	struct recorder_telemetry recorder;
	recording_runtime_telemetry(&recorder);
	state.resource_busy = state.resource_busy || (recorder.flags & RT_BUSY);
#ifdef OPENPENDANT_PORTABLE_RECORDING
	struct battery_telemetry battery;
	recording_runtime_battery_telemetry(&battery);
	return device_telemetry_encode_battery(&state, &recorder, &battery, reply, size);
#else
	return device_telemetry_encode_recorder(&state, &recorder, reply, size);
#endif
#else
	return device_telemetry_encode(&state, reply, size);
#endif
}

static void cancel_connection_audio(struct bt_conn *conn, void *unused)
{
	ARG_UNUSED(unused);
	ble_audio_disconnected(conn);
#ifdef OPENPENDANT_RECORDING_PROFILE
	recording_ble_disconnected(conn);
#ifdef OPENPENDANT_LONG_CONTROL
	recording_control_ble_disconnected(conn);
#endif
#endif
}

static void notify_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	if (value != BT_GATT_CCC_NOTIFY) {
		bt_conn_foreach(BT_CONN_TYPE_LE, cancel_connection_audio, NULL);
	}
}

BT_GATT_SERVICE_DEFINE(
	pendant_service,
	BT_GATT_PRIMARY_SERVICE(&pendant_service_uuid),
	BT_GATT_CHARACTERISTIC(&pendant_write_uuid.uuid,
		BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE_AUTHEN | BT_GATT_PERM_WRITE_LESC,
		NULL, write_request, NULL),
	BT_GATT_CHARACTERISTIC(&pendant_notify_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_READ_LESC, NULL, NULL, NULL),
	BT_GATT_CCC(notify_changed, BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_READ_LESC |
		BT_GATT_PERM_WRITE_AUTHEN | BT_GATT_PERM_WRITE_LESC));

#ifdef OPENPENDANT_RECORDING_PROFILE
static int durable_authorized(void *unused, struct bt_conn *conn)
{
	ARG_UNUSED(unused);
	return !pendant_recovery_is_pending() && pendant_ble_authorized(conn) &&
		bt_gatt_is_subscribed(conn, &pendant_service.attrs[4], BT_GATT_CCC_NOTIFY) &&
		bt_gatt_get_mtu(conn) >= RECORDING_BLE_MIN_MTU;
}

static void durable_tx_complete(struct bt_conn *conn, void *user_data)
{
	/* The callback may be late: ignore its identity and grant no authority.
	 * The worker independently checks its original epoch and deadline. */
	ARG_UNUSED(conn); ARG_UNUSED(user_data);
	recording_ble_tx_progress();
}

static int durable_notify(void *unused, struct bt_conn *conn, const uint8_t *frame, size_t bytes)
{
	if (!frame || bytes < OP_HEADER_SIZE + 1U || bytes > DB_MAX_RESPONSE ||
	    !durable_authorized(unused, conn) ||
	    bytes > bt_gatt_get_mtu(conn) - 3U ||
	    !recording_ble_reply_allowed(conn, sys_get_le16(&frame[4]))) { return -EPERM; }
	/* This enqueue copies the complete ciphertext/metadata frame. An enqueue
	 * success is not a phone receipt; only explicit durable ACK records that. */
	if (frame[3] == (DB_FULL_STREAM | 0x80U)) {
		struct bt_gatt_notify_params params = {
			.attr = &pendant_service.attrs[4], .data = frame,
			.len = (uint16_t)bytes, .func = durable_tx_complete,
		};
		return bt_gatt_notify_cb(conn, &params);
	}
	return bt_gatt_notify(conn, &pendant_service.attrs[4], frame, bytes);
}

static const struct recording_ble_hooks durable_hooks = {
	.user = NULL, .ready = recording_runtime_sync_ready,
	.authorized = durable_authorized, .submit = recording_runtime_sync_submit,
	.notify = durable_notify, .retire = recording_runtime_sync_retire,
};
#ifdef OPENPENDANT_LONG_CONTROL
static int long_notify(void *u,struct bt_conn *conn,const uint8_t *frame,size_t bytes)
{
 if(!frame||bytes!=LC_RESPONSE_BYTES||!durable_authorized(u,conn)||bytes>bt_gatt_get_mtu(conn)-3U)return -EPERM;
 /* The coordinator's metadata gate may be publishing at this exact recheck.
  * Withhold this reply; the broker may recheck only inside its original budget. */
 if(!recording_control_ble_reply_allowed(conn,sys_get_le16(frame+4)))return -EAGAIN;
 return bt_gatt_notify(conn,&pendant_service.attrs[4],frame,bytes);
}
static const struct recording_control_ble_hooks long_hooks={durable_authorized,long_notify,NULL};
#endif
#endif

static void send_response(struct bt_conn *conn, uint8_t command, uint16_t sequence, uint8_t status,
			  const uint8_t *payload, uint16_t payload_len)
{
	uint8_t response[96];
	uint16_t body_len = payload_len + 1U;

	if (!pendant_ble_authorized(conn) ||
	    !bt_gatt_is_subscribed(conn, &pendant_service.attrs[4], BT_GATT_CCC_NOTIFY) ||
	    body_len > (sizeof(response) - OP_HEADER_SIZE) ||
	    OP_HEADER_SIZE + body_len > bt_gatt_get_mtu(conn) - 3U) {
		ble_audio_disconnected(conn);
		return;
	}

	response[0] = OP_MAGIC_0;
	response[1] = OP_MAGIC_1;
	response[2] = OP_VERSION;
	response[3] = command | OP_RESPONSE_BIT;
	sys_put_le16(sequence, &response[4]);
	sys_put_le16(body_len, &response[6]);
	response[8] = status;
	if (payload_len != 0U) {
		memcpy(&response[9], payload, payload_len);
	}

	int err = bt_gatt_notify(conn, &pendant_service.attrs[4], response,
				 OP_HEADER_SIZE + body_len);
	/* bt_gatt_notify copies into the bounded Bluetooth transport before return.
	 * Do not leave a second plaintext PCM copy on this callback's stack. */
	for (size_t i = 0; i < sizeof(response); ++i) {
		((volatile uint8_t *)response)[i] = 0;
	}
	if (err != 0) { ble_audio_disconnected(conn); }
}

static ssize_t write_request(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *buf,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *request = buf;
	uint16_t sequence = 0U;
	uint16_t payload_len;
	uint16_t request_limit = 80U;
	uint8_t command;
	uint8_t reply[72];
	int value;

	ARG_UNUSED(attr);
	if (!pendant_ble_authorized(conn)) {
		cancel_connection_audio(conn, NULL);
		return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
	}
	if (flags != 0U) { return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED); }

	if (offset != 0U || len < OP_HEADER_SIZE) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	sequence = sys_get_le16(&request[4]);
	command = request[3];
#ifdef OPENPENDANT_RECORDING_PROFILE
	if (durable_ble_is_full(command)) { request_limit = DB_MAX_REQUEST; }
#endif
	/* Full-profile 32-bit offsets add two bytes to ranged requests. */
	if (len > request_limit || (command & OP_RESPONSE_BIT) != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (request[0] != OP_MAGIC_0 || request[1] != OP_MAGIC_1) {
		send_response(conn, command, sequence, OP_STATUS_BAD_PACKET, NULL, 0U);
		return len;
	}

	if (request[2] != OP_VERSION) {
		send_response(conn, command, sequence, OP_STATUS_BAD_VERSION, NULL, 0U);
		return len;
	}

	payload_len = sys_get_le16(&request[6]);
	if (payload_len != (len - OP_HEADER_SIZE)) {
		send_response(conn, command, sequence, OP_STATUS_BAD_LENGTH, NULL, 0U);
		return len;
	}

#ifdef OPENPENDANT_RECORDING_PROFILE
	if ((command >= DB_CATALOG && command <= DB_DELETE) || durable_ble_is_full(command)) {
		/* Refuse oversized replies before any storage job. Full clients use
		 * wide fragments only with capability bit9 and sufficient actual MTU. */
		struct db_request parsed;
		if (durable_ble_parse_request(request, len, &parsed) != DB_OK ||
		    (durable_ble_is_full(command) && (command <= DB_FULL_SEGMENT || command == DB_FULL_STREAM) &&
		     33U + (command == DB_FULL_STREAM ? DB_FULL_MAX_DATA : parsed.maximum) > bt_gatt_get_mtu(conn) - 3U)) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		/* The callback only admits a bounded immutable job. Storage and crypto
		 * run on the recorder worker, never this Bluetooth receive stack. */
		int rc = recording_ble_command(conn, request, len);
		return rc == RB_OK ? (ssize_t)len : BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
#ifdef OPENPENDANT_LONG_CONTROL
	if(command>=LC_STATUS&&command<=LC_STOP){
		int rc=recording_control_ble_command(conn,request,len);
		return rc==0?(ssize_t)len:BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
#endif
#endif

	switch (command) {
	case OP_CMD_PING:
		if (payload_len > 10U) {
			send_response(conn, command, sequence, OP_STATUS_BAD_LENGTH, NULL, 0U);
		} else {
			send_response(conn, command, sequence, OP_STATUS_OK,
				      &request[OP_HEADER_SIZE], payload_len);
		}
		break;

	case OP_CMD_GET_INFO:
		if (payload_len != 0U) {
			send_response(conn, command, sequence, OP_STATUS_BAD_LENGTH, NULL, 0U);
			break;
		}
		reply[0] = 0U; /* Hardware revision not yet identified. */
		reply[1] = OP_VERSION;
		sys_put_le32(OP_CAP_BUTTON | OP_CAP_RGB | OP_CAP_AUDIO | OP_CAP_PHYSICAL_CONFIRM | BIT(4), &reply[2]);
		reply[6] = mic_diagnostics_power_is_on();
#ifdef OPENPENDANT_RECORDING_PROFILE
		reply[6] = reply[6] || recording_runtime_microphone_power();
		if (recording_ble_available()) {
			sys_put_le32(sys_get_le32(&reply[2]) | (recording_runtime_full_storage()?(DB_FULL_CAPABILITY | DB_WIDE_CAPABILITY | DB_STREAM_CAPABILITY | DB_RECEIPT_BATCH_CAPABILITY):DB_CAPABILITY), &reply[2]);
		}
#endif
		reply[7] = 0U; /* Reserved; durable storage uses capability bit 5. */
#ifdef OPENPENDANT_RECORDING_PROFILE
		if(atomic_get(&dp_ready))sys_put_le32(sys_get_le32(&reply[2])|DP_CAPABILITY,&reply[2]);
#endif
#ifdef OPENPENDANT_LONG_CONTROL
		if(recording_runtime_long_available()) {
#ifdef OPENPENDANT_BATTERY_SYNC
            sys_put_le32(sys_get_le32(&reply[2])|BIT(14),&reply[2]);
#endif
#ifdef OPENPENDANT_STANDALONE_CONTROL
			if(atomic_get(&hardware_ready))sys_put_le32(sys_get_le32(&reply[2])|BIT(13),&reply[2]);
#endif
			/* Supported profile remains discoverable while a recording is active.
			 * Runtime admission still excludes sync/short clips during recording. */
			sys_put_le32(sys_get_le32(&reply[2])|LC_CAPABILITY|DB_FULL_CAPABILITY|
				DB_WIDE_CAPABILITY|DB_STREAM_CAPABILITY|DB_RECEIPT_BATCH_CAPABILITY,&reply[2]);
		}
#endif
		send_response(conn, command, sequence, OP_STATUS_OK, reply, 8U);
		break;

	case OP_CMD_GET_BUTTON:
		if (payload_len != 0U) {
			send_response(conn, command, sequence, OP_STATUS_BAD_LENGTH, NULL, 0U);
			break;
		}
		value = atomic_get(&hardware_ready) ? gpio_pin_get_dt(&button) : -ENODEV;
		if (value < 0) {
			send_response(conn, command, sequence, OP_STATUS_HARDWARE_ERROR,
				      NULL, 0U);
		} else {
			reply[0] = value != 0;
			send_response(conn, command, sequence, OP_STATUS_OK, reply, 1U);
		}
		break;

	case DEVICE_TELEMETRY_COMMAND:
		if (payload_len != 0U) {
			send_response(conn, command, sequence, OP_STATUS_BAD_LENGTH, NULL, 0U);
		} else if (read_device_telemetry(reply, sizeof(reply)) != 0) {
			send_response(conn, command, sequence, OP_STATUS_HARDWARE_ERROR, NULL, 0U);
		} else {
			send_response(conn, command, sequence, OP_STATUS_OK, reply, reply[1]);
		}
		break;

#ifdef OPENPENDANT_RECORDING_PROFILE
	case DP_GET:
		if(payload_len||!atomic_get(&dp_ready)||atomic_get(&dp_failed)){
			send_response(conn,command,sequence,OP_STATUS_HARDWARE_ERROR,NULL,0);
		}else{dp_copy(reply);send_response(conn,command,sequence,OP_STATUS_OK,reply,DP_BYTES);}
		break;
	case DP_SET:
		if(dp_queue(conn,sequence,&request[8],payload_len))
			send_response(conn,command,sequence,OP_STATUS_HARDWARE_ERROR,NULL,0);
		break;
#endif
	case OP_CMD_SET_RGB:
		if (payload_len != 3U) {
			send_response(conn, command, sequence, OP_STATUS_BAD_LENGTH, NULL, 0U);
			break;
		}
		value = atomic_get(&hardware_ready)
			? set_rgb(request[8], request[9], request[10])
			: -ENODEV;
#ifdef OPENPENDANT_RECORDING_PROFILE
		if(!value)atomic_set(&dp_manual_until,(atomic_val_t)(k_uptime_get_32()+10000U));
#endif
		send_response(conn, command, sequence,
			      value == 0 ? OP_STATUS_OK : OP_STATUS_HARDWARE_ERROR,
			      NULL, 0U);
		break;

	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13: {
		if (!bt_gatt_is_subscribed(conn, &pendant_service.attrs[4], BT_GATT_CCC_NOTIFY)) {
			ble_audio_disconnected(conn);
			return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
		}
		size_t reply_len = sizeof(reply);
		int status = ble_audio_command(conn, command, &request[8], payload_len, reply, &reply_len);
		send_response(conn, command, sequence, (uint8_t)status, reply, (uint16_t)reply_len);
		for (size_t i = 0; i < sizeof(reply); ++i) { ((volatile uint8_t *)reply)[i] = 0; }
		break;
	}
	default:
		send_response(conn, command, sequence, OP_STATUS_UNKNOWN_COMMAND, NULL, 0U);
		break;
	}

	return len;
}

static const struct bt_data advertising_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, PENDANT_SERVICE_UUID_VAL),
};

static const struct bt_data scan_response_data[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1U),
};
#ifdef OPENPENDANT_RECORDING_PROFILE
#include "device_preferences_runtime.inc"
#endif

static int start_advertising(void)
{
#ifdef OPENPENDANT_RECORDING_PROFILE
	if(k_mutex_lock(&dp_adv_lock,K_NO_WAIT))return -EALREADY;
	struct bt_le_adv_param param=*BT_LE_ADV_CONN_FAST_1;
	param.interval_min=dp_adv_units;param.interval_max=dp_adv_units==48?96:dp_adv_units+16;
	int rc=bt_le_adv_start(&param,advertising_data,ARRAY_SIZE(advertising_data),
		scan_response_data,ARRAY_SIZE(scan_response_data));
	k_mutex_unlock(&dp_adv_lock);return rc;
#else
	return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising_data,
			      ARRAY_SIZE(advertising_data), scan_response_data,
			      ARRAY_SIZE(scan_response_data));
#endif
}

static void restart_advertising(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);
	if (atomic_get(&recovery_pending)) {
		return;
	}

	err = start_advertising();
	atomic_set(&bluetooth_result, err == -EALREADY ? 0 : err);
}

K_WORK_DEFINE(advertising_work, restart_advertising);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
#ifdef OPENPENDANT_RECORDING_PROFILE
	atomic_clear(&dp_connected);
#endif
	cancel_connection_audio(conn, NULL);
}
static void connected(struct bt_conn *conn,uint8_t err)
{
	ARG_UNUSED(conn);
#ifdef OPENPENDANT_RECORDING_PROFILE
	if(!err)atomic_set(&dp_connected,1);
#else
	ARG_UNUSED(err);
#endif
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err != BT_SECURITY_ERR_SUCCESS || level != BT_SECURITY_L4 ||
	    !pendant_ble_authorized(conn)) {
		cancel_connection_audio(conn, NULL);
	}
}

static void connection_recycled(void)
{
	/* Wait until the single connection object is free before advertising. */
	(void)k_work_submit(&advertising_work);
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.recycled = connection_recycled,
};

/*
 * This is a deliberately small, local-USB-only command surface. There is no
 * flash, arbitrary-memory, GPIO-write, or SMP/DFU command in the application.
 * micdiag gates one short USB playback sample. BLE additionally requires
 * authenticated SC pairing and a fresh physical tap for each short clip.
 * Uploads are handled only by MCUboot after explicit recovery entry.
 */
static void reboot_to_recovery(struct k_work *work)
{
	ARG_UNUSED(work);

	/* GPREGRET1 survives the software reset; MCUboot consumes and clears it. */
	sys_reboot(SYS_REBOOT_COLD);
}

K_WORK_DELAYABLE_DEFINE(recovery_work, reboot_to_recovery);

/* Local maintenance reset only. It shares the recovery exclusion flag so no
 * capture, pairing or external NAND reservation can start while it is pending.
 * Unlike recovery, it explicitly verifies the bootloader request is cleared.
 * This exercises reboot recovery; it is NOT a simulated loss of NAND power. */
static int command_restart(const struct shell *sh, size_t argc, char **argv)
{
	if (sh != shell_backend_uart_get_ptr() || argc != 2U ||
	    strcmp(argv[1], "confirm") != 0) { return -EINVAL; }
	if (mic_commands_busy() || pendant_ble_pairing_busy() ||
	    pendant_button_read() != 0 || !atomic_cas(&recovery_pending, 0, 1)) {
		return -EBUSY;
	}
	if (mic_commands_busy() || pendant_ble_pairing_busy() || pendant_button_read() != 0) {
		atomic_clear(&recovery_pending);
		return -EBUSY;
	}
	int err = bootmode_clear();
	if (err == 0) {
		err = bootmode_check(BOOT_MODE_TYPE_BOOTLOADER);
		if (err > 0) { err = -EIO; }
	}
	if (err != 0) {
		atomic_clear(&recovery_pending);
		shell_error(sh, "Normal restart request could not be verified: %d", err);
		return err;
	}
	shell_print(sh, "Restarting application; no upload or NAND command.");
	err = k_work_schedule(&recovery_work, K_MSEC(500));
	if (err < 0) { atomic_clear(&recovery_pending); return err; }
	return 0;
}

static int command_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "OpenPendant USB development base; OP protocol %u", OP_VERSION);
	shell_print(sh, "Application version: %s", APP_VERSION_STRING);
	shell_print(sh, "USB init: %d; button/RGB init: %d; BLE init/advertising: %d",
		    (int)atomic_get(&usb_result),
		    (int)atomic_get(&hardware_result),
		    (int)atomic_get(&bluetooth_result));
#ifdef OPENPENDANT_RECORDING_PROFILE
	shell_print(sh, "Recorder engineering profile: public-key enrollment and exact preserved pool only; USB-powered operations; explicit audio start; no automatic recording or raw diagnostic writes. Use recorder status.");
#else
	shell_print(sh, "Microphones: bounded USB/BLE clips with explicit consent, power %s; NAND: USB fixed diagnostics, raw backup and preimage-bound block1024 qualification; no recording storage, dormant until confirmed; IMU: USB identity-only, dormant until confirmed; persistent settings: BLE bonds only in reserved 32 KiB",
		mic_diagnostics_power_is_on() ? "ON" : "off");
#endif
	shell_print(sh, "Recovery request pending: %s",
		    atomic_get(&recovery_pending) ? "yes" : "no");
	shell_print(sh, "USB commands: pendant status | pendant button | pendant recovery confirm");
	return 0;
}

static int command_telemetry(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	uint8_t bytes[DEVICE_TELEMETRY_RECORDER_SIZE];
	char hex[DEVICE_TELEMETRY_RECORDER_SIZE * 2U + 1U];
	static const char digits[] = "0123456789abcdef";
	if (read_device_telemetry(bytes, sizeof(bytes)) != 0) { return -EIO; }
	for (size_t i = 0; i < bytes[1]; ++i) {
		hex[2U * i] = digits[bytes[i] >> 4]; hex[2U * i + 1U] = digits[bytes[i] & 15];
	}
	hex[bytes[1] * 2U] = 0;
	shell_print(sh, "DEVICE_STATUS v=%u data=%s", bytes[0], hex);
	return 0;
}

static int command_button(const struct shell *sh, size_t argc, char **argv)
{
	int value;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	value = atomic_get(&hardware_ready) ? gpio_pin_get_dt(&button) : -ENODEV;
	if (value < 0) {
		shell_error(sh, "Button unavailable: %d", value);
		return value;
	}

	shell_print(sh, "Button: %s", value ? "pressed" : "released");
	return 0;
}

static int command_recovery(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	if (argc != 2U || strcmp(argv[1], "confirm") != 0) {
		shell_error(sh, "This restarts into MCUboot USB recovery; it does not upload or erase.");
		shell_error(sh, "To proceed, type: pendant recovery confirm");
		return -EINVAL;
	}
	if (mic_commands_busy() || pendant_ble_pairing_busy()) {
		shell_error(sh, "Wait for the bounded microphone test to stop before recovery.");
		return -EBUSY;
	}

	if (!atomic_cas(&recovery_pending, 0, 1)) {
		shell_error(sh, "A recovery restart is already pending.");
		return -EBUSY;
	}
	/* Close the race with a pairing/microphone reservation between the first
	 * check and our CAS. Their queues also recheck recovery after reservation. */
	if (mic_commands_busy() || pendant_ble_pairing_busy()) {
		atomic_clear(&recovery_pending);
		return -EBUSY;
	}

	err = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
	if (err == 0) {
		err = bootmode_check(BOOT_MODE_TYPE_BOOTLOADER);
		if (err != 1) {
			err = err < 0 ? err : -EIO;
		} else {
			err = 0;
		}
	}

	if (err != 0) {
		(void)bootmode_clear();
		atomic_clear(&recovery_pending);
		shell_error(sh, "Recovery request could not be verified: %d; not restarting.", err);
		return err;
	}

	/* Give the response time to leave the CDC ACM transport before reset. */
	shell_print(sh, "Restarting into MCUboot USB recovery. Reopen its USB serial port.");
	err = k_work_schedule(&recovery_work, K_MSEC(500));
	if (err < 0) {
		(void)bootmode_clear();
		atomic_clear(&recovery_pending);
		shell_error(sh, "Restart scheduling failed: %d", err);
		return err;
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	pendant_commands,
	SHELL_CMD_ARG(status, NULL, "Read initialization status and supported commands.",
		      command_status, 1, 0),
	SHELL_CMD_ARG(telemetry, NULL, "Read cached device observations; no peripheral writes.",
		      command_telemetry, 1, 0),
	SHELL_CMD_ARG(button, NULL, "Read the physical button (no GPIO writes).",
		      command_button, 1, 0),
	SHELL_CMD_ARG(recovery, NULL, "With 'confirm', restart into MCUboot USB recovery.",
		      command_recovery, 1, 1),
	SHELL_CMD_ARG(restart, NULL, "With 'confirm', restart application without recovery.",
		      command_restart, 1, 1),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(pendant, &pendant_commands, "OpenPendant USB diagnostics.", NULL);

static int init_hardware(void)
{
	int err;

	if (!gpio_is_ready_dt(&button) || !pwm_is_ready_dt(&led_red) ||
	    !pwm_is_ready_dt(&led_green) || !pwm_is_ready_dt(&led_blue)) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err != 0) {
		return err;
	}

#ifdef OPENPENDANT_STANDALONE_CONTROL
	gpio_init_callback(&button_callback,button_edge,BIT(button.pin));
	err=gpio_add_callback(button.port,&button_callback);
	if(err)return err;
	err=gpio_pin_interrupt_configure_dt(&button,GPIO_INT_EDGE_BOTH);
	if(err)return err;
#endif

	err = set_rgb(0U, 0U, 0U);
	if (err != 0) {
		return err;
	}
	atomic_set(&hardware_ready, 1);
	return 0;
}

/* Callbacks only latch USB lifecycle metadata. None starts a diagnostic
 * or touches NAND. Each independent session rejects lifecycle changes. */
static void pendant_diagnostic_usb_status(enum usb_dc_status_code status,
                                         const uint8_t *param)
{
#ifdef OPENPENDANT_RECORDING_PROFILE
	recording_runtime_usb_status(status, param);
	if(status==USB_DC_CONFIGURED)atomic_set(&dp_usb,1);
	else if(status==USB_DC_DISCONNECTED||status==USB_DC_RESET||status==USB_DC_SUSPEND)atomic_clear(&dp_usb);
#else
	nand_backup_usb_status(status, param);
	nand_qualification_usb_status(status, param);
	nand_public_object_usb_status(status, param);
	nand_read_rate_usb_status(status, param);
	nand_write_rate_usb_status(status, param);
#endif
}

int main(void)
{
	int err;

	/*
	 * Keep USB diagnostics/recovery available even when peripheral or BLE
	 * bring-up fails. Do not wait for a host/DTR: battery-only boot must work.
	 */
	err = usb_enable(pendant_diagnostic_usb_status);
	atomic_set(&usb_result, err);

	err = init_hardware();
	atomic_set(&hardware_result, err);
	(void)mic_commands_init();
#ifndef OPENPENDANT_RECORDING_PROFILE
	crypto_commands_init(); /* Starts waiting workers only; no crypto job at boot. */
	(void)opus_commands_init(); /* Idle supervisor only; no audio or codec work. */
#endif

	err = bt_enable(NULL);
	if (err == 0) { err = pendant_ble_security_init(); }
#ifdef OPENPENDANT_RECORDING_PROFILE
	if (err == 0) { err = recording_ble_init(&durable_hooks); }
#ifdef OPENPENDANT_LONG_CONTROL
	if (err == 0) { err = recording_control_ble_init(&long_hooks); }
#endif
	if (err == 0) { err = recording_runtime_init(); }
	if (err == 0) { dp_initialize(); }
#endif
	if (err == 0) {
		err = start_advertising();
	}
	atomic_set(&bluetooth_result, err);

#ifdef OPENPENDANT_STANDALONE_CONTROL
	uint64_t button_fast_until=0;
#endif
	for (;;) {
#ifdef OPENPENDANT_RECORDING_PROFILE
		dp_process();dp_policy_tick();
#endif
#ifdef OPENPENDANT_LONG_CONTROL
		recording_runtime_long_button(pendant_button_read());
#ifdef OPENPENDANT_STANDALONE_CONTROL
		uint64_t now=(uint64_t)k_uptime_get();
		int standby=recording_runtime_standby(dp_standby_requested&&atomic_get(&hardware_ready)&&now>=button_fast_until);
		/* GPIO wakes the actor immediately. Retain normal debounce sampling for
		 * 1.1 seconds after either edge, including a tap which woke standby. */
		if(!k_sem_take(&button_wake,K_MSEC(standby?250:25))){
			button_fast_until=(uint64_t)k_uptime_get()+1100U;
			(void)recording_runtime_standby(0);
		}
#else
		k_sleep(K_MSEC(25));
#endif
#else
		k_sleep(K_HOURS(1));
#endif
	}
}
