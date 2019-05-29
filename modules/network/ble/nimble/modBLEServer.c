/*
 * Copyright (c) 2016-2019  Moddable Tech, Inc.
 *
 *   This file is part of the Moddable SDK Runtime.
 * 
 *   The Moddable SDK Runtime is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 * 
 *   The Moddable SDK Runtime is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 * 
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with the Moddable SDK Runtime.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "xsmc.h"
#include "xsesp.h"
#include "mc.xs.h"
#include "modBLE.h"
#include "modBLECommon.h"

#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "esp_nimble_hci.h"
#include "services/gap/ble_svc_gap.h"

#include "mc.bleservices.c"

#define DEVICE_FRIENDLY_NAME "Moddable"

#define LOG_GAP 0
#if LOG_GAP
	#define LOG_GAP_EVENT(event) logGAPEvent(event)
	#define LOG_GAP_MSG(msg) modLog(msg)
	#define LOG_GAP_INT(i) modLogInt(i)
#else
	#define LOG_GAP_EVENT(event)
	#define LOG_GAP_MSG(msg)
	#define LOG_GAP_INT(i)
#endif

#define LOG_GATT 0
#if LOG_GATT
	#define LOG_GATT_EVENT(event) logGATTEvent(event)
	#define LOG_GATT_MSG(msg) modLog(msg)
	#define LOG_GATT_INT(i) modLogInt(i)
#else
	#define LOG_GATT_EVENT(event)
	#define LOG_GATT_MSG(msg)
	#define LOG_GATT_INT(i)
#endif

typedef struct {
	xsMachine	*the;
	xsSlot		obj;
	
	// server
	ble_addr_t	bda;
	
	// services
	uint16_t handles[service_count][max_attribute_count];
	
	// requests
	void *pending;
	
	// security
	uint8_t encryption;
	uint8_t bonding;
	uint8_t mitm;
	uint8_t iocap;
	
	// connection
	int16_t conn_id;
	ble_addr_t remote_bda;
	uint8_t terminating;
} modBLERecord, *modBLE;

typedef struct {
	uint16_t conn_id;
	uint16_t handle;
	uint16_t length;
	ble_uuid_any_t uuid;
	uint8_t isCharacteristic;
	uint8_t data[1];
} attributeDataRecord, *attributeData;

typedef struct {
	uint8_t notify;
	uint16_t conn_id;
	uint16_t handle;
	ble_uuid_any_t uuid;
} notificationStateRecord, *notificationState;

typedef struct {
	uint16_t length;
	uint8_t data[1];
} readDataRequestRecord, *readDataRequest;

static void uuidToBuffer(uint8_t *buffer, ble_uuid_any_t *uuid, uint16_t *length);
static const char_name_table *handleToCharName(uint16_t handle);
static const ble_uuid16_t *handleToUUID(uint16_t handle);

static void logGAPEvent(struct ble_gap_event *event);
static void logGATTEvent(uint8_t op);

static void nimble_host_task(void *param);
static void ble_host_task(void *param);
static void nimble_on_reset(int reason);
static void nimble_on_sync(void);
static void nimble_on_register(struct ble_gatt_register_ctxt *ctxt, void *arg);

static int nimble_gap_event(struct ble_gap_event *event, void *arg);

static modBLE gBLE = NULL;

void xs_ble_server_initialize(xsMachine *the)
{
	if (NULL != gBLE)
		xsUnknownError("BLE already initialized");
	gBLE = (modBLE)c_calloc(sizeof(modBLERecord), 1);
	if (!gBLE)
		xsUnknownError("no memory");
	xsmcSetHostData(xsThis, gBLE);
	gBLE->the = the;
	gBLE->obj = xsThis;
	gBLE->conn_id = -1;
	xsRemember(gBLE->obj);
	
	esp_err_t err = modBLEPlatformInitialize();
	if (ESP_OK != err)
		xsUnknownError("ble initialization failed");

	ble_hs_cfg.reset_cb = nimble_on_reset;
	ble_hs_cfg.sync_cb = nimble_on_sync;
	ble_hs_cfg.gatts_register_cb = nimble_on_register;
	ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

   // ble_store_ram_init();

	nimble_port_freertos_init(ble_host_task);
}

void xs_ble_server_close(xsMachine *the)
{
	modBLE *ble = xsmcGetHostData(xsThis);
	if (!ble) return;
	
	xsForget(gBLE->obj);
	xs_ble_server_destructor(gBLE);
	xsmcSetHostData(xsThis, NULL);
}

void xs_ble_server_destructor(void *data)
{
	modBLE ble = data;
	if (!ble) return;
	
	ble->terminating = true;
	if (-1 != ble->conn_id)
		ble_gap_terminate(ble->conn_id, BLE_ERR_REM_USER_CONN_TERM);
	if (0 != service_count)
		ble_gatts_reset();
	c_free(ble);
	gBLE = NULL;
	
	modBLEPlatformTerminate();
}

void xs_ble_server_disconnect(xsMachine *the)
{
	if (-1 != gBLE->conn_id)
		ble_gap_terminate(gBLE->conn_id, BLE_ERR_REM_USER_CONN_TERM);
}

void xs_ble_server_get_local_address(xsMachine *the)
{
	xsmcSetArrayBuffer(xsResult, (void*)gBLE->bda.val, 6);
}

void xs_ble_server_set_device_name(xsMachine *the)
{
	ble_svc_gap_device_name_set(xsmcToString(xsArg(0)));
}

void xs_ble_server_start_advertising(xsMachine *the)
{
	uint32_t intervalMin = xsmcToInteger(xsArg(0));
	uint32_t intervalMax = xsmcToInteger(xsArg(1));
	uint8_t *advertisingData = (uint8_t*)xsmcToArrayBuffer(xsArg(2));
	uint32_t advertisingDataLength = xsGetArrayBufferLength(xsArg(2));
	uint8_t *scanResponseData = xsmcTest(xsArg(3)) ? (uint8_t*)xsmcToArrayBuffer(xsArg(3)) : NULL;
	uint32_t scanResponseDataLength = xsmcTest(xsArg(3)) ? xsGetArrayBufferLength(xsArg(3)) : 0;	
	struct ble_gap_adv_params adv_params;
	
	c_memset(&adv_params, 0, sizeof(adv_params));
	adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	adv_params.itvl_min = intervalMin;
	adv_params.itvl_max = intervalMax;
	if (NULL != advertisingData)
		ble_gap_adv_set_data(advertisingData, advertisingDataLength);
	if (NULL != scanResponseData)
		ble_gap_adv_rsp_set_data(scanResponseData, scanResponseDataLength);
	ble_gap_adv_start(gBLE->bda.type, NULL, BLE_HS_FOREVER, &adv_params, nimble_gap_event, NULL);
}
	
void xs_ble_server_stop_advertising(xsMachine *the)
{
	ble_gap_adv_stop();
}

void xs_ble_server_characteristic_notify_value(xsMachine *the)
{
	uint16_t handle = xsmcToInteger(xsArg(0));
	uint16_t notify = xsmcToInteger(xsArg(1));
	struct os_mbuf *om;

	om = ble_hs_mbuf_from_flat(xsmcToArrayBuffer(xsArg(2)), xsGetArrayBufferLength(xsArg(2)));
	if (notify)
		ble_gattc_notify_custom(gBLE->conn_id, handle, om);
	else
		ble_gattc_indicate_custom(gBLE->conn_id, handle, om);
}

void xs_ble_server_set_security_parameters(xsMachine *the)
{
	uint8_t encryption = xsmcToBoolean(xsArg(0));
	uint8_t bonding = xsmcToBoolean(xsArg(1));
	uint8_t mitm = xsmcToBoolean(xsArg(2));
	uint8_t iocap = xsmcToInteger(xsArg(3));
	
	gBLE->encryption = encryption;
	gBLE->bonding = bonding;
	gBLE->mitm = mitm;
	gBLE->iocap = iocap;

	modBLESetSecurityParameters(encryption, bonding, mitm, iocap);
}

void xs_ble_server_passkey_input(xsMachine *the)
{
	uint8_t *address = (uint8_t*)xsmcToArrayBuffer(xsArg(0));
	uint32_t passkey = xsmcToInteger(xsArg(1));
	if (0 == c_memcmp(address, gBLE->remote_bda.val, 6)) {
		struct ble_sm_io pkey = {0};
		pkey.action = BLE_SM_IOACT_INPUT;
		pkey.passkey = passkey;
		ble_sm_inject_io(gBLE->conn_id, &pkey);
	}
}

void xs_ble_server_passkey_reply(xsMachine *the)
{
	uint8_t *address = (uint8_t*)xsmcToArrayBuffer(xsArg(0));
	if (0 == c_memcmp(address, gBLE->remote_bda.val, 6)) {
		struct ble_sm_io pkey = {0};
		pkey.action = BLE_SM_IOACT_NUMCMP;
		pkey.numcmp_accept = xsmcToBoolean(xsArg(1));
		ble_sm_inject_io(gBLE->conn_id, &pkey);
	}
}

void xs_ble_server_deploy(xsMachine *the)
{
	// services deployed from readyEvent(), since stack requires deploy before advertising
}

static void deployServices(xsMachine *the)
{
	static const ble_uuid16_t BT_UUID_GAP = BLE_UUID16_INIT(0x1800);
	uint8_t hasGAP = false;

	if (0 == service_count)
		return;
		
	// Set device name and appearance from app GAP service when available
	for (int service_index = 0; service_index < service_count; ++service_index) {
		const struct ble_gatt_svc_def *service = &gatt_svr_svcs[service_index];
		if (0 == ble_uuid_cmp((const ble_uuid_t*)service->uuid, &BT_UUID_GAP.u)) {
			hasGAP = true;
			break;
		}
	}
	
	int rc = ble_gatts_reset();
	if (0 == rc) {
		if (!hasGAP) {
			ble_svc_gap_init();
			ble_svc_gap_device_name_set(DEVICE_FRIENDLY_NAME);
			ble_svc_gap_device_appearance_set(BLE_SVC_GAP_APPEARANCE_GEN_COMPUTER);
		}
	}
	if (0 == rc)
		rc = ble_gatts_count_cfg(gatt_svr_svcs);
	if (0 == rc)
		rc = ble_gatts_add_svcs(gatt_svr_svcs);
	if (0 == rc)
		rc = ble_gatts_start();
	if (0 != rc)
		xsUnknownError("failed to start services");
}

static void readyEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	ble_hs_id_infer_auto(0, &gBLE->bda.type);
	ble_hs_id_copy_addr(gBLE->bda.type, gBLE->bda.val, NULL);

	deployServices(the);

	xsBeginHost(gBLE->the);
	xsCall1(gBLE->obj, xsID_callback, xsString("onReady"));
	xsEndHost(gBLE->the);
}

static void connectEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	struct ble_gap_conn_desc *desc = (struct ble_gap_conn_desc *)message;
	xsBeginHost(gBLE->the);		
	if (-1 != desc->conn_handle) {
		if (-1 != gBLE->conn_id) {
			LOG_GAP_MSG("Ignoring duplicate connect event");
			goto bail;
		}
		gBLE->conn_id = desc->conn_handle;
		gBLE->remote_bda = desc->peer_id_addr;
		xsmcVars(3);
		xsVar(0) = xsmcNewObject();
		xsmcSetInteger(xsVar(1), desc->conn_handle);
		xsmcSet(xsVar(0), xsID_connection, xsVar(1));
		xsmcSetArrayBuffer(xsVar(2), &desc->peer_id_addr.val, 6);
		xsmcSet(xsVar(0), xsID_address, xsVar(2));
		xsCall2(gBLE->obj, xsID_callback, xsString("onConnected"), xsVar(0));
	}
	else {
		LOG_GAP_MSG("BLE_GAP_EVENT_CONNECT failed");
		xsCall1(gBLE->obj, xsID_callback, xsString("onDisconnected"));
	}
bail:
	xsEndHost(gBLE->the);
}

static void disconnectEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	struct ble_gap_conn_desc *desc = (struct ble_gap_conn_desc *)message;
	xsBeginHost(gBLE->the);
	
	// ignore multiple disconnects on same connection
	if (-1 == gBLE->conn_id) {
		LOG_GAP_MSG("Ignoring duplicate disconnect event");
		goto bail;
	}	
	
	gBLE->conn_id = -1;
	xsmcVars(2);
	xsVar(0) = xsmcNewObject();
	xsmcSetInteger(xsVar(1), desc->conn_handle);
	xsmcSet(xsVar(0), xsID_connection, xsVar(1));
	xsmcSetArrayBuffer(xsVar(1), desc->peer_id_addr.val, 6);
	xsmcSet(xsVar(0), xsID_address, xsVar(1));
	xsCall2(gBLE->obj, xsID_callback, xsString("onDisconnected"), xsVar(0));
bail:
	xsEndHost(gBLE->the);
}

static void writeEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	attributeData value = (attributeData)refcon;
	uint8_t buffer[16];
	uint16_t length;
	const char_name_table *char_name = handleToCharName(value->handle);
	
	xsBeginHost(gBLE->the);

	xsmcVars(4);
	xsVar(0) = xsmcNewObject();
	uuidToBuffer(buffer, &value->uuid, &length);
	xsmcSetArrayBuffer(xsVar(1), buffer, length);
	xsmcSet(xsVar(0), xsID_uuid, xsVar(1));
	if (char_name) {
		xsmcSetString(xsVar(2), (char*)char_name->name);
		xsmcSet(xsVar(0), xsID_name, xsVar(2));
		xsmcSetString(xsVar(3), (char*)char_name->type);
		xsmcSet(xsVar(0), xsID_type, xsVar(3));
	}
	xsmcSetInteger(xsVar(2), value->handle);
	xsmcSet(xsVar(0), xsID_handle, xsVar(2));
	xsmcSetArrayBuffer(xsVar(3), value->data, value->length);
	xsmcSet(xsVar(0), xsID_value, xsVar(3));
	xsCall2(gBLE->obj, xsID_callback, xsString("onCharacteristicWritten"), xsVar(0));

	c_free(value);
	xsEndHost(gBLE->the);
}

static void readEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	struct ble_gatt_access_ctxt *ctxt = (struct ble_gatt_access_ctxt *)refcon;
	const struct ble_gatt_chr_def *chr = (const struct ble_gatt_chr_def *)ctxt->chr;
	const ble_uuid_t *uuid = chr->uuid;
	const char_name_table *char_name;
	uint8_t buffer[16];
	uint16_t uuid_length;
	
	char_name = handleToCharName(*chr->val_handle);
	uuidToBuffer(buffer, (ble_uuid_any_t *)chr->uuid, &uuid_length);

	xsBeginHost(gBLE->the);
	xsmcVars(5);

	xsVar(0) = xsmcNewObject();
	xsmcSetArrayBuffer(xsVar(1), buffer, uuid_length);
	xsmcSetInteger(xsVar(2), *chr->val_handle);
	xsmcSet(xsVar(0), xsID_uuid, xsVar(1));
	xsmcSet(xsVar(0), xsID_handle, xsVar(2));
	if (char_name) {
		xsmcSetString(xsVar(3), (char*)char_name->name);
		xsmcSet(xsVar(0), xsID_name, xsVar(3));
		xsmcSetString(xsVar(4), (char*)char_name->type);
		xsmcSet(xsVar(0), xsID_type, xsVar(4));
	}
	xsResult = xsCall2(gBLE->obj, xsID_callback, xsString("onCharacteristicRead"), xsVar(0));
	if (xsUndefinedType != xsmcTypeOf(xsResult)) {
		readDataRequest data = c_malloc(sizeof(readDataRequestRecord) + xsGetArrayBufferLength(xsResult));
		if (NULL == data)
			gBLE->pending = (void*)-1L;
		else {
			data->length = xsGetArrayBufferLength(xsResult);
			c_memmove(data->data, xsmcToArrayBuffer(xsResult), data->length);
			gBLE->pending = data;
		}
	}
	else
		gBLE->pending = (void*)-1L;

	xsEndHost(gBLE->the);
}

static void notificationStateEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	notificationState state = (notificationState)refcon;
	xsBeginHost(gBLE->the);
	if (state->conn_id != gBLE->conn_id)
		goto bail;
	uint16_t uuid_length;
	uint8_t buffer[16];
	char_name_table *char_name = (char_name_table *)handleToCharName(state->handle);
	const ble_uuid16_t *uuid = handleToUUID(state->handle);
	
	xsmcVars(6);
	xsVar(0) = xsmcNewObject();
	uuidToBuffer(buffer, (ble_uuid_any_t *)uuid, &uuid_length);
	xsmcSetArrayBuffer(xsVar(1), buffer, uuid_length);
	xsmcSet(xsVar(0), xsID_uuid, xsVar(1));
	if (char_name) {
		xsmcSetString(xsVar(2), (char*)char_name->name);
		xsmcSet(xsVar(0), xsID_name, xsVar(2));
		xsmcSetString(xsVar(3), (char*)char_name->type);
		xsmcSet(xsVar(0), xsID_type, xsVar(3));
	}
	xsmcSetInteger(xsVar(4), state->handle);
	xsmcSetInteger(xsVar(5), state->notify);
	xsmcSet(xsVar(0), xsID_handle, xsVar(4));
	xsmcSet(xsVar(0), xsID_notify, xsVar(5));
	xsCall2(gBLE->obj, xsID_callback, xsString(state->notify ? "onCharacteristicNotifyEnabled" : "onCharacteristicNotifyDisabled"), xsVar(0));
bail:
	c_free(state);
	xsEndHost(gBLE->the);
}

static void passkeyEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	struct ble_gap_event *event = (struct ble_gap_event *)message;
	struct ble_sm_io pkey = {0};
	
	pkey.action = event->passkey.params.action;
	
	if (event->passkey.conn_handle != gBLE->conn_id)
		xsUnknownError("connection not found");
		
	xsBeginHost(gBLE->the);
	xsmcVars(3);
	xsVar(0) = xsmcNewObject();

    if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
    	pkey.passkey = (c_rand() % 999999) + 1;
		xsmcSetArrayBuffer(xsVar(1), gBLE->remote_bda.val, 6);
		xsmcSetInteger(xsVar(2), pkey.passkey);
		xsmcSet(xsVar(0), xsID_address, xsVar(1));
		xsmcSet(xsVar(0), xsID_passkey, xsVar(2));
		xsCall2(gBLE->obj, xsID_callback, xsString("onPasskeyDisplay"), xsVar(0));
		ble_sm_inject_io(event->passkey.conn_handle, &pkey);
	}
    else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
		xsmcSetArrayBuffer(xsVar(1), gBLE->remote_bda.val, 6);
		xsmcSet(xsVar(0), xsID_address, xsVar(1));
		if (gBLE->iocap == KeyboardOnly)
			xsCall2(gBLE->obj, xsID_callback, xsString("onPasskeyInput"), xsVar(0));
		else {
			xsResult = xsCall2(gBLE->obj, xsID_callback, xsString("onPasskeyRequested"), xsVar(0));
			pkey.passkey = xsmcToInteger(xsResult);
			ble_sm_inject_io(event->passkey.conn_handle, &pkey);
		}
	}
	else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
		xsmcSetArrayBuffer(xsVar(1), gBLE->remote_bda.val, 6);
		xsmcSetInteger(xsVar(2), event->passkey.params.numcmp);
		xsmcSet(xsVar(0), xsID_address, xsVar(1));
		xsmcSet(xsVar(0), xsID_passkey, xsVar(2));
		xsCall2(gBLE->obj, xsID_callback, xsString("onPasskeyConfirm"), xsVar(0));
	}
	
bail:
	xsEndHost(gBLE->the);
}

static void encryptionChangeEvent(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	struct ble_gap_event *event = (struct ble_gap_event *)message;
	if (0 == event->enc_change.status) {
		struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (0 == rc) {
        	if (desc.sec_state.encrypted) {
				xsBeginHost(gBLE->the);
				xsCall1(gBLE->obj, xsID_callback, xsString("onAuthenticated"));
				xsEndHost(gBLE->the);
        	}
        }
	}
}

void nimble_host_task(void *param)
{
	nimble_port_run();
}

void ble_host_task(void *param)
{
	nimble_host_task(param);
}

void nimble_on_reset(int reason)
{
	// fatal controller reset - all connections have been closed
	if (gBLE)
		xs_ble_server_close(gBLE->the);
}

void nimble_on_sync(void)
{
	ble_hs_util_ensure_addr(0);
	modMessagePostToMachine(gBLE->the, NULL, 0, readyEvent, NULL);
}

static void nimble_on_register(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
	int service_index, att_index;
	
	if (!gBLE || gBLE->terminating)
		return;

	switch (ctxt->op) {
		case BLE_GATT_REGISTER_OP_CHR: {
			const struct ble_gatt_svc_def *svc_def = ctxt->chr.svc_def;
			const struct ble_gatt_chr_def *chr_def = ctxt->chr.chr_def;
			for (service_index = 0; service_index < service_count; ++service_index) {
				const struct ble_gatt_svc_def *service = &gatt_svr_svcs[service_index];
				if (0 == ble_uuid_cmp((const ble_uuid_t*)svc_def->uuid, (const ble_uuid_t*)service->uuid)) {
					for (att_index = 0; att_index < attribute_counts[service_index]; ++att_index) {
						const struct ble_gatt_chr_def *characteristic = &service->characteristics[att_index];
						if (0 == ble_uuid_cmp((const ble_uuid_t*)chr_def->uuid, (const ble_uuid_t*)characteristic->uuid)) {
							gBLE->handles[service_index][att_index] = ctxt->chr.val_handle;
							return;
						}
					}
				}
			}
			break;
		}
#if 0
		case BLE_GATT_REGISTER_OP_DSC: {
			const struct ble_gatt_svc_def *svc_def = ctxt->chr.svc_def;
			const struct ble_gatt_dsc_def *dsc_def = ctxt->dsc.dsc_def;
			for (service_index = 0; service_index < service_count; ++service_index) {
				const struct ble_gatt_svc_def *service = &gatt_svr_svcs[service_index];
				if (0 == ble_uuid_cmp((const ble_uuid_t*)svc_def->uuid, (const ble_uuid_t*)service->uuid)) {
					for (att_index = 0; att_index < attribute_counts[service_index]; ++att_index) {
						const struct ble_gatt_chr_def *characteristic = &service->characteristics[att_index];
						if (0 == ble_uuid_cmp((const ble_uuid_t*)chr_def->uuid, (const ble_uuid_t*)characteristic->uuid)) {
							gBLE->handles[service_index][att_index] = ctxt->chr.val_handle;
							return;
						}
					}
				}
			}
			break;
		}
#endif
		default:
			break;
	}
}

static int nimble_gap_event(struct ble_gap_event *event, void *arg)
{
    int rc = 0;
	struct ble_gap_conn_desc desc;

	LOG_GAP_EVENT(event);

	if (!gBLE || gBLE->terminating)
		goto bail;

    switch (event->type) {
		case BLE_GAP_EVENT_CONNECT:
			if (event->connect.status == 0) {
				if (0 == ble_gap_conn_find(event->connect.conn_handle, &desc)) {
					if (gBLE->mitm || gBLE->encryption)
						ble_gap_security_initiate(desc.conn_handle);
					modMessagePostToMachine(gBLE->the, (uint8_t*)&desc, sizeof(desc), connectEvent, NULL);
					goto bail;
				}
			}
			c_memset(&desc, 0, sizeof(desc));
			desc.conn_handle = -1;
			modMessagePostToMachine(gBLE->the, (uint8_t*)&desc, sizeof(desc), connectEvent, NULL);
			break;
		case BLE_GAP_EVENT_DISCONNECT:
			modMessagePostToMachine(gBLE->the, (uint8_t*)&event->disconnect.conn, sizeof(event->disconnect.conn), disconnectEvent, NULL);
			break;
		case BLE_GAP_EVENT_SUBSCRIBE: {
			notificationState state = c_malloc(sizeof(notificationStateRecord));
			if (NULL != state) {
				state->notify = event->subscribe.cur_notify;
				state->conn_id = event->subscribe.conn_handle;
				state->handle = event->subscribe.attr_handle;
				modMessagePostToMachine(gBLE->the, NULL, 0, notificationStateEvent, state);
			}
			break;
		}
		case BLE_GAP_EVENT_ENC_CHANGE:
			modMessagePostToMachine(gBLE->the, (uint8_t*)event, sizeof(struct ble_gap_event), encryptionChangeEvent, NULL);
			break;
		case BLE_GAP_EVENT_REPEAT_PAIRING:
			// delete old bond and accept new link
			if (0 == ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc))
				ble_store_util_delete_peer(&desc.peer_id_addr);
			return BLE_GAP_REPEAT_PAIRING_RETRY;
			break;
		case BLE_GAP_EVENT_PASSKEY_ACTION:
			modMessagePostToMachine(gBLE->the, (uint8_t*)event, sizeof(struct ble_gap_event), passkeyEvent, NULL);
			break;
		default:
			break;
    }
    
bail:
	return rc;
}

void uuidToBuffer(uint8_t *buffer, ble_uuid_any_t *uuid, uint16_t *length)
{
	if (uuid->u.type == BLE_UUID_TYPE_16) {
		*length = 2;
		buffer[0] = uuid->u16.value & 0xFF;
		buffer[1] = (uuid->u16.value >> 8) & 0xFF;
	}
	else if (uuid->u.type == BLE_UUID_TYPE_32) {
		*length = 4;
		buffer[0] = uuid->u32.value & 0xFF;
		buffer[1] = (uuid->u32.value >> 8) & 0xFF;
		buffer[2] = (uuid->u32.value >> 16) & 0xFF;
		buffer[3] = (uuid->u32.value >> 24) & 0xFF;
	}
	else {
		*length = 16;
		c_memmove(buffer, uuid->u128.value, *length);
	}
}

const char_name_table *handleToCharName(uint16_t handle) {
	for (uint16_t i = 0; i < service_count; ++i) {
		for (uint16_t j = 0; j < attribute_counts[i]; ++j) {
			if (handle == gBLE->handles[i][j]) {
				for (uint16_t k = 0; k < char_name_count; ++k) {
					if (char_names[k].service_index == i && char_names[k].att_index == j)
						return &char_names[k];
				}
			}
		}
	}
	return NULL;
}

const ble_uuid16_t *handleToUUID(uint16_t handle) {
	for (int service_index = 0; service_index < service_count; ++service_index) {
		const struct ble_gatt_svc_def *service = &gatt_svr_svcs[service_index];
		for (int att_index = 0; att_index < attribute_counts[service_index]; ++att_index) {
			const struct ble_gatt_chr_def *chr_def = &service->characteristics[att_index];
			if (handle == *chr_def->val_handle)
				return (ble_uuid16_t *)chr_def->uuid;
		}
	}
	return NULL;
}

int gatt_svr_chr_dynamic_value_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	LOG_GATT_EVENT(ctxt->op);
	
	if (!gBLE || gBLE->terminating)
		goto bail;

	switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR: {
        	// The read request must be satisfied from this task.
        	gBLE->pending = NULL;
        	modMessagePostToMachine(gBLE->the, NULL, 0, readEvent, (void*)ctxt);
        	while (NULL == gBLE->pending) {
        		modDelayMilliseconds(5);
        	}
        	if ((void*)-1L == gBLE->pending)
        		return BLE_ATT_ERR_INSUFFICIENT_RES;
        	else {
        		readDataRequest data = (readDataRequest)gBLE->pending;
        		os_mbuf_append(ctxt->om, data->data, data->length);
        		c_free(data);
        	}
        	return 0;
        	break;
        }
		case BLE_GATT_ACCESS_OP_WRITE_CHR: {
			uint8_t *data;
			uint16_t length = sizeof(attributeDataRecord) + ctxt->om->om_len;
			attributeData value = c_malloc(length);
			if (NULL != value) {
				value->isCharacteristic = 1;
				value->conn_id = conn_handle;
				value->handle = attr_handle;
				value->length = ctxt->om->om_len;
				c_memmove(value->data, ctxt->om->om_data, ctxt->om->om_len);
				ble_uuid_copy(&value->uuid, ctxt->chr->uuid);
				modMessagePostToMachine(gBLE->the, NULL, 0, writeEvent, (void*)value);
			}
			break;
		}
	}

bail:
	return 0;
}

void logGAPEvent(struct ble_gap_event *event) {
	switch(event->type) {
		case BLE_GAP_EVENT_CONNECT: modLog("BLE_GAP_EVENT_CONNECT"); break;
		case BLE_GAP_EVENT_DISCONNECT: modLog("BLE_GAP_EVENT_DISCONNECT"); break;
		case BLE_GAP_EVENT_CONN_UPDATE: modLog("BLE_GAP_EVENT_CONN_UPDATE"); break;
		case BLE_GAP_EVENT_CONN_UPDATE_REQ: modLog("BLE_GAP_EVENT_CONN_UPDATE_REQ"); break;
		case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: modLog("BLE_GAP_EVENT_L2CAP_UPDATE_REQ"); break;
		case BLE_GAP_EVENT_TERM_FAILURE: modLog("BLE_GAP_EVENT_TERM_FAILURE"); break;
		case BLE_GAP_EVENT_DISC: modLog("BLE_GAP_EVENT_DISC"); break;
		case BLE_GAP_EVENT_DISC_COMPLETE: modLog("BLE_GAP_EVENT_DISC_COMPLETE"); break;
		case BLE_GAP_EVENT_ADV_COMPLETE: modLog("BLE_GAP_EVENT_ADV_COMPLETE"); break;
		case BLE_GAP_EVENT_ENC_CHANGE: modLog("BLE_GAP_EVENT_ENC_CHANGE"); break;
		case BLE_GAP_EVENT_PASSKEY_ACTION: modLog("BLE_GAP_EVENT_PASSKEY_ACTION"); break;
		case BLE_GAP_EVENT_NOTIFY_RX: modLog("BLE_GAP_EVENT_NOTIFY_RX"); break;
		case BLE_GAP_EVENT_NOTIFY_TX: modLog("BLE_GAP_EVENT_NOTIFY_TX"); break;
		case BLE_GAP_EVENT_SUBSCRIBE: modLog("BLE_GAP_EVENT_SUBSCRIBE"); break;
		case BLE_GAP_EVENT_MTU: modLog("BLE_GAP_EVENT_MTU"); break;
		case BLE_GAP_EVENT_IDENTITY_RESOLVED: modLog("BLE_GAP_EVENT_IDENTITY_RESOLVED"); break;
		case BLE_GAP_EVENT_REPEAT_PAIRING: modLog("BLE_GAP_EVENT_REPEAT_PAIRING"); break;
		case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: modLog("BLE_GAP_EVENT_PHY_UPDATE_COMPLETE"); break;
		case BLE_GAP_EVENT_EXT_DISC: modLog("BLE_GAP_EVENT_EXT_DISC"); break;
	}
}

void logGATTEvent(uint8_t op)
{
	switch(op) {
		case BLE_GATT_ACCESS_OP_READ_CHR: modLog("BLE_GATT_ACCESS_OP_READ_CHR"); break;
		case BLE_GATT_ACCESS_OP_WRITE_CHR: modLog("BLE_GATT_ACCESS_OP_WRITE_CHR"); break;
		case BLE_GATT_ACCESS_OP_READ_DSC: modLog("BLE_GATT_ACCESS_OP_READ_DSC"); break;
		case BLE_GATT_ACCESS_OP_WRITE_DSC: modLog("BLE_GATT_ACCESS_OP_WRITE_DSC"); break;
	}
}
