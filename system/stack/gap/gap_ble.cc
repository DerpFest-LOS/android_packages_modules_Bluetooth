/******************************************************************************
 *
 *  Copyright 2017 The Android Open Source Project
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

#include <bluetooth/log.h>
#include <string.h>

#include <array>
#include <queue>

#include "gap_api.h"
#include "gap_int.h"
#include "gatt_api.h"
#include "hardware/bt_gatt_types.h"
#include "stack/include/bt_types.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/btm_status.h"
#include "types/bluetooth/uuid.h"
#include "types/bt_transport.h"
#include "types/raw_address.h"

using bluetooth::Uuid;
using namespace bluetooth;

namespace {

typedef struct {
  uint16_t uuid;
  tGAP_BLE_CMPL_CBACK* p_cback;
} tGAP_REQUEST;

typedef struct {
  RawAddress bda;
  tGAP_BLE_CMPL_CBACK* p_cback;
  tCONN_ID conn_id;
  uint16_t cl_op_uuid;
  bool connected;
  std::queue<tGAP_REQUEST> requests;
} tGAP_CLCB;

typedef struct {
  uint16_t handle;
  uint16_t uuid;
  tGAP_BLE_ATTR_VALUE attr_value;
} tGAP_ATTR;

static void server_attr_request_cback(tCONN_ID, uint32_t, tGATTS_REQ_TYPE, tGATTS_DATA*);
static void client_connect_cback(tGATT_IF, const RawAddress&, tCONN_ID, bool, tGATT_DISCONN_REASON,
                                 tBT_TRANSPORT);
static void client_cmpl_cback(tCONN_ID, tGATTC_OPTYPE, tGATT_STATUS, tGATT_CL_COMPLETE*);

tGATT_CBACK gap_cback = {
        .p_conn_cb = client_connect_cback,
        .p_cmpl_cb = client_cmpl_cback,
        .p_disc_res_cb = nullptr,
        .p_disc_cmpl_cb = nullptr,
        .p_req_cb = server_attr_request_cback,
        .p_enc_cmpl_cb = nullptr,
        .p_congestion_cb = nullptr,
        .p_phy_update_cb = nullptr,
        .p_conn_update_cb = nullptr,
        .p_subrate_chg_cb = nullptr,
};

constexpr int GAP_CHAR_DEV_NAME_SIZE = BD_NAME_LEN;
constexpr int GAP_MAX_CHAR_NUM = 4;

std::vector<tGAP_CLCB> gap_clcbs;
/* LE GAP attribute database */
std::array<tGAP_ATTR, GAP_MAX_CHAR_NUM> gatt_attr;
tGATT_IF gatt_if;

/** returns LCB with matching bd address, or nullptr */
static tGAP_CLCB* find_clcb_by_bd_addr(const RawAddress& bda) {
  for (auto& cb : gap_clcbs) {
    if (cb.bda == bda) {
      return &cb;
    }
  }

  return nullptr;
}

/** returns LCB with matching connection ID, or nullptr if not found  */
static tGAP_CLCB* ble_find_clcb_by_conn_id(tCONN_ID conn_id) {
  for (auto& cb : gap_clcbs) {
    if (cb.connected && cb.conn_id == conn_id) {
      return &cb;
    }
  }

  return nullptr;
}

/** allocates a GAP connection link control block */
static tGAP_CLCB* clcb_alloc(const RawAddress& bda) {
  gap_clcbs.emplace_back();
  tGAP_CLCB& cb = gap_clcbs.back();
  cb.bda = bda;
  return &cb;
}

/** The function clean up the pending request queue in GAP */
static void clcb_dealloc(tGAP_CLCB& clcb) {
  // put last element into place of current element, and remove last one - just
  // fast remove.
  for (auto it = gap_clcbs.begin(); it != gap_clcbs.end(); it++) {
    if (it->conn_id == clcb.conn_id) {
      auto last_one = std::prev(gap_clcbs.end());
      *it = *last_one;
      gap_clcbs.erase(last_one);
      return;
    }
  }
}

/** GAP Attributes Database Request callback */
static tGATT_STATUS read_attr_value(uint16_t handle, tGATT_VALUE* p_value, bool is_long) {
  uint8_t* p = p_value->value;
  uint16_t offset = p_value->offset;
  uint8_t* p_dev_name = NULL;

  for (const tGAP_ATTR& db_attr : gatt_attr) {
    const tGAP_BLE_ATTR_VALUE& attr_value = db_attr.attr_value;
    if (handle == db_attr.handle) {
      if (db_attr.uuid != GATT_UUID_GAP_DEVICE_NAME && is_long) {
        return GATT_NOT_LONG;
      }

      switch (db_attr.uuid) {
        case GATT_UUID_GAP_DEVICE_NAME:
          if (get_btm_client_interface().local.BTM_ReadLocalDeviceName((const char**)&p_dev_name) !=
              tBTM_STATUS::BTM_SUCCESS) {
            log::warn("Unable to read local device name");
          };
          if (strlen((char*)p_dev_name) > GATT_MAX_ATTR_LEN) {
            p_value->len = GATT_MAX_ATTR_LEN;
          } else {
            p_value->len = (uint16_t)strlen((char*)p_dev_name);
          }

          if (offset > p_value->len) {
            return GATT_INVALID_OFFSET;
          } else {
            p_value->len -= offset;
            p_dev_name += offset;
            ARRAY_TO_STREAM(p, p_dev_name, p_value->len);
          }
          break;

        case GATT_UUID_GAP_ICON:
          UINT16_TO_STREAM(p, attr_value.icon);
          p_value->len = 2;
          break;

        case GATT_UUID_GAP_PREF_CONN_PARAM:
          UINT16_TO_STREAM(p, attr_value.conn_param.int_min); /* int_min */
          UINT16_TO_STREAM(p, attr_value.conn_param.int_max); /* int_max */
          UINT16_TO_STREAM(p, attr_value.conn_param.latency); /* latency */
          UINT16_TO_STREAM(p, attr_value.conn_param.sp_tout); /* sp_tout */
          p_value->len = 8;
          break;

        /* address resolution */
        case GATT_UUID_GAP_CENTRAL_ADDR_RESOL:
          UINT8_TO_STREAM(p, attr_value.addr_resolution);
          p_value->len = 1;
          break;
      }
      return GATT_SUCCESS;
    }
  }
  return GATT_NOT_FOUND;
}

/** GAP Attributes Database Read/Read Blob Request process */
static tGATT_STATUS proc_read(tGATTS_REQ_TYPE, tGATT_READ_REQ* p_data, tGATTS_RSP* p_rsp) {
  if (p_data->is_long) {
    p_rsp->attr_value.offset = p_data->offset;
  }

  p_rsp->attr_value.handle = p_data->handle;

  return read_attr_value(p_data->handle, &p_rsp->attr_value, p_data->is_long);
}

/** GAP ATT server process a write request */
static tGATT_STATUS proc_write_req(tGATTS_REQ_TYPE, tGATT_WRITE_REQ* p_data) {
  for (const auto& db_addr : gatt_attr) {
    if (p_data->handle == db_addr.handle) {
      return GATT_WRITE_NOT_PERMIT;
    }
  }

  return GATT_NOT_FOUND;
}

/** GAP ATT server attribute access request callback */
static void server_attr_request_cback(tCONN_ID conn_id, uint32_t trans_id, tGATTS_REQ_TYPE type,
                                      tGATTS_DATA* p_data) {
  tGATT_STATUS status = GATT_INVALID_PDU;
  bool ignore = false;

  tGATTS_RSP rsp_msg;
  memset(&rsp_msg, 0, sizeof(tGATTS_RSP));

  switch (type) {
    case GATTS_REQ_TYPE_READ_CHARACTERISTIC:
    case GATTS_REQ_TYPE_READ_DESCRIPTOR:
      status = proc_read(type, &p_data->read_req, &rsp_msg);
      break;

    case GATTS_REQ_TYPE_WRITE_CHARACTERISTIC:
    case GATTS_REQ_TYPE_WRITE_DESCRIPTOR:
      if (!p_data->write_req.need_rsp) {
        ignore = true;
      }

      status = proc_write_req(type, &p_data->write_req);
      break;

    case GATTS_REQ_TYPE_WRITE_EXEC:
      ignore = true;
      log::verbose("Ignore GATTS_REQ_TYPE_WRITE_EXEC");
      break;

    case GATTS_REQ_TYPE_MTU:
      log::verbose("Get MTU exchange new mtu size: {}", p_data->mtu);
      ignore = true;
      break;

    default:
      log::verbose("Unknown/unexpected LE GAP ATT request: 0x{:02x}", type);
      break;
  }

  if (!ignore) {
    if (GATTS_SendRsp(conn_id, trans_id, status, &rsp_msg) != GATT_SUCCESS) {
      log::warn("Unable to send GATT ervier response conn_id:{}", conn_id);
    }
  }
}

/**
 * Utility function to send a read request for GAP characteristics.
 * Returns true if read started, else false if GAP is busy.
 */
static bool send_cl_read_request(tGAP_CLCB& clcb) {
  if (!clcb.requests.size() || clcb.cl_op_uuid != 0) {
    return false;
  }

  tGAP_REQUEST& req = clcb.requests.front();
  clcb.p_cback = req.p_cback;
  uint16_t uuid = req.uuid;
  clcb.requests.pop();

  tGATT_READ_PARAM param;
  memset(&param, 0, sizeof(tGATT_READ_PARAM));

  param.service.uuid = Uuid::From16Bit(uuid);
  param.service.s_handle = 1;
  param.service.e_handle = 0xFFFF;
  param.service.auth_req = 0;

  if (GATTC_Read(clcb.conn_id, GATT_READ_BY_TYPE, &param) == GATT_SUCCESS) {
    clcb.cl_op_uuid = uuid;
  }

  return true;
}

/** GAP client operation complete callback */
static void cl_op_cmpl(tGAP_CLCB& clcb, bool status, uint16_t len, uint8_t* p_name) {
  tGAP_BLE_CMPL_CBACK* p_cback = clcb.p_cback;
  uint16_t op = clcb.cl_op_uuid;

  clcb.cl_op_uuid = 0;
  clcb.p_cback = NULL;

  if (p_cback && op) {
    (*p_cback)(status, clcb.bda, len, (char*)p_name);
  }

  /* if no further activity is requested in callback, drop the link */
  if (clcb.connected) {
    if (!send_cl_read_request(clcb)) {
      if (GATT_Disconnect(clcb.conn_id) != GATT_SUCCESS) {
        log::warn("Unable to disconnect GATT conn_id:{}", clcb.conn_id);
      }
      clcb_dealloc(clcb);
    }
  }
}

/** Client connection callback */
static void client_connect_cback(tGATT_IF, const RawAddress& bda, tCONN_ID conn_id, bool connected,
                                 tGATT_DISCONN_REASON /* reason */, tBT_TRANSPORT) {
  tGAP_CLCB* p_clcb = find_clcb_by_bd_addr(bda);
  if (p_clcb == NULL) {
    log::info("No active GAP service found for peer:{} callback:{}", bda,
              (connected) ? "Connected" : "Disconnected");
    return;
  }

  if (connected) {
    log::debug("Connected GAP to remote device");
    p_clcb->conn_id = conn_id;
    p_clcb->connected = true;
    /* start operation is pending */
    send_cl_read_request(*p_clcb);
  } else {
    log::warn("Disconnected GAP from remote device");
    p_clcb->connected = false;
    cl_op_cmpl(*p_clcb, false, 0, NULL);
    /* clean up clcb */
    clcb_dealloc(*p_clcb);
  }
}

/** Client operation complete callback */
static void client_cmpl_cback(tCONN_ID conn_id, tGATTC_OPTYPE op, tGATT_STATUS status,
                              tGATT_CL_COMPLETE* p_data) {
  tGAP_CLCB* p_clcb = ble_find_clcb_by_conn_id(conn_id);
  uint16_t op_type;
  uint16_t min, max, latency, tout;
  uint16_t len;
  uint8_t* pp;

  if (p_clcb == NULL) {
    return;
  }

  op_type = p_clcb->cl_op_uuid;

  /* Currently we only issue read commands */
  if (op != GATTC_OPTYPE_READ) {
    return;
  }

  if (status != GATT_SUCCESS) {
    cl_op_cmpl(*p_clcb, false, 0, NULL);
    return;
  }

  pp = p_data->att_value.value;
  switch (op_type) {
    case GATT_UUID_GAP_PREF_CONN_PARAM:
      /* Extract the peripheral preferred connection parameters and save them */
      STREAM_TO_UINT16(min, pp);
      STREAM_TO_UINT16(max, pp);
      STREAM_TO_UINT16(latency, pp);
      STREAM_TO_UINT16(tout, pp);

      get_btm_client_interface().ble.BTM_BleSetPrefConnParams(p_clcb->bda, min, max, latency, tout);
      /* release the connection here */
      cl_op_cmpl(*p_clcb, true, 0, NULL);
      break;

    case GATT_UUID_GAP_DEVICE_NAME:
      len = (uint16_t)strlen((char*)pp);
      if (len > GAP_CHAR_DEV_NAME_SIZE) {
        len = GAP_CHAR_DEV_NAME_SIZE;
      }
      cl_op_cmpl(*p_clcb, true, len, pp);
      break;

    case GATT_UUID_GAP_CENTRAL_ADDR_RESOL:
      cl_op_cmpl(*p_clcb, true, 1, pp);
      break;

    case GATT_UUID_GAP_ICON:
      cl_op_cmpl(*p_clcb, true, p_data->att_value.len, pp);
      break;

    default:
      log::error("Unexpected operation {}", op);
      break;
  }
}

static bool accept_client_operation(const RawAddress& peer_bda, uint16_t uuid,
                                    tGAP_BLE_CMPL_CBACK* p_cback) {
  if (p_cback == NULL && uuid != GATT_UUID_GAP_PREF_CONN_PARAM) {
    return false;
  }

  tGAP_CLCB* p_clcb = find_clcb_by_bd_addr(peer_bda);
  if (p_clcb == NULL) {
    p_clcb = clcb_alloc(peer_bda);
  }

  if (GATT_GetConnIdIfConnected(gatt_if, peer_bda, &p_clcb->conn_id, BT_TRANSPORT_LE)) {
    p_clcb->connected = true;
  }

  if (!GATT_Connect(gatt_if, p_clcb->bda, BTM_BLE_DIRECT_CONNECTION, BT_TRANSPORT_LE, true)) {
    return false;
  }

  /* enqueue the request */
  p_clcb->requests.push({.uuid = uuid, .p_cback = p_cback});

  if (p_clcb->connected && p_clcb->cl_op_uuid == 0) {
    return send_cl_read_request(*p_clcb);
  } else { /* wait for connection up or pending operation to finish */
    return true;
  }
}

}  // namespace

/*******************************************************************************
 *
 * Function         gap_attr_db_init
 *
 * Description      GAP ATT database initialization.
 *
 * Returns          void.
 *
 ******************************************************************************/
void gap_attr_db_init(void) {
  /* Fill our internal UUID with a fixed pattern 0x82 */
  std::array<uint8_t, Uuid::kNumBytes128> tmp;
  tmp.fill(0x82);
  Uuid app_uuid = Uuid::From128BitBE(tmp);
  gatt_attr.fill({});

  gatt_if = GATT_Register(app_uuid, "Gap", &gap_cback, false);

  GATT_StartIf(gatt_if);

  Uuid svc_uuid = Uuid::From16Bit(UUID_SERVCLASS_GAP_SERVER);
  Uuid name_uuid = Uuid::From16Bit(GATT_UUID_GAP_DEVICE_NAME);
  Uuid icon_uuid = Uuid::From16Bit(GATT_UUID_GAP_ICON);
  Uuid addr_res_uuid = Uuid::From16Bit(GATT_UUID_GAP_CENTRAL_ADDR_RESOL);

  btgatt_db_element_t service[] = {{
                                           .uuid = svc_uuid,
                                           .type = BTGATT_DB_PRIMARY_SERVICE,
                                   },
                                   {.uuid = name_uuid,
                                    .type = BTGATT_DB_CHARACTERISTIC,
                                    .properties = GATT_CHAR_PROP_BIT_READ,
                                    .permissions = GATT_PERM_READ_IF_ENCRYPTED_OR_DISCOVERABLE},
                                   {.uuid = icon_uuid,
                                    .type = BTGATT_DB_CHARACTERISTIC,
                                    .properties = GATT_CHAR_PROP_BIT_READ,
                                    .permissions = GATT_PERM_READ},
                                   {.uuid = addr_res_uuid,
                                    .type = BTGATT_DB_CHARACTERISTIC,
                                    .properties = GATT_CHAR_PROP_BIT_READ,
                                    .permissions = GATT_PERM_READ}
#if (BTM_PERIPHERAL_ENABLED == TRUE) /* Only needed for peripheral testing */
                                   ,
                                   {.uuid = Uuid::From16Bit(GATT_UUID_GAP_PREF_CONN_PARAM),
                                    .type = BTGATT_DB_CHARACTERISTIC,
                                    .properties = GATT_CHAR_PROP_BIT_READ,
                                    .permissions = GATT_PERM_READ}
#endif
  };

  /* Add a GAP service */
  if (GATTS_AddService(gatt_if, service, sizeof(service) / sizeof(btgatt_db_element_t)) !=
      GATT_SERVICE_STARTED) {
    log::warn("Unable to add GATT services gatt_if:{}", gatt_if);
  }

  gatt_attr[0].uuid = GATT_UUID_GAP_DEVICE_NAME;
  gatt_attr[0].handle = service[1].attribute_handle;

  gatt_attr[1].uuid = GATT_UUID_GAP_ICON;
  gatt_attr[1].handle = service[2].attribute_handle;

  gatt_attr[2].uuid = GATT_UUID_GAP_CENTRAL_ADDR_RESOL;
  gatt_attr[2].handle = service[3].attribute_handle;
  gatt_attr[2].attr_value.addr_resolution = 0;

#if (BTM_PERIPHERAL_ENABLED == TRUE) /*  Only needed for peripheral testing */

  gatt_attr[3].uuid = GATT_UUID_GAP_PREF_CONN_PARAM;
  gatt_attr[3].attr_value.conn_param.int_max = GAP_PREFER_CONN_INT_MAX; /* 6 */
  gatt_attr[3].attr_value.conn_param.int_min = GAP_PREFER_CONN_INT_MIN; /* 0 */
  gatt_attr[3].attr_value.conn_param.latency = GAP_PREFER_CONN_LATENCY; /* 0 */
  gatt_attr[3].attr_value.conn_param.sp_tout = GAP_PREFER_CONN_SP_TOUT; /* 2000 */
  gatt_attr[3].handle = service[4].attribute_handle;
#endif
}

/*******************************************************************************
 *
 * Function         GAP_BleAttrDBUpdate
 *
 * Description      GAP ATT database update.
 *
 ******************************************************************************/
void GAP_BleAttrDBUpdate(uint16_t attr_uuid, tGAP_BLE_ATTR_VALUE* p_value) {
  for (tGAP_ATTR& db_attr : gatt_attr) {
    if (db_attr.uuid == attr_uuid) {
      switch (attr_uuid) {
        case GATT_UUID_GAP_ICON:
          db_attr.attr_value.icon = p_value->icon;
          break;

        case GATT_UUID_GAP_PREF_CONN_PARAM:
          memcpy((void*)&db_attr.attr_value.conn_param, (const void*)&p_value->conn_param,
                 sizeof(tGAP_BLE_PREF_PARAM));
          break;

        case GATT_UUID_GAP_DEVICE_NAME:
          if (get_btm_client_interface().local.BTM_SetLocalDeviceName(
                      (const char*)p_value->p_dev_name) != tBTM_STATUS::BTM_SUCCESS) {
            log::warn("Unable to set local name");
          }
          break;

        case GATT_UUID_GAP_CENTRAL_ADDR_RESOL:
          db_attr.attr_value.addr_resolution = p_value->addr_resolution;
          break;
      }
      break;
    }
  }

  return;
}

/*******************************************************************************
 *
 * Function         GAP_BleReadPeerPrefConnParams
 *
 * Description      Start a process to read a connected peripheral's preferred
 *                  connection parameters
 *
 * Returns          true if read started, else false if GAP is busy
 *
 ******************************************************************************/
bool GAP_BleReadPeerPrefConnParams(const RawAddress& peer_bda) {
  return accept_client_operation(peer_bda, GATT_UUID_GAP_PREF_CONN_PARAM, NULL);
}

/*******************************************************************************
 *
 * Function         GAP_BleReadPeerDevName
 *
 * Description      Start a process to read a connected peripheral's device
 *                  name.
 *
 * Returns          true if request accepted
 *
 ******************************************************************************/
bool GAP_BleReadPeerDevName(const RawAddress& peer_bda, tGAP_BLE_CMPL_CBACK* p_cback) {
  return accept_client_operation(peer_bda, GATT_UUID_GAP_DEVICE_NAME, p_cback);
}

/*******************************************************************************
 *
 * Function         GAP_BleReadPeerAppearance
 *
 * Description      Start a process to read a connected peripheral's appearance.
 *
 * Returns          true if request accepted
 *
 ******************************************************************************/
bool GAP_BleReadPeerAppearance(const RawAddress& peer_bda, tGAP_BLE_CMPL_CBACK* p_cback) {
  return accept_client_operation(peer_bda, GATT_UUID_GAP_ICON, p_cback);
}

/*******************************************************************************
 *
 * Function         GAP_BleCancelReadPeerDevName
 *
 * Description      Cancel reading a peripheral's device name.
 *
 * Returns          true if request accepted
 *
 ******************************************************************************/
bool GAP_BleCancelReadPeerDevName(const RawAddress& peer_bda) {
  tGAP_CLCB* p_clcb = find_clcb_by_bd_addr(peer_bda);

  if (p_clcb == NULL) {
    log::error("Cannot cancel current op is not get dev name");
    return false;
  }

  if (!p_clcb->connected) {
    if (!GATT_CancelConnect(gatt_if, peer_bda, true)) {
      log::error("Cannot cancel where No connection id");
      return false;
    }
  }

  cl_op_cmpl(*p_clcb, false, 0, NULL);

  return true;
}
