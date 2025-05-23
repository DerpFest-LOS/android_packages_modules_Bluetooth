/******************************************************************************
 *
 *  Copyright 2003-2012 Broadcom Corporation
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

/******************************************************************************
 *
 *  This file contains the GATT Server action functions for the state
 *  machine.
 *
 ******************************************************************************/

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <cstdint>

#include "bta/gatt/bta_gatts_int.h"
#include "bta/include/bta_api.h"
#include "btif/include/btif_debug_conn.h"
#include "internal_include/bt_target.h"
#include "internal_include/bt_trace.h"
#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "stack/include/gatt_api.h"
#include "types/raw_address.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using namespace bluetooth;

static void bta_gatts_nv_save_cback(bool is_saved, tGATTS_HNDL_RANGE* p_hndl_range);
static bool bta_gatts_nv_srv_chg_cback(tGATTS_SRV_CHG_CMD cmd, tGATTS_SRV_CHG_REQ* p_req,
                                       tGATTS_SRV_CHG_RSP* p_rsp);

static void bta_gatts_conn_cback(tGATT_IF gatt_if, const RawAddress& bda, tCONN_ID conn_id,
                                 bool connected, tGATT_DISCONN_REASON reason,
                                 tBT_TRANSPORT transport);
static void bta_gatts_send_request_cback(tCONN_ID conn_id, uint32_t trans_id,
                                         tGATTS_REQ_TYPE req_type, tGATTS_DATA* p_data);
static void bta_gatts_cong_cback(tCONN_ID conn_id, bool congested);
static void bta_gatts_phy_update_cback(tGATT_IF gatt_if, tCONN_ID conn_id, uint8_t tx_phy,
                                       uint8_t rx_phy, tGATT_STATUS status);
static void bta_gatts_conn_update_cback(tGATT_IF gatt_if, tCONN_ID conn_id, uint16_t interval,
                                        uint16_t latency, uint16_t timeout, tGATT_STATUS status);
static void bta_gatts_subrate_chg_cback(tGATT_IF gatt_if, tCONN_ID conn_id, uint16_t subrate_factor,
                                        uint16_t latency, uint16_t cont_num, uint16_t timeout,
                                        tGATT_STATUS status);

static tGATT_CBACK bta_gatts_cback = {
        .p_conn_cb = bta_gatts_conn_cback,
        .p_cmpl_cb = nullptr,
        .p_disc_res_cb = nullptr,
        .p_disc_cmpl_cb = nullptr,
        .p_req_cb = bta_gatts_send_request_cback,
        .p_enc_cmpl_cb = nullptr,
        .p_congestion_cb = bta_gatts_cong_cback,
        .p_phy_update_cb = bta_gatts_phy_update_cback,
        .p_conn_update_cb = bta_gatts_conn_update_cback,
        .p_subrate_chg_cb = bta_gatts_subrate_chg_cback,
};

tGATT_APPL_INFO bta_gatts_nv_cback = {bta_gatts_nv_save_cback, bta_gatts_nv_srv_chg_cback};

/*******************************************************************************
 *
 * Function         bta_gatts_nv_save_cback
 *
 * Description      NV save callback function.
 *
 * Parameter        is_add: true is to add a handle range; otherwise is to
 *                          delete.
 * Returns          none.
 *
 ******************************************************************************/
static void bta_gatts_nv_save_cback(bool /*is_add*/, tGATTS_HNDL_RANGE* /*p_hndl_range*/) {}

/*******************************************************************************
 *
 * Function         bta_gatts_nv_srv_chg_cback
 *
 * Description      NV save callback function.
 *
 * Parameter        is_add: true is to add a handle range; otherwise is to
 *                          delete.
 * Returns          none.
 *
 ******************************************************************************/
static bool bta_gatts_nv_srv_chg_cback(tGATTS_SRV_CHG_CMD /*cmd*/, tGATTS_SRV_CHG_REQ* /*p_req*/,
                                       tGATTS_SRV_CHG_RSP* /*p_rsp*/) {
  return false;
}

/*******************************************************************************
 *
 * Function         bta_gatts_enable
 *
 * Description      enable BTA GATTS module.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_enable(tBTA_GATTS_CB* p_cb) {
  if (p_cb->enabled) {
    log::verbose("GATTS already enabled.");
  } else {
    memset(p_cb, 0, sizeof(tBTA_GATTS_CB));

    p_cb->enabled = true;

    gatt_load_bonded();

    if (!GATTS_NVRegister(&bta_gatts_nv_cback)) {
      log::error("BTA GATTS NV register failed.");
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_api_disable
 *
 * Description      disable BTA GATTS module.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_api_disable(tBTA_GATTS_CB* p_cb) {
  uint8_t i;

  if (p_cb->enabled) {
    for (i = 0; i < BTA_GATTS_MAX_APP_NUM; i++) {
      if (p_cb->rcb[i].in_use) {
        GATT_Deregister(p_cb->rcb[i].gatt_if);
      }
    }
    memset(p_cb, 0, sizeof(tBTA_GATTS_CB));
  } else {
    log::error("GATTS not enabled");
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_register
 *
 * Description      register an application.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_register(tBTA_GATTS_CB* p_cb, tBTA_GATTS_DATA* p_msg) {
  tBTA_GATTS cb_data;
  tGATT_STATUS status = GATT_SUCCESS;
  uint8_t i, first_unuse = 0xff;

  if (!p_cb->enabled) {
    bta_gatts_enable(p_cb);
  }

  for (i = 0; i < BTA_GATTS_MAX_APP_NUM; i++) {
    if (p_cb->rcb[i].in_use) {
      if (p_cb->rcb[i].app_uuid == p_msg->api_reg.app_uuid) {
        log::error("application already registered.");
        status = GATT_DUP_REG;
        break;
      }
    }
  }

  if (status == GATT_SUCCESS) {
    for (i = 0; i < BTA_GATTS_MAX_APP_NUM; i++) {
      if (first_unuse == 0xff && !p_cb->rcb[i].in_use) {
        first_unuse = i;
        break;
      }
    }

    cb_data.reg_oper.server_if = BTA_GATTS_INVALID_IF;
    cb_data.reg_oper.uuid = p_msg->api_reg.app_uuid;
    if (first_unuse != 0xff) {
      log::info("register application first_unuse rcb_idx={}", first_unuse);

      p_cb->rcb[first_unuse].in_use = true;
      p_cb->rcb[first_unuse].p_cback = p_msg->api_reg.p_cback;
      p_cb->rcb[first_unuse].app_uuid = p_msg->api_reg.app_uuid;
      cb_data.reg_oper.server_if = p_cb->rcb[first_unuse].gatt_if = GATT_Register(
              p_msg->api_reg.app_uuid, "GattServer", &bta_gatts_cback, p_msg->api_reg.eatt_support);
      if (!p_cb->rcb[first_unuse].gatt_if) {
        status = GATT_NO_RESOURCES;
      } else {
        tBTA_GATTS_INT_START_IF* p_buf =
                (tBTA_GATTS_INT_START_IF*)osi_malloc(sizeof(tBTA_GATTS_INT_START_IF));
        p_buf->hdr.event = BTA_GATTS_INT_START_IF_EVT;
        p_buf->server_if = p_cb->rcb[first_unuse].gatt_if;

        bta_sys_sendmsg(p_buf);
      }
    } else {
      status = GATT_NO_RESOURCES;
    }
  }
  cb_data.reg_oper.status = status;
  if (p_msg->api_reg.p_cback) {
    (*p_msg->api_reg.p_cback)(BTA_GATTS_REG_EVT, &cb_data);
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_start_if
 *
 * Description      start an application interface.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_start_if(tBTA_GATTS_CB* /* p_cb */, tBTA_GATTS_DATA* p_msg) {
  if (bta_gatts_find_app_rcb_by_app_if(p_msg->int_start_if.server_if)) {
    GATT_StartIf(p_msg->int_start_if.server_if);
  } else {
    log::error("Unable to start app.: Unknown interface={}", p_msg->int_start_if.server_if);
  }
}
/*******************************************************************************
 *
 * Function         bta_gatts_deregister
 *
 * Description      deregister an application.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_deregister(tBTA_GATTS_CB* p_cb, tBTA_GATTS_DATA* p_msg) {
  tGATT_STATUS status = GATT_ERROR;
  tBTA_GATTS_CBACK* p_cback = NULL;
  uint8_t i;
  tBTA_GATTS cb_data;

  cb_data.reg_oper.server_if = p_msg->api_dereg.server_if;
  cb_data.reg_oper.status = status;

  for (i = 0; i < BTA_GATTS_MAX_APP_NUM; i++) {
    if (p_cb->rcb[i].in_use && p_cb->rcb[i].gatt_if == p_msg->api_dereg.server_if) {
      p_cback = p_cb->rcb[i].p_cback;
      status = GATT_SUCCESS;

      /* deregister the app */
      GATT_Deregister(p_cb->rcb[i].gatt_if);

      /* reset cb */
      memset(&p_cb->rcb[i], 0, sizeof(tBTA_GATTS_RCB));
      cb_data.reg_oper.status = status;
      break;
    }
  }

  if (p_cback) {
    (*p_cback)(BTA_GATTS_DEREG_EVT, &cb_data);
  } else {
    log::error("application not registered.");
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_delete_service
 *
 * Description      action function to delete a service.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_delete_service(tBTA_GATTS_SRVC_CB* p_srvc_cb, tBTA_GATTS_DATA* /*p_msg*/) {
  tBTA_GATTS_RCB* p_rcb = &bta_gatts_cb.rcb[p_srvc_cb->rcb_idx];
  tBTA_GATTS cb_data;

  cb_data.srvc_oper.server_if = p_rcb->gatt_if;
  cb_data.srvc_oper.service_id = p_srvc_cb->service_id;

  if (GATTS_DeleteService(p_rcb->gatt_if, &p_srvc_cb->service_uuid, p_srvc_cb->service_id)) {
    cb_data.srvc_oper.status = GATT_SUCCESS;
    memset(p_srvc_cb, 0, sizeof(tBTA_GATTS_SRVC_CB));
  } else {
    cb_data.srvc_oper.status = GATT_ERROR;
  }

  if (p_rcb->p_cback) {
    (*p_rcb->p_cback)(BTA_GATTS_DELETE_EVT, &cb_data);
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_stop_service
 *
 * Description      action function to stop a service.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_stop_service(tBTA_GATTS_SRVC_CB* p_srvc_cb, tBTA_GATTS_DATA* /* p_msg */) {
  tBTA_GATTS_RCB* p_rcb = &bta_gatts_cb.rcb[p_srvc_cb->rcb_idx];
  tBTA_GATTS cb_data;

  GATTS_StopService(p_srvc_cb->service_id);
  cb_data.srvc_oper.server_if = p_rcb->gatt_if;
  cb_data.srvc_oper.service_id = p_srvc_cb->service_id;
  cb_data.srvc_oper.status = GATT_SUCCESS;
  log::error("service_id={}", p_srvc_cb->service_id);

  if (p_rcb->p_cback) {
    (*p_rcb->p_cback)(BTA_GATTS_STOP_EVT, &cb_data);
  }
}
/*******************************************************************************
 *
 * Function         bta_gatts_send_rsp
 *
 * Description      GATTS send response.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_send_rsp(tBTA_GATTS_CB* /* p_cb */, tBTA_GATTS_DATA* p_msg) {
  auto conn_id = static_cast<tCONN_ID>(p_msg->api_rsp.hdr.layer_specific);
  if (GATTS_SendRsp(conn_id, p_msg->api_rsp.trans_id, p_msg->api_rsp.status,
                    (tGATTS_RSP*)p_msg->api_rsp.p_rsp) != GATT_SUCCESS) {
    log::error("Sending response failed");
  }
}
/*******************************************************************************
 *
 * Function         bta_gatts_indicate_handle
 *
 * Description      GATTS send handle value indication or notification.
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_indicate_handle(tBTA_GATTS_CB* p_cb, tBTA_GATTS_DATA* p_msg) {
  tBTA_GATTS_SRVC_CB* p_srvc_cb;
  tBTA_GATTS_RCB* p_rcb = NULL;
  tGATT_STATUS status = GATT_ILLEGAL_PARAMETER;
  tGATT_IF gatt_if;
  RawAddress remote_bda;
  tBT_TRANSPORT transport;
  tBTA_GATTS cb_data;

  p_srvc_cb = bta_gatts_find_srvc_cb_by_attr_id(p_cb, p_msg->api_indicate.attr_id);

  if (p_srvc_cb) {
    auto conn_id = static_cast<tCONN_ID>(p_msg->api_indicate.hdr.layer_specific);
    if (GATT_GetConnectionInfor(conn_id, &gatt_if, remote_bda, &transport)) {
      p_rcb = bta_gatts_find_app_rcb_by_app_if(gatt_if);

      if (p_msg->api_indicate.need_confirm) {
        status = GATTS_HandleValueIndication(conn_id, p_msg->api_indicate.attr_id,
                                             p_msg->api_indicate.len, p_msg->api_indicate.value);
      } else {
        status = GATTS_HandleValueNotification(conn_id, p_msg->api_indicate.attr_id,
                                               p_msg->api_indicate.len, p_msg->api_indicate.value);
      }

      /* if over BR_EDR, inform PM for mode change */
      if (transport == BT_TRANSPORT_BR_EDR) {
        bta_sys_busy(BTA_ID_GATTS, BTA_ALL_APP_ID, remote_bda);
        bta_sys_idle(BTA_ID_GATTS, BTA_ALL_APP_ID, remote_bda);
      }
    } else {
      log::error("Unknown connection_id=0x{:x} fail sending notification",
                 p_msg->api_indicate.hdr.layer_specific);
    }

    if ((status != GATT_SUCCESS || !p_msg->api_indicate.need_confirm) && p_rcb &&
        p_cb->rcb[p_srvc_cb->rcb_idx].p_cback) {
      cb_data.req_data.status = status;
      cb_data.req_data.conn_id = conn_id;

      (*p_rcb->p_cback)(BTA_GATTS_CONF_EVT, &cb_data);
    }
  } else {
    log::error("Not an registered servce attribute ID: 0x{:x}", p_msg->api_indicate.attr_id);
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_open
 *
 * Description
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_open(tBTA_GATTS_CB* /* p_cb */, tBTA_GATTS_DATA* p_msg) {
  tBTA_GATTS_RCB* p_rcb = NULL;
  tGATT_STATUS status = GATT_ERROR;
  tCONN_ID conn_id;

  p_rcb = bta_gatts_find_app_rcb_by_app_if(p_msg->api_open.server_if);
  if (p_rcb != NULL) {
    /* should always get the connection ID */
    bool success = false;
    if (com::android::bluetooth::flags::ble_gatt_server_use_address_type_in_connection()) {
      success = GATT_Connect(p_rcb->gatt_if, p_msg->api_open.remote_bda,
                             p_msg->api_open.remote_addr_type, p_msg->api_open.connection_type,
                             p_msg->api_open.transport, false, LE_PHY_1M, 0);
    } else {
      success = GATT_Connect(p_rcb->gatt_if, p_msg->api_open.remote_bda,
                             p_msg->api_open.connection_type, p_msg->api_open.transport, false);
    }

    if (success) {
      status = GATT_SUCCESS;
      if (GATT_GetConnIdIfConnected(p_rcb->gatt_if, p_msg->api_open.remote_bda, &conn_id,
                                    p_msg->api_open.transport)) {
        status = GATT_ALREADY_OPEN;
      }
    }
  } else {
    log::error("Inavlid server_if={}", p_msg->api_open.server_if);
  }

  if (p_rcb && p_rcb->p_cback) {
    tBTA_GATTS bta_gatts;
    bta_gatts.status = status;
    (*p_rcb->p_cback)(BTA_GATTS_OPEN_EVT, &bta_gatts);
  }
}
/*******************************************************************************
 *
 * Function         bta_gatts_cancel_open
 *
 * Description
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_cancel_open(tBTA_GATTS_CB* /* p_cb */, tBTA_GATTS_DATA* p_msg) {
  tBTA_GATTS_RCB* p_rcb;
  tGATT_STATUS status = GATT_ERROR;

  p_rcb = bta_gatts_find_app_rcb_by_app_if(p_msg->api_cancel_open.server_if);
  if (p_rcb != NULL) {
    if (!GATT_CancelConnect(p_rcb->gatt_if, p_msg->api_cancel_open.remote_bda,
                            p_msg->api_cancel_open.is_direct)) {
      log::error("failed for open request");
    } else {
      status = GATT_SUCCESS;
    }
  } else {
    log::error("Inavlid server_if={}", p_msg->api_cancel_open.server_if);
  }

  if (p_rcb && p_rcb->p_cback) {
    tBTA_GATTS bta_gatts;
    bta_gatts.status = status;
    (*p_rcb->p_cback)(BTA_GATTS_CANCEL_OPEN_EVT, &bta_gatts);
  }
}
/*******************************************************************************
 *
 * Function         bta_gatts_close
 *
 * Description
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gatts_close(tBTA_GATTS_CB* /* p_cb */, tBTA_GATTS_DATA* p_msg) {
  tBTA_GATTS_RCB* p_rcb;
  tGATT_STATUS status = GATT_ERROR;
  tGATT_IF gatt_if;
  RawAddress remote_bda;
  tBT_TRANSPORT transport;
  tCONN_ID conn_id = static_cast<tCONN_ID>(p_msg->hdr.layer_specific);

  if (GATT_GetConnectionInfor(conn_id, &gatt_if, remote_bda, &transport)) {
    log::debug("Disconnecting gatt_if={}, remote_bda={}, transport={}", gatt_if, remote_bda,
               transport);
    status = GATT_Disconnect(conn_id);
    if (status != GATT_SUCCESS) {
      log::error("fail conn_id={}", p_msg->hdr.layer_specific);
      status = GATT_ERROR;
    }

    p_rcb = bta_gatts_find_app_rcb_by_app_if(gatt_if);

    if (p_rcb && p_rcb->p_cback) {
      if (transport == BT_TRANSPORT_BR_EDR) {
        bta_sys_conn_close(BTA_ID_GATTS, BTA_ALL_APP_ID, remote_bda);
      }

      tBTA_GATTS bta_gatts;
      bta_gatts.status = status;
      (*p_rcb->p_cback)(BTA_GATTS_CLOSE_EVT, &bta_gatts);
    }
  } else {
    log::error("Unknown connection_id=0x{:x}", p_msg->hdr.layer_specific);
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_request_cback
 *
 * Description      GATTS attribute request callback.
 *
 * Returns          none.
 *
 ******************************************************************************/
static void bta_gatts_send_request_cback(tCONN_ID conn_id, uint32_t trans_id,
                                         tGATTS_REQ_TYPE req_type, tGATTS_DATA* p_data) {
  tBTA_GATTS cb_data;
  tBTA_GATTS_RCB* p_rcb;
  tGATT_IF gatt_if;
  tBT_TRANSPORT transport;

  memset(&cb_data, 0, sizeof(tBTA_GATTS));

  if (GATT_GetConnectionInfor(conn_id, &gatt_if, cb_data.req_data.remote_bda, &transport)) {
    p_rcb = bta_gatts_find_app_rcb_by_app_if(gatt_if);

    log::verbose("conn_id=0x{:x}, trans_id={}, req_type={}", conn_id, trans_id, req_type);

    if (p_rcb && p_rcb->p_cback) {
      /* if over BR_EDR, inform PM for mode change */
      if (transport == BT_TRANSPORT_BR_EDR) {
        bta_sys_busy(BTA_ID_GATTS, BTA_ALL_APP_ID, cb_data.req_data.remote_bda);
        bta_sys_idle(BTA_ID_GATTS, BTA_ALL_APP_ID, cb_data.req_data.remote_bda);
      }

      cb_data.req_data.conn_id = conn_id;
      cb_data.req_data.trans_id = trans_id;
      cb_data.req_data.p_data = (tGATTS_DATA*)p_data;

      (*p_rcb->p_cback)(req_type, &cb_data);
    } else {
      log::error("connection request on gatt_if={} is not interested", gatt_if);
    }
  } else {
    log::error("request received on unknown conn_id=0x{:x}", conn_id);
  }
}

/*******************************************************************************
 *
 * Function         bta_gatts_conn_cback
 *
 * Description      connection callback.
 *
 * Returns          none.
 *
 ******************************************************************************/
static void bta_gatts_conn_cback(tGATT_IF gatt_if, const RawAddress& bdaddr, tCONN_ID conn_id,
                                 bool connected, tGATT_DISCONN_REASON, tBT_TRANSPORT transport) {
  tBTA_GATTS cb_data;
  uint8_t evt = connected ? BTA_GATTS_CONNECT_EVT : BTA_GATTS_DISCONNECT_EVT;
  tBTA_GATTS_RCB* p_reg;

  log::verbose("bda={} gatt_if= {}, conn_id=0x{:x} connected={}", bdaddr, gatt_if, conn_id,
               connected);

  if (connected) {
    btif_debug_conn_state(bdaddr, BTIF_DEBUG_CONNECTED, GATT_CONN_OK);
  } else {
    btif_debug_conn_state(bdaddr, BTIF_DEBUG_DISCONNECTED, GATT_CONN_OK);
  }

  p_reg = bta_gatts_find_app_rcb_by_app_if(gatt_if);

  if (p_reg && p_reg->p_cback) {
    /* there is no RM for GATT */
    if (transport == BT_TRANSPORT_BR_EDR) {
      if (connected) {
        bta_sys_conn_open(BTA_ID_GATTS, BTA_ALL_APP_ID, bdaddr);
      } else {
        bta_sys_conn_close(BTA_ID_GATTS, BTA_ALL_APP_ID, bdaddr);
      }
    }

    cb_data.conn.conn_id = conn_id;
    cb_data.conn.server_if = gatt_if;
    cb_data.conn.transport = transport;
    cb_data.conn.remote_bda = bdaddr;
    (*p_reg->p_cback)(evt, &cb_data);
  } else {
    log::error("server_if={} not found", gatt_if);
  }
}

static void bta_gatts_phy_update_cback(tGATT_IF gatt_if, tCONN_ID conn_id, uint8_t tx_phy,
                                       uint8_t rx_phy, tGATT_STATUS status) {
  tBTA_GATTS_RCB* p_reg = bta_gatts_find_app_rcb_by_app_if(gatt_if);
  if (!p_reg || !p_reg->p_cback) {
    log::error("server_if={} not found", gatt_if);
    return;
  }

  tBTA_GATTS cb_data;
  cb_data.phy_update.conn_id = conn_id;
  cb_data.phy_update.server_if = gatt_if;
  cb_data.phy_update.tx_phy = tx_phy;
  cb_data.phy_update.rx_phy = rx_phy;
  cb_data.phy_update.status = status;
  (*p_reg->p_cback)(BTA_GATTS_PHY_UPDATE_EVT, &cb_data);
}

static void bta_gatts_conn_update_cback(tGATT_IF gatt_if, tCONN_ID conn_id, uint16_t interval,
                                        uint16_t latency, uint16_t timeout, tGATT_STATUS status) {
  tBTA_GATTS_RCB* p_reg = bta_gatts_find_app_rcb_by_app_if(gatt_if);
  if (!p_reg || !p_reg->p_cback) {
    log::error("server_if={} not found", gatt_if);
    return;
  }

  tBTA_GATTS cb_data;
  cb_data.conn_update.conn_id = conn_id;
  cb_data.conn_update.server_if = gatt_if;
  cb_data.conn_update.interval = interval;
  cb_data.conn_update.latency = latency;
  cb_data.conn_update.timeout = timeout;
  cb_data.conn_update.status = status;
  (*p_reg->p_cback)(BTA_GATTS_CONN_UPDATE_EVT, &cb_data);
}

static void bta_gatts_subrate_chg_cback(tGATT_IF gatt_if, tCONN_ID conn_id, uint16_t subrate_factor,
                                        uint16_t latency, uint16_t cont_num, uint16_t timeout,
                                        tGATT_STATUS status) {
  tBTA_GATTS_RCB* p_reg = bta_gatts_find_app_rcb_by_app_if(gatt_if);
  if (!p_reg || !p_reg->p_cback) {
    log::error("server_if={} not found", gatt_if);
    return;
  }

  tBTA_GATTS cb_data;
  cb_data.subrate_chg.conn_id = conn_id;
  cb_data.subrate_chg.server_if = gatt_if;
  cb_data.subrate_chg.subrate_factor = subrate_factor;
  cb_data.subrate_chg.latency = latency;
  cb_data.subrate_chg.cont_num = cont_num;
  cb_data.subrate_chg.timeout = timeout;
  cb_data.subrate_chg.status = status;
  (*p_reg->p_cback)(BTA_GATTS_SUBRATE_CHG_EVT, &cb_data);
}

/*******************************************************************************
 *
 * Function         bta_gatts_cong_cback
 *
 * Description      congestion callback.
 *
 * Returns          none.
 *
 ******************************************************************************/
static void bta_gatts_cong_cback(tCONN_ID conn_id, bool congested) {
  tBTA_GATTS_RCB* p_rcb;
  tGATT_IF gatt_if;
  tBT_TRANSPORT transport;
  tBTA_GATTS cb_data;

  if (GATT_GetConnectionInfor(conn_id, &gatt_if, cb_data.req_data.remote_bda, &transport)) {
    p_rcb = bta_gatts_find_app_rcb_by_app_if(gatt_if);

    if (p_rcb && p_rcb->p_cback) {
      cb_data.congest.conn_id = conn_id;
      cb_data.congest.congested = congested;

      (*p_rcb->p_cback)(BTA_GATTS_CONGEST_EVT, &cb_data);
    }
  }
}
