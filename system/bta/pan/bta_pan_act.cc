/******************************************************************************
 *
 *  Copyright 2004-2012 Broadcom Corporation
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
 *  This file contains the pan action functions for the state machine.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth"

#include <bluetooth/log.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "bta/include/bta_pan_co.h"
#include "bta/pan/bta_pan_int.h"
#include "bta_api.h"
#include "bta_pan_api.h"
#include "bta_sys.h"
#include "internal_include/bt_target.h"  // PAN_INCLUDED
#include "osi/include/allocator.h"
#include "osi/include/fixed_queue.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/pan_api.h"
#include "types/raw_address.h"

using namespace bluetooth;

#if (PAN_INCLUDED == TRUE)

/* RX and TX data flow mask */
#define BTA_PAN_RX_MASK 0x0F
#define BTA_PAN_TX_MASK 0xF0

/*******************************************************************************
 *
 * Function    bta_pan_pm_conn_busy
 *
 * Description set pan pm connection busy state
 *
 * Params      p_scb: state machine control block of pan connection
 *
 * Returns     void
 *
 ******************************************************************************/
static void bta_pan_pm_conn_busy(tBTA_PAN_SCB* p_scb) {
  if ((p_scb != NULL) && (p_scb->state != BTA_PAN_IDLE_ST)) {
    bta_sys_busy(BTA_ID_PAN, p_scb->app_id, p_scb->bd_addr);
  }
}

/*******************************************************************************
 *
 * Function    bta_pan_pm_conn_idle
 *
 * Description set pan pm connection idle state
 *
 * Params      p_scb: state machine control block of pan connection
 *
 * Returns     void
 *
 ******************************************************************************/
static void bta_pan_pm_conn_idle(tBTA_PAN_SCB* p_scb) {
  if ((p_scb != NULL) && (p_scb->state != BTA_PAN_IDLE_ST)) {
    bta_sys_idle(BTA_ID_PAN, p_scb->app_id, p_scb->bd_addr);
  }
}

/*******************************************************************************
 *
 * Function         bta_pan_conn_state_cback
 *
 * Description      Connection state callback from Pan profile
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_pan_conn_state_cback(uint16_t handle, const RawAddress& bd_addr, tPAN_RESULT state,
                                     bool is_role_change, uint8_t src_role, uint8_t dst_role) {
  tBTA_PAN_SCB* p_scb;
  tBTA_PAN_CONN* p_buf = (tBTA_PAN_CONN*)osi_malloc(sizeof(tBTA_PAN_CONN));

  if ((state == PAN_SUCCESS) && !is_role_change) {
    p_buf->hdr.event = BTA_PAN_CONN_OPEN_EVT;
    p_scb = bta_pan_scb_by_handle(handle);
    if (p_scb == NULL) {
      /* allocate an scb */
      p_scb = bta_pan_scb_alloc();
    }
    /* we have exceeded maximum number of connections */
    if (!p_scb) {
      PAN_Disconnect(handle);
      return;
    }

    p_scb->handle = handle;
    p_scb->local_role = src_role;
    p_scb->peer_role = dst_role;
    p_scb->pan_flow_enable = true;
    p_scb->bd_addr = bd_addr;
    p_scb->data_queue = fixed_queue_new(SIZE_MAX);

    if (src_role == PAN_ROLE_CLIENT) {
      p_scb->app_id = bta_pan_cb.app_id[0];
    } else if (src_role == PAN_ROLE_NAP_SERVER) {
      p_scb->app_id = bta_pan_cb.app_id[2];
    }
  } else if ((state != PAN_SUCCESS) && !is_role_change) {
    p_buf->hdr.event = BTA_PAN_CONN_CLOSE_EVT;
  } else {
    return;
  }

  p_buf->result = state;
  p_buf->hdr.layer_specific = handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         bta_pan_data_flow_cb
 *
 * Description      Data flow status callback from PAN
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_pan_data_flow_cb(uint16_t handle, tPAN_RESULT result) {
  tBTA_PAN_SCB* p_scb;

  p_scb = bta_pan_scb_by_handle(handle);
  if (p_scb == NULL) {
    return;
  }

  if (result == PAN_TX_FLOW_ON) {
    BT_HDR_RIGID* p_buf = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));
    p_buf->layer_specific = handle;
    p_buf->event = BTA_PAN_BNEP_FLOW_ENABLE_EVT;
    bta_sys_sendmsg(p_buf);
    bta_pan_co_rx_flow(handle, p_scb->app_id, true);
  } else if (result == PAN_TX_FLOW_OFF) {
    p_scb->pan_flow_enable = false;
    bta_pan_co_rx_flow(handle, p_scb->app_id, false);
  }
}

/*******************************************************************************
 *
 * Function         bta_pan_data_buf_ind_cback
 *
 * Description      data indication callback from pan profile
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_pan_data_buf_ind_cback(uint16_t handle, const RawAddress& src,
                                       const RawAddress& dst, uint16_t protocol, BT_HDR* p_buf,
                                       bool ext, bool forward) {
  tBTA_PAN_SCB* p_scb = bta_pan_scb_by_handle(handle);
  if (p_scb == NULL) {
    return;
  }

  if (sizeof(BT_HDR) + sizeof(tBTA_PAN_DATA_PARAMS) + p_buf->len > PAN_BUF_SIZE) {
    log::error("received buffer length too large: {}", p_buf->len);
    return;
  }

  BT_HDR* p_new_buf = (BT_HDR*)osi_malloc(PAN_BUF_SIZE);
  memcpy((uint8_t*)(p_new_buf + 1) + sizeof(tBTA_PAN_DATA_PARAMS),
         (uint8_t*)(p_buf + 1) + p_buf->offset, p_buf->len);
  p_new_buf->len = p_buf->len;
  p_new_buf->offset = sizeof(tBTA_PAN_DATA_PARAMS);

  /* copy params into the space before the data */
  ((tBTA_PAN_DATA_PARAMS*)p_new_buf)->src = src;
  ((tBTA_PAN_DATA_PARAMS*)p_new_buf)->dst = dst;
  ((tBTA_PAN_DATA_PARAMS*)p_new_buf)->protocol = protocol;
  ((tBTA_PAN_DATA_PARAMS*)p_new_buf)->ext = ext;
  ((tBTA_PAN_DATA_PARAMS*)p_new_buf)->forward = forward;

  fixed_queue_enqueue(p_scb->data_queue, p_new_buf);
  BT_HDR_RIGID* p_event = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));
  p_event->layer_specific = handle;
  p_event->event = BTA_PAN_RX_FROM_BNEP_READY_EVT;
  bta_sys_sendmsg(p_event);
}

/*******************************************************************************
 *
 * Function         bta_pan_pfilt_ind_cback
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_pan_pfilt_ind_cback(uint16_t handle, bool indication, tPAN_RESULT result,
                                    uint16_t num_filters, uint8_t* p_filters) {
  bta_pan_co_pfilt_ind(handle, indication, (result == PAN_SUCCESS) ? BTA_PAN_SUCCESS : BTA_PAN_FAIL,
                       num_filters, p_filters);
}

/*******************************************************************************
 *
 * Function         bta_pan_mfilt_ind_cback
 *
 * Description
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_pan_mfilt_ind_cback(uint16_t handle, bool indication, tPAN_RESULT result,
                                    uint16_t num_mfilters, uint8_t* p_mfilters) {
  bta_pan_co_mfilt_ind(handle, indication, (result == PAN_SUCCESS) ? BTA_PAN_SUCCESS : BTA_PAN_FAIL,
                       num_mfilters, p_mfilters);
}

/*******************************************************************************
 *
 * Function         bta_pan_has_multiple_connections
 *
 * Description      Check whether there are multiple GN/NAP connections to
 *                  different devices
 *
 *
 * Returns          bool
 *
 ******************************************************************************/
static bool bta_pan_has_multiple_connections(uint8_t app_id) {
  tBTA_PAN_SCB* p_scb = NULL;
  bool found = false;
  RawAddress bd_addr;

  for (uint8_t index = 0; index < BTA_PAN_NUM_CONN; index++) {
    p_scb = &bta_pan_cb.scb[index];
    if (p_scb->in_use && app_id == p_scb->app_id) {
      /* save temp bd_addr */
      bd_addr = p_scb->bd_addr;
      found = true;
      break;
    }
  }

  /* If cannot find a match then there is no connection at all */
  if (!found) {
    return false;
  }

  /* Find whether there is another connection with different device other than
     PANU.
      Could be same service or different service */
  for (uint8_t index = 0; index < BTA_PAN_NUM_CONN; index++) {
    p_scb = &bta_pan_cb.scb[index];
    if (p_scb->in_use && p_scb->app_id != bta_pan_cb.app_id[0] && bd_addr != p_scb->bd_addr) {
      return true;
    }
  }
  return false;
}

/*******************************************************************************
 *
 * Function         bta_pan_enable
 *
 * Description
 *
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_enable(tBTA_PAN_DATA* p_data) {
  tPAN_REGISTER reg_data;

  bta_pan_cb.p_cback = p_data->api_enable.p_cback;

  reg_data.pan_conn_state_cb = bta_pan_conn_state_cback;
  reg_data.pan_bridge_req_cb = NULL;
  reg_data.pan_data_buf_ind_cb = bta_pan_data_buf_ind_cback;
  reg_data.pan_data_ind_cb = NULL;
  reg_data.pan_pfilt_ind_cb = bta_pan_pfilt_ind_cback;
  reg_data.pan_mfilt_ind_cb = bta_pan_mfilt_ind_cback;
  reg_data.pan_tx_data_flow_cb = bta_pan_data_flow_cb;

  PAN_Register(&reg_data);

  bta_pan_cb.flow_mask = bta_pan_co_init(&bta_pan_cb.q_level);
  bta_pan_cb.p_cback(BTA_PAN_ENABLE_EVT, NULL);
}

/*******************************************************************************
 *
 * Function         bta_pan_set_role
 *
 * Description
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_set_role(tBTA_PAN_DATA* p_data) {
  bta_pan_cb.app_id[0] = p_data->api_set_role.user_app_id;
  bta_pan_cb.app_id[2] = p_data->api_set_role.nap_app_id;

  /* set security correctly in api and here */
  tPAN_RESULT status =
          PAN_SetRole(p_data->api_set_role.role, std::string(p_data->api_set_role.user_name),
                      std::string(p_data->api_set_role.nap_name));

  tBTA_PAN bta_pan = {
          .set_role =
                  {
                          .status = (status == PAN_SUCCESS) ? BTA_PAN_SUCCESS : BTA_PAN_FAIL,
                          .role = p_data->api_set_role.role,
                  },
  };

  if (status == PAN_SUCCESS) {
    if (p_data->api_set_role.role & PAN_ROLE_NAP_SERVER) {
      bta_sys_add_uuid(UUID_SERVCLASS_NAP);
    } else {
      bta_sys_remove_uuid(UUID_SERVCLASS_NAP);
    }

    if (p_data->api_set_role.role & PAN_ROLE_CLIENT) {
      bta_sys_add_uuid(UUID_SERVCLASS_PANU);
    } else {
      bta_sys_remove_uuid(UUID_SERVCLASS_PANU);
    }
  } else {
    /* if status is not success clear everything */
    PAN_SetRole(0, std::string(), std::string());
    bta_sys_remove_uuid(UUID_SERVCLASS_NAP);
    bta_sys_remove_uuid(UUID_SERVCLASS_GN);
    bta_sys_remove_uuid(UUID_SERVCLASS_PANU);
  }
  bta_pan_cb.p_cback(BTA_PAN_SET_ROLE_EVT, &bta_pan);
}

/*******************************************************************************
 *
 * Function         bta_pan_disable
 *
 * Description
 *
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_disable(void) {
  BT_HDR* p_buf;
  tBTA_PAN_SCB* p_scb = &bta_pan_cb.scb[0];
  uint8_t i;

  /* close all connections */
  PAN_SetRole(0, std::string(), std::string());

  bta_sys_remove_uuid(UUID_SERVCLASS_NAP);
  bta_sys_remove_uuid(UUID_SERVCLASS_GN);
  bta_sys_remove_uuid(UUID_SERVCLASS_PANU);
  /* free all queued up data buffers */
  for (i = 0; i < BTA_PAN_NUM_CONN; i++, p_scb++) {
    if (p_scb->in_use) {
      while ((p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_scb->data_queue)) != NULL) {
        osi_free(p_buf);
      }

      bta_pan_co_close(p_scb->handle, p_scb->app_id);
    }
  }

  PAN_Deregister();
}

/*******************************************************************************
 *
 * Function         bta_pan_open
 *
 * Description
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_open(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data) {
  tPAN_RESULT status;
  tBTA_PAN bta_pan;

  status = PAN_Connect(p_data->api_open.bd_addr, p_data->api_open.local_role,
                       p_data->api_open.peer_role, &p_scb->handle);
  log::verbose("pan connect status: {}", status);

  if (status == PAN_SUCCESS) {
    p_scb->bd_addr = p_data->api_open.bd_addr;
    p_scb->local_role = p_data->api_open.local_role;
    p_scb->peer_role = p_data->api_open.peer_role;
    bta_pan.opening.bd_addr = p_data->api_open.bd_addr;
    bta_pan.opening.handle = p_scb->handle;
    bta_pan_cb.p_cback(BTA_PAN_OPENING_EVT, &bta_pan);

  } else {
    bta_pan_scb_dealloc(p_scb);
    bta_pan.open.bd_addr = p_data->api_open.bd_addr;
    bta_pan.open.status = BTA_PAN_FAIL;
    bta_pan.open.local_role = p_data->api_open.local_role;
    bta_pan.open.peer_role = p_data->api_open.peer_role;
    bta_pan_cb.p_cback(BTA_PAN_OPEN_EVT, &bta_pan);
  }
}

/*******************************************************************************
 *
 * Function         bta_pan_close
 *
 * Description
 *
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_api_close(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* /* p_data */) {
  tBTA_PAN_CONN* p_buf = (tBTA_PAN_CONN*)osi_malloc(sizeof(tBTA_PAN_CONN));

  PAN_Disconnect(p_scb->handle);

  /*
   * Send an event to BTA so that application will get the connection
   * close event.
   */
  p_buf->hdr.event = BTA_PAN_CONN_CLOSE_EVT;
  p_buf->hdr.layer_specific = p_scb->handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         bta_pan_conn_open
 *
 * Description      process connection open event
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_conn_open(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data) {
  tBTA_PAN bta_pan;

  log::verbose("pan connection result: {}", p_data->conn.result);

  bta_pan.open.bd_addr = p_scb->bd_addr;
  bta_pan.open.handle = p_scb->handle;
  bta_pan.open.local_role = p_scb->local_role;
  bta_pan.open.peer_role = p_scb->peer_role;

  if (p_data->conn.result == PAN_SUCCESS) {
    bta_pan.open.status = BTA_PAN_SUCCESS;
    p_scb->pan_flow_enable = true;
    p_scb->app_flow_enable = true;
    bta_sys_conn_open(BTA_ID_PAN, p_scb->app_id, p_scb->bd_addr);
  } else {
    bta_pan_scb_dealloc(p_scb);
    bta_pan.open.status = BTA_PAN_FAIL;
  }

  p_scb->pan_flow_enable = true;
  p_scb->app_flow_enable = true;

  /* If app_id is NAP/GN, check whether there are multiple connections.
     If there are, provide a special app_id to dm to enforce central role only.
     */
  if (p_scb->app_id == bta_pan_cb.app_id[2] && bta_pan_has_multiple_connections(p_scb->app_id)) {
    p_scb->app_id = BTA_APP_ID_PAN_MULTI;
  }

  bta_sys_conn_open(BTA_ID_PAN, p_scb->app_id, p_scb->bd_addr);
  bta_pan_cb.p_cback(BTA_PAN_OPEN_EVT, &bta_pan);
}

/*******************************************************************************
 *
 * Function         bta_pan_conn_close
 *
 * Description      process connection close event
 *
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_conn_close(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data) {
  tBTA_PAN bta_pan;
  BT_HDR* p_buf;

  bta_pan.close.handle = p_data->hdr.layer_specific;

  bta_sys_conn_close(BTA_ID_PAN, p_scb->app_id, p_scb->bd_addr);

  /* free all queued up data buffers */
  while ((p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_scb->data_queue)) != NULL) {
    osi_free(p_buf);
  }

  bta_pan_scb_dealloc(p_scb);

  bta_pan_cb.p_cback(BTA_PAN_CLOSE_EVT, &bta_pan);
}

/*******************************************************************************
 *
 * Function         bta_pan_rx_path
 *
 * Description      Handle data on the RX path (data sent from the phone to
 *                  BTA).
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_rx_path(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* /* p_data */) {
  /* if data path configured for rx pull */
  if ((bta_pan_cb.flow_mask & BTA_PAN_RX_MASK) == BTA_PAN_RX_PULL) {
    /* if we can accept data */
    if (p_scb->pan_flow_enable) {
      /* call application callout function for rx path */
      bta_pan_co_rx_path(p_scb->handle, p_scb->app_id);
    }
  } else {
    /* else data path configured for rx push */
  }
}

/*******************************************************************************
 *
 * Function         bta_pan_tx_path
 *
 * Description      Handle the TX data path (data sent from BTA to the phone).
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_tx_path(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* /* p_data */) {
  bta_pan_pm_conn_busy(p_scb);
  /* call application callout function for tx path */
  bta_pan_co_tx_path(p_scb->handle, p_scb->app_id);

  /* free data that exceeds queue level */
  while (fixed_queue_length(p_scb->data_queue) > bta_pan_cb.q_level) {
    BT_HDR* p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_scb->data_queue);
    if (p_buf != nullptr) {
      osi_free(p_buf);
    }
  }

  bta_pan_pm_conn_idle(p_scb);
}

/*******************************************************************************
 *
 * Function         bta_pan_tx_flow
 *
 * Description      Set the application flow control state.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_tx_flow(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data) {
  p_scb->app_flow_enable = p_data->ci_tx_flow.enable;
}

/*******************************************************************************
 *
 * Function         bta_pan_write_buf
 *
 * Description      Handle a bta_pan_ci_rx_writebuf() and send data to PAN.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_write_buf(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data) {
  if ((bta_pan_cb.flow_mask & BTA_PAN_RX_MASK) == BTA_PAN_RX_PUSH_BUF) {
    bta_pan_pm_conn_busy(p_scb);

    PAN_WriteBuf(p_scb->handle, ((tBTA_PAN_DATA_PARAMS*)p_data)->dst,
                 ((tBTA_PAN_DATA_PARAMS*)p_data)->src, ((tBTA_PAN_DATA_PARAMS*)p_data)->protocol,
                 (BT_HDR*)p_data, ((tBTA_PAN_DATA_PARAMS*)p_data)->ext);
    bta_pan_pm_conn_idle(p_scb);
  }
}

/*******************************************************************************
 *
 * Function         bta_pan_free_buf
 *
 * Description      Frees the data buffer during closing state
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_pan_free_buf(tBTA_PAN_SCB* /* p_scb */, tBTA_PAN_DATA* p_data) { osi_free(p_data); }

#endif /* PAN_INCLUDED */
