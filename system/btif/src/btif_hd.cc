/******************************************************************************
 *
 *  Copyright 2016 The Android Open Source Project
 *  Copyright 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/************************************************************************************
 *
 *  Filename:      btif_hd.c
 *
 *  Description:   HID Device Profile Bluetooth Interface
 *
 *
 ***********************************************************************************/

#define LOG_TAG "BTIF_HD"

#include "btif/include/btif_hd.h"

#include <bluetooth/log.h>
#include <string.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bta/include/bta_dm_api.h"
#include "bta/include/bta_hd_api.h"
#include "bta/sys/bta_sys.h"
#include "bta_api.h"
#include "bta_sec_api.h"
#include "btif/include/btif_common.h"
#include "btif/include/btif_dm.h"
#include "btif/include/btif_hh.h"
#include "btif/include/btif_profile_storage.h"
#include "btif/include/btif_util.h"
#include "hardware/bluetooth.h"
#include "include/hardware/bt_hd.h"
#include "internal_include/bt_target.h"
#include "osi/include/allocator.h"
#include "osi/include/compat.h"
#include "types/raw_address.h"

#define BTIF_HD_APP_NAME_LEN 50
#define BTIF_HD_APP_DESCRIPTION_LEN 50
#define BTIF_HD_APP_PROVIDER_LEN 50
#define BTIF_HD_APP_DESCRIPTOR_LEN 2048

#define COD_HID_KEYBOARD 0x0540
#define COD_HID_POINTING 0x0580
#define COD_HID_COMBO 0x05C0
#define COD_HID_MAJOR 0x0500

using namespace bluetooth;

/* HD request events */
typedef enum { BTIF_HD_DUMMY_REQ_EVT = 0 } btif_hd_req_evt_t;

btif_hd_cb_t btif_hd_cb;

static bthd_callbacks_t* bt_hd_callbacks = NULL;
static tBTA_HD_APP_INFO app_info;
static tBTA_HD_QOS_INFO in_qos;
static tBTA_HD_QOS_INFO out_qos;

static void intr_data_copy_cb(uint16_t event, char* p_dst, const char* p_src) {
  tBTA_HD_INTR_DATA* p_dst_data = (tBTA_HD_INTR_DATA*)p_dst;
  tBTA_HD_INTR_DATA* p_src_data = (tBTA_HD_INTR_DATA*)p_src;
  uint8_t* p_data;

  if (!p_src) {
    return;
  }

  if (event != BTA_HD_INTR_DATA_EVT) {
    return;
  }

  memcpy(p_dst, p_src, sizeof(tBTA_HD_INTR_DATA));

  p_data = ((uint8_t*)p_dst_data) + sizeof(tBTA_HD_INTR_DATA);

  memcpy(p_data, p_src_data->p_data, p_src_data->len);

  p_dst_data->p_data = p_data;
}

static void set_report_copy_cb(uint16_t event, char* p_dst, const char* p_src) {
  tBTA_HD_SET_REPORT* p_dst_data = (tBTA_HD_SET_REPORT*)p_dst;
  tBTA_HD_SET_REPORT* p_src_data = (tBTA_HD_SET_REPORT*)p_src;
  uint8_t* p_data;

  if (!p_src) {
    return;
  }

  if (event != BTA_HD_SET_REPORT_EVT) {
    return;
  }

  memcpy(p_dst, p_src, sizeof(tBTA_HD_SET_REPORT));

  p_data = ((uint8_t*)p_dst_data) + sizeof(tBTA_HD_SET_REPORT);

  memcpy(p_data, p_src_data->p_data, p_src_data->len);

  p_dst_data->p_data = p_data;
}

static void btif_hd_free_buf() {
  if (app_info.descriptor.dsc_list) {
    osi_free(app_info.descriptor.dsc_list);
  }
  if (app_info.p_description) {
    osi_free(app_info.p_description);
  }
  if (app_info.p_name) {
    osi_free(app_info.p_name);
  }
  if (app_info.p_provider) {
    osi_free(app_info.p_provider);
  }
  app_info.descriptor.dsc_list = NULL;
  app_info.p_description = NULL;
  app_info.p_name = NULL;
  app_info.p_provider = NULL;
}

/*******************************************************************************
 *
 * Function         btif_hd_remove_device
 *
 * Description      Removes plugged device
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_hd_remove_device(RawAddress bd_addr) {
  BTA_HdRemoveDevice(bd_addr);
  btif_storage_remove_hidd(&bd_addr);
}

/*******************************************************************************
 *
 * Function         btif_hd_upstreams_evt
 *
 * Description      Executes events in btif context
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_hd_upstreams_evt(uint16_t event, char* p_param) {
  tBTA_HD* p_data = (tBTA_HD*)p_param;

  log::verbose("event={}", dump_hd_event(event));

  switch (event) {
    case BTA_HD_ENABLE_EVT:
      log::verbose("status={}", p_data->status);
      if (p_data->status == BTA_HD_OK) {
        btif_storage_load_hidd();
        btif_hd_cb.status = BTIF_HD_ENABLED;
        /* Register the app if not yet registered */
        if (!btif_hd_cb.app_registered) {
          BTA_HdRegisterApp(&app_info, &in_qos, &out_qos);
          btif_hd_free_buf();
        }
      } else {
        btif_hd_cb.status = BTIF_HD_DISABLED;
        log::warn("Failed to enable BT-HD, status={}", p_data->status);
      }
      break;

    case BTA_HD_DISABLE_EVT:
      log::verbose("status={}", p_data->status);
      btif_hd_cb.status = BTIF_HD_DISABLED;
      if (btif_hd_cb.service_dereg_active) {
        bta_sys_deregister(BTA_ID_HD);
        log::warn("registering hid host now");
        btif_hh_service_registration(TRUE);
        btif_hd_cb.service_dereg_active = FALSE;
      }
      btif_hd_free_buf();
      if (p_data->status == BTA_HD_OK) {
        memset(&btif_hd_cb, 0, sizeof(btif_hd_cb));
      } else {
        log::warn("Failed to disable BT-HD, status={}", p_data->status);
      }
      break;

    case BTA_HD_REGISTER_APP_EVT: {
      RawAddress* addr = (RawAddress*)&p_data->reg_status.bda;

      if (!p_data->reg_status.in_use) {
        addr = NULL;
      }

      log::info("Registering HID device app");
      btif_hd_cb.app_registered = TRUE;
      HAL_CBACK(bt_hd_callbacks, application_state_cb, addr, BTHD_APP_STATE_REGISTERED);
    } break;

    case BTA_HD_UNREGISTER_APP_EVT:
      btif_hd_cb.app_registered = FALSE;
      HAL_CBACK(bt_hd_callbacks, application_state_cb, NULL, BTHD_APP_STATE_NOT_REGISTERED);
      if (btif_hd_cb.service_dereg_active) {
        log::warn("disabling hid device service now");
        BTA_HdDisable();
      }
      break;

    case BTA_HD_OPEN_EVT: {
      RawAddress& addr = p_data->conn.bda;
      log::warn("BTA_HD_OPEN_EVT, address={}", addr);
      /* Check if the connection is from hid host and not hid device */
      if (check_cod_hid(addr)) {
        /* Incoming connection from hid device, reject it */
        log::warn("remote device is not hid host, disconnecting");
        btif_hd_cb.forced_disc = TRUE;
        BTA_HdDisconnect();
        break;
      }
      btif_storage_set_hidd(p_data->conn.bda);

      HAL_CBACK(bt_hd_callbacks, connection_state_cb, (RawAddress*)&p_data->conn.bda,
                BTHD_CONN_STATE_CONNECTED);
    } break;

    case BTA_HD_CLOSE_EVT:
      if (btif_hd_cb.forced_disc) {
        RawAddress* addr = (RawAddress*)&p_data->conn.bda;
        log::warn("remote device was forcefully disconnected");
        btif_hd_remove_device(*addr);
        btif_hd_cb.forced_disc = FALSE;
        break;
      }
      HAL_CBACK(bt_hd_callbacks, connection_state_cb, (RawAddress*)&p_data->conn.bda,
                BTHD_CONN_STATE_DISCONNECTED);
      break;

    case BTA_HD_GET_REPORT_EVT:
      HAL_CBACK(bt_hd_callbacks, get_report_cb, p_data->get_report.report_type,
                p_data->get_report.report_id, p_data->get_report.buffer_size);
      break;

    case BTA_HD_SET_REPORT_EVT:
      HAL_CBACK(bt_hd_callbacks, set_report_cb, p_data->set_report.report_type,
                p_data->set_report.report_id, p_data->set_report.len, p_data->set_report.p_data);
      break;

    case BTA_HD_SET_PROTOCOL_EVT:
      HAL_CBACK(bt_hd_callbacks, set_protocol_cb, p_data->set_protocol);
      break;

    case BTA_HD_INTR_DATA_EVT:
      HAL_CBACK(bt_hd_callbacks, intr_data_cb, p_data->intr_data.report_id, p_data->intr_data.len,
                p_data->intr_data.p_data);
      break;

    case BTA_HD_VC_UNPLUG_EVT:
      HAL_CBACK(bt_hd_callbacks, connection_state_cb, (RawAddress*)&p_data->conn.bda,
                BTHD_CONN_STATE_DISCONNECTED);
      if (bta_dm_check_if_only_hd_connected(p_data->conn.bda)) {
        log::verbose("Removing bonding as only HID profile connected");
        BTA_DmRemoveDevice(p_data->conn.bda);
      } else {
        RawAddress* bd_addr = (RawAddress*)&p_data->conn.bda;
        log::verbose("Only removing HID data as some other profiles connected");
        btif_hd_remove_device(*bd_addr);
      }
      HAL_CBACK(bt_hd_callbacks, vc_unplug_cb);
      break;

    case BTA_HD_CONN_STATE_EVT:
      HAL_CBACK(bt_hd_callbacks, connection_state_cb, (RawAddress*)&p_data->conn.bda,
                (bthd_connection_state_t)p_data->conn.status);
      break;

    default:
      log::warn("unknown event ({})", event);
      break;
  }
}

/*******************************************************************************
 *
 * Function         bte_hd_evt
 *
 * Description      Switches context from BTE to BTIF for all BT-HD events
 *
 * Returns          void
 *
 ******************************************************************************/

static void bte_hd_evt(tBTA_HD_EVT event, tBTA_HD* p_data) {
  bt_status_t status;
  int param_len = 0;
  tBTIF_COPY_CBACK* p_copy_cback = NULL;

  log::verbose("event={}", event);

  switch (event) {
    case BTA_HD_ENABLE_EVT:
    case BTA_HD_DISABLE_EVT:
    case BTA_HD_UNREGISTER_APP_EVT:
      param_len = sizeof(tBTA_HD_STATUS);
      break;

    case BTA_HD_REGISTER_APP_EVT:
      param_len = sizeof(tBTA_HD_REG_STATUS);
      break;

    case BTA_HD_OPEN_EVT:
    case BTA_HD_CLOSE_EVT:
    case BTA_HD_VC_UNPLUG_EVT:
    case BTA_HD_CONN_STATE_EVT:
      param_len = sizeof(tBTA_HD_CONN);
      break;

    case BTA_HD_GET_REPORT_EVT:
      param_len += sizeof(tBTA_HD_GET_REPORT);
      break;

    case BTA_HD_SET_REPORT_EVT:
      param_len = sizeof(tBTA_HD_SET_REPORT) + p_data->set_report.len;
      p_copy_cback = set_report_copy_cb;
      break;

    case BTA_HD_SET_PROTOCOL_EVT:
      param_len += sizeof(p_data->set_protocol);
      break;

    case BTA_HD_INTR_DATA_EVT:
      param_len = sizeof(tBTA_HD_INTR_DATA) + p_data->intr_data.len;
      p_copy_cback = intr_data_copy_cb;
      break;
  }

  status = btif_transfer_context(btif_hd_upstreams_evt, (uint16_t)event, (char*)p_data, param_len,
                                 p_copy_cback);

  ASSERTC(status == BT_STATUS_SUCCESS, "context transfer failed", status);
}

/*******************************************************************************
 *
 * Function        init
 *
 * Description     Initializes BT-HD interface
 *
 * Returns         BT_STATUS_SUCCESS
 *
 ******************************************************************************/
static bt_status_t init(bthd_callbacks_t* callbacks) {
  log::verbose("");

  bt_hd_callbacks = callbacks;
  memset(&btif_hd_cb, 0, sizeof(btif_hd_cb));

  btif_enable_service(BTA_HIDD_SERVICE_ID);

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         cleanup
 *
 * Description      Cleans up BT-HD interface
 *
 * Returns          none
 *
 ******************************************************************************/
static void cleanup(void) {
  log::verbose("");

  if (bt_hd_callbacks) {
    /* update flag, not to enable hid host service now as BT is switching off */
    btif_hd_cb.service_dereg_active = FALSE;
    btif_disable_service(BTA_HIDD_SERVICE_ID);
    bt_hd_callbacks = NULL;
  }
}

/*******************************************************************************
 *
 * Function         register_app
 *
 * Description      Registers HID Device application
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t register_app(bthd_app_param_t* p_app_param, bthd_qos_param_t* p_in_qos,
                                bthd_qos_param_t* p_out_qos) {
  log::verbose("");

  if (btif_hd_cb.app_registered) {
    log::warn("application already registered");
    return BT_STATUS_DONE;
  }

  app_info.p_name = (char*)osi_calloc(BTIF_HD_APP_NAME_LEN);
  osi_strlcpy(app_info.p_name, p_app_param->name, BTIF_HD_APP_NAME_LEN);
  app_info.p_description = (char*)osi_calloc(BTIF_HD_APP_DESCRIPTION_LEN);
  osi_strlcpy(app_info.p_description, p_app_param->description, BTIF_HD_APP_DESCRIPTION_LEN);
  app_info.p_provider = (char*)osi_calloc(BTIF_HD_APP_PROVIDER_LEN);
  osi_strlcpy(app_info.p_provider, p_app_param->provider, BTIF_HD_APP_PROVIDER_LEN);
  app_info.subclass = p_app_param->subclass;
  app_info.descriptor.dl_len = p_app_param->desc_list_len;
  app_info.descriptor.dsc_list = (uint8_t*)osi_malloc(app_info.descriptor.dl_len);
  memcpy(app_info.descriptor.dsc_list, p_app_param->desc_list, p_app_param->desc_list_len);

  in_qos.service_type = p_in_qos->service_type;
  in_qos.token_rate = p_in_qos->token_rate;
  in_qos.token_bucket_size = p_in_qos->token_bucket_size;
  in_qos.peak_bandwidth = p_in_qos->peak_bandwidth;
  in_qos.access_latency = p_in_qos->access_latency;
  in_qos.delay_variation = p_in_qos->delay_variation;

  out_qos.service_type = p_out_qos->service_type;
  out_qos.token_rate = p_out_qos->token_rate;
  out_qos.token_bucket_size = p_out_qos->token_bucket_size;
  out_qos.peak_bandwidth = p_out_qos->peak_bandwidth;
  out_qos.access_latency = p_out_qos->access_latency;
  out_qos.delay_variation = p_out_qos->delay_variation;

  /* register HID Device with L2CAP and unregister HID Host with L2CAP */
  /* Disable HH */
  btif_hh_service_registration(FALSE);

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         unregister_app
 *
 * Description      Unregisters HID Device application
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t unregister_app(void) {
  log::verbose("");

  if (!btif_hd_cb.app_registered) {
    log::warn("application not yet registered");
    return BT_STATUS_NOT_READY;
  }

  if (btif_hd_cb.status != BTIF_HD_ENABLED) {
    log::warn("BT-HD not enabled, status={}", btif_hd_cb.status);
    return BT_STATUS_NOT_READY;
  }

  if (btif_hd_cb.service_dereg_active) {
    log::warn("BT-HD deregistering in progress");
    return BT_STATUS_BUSY;
  }

  btif_hd_cb.service_dereg_active = TRUE;
  BTA_HdUnregisterApp();

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         connect
 *
 * Description      Connects to host
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t connect(RawAddress* bd_addr) {
  log::verbose("");

  if (!btif_hd_cb.app_registered) {
    log::warn("application not yet registered");
    return BT_STATUS_NOT_READY;
  }

  if (btif_hd_cb.status != BTIF_HD_ENABLED) {
    log::warn("BT-HD not enabled, status={}", btif_hd_cb.status);
    return BT_STATUS_NOT_READY;
  }

  BTA_HdConnect(*bd_addr);

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         disconnect
 *
 * Description      Disconnects from host
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t disconnect(void) {
  log::verbose("");

  if (!btif_hd_cb.app_registered) {
    log::warn("application not yet registered");
    return BT_STATUS_NOT_READY;
  }

  if (btif_hd_cb.status != BTIF_HD_ENABLED) {
    log::warn("BT-HD not enabled, status={}", btif_hd_cb.status);
    return BT_STATUS_NOT_READY;
  }

  BTA_HdDisconnect();

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         send_report
 *
 * Description      Sends Reports to hid host
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t send_report(bthd_report_type_t type, uint8_t id, uint16_t len, uint8_t* p_data) {
  tBTA_HD_REPORT report;

  log::verbose("type={} id={} len={}", type, id, len);

  if (!btif_hd_cb.app_registered) {
    log::warn("application not yet registered");
    return BT_STATUS_NOT_READY;
  }

  if (btif_hd_cb.status != BTIF_HD_ENABLED) {
    log::warn("BT-HD not enabled, status={}", btif_hd_cb.status);
    return BT_STATUS_NOT_READY;
  }

  if (type == BTHD_REPORT_TYPE_INTRDATA) {
    report.type = BTHD_REPORT_TYPE_INPUT;
    report.use_intr = TRUE;
  } else {
    report.type = (type & 0x03);
    report.use_intr = FALSE;
  }

  report.id = id;
  report.len = len;
  report.p_data = p_data;

  BTA_HdSendReport(&report);

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         report_error
 *
 * Description      Sends HANDSHAKE with error info for invalid SET_REPORT
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t report_error(uint8_t error) {
  log::verbose("");

  if (!btif_hd_cb.app_registered) {
    log::warn("application not yet registered");
    return BT_STATUS_NOT_READY;
  }

  if (btif_hd_cb.status != BTIF_HD_ENABLED) {
    log::warn("BT-HD not enabled, status={}", btif_hd_cb.status);
    return BT_STATUS_NOT_READY;
  }

  BTA_HdReportError(error);

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         virtual_cable_unplug
 *
 * Description      Sends Virtual Cable Unplug to host
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t virtual_cable_unplug(void) {
  log::verbose("");

  if (!btif_hd_cb.app_registered) {
    log::warn("application not yet registered");
    return BT_STATUS_NOT_READY;
  }

  if (btif_hd_cb.status != BTIF_HD_ENABLED) {
    log::warn("BT-HD not enabled, status={}", btif_hd_cb.status);
    return BT_STATUS_NOT_READY;
  }

  BTA_HdVirtualCableUnplug();

  return BT_STATUS_SUCCESS;
}

static const bthd_interface_t bthdInterface = {
        sizeof(bthdInterface),
        init,
        cleanup,
        register_app,
        unregister_app,
        connect,
        disconnect,
        send_report,
        report_error,
        virtual_cable_unplug,
};

/*******************************************************************************
 *
 * Function         btif_hd_execute_service
 *
 * Description      Enabled/disables BT-HD service
 *
 * Returns          BT_STATUS_SUCCESS
 *
 ******************************************************************************/
bt_status_t btif_hd_execute_service(bool b_enable) {
  log::verbose("b_enable={}", b_enable);

  if (!b_enable) {
    BTA_HdDisable();
  }

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         btif_hd_get_interface
 *
 * Description      Gets BT-HD interface
 *
 * Returns          bthd_interface_t
 *
 ******************************************************************************/
const bthd_interface_t* btif_hd_get_interface() {
  log::verbose("");
  return &bthdInterface;
}

/*******************************************************************************
 *
 * Function         btif_hd_service_registration
 *
 * Description      Registers hid device service
 *
 * Returns          none
 *
 ******************************************************************************/
void btif_hd_service_registration() {
  log::verbose("");
  /* enable HD */
  if (bt_hd_callbacks != NULL) {
    BTA_HdEnable(bte_hd_evt);
  }
}
