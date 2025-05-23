/******************************************************************************
 *
 *  Copyright 2001-2012 Broadcom Corporation
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
 *  this file contains the main BNEP functions
 *
 ******************************************************************************/

#include <bluetooth/log.h>
#include <string.h>

#include <cstdint>

#include "bnep_api.h"
#include "bnep_int.h"
#include "bta/include/bta_sec_api.h"
#include "hci/controller_interface.h"
#include "internal_include/bt_target.h"
#include "l2cap_types.h"
#include "l2cdefs.h"
#include "main/shim/entry.h"
#include "main/shim/helpers.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/fixed_queue.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_psm_types.h"
#include "stack/include/bt_types.h"
#include "stack/include/l2cap_interface.h"
#include "types/bt_transport.h"
#include "types/raw_address.h"

using namespace bluetooth;

/******************************************************************************/
/*                     G L O B A L    B N E P       D A T A                   */
/******************************************************************************/
tBNEP_CB bnep_cb;

const uint16_t bnep_frame_hdr_sizes[] = {14, 1, 2, 8, 8};

/******************************************************************************/
/*            L O C A L    F U N C T I O N     P R O T O T Y P E S            */
/******************************************************************************/
static void bnep_connect_ind(const RawAddress& bd_addr, uint16_t l2cap_cid, uint16_t psm,
                             uint8_t l2cap_id);
static void bnep_connect_cfm(uint16_t l2cap_cid, tL2CAP_CONN result);
static void bnep_config_cfm(uint16_t l2cap_cid, uint16_t result, tL2CAP_CFG_INFO* p_cfg);
static void bnep_disconnect_ind(uint16_t l2cap_cid, bool ack_needed);
static void bnep_data_ind(uint16_t l2cap_cid, BT_HDR* p_msg);
static void bnep_congestion_ind(uint16_t lcid, bool is_congested);
static void bnep_on_l2cap_error(uint16_t l2cap_cid, uint16_t result);
/*******************************************************************************
 *
 * Function         bnep_register_with_l2cap
 *
 * Description      This function registers BNEP PSM with L2CAP
 *
 * Returns          void
 *
 ******************************************************************************/
tBNEP_RESULT bnep_register_with_l2cap(void) {
  /* Initialize the L2CAP configuration. We only care about MTU and flush */
  memset(&bnep_cb.l2cap_my_cfg, 0, sizeof(tL2CAP_CFG_INFO));

  bnep_cb.l2cap_my_cfg.mtu_present = true;
  bnep_cb.l2cap_my_cfg.mtu = BNEP_MTU_SIZE;

  bnep_cb.reg_info.pL2CA_ConnectInd_Cb = bnep_connect_ind;
  bnep_cb.reg_info.pL2CA_ConnectCfm_Cb = bnep_connect_cfm;
  bnep_cb.reg_info.pL2CA_ConfigInd_Cb = nullptr;
  bnep_cb.reg_info.pL2CA_ConfigCfm_Cb = bnep_config_cfm;
  bnep_cb.reg_info.pL2CA_DisconnectInd_Cb = bnep_disconnect_ind;
  bnep_cb.reg_info.pL2CA_DataInd_Cb = bnep_data_ind;
  bnep_cb.reg_info.pL2CA_CongestionStatus_Cb = bnep_congestion_ind;
  bnep_cb.reg_info.pL2CA_Error_Cb = bnep_on_l2cap_error;

  /* Now, register with L2CAP */
  if (!stack::l2cap::get_interface().L2CA_RegisterWithSecurity(
              BT_PSM_BNEP, bnep_cb.reg_info, false /* enable_snoop */, nullptr, BNEP_MTU_SIZE,
              BNEP_MTU_SIZE, BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT)) {
    log::error("BNEP - Registration failed");
    return BNEP_SECURITY_FAIL;
  }

  return BNEP_SUCCESS;
}

/*******************************************************************************
 *
 * Function         bnep_connect_ind
 *
 * Description      This function handles an inbound connection indication
 *                  from L2CAP. This is the case where we are acting as a
 *                  server.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bnep_connect_ind(const RawAddress& bd_addr, uint16_t l2cap_cid, uint16_t /* psm */,
                             uint8_t /* l2cap_id */) {
  tBNEP_CONN* p_bcb = bnepu_find_bcb_by_bd_addr(bd_addr);

  /* If we are not acting as server, or already have a connection, or have */
  /* no more resources to handle the connection, reject the connection.    */
  if (!(bnep_cb.profile_registered) || (p_bcb) || ((p_bcb = bnepu_allocate_bcb(bd_addr)) == NULL)) {
    if (!stack::l2cap::get_interface().L2CA_DisconnectReq(l2cap_cid)) {
      log::warn("Unable to request L2CAP disconnect peer:{} cid:{}", bd_addr, l2cap_cid);
    }
    return;
  }

  /* Transition to the next appropriate state, waiting for config setup. */
  p_bcb->con_state = BNEP_STATE_CFG_SETUP;

  /* Save the L2CAP Channel ID. */
  p_bcb->l2cap_cid = l2cap_cid;

  /* Start timer waiting for config setup */
  alarm_set_on_mloop(p_bcb->conn_timer, BNEP_CONN_TIMEOUT_MS, bnep_conn_timer_timeout, p_bcb);

  log::debug("BNEP - Rcvd L2CAP conn ind, CID: 0x{:x}", p_bcb->l2cap_cid);
}

static void bnep_on_l2cap_error(uint16_t l2cap_cid, uint16_t /* result */) {
  tBNEP_CONN* p_bcb = bnepu_find_bcb_by_cid(l2cap_cid);
  if (p_bcb == nullptr) {
    return;
  }

  /* Tell the upper layer, if there is a callback */
  if ((p_bcb->con_flags & BNEP_FLAGS_IS_ORIG) && (bnep_cb.p_conn_state_cb)) {
    (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_CONN_FAILED, false);
  }

  if (!stack::l2cap::get_interface().L2CA_DisconnectReq(p_bcb->l2cap_cid)) {
    log::warn("Unable to request L2CAP disconnect peer:{} cid:{}", p_bcb->rem_bda, l2cap_cid);
  }

  bnepu_release_bcb(p_bcb);
}

/*******************************************************************************
 *
 * Function         bnep_connect_cfm
 *
 * Description      This function handles the connect confirm events
 *                  from L2CAP. This is the case when we are acting as a
 *                  client and have sent a connect request.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bnep_connect_cfm(uint16_t l2cap_cid, tL2CAP_CONN result) {
  tBNEP_CONN* p_bcb;

  /* Find CCB based on CID */
  p_bcb = bnepu_find_bcb_by_cid(l2cap_cid);
  if (p_bcb == NULL) {
    log::warn("BNEP - Rcvd conn cnf for unknown CID 0x{:x}", l2cap_cid);
    return;
  }

  /* If the connection response contains success status, then */
  /* Transition to the next state and startup the timer.      */
  if ((result == tL2CAP_CONN::L2CAP_CONN_OK) && (p_bcb->con_state == BNEP_STATE_CONN_START)) {
    p_bcb->con_state = BNEP_STATE_CFG_SETUP;

    /* Start timer waiting for config results */
    alarm_set_on_mloop(p_bcb->conn_timer, BNEP_CONN_TIMEOUT_MS, bnep_conn_timer_timeout, p_bcb);

    log::debug("BNEP - got conn cnf, sent cfg req, CID: 0x{:x}", p_bcb->l2cap_cid);
  } else {
    log::error("invoked with non OK status");
  }
}

/*******************************************************************************
 *
 * Function         bnep_config_cfm
 *
 * Description      This function processes the L2CAP configuration confirmation
 *                  event.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bnep_config_cfm(uint16_t l2cap_cid, uint16_t /* initiator */,
                            tL2CAP_CFG_INFO* /* p_cfg */) {
  tBNEP_CONN* p_bcb;

  log::debug("BNEP - Rcvd cfg cfm, CID: 0x{:x}", l2cap_cid);

  /* Find CCB based on CID */
  p_bcb = bnepu_find_bcb_by_cid(l2cap_cid);
  if (p_bcb == NULL) {
    log::warn("BNEP - Rcvd L2CAP cfg ind, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  /* For now, always accept configuration from the other side */
  p_bcb->con_state = BNEP_STATE_SEC_CHECKING;

  /* Start timer waiting for setup or response */
  alarm_set_on_mloop(p_bcb->conn_timer, BNEP_HOST_TIMEOUT_MS, bnep_conn_timer_timeout, p_bcb);

  if (p_bcb->con_flags & BNEP_FLAGS_IS_ORIG) {
    bnep_sec_check_complete(&p_bcb->rem_bda, BT_TRANSPORT_BR_EDR, p_bcb);
  }
}

/*******************************************************************************
 *
 * Function         bnep_disconnect_ind
 *
 * Description      This function handles a disconnect event from L2CAP. If
 *                  requested to, we ack the disconnect before dropping the CCB
 *
 * Returns          void
 *
 ******************************************************************************/
static void bnep_disconnect_ind(uint16_t l2cap_cid, bool /* ack_needed */) {
  tBNEP_CONN* p_bcb;

  /* Find CCB based on CID */
  p_bcb = bnepu_find_bcb_by_cid(l2cap_cid);
  if (p_bcb == NULL) {
    log::warn("BNEP - Rcvd L2CAP disc, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  log::debug("BNEP - Rcvd L2CAP disc, CID: 0x{:x}", l2cap_cid);

  /* Tell the user if there is a callback */
  if (p_bcb->con_state == BNEP_STATE_CONNECTED) {
    if (bnep_cb.p_conn_state_cb) {
      (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_CONN_DISCONNECTED, false);
    }
  } else {
    if ((bnep_cb.p_conn_state_cb) && ((p_bcb->con_flags & BNEP_FLAGS_IS_ORIG) ||
                                      (p_bcb->con_flags & BNEP_FLAGS_CONN_COMPLETED))) {
      (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_CONN_FAILED, false);
    }
  }

  bnepu_release_bcb(p_bcb);
}

/*******************************************************************************
 *
 * Function         bnep_congestion_ind
 *
 * Description      This is a callback function called by L2CAP when
 *                  congestion status changes
 *
 ******************************************************************************/
static void bnep_congestion_ind(uint16_t l2cap_cid, bool is_congested) {
  tBNEP_CONN* p_bcb;

  /* Find BCB based on CID */
  p_bcb = bnepu_find_bcb_by_cid(l2cap_cid);
  if (p_bcb == NULL) {
    log::warn("BNEP - Rcvd L2CAP cong, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  if (is_congested) {
    p_bcb->con_flags |= BNEP_FLAGS_L2CAP_CONGESTED;
    if (bnep_cb.p_tx_data_flow_cb) {
      bnep_cb.p_tx_data_flow_cb(p_bcb->handle, BNEP_TX_FLOW_OFF);
    }
  } else {
    p_bcb->con_flags &= ~BNEP_FLAGS_L2CAP_CONGESTED;

    if (bnep_cb.p_tx_data_flow_cb) {
      bnep_cb.p_tx_data_flow_cb(p_bcb->handle, BNEP_TX_FLOW_ON);
    }

    /* While not congested, send as many buffers as we can */
    while (!(p_bcb->con_flags & BNEP_FLAGS_L2CAP_CONGESTED)) {
      BT_HDR* p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_bcb->xmit_q);

      if (!p_buf) {
        break;
      }

      uint16_t len = p_buf->len;

      if (stack::l2cap::get_interface().L2CA_DataWrite(l2cap_cid, p_buf) !=
          tL2CAP_DW_RESULT::SUCCESS) {
        log::warn("Unable to write L2CAP data peer:{} cid:{} len:{}", p_bcb->rem_bda, l2cap_cid,
                  len);
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         bnep_data_ind
 *
 * Description      This function is called when data is received from L2CAP.
 *                  if we are the originator of the connection, we are the SDP
 *                  client, and the received message is queued for the client.
 *
 *                  If we are the destination of the connection, we are the SDP
 *                  server, so the message is passed to the server processing
 *                  function.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bnep_data_ind(uint16_t l2cap_cid, BT_HDR* p_buf) {
  tBNEP_CONN* p_bcb;
  uint8_t* p = (uint8_t*)(p_buf + 1) + p_buf->offset;
  uint16_t rem_len = p_buf->len;
  if (rem_len == 0) {
    osi_free(p_buf);
    return;
  }
  uint8_t type, ctrl_type, ext_type = 0;
  bool extension_present, fw_ext_present;
  uint16_t protocol = 0;

  /* Find CCB based on CID */
  p_bcb = bnepu_find_bcb_by_cid(l2cap_cid);
  if (p_bcb == NULL) {
    log::warn("BNEP - Rcvd L2CAP data, unknown CID: 0x{:x}", l2cap_cid);
    osi_free(p_buf);
    return;
  }

  /* Get the type and extension bits */
  type = *p++;
  extension_present = type >> 7;
  type &= 0x7f;
  if (type >= sizeof(bnep_frame_hdr_sizes) / sizeof(bnep_frame_hdr_sizes[0])) {
    log::info("BNEP - rcvd frame, bad type: 0x{:02x}", type);
    osi_free(p_buf);
    return;
  }
  if ((rem_len <= bnep_frame_hdr_sizes[type]) || (rem_len > BNEP_MTU_SIZE)) {
    log::debug("BNEP - rcvd frame, bad len: {}  type: 0x{:02x}", p_buf->len, type);
    osi_free(p_buf);
    return;
  }

  rem_len--;

  if ((p_bcb->con_state != BNEP_STATE_CONNECTED) &&
      (!(p_bcb->con_flags & BNEP_FLAGS_CONN_COMPLETED)) && (type != BNEP_FRAME_CONTROL)) {
    log::warn("BNEP - Ignored L2CAP data while in state: {}, CID: 0x{:x}", p_bcb->con_state,
              l2cap_cid);

    if (extension_present) {
      /*
      ** When there is no connection if a data packet is received
      ** with unknown control extension headers then those should be processed
      ** according to complain/ignore law
      */
      uint8_t ext, length;
      uint16_t org_len, new_len;
      /* parse the extension headers and process unknown control headers */
      org_len = rem_len;
      do {
        if (org_len < 2) {
          break;
        }
        ext = *p++;
        length = *p++;

        new_len = (length + 2);
        if (new_len > org_len) {
          break;
        }

        if ((ext & 0x7F) == BNEP_EXTENSION_FILTER_CONTROL) {
          if (length == 0) {
            break;
          }
          if (*p > BNEP_FILTER_MULTI_ADDR_RESPONSE_MSG) {
            bnep_send_command_not_understood(p_bcb, *p);
          }
        }

        p += length;

        org_len -= new_len;
      } while (ext & 0x80);
    }
    osi_free(p_buf);
    return;
  }

  if (type > BNEP_FRAME_COMPRESSED_ETHERNET_DEST_ONLY) {
    log::debug("BNEP - rcvd frame, unknown type: 0x{:02x}", type);
    osi_free(p_buf);
    return;
  }

  log::debug("BNEP - rcv frame, type: {} len: {} Ext: {}", type, p_buf->len, extension_present);

  /* Initialize addresses to 'not supplied' */
  RawAddress src_addr = RawAddress::kEmpty;
  RawAddress dst_addr = RawAddress::kEmpty;

  switch (type) {
    case BNEP_FRAME_GENERAL_ETHERNET:
      dst_addr = *(RawAddress*)p;
      p += BD_ADDR_LEN;
      src_addr = *(RawAddress*)p;
      p += BD_ADDR_LEN;
      BE_STREAM_TO_UINT16(protocol, p);
      rem_len -= 14;
      break;

    case BNEP_FRAME_CONTROL:
      ctrl_type = *p;
      p = bnep_process_control_packet(p_bcb, p, &rem_len, false);

      if (ctrl_type == BNEP_SETUP_CONNECTION_REQUEST_MSG &&
          p_bcb->con_state != BNEP_STATE_CONNECTED && extension_present && p && rem_len) {
        osi_free(p_bcb->p_pending_data);
        p_bcb->p_pending_data = (BT_HDR*)osi_malloc(rem_len + sizeof(BT_HDR));
        memcpy((uint8_t*)(p_bcb->p_pending_data + 1), p, rem_len);
        p_bcb->p_pending_data->len = rem_len;
        p_bcb->p_pending_data->offset = 0;
      } else {
        while (extension_present && p && rem_len) {
          ext_type = *p++;
          rem_len--;
          extension_present = ext_type >> 7;
          ext_type &= 0x7F;

          /* if unknown extension present stop processing */
          if (ext_type != BNEP_EXTENSION_FILTER_CONTROL) {
            break;
          }

          p = bnep_process_control_packet(p_bcb, p, &rem_len, true);
        }
      }
      osi_free(p_buf);
      return;

    case BNEP_FRAME_COMPRESSED_ETHERNET:
      BE_STREAM_TO_UINT16(protocol, p);
      rem_len -= 2;
      break;

    case BNEP_FRAME_COMPRESSED_ETHERNET_SRC_ONLY:
      src_addr = *(RawAddress*)p;
      p += BD_ADDR_LEN;
      BE_STREAM_TO_UINT16(protocol, p);
      rem_len -= 8;
      break;

    case BNEP_FRAME_COMPRESSED_ETHERNET_DEST_ONLY:
      dst_addr = *(RawAddress*)p;
      p += BD_ADDR_LEN;
      BE_STREAM_TO_UINT16(protocol, p);
      rem_len -= 8;
      break;
  }

  /* Process the header extension if there is one */
  while (extension_present && p && rem_len) {
    ext_type = *p;
    extension_present = ext_type >> 7;
    ext_type &= 0x7F;

    /* if unknown extension present stop processing */
    if (ext_type) {
      log::debug("Data extension type 0x{:x} found", ext_type);
      break;
    }

    p++;
    rem_len--;
    p = bnep_process_control_packet(p_bcb, p, &rem_len, true);
  }

  p_buf->offset += p_buf->len - rem_len;
  p_buf->len = rem_len;

  /* Always give the upper layer MAC addresses */
  if (src_addr == RawAddress::kEmpty) {
    src_addr = p_bcb->rem_bda;
  }

  if (dst_addr == RawAddress::kEmpty) {
    dst_addr = bluetooth::ToRawAddress(bluetooth::shim::GetController()->GetMacAddress());
  }

  /* check whether there are any extensions to be forwarded */
  if (ext_type) {
    fw_ext_present = true;
  } else {
    fw_ext_present = false;
  }

  if (bnep_cb.p_data_buf_cb) {
    (*bnep_cb.p_data_buf_cb)(p_bcb->handle, src_addr, dst_addr, protocol, p_buf, fw_ext_present);
  } else if (bnep_cb.p_data_ind_cb) {
    (*bnep_cb.p_data_ind_cb)(p_bcb->handle, src_addr, dst_addr, protocol, p, rem_len,
                             fw_ext_present);
    osi_free(p_buf);
  }
}

/*******************************************************************************
 *
 * Function         bnep_conn_timer_timeout
 *
 * Description      This function processes a timeout. If it is a startup
 *                  timeout, we check for reading our BD address. If it
 *                  is an L2CAP timeout, we send a disconnect req to L2CAP.
 *
 * Returns          void
 *
 ******************************************************************************/
void bnep_conn_timer_timeout(void* data) {
  tBNEP_CONN* p_bcb = (tBNEP_CONN*)data;

  log::debug("BNEP - CCB timeout in state: {}  CID: 0x{:x} flags {:x}, re_transmit {}",
             p_bcb->con_state, p_bcb->l2cap_cid, p_bcb->con_flags, p_bcb->re_transmits);

  if (p_bcb->con_state == BNEP_STATE_CONN_SETUP) {
    log::debug("BNEP - CCB timeout in state: {}  CID: 0x{:x}", p_bcb->con_state, p_bcb->l2cap_cid);

    if (!(p_bcb->con_flags & BNEP_FLAGS_IS_ORIG)) {
      if (!stack::l2cap::get_interface().L2CA_DisconnectReq(p_bcb->l2cap_cid)) {
        log::warn("Unable to request L2CAP disconnect peer:{} cid:{}", p_bcb->rem_bda,
                  p_bcb->l2cap_cid);
      }
      bnepu_release_bcb(p_bcb);
      return;
    }

    if (p_bcb->re_transmits++ != BNEP_MAX_RETRANSMITS) {
      bnep_send_conn_req(p_bcb);
      alarm_set_on_mloop(p_bcb->conn_timer, BNEP_CONN_TIMEOUT_MS, bnep_conn_timer_timeout, p_bcb);
    } else {
      if (!stack::l2cap::get_interface().L2CA_DisconnectReq(p_bcb->l2cap_cid)) {
        log::warn("Unable to request L2CAP disconnect peer:{} cid:{}", p_bcb->rem_bda,
                  p_bcb->l2cap_cid);
      }

      if ((p_bcb->con_flags & BNEP_FLAGS_IS_ORIG) && (bnep_cb.p_conn_state_cb)) {
        (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_CONN_FAILED, false);
      }

      bnepu_release_bcb(p_bcb);
      return;
    }
  } else if (p_bcb->con_state != BNEP_STATE_CONNECTED) {
    log::debug("BNEP - CCB timeout in state: {}  CID: 0x{:x}", p_bcb->con_state, p_bcb->l2cap_cid);

    if (!stack::l2cap::get_interface().L2CA_DisconnectReq(p_bcb->l2cap_cid)) {
      log::warn("Unable to request L2CAP disconnect peer:{} cid:{}", p_bcb->rem_bda,
                p_bcb->l2cap_cid);
    }

    /* Tell the user if there is a callback */
    if ((p_bcb->con_flags & BNEP_FLAGS_IS_ORIG) && (bnep_cb.p_conn_state_cb)) {
      (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_CONN_FAILED, false);
    }

    bnepu_release_bcb(p_bcb);
  } else if (p_bcb->con_flags & BNEP_FLAGS_FILTER_RESP_PEND) {
    if (p_bcb->re_transmits++ != BNEP_MAX_RETRANSMITS) {
      bnepu_send_peer_our_filters(p_bcb);
      alarm_set_on_mloop(p_bcb->conn_timer, BNEP_FILTER_SET_TIMEOUT_MS, bnep_conn_timer_timeout,
                         p_bcb);
    } else {
      if (!stack::l2cap::get_interface().L2CA_DisconnectReq(p_bcb->l2cap_cid)) {
        log::warn("Unable to request L2CAP disconnect peer:{} cid:{}", p_bcb->rem_bda,
                  p_bcb->l2cap_cid);
      }

      /* Tell the user if there is a callback */
      if (bnep_cb.p_conn_state_cb) {
        (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_SET_FILTER_FAIL, false);
      }

      bnepu_release_bcb(p_bcb);
      return;
    }
  } else if (p_bcb->con_flags & BNEP_FLAGS_MULTI_RESP_PEND) {
    if (p_bcb->re_transmits++ != BNEP_MAX_RETRANSMITS) {
      bnepu_send_peer_our_multi_filters(p_bcb);
      alarm_set_on_mloop(p_bcb->conn_timer, BNEP_FILTER_SET_TIMEOUT_MS, bnep_conn_timer_timeout,
                         p_bcb);
    } else {
      if (!stack::l2cap::get_interface().L2CA_DisconnectReq(p_bcb->l2cap_cid)) {
        log::warn("Unable to request L2CAP disconnect peer:{} cid:{}", p_bcb->rem_bda,
                  p_bcb->l2cap_cid);
      }

      /* Tell the user if there is a callback */
      if (bnep_cb.p_conn_state_cb) {
        (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_SET_FILTER_FAIL, false);
      }

      bnepu_release_bcb(p_bcb);
      return;
    }
  }
}

/*******************************************************************************
 *
 * Function         bnep_connected
 *
 * Description      This function is called when a connection is established
 *                  (after config).
 *
 * Returns          void
 *
 ******************************************************************************/
void bnep_connected(tBNEP_CONN* p_bcb) {
  bool is_role_change;

  if (p_bcb->con_flags & BNEP_FLAGS_CONN_COMPLETED) {
    is_role_change = true;
  } else {
    is_role_change = false;
  }

  p_bcb->con_state = BNEP_STATE_CONNECTED;
  p_bcb->con_flags |= BNEP_FLAGS_CONN_COMPLETED;
  p_bcb->con_flags &= (~BNEP_FLAGS_SETUP_RCVD);

  /* Ensure timer is stopped */
  alarm_cancel(p_bcb->conn_timer);
  p_bcb->re_transmits = 0;

  /* Tell the upper layer, if there is a callback */
  if (bnep_cb.p_conn_state_cb) {
    (*bnep_cb.p_conn_state_cb)(p_bcb->handle, p_bcb->rem_bda, BNEP_SUCCESS, is_role_change);
  }
}
