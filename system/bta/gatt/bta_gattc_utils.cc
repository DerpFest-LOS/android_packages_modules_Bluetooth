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
 *  This file contains the GATT client utility function.
 *
 ******************************************************************************/

#define LOG_TAG "bt_bta_gattc"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <cstdint>

#include "bta/gatt/bta_gattc_int.h"
#include "hci/controller_interface.h"
#include "internal_include/bt_target.h"
#include "internal_include/bt_trace.h"
#include "main/shim/entry.h"
#include "osi/include/allocator.h"
#include "types/bt_transport.h"
#include "types/hci_role.h"
#include "types/raw_address.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using namespace bluetooth;

static uint8_t ble_acceptlist_size() {
  if (!bluetooth::shim::GetController()->SupportsBle()) {
    return 0;
  }
  return bluetooth::shim::GetController()->GetLeFilterAcceptListSize();
}

/*******************************************************************************
 *
 * Function         bta_gattc_cl_get_regcb
 *
 * Description      get registration control block by client interface.
 *
 * Returns          pointer to the regcb
 *
 ******************************************************************************/
tBTA_GATTC_RCB* bta_gattc_cl_get_regcb(uint8_t client_if) {
  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    auto it = bta_gattc_cb.cl_rcb_map.find(client_if);
    if (it == bta_gattc_cb.cl_rcb_map.end()) {
      return NULL;
    } else {
      return it->second.get();
    }
  } else {
    uint8_t i = 0;
    tBTA_GATTC_RCB* p_clrcb = &bta_gattc_cb.cl_rcb[0];

    for (i = 0; i < BTA_GATTC_CL_MAX; i++, p_clrcb++) {
      if (p_clrcb->in_use && p_clrcb->client_if == client_if) {
        return p_clrcb;
      }
    }
    return NULL;
  }
}
/*******************************************************************************
 *
 * Function         bta_gattc_num_reg_app
 *
 * Description      find the number of registered application.
 *
 * Returns          pointer to the regcb
 *
 ******************************************************************************/
uint8_t bta_gattc_num_reg_app(void) {
  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    return (uint8_t)bta_gattc_cb.cl_rcb_map.size();
  } else {
    uint8_t i = 0, j = 0;

    for (i = 0; i < BTA_GATTC_CL_MAX; i++) {
      if (bta_gattc_cb.cl_rcb[i].in_use) {
        j++;
      }
    }
    return j;
  }
}
/*******************************************************************************
 *
 * Function         bta_gattc_find_clcb_by_cif
 *
 * Description      get clcb by client interface and remote bd adddress
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CLCB* bta_gattc_find_clcb_by_cif(uint8_t client_if, const RawAddress& remote_bda,
                                            tBT_TRANSPORT transport) {
  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    for (auto& p_clcb : bta_gattc_cb.clcb_set) {
      if (p_clcb->in_use && p_clcb->p_rcb->client_if == client_if &&
          p_clcb->transport == transport && p_clcb->bda == remote_bda) {
        return p_clcb.get();
      }
    }
  } else {
    tBTA_GATTC_CLCB* p_clcb = &bta_gattc_cb.clcb[0];

    for (size_t i = 0; i < BTA_GATTC_CLCB_MAX; i++, p_clcb++) {
      if (p_clcb->in_use && p_clcb->p_rcb->client_if == client_if &&
          p_clcb->transport == transport && p_clcb->bda == remote_bda) {
        return p_clcb;
      }
    }
  }
  return NULL;
}
/*******************************************************************************
 *
 * Function         bta_gattc_find_clcb_by_conn_id
 *
 * Description      get clcb by connection ID
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CLCB* bta_gattc_find_clcb_by_conn_id(tCONN_ID conn_id) {
  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    for (auto& p_clcb : bta_gattc_cb.clcb_set) {
      if (p_clcb->in_use && p_clcb->bta_conn_id == conn_id) {
        return p_clcb.get();
      }
    }
  } else {
    tBTA_GATTC_CLCB* p_clcb = &bta_gattc_cb.clcb[0];

    for (size_t i = 0; i < BTA_GATTC_CLCB_MAX; i++, p_clcb++) {
      if (p_clcb->in_use && p_clcb->bta_conn_id == conn_id) {
        return p_clcb;
      }
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_gattc_clcb_alloc
 *
 * Description      allocate CLCB
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CLCB* bta_gattc_clcb_alloc(tGATT_IF client_if, const RawAddress& remote_bda,
                                      tBT_TRANSPORT transport) {
  tBTA_GATTC_CLCB* p_clcb = NULL;

  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    bta_gattc_cleanup_clcb();
    auto [p_clcb_i, b] = bta_gattc_cb.clcb_set.emplace(std::make_unique<tBTA_GATTC_CLCB>());
    p_clcb = p_clcb_i->get();

    p_clcb->in_use = true;
    p_clcb->status = GATT_SUCCESS;
    p_clcb->transport = transport;
    p_clcb->bda = remote_bda;
    p_clcb->p_q_cmd = NULL;

    p_clcb->p_rcb = bta_gattc_cl_get_regcb(client_if);

    p_clcb->p_srcb = bta_gattc_find_srcb(remote_bda);
    if (p_clcb->p_srcb == NULL) {
      p_clcb->p_srcb = bta_gattc_srcb_alloc(remote_bda);
    }

    if (p_clcb->p_rcb != NULL && p_clcb->p_srcb != NULL) {
      p_clcb->p_srcb->num_clcb++;
      p_clcb->p_rcb->num_clcb++;
    } else {
      /* release this clcb if clcb or srcb allocation failed */
      bta_gattc_cb.clcb_set.erase(p_clcb_i);
      p_clcb = NULL;
    }
  } else {
    for (int i_clcb = 0; i_clcb < BTA_GATTC_CLCB_MAX; i_clcb++) {
      if (!bta_gattc_cb.clcb[i_clcb].in_use) {
#if (BTA_GATT_DEBUG == TRUE)
        log::verbose("found clcb:{} available", i_clcb);
#endif
        p_clcb = &bta_gattc_cb.clcb[i_clcb];
        p_clcb->in_use = true;
        p_clcb->status = GATT_SUCCESS;
        p_clcb->transport = transport;
        p_clcb->bda = remote_bda;
        p_clcb->p_q_cmd = NULL;

        p_clcb->p_rcb = bta_gattc_cl_get_regcb(client_if);

        p_clcb->p_srcb = bta_gattc_find_srcb(remote_bda);
        if (p_clcb->p_srcb == NULL) {
          p_clcb->p_srcb = bta_gattc_srcb_alloc(remote_bda);
        }

        if (p_clcb->p_rcb != NULL && p_clcb->p_srcb != NULL) {
          p_clcb->p_srcb->num_clcb++;
          p_clcb->p_rcb->num_clcb++;
        } else {
          /* release this clcb if clcb or srcb allocation failed */
          p_clcb->in_use = false;
          p_clcb = NULL;
        }
        break;
      }
    }
  }
  return p_clcb;
}
/*******************************************************************************
 *
 * Function         bta_gattc_find_alloc_clcb
 *
 * Description      find or allocate CLCB if not found.
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CLCB* bta_gattc_find_alloc_clcb(tGATT_IF client_if, const RawAddress& remote_bda,
                                           tBT_TRANSPORT transport) {
  tBTA_GATTC_CLCB* p_clcb;

  p_clcb = bta_gattc_find_clcb_by_cif(client_if, remote_bda, transport);
  if (p_clcb == NULL) {
    p_clcb = bta_gattc_clcb_alloc(client_if, remote_bda, transport);
  }
  return p_clcb;
}

/*******************************************************************************
 *
 * Function         bta_gattc_server_disconnected
 *
 * Description      Set server cache disconnected
 *
 * Returns          pointer to the srcb
 *
 ******************************************************************************/
void bta_gattc_server_disconnected(tBTA_GATTC_SERV* p_srcb) {
  if (p_srcb && p_srcb->connected) {
    p_srcb->connected = false;
    p_srcb->state = BTA_GATTC_SERV_IDLE;
    p_srcb->mtu = 0;

    // clear reallocating
    p_srcb->gatt_database.Clear();
  }
}

/*******************************************************************************
 *
 * Function         bta_gattc_clcb_dealloc
 *
 * Description      Deallocte a clcb
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
void bta_gattc_clcb_dealloc(tBTA_GATTC_CLCB* p_clcb) {
  if (!p_clcb) {
    log::error("p_clcb=NULL");
    return;
  }

  tBTA_GATTC_SERV* p_srcb = p_clcb->p_srcb;
  if (p_srcb->num_clcb) {
    p_srcb->num_clcb--;
  }

  if (p_clcb->p_rcb->num_clcb) {
    p_clcb->p_rcb->num_clcb--;
  }

  /* if the srcb is no longer needed, reset the state */
  if (p_srcb->num_clcb == 0) {
    p_srcb->connected = false;
    p_srcb->state = BTA_GATTC_SERV_IDLE;
    p_srcb->mtu = 0;

    // clear reallocating
    p_srcb->gatt_database.Clear();
  }

  while (!p_clcb->p_q_cmd_queue.empty()) {
    auto p_q_cmd = p_clcb->p_q_cmd_queue.front();
    p_clcb->p_q_cmd_queue.pop_front();
    osi_free_and_reset((void**)&p_q_cmd);
  }

  if (p_clcb->p_q_cmd != NULL) {
    osi_free_and_reset((void**)&p_clcb->p_q_cmd);
  }

  /* Clear p_clcb. Some of the fields are already reset e.g. p_q_cmd_queue and
   * p_q_cmd. */
  p_clcb->bta_conn_id = 0;
  p_clcb->bda = {};
  p_clcb->transport = BT_TRANSPORT_AUTO;
  p_clcb->p_rcb = NULL;
  p_clcb->p_srcb = NULL;
  p_clcb->request_during_discovery = 0;
  p_clcb->auto_update = 0;
  p_clcb->disc_active = 0;
  p_clcb->in_use = 0;
  p_clcb->state = BTA_GATTC_IDLE_ST;
  p_clcb->status = GATT_SUCCESS;
  // in bta_gattc_sm_execute(), p_clcb is accessed again so we dealloc clcb later.
  // it will be claned up when the client is deregistered or a new clcb is allocated.
  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    bta_gattc_cb.clcb_pending_dealloc.insert(p_clcb);
  }
}

/*******************************************************************************
 *
 * Function         bta_gattc_cleanup_clcb
 *
 * Description      cleans up resources from deallocated clcb
 *
 * Returns          none
 *
 ******************************************************************************/
void bta_gattc_cleanup_clcb() {
  if (bta_gattc_cb.clcb_pending_dealloc.empty()) {
    return;
  }
  auto it = bta_gattc_cb.clcb_set.begin();
  while (it != bta_gattc_cb.clcb_set.end()) {
    if (bta_gattc_cb.clcb_pending_dealloc.contains(it->get())) {
      it = bta_gattc_cb.clcb_set.erase(it);
    } else {
      it++;
    }
  }
  bta_gattc_cb.clcb_pending_dealloc.clear();
}

/*******************************************************************************
 *
 * Function         bta_gattc_find_srcb
 *
 * Description      find server cache by remote bd address currently in use
 *
 * Returns          pointer to the server cache.
 *
 ******************************************************************************/
tBTA_GATTC_SERV* bta_gattc_find_srcb(const RawAddress& bda) {
  tBTA_GATTC_SERV* p_srcb = &bta_gattc_cb.known_server[0];
  uint8_t i;

  for (i = 0; i < ble_acceptlist_size(); i++, p_srcb++) {
    if (p_srcb->in_use && p_srcb->server_bda == bda) {
      return p_srcb;
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_gattc_find_srvr_cache
 *
 * Description      find server cache by remote bd address
 *
 * Returns          pointer to the server cache.
 *
 ******************************************************************************/
tBTA_GATTC_SERV* bta_gattc_find_srvr_cache(const RawAddress& bda) {
  tBTA_GATTC_SERV* p_srcb = &bta_gattc_cb.known_server[0];
  uint8_t i;

  for (i = 0; i < ble_acceptlist_size(); i++, p_srcb++) {
    if (p_srcb->server_bda == bda) {
      return p_srcb;
    }
  }
  return NULL;
}
/*******************************************************************************
 *
 * Function         bta_gattc_find_scb_by_cid
 *
 * Description      find server control block by connection ID
 *
 * Returns          pointer to the server cache.
 *
 ******************************************************************************/
tBTA_GATTC_SERV* bta_gattc_find_scb_by_cid(tCONN_ID conn_id) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  if (p_clcb) {
    return p_clcb->p_srcb;
  } else {
    return NULL;
  }
}
/*******************************************************************************
 *
 * Function         bta_gattc_srcb_alloc
 *
 * Description      allocate server cache control block
 *
 * Returns          pointer to the server cache.
 *
 ******************************************************************************/
tBTA_GATTC_SERV* bta_gattc_srcb_alloc(const RawAddress& bda) {
  tBTA_GATTC_SERV *p_tcb = &bta_gattc_cb.known_server[0], *p_recycle = NULL;
  bool found = false;
  uint8_t i;

  for (i = 0; i < ble_acceptlist_size(); i++, p_tcb++) {
    if (!p_tcb->in_use) {
      found = true;
      break;
    } else if (!p_tcb->connected) {
      p_recycle = p_tcb;
    }
  }

  /* if not found, try to recycle one known device */
  if (!found && !p_recycle) {
    p_tcb = NULL;
  } else if (!found && p_recycle) {
    p_tcb = p_recycle;
  }

  if (p_tcb != NULL) {
    // clear reallocating
    p_tcb->gatt_database.Clear();
    p_tcb->pending_discovery.Clear();
    *p_tcb = tBTA_GATTC_SERV();

    p_tcb->in_use = true;
    p_tcb->server_bda = bda;
  }
  return p_tcb;
}

void bta_gattc_send_mtu_response(tBTA_GATTC_CLCB* p_clcb, const tBTA_GATTC_DATA* p_data,
                                 uint16_t current_mtu) {
  GATT_CONFIGURE_MTU_OP_CB cb = p_data->api_mtu.mtu_cb;
  if (cb) {
    void* my_cb_data = p_data->api_mtu.mtu_cb_data;
    cb(p_clcb->bta_conn_id, GATT_SUCCESS, my_cb_data);
  }

  tBTA_GATTC cb_data;
  p_clcb->status = GATT_SUCCESS;
  cb_data.cfg_mtu.conn_id = p_clcb->bta_conn_id;
  cb_data.cfg_mtu.status = GATT_SUCCESS;

  cb_data.cfg_mtu.mtu = current_mtu;

  if (p_clcb->p_rcb) {
    (*p_clcb->p_rcb->p_cback)(BTA_GATTC_CFG_MTU_EVT, &cb_data);
  }
}

void bta_gattc_continue(tBTA_GATTC_CLCB* p_clcb) {
  if (p_clcb->p_q_cmd != NULL) {
    log::info("Already scheduled another request for conn_id = 0x{:04x}", p_clcb->bta_conn_id);
    return;
  }

  while (!p_clcb->p_q_cmd_queue.empty()) {
    const tBTA_GATTC_DATA* p_q_cmd = p_clcb->p_q_cmd_queue.front();
    if (p_q_cmd->hdr.event != BTA_GATTC_API_CFG_MTU_EVT) {
      p_clcb->p_q_cmd_queue.pop_front();
      bta_gattc_sm_execute(p_clcb, p_q_cmd->hdr.event, p_q_cmd);
      return;
    }

    /* The p_q_cmd is the MTU Request event. */
    uint16_t current_mtu = 0;
    auto result =
            GATTC_TryMtuRequest(p_clcb->bda, p_clcb->transport, p_clcb->bta_conn_id, &current_mtu);
    switch (result) {
      case MTU_EXCHANGE_DEVICE_DISCONNECTED:
        bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_CONFIG, GATT_NO_RESOURCES, NULL);
        /* Handled, free command below and continue with a p_q_cmd_queue */
        break;
      case MTU_EXCHANGE_NOT_ALLOWED:
        bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_CONFIG, GATT_ERR_UNLIKELY, NULL);
        /* Handled, free command below and continue with a p_q_cmd_queue */
        break;
      case MTU_EXCHANGE_ALREADY_DONE:
        bta_gattc_send_mtu_response(p_clcb, p_q_cmd, current_mtu);
        /* Handled, free command below and continue with a p_q_cmd_queue */
        break;
      case MTU_EXCHANGE_IN_PROGRESS:
        log::warn("Waiting p_clcb {}", std::format_ptr(p_clcb));
        return;
      case MTU_EXCHANGE_NOT_DONE_YET:
        p_clcb->p_q_cmd_queue.pop_front();
        bta_gattc_sm_execute(p_clcb, p_q_cmd->hdr.event, p_q_cmd);
        return;
    }

    /* p_q_cmd was the MTU request and it was handled.
     * If MTU request was handled without actually ATT request,
     * it is ok to take another message from the queue and proceed.
     */
    p_clcb->p_q_cmd_queue.pop_front();
    osi_free_and_reset((void**)&p_q_cmd);
  }
}

bool bta_gattc_is_data_queued(tBTA_GATTC_CLCB* p_clcb, const tBTA_GATTC_DATA* p_data) {
  if (p_clcb->p_q_cmd == p_data) {
    return true;
  }

  auto it = std::find(p_clcb->p_q_cmd_queue.begin(), p_clcb->p_q_cmd_queue.end(), p_data);
  return it != p_clcb->p_q_cmd_queue.end();
}
/*******************************************************************************
 *
 * Function         bta_gattc_enqueue
 *
 * Description      enqueue a client request in clcb.
 *
 * Returns          BtaEnqueuedResult_t
 *
 ******************************************************************************/
BtaEnqueuedResult_t bta_gattc_enqueue(tBTA_GATTC_CLCB* p_clcb, const tBTA_GATTC_DATA* p_data) {
  if (p_clcb->p_q_cmd == NULL) {
    p_clcb->p_q_cmd = p_data;
    return ENQUEUED_READY_TO_SEND;
  }

  log::info("Already has a pending command to executer. Queuing for later {} conn id=0x{:04x}",
            p_clcb->bda, p_clcb->bta_conn_id);
  p_clcb->p_q_cmd_queue.push_back(p_data);

  return ENQUEUED_FOR_LATER;
}

/*******************************************************************************
 *
 * Function         bta_gattc_check_notif_registry
 *
 * Description      check if the service notificaition has been registered.
 *
 * Returns
 *
 ******************************************************************************/
bool bta_gattc_check_notif_registry(tBTA_GATTC_RCB* p_clreg, tBTA_GATTC_SERV* p_srcb,
                                    tBTA_GATTC_NOTIFY* p_notify) {
  uint8_t i;

  for (i = 0; i < BTA_GATTC_NOTIF_REG_MAX; i++) {
    if (p_clreg->notif_reg[i].in_use && p_clreg->notif_reg[i].remote_bda == p_srcb->server_bda &&
        p_clreg->notif_reg[i].handle == p_notify->handle &&
        !p_clreg->notif_reg[i].app_disconnected) {
      log::verbose("Notification registered!");
      return true;
    }
  }
  return false;
}
/*******************************************************************************
 *
 * Function         bta_gattc_clear_notif_registration
 *
 * Description      Clear up the notification registration information by
 *                  RawAddress.
 *                  Where handle is between start_handle and end_handle, and
 *                  start_handle and end_handle are boundaries of service
 *                  containing characteristic.
 *
 * Returns          None.
 *
 ******************************************************************************/
void bta_gattc_clear_notif_registration(tBTA_GATTC_SERV* /*p_srcb*/, tCONN_ID conn_id,
                                        uint16_t start_handle, uint16_t end_handle) {
  RawAddress remote_bda;
  tGATT_IF gatt_if;
  tBTA_GATTC_RCB* p_clrcb;
  uint8_t i;
  tBT_TRANSPORT transport;
  uint16_t handle;

  if (GATT_GetConnectionInfor(conn_id, &gatt_if, remote_bda, &transport)) {
    p_clrcb = bta_gattc_cl_get_regcb(gatt_if);
    if (p_clrcb != NULL) {
      for (i = 0; i < BTA_GATTC_NOTIF_REG_MAX; i++) {
        if (p_clrcb->notif_reg[i].in_use && p_clrcb->notif_reg[i].remote_bda == remote_bda) {
          /* It's enough to get service or characteristic handle, as
           * clear boundaries are always around service.
           */
          handle = p_clrcb->notif_reg[i].handle;
          if (handle >= start_handle && handle <= end_handle) {
            memset(&p_clrcb->notif_reg[i], 0, sizeof(tBTA_GATTC_NOTIF_REG));
          }
        }
      }
    }
  } else {
    log::error("can not clear indication/notif registration for unknown app");
  }
  return;
}

/*******************************************************************************
 *
 * Function         bta_gattc_mark_bg_conn
 *
 * Description      mark background connection status when a bg connection is
 *                  initiated or terminated.
 *
 * Returns          true if success; false otherwise.
 *
 ******************************************************************************/
bool bta_gattc_mark_bg_conn(tGATT_IF client_if, const RawAddress& remote_bda_ptr, bool add) {
  tBTA_GATTC_BG_TCK* p_bg_tck = &bta_gattc_cb.bg_track[0];
  uint8_t i = 0;
  tBTA_GATTC_CIF_MASK* p_cif_mask;

  for (i = 0; i < ble_acceptlist_size(); i++, p_bg_tck++) {
    if (p_bg_tck->in_use &&
        ((p_bg_tck->remote_bda == remote_bda_ptr) || (p_bg_tck->remote_bda.IsEmpty()))) {
      if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
        auto& p_cif_set = p_bg_tck->cif_set;
        if (add) { /* mask on the cif bit */
          p_cif_set.insert(client_if);
        } else {
          if (client_if != 0) {
            p_cif_set.erase(client_if);
          } else {
            p_cif_set.clear();
          }
        }
        /* no BG connection for this device, make it available */
        if (p_bg_tck->cif_set.empty()) {
          p_bg_tck->in_use = false;
          p_bg_tck->remote_bda = RawAddress::kEmpty;
        }
      } else {
        p_cif_mask = &p_bg_tck->cif_mask;

        if (add) { /* mask on the cif bit */
          *p_cif_mask |= (1 << (client_if - 1));
        } else {
          if (client_if != 0) {
            *p_cif_mask &= (~(1 << (client_if - 1)));
          } else {
            *p_cif_mask = 0;
          }
        }
        /* no BG connection for this device, make it available */
        if (p_bg_tck->cif_mask == 0) {
          *p_bg_tck = tBTA_GATTC_BG_TCK{};
        }
      }
      return true;
    }
  }
  if (!add) {
    log::error("unable to find the bg connection mask for bd_addr={}", remote_bda_ptr);
    return false;
  } else { /* adding a new device mask */
    for (i = 0, p_bg_tck = &bta_gattc_cb.bg_track[0]; i < ble_acceptlist_size(); i++, p_bg_tck++) {
      if (!p_bg_tck->in_use) {
        p_bg_tck->in_use = true;
        p_bg_tck->remote_bda = remote_bda_ptr;

        if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
          p_bg_tck->cif_set = {client_if};
        } else {
          p_cif_mask = &p_bg_tck->cif_mask;
          *p_cif_mask = ((tBTA_GATTC_CIF_MASK)1 << (client_if - 1));
        }
        return true;
      }
    }
    log::error("no available space to mark the bg connection status");
    return false;
  }
}
/*******************************************************************************
 *
 * Function         bta_gattc_check_bg_conn
 *
 * Description      check if this is a background connection background
 *                  connection.
 *
 * Returns          true if success; false otherwise.
 *
 ******************************************************************************/
bool bta_gattc_check_bg_conn(tGATT_IF client_if, const RawAddress& remote_bda, uint8_t role) {
  tBTA_GATTC_BG_TCK* p_bg_tck = &bta_gattc_cb.bg_track[0];
  uint8_t i = 0;
  bool is_bg_conn = false;

  for (i = 0; i < ble_acceptlist_size() && !is_bg_conn; i++, p_bg_tck++) {
    if (p_bg_tck->in_use &&
        (p_bg_tck->remote_bda == remote_bda || p_bg_tck->remote_bda.IsEmpty())) {
      if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
        if (p_bg_tck->cif_set.contains(client_if) && role == HCI_ROLE_CENTRAL) {
          is_bg_conn = true;
        }
      } else {
        if (((p_bg_tck->cif_mask & ((tBTA_GATTC_CIF_MASK)1 << (client_if - 1))) != 0) &&
            role == HCI_ROLE_CENTRAL) {
          is_bg_conn = true;
        }
      }
    }
  }
  return is_bg_conn;
}
/*******************************************************************************
 *
 * Function         bta_gattc_send_open_cback
 *
 * Description      send open callback
 *
 * Returns
 *
 ******************************************************************************/
void bta_gattc_send_open_cback(tBTA_GATTC_RCB* p_clreg, tGATT_STATUS status,
                               const RawAddress& remote_bda, tCONN_ID conn_id,
                               tBT_TRANSPORT transport, uint16_t mtu) {
  tBTA_GATTC cb_data;

  if (p_clreg->p_cback) {
    memset(&cb_data, 0, sizeof(tBTA_GATTC));

    cb_data.open.status = status;
    cb_data.open.client_if = p_clreg->client_if;
    cb_data.open.conn_id = conn_id;
    cb_data.open.mtu = mtu;
    cb_data.open.transport = transport;
    cb_data.open.remote_bda = remote_bda;

    (*p_clreg->p_cback)(BTA_GATTC_OPEN_EVT, &cb_data);
  }
}
/*******************************************************************************
 *
 * Function         bta_gattc_conn_alloc
 *
 * Description      allocate connection tracking spot
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CONN* bta_gattc_conn_alloc(const RawAddress& remote_bda) {
  uint8_t i_conn = 0;
  tBTA_GATTC_CONN* p_conn = &bta_gattc_cb.conn_track[0];

  for (i_conn = 0; i_conn < GATT_MAX_PHY_CHANNEL; i_conn++, p_conn++) {
    if (!p_conn->in_use) {
#if (BTA_GATT_DEBUG == TRUE)
      log::verbose("found conn_track:{} available", i_conn);
#endif
      p_conn->in_use = true;
      p_conn->remote_bda = remote_bda;
      return p_conn;
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_gattc_conn_find
 *
 * Description      allocate connection tracking spot
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CONN* bta_gattc_conn_find(const RawAddress& remote_bda) {
  uint8_t i_conn = 0;
  tBTA_GATTC_CONN* p_conn = &bta_gattc_cb.conn_track[0];

  for (i_conn = 0; i_conn < GATT_MAX_PHY_CHANNEL; i_conn++, p_conn++) {
    if (p_conn->in_use && remote_bda == p_conn->remote_bda) {
#if (BTA_GATT_DEBUG == TRUE)
      log::verbose("found conn_track:{} matched", i_conn);
#endif
      return p_conn;
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_gattc_conn_find_alloc
 *
 * Description      find or allocate connection tracking spot
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CONN* bta_gattc_conn_find_alloc(const RawAddress& remote_bda) {
  tBTA_GATTC_CONN* p_conn = bta_gattc_conn_find(remote_bda);

  if (p_conn == NULL) {
    p_conn = bta_gattc_conn_alloc(remote_bda);
  }
  return p_conn;
}

/*******************************************************************************
 *
 * Function         bta_gattc_conn_dealloc
 *
 * Description      de-allocate connection tracking spot
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
bool bta_gattc_conn_dealloc(const RawAddress& remote_bda) {
  tBTA_GATTC_CONN* p_conn = bta_gattc_conn_find(remote_bda);

  if (p_conn != NULL) {
    p_conn->in_use = false;
    p_conn->remote_bda = RawAddress::kEmpty;
    return true;
  }
  return false;
}

/*******************************************************************************
 *
 * Function         bta_gattc_find_int_conn_clcb
 *
 * Description      try to locate a clcb when an internal connecion event
 *                  arrives.
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CLCB* bta_gattc_find_int_conn_clcb(tBTA_GATTC_DATA* p_msg) {
  tBTA_GATTC_CLCB* p_clcb = NULL;

  if (p_msg->int_conn.role == HCI_ROLE_PERIPHERAL) {
    bta_gattc_conn_find_alloc(p_msg->int_conn.remote_bda);
  }

  /* try to locate a logic channel */
  p_clcb = bta_gattc_find_clcb_by_cif(p_msg->int_conn.client_if, p_msg->int_conn.remote_bda,
                                      p_msg->int_conn.transport);
  if (p_clcb == NULL) {
    /* for a background connection or listening connection */
    if (/*p_msg->int_conn.role == HCI_ROLE_PERIPHERAL ||  */
        bta_gattc_check_bg_conn(p_msg->int_conn.client_if, p_msg->int_conn.remote_bda,
                                p_msg->int_conn.role)) {
      /* allocate a new channel */
      p_clcb = bta_gattc_clcb_alloc(p_msg->int_conn.client_if, p_msg->int_conn.remote_bda,
                                    p_msg->int_conn.transport);
    }
  }
  return p_clcb;
}

/*******************************************************************************
 *
 * Function         bta_gattc_find_int_disconn_clcb
 *
 * Description      try to locate a clcb when an internal disconnect callback
 *                  arrives.
 *
 * Returns          pointer to the clcb
 *
 ******************************************************************************/
tBTA_GATTC_CLCB* bta_gattc_find_int_disconn_clcb(tBTA_GATTC_DATA* p_msg) {
  tBTA_GATTC_CLCB* p_clcb = NULL;

  bta_gattc_conn_dealloc(p_msg->int_conn.remote_bda);
  p_clcb =
          bta_gattc_find_clcb_by_conn_id(static_cast<tCONN_ID>(p_msg->int_conn.hdr.layer_specific));
  if (p_clcb == NULL) {
    /* connection attempt failed, send connection callback event */
    p_clcb = bta_gattc_find_clcb_by_cif(p_msg->int_conn.client_if, p_msg->int_conn.remote_bda,
                                        p_msg->int_conn.transport);
  }
  if (p_clcb == NULL) {
    log::verbose("disconnection ID:{} not used by BTA", p_msg->int_conn.hdr.layer_specific);
  }
  return p_clcb;
}

void bta_gatt_client_dump(int fd) {
  std::stringstream stream;
  int entry_count = 0;

  stream << " ->conn_track (GATT_MAX_PHY_CHANNEL=" << GATT_MAX_PHY_CHANNEL << ")\n";
  for (int i = 0; i < GATT_MAX_PHY_CHANNEL; i++) {
    tBTA_GATTC_CONN* p_conn_track = &bta_gattc_cb.conn_track[i];
    if (p_conn_track->in_use) {
      entry_count++;
      stream << "  address: " << ADDRESS_TO_LOGGABLE_STR(p_conn_track->remote_bda);
      stream << "\n";
    }
  }
  stream << "  -- used: " << entry_count << "\n";
  entry_count = 0;

  stream << " ->bg_track (BTA_GATTC_KNOWN_SR_MAX=" << BTA_GATTC_KNOWN_SR_MAX << ")\n";
  for (int i = 0; i < BTA_GATTC_KNOWN_SR_MAX; i++) {
    tBTA_GATTC_BG_TCK* p_bg_track = &bta_gattc_cb.bg_track[i];
    if (!p_bg_track->in_use) {
      continue;
    }
    entry_count++;
    stream << "  address: " << ADDRESS_TO_LOGGABLE_STR(p_bg_track->remote_bda)
           << "  cif_mask: " << loghex(p_bg_track->cif_mask);
    stream << "\n";
  }

  stream << "  -- used: " << entry_count << "\n";
  entry_count = 0;
  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    stream << " ->cl_rcb (dynamic)\n";
    for (auto& [i, p_cl_rcb] : bta_gattc_cb.cl_rcb_map) {
      entry_count++;
      stream << "  client_if: " << +p_cl_rcb->client_if << "  app uuids: " << p_cl_rcb->app_uuid
             << "  clcb_num: " << +p_cl_rcb->num_clcb;
      stream << "\n";
    }
  } else {
    stream << " ->cl_rcb (BTA_GATTC_CL_MAX=" << BTA_GATTC_CL_MAX << ")\n";
    for (int i = 0; i < BTA_GATTC_CL_MAX; i++) {
      tBTA_GATTC_RCB* p_cl_rcb = &bta_gattc_cb.cl_rcb[i];
      if (!p_cl_rcb->in_use) {
        continue;
      }
      entry_count++;
      stream << "  client_if: " << +p_cl_rcb->client_if << "  app uuids: " << p_cl_rcb->app_uuid
             << "  clcb_num: " << +p_cl_rcb->num_clcb;
      stream << "\n";
    }
  }

  stream << "  -- used: " << entry_count << "\n";
  entry_count = 0;

  if (com::android::bluetooth::flags::gatt_client_dynamic_allocation()) {
    stream << " ->clcb (dynamic)\n";
    for (auto& p_clcb : bta_gattc_cb.clcb_set) {
      if (!p_clcb->in_use) {
        continue;
      }
      entry_count++;
      stream << "  conn_id: " << loghex(p_clcb->bta_conn_id)
             << "  address: " << ADDRESS_TO_LOGGABLE_STR(p_clcb->bda)
             << "  transport: " << bt_transport_text(p_clcb->transport)
             << "  state: " << bta_clcb_state_text(p_clcb->state);
      stream << "\n";
    }
  } else {
    stream << " ->clcb (BTA_GATTC_CLCB_MAX=" << BTA_GATTC_CLCB_MAX << ")\n";
    for (size_t i = 0; i < BTA_GATTC_CLCB_MAX; i++) {
      tBTA_GATTC_CLCB* p_clcb = &bta_gattc_cb.clcb[i];
      if (!p_clcb->in_use) {
        continue;
      }
      entry_count++;
      stream << "  conn_id: " << loghex(p_clcb->bta_conn_id)
             << "  address: " << ADDRESS_TO_LOGGABLE_STR(p_clcb->bda)
             << "  transport: " << bt_transport_text(p_clcb->transport)
             << "  state: " << bta_clcb_state_text(p_clcb->state);
      stream << "\n";
    }
  }

  stream << "  -- used: " << entry_count << "\n";
  entry_count = 0;
  stream << " ->known_server (BTA_GATTC_KNOWN_SR_MAX=" << BTA_GATTC_KNOWN_SR_MAX << ")\n";
  for (int i = 0; i < BTA_GATTC_CL_MAX; i++) {
    tBTA_GATTC_SERV* p_known_server = &bta_gattc_cb.known_server[i];
    if (!p_known_server->in_use) {
      continue;
    }
    entry_count++;
    stream << "  server_address: " << ADDRESS_TO_LOGGABLE_STR(p_known_server->server_bda)
           << "  mtu: " << p_known_server->mtu
           << "  blocked_conn_id: " << loghex(p_known_server->blocked_conn_id)
           << "  num_clcb: " << +p_known_server->num_clcb
           << "  state: " << bta_server_state_text(p_known_server->state)
           << "  connected: " << p_known_server->connected
           << "  srvc_disc_count: " << p_known_server->srvc_disc_count
           << "  disc_blocked_waiting_on_version: "
           << p_known_server->disc_blocked_waiting_on_version
           << "  srvc_hdl_chg: " << +p_known_server->srvc_hdl_chg
           << "  srvc_hdl_db_hash: " << p_known_server->srvc_hdl_db_hash
           << "  update_count: " << +p_known_server->update_count;

    stream << "\n";
  }

  stream << "  -- used: " << entry_count << "\n";
  entry_count = 0;
  dprintf(fd, "BTA_GATTC_CB state %s \n%s\n", bta_gattc_state_text(bta_gattc_cb.state).c_str(),
          stream.str().c_str());
}
