/******************************************************************************
 *
 *  Copyright 2004-2016 Broadcom Corporation
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
 *  This file contains action functions for advanced audio/video main state
 *  machine.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth-a2dp"

#include <bluetooth/log.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "avct_api.h"
#include "avdt_api.h"
#include "avrc_api.h"
#include "avrc_defs.h"
#include "bt_dev_class.h"
#include "bta/av/bta_av_int.h"
#include "bta/include/bta_ar_api.h"
#include "bta/include/utl.h"
#include "bta_av_api.h"
#include "bta_sys.h"
#include "btif/avrcp/avrcp_service.h"
#include "btif/include/btif_av.h"
#include "common/bind.h"
#include "device/include/device_iot_conf_defs.h"
#include "device/include/device_iot_config.h"
#include "device/include/interop.h"
#include "internal_include/bt_target.h"
#include "l2cap_types.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/list.h"
#include "osi/include/osi.h"  // UINT_TO_PTR PTR_TO_UINT
#include "osi/include/properties.h"
#include "sdpdefs.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_types.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/l2cap_interface.h"
#include "stack/include/sdp_api.h"
#include "stack/include/sdp_status.h"
#include "stack/sdp/sdp_discovery_db.h"
#include "types/raw_address.h"

using namespace bluetooth::legacy::stack::sdp;
using namespace bluetooth;

/*****************************************************************************
 *  Constants
 ****************************************************************************/
/* the timeout to wait for open req after setconfig for incoming connections */
#ifndef BTA_AV_SIGNALLING_TIMEOUT_MS
#define BTA_AV_SIGNALLING_TIMEOUT_MS (8 * 1000) /* 8 seconds */
#endif

/* Time to wait for signalling from SNK when it is initiated from SNK. */
/* If not, we will start signalling from SRC. */
#ifndef BTA_AV_ACCEPT_SIGNALLING_TIMEOUT_MS
#define BTA_AV_ACCEPT_SIGNALLING_TIMEOUT_MS (2 * 1000) /* 2 seconds */
#endif

static void bta_av_accept_signalling_timer_cback(void* data);

#ifndef AVRC_MIN_META_CMD_LEN
#define AVRC_MIN_META_CMD_LEN 20
#endif

/*******************************************************************************
 *
 * Function         bta_av_get_rcb_by_shdl
 *
 * Description      find the RCB associated with the given SCB handle.
 *
 * Returns          tBTA_AV_RCB
 *
 ******************************************************************************/
tBTA_AV_RCB* bta_av_get_rcb_by_shdl(uint8_t shdl) {
  tBTA_AV_RCB* p_rcb = NULL;
  int i;

  for (i = 0; i < BTA_AV_NUM_RCB; i++) {
    if (bta_av_cb.rcb[i].shdl == shdl && bta_av_cb.rcb[i].handle != BTA_AV_RC_HANDLE_NONE) {
      p_rcb = &bta_av_cb.rcb[i];
      break;
    }
  }
  return p_rcb;
}
#define BTA_AV_STS_NO_RSP 0xFF /* a number not used by tAVRC_STS */

/*******************************************************************************
 *
 * Function         bta_av_del_rc
 *
 * Description      delete the given AVRC handle.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_del_rc(tBTA_AV_RCB* p_rcb) {
  tBTA_AV_SCB* p_scb;
  uint8_t rc_handle; /* connected AVRCP handle */

  p_scb = NULL;
  if (p_rcb->handle != BTA_AV_RC_HANDLE_NONE) {
    if (p_rcb->shdl) {
      /* Validate array index*/
      if ((p_rcb->shdl - 1) < BTA_AV_NUM_STRS) {
        p_scb = bta_av_cb.p_scb[p_rcb->shdl - 1];
      }
      if (p_scb) {
        log::verbose("shdl:{}, srch:{} rc_handle:{}", p_rcb->shdl, p_scb->rc_handle, p_rcb->handle);
        if (p_scb->rc_handle == p_rcb->handle) {
          p_scb->rc_handle = BTA_AV_RC_HANDLE_NONE;
        }
        /* just in case the RC timer is active
        if (bta_av_cb.features & BTA_AV_FEAT_RCCT && p_scb->chnl ==
        BTA_AV_CHNL_AUDIO) */
        alarm_cancel(p_scb->avrc_ct_timer);
      }
    }

    log::verbose("handle: {} status=0x{:x}, rc_acp_handle:{}, idx:{}", p_rcb->handle, p_rcb->status,
                 bta_av_cb.rc_acp_handle, bta_av_cb.rc_acp_idx);
    rc_handle = p_rcb->handle;
    if (!(p_rcb->status & BTA_AV_RC_CONN_MASK) ||
        ((p_rcb->status & BTA_AV_RC_ROLE_MASK) == BTA_AV_RC_ROLE_INT)) {
      p_rcb->status = 0;
      p_rcb->handle = BTA_AV_RC_HANDLE_NONE;
      p_rcb->shdl = 0;
      p_rcb->lidx = 0;
    }
    /* else ACP && connected. do not clear the handle yet */
    AVRC_Close(rc_handle);
    if (rc_handle == bta_av_cb.rc_acp_handle) {
      bta_av_cb.rc_acp_handle = BTA_AV_RC_HANDLE_NONE;
    }
    log::verbose("end del_rc handle: {} status=0x{:x}, rc_acp_handle:{}, lidx:{}", p_rcb->handle,
                 p_rcb->status, bta_av_cb.rc_acp_handle, p_rcb->lidx);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_close_all_rc
 *
 * Description      close the all AVRC handle.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_close_all_rc(tBTA_AV_CB* p_cb) {
  int i;

  for (i = 0; i < BTA_AV_NUM_RCB; i++) {
    if ((p_cb->disabling) || (bta_av_cb.rcb[i].shdl != 0)) {
      bta_av_del_rc(&bta_av_cb.rcb[i]);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_del_sdp_rec
 *
 * Description      delete the given SDP record handle.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_del_sdp_rec(uint32_t* p_sdp_handle) {
  if (*p_sdp_handle != 0) {
    if (!get_legacy_stack_sdp_api()->handle.SDP_DeleteRecord(*p_sdp_handle)) {
      log::warn("Unable to delete SDP record:{}", *p_sdp_handle);
    }
    *p_sdp_handle = 0;
  }
}

/*******************************************************************************
 *
 * Function         bta_av_avrc_sdp_cback
 *
 * Description      AVRCP service discovery callback.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_avrc_sdp_cback(tSDP_STATUS /* status */) {
  BT_HDR_RIGID* p_msg = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));

  p_msg->event = BTA_AV_SDP_AVRC_DISC_EVT;

  bta_sys_sendmsg(p_msg);
}

/*******************************************************************************
 *
 * Function         bta_av_rc_ctrl_cback
 *
 * Description      AVRCP control callback.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_rc_ctrl_cback(uint8_t handle, uint8_t event, uint16_t /* result */,
                                 const RawAddress* peer_addr) {
  uint16_t msg_event = 0;

  if (btif_av_both_enable() && peer_addr != NULL && btif_av_peer_is_connected_sink(*peer_addr)) {
    log::warn("not cback legacy cback, and close the handle");

    if (event == AVRC_CLOSE_IND_EVT || event == AVRC_OPEN_IND_EVT) {
      log::verbose("resend close event");
      tBTA_AV_RC_CONN_CHG* p_msg = (tBTA_AV_RC_CONN_CHG*)osi_malloc(sizeof(tBTA_AV_RC_CONN_CHG));
      p_msg->hdr.event = BTA_AV_AVRC_CLOSE_EVT;
      p_msg->handle = handle;
      p_msg->peer_addr = *peer_addr;
      bta_sys_sendmsg(p_msg);
    }
    return;
  }

  log::verbose("handle: {} event=0x{:x}", handle, event);
  if (event == AVRC_OPEN_IND_EVT) {
    /* save handle of opened connection
    bta_av_cb.rc_handle = handle;*/

    msg_event = BTA_AV_AVRC_OPEN_EVT;
  } else if (event == AVRC_CLOSE_IND_EVT) {
    msg_event = BTA_AV_AVRC_CLOSE_EVT;
  } else if (event == AVRC_BROWSE_OPEN_IND_EVT) {
    msg_event = BTA_AV_AVRC_BROWSE_OPEN_EVT;
  } else if (event == AVRC_BROWSE_CLOSE_IND_EVT) {
    msg_event = BTA_AV_AVRC_BROWSE_CLOSE_EVT;
  }

  if (msg_event) {
    tBTA_AV_RC_CONN_CHG* p_msg = (tBTA_AV_RC_CONN_CHG*)osi_malloc(sizeof(tBTA_AV_RC_CONN_CHG));
    p_msg->hdr.event = msg_event;
    p_msg->handle = handle;
    p_msg->peer_addr = (peer_addr) ? (*peer_addr) : RawAddress::kEmpty;
    bta_sys_sendmsg(p_msg);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_msg_cback
 *
 * Description      AVRCP message callback.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_rc_msg_cback(uint8_t handle, uint8_t label, uint8_t opcode, tAVRC_MSG* p_msg) {
  uint8_t* p_data_src = NULL;
  uint16_t data_len = 0;

  log::verbose("handle: {} opcode=0x{:x}", handle, opcode);

  /* Copy avrc packet into BTA message buffer (for sending to BTA state machine)
   */

  /* Get size of payload data  (for vendor and passthrough messages only; for
   * browsing
   * messages, use zero-copy) */
  if (opcode == AVRC_OP_VENDOR && p_msg->vendor.p_vendor_data != NULL) {
    p_data_src = p_msg->vendor.p_vendor_data;
    data_len = (uint16_t)p_msg->vendor.vendor_len;
  } else if (opcode == AVRC_OP_PASS_THRU && p_msg->pass.p_pass_data != NULL) {
    p_data_src = p_msg->pass.p_pass_data;
    data_len = (uint16_t)p_msg->pass.pass_len;
  }

  /* Create a copy of the message */
  tBTA_AV_RC_MSG* p_buf = (tBTA_AV_RC_MSG*)osi_malloc(sizeof(tBTA_AV_RC_MSG) + data_len);

  p_buf->hdr.event = BTA_AV_AVRC_MSG_EVT;
  p_buf->handle = handle;
  p_buf->label = label;
  p_buf->opcode = opcode;
  memcpy(&p_buf->msg, p_msg, sizeof(tAVRC_MSG));
  /* Copy the data payload, and set the pointer to it */
  if (p_data_src != NULL) {
    uint8_t* p_data_dst = (uint8_t*)(p_buf + 1);
    memcpy(p_data_dst, p_data_src, data_len);

    /* Update bta message buffer to point to payload data */
    /* (Note AVRC_OP_BROWSING uses zero-copy: p_buf->msg.browse.p_browse_data
     * already points to original avrc buffer) */
    if (opcode == AVRC_OP_VENDOR) {
      p_buf->msg.vendor.p_vendor_data = p_data_dst;
    } else if (opcode == AVRC_OP_PASS_THRU) {
      p_buf->msg.pass.p_pass_data = p_data_dst;
    }
  }

  if (opcode == AVRC_OP_BROWSE) {
    /* set p_pkt to NULL, so avrc would not free the buffer */
    p_msg->browse.p_browse_pkt = NULL;
  }

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         bta_av_rc_create
 *
 * Description      alloc RCB and call AVRC_Open
 *
 * Returns          the created rc handle
 *
 ******************************************************************************/
uint8_t bta_av_rc_create(tBTA_AV_CB* p_cb, tAVCT_ROLE role, uint8_t shdl, uint8_t lidx) {
  if ((!btif_av_src_sink_coexist_enabled() ||
       (btif_av_src_sink_coexist_enabled() && !btif_av_is_sink_enabled() &&
        btif_av_is_source_enabled())) &&
      is_new_avrcp_enabled()) {
    log::info("Skipping RC creation for the old AVRCP profile");
    return BTA_AV_RC_HANDLE_NONE;
  }

  RawAddress bda = RawAddress::kAny;
  uint8_t status = BTA_AV_RC_ROLE_ACP;
  int i;
  uint8_t rc_handle;
  tBTA_AV_RCB* p_rcb{nullptr};

  if (role == AVCT_ROLE_INITIATOR) {
    // Can't grab a stream control block that doesn't have a valid handle
    if (!shdl) {
      log::error("Can't grab stream control block for shdl = {} -> index = {}", shdl, shdl - 1);
      return BTA_AV_RC_HANDLE_NONE;
    }
    tBTA_AV_SCB* p_scb = p_cb->p_scb[shdl - 1];
    bda = p_scb->PeerAddress();
    status = BTA_AV_RC_ROLE_INT;
    DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(p_scb->PeerAddress(), IOT_CONF_KEY_AVRCP_CONN_COUNT);

  } else {
    p_rcb = bta_av_get_rcb_by_shdl(shdl);
    if (p_rcb != NULL) {
      log::error("ACP handle exist for shdl:{}", shdl);
      p_rcb->lidx = lidx;
      return p_rcb->handle;
    }
  }

  tAVRC_CONN_CB ccb = {
          .ctrl_cback = base::Bind(bta_av_rc_ctrl_cback),
          .msg_cback = base::Bind(bta_av_rc_msg_cback),
          .company_id = p_bta_av_cfg->company_id,
          .conn = role,
          // note: BTA_AV_FEAT_RCTG = AVRC_CT_TARGET, BTA_AV_FEAT_RCCT = AVRC_CT_CONTROL
          .control =
                  static_cast<uint8_t>(p_cb->features & (BTA_AV_FEAT_RCTG | BTA_AV_FEAT_RCCT |
                                                         BTA_AV_FEAT_METADATA | AVRC_CT_PASSIVE)),
  };

  if (AVRC_Open(&rc_handle, &ccb, bda) != AVRC_SUCCESS) {
    DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(bda, IOT_CONF_KEY_AVRCP_CONN_FAIL_COUNT);
    return BTA_AV_RC_HANDLE_NONE;
  }

  i = rc_handle;
  p_rcb = &p_cb->rcb[i];

  if (p_rcb->handle != BTA_AV_RC_HANDLE_NONE) {
    log::error("found duplicated handle:{}", rc_handle);
  }

  p_rcb->handle = rc_handle;
  p_rcb->status = status;
  p_rcb->shdl = shdl;
  p_rcb->lidx = lidx;
  p_rcb->peer_features = 0;
  p_rcb->peer_ct_features = 0;
  p_rcb->peer_tg_features = 0;
  p_rcb->cover_art_psm = 0;
  if (lidx == (BTA_AV_NUM_LINKS + 1)) {
    /* this LIDX is reserved for the AVRCP ACP connection */
    p_cb->rc_acp_handle = p_rcb->handle;
    p_cb->rc_acp_idx = (i + 1);
    log::verbose("rc_acp_handle:{} idx:{}", p_cb->rc_acp_handle, p_cb->rc_acp_idx);
  }
  log::verbose("create {}, role: {}, shdl:{}, rc_handle:{}, lidx:{}, status:0x{:x}", i,
               avct_role_text(role), shdl, p_rcb->handle, lidx, p_rcb->status);

  return rc_handle;
}

/*******************************************************************************
 *
 * Function         bta_av_valid_group_navi_msg
 *
 * Description      Check if it is Group Navigation Msg for Metadata
 *
 * Returns          AVRC_RSP_ACCEPT or AVRC_RSP_NOT_IMPL
 *
 ******************************************************************************/
static tBTA_AV_CODE bta_av_group_navi_supported(uint8_t len, uint8_t* p_data, bool is_inquiry) {
  tBTA_AV_CODE ret = AVRC_RSP_NOT_IMPL;
  uint8_t* p_ptr = p_data;
  uint16_t u16;
  uint32_t u32;

  if (p_bta_av_cfg->avrc_group && len == BTA_GROUP_NAVI_MSG_OP_DATA_LEN) {
    BTA_AV_BE_STREAM_TO_CO_ID(u32, p_ptr);
    BE_STREAM_TO_UINT16(u16, p_ptr);

    if (u32 == AVRC_CO_METADATA) {
      if (is_inquiry) {
        if (u16 <= AVRC_PDU_PREV_GROUP) {
          ret = AVRC_RSP_IMPL_STBL;
        }
      } else {
        if (u16 <= AVRC_PDU_PREV_GROUP) {
          ret = AVRC_RSP_ACCEPT;
        } else {
          ret = AVRC_RSP_REJ;
        }
      }
    }
  }

  return ret;
}

/*******************************************************************************
 *
 * Function         bta_av_op_supported
 *
 * Description      Check if remote control operation is supported.
 *
 * Returns          AVRC_RSP_ACCEPT of supported, AVRC_RSP_NOT_IMPL if not.
 *
 ******************************************************************************/
static tBTA_AV_CODE bta_av_op_supported(tBTA_AV_RC rc_id, bool is_inquiry) {
  tBTA_AV_CODE ret_code = AVRC_RSP_NOT_IMPL;

  if (p_bta_av_rc_id) {
    if (is_inquiry) {
      if (p_bta_av_rc_id[rc_id >> 4] & (1 << (rc_id & 0x0F))) {
        ret_code = AVRC_RSP_IMPL_STBL;
      }
    } else {
      if (p_bta_av_rc_id[rc_id >> 4] & (1 << (rc_id & 0x0F))) {
        ret_code = AVRC_RSP_ACCEPT;
      } else if ((p_bta_av_cfg->rc_pass_rsp == AVRC_RSP_INTERIM) && p_bta_av_rc_id_ac) {
        if (p_bta_av_rc_id_ac[rc_id >> 4] & (1 << (rc_id & 0x0F))) {
          ret_code = AVRC_RSP_INTERIM;
        }
      }
    }
  }
  return ret_code;
}

/*******************************************************************************
 *
 * Function         bta_av_find_lcb
 *
 * Description      Given BD_addr, find the associated LCB.
 *
 * Returns          NULL, if not found.
 *
 ******************************************************************************/
tBTA_AV_LCB* bta_av_find_lcb(const RawAddress& addr, uint8_t op) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  int xx;
  uint8_t mask;
  tBTA_AV_LCB* p_lcb = NULL;

  log::verbose("address: {} op:{}", addr, op);
  for (xx = 0; xx < BTA_AV_NUM_LINKS; xx++) {
    mask = 1 << xx; /* the used mask for this lcb */
    if ((mask & p_cb->conn_lcb) && p_cb->lcb[xx].addr == addr) {
      p_lcb = &p_cb->lcb[xx];
      if (op == BTA_AV_LCB_FREE) {
        p_cb->conn_lcb &= ~mask; /* clear the connect mask */
        log::verbose("conn_lcb: 0x{:x}", p_cb->conn_lcb);
      }
      break;
    }
  }
  return p_lcb;
}

/*******************************************************************************
 *
 * Function         bta_av_rc_opened
 *
 * Description      Set AVRCP state to opened.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_opened(tBTA_AV_CB* p_cb, tBTA_AV_DATA* p_data) {
  tBTA_AV_RC_OPEN rc_open;
  tBTA_AV_SCB* p_scb;
  int i;
  uint8_t shdl = 0;
  tBTA_AV_LCB* p_lcb;
  tBTA_AV_RCB* p_rcb;
  uint8_t tmp;
  uint8_t disc = 0;

  /* find the SCB & stop the timer */
  for (i = 0; i < BTA_AV_NUM_STRS; i++) {
    p_scb = p_cb->p_scb[i];
    if (p_scb && p_scb->PeerAddress() == p_data->rc_conn_chg.peer_addr) {
      p_scb->rc_handle = p_data->rc_conn_chg.handle;
      log::verbose("shdl:{}, srch {}", i + 1, p_scb->rc_handle);
      shdl = i + 1;
      log::info("allow incoming AVRCP connections:{}", p_scb->use_rc);
      alarm_cancel(p_scb->avrc_ct_timer);
      disc = p_scb->hndl;
      break;
    }
  }

  i = p_data->rc_conn_chg.handle;
  if (p_cb->rcb[i].handle == BTA_AV_RC_HANDLE_NONE) {
    log::error("not a valid handle:{} any more", i);
    return;
  }

  log::verbose("local features {} peer features {}", p_cb->features, p_cb->rcb[i].peer_features);

  /* listen to browsing channel when the connection is open,
   * if peer initiated AVRCP connection and local device supports browsing
   * channel */
  AVRC_OpenBrowse(p_data->rc_conn_chg.handle, AVCT_ROLE_ACCEPTOR);

  if (p_cb->rcb[i].lidx == (BTA_AV_NUM_LINKS + 1) && shdl != 0) {
    /* rc is opened on the RC only ACP channel, but is for a specific
     * SCB -> need to switch RCBs */
    p_rcb = bta_av_get_rcb_by_shdl(shdl);
    if (p_rcb) {
      p_rcb->shdl = p_cb->rcb[i].shdl;
      tmp = p_rcb->lidx;
      p_rcb->lidx = p_cb->rcb[i].lidx;
      p_cb->rcb[i].lidx = tmp;
      p_cb->rc_acp_handle = p_rcb->handle;
      p_cb->rc_acp_idx = (p_rcb - p_cb->rcb) + 1;
      log::verbose("switching RCB rc_acp_handle:{} idx:{}", p_cb->rc_acp_handle, p_cb->rc_acp_idx);
    }
  }

  p_cb->rcb[i].shdl = shdl;
  rc_open.rc_handle = i;
  log::error("rcb[{}] shdl:{} lidx:{}/{}", i, shdl, p_cb->rcb[i].lidx,
             p_cb->lcb[BTA_AV_NUM_LINKS].lidx);
  p_cb->rcb[i].status |= BTA_AV_RC_CONN_MASK;

  if (!shdl && 0 == p_cb->lcb[BTA_AV_NUM_LINKS].lidx) {
    /* no associated SCB -> connected to an RC only device
     * update the index to the extra LCB */
    p_lcb = &p_cb->lcb[BTA_AV_NUM_LINKS];
    p_lcb->addr = p_data->rc_conn_chg.peer_addr;
    p_lcb->lidx = BTA_AV_NUM_LINKS + 1;
    p_cb->rcb[i].lidx = p_lcb->lidx;
    p_lcb->conn_msk = 1;
    log::error("bd_addr: {} rcb[{}].lidx={}, lcb.conn_msk=x{:x}", p_lcb->addr, i, p_cb->rcb[i].lidx,
               p_lcb->conn_msk);
    disc = p_data->rc_conn_chg.handle | BTA_AV_CHNL_MSK;
  }

  rc_open.peer_addr = p_data->rc_conn_chg.peer_addr;
  rc_open.peer_features = p_cb->rcb[i].peer_features;
  rc_open.cover_art_psm = p_cb->rcb[i].cover_art_psm;
  if (btif_av_both_enable()) {
    if (rc_open.peer_addr == p_cb->rc_feature.peer_addr) {
      rc_open.peer_features = p_cb->rc_feature.peer_features;
      rc_open.peer_ct_features = p_cb->rc_feature.peer_ct_features;
      rc_open.peer_tg_features = p_cb->rc_feature.peer_tg_features;
    } else {
      rc_open.peer_features = p_cb->rcb[i].peer_features;
      rc_open.peer_ct_features = p_cb->rcb[i].peer_ct_features;
      rc_open.peer_tg_features = p_cb->rcb[i].peer_tg_features;
    }
    rc_open.status = BTA_AV_SUCCESS;
    log::verbose(
            "local features:0x{:x} peer_features:0x{:x}, peer_ct_feature:0x{:x}, "
            "peer_tg_feature:0x{:x}",
            p_cb->features, rc_open.peer_features, rc_open.peer_ct_features,
            rc_open.peer_tg_features);
    if (rc_open.peer_features == 0 && rc_open.peer_ct_features == 0 &&
        rc_open.peer_tg_features == 0) {
      /* we have not done SDP on peer RC capabilities.
       * peer must have initiated the RC connection
       * We Don't have SDP records of Peer, so we by
       * default will take values depending upon registered
       * features */
      if (p_cb->features & BTA_AV_FEAT_RCTG) {
        rc_open.peer_ct_features |= BTA_AV_FEAT_RCCT;
        rc_open.peer_features |= BTA_AV_FEAT_RCCT;
      }
      bta_av_rc_disc(disc);
    }
    (*p_cb->p_cback)(BTA_AV_RC_OPEN_EVT, (tBTA_AV*)&rc_open);

    /* if local initiated AVRCP connection and both peer and locals device
     * support
     * browsing channel, open the browsing channel now
     * Some TG would not broadcast browse feature hence check inter-op. */
    if ((p_cb->features & BTA_AV_FEAT_BROWSE) &&
        ((rc_open.peer_ct_features & BTA_AV_FEAT_BROWSE) ||
         (rc_open.peer_tg_features & BTA_AV_FEAT_BROWSE))) {
      if ((p_cb->rcb[i].status & BTA_AV_RC_ROLE_MASK) == BTA_AV_RC_ROLE_INT) {
        log::verbose("opening AVRC Browse channel");
        AVRC_OpenBrowse(p_data->rc_conn_chg.handle, AVCT_ROLE_INITIATOR);
      }
    }
    return;
  }
  rc_open.status = BTA_AV_SUCCESS;
  log::verbose("local features:x{:x} peer_features:x{:x}", p_cb->features, rc_open.peer_features);
  log::verbose("cover art psm:x{:x}", rc_open.cover_art_psm);
  if (rc_open.peer_features == 0) {
    /* we have not done SDP on peer RC capabilities.
     * peer must have initiated the RC connection */
    if (p_cb->features & BTA_AV_FEAT_RCCT) {
      rc_open.peer_features |= BTA_AV_FEAT_RCTG;
    }
    if (p_cb->features & BTA_AV_FEAT_RCTG) {
      rc_open.peer_features |= BTA_AV_FEAT_RCCT;
    }

    bta_av_rc_disc(disc);
  }
  tBTA_AV bta_av_data;
  bta_av_data.rc_open = rc_open;
  (*p_cb->p_cback)(BTA_AV_RC_OPEN_EVT, &bta_av_data);

  /* if local initiated AVRCP connection and both peer and locals device support
   * browsing channel, open the browsing channel now
   * TODO (sanketa): Some TG would not broadcast browse feature hence check
   * inter-op. */
  if ((p_cb->features & BTA_AV_FEAT_BROWSE) && (rc_open.peer_features & BTA_AV_FEAT_BROWSE) &&
      ((p_cb->rcb[i].status & BTA_AV_RC_ROLE_MASK) == BTA_AV_RC_ROLE_INT)) {
    log::verbose("opening AVRC Browse channel");
    AVRC_OpenBrowse(p_data->rc_conn_chg.handle, AVCT_ROLE_INITIATOR);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_remote_cmd
 *
 * Description      Send an AVRCP remote control command.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_remote_cmd(tBTA_AV_CB* p_cb, tBTA_AV_DATA* p_data) {
  tBTA_AV_RCB* p_rcb;
  if (p_cb->features & BTA_AV_FEAT_RCCT) {
    if (p_data->hdr.layer_specific < BTA_AV_NUM_RCB) {
      p_rcb = &p_cb->rcb[p_data->hdr.layer_specific];
      if (p_rcb->status & BTA_AV_RC_CONN_MASK) {
        AVRC_PassCmd(p_rcb->handle, p_data->api_remote_cmd.label, &p_data->api_remote_cmd.msg);
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_vendor_cmd
 *
 * Description      Send an AVRCP vendor specific command.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_vendor_cmd(tBTA_AV_CB* p_cb, tBTA_AV_DATA* p_data) {
  tBTA_AV_RCB* p_rcb;
  if ((p_cb->features & (BTA_AV_FEAT_RCCT | BTA_AV_FEAT_VENDOR)) ==
      (BTA_AV_FEAT_RCCT | BTA_AV_FEAT_VENDOR)) {
    if (p_data->hdr.layer_specific < BTA_AV_NUM_RCB) {
      p_rcb = &p_cb->rcb[p_data->hdr.layer_specific];
      AVRC_VendorCmd(p_rcb->handle, p_data->api_vendor.label, &p_data->api_vendor.msg);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_vendor_rsp
 *
 * Description      Send an AVRCP vendor specific response.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_vendor_rsp(tBTA_AV_CB* p_cb, tBTA_AV_DATA* p_data) {
  tBTA_AV_RCB* p_rcb;
  if ((p_cb->features & (BTA_AV_FEAT_RCTG | BTA_AV_FEAT_VENDOR)) ==
      (BTA_AV_FEAT_RCTG | BTA_AV_FEAT_VENDOR)) {
    if (p_data->hdr.layer_specific < BTA_AV_NUM_RCB) {
      p_rcb = &p_cb->rcb[p_data->hdr.layer_specific];
      AVRC_VendorRsp(p_rcb->handle, p_data->api_vendor.label, &p_data->api_vendor.msg);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_meta_rsp
 *
 * Description      Send an AVRCP metadata/advanced control command/response.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_meta_rsp(tBTA_AV_CB* p_cb, tBTA_AV_DATA* p_data) {
  tBTA_AV_RCB* p_rcb;
  bool do_free = true;

  if ((p_cb->features & BTA_AV_FEAT_METADATA) && (p_data->hdr.layer_specific < BTA_AV_NUM_RCB)) {
    if ((p_data->api_meta_rsp.is_rsp && (p_cb->features & BTA_AV_FEAT_RCTG)) ||
        (!p_data->api_meta_rsp.is_rsp && (p_cb->features & BTA_AV_FEAT_RCCT))) {
      p_rcb = &p_cb->rcb[p_data->hdr.layer_specific];
      if (p_rcb->handle != BTA_AV_RC_HANDLE_NONE) {
        AVRC_MsgReq(p_rcb->handle, p_data->api_meta_rsp.label, p_data->api_meta_rsp.rsp_code,
                    p_data->api_meta_rsp.p_pkt, false);
        do_free = false;
      }
    }
  }

  if (do_free) {
    osi_free_and_reset((void**)&p_data->api_meta_rsp.p_pkt);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_free_rsp
 *
 * Description      free an AVRCP metadata command buffer.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_free_rsp(tBTA_AV_CB* /* p_cb */, tBTA_AV_DATA* p_data) {
  osi_free_and_reset((void**)&p_data->api_meta_rsp.p_pkt);
}

/*******************************************************************************
 *
 * Function         bta_av_rc_free_browse_msg
 *
 * Description      free an AVRCP browse message buffer.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_free_browse_msg(tBTA_AV_CB* /* p_cb */, tBTA_AV_DATA* p_data) {
  if (p_data->rc_msg.opcode == AVRC_OP_BROWSE) {
    osi_free_and_reset((void**)&p_data->rc_msg.msg.browse.p_browse_pkt);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_chk_notif_evt_id
 *
 * Description      make sure the requested player id is valid.
 *
 * Returns          BTA_AV_STS_NO_RSP, if no error
 *
 ******************************************************************************/
static tAVRC_STS bta_av_chk_notif_evt_id(tAVRC_MSG_VENDOR* p_vendor) {
  tAVRC_STS status = BTA_AV_STS_NO_RSP;
  uint8_t xx;
  uint16_t u16;
  uint8_t* p = p_vendor->p_vendor_data + 2;

  BE_STREAM_TO_UINT16(u16, p);
  /* double check the fixed length */
  if ((u16 != 5) || (p_vendor->vendor_len != 9)) {
    status = AVRC_STS_INTERNAL_ERR;
  } else {
    if (btif_av_both_enable()) {
      for (xx = 0; xx < bta_av_cfg.num_evt_ids; xx++) {
        if (*p == bta_av_cfg.p_meta_evt_ids[xx]) {
          return status;
        }
      }
      for (xx = 0; xx < get_bta_avk_cfg()->num_evt_ids; xx++) {
        if (*p == get_bta_avk_cfg()->p_meta_evt_ids[xx]) {
          return status;
        }
      }
      return AVRC_STS_BAD_PARAM;
    }
    /* make sure the player_id is valid */
    for (xx = 0; xx < p_bta_av_cfg->num_evt_ids; xx++) {
      if (*p == p_bta_av_cfg->p_meta_evt_ids[xx]) {
        break;
      }
    }
    if (xx == p_bta_av_cfg->num_evt_ids) {
      status = AVRC_STS_BAD_PARAM;
    }
  }

  return status;
}

static void bta_av_proc_rsp(tAVRC_RESPONSE* p_rc_rsp) {
  uint16_t rc_ver = 0x105;
  const tBTA_AV_CFG* p_src_cfg = NULL;
  if (rc_ver != 0x103) {
    p_src_cfg = &bta_av_cfg;
  } else {
    p_src_cfg = &bta_av_cfg_compatibility;
  }
  p_rc_rsp->get_caps.count = p_src_cfg->num_evt_ids;
  memcpy(p_rc_rsp->get_caps.param.event_id, p_src_cfg->p_meta_evt_ids, p_src_cfg->num_evt_ids);
  log::verbose("ver: 0x{:x}", rc_ver);
  /* if it's not 1.3, then there should be a absolute volume */
  if (rc_ver != 0x103) {
    uint8_t evt_cnt = p_rc_rsp->get_caps.count;
    p_rc_rsp->get_caps.count += get_bta_avk_cfg()->num_evt_ids;
    if (evt_cnt < AVRC_CAP_MAX_NUM_EVT_ID) {
      uint32_t i = 0;
      for (i = 0; i < get_bta_avk_cfg()->num_evt_ids && i + evt_cnt < AVRC_CAP_MAX_NUM_EVT_ID;
           i++) {
        p_rc_rsp->get_caps.param.event_id[evt_cnt + i] = get_bta_avk_cfg()->p_meta_evt_ids[i];
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_proc_meta_cmd
 *
 * Description      Process an AVRCP metadata command from the peer.
 *
 * Returns          true to respond immediately
 *
 ******************************************************************************/
static tBTA_AV_EVT bta_av_proc_meta_cmd(tAVRC_RESPONSE* p_rc_rsp, tBTA_AV_RC_MSG* p_msg,
                                        uint8_t* p_ctype) {
  tBTA_AV_EVT evt = BTA_AV_META_MSG_EVT;
  uint8_t u8, pdu, *p;
  uint16_t u16;
  tAVRC_MSG_VENDOR* p_vendor = &p_msg->msg.vendor;

  if (p_vendor->vendor_len == 0) {
    p_rc_rsp->rsp.status = AVRC_STS_BAD_PARAM;
    log::verbose("p_vendor->vendor_len == 0");
    // the caller of this function assume 0 to be an invalid event
    return 0;
  }

  pdu = *(p_vendor->p_vendor_data);
  p_rc_rsp->pdu = pdu;
  *p_ctype = AVRC_RSP_REJ;

  /* Check to ansure a  valid minimum meta data length */
  if ((AVRC_MIN_META_CMD_LEN + p_vendor->vendor_len) > AVRC_META_CMD_BUF_SIZE) {
    /* reject it */
    p_rc_rsp->rsp.status = AVRC_STS_BAD_PARAM;
    log::error("Invalid meta-command length: {}", p_vendor->vendor_len);
    return 0;
  }

  /* Metadata messages only use PANEL sub-unit type */
  if (p_vendor->hdr.subunit_type != AVRC_SUB_PANEL) {
    log::verbose("SUBUNIT must be PANEL");
    /* reject it */
    evt = 0;
    p_vendor->hdr.ctype = AVRC_RSP_NOT_IMPL;
    p_vendor->vendor_len = 0;
    p_rc_rsp->rsp.status = AVRC_STS_BAD_PARAM;
  } else if (!AVRC_IsValidAvcType(pdu, p_vendor->hdr.ctype)) {
    log::verbose("Invalid pdu/ctype: 0x{:x}, {}", pdu, p_vendor->hdr.ctype);
    /* reject invalid message without reporting to app */
    evt = 0;
    p_rc_rsp->rsp.status = AVRC_STS_BAD_CMD;
  } else {
    switch (pdu) {
      case AVRC_PDU_GET_CAPABILITIES:
        /* process GetCapabilities command without reporting the event to app */
        evt = 0;
        if (p_vendor->vendor_len != 5) {
          p_rc_rsp->get_caps.status = AVRC_STS_INTERNAL_ERR;
          break;
        }
        u8 = *(p_vendor->p_vendor_data + 4);
        p = p_vendor->p_vendor_data + 2;
        p_rc_rsp->get_caps.capability_id = u8;
        BE_STREAM_TO_UINT16(u16, p);
        if (u16 != 1) {
          p_rc_rsp->get_caps.status = AVRC_STS_INTERNAL_ERR;
        } else {
          p_rc_rsp->get_caps.status = AVRC_STS_NO_ERROR;
          if (u8 == AVRC_CAP_COMPANY_ID) {
            *p_ctype = AVRC_RSP_IMPL_STBL;
            p_rc_rsp->get_caps.count = p_bta_av_cfg->num_co_ids;
            memcpy(p_rc_rsp->get_caps.param.company_id, p_bta_av_cfg->p_meta_co_ids,
                   (p_bta_av_cfg->num_co_ids << 2));
          } else if (u8 == AVRC_CAP_EVENTS_SUPPORTED) {
            *p_ctype = AVRC_RSP_IMPL_STBL;
            if (btif_av_src_sink_coexist_enabled() && btif_av_both_enable()) {
              bta_av_proc_rsp(p_rc_rsp);
              break;
            }
            p_rc_rsp->get_caps.count = p_bta_av_cfg->num_evt_ids;
            memcpy(p_rc_rsp->get_caps.param.event_id, p_bta_av_cfg->p_meta_evt_ids,
                   p_bta_av_cfg->num_evt_ids);
          } else {
            log::verbose("Invalid capability ID: 0x{:x}", u8);
            /* reject - unknown capability ID */
            p_rc_rsp->get_caps.status = AVRC_STS_BAD_PARAM;
          }
        }
        break;

      case AVRC_PDU_REGISTER_NOTIFICATION:
        /* make sure the event_id is implemented */
        p_rc_rsp->rsp.status = bta_av_chk_notif_evt_id(p_vendor);
        if (p_rc_rsp->rsp.status != BTA_AV_STS_NO_RSP) {
          evt = 0;
        }
        break;
    }
  }

  return evt;
}

/*******************************************************************************
 *
 * Function         bta_av_rc_msg
 *
 * Description      Process an AVRCP message from the peer.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_msg(tBTA_AV_CB* p_cb, tBTA_AV_DATA* p_data) {
  tBTA_AV_EVT evt = 0;
  tBTA_AV av;
  BT_HDR* p_pkt = NULL;
  tAVRC_MSG_VENDOR* p_vendor = &p_data->rc_msg.msg.vendor;
  bool is_inquiry = ((p_data->rc_msg.msg.hdr.ctype == AVRC_CMD_SPEC_INQ) ||
                     p_data->rc_msg.msg.hdr.ctype == AVRC_CMD_GEN_INQ);
  uint8_t ctype = 0;
  tAVRC_RESPONSE rc_rsp;

  rc_rsp.rsp.status = BTA_AV_STS_NO_RSP;

  if (NULL == p_data) {
    log::error("Message from peer with no data");
    return;
  }

  log::verbose("opcode={:x}, ctype={:x}", p_data->rc_msg.opcode, p_data->rc_msg.msg.hdr.ctype);

  if (p_data->rc_msg.opcode == AVRC_OP_PASS_THRU) {
    /* if this is a pass thru command */
    if ((p_data->rc_msg.msg.hdr.ctype == AVRC_CMD_CTRL) ||
        (p_data->rc_msg.msg.hdr.ctype == AVRC_CMD_SPEC_INQ) ||
        (p_data->rc_msg.msg.hdr.ctype == AVRC_CMD_GEN_INQ)) {
      /* check if operation is supported */
      char avrcp_ct_support[PROPERTY_VALUE_MAX];
      osi_property_get("bluetooth.pts.avrcp_ct.support", avrcp_ct_support, "false");
      if (p_data->rc_msg.msg.pass.op_id == AVRC_ID_VENDOR) {
        p_data->rc_msg.msg.hdr.ctype = AVRC_RSP_NOT_IMPL;
        if (p_cb->features & BTA_AV_FEAT_METADATA) {
          p_data->rc_msg.msg.hdr.ctype =
                  bta_av_group_navi_supported(p_data->rc_msg.msg.pass.pass_len,
                                              p_data->rc_msg.msg.pass.p_pass_data, is_inquiry);
        }
      } else if (((p_data->rc_msg.msg.pass.op_id == AVRC_ID_VOL_UP) ||
                  (p_data->rc_msg.msg.pass.op_id == AVRC_ID_VOL_DOWN)) &&
                 !strcmp(avrcp_ct_support, "true")) {
        p_data->rc_msg.msg.hdr.ctype = AVRC_RSP_ACCEPT;
      } else {
        p_data->rc_msg.msg.hdr.ctype =
                bta_av_op_supported(p_data->rc_msg.msg.pass.op_id, is_inquiry);
      }

      log::verbose("ctype {}", p_data->rc_msg.msg.hdr.ctype);

      /* send response */
      if (p_data->rc_msg.msg.hdr.ctype != AVRC_RSP_INTERIM) {
        AVRC_PassRsp(p_data->rc_msg.handle, p_data->rc_msg.label, &p_data->rc_msg.msg.pass);
      }

      /* set up for callback if supported */
      if (p_data->rc_msg.msg.hdr.ctype == AVRC_RSP_ACCEPT ||
          p_data->rc_msg.msg.hdr.ctype == AVRC_RSP_INTERIM) {
        evt = BTA_AV_REMOTE_CMD_EVT;
        av.remote_cmd.rc_id = p_data->rc_msg.msg.pass.op_id;
        av.remote_cmd.key_state = p_data->rc_msg.msg.pass.state;
        av.remote_cmd.p_data = p_data->rc_msg.msg.pass.p_pass_data;
        av.remote_cmd.len = p_data->rc_msg.msg.pass.pass_len;
        memcpy(&av.remote_cmd.hdr, &p_data->rc_msg.msg.hdr, sizeof(tAVRC_HDR));
        av.remote_cmd.label = p_data->rc_msg.label;
      }
    } else if (p_data->rc_msg.msg.hdr.ctype >= AVRC_RSP_NOT_IMPL) {
      /* else if this is a pass thru response */
      /* id response type is not impl, we have to release label */
      /* set up for callback */
      evt = BTA_AV_REMOTE_RSP_EVT;
      av.remote_rsp.rc_id = p_data->rc_msg.msg.pass.op_id;
      av.remote_rsp.key_state = p_data->rc_msg.msg.pass.state;
      av.remote_rsp.rsp_code = p_data->rc_msg.msg.hdr.ctype;
      av.remote_rsp.label = p_data->rc_msg.label;
      av.remote_rsp.len = p_data->rc_msg.msg.pass.pass_len;
      av.remote_rsp.p_data = NULL;

      /* If this response is for vendor unique command  */
      if ((p_data->rc_msg.msg.pass.op_id == AVRC_ID_VENDOR) &&
          (p_data->rc_msg.msg.pass.pass_len > 0)) {
        av.remote_rsp.p_data = (uint8_t*)osi_malloc(p_data->rc_msg.msg.pass.pass_len);
        log::verbose("Vendor Unique data len = {}", p_data->rc_msg.msg.pass.pass_len);
        memcpy(av.remote_rsp.p_data, p_data->rc_msg.msg.pass.p_pass_data,
               p_data->rc_msg.msg.pass.pass_len);
      }
    } else {
      /* must be a bad ctype -> reject*/
      p_data->rc_msg.msg.hdr.ctype = AVRC_RSP_REJ;
      AVRC_PassRsp(p_data->rc_msg.handle, p_data->rc_msg.label, &p_data->rc_msg.msg.pass);
    }
  } else if (p_data->rc_msg.opcode == AVRC_OP_VENDOR) {
    /* else if this is a vendor specific command or response */
    /* set up for callback */
    av.vendor_cmd.code = p_data->rc_msg.msg.hdr.ctype;
    av.vendor_cmd.company_id = p_vendor->company_id;
    av.vendor_cmd.label = p_data->rc_msg.label;
    av.vendor_cmd.p_data = p_vendor->p_vendor_data;
    av.vendor_cmd.len = p_vendor->vendor_len;

    /* if configured to support vendor specific and it's a command */
    if ((p_cb->features & BTA_AV_FEAT_VENDOR) && p_data->rc_msg.msg.hdr.ctype <= AVRC_CMD_GEN_INQ) {
      if ((p_cb->features & BTA_AV_FEAT_METADATA) && (p_vendor->company_id == AVRC_CO_METADATA)) {
        av.meta_msg.p_msg = &p_data->rc_msg.msg;
        rc_rsp.rsp.status = BTA_AV_STS_NO_RSP;
        evt = bta_av_proc_meta_cmd(&rc_rsp, &p_data->rc_msg, &ctype);
      } else {
        evt = BTA_AV_VENDOR_CMD_EVT;
      }
    } else if ((p_cb->features & BTA_AV_FEAT_VENDOR) &&
               p_data->rc_msg.msg.hdr.ctype >= AVRC_RSP_NOT_IMPL) {
      /* else if configured to support vendor specific and it's a response */
      if ((p_cb->features & BTA_AV_FEAT_METADATA) && (p_vendor->company_id == AVRC_CO_METADATA)) {
        av.meta_msg.p_msg = &p_data->rc_msg.msg;
        evt = BTA_AV_META_MSG_EVT;
      } else {
        evt = BTA_AV_VENDOR_RSP_EVT;
      }
    } else if (!(p_cb->features & BTA_AV_FEAT_VENDOR) &&
               p_data->rc_msg.msg.hdr.ctype <= AVRC_CMD_GEN_INQ) {
      /* else if not configured to support vendor specific and it's a command */
      if (p_data->rc_msg.msg.vendor.p_vendor_data[0] == AVRC_PDU_INVALID) {
        /* reject it */
        p_data->rc_msg.msg.hdr.ctype = AVRC_RSP_REJ;
        p_data->rc_msg.msg.vendor.p_vendor_data[4] = AVRC_STS_BAD_CMD;
      } else {
        p_data->rc_msg.msg.hdr.ctype = AVRC_RSP_NOT_IMPL;
      }
      AVRC_VendorRsp(p_data->rc_msg.handle, p_data->rc_msg.label, &p_data->rc_msg.msg.vendor);
    }
  } else if (p_data->rc_msg.opcode == AVRC_OP_BROWSE) {
    /* set up for callback */
    av.meta_msg.rc_handle = p_data->rc_msg.handle;
    av.meta_msg.company_id = p_vendor->company_id;
    av.meta_msg.code = p_data->rc_msg.msg.hdr.ctype;
    av.meta_msg.label = p_data->rc_msg.label;
    av.meta_msg.p_msg = &p_data->rc_msg.msg;
    av.meta_msg.p_data = p_data->rc_msg.msg.browse.p_browse_data;
    av.meta_msg.len = p_data->rc_msg.msg.browse.browse_len;
    evt = BTA_AV_META_MSG_EVT;
  }

  if (evt == 0 && rc_rsp.rsp.status != BTA_AV_STS_NO_RSP) {
    if (!p_pkt) {
      rc_rsp.rsp.opcode = p_data->rc_msg.opcode;
      AVRC_BldResponse(0, &rc_rsp, &p_pkt);
    }
    if (p_pkt) {
      AVRC_MsgReq(p_data->rc_msg.handle, p_data->rc_msg.label, ctype, p_pkt, false);
    }
  }

  /* call callback */
  if (evt != 0) {
    av.remote_cmd.rc_handle = p_data->rc_msg.handle;
    (*p_cb->p_cback)(evt, &av);
    /* If browsing message, then free the browse message buffer */
    if (p_data->rc_msg.opcode == AVRC_OP_BROWSE && p_data->rc_msg.msg.browse.p_browse_pkt != NULL) {
      bta_av_rc_free_browse_msg(p_cb, p_data);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_close
 *
 * Description      close the specified AVRC handle.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_close(tBTA_AV_CB* p_cb, tBTA_AV_DATA* p_data) {
  uint16_t handle = p_data->hdr.layer_specific;
  tBTA_AV_SCB* p_scb;
  tBTA_AV_RCB* p_rcb;

  if (handle < BTA_AV_NUM_RCB) {
    p_rcb = &p_cb->rcb[handle];

    log::verbose("handle: {}, status=0x{:x}", p_rcb->handle, p_rcb->status);
    if (p_rcb->handle != BTA_AV_RC_HANDLE_NONE) {
      if (p_rcb->shdl) {
        p_scb = bta_av_cb.p_scb[p_rcb->shdl - 1];
        if (p_scb) {
          /* just in case the RC timer is active
          if (bta_av_cb.features & BTA_AV_FEAT_RCCT &&
             p_scb->chnl == BTA_AV_CHNL_AUDIO) */
          alarm_cancel(p_scb->avrc_ct_timer);
        }
      }

      AVRC_Close(p_rcb->handle);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_get_shdl
 *
 * Returns          The index to p_scb[]
 *
 ******************************************************************************/
static uint8_t bta_av_get_shdl(tBTA_AV_SCB* p_scb) {
  int i;
  uint8_t shdl = 0;
  /* find the SCB & stop the timer */
  for (i = 0; i < BTA_AV_NUM_STRS; i++) {
    if (p_scb == bta_av_cb.p_scb[i]) {
      shdl = i + 1;
      break;
    }
  }
  return shdl;
}

/*******************************************************************************
 *
 * Function         bta_av_stream_chg
 *
 * Description      audio streaming status changed.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_stream_chg(tBTA_AV_SCB* p_scb, bool started) {
  uint8_t started_msk = BTA_AV_HNDL_TO_MSK(p_scb->hdi);

  log::verbose("peer {} started:{} started_msk:0x{:x}", p_scb->PeerAddress(), started, started_msk);

  if (started) {
    /* Let L2CAP know this channel is processed with high priority */
    if (!stack::l2cap::get_interface().L2CA_SetAclPriority(p_scb->PeerAddress(),
                                                           L2CAP_PRIORITY_HIGH)) {
      log::warn("Unable to set L2CAP acl high priority peer:{}", p_scb->PeerAddress());
    }
  } else {
    /* Let L2CAP know this channel is processed with low priority */
    if (!stack::l2cap::get_interface().L2CA_SetAclPriority(p_scb->PeerAddress(),
                                                           L2CAP_PRIORITY_NORMAL)) {
      log::warn("Unable to set L2CAP acl normal priority peer:{}", p_scb->PeerAddress());
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_conn_chg
 *
 * Description      connetion status changed.
 *                  Open an AVRCP acceptor channel, if new conn.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_conn_chg(tBTA_AV_DATA* p_data) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_SCB* p_scb = NULL;
  tBTA_AV_SCB* p_scbi;
  uint8_t mask;
  uint8_t conn_msk;
  uint8_t old_msk;
  int i;
  int index = (p_data->hdr.layer_specific & BTA_AV_HNDL_MSK) - 1;
  tBTA_AV_LCB* p_lcb;
  tBTA_AV_LCB* p_lcb_rc;
  tBTA_AV_RCB *p_rcb, *p_rcb2;
  bool chk_restore = false;

  /* Validate array index*/
  if (index < BTA_AV_NUM_STRS) {
    p_scb = p_cb->p_scb[index];
  }
  mask = BTA_AV_HNDL_TO_MSK(index);
  p_lcb = bta_av_find_lcb(p_data->conn_chg.peer_addr, BTA_AV_LCB_FIND);
  conn_msk = 1 << (index + 1);
  if (p_data->conn_chg.is_up) {
    /* set the conned mask for this channel */
    if (p_scb) {
      if (p_lcb) {
        p_lcb->conn_msk |= conn_msk;
        for (i = 0; i < BTA_AV_NUM_RCB; i++) {
          if (bta_av_cb.rcb[i].lidx == p_lcb->lidx) {
            bta_av_cb.rcb[i].shdl = index + 1;
            log::verbose("conn_chg up[{}]: {}, status=0x{:x}, shdl:{}, lidx:{}", i,
                         bta_av_cb.rcb[i].handle, bta_av_cb.rcb[i].status, bta_av_cb.rcb[i].shdl,
                         bta_av_cb.rcb[i].lidx);
            break;
          }
        }
      }
      old_msk = p_cb->conn_audio;
      p_cb->conn_audio |= mask;

      if ((old_msk & mask) == 0) {
        /* increase the audio open count, if not set yet */
        bta_av_cb.audio_open_cnt++;
      }

      log::verbose("rc_acp_handle:{} rc_acp_idx:{}", p_cb->rc_acp_handle, p_cb->rc_acp_idx);
      /* check if the AVRCP ACP channel is already connected */
      if (p_lcb && p_cb->rc_acp_handle != BTA_AV_RC_HANDLE_NONE && p_cb->rc_acp_idx) {
        p_lcb_rc = &p_cb->lcb[BTA_AV_NUM_LINKS];
        log::verbose("rc_acp is connected && conn_chg on same addr p_lcb_rc->conn_msk:x{:x}",
                     p_lcb_rc->conn_msk);
        /* check if the RC is connected to the scb addr */
        log::info("p_lcb_rc->addr: {} conn_chg.peer_addr: {}", p_lcb_rc->addr,
                  p_data->conn_chg.peer_addr);

        if (p_lcb_rc->conn_msk && p_lcb_rc->addr == p_data->conn_chg.peer_addr) {
          /* AVRCP is already connected.
           * need to update the association between SCB and RCB */
          p_lcb_rc->conn_msk = 0; /* indicate RC ONLY is not connected */
          p_lcb_rc->lidx = 0;
          p_scb->rc_handle = p_cb->rc_acp_handle;
          p_rcb = &p_cb->rcb[p_cb->rc_acp_idx - 1];
          p_rcb->shdl = bta_av_get_shdl(p_scb);
          log::verbose("update rc_acp shdl:{}/{} srch:{}", index + 1, p_rcb->shdl,
                       p_scb->rc_handle);

          p_rcb2 = bta_av_get_rcb_by_shdl(p_rcb->shdl);
          if (p_rcb2) {
            /* found the RCB that was created to associated with this SCB */
            p_cb->rc_acp_handle = p_rcb2->handle;
            p_cb->rc_acp_idx = (p_rcb2 - p_cb->rcb) + 1;
            log::verbose("new rc_acp_handle:{}, idx:{}", p_cb->rc_acp_handle, p_cb->rc_acp_idx);
            p_rcb2->lidx = (BTA_AV_NUM_LINKS + 1);
            log::verbose("rc2 handle:{} lidx:{}/{}", p_rcb2->handle, p_rcb2->lidx,
                         p_cb->lcb[p_rcb2->lidx - 1].lidx);
          }
          p_rcb->lidx = p_lcb->lidx;
          log::verbose("rc handle:{} lidx:{}/{}", p_rcb->handle, p_rcb->lidx,
                       p_cb->lcb[p_rcb->lidx - 1].lidx);
        }
      }
    }
  } else {
    if ((p_cb->conn_audio & mask) && bta_av_cb.audio_open_cnt) {
      /* this channel is still marked as open. decrease the count */
      bta_av_cb.audio_open_cnt--;
    }

    /* clear the conned mask for this channel */
    p_cb->conn_audio &= ~mask;
    if (p_scb) {
      // The stream is closed. Clear the state.
      p_scb->OnDisconnected();
      if (p_scb->chnl == BTA_AV_CHNL_AUDIO) {
        if (p_lcb) {
          p_lcb->conn_msk &= ~conn_msk;
        }
        /* audio channel is down. make sure the INT channel is down */
        /* just in case the RC timer is active
        if (p_cb->features & BTA_AV_FEAT_RCCT) */
        { alarm_cancel(p_scb->avrc_ct_timer); }
        /* one audio channel goes down. check if we need to restore high
         * priority */
        chk_restore = true;
      }
    }

    log::verbose("shdl:{}", index + 1);
    for (i = 0; i < BTA_AV_NUM_RCB; i++) {
      log::verbose("conn_chg dn[{}]: {}, status=0x{:x}, shdl:{}, lidx:{}", i,
                   bta_av_cb.rcb[i].handle, bta_av_cb.rcb[i].status, bta_av_cb.rcb[i].shdl,
                   bta_av_cb.rcb[i].lidx);
      if (bta_av_cb.rcb[i].shdl == index + 1) {
        bta_av_del_rc(&bta_av_cb.rcb[i]);
        /* since the connection is already down and info was removed, clean
         * reference */
        bta_av_cb.rcb[i].shdl = 0;
        break;
      }
    }

    if (p_cb->conn_audio == 0) {
      /* if both channels are not connected,
       * close all RC channels */
      bta_av_close_all_rc(p_cb);
    }

    /* if the AVRCP is no longer listening, create the listening channel */
    if (bta_av_cb.rc_acp_handle == BTA_AV_RC_HANDLE_NONE && bta_av_cb.features & BTA_AV_FEAT_RCTG) {
      bta_av_rc_create(&bta_av_cb, AVCT_ROLE_ACCEPTOR, 0, BTA_AV_NUM_LINKS + 1);
    }
  }

  log::verbose("audio:{:x} up:{} conn_msk:0x{:x} chk_restore:{} audio_open_cnt:{}",
               p_cb->conn_audio, p_data->conn_chg.is_up, conn_msk, chk_restore,
               p_cb->audio_open_cnt);

  if (chk_restore) {
    if (p_cb->audio_open_cnt == 1) {
      /* one audio channel goes down and there's one audio channel remains open.
       * restore the switch role in default link policy */
      get_btm_client_interface().link_policy.BTM_default_unblock_role_switch();
      bta_av_restore_switch();
    }
    if (p_cb->audio_open_cnt) {
      /* adjust flush timeout settings to longer period */
      for (i = 0; i < BTA_AV_NUM_STRS; i++) {
        p_scbi = bta_av_cb.p_scb[i];
        if (p_scbi && p_scbi->chnl == BTA_AV_CHNL_AUDIO && p_scbi->co_started) {
          /* may need to update the flush timeout of this already started stream
           */
          if (p_scbi->co_started != bta_av_cb.audio_open_cnt) {
            p_scbi->co_started = bta_av_cb.audio_open_cnt;
          }
        }
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_disable
 *
 * Description      disable AV.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_disable(tBTA_AV_CB* p_cb, tBTA_AV_DATA* /* p_data */) {
  BT_HDR_RIGID hdr;
  bool disabling_in_progress = false;
  uint16_t xx;

  p_cb->disabling = true;

  bta_av_close_all_rc(p_cb);

  osi_free_and_reset((void**)&p_cb->p_disc_db);

  /* disable audio/video - de-register all channels,
   * expect BTA_AV_DEREG_COMP_EVT when deregister is complete */
  for (xx = 0; xx < BTA_AV_NUM_STRS; xx++) {
    if (p_cb->p_scb[xx] != NULL) {
      // Free signalling timers
      alarm_free(p_cb->p_scb[xx]->link_signalling_timer);
      p_cb->p_scb[xx]->link_signalling_timer = NULL;
      alarm_free(p_cb->p_scb[xx]->accept_signalling_timer);
      p_cb->p_scb[xx]->accept_signalling_timer = NULL;

      hdr.layer_specific = xx + 1;
      bta_av_api_deregister((tBTA_AV_DATA*)&hdr);
      disabling_in_progress = true;
    }
  }
  // Since All channels are deregistering by API_DEREGISTER, the DEREG_COMP_EVT
  // would come first before API_DISABLE if there is no connections, and it is
  // no needed to setup this disabling flag.
  p_cb->disabling = disabling_in_progress;
}

/*******************************************************************************
 *
 * Function         bta_av_api_disconnect
 *
 * Description      .
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_api_disconnect(tBTA_AV_DATA* p_data) {
  tBTA_AV_SCB* p_scb = bta_av_hndl_to_scb(p_data->api_discnt.hdr.layer_specific);
  AVDT_DisconnectReq(p_scb->PeerAddress(), bta_av_conn_cback);
  alarm_cancel(p_scb->link_signalling_timer);
}

/*******************************************************************************
 *
 * Function         bta_av_set_use_latency_mode
 *
 * Description      Sets stream use latency mode.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_set_use_latency_mode(tBTA_AV_SCB* p_scb, bool use_latency_mode) {
  if (!stack::l2cap::get_interface().L2CA_UseLatencyMode(p_scb->PeerAddress(), use_latency_mode)) {
    log::warn("Unable to set L2CAP latenty mode peer:{} use_latency_mode:{}", p_scb->PeerAddress(),
              use_latency_mode);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_api_set_latency
 *
 * Description      set stream latency.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_api_set_latency(tBTA_AV_DATA* p_data) {
  tBTA_AV_SCB* p_scb = bta_av_hndl_to_scb(p_data->api_set_latency.hdr.layer_specific);

  tL2CAP_LATENCY latency =
          p_data->api_set_latency.is_low_latency ? L2CAP_LATENCY_LOW : L2CAP_LATENCY_NORMAL;
  if (!stack::l2cap::get_interface().L2CA_SetAclLatency(p_scb->PeerAddress(), latency)) {
    log::warn("Unable to set L2CAP latenty mode peer:{} use_latency_mode:{}", p_scb->PeerAddress(),
              latency);
  }
}

/**
 * Find the index for the free LCB entry to use.
 *
 * The selection order is:
 * (1) Find the index if there is already SCB entry for the peer address
 * (2) If there is no SCB entry for the peer address, find the first
 * SCB entry that is not assigned.
 *
 * @param peer_address the peer address to use
 * @return the index for the free LCB entry to use or BTA_AV_NUM_LINKS
 * if no entry is found
 */
static uint8_t bta_av_find_lcb_index_by_scb_and_address(const RawAddress& peer_address) {
  log::verbose("peer_address: {} conn_lcb: 0x{:x}", peer_address, bta_av_cb.conn_lcb);

  // Find the index if there is already SCB entry for the peer address
  for (uint8_t index = 0; index < BTA_AV_NUM_LINKS; index++) {
    uint8_t mask = 1 << index;
    if (mask & bta_av_cb.conn_lcb) {
      continue;
    }
    tBTA_AV_SCB* p_scb = bta_av_cb.p_scb[index];
    if (p_scb == nullptr) {
      continue;
    }
    if (p_scb->PeerAddress() == peer_address) {
      return index;
    }
  }

  // Find the first SCB entry that is not assigned.
  for (uint8_t index = 0; index < BTA_AV_NUM_LINKS; index++) {
    uint8_t mask = 1 << index;
    if (mask & bta_av_cb.conn_lcb) {
      continue;
    }
    tBTA_AV_SCB* p_scb = bta_av_cb.p_scb[index];
    if (p_scb == nullptr) {
      continue;
    }
    if (!p_scb->IsAssigned()) {
      const RawAddress& btif_addr = btif_av_find_by_handle(p_scb->hndl);
      if (!btif_addr.IsEmpty() && btif_addr != peer_address) {
        log::debug("btif_addr = {}, index={}!", btif_addr, index);
        continue;
      }
      return index;
    }
  }

  return BTA_AV_NUM_LINKS;
}

/*******************************************************************************
 *
 * Function         bta_av_sig_chg
 *
 * Description      process AVDT signal channel up/down.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_sig_chg(tBTA_AV_DATA* p_data) {
  uint16_t event = p_data->str_msg.hdr.layer_specific;
  tBTA_AV_CB* p_cb = &bta_av_cb;
  uint32_t xx;
  uint8_t mask;
  tBTA_AV_LCB* p_lcb = NULL;

  log::verbose("event: {}", event);
  if (event == AVDT_CONNECT_IND_EVT) {
    log::verbose("AVDT_CONNECT_IND_EVT: peer {}", p_data->str_msg.bd_addr);

    p_lcb = bta_av_find_lcb(p_data->str_msg.bd_addr, BTA_AV_LCB_FIND);
    if (!p_lcb) {
      /* if the address does not have an LCB yet, alloc one */
      xx = bta_av_find_lcb_index_by_scb_and_address(p_data->str_msg.bd_addr);

      /* check if we found something */
      if (xx >= BTA_AV_NUM_LINKS) {
        /* We do not have scb for this avdt connection.     */
        /* Silently close the connection.                   */
        log::error("av scb not available for avdt connection for {}", p_data->str_msg.bd_addr);
        AVDT_DisconnectReq(p_data->str_msg.bd_addr, NULL);
        return;
      }
      log::info("AVDT_CONNECT_IND_EVT: peer {} selected lcb_index {}", p_data->str_msg.bd_addr, xx);

      tBTA_AV_SCB* p_scb = p_cb->p_scb[xx];
      mask = 1 << xx;
      p_lcb = &p_cb->lcb[xx];
      p_lcb->lidx = xx + 1;
      p_lcb->addr = p_data->str_msg.bd_addr;
      p_lcb->conn_msk = 0; /* clear the connect mask */
      /* start listening when the signal channel is open */
      if (p_cb->features & BTA_AV_FEAT_RCTG) {
        bta_av_rc_create(p_cb, AVCT_ROLE_ACCEPTOR, 0, p_lcb->lidx);
      }
      /* this entry is not used yet. */
      p_cb->conn_lcb |= mask; /* mark it as used */
      log::verbose("start sig timer {}", p_data->hdr.offset);
      if (p_data->hdr.offset == static_cast<uint16_t>(tAVDT_ROLE::AVDT_ACP)) {
        log::verbose("Incoming L2CAP acquired, set state as incoming");
        p_scb->OnConnected(p_data->str_msg.bd_addr);
        p_scb->use_rc = true; /* allowing RC for incoming connection */
        bta_av_ssm_execute(p_scb, BTA_AV_ACP_CONNECT_EVT, p_data);

        /* The Pending Event should be sent as soon as the L2CAP signalling
         * channel
         * is set up, which is NOW. Earlier this was done only after
         * BTA_AV_SIGNALLING_TIMEOUT_MS.
         * The following function shall send the event and start the
         * recurring timer
         */
        if (!p_scb->link_signalling_timer) {
          p_scb->link_signalling_timer = alarm_new("link_signalling_timer");
        }
        BT_HDR hdr;
        hdr.layer_specific = p_scb->hndl;
        bta_av_signalling_timer((tBTA_AV_DATA*)&hdr);

        log::verbose("Re-start timer for AVDTP service");
        bta_sys_conn_open(BTA_ID_AV, p_scb->app_id, p_scb->PeerAddress());
        /* Possible collision : need to avoid outgoing processing while the
         * timer is running */
        p_scb->coll_mask = BTA_AV_COLL_INC_TMR;
        if (!p_scb->accept_signalling_timer) {
          p_scb->accept_signalling_timer = alarm_new("accept_signalling_timer");
        }
        alarm_set_on_mloop(p_scb->accept_signalling_timer, BTA_AV_ACCEPT_SIGNALLING_TIMEOUT_MS,
                           bta_av_accept_signalling_timer_cback, UINT_TO_PTR(xx));
      }
    }
  } else if (event == BTA_AR_AVDT_CONN_EVT) {
    uint8_t scb_index = p_data->str_msg.scb_index;
    alarm_cancel(p_cb->p_scb[scb_index]->link_signalling_timer);
  } else {
    /* disconnected. */
    log::verbose("bta_av_cb.conn_lcb=0x{:x}", bta_av_cb.conn_lcb);

    p_lcb = bta_av_find_lcb(p_data->str_msg.bd_addr, BTA_AV_LCB_FREE);
    if (p_lcb && (p_lcb->conn_msk || bta_av_cb.conn_lcb)) {
      log::verbose("conn_msk: 0x{:x}", p_lcb->conn_msk);
      /* clean up ssm  */
      for (xx = 0; xx < BTA_AV_NUM_STRS; xx++) {
        if (p_cb->p_scb[xx] && p_cb->p_scb[xx]->PeerAddress() == p_data->str_msg.bd_addr) {
          if ((p_cb->p_scb[xx]->state == 1) &&
              alarm_is_scheduled(p_cb->p_scb[xx]->accept_signalling_timer) &&
              interop_match_addr(INTEROP_IGNORE_DISC_BEFORE_SIGNALLING_TIMEOUT,
                                 &(p_data->str_msg.bd_addr))) {
            continue;
          }
          log::verbose("Closing timer for AVDTP service");
          bta_sys_conn_close(BTA_ID_AV, p_cb->p_scb[xx]->app_id, p_cb->p_scb[xx]->PeerAddress());
        }
        mask = 1 << (xx + 1);
        if (((mask & p_lcb->conn_msk) || bta_av_cb.conn_lcb) && p_cb->p_scb[xx] &&
            p_cb->p_scb[xx]->PeerAddress() == p_data->str_msg.bd_addr) {
          log::warn("Sending AVDT_DISCONNECT_EVT peer_addr={}", p_cb->p_scb[xx]->PeerAddress());
          bta_av_ssm_execute(p_cb->p_scb[xx], BTA_AV_AVDT_DISCONNECT_EVT, NULL);
        }
      }
    }
  }
  log::verbose("bta_av_cb.conn_lcb=0x{:x} after sig_chg", p_cb->conn_lcb);
}

/*******************************************************************************
 *
 * Function         bta_av_signalling_timer
 *
 * Description      process the signal channel timer. This timer is started
 *                  when the AVDTP signal channel is connected. If no profile
 *                  is connected, the timer goes off every
 *                  BTA_AV_SIGNALLING_TIMEOUT_MS.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_signalling_timer(tBTA_AV_DATA* p_data) {
  tBTA_AV_HNDL hndl = p_data->hdr.layer_specific;
  tBTA_AV_SCB* p_scb = bta_av_hndl_to_scb(hndl);

  tBTA_AV_CB* p_cb = &bta_av_cb;
  int xx;
  uint8_t mask;
  tBTA_AV_LCB* p_lcb = NULL;

  log::verbose("conn_lcb=0x{:x}", p_cb->conn_lcb);
  for (xx = 0; xx < BTA_AV_NUM_LINKS; xx++) {
    p_lcb = &p_cb->lcb[xx];
    mask = 1 << xx;
    log::verbose("index={} conn_lcb=0x{:x} peer={} conn_mask=0x{:x} lidx={}", xx, p_cb->conn_lcb,
                 p_lcb->addr, p_lcb->conn_msk, p_lcb->lidx);
    if (mask & p_cb->conn_lcb) {
      /* this entry is used. check if it is connected */
      if (!p_lcb->conn_msk) {
        log::verbose("hndl 0x{:x}", p_scb->hndl);
        bta_sys_start_timer(p_scb->link_signalling_timer, BTA_AV_SIGNALLING_TIMEOUT_MS,
                            BTA_AV_SIGNALLING_TIMER_EVT, hndl);
        tBTA_AV bta_av_data = {
                .pend =
                        {
                                .bd_addr = p_lcb->addr,
                        },
        };
        log::verbose("BTA_AV_PENDING_EVT for {} index={} conn_mask=0x{:x} lidx={}", p_lcb->addr, xx,
                     p_lcb->conn_msk, p_lcb->lidx);
        (*p_cb->p_cback)(BTA_AV_PENDING_EVT, &bta_av_data);
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_accept_signalling_timer_cback
 *
 * Description      Process the timeout when SRC is accepting connection
 *                  and SNK did not start signalling.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_accept_signalling_timer_cback(void* data) {
  uint32_t inx = PTR_TO_UINT(data);
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_SCB* p_scb = NULL;
  if (inx < BTA_AV_NUM_STRS) {
    p_scb = p_cb->p_scb[inx];
  }
  if (p_scb) {
    log::verbose("coll_mask=0x{:02x}", p_scb->coll_mask);

    if (p_scb->coll_mask & BTA_AV_COLL_INC_TMR) {
      p_scb->coll_mask &= ~BTA_AV_COLL_INC_TMR;

      if (bta_av_is_scb_opening(p_scb)) {
        log::verbose("stream state opening: SDP started = {}", p_scb->sdp_discovery_started);
        if (p_scb->sdp_discovery_started) {
          /* We are still doing SDP. Run the timer again. */
          p_scb->coll_mask |= BTA_AV_COLL_INC_TMR;

          alarm_set_on_mloop(p_scb->accept_signalling_timer, BTA_AV_ACCEPT_SIGNALLING_TIMEOUT_MS,
                             bta_av_accept_signalling_timer_cback, UINT_TO_PTR(inx));
        } else {
          /* SNK did not start signalling, resume signalling process. */
          bta_av_discover_req(p_scb, NULL);
        }
      } else if (bta_av_is_scb_incoming(p_scb)) {
        /* Stay in incoming state if SNK does not start signalling */

        log::verbose("stream state incoming");
        /* API open was called right after SNK opened L2C connection. */
        if (p_scb->coll_mask & BTA_AV_COLL_API_CALLED) {
          p_scb->coll_mask &= ~BTA_AV_COLL_API_CALLED;

          /* BTA_AV_API_OPEN_EVT */
          tBTA_AV_API_OPEN* p_buf = (tBTA_AV_API_OPEN*)osi_malloc(sizeof(tBTA_AV_API_OPEN));
          memcpy(p_buf, &(p_scb->open_api), sizeof(tBTA_AV_API_OPEN));
          bta_sys_sendmsg(p_buf);
        }
      }
    }
  }
}

static void bta_av_store_peer_rc_version() {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tSDP_DISC_REC* p_rec = NULL;
  uint16_t peer_rc_version = 0; /*Assuming Default peer version as 1.3*/

  if ((p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
               p_cb->p_disc_db, UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL)) != NULL) {
    if ((get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                p_rec, ATTR_ID_BT_PROFILE_DESC_LIST)) != NULL) {
      /* get profile version (if failure, version parameter is not updated) */
      if (!get_legacy_stack_sdp_api()->record.SDP_FindProfileVersionInRec(
                  p_rec, UUID_SERVCLASS_AV_REMOTE_CONTROL, &peer_rc_version)) {
        log::warn("Unable to find AVRC profile version in record peer:{}", p_rec->remote_bd_addr);
      }
    }
    if (peer_rc_version != 0) {
      DEVICE_IOT_CONFIG_ADDR_SET_HEX_IF_GREATER(p_rec->remote_bd_addr,
                                                IOT_CONF_KEY_AVRCP_CTRL_VERSION, peer_rc_version,
                                                IOT_CONF_BYTE_NUM_2);
    }
  }

  peer_rc_version = 0;
  if ((p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
               p_cb->p_disc_db, UUID_SERVCLASS_AV_REM_CTRL_TARGET, NULL)) != NULL) {
    if ((get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                p_rec, ATTR_ID_BT_PROFILE_DESC_LIST)) != NULL) {
      /* get profile version (if failure, version parameter is not updated) */
      if (!get_legacy_stack_sdp_api()->record.SDP_FindProfileVersionInRec(
                  p_rec, UUID_SERVCLASS_AV_REMOTE_CONTROL, &peer_rc_version)) {
        log::warn("Unable to find SDP profile version in record peer:{}", p_rec->remote_bd_addr);
      }
    }
    if (peer_rc_version != 0) {
      DEVICE_IOT_CONFIG_ADDR_SET_HEX_IF_GREATER(p_rec->remote_bd_addr,
                                                IOT_CONF_KEY_AVRCP_TG_VERSION, peer_rc_version,
                                                IOT_CONF_BYTE_NUM_2);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_check_peer_features
 *
 * Description      check supported features on the peer device from the SDP
 *                  record and return the feature mask
 *
 * Returns          tBTA_AV_FEAT peer device feature mask
 *
 ******************************************************************************/
static tBTA_AV_FEAT bta_av_check_peer_features(uint16_t service_uuid) {
  tBTA_AV_FEAT peer_features = 0;
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tSDP_DISC_REC* p_rec = NULL;
  tSDP_DISC_ATTR* p_attr;
  uint16_t peer_rc_version = 0;
  uint16_t categories = 0;

  log::verbose("service_uuid:x{:x}", service_uuid);
  /* loop through all records we found */
  while (true) {
    /* get next record; if none found, we're done */
    p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(p_cb->p_disc_db, service_uuid,
                                                               p_rec);
    if (p_rec == NULL) {
      break;
    }

    if ((get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                p_rec, ATTR_ID_SERVICE_CLASS_ID_LIST)) != NULL) {
      /* find peer features */
      if (get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
                  p_cb->p_disc_db, UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL)) {
        peer_features |= BTA_AV_FEAT_RCCT;
      }
      if (get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
                  p_cb->p_disc_db, UUID_SERVCLASS_AV_REM_CTRL_TARGET, NULL)) {
        peer_features |= BTA_AV_FEAT_RCTG;
      }
    }

    if ((get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                p_rec, ATTR_ID_BT_PROFILE_DESC_LIST)) != NULL) {
      /* get profile version (if failure, version parameter is not updated) */
      if (!get_legacy_stack_sdp_api()->record.SDP_FindProfileVersionInRec(
                  p_rec, UUID_SERVCLASS_AV_REMOTE_CONTROL, &peer_rc_version)) {
        log::warn("Unable to find AVRC profile version in record peer:{}", p_rec->remote_bd_addr);
      }
      log::verbose("peer_rc_version 0x{:x}", peer_rc_version);

      if (peer_rc_version >= AVRC_REV_1_3) {
        peer_features |= (BTA_AV_FEAT_VENDOR | BTA_AV_FEAT_METADATA);
      }

      if (peer_rc_version >= AVRC_REV_1_4) {
        /* get supported categories */
        p_attr = get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                p_rec, ATTR_ID_SUPPORTED_FEATURES);
        if (p_attr != NULL && SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UINT_DESC_TYPE &&
            SDP_DISC_ATTR_LEN(p_attr->attr_len_type) >= 2) {
          categories = p_attr->attr_value.v.u16;
          if (categories & AVRC_SUPF_CT_CAT2) {
            peer_features |= (BTA_AV_FEAT_ADV_CTRL);
          }
          if (categories & AVRC_SUPF_CT_BROWSE) {
            peer_features |= (BTA_AV_FEAT_BROWSE);
          }
        }
      }
    }
  }
  log::verbose("peer_features:x{:x}", peer_features);
  return peer_features;
}

/*******************************************************************************
 *
 * Function         bta_avk_check_peer_features
 *
 * Description      check supported features on the peer device from the SDP
 *                  record and return the feature mask
 *
 * Returns          tBTA_AV_FEAT peer device feature mask
 *
 ******************************************************************************/
static tBTA_AV_FEAT bta_avk_check_peer_features(uint16_t service_uuid) {
  tBTA_AV_FEAT peer_features = 0;
  tBTA_AV_CB* p_cb = &bta_av_cb;

  log::verbose("service_uuid:x{:x}", service_uuid);

  /* loop through all records we found */
  tSDP_DISC_REC* p_rec =
          get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(p_cb->p_disc_db, service_uuid, NULL);
  while (p_rec) {
    log::verbose("found Service record for x{:x}", service_uuid);

    if ((get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                p_rec, ATTR_ID_SERVICE_CLASS_ID_LIST)) != NULL) {
      /* find peer features */
      if (get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
                  p_cb->p_disc_db, UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL)) {
        peer_features |= BTA_AV_FEAT_RCCT;
      }
      if (get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
                  p_cb->p_disc_db, UUID_SERVCLASS_AV_REM_CTRL_TARGET, NULL)) {
        peer_features |= BTA_AV_FEAT_RCTG;
      }
    }

    if ((get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                p_rec, ATTR_ID_BT_PROFILE_DESC_LIST)) != NULL) {
      /* get profile version (if failure, version parameter is not updated) */
      uint16_t peer_rc_version = 0;
      bool val = get_legacy_stack_sdp_api()->record.SDP_FindProfileVersionInRec(
              p_rec, UUID_SERVCLASS_AV_REMOTE_CONTROL, &peer_rc_version);
      log::verbose("peer_rc_version for TG 0x{:x}, profile_found {}", peer_rc_version, val);

      if (peer_rc_version >= AVRC_REV_1_3) {
        peer_features |= (BTA_AV_FEAT_VENDOR | BTA_AV_FEAT_METADATA);
      }

      /* Get supported features */
      tSDP_DISC_ATTR* p_attr = get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
              p_rec, ATTR_ID_SUPPORTED_FEATURES);
      if (p_attr != NULL && SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == UINT_DESC_TYPE &&
          SDP_DISC_ATTR_LEN(p_attr->attr_len_type) >= 2) {
        uint16_t categories = p_attr->attr_value.v.u16;
        /*
         * Though Absolute Volume came after in 1.4 and above, but there are
         * few devices in market which supports absolute Volume and they are
         * still 1.3. To avoid IOP issuses with those devices, we check for
         * 1.3 as minimum version
         */
        if (peer_rc_version >= AVRC_REV_1_3) {
          if (categories & AVRC_SUPF_TG_CAT2) {
            peer_features |= (BTA_AV_FEAT_ADV_CTRL);
          }
          if (categories & AVRC_SUPF_TG_APP_SETTINGS) {
            peer_features |= (BTA_AV_FEAT_APP_SETTING);
          }
          if (categories & AVRC_SUPF_TG_BROWSE) {
            peer_features |= (BTA_AV_FEAT_BROWSE);
          }
        }

        /* AVRCP Cover Artwork over BIP */
        if (peer_rc_version >= AVRC_REV_1_6) {
          if (service_uuid == UUID_SERVCLASS_AV_REM_CTRL_TARGET &&
              categories & AVRC_SUPF_TG_PLAYER_COVER_ART) {
            peer_features |= (BTA_AV_FEAT_COVER_ARTWORK);
          }
        }
      }
    }
    /* get next record; if none found, we're done */
    p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(p_cb->p_disc_db, service_uuid,
                                                               p_rec);
  }
  log::verbose("peer_features:x{:x}", peer_features);
  return peer_features;
}

/******************************************************************************
 *
 * Function         bta_avk_get_cover_art_psm
 *
 * Description      Get the PSM associated with the AVRCP Target cover art
 *                  feature
 *
 * Returns          uint16_t PSM value used to get cover artwork, or 0x0000 if
 *                  one does not exist.
 *
 *****************************************************************************/
static uint16_t bta_avk_get_cover_art_psm() {
  log::verbose("searching for cover art psm");
  /* Cover Art L2CAP PSM is only available on a target device */
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tSDP_DISC_REC* p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
          p_cb->p_disc_db, UUID_SERVCLASS_AV_REM_CTRL_TARGET, NULL);
  while (p_rec) {
    tSDP_DISC_ATTR* p_attr = (get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
            p_rec, ATTR_ID_ADDITION_PROTO_DESC_LISTS));
    /*
     * If we have the Additional Protocol Description Lists attribute then we
     * specifically want the list that is an L2CAP protocol leading to OBEX.
     * Because the is a case where cover art is supported and browsing isn't
     * we need to check each list for the one we want.
     *
     * This means we need to do drop down into the protocol list and do a
     * "for each protocol, for each protocol element, for each protocol element
     * list parameter, if the parameter is L2CAP then find the PSM associated
     * with it, then make sure we see OBEX in that same protocol"
     */
    if (p_attr != NULL && SDP_DISC_ATTR_TYPE(p_attr->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE) {
      // Point to first in List of protocols (i.e [(L2CAP -> AVCTP),
      // (L2CAP -> OBEX)])
      tSDP_DISC_ATTR* p_protocol_list = p_attr->attr_value.v.p_sub_attr;
      while (p_protocol_list != NULL) {
        if (SDP_DISC_ATTR_TYPE(p_protocol_list->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE) {
          // Point to fist in list of protocol elements (i.e. [L2CAP, AVCTP])
          tSDP_DISC_ATTR* p_protocol = p_protocol_list->attr_value.v.p_sub_attr;
          bool protocol_has_obex = false;
          bool protocol_has_l2cap = false;
          uint16_t psm = 0x0000;
          while (p_protocol) {
            if (SDP_DISC_ATTR_TYPE(p_protocol->attr_len_type) == DATA_ELE_SEQ_DESC_TYPE) {
              // Point to first item protocol parameters list (i.e [UUID=L2CAP,
              // PSM=0x1234])
              tSDP_DISC_ATTR* p_protocol_param = p_protocol->attr_value.v.p_sub_attr;
              /*
               * Currently there's only ever one UUID and one parameter. L2cap
               * has a single PSM, AVCTP has a version and OBEX has nothing.
               * Change this if that ever changes.
               */
              uint16_t protocol_uuid = 0;
              uint16_t protocol_param = 0;
              while (p_protocol_param) {
                uint16_t param_type = SDP_DISC_ATTR_TYPE(p_protocol_param->attr_len_type);
                uint16_t param_len = SDP_DISC_ATTR_LEN(p_protocol_param->attr_len_type);
                if (param_type == UUID_DESC_TYPE) {
                  protocol_uuid = p_protocol_param->attr_value.v.u16;
                } else if (param_type == UINT_DESC_TYPE) {
                  protocol_param = (param_len == 2) ? p_protocol_param->attr_value.v.u16
                                                    : p_protocol_param->attr_value.v.u8;
                } /* else dont care */
                p_protocol_param = p_protocol_param->p_next_attr;  // next
              }
              // If we've found L2CAP then the parameter is a PSM
              if (protocol_uuid == UUID_PROTOCOL_L2CAP) {
                protocol_has_l2cap = true;
                psm = protocol_param;
              } else if (protocol_uuid == UUID_PROTOCOL_OBEX) {
                protocol_has_obex = true;
              }
            }
            // If this protocol has l2cap and obex then we're found the BIP PSM
            if (protocol_has_l2cap && protocol_has_obex) {
              log::verbose("found psm 0x{:x}", psm);
              return psm;
            }
            p_protocol = p_protocol->p_next_attr;  // next protocol element
          }
        }
        p_protocol_list = p_protocol_list->p_next_attr;  // next protocol
      }
    }
    /* get next record; if none found, we're done */
    p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
            p_cb->p_disc_db, UUID_SERVCLASS_AV_REM_CTRL_TARGET, p_rec);
  }
  /* L2CAP PSM range is 0x1000-0xFFFF so 0x0000 is safe default invalid */
  log::verbose("could not find a BIP psm");
  return 0x0000;
}

static void bta_av_rc_disc_done_all(tBTA_AV_DATA* /* p_data */) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_SCB* p_scb = NULL;
  tBTA_AV_LCB* p_lcb;
  uint8_t rc_handle = BTA_AV_RC_HANDLE_NONE;
  tBTA_AV_FEAT peer_tg_features = 0;
  tBTA_AV_FEAT peer_ct_features = 0;
  uint16_t cover_art_psm = 0x0000;

  log::verbose("bta_av_rc_disc_done disc:x{:x}", p_cb->disc);
  if (!p_cb->disc) {
    return;
  }

  if ((p_cb->disc & BTA_AV_CHNL_MSK) == BTA_AV_CHNL_MSK) {
    /* this is the rc handle/index to tBTA_AV_RCB */
    rc_handle = p_cb->disc & (~BTA_AV_CHNL_MSK);
    log::error("WRONG MASK A2dp not connect");
  } else {
    /* Validate array index*/
    if (((p_cb->disc & BTA_AV_HNDL_MSK) - 1) < BTA_AV_NUM_STRS) {
      log::verbose("wrong data bta_av_rc_disc_done disc:x{:x}", p_cb->disc);
      p_scb = p_cb->p_scb[(p_cb->disc & BTA_AV_HNDL_MSK) - 1];
    }
    if (p_scb) {
      rc_handle = p_scb->rc_handle;
    } else {
      p_cb->disc = 0;
      return;
    }
  }

  log::verbose("rc_handle {}", rc_handle);
  if (p_cb->sdp_a2dp_snk_handle) {
    /* This is Sink + CT + TG(Abs Vol) */
    peer_tg_features = bta_avk_check_peer_features(UUID_SERVCLASS_AV_REM_CTRL_TARGET);
    log::verbose("populating rem ctrl target features {}", peer_tg_features);
    if (BTA_AV_FEAT_ADV_CTRL & bta_avk_check_peer_features(UUID_SERVCLASS_AV_REMOTE_CONTROL)) {
      peer_tg_features |= (BTA_AV_FEAT_ADV_CTRL | BTA_AV_FEAT_RCCT);
    }

    if (peer_tg_features & BTA_AV_FEAT_COVER_ARTWORK) {
      cover_art_psm = bta_avk_get_cover_art_psm();
    }

    log::verbose("populating rem ctrl target bip psm 0x{:x}", cover_art_psm);
  } else if (p_cb->sdp_a2dp_handle) {
    /* check peer version and whether support CT and TG role */
    peer_ct_features = bta_av_check_peer_features(UUID_SERVCLASS_AV_REMOTE_CONTROL);
    if ((p_cb->features & BTA_AV_FEAT_ADV_CTRL) &&
        ((peer_ct_features & BTA_AV_FEAT_ADV_CTRL) == 0)) {
      /* if we support advance control and peer does not, check their support on
       * TG role
       * some implementation uses 1.3 on CT ans 1.4 on TG */
      peer_ct_features |= bta_av_check_peer_features(UUID_SERVCLASS_AV_REM_CTRL_TARGET);
    }

    /* Change our features if the remote AVRCP version is 1.3 or less */
    tSDP_DISC_REC* p_rec = nullptr;
    p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
            p_cb->p_disc_db, UUID_SERVCLASS_AV_REMOTE_CONTROL, p_rec);
    if (p_rec != NULL && get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                                 p_rec, ATTR_ID_BT_PROFILE_DESC_LIST) != NULL) {
      /* get profile version (if failure, version parameter is not updated) */
      uint16_t peer_rc_version = 0xFFFF;  // Don't change the AVRCP version
      if (!get_legacy_stack_sdp_api()->record.SDP_FindProfileVersionInRec(
                  p_rec, UUID_SERVCLASS_AV_REMOTE_CONTROL, &peer_rc_version)) {
        log::warn("Unable to find SDP in record peer:{}", p_rec->remote_bd_addr);
      }
      if (peer_rc_version <= AVRC_REV_1_3) {
        log::verbose("Using AVRCP 1.3 Capabilities with remote device");
        p_bta_av_cfg = &bta_av_cfg_compatibility;
      }
    }
  }

  p_cb->disc = 0;
  osi_free_and_reset((void**)&p_cb->p_disc_db);
  p_cb->rc_feature.peer_ct_features = peer_ct_features;
  p_cb->rc_feature.peer_tg_features = peer_tg_features;
  p_cb->rc_feature.rc_handle = rc_handle;
  if (p_scb) {
    p_cb->rc_feature.peer_addr = p_scb->PeerAddress();
  }

  log::verbose("peer_tg_features 0x{:x}, peer_ct_features 0x{:x}, features 0x{:x}",
               peer_tg_features, peer_ct_features, p_cb->features);

  /* if we have no rc connection */
  if (rc_handle == BTA_AV_RC_HANDLE_NONE) {
    if (p_scb) {
      /* if peer remote control service matches ours and USE_RC is true */
      if (((p_cb->features & BTA_AV_FEAT_RCCT) && (peer_tg_features & BTA_AV_FEAT_RCTG)) ||
          ((p_cb->features & BTA_AV_FEAT_RCTG) && (peer_ct_features & BTA_AV_FEAT_RCCT))) {
        p_lcb = bta_av_find_lcb(p_scb->PeerAddress(), BTA_AV_LCB_FIND);
        if (p_lcb) {
          rc_handle = bta_av_rc_create(p_cb, AVCT_ROLE_INITIATOR, (uint8_t)(p_scb->hdi + 1),
                                       p_lcb->lidx);
          if (rc_handle != BTA_AV_RC_HANDLE_NONE) {
            p_cb->rcb[rc_handle].peer_ct_features = peer_ct_features;
            p_cb->rcb[rc_handle].peer_tg_features = peer_tg_features;
            p_cb->rcb[rc_handle].peer_features = 0;
            p_cb->rcb[rc_handle].cover_art_psm = cover_art_psm;
          } else {
            /* cannot create valid rc_handle for current device. report failure
             */
            log::error("no link resources available");
            p_scb->use_rc = false;
            tBTA_AV bta_av_data = {
                    .rc_open =
                            {
                                    .peer_addr = p_scb->PeerAddress(),
                                    .status = BTA_AV_FAIL_RESOURCES,
                            },
            };
            (*p_cb->p_cback)(BTA_AV_RC_OPEN_EVT, &bta_av_data);
          }
        } else {
          log::error("can not find LCB!!");
        }
      } else if (p_scb->use_rc) {
        /* can not find AVRC on peer device. report failure */
        p_scb->use_rc = false;
        tBTA_AV bta_av_data = {
                .rc_open =
                        {
                                .peer_ct_features = peer_ct_features,
                                .peer_tg_features = peer_tg_features,
                                .peer_addr = p_scb->PeerAddress(),
                                .status = BTA_AV_FAIL_SDP,
                        },
        };
        (*p_cb->p_cback)(BTA_AV_RC_OPEN_EVT, &bta_av_data);
      }
    }
  } else {
    p_cb->rcb[rc_handle].peer_ct_features = peer_ct_features;
    p_cb->rcb[rc_handle].peer_tg_features = peer_tg_features;
    p_cb->rcb[rc_handle].peer_features = 0;

    RawAddress peer_addr = RawAddress::kEmpty;
    if (p_scb == NULL) {
      /*
       * In case scb is not created by the time we are done with SDP
       * we still need to send RC feature event. So we need to get BD
       * from Message.  Note that lidx is 1 based not 0 based
       */
      if (p_cb->rcb[rc_handle].lidx > 0) {
        peer_addr = p_cb->lcb[p_cb->rcb[rc_handle].lidx - 1].addr;
      } else {
        peer_addr = p_cb->lcb[p_cb->rcb[rc_handle].lidx].addr;
      }
    } else {
      peer_addr = p_scb->PeerAddress();
    }

    tBTA_AV bta_av_feat = {.rc_feat = {
                                   .rc_handle = rc_handle,
                                   .peer_ct_features = peer_ct_features,
                                   .peer_tg_features = peer_tg_features,
                                   .peer_addr = peer_addr,
                           }};
    (*p_cb->p_cback)(BTA_AV_RC_FEAT_EVT, &bta_av_feat);

    // Send PSM data
    log::verbose("Send PSM data. rc_psm = {:#x}", cover_art_psm);
    p_cb->rcb[rc_handle].cover_art_psm = cover_art_psm;
    tBTA_AV bta_av_psm = {
            .rc_cover_art_psm =
                    {
                            .rc_handle = rc_handle,
                            .cover_art_psm = cover_art_psm,
                            .peer_addr = peer_addr,
                    },
    };
    (*p_cb->p_cback)(BTA_AV_RC_PSM_EVT, &bta_av_psm);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_disc_done
 *
 * Description      Handle AVRCP service discovery results.  If matching
 *                  service found, open AVRCP connection.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_disc_done(tBTA_AV_DATA* p_data) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_SCB* p_scb = NULL;
  tBTA_AV_LCB* p_lcb;
  uint8_t rc_handle;
  tBTA_AV_FEAT peer_features = 0; /* peer features mask */
  uint16_t cover_art_psm = 0x0000;

  if (btif_av_both_enable()) {
    bta_av_rc_disc_done_all(p_data);
    return;
  }

  log::verbose("bta_av_rc_disc_done disc:x{:x}", p_cb->disc);
  if (!p_cb->disc) {
    return;
  }

  if ((p_cb->disc & BTA_AV_CHNL_MSK) == BTA_AV_CHNL_MSK) {
    /* this is the rc handle/index to tBTA_AV_RCB */
    rc_handle = p_cb->disc & (~BTA_AV_CHNL_MSK);
  } else {
    /* Validate array index*/
    if (((p_cb->disc & BTA_AV_HNDL_MSK) - 1) < BTA_AV_NUM_STRS) {
      p_scb = p_cb->p_scb[(p_cb->disc & BTA_AV_HNDL_MSK) - 1];
    }
    if (p_scb) {
      rc_handle = p_scb->rc_handle;
    } else {
      p_cb->disc = 0;
      return;
    }
  }

  log::verbose("rc_handle {}", rc_handle);
  if (p_cb->sdp_a2dp_snk_handle) {
    /* This is Sink + CT + TG(Abs Vol) */
    peer_features = bta_avk_check_peer_features(UUID_SERVCLASS_AV_REM_CTRL_TARGET);
    log::verbose("populating rem ctrl target features {}", peer_features);
    if (BTA_AV_FEAT_ADV_CTRL & bta_avk_check_peer_features(UUID_SERVCLASS_AV_REMOTE_CONTROL)) {
      peer_features |= (BTA_AV_FEAT_ADV_CTRL | BTA_AV_FEAT_RCCT);
    }

    if (peer_features & BTA_AV_FEAT_COVER_ARTWORK) {
      cover_art_psm = bta_avk_get_cover_art_psm();
    }

    log::verbose("populating rem ctrl target bip psm 0x{:x}", cover_art_psm);
  } else if (p_cb->sdp_a2dp_handle) {
    /* check peer version and whether support CT and TG role */
    peer_features = bta_av_check_peer_features(UUID_SERVCLASS_AV_REMOTE_CONTROL);
    if ((p_cb->features & BTA_AV_FEAT_ADV_CTRL) && ((peer_features & BTA_AV_FEAT_ADV_CTRL) == 0)) {
      /* if we support advance control and peer does not, check their support on
       * TG role
       * some implementation uses 1.3 on CT ans 1.4 on TG */
      peer_features |= bta_av_check_peer_features(UUID_SERVCLASS_AV_REM_CTRL_TARGET);
    }

    /* Change our features if the remote AVRCP version is 1.3 or less */
    tSDP_DISC_REC* p_rec = nullptr;
    p_rec = get_legacy_stack_sdp_api()->db.SDP_FindServiceInDb(
            p_cb->p_disc_db, UUID_SERVCLASS_AV_REMOTE_CONTROL, p_rec);
    if (p_rec != NULL && get_legacy_stack_sdp_api()->record.SDP_FindAttributeInRec(
                                 p_rec, ATTR_ID_BT_PROFILE_DESC_LIST) != NULL) {
      /* get profile version (if failure, version parameter is not updated) */
      uint16_t peer_rc_version = 0xFFFF;  // Don't change the AVRCP version
      if (!get_legacy_stack_sdp_api()->record.SDP_FindProfileVersionInRec(
                  p_rec, UUID_SERVCLASS_AV_REMOTE_CONTROL, &peer_rc_version)) {
        log::warn("Unable to find AVRCP version peer:{}", p_rec->remote_bd_addr);
      }
      if (peer_rc_version <= AVRC_REV_1_3) {
        log::verbose("Using AVRCP 1.3 Capabilities with remote device");
        p_bta_av_cfg = &bta_av_cfg_compatibility;
      }
    }
  }

  bta_av_store_peer_rc_version();

  p_cb->disc = 0;
  osi_free_and_reset((void**)&p_cb->p_disc_db);

  log::verbose("peer_features 0x{:x}, features 0x{:x}", peer_features, p_cb->features);

  /* if we have no rc connection */
  if (rc_handle == BTA_AV_RC_HANDLE_NONE) {
    if (p_scb) {
      /* if peer remote control service matches ours and USE_RC is true */
      if (((p_cb->features & BTA_AV_FEAT_RCCT) && (peer_features & BTA_AV_FEAT_RCTG)) ||
          ((p_cb->features & BTA_AV_FEAT_RCTG) && (peer_features & BTA_AV_FEAT_RCCT))) {
        p_lcb = bta_av_find_lcb(p_scb->PeerAddress(), BTA_AV_LCB_FIND);
        if (p_lcb) {
          rc_handle = bta_av_rc_create(p_cb, AVCT_ROLE_INITIATOR, (uint8_t)(p_scb->hdi + 1),
                                       p_lcb->lidx);
          if (rc_handle < BTA_AV_NUM_RCB) {
            p_cb->rcb[rc_handle].peer_features = peer_features;
            p_cb->rcb[rc_handle].cover_art_psm = cover_art_psm;
          } else {
            /* cannot create valid rc_handle for current device. report failure
             */
            log::error("no link resources available");
            p_scb->use_rc = false;
            tBTA_AV bta_av_data = {
                    .rc_open =
                            {
                                    .cover_art_psm = 0,
                                    .peer_features = 0,
                                    .peer_addr = p_scb->PeerAddress(),
                                    .status = BTA_AV_FAIL_RESOURCES,
                            },
            };
            (*p_cb->p_cback)(BTA_AV_RC_OPEN_EVT, &bta_av_data);
          }
        } else {
          log::error("can not find LCB!!");
        }
      } else if (p_scb->use_rc) {
        /* can not find AVRC on peer device. report failure */
        p_scb->use_rc = false;
        tBTA_AV bta_av_data = {
                .rc_open =
                        {
                                .rc_handle = BTA_AV_RC_HANDLE_NONE,
                                .cover_art_psm = 0,
                                .peer_features = 0,
                                .peer_addr = p_scb->PeerAddress(),
                                .status = BTA_AV_FAIL_SDP,
                        },
        };
        (*p_cb->p_cback)(BTA_AV_RC_OPEN_EVT, &bta_av_data);
      }
      if (peer_features != 0) {
        DEVICE_IOT_CONFIG_ADDR_SET_HEX(p_scb->PeerAddress(), IOT_CONF_KEY_AVRCP_FEATURES,
                                       peer_features, IOT_CONF_BYTE_NUM_2);
      }
    }
  } else {
    tBTA_AV_RC_FEAT rc_feat;
    p_cb->rcb[rc_handle].peer_features = peer_features;
    rc_feat.rc_handle = rc_handle;
    rc_feat.peer_features = peer_features;
    if (p_scb == NULL) {
      /*
       * In case scb is not created by the time we are done with SDP
       * we still need to send RC feature event. So we need to get BD
       * from Message.  Note that lidx is 1 based not 0 based
       */
      rc_feat.peer_addr = p_cb->lcb[p_cb->rcb[rc_handle].lidx - 1].addr;
    } else {
      rc_feat.peer_addr = p_scb->PeerAddress();
    }

    tBTA_AV bta_av_feat;
    bta_av_feat.rc_feat = rc_feat;
    (*p_cb->p_cback)(BTA_AV_RC_FEAT_EVT, &bta_av_feat);

    if (peer_features != 0) {
      DEVICE_IOT_CONFIG_ADDR_SET_HEX(rc_feat.peer_addr, IOT_CONF_KEY_AVRCP_FEATURES, peer_features,
                                     IOT_CONF_BYTE_NUM_2);
    }

    // Send PSM data
    log::verbose("Send PSM data");
    tBTA_AV_RC_PSM rc_psm;
    p_cb->rcb[rc_handle].cover_art_psm = cover_art_psm;
    rc_psm.rc_handle = rc_handle;
    rc_psm.cover_art_psm = cover_art_psm;
    if (p_scb == NULL) {
      rc_psm.peer_addr = p_cb->lcb[p_cb->rcb[rc_handle].lidx - 1].addr;
    } else {
      rc_psm.peer_addr = p_scb->PeerAddress();
    }

    log::verbose("rc_psm = 0x{:x}", rc_psm.cover_art_psm);

    tBTA_AV bta_av_psm;
    bta_av_psm.rc_cover_art_psm = rc_psm;
    (*p_cb->p_cback)(BTA_AV_RC_PSM_EVT, &bta_av_psm);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_closed
 *
 * Description      Set AVRCP state to closed.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_closed(tBTA_AV_DATA* p_data) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_RC_CLOSE rc_close;
  tBTA_AV_RC_CONN_CHG* p_msg = (tBTA_AV_RC_CONN_CHG*)p_data;
  tBTA_AV_RCB* p_rcb;
  tBTA_AV_SCB* p_scb;
  int i;
  bool conn = false;
  tBTA_AV_LCB* p_lcb;

  rc_close.rc_handle = BTA_AV_RC_HANDLE_NONE;
  rc_close.peer_addr = RawAddress::kEmpty;
  p_scb = NULL;
  log::verbose("rc_handle:{}, address:{}", p_msg->handle, p_msg->peer_addr);
  for (i = 0; i < BTA_AV_NUM_RCB; i++) {
    p_rcb = &p_cb->rcb[i];
    log::verbose("rcb[{}] rc_handle:{}, status=0x{:x}, shdl:{}, lidx:{}", i, p_rcb->handle,
                 p_rcb->status, p_rcb->shdl, p_rcb->lidx);
    if (p_rcb->handle == p_msg->handle) {
      if (btif_av_src_sink_coexist_enabled() && p_rcb->shdl &&
          (p_rcb->shdl - 1) < BTA_AV_NUM_STRS) {
        p_scb = bta_av_cb.p_scb[p_rcb->shdl - 1];
        if (p_scb && !(p_scb->PeerAddress() == p_msg->peer_addr)) {
          log::verbose("handle{} {} error p_scb or addr", i, p_scb->PeerAddress());
          conn = true;
          continue;
        }
      }
      rc_close.rc_handle = i;
      p_rcb->status &= ~BTA_AV_RC_CONN_MASK;
      p_rcb->peer_features = 0;
      p_rcb->cover_art_psm = 0;
      p_rcb->peer_ct_features = 0;
      p_rcb->peer_tg_features = 0;
      p_cb->rc_feature = {};
      log::verbose("shdl:{}, lidx:{}", p_rcb->shdl, p_rcb->lidx);
      if (p_rcb->shdl) {
        if ((p_rcb->shdl - 1) < BTA_AV_NUM_STRS) {
          p_scb = bta_av_cb.p_scb[p_rcb->shdl - 1];
        }
        if (p_scb) {
          rc_close.peer_addr = p_scb->PeerAddress();
          if (p_scb->rc_handle == p_rcb->handle) {
            p_scb->rc_handle = BTA_AV_RC_HANDLE_NONE;
          }
          log::verbose("shdl:{}, srch:{}", p_rcb->shdl, p_scb->rc_handle);
        }
        p_rcb->shdl = 0;
      } else if (p_rcb->lidx == (BTA_AV_NUM_LINKS + 1)) {
        /* if the RCB uses the extra LCB, use the addr for event and clean it */
        p_lcb = &p_cb->lcb[BTA_AV_NUM_LINKS];
        rc_close.peer_addr = p_msg->peer_addr;
        log::info("rc_only closed bd_addr: {}", p_msg->peer_addr);
        p_lcb->conn_msk = 0;
        p_lcb->lidx = 0;
      }
      p_rcb->lidx = 0;

      if ((p_rcb->status & BTA_AV_RC_ROLE_MASK) == BTA_AV_RC_ROLE_INT) {
        /* AVCT CCB is deallocated */
        p_rcb->handle = BTA_AV_RC_HANDLE_NONE;
        p_rcb->status = 0;
      } else {
        /* AVCT CCB is still there. dealloc */
        bta_av_del_rc(p_rcb);
      }
    } else if ((p_rcb->handle != BTA_AV_RC_HANDLE_NONE) && (p_rcb->status & BTA_AV_RC_CONN_MASK)) {
      /* at least one channel is still connected */
      conn = true;
    }
  }

  if (!conn) {
    /* no AVRC channels are connected, go back to INIT state */
    bta_av_sm_execute(p_cb, BTA_AV_AVRC_NONE_EVT, NULL);
  }

  if (rc_close.rc_handle == BTA_AV_RC_HANDLE_NONE) {
    rc_close.rc_handle = p_msg->handle;
    rc_close.peer_addr = p_msg->peer_addr;
  }
  tBTA_AV bta_av_data;
  bta_av_data.rc_close = rc_close;
  (*p_cb->p_cback)(BTA_AV_RC_CLOSE_EVT, &bta_av_data);
  if (bta_av_cb.rc_acp_handle == BTA_AV_RC_HANDLE_NONE && bta_av_cb.features & BTA_AV_FEAT_RCTG) {
    bta_av_rc_create(&bta_av_cb, AVCT_ROLE_ACCEPTOR, 0, BTA_AV_NUM_LINKS + 1);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rc_browse_opened
 *
 * Description      AVRC browsing channel is opened
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_browse_opened(tBTA_AV_DATA* p_data) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_RC_CONN_CHG* p_msg = (tBTA_AV_RC_CONN_CHG*)p_data;

  log::info("peer_addr: {} rc_handle:{}", p_msg->peer_addr, p_msg->handle);

  tBTA_AV bta_av_data = {
          .rc_browse_open =
                  {
                          .rc_handle = p_msg->handle,
                          .peer_addr = p_msg->peer_addr,
                          .status = BTA_AV_SUCCESS,
                  },
  };

  (*p_cb->p_cback)(BTA_AV_RC_BROWSE_OPEN_EVT, &bta_av_data);
}

/*******************************************************************************
 *
 * Function         bta_av_rc_browse_closed
 *
 * Description      AVRC browsing channel is closed
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_browse_closed(tBTA_AV_DATA* p_data) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_RC_CONN_CHG* p_msg = (tBTA_AV_RC_CONN_CHG*)p_data;

  log::info("peer_addr: {} rc_handle:{}", p_msg->peer_addr, p_msg->handle);

  tBTA_AV bta_av_data = {
          .rc_browse_close =
                  {
                          .rc_handle = p_msg->handle,
                          .peer_addr = p_msg->peer_addr,
                  },
  };

  (*p_cb->p_cback)(BTA_AV_RC_BROWSE_CLOSE_EVT, &bta_av_data);
}

/*******************************************************************************
 *
 * Function         bta_av_rc_disc
 *
 * Description      start AVRC SDP discovery.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_rc_disc(uint8_t disc) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tAVRC_SDP_DB_PARAMS db_params;
  uint16_t attr_list[] = {ATTR_ID_SERVICE_CLASS_ID_LIST, ATTR_ID_BT_PROFILE_DESC_LIST,
                          ATTR_ID_SUPPORTED_FEATURES, ATTR_ID_ADDITION_PROTO_DESC_LISTS};
  uint8_t hdi;
  tBTA_AV_SCB* p_scb;
  RawAddress peer_addr = RawAddress::kEmpty;
  uint8_t rc_handle;

  log::verbose("disc: 0x{:x}, bta_av_cb.disc: 0x{:x}", disc, bta_av_cb.disc);
  if ((bta_av_cb.disc != 0) || (disc == 0)) {
    return;
  }

  if ((disc & BTA_AV_CHNL_MSK) == BTA_AV_CHNL_MSK) {
    /* this is the rc handle/index to tBTA_AV_RCB */
    rc_handle = disc & (~BTA_AV_CHNL_MSK);
    if (p_cb->rcb[rc_handle].lidx) {
      peer_addr = p_cb->lcb[p_cb->rcb[rc_handle].lidx - 1].addr;
    }
  } else {
    hdi = (disc & BTA_AV_HNDL_MSK) - 1;
    p_scb = p_cb->p_scb[hdi];

    if (p_scb) {
      log::verbose("rc_handle {}", p_scb->rc_handle);
      peer_addr = p_scb->PeerAddress();
    }
  }

  if (!peer_addr.IsEmpty()) {
    /* allocate discovery database */
    if (p_cb->p_disc_db == NULL) {
      p_cb->p_disc_db = (tSDP_DISCOVERY_DB*)osi_malloc(BTA_AV_DISC_BUF_SIZE);
    }

    /* set up parameters */
    db_params.db_len = BTA_AV_DISC_BUF_SIZE;
    db_params.num_attr = sizeof(attr_list) / sizeof(uint16_t);
    db_params.p_db = p_cb->p_disc_db;
    db_params.p_attrs = attr_list;

    /* searching for UUID_SERVCLASS_AV_REMOTE_CONTROL gets both TG and CT */
    if (AVRC_FindService(UUID_SERVCLASS_AV_REMOTE_CONTROL, peer_addr, &db_params,
                         base::Bind(bta_av_avrc_sdp_cback)) == AVRC_SUCCESS) {
      p_cb->disc = disc;
      log::verbose("disc 0x{:x}", p_cb->disc);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_dereg_comp
 *
 * Description      deregister complete. free the stream control block.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_dereg_comp(tBTA_AV_DATA* p_data) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  tBTA_AV_SCB* p_scb;
  tBTA_UTL_COD cod = {
          .minor = BTM_COD_MINOR_UNCLASSIFIED,
          .major = BTM_COD_MAJOR_UNCLASSIFIED,
          .service = 0,
  };

  uint8_t mask;
  BT_HDR* p_buf;

  /* find the stream control block */
  p_scb = bta_av_hndl_to_scb(p_data->hdr.layer_specific);

  if (p_scb) {
    log::verbose("deregistered {}(h{})", p_scb->chnl, p_scb->hndl);
    mask = BTA_AV_HNDL_TO_MSK(p_scb->hdi);
    p_cb->reg_audio &= ~mask;
    if ((p_cb->conn_audio & mask) && p_cb->audio_open_cnt) {
      /* this channel is still marked as open. decrease the count */
      p_cb->audio_open_cnt--;
    }
    p_cb->conn_audio &= ~mask;

    if (p_scb->q_tag == BTA_AV_Q_TAG_STREAM && p_scb->a2dp_list) {
      /* make sure no buffers are in a2dp_list */
      while (!list_is_empty(p_scb->a2dp_list)) {
        p_buf = (BT_HDR*)list_front(p_scb->a2dp_list);
        list_remove(p_scb->a2dp_list, p_buf);
        osi_free(p_buf);
      }
    }

    /* remove the A2DP SDP record, if no more audio stream is left */
    if (!p_cb->reg_audio) {
      /* Only remove the SDP record if we're the ones that created it */
      if (is_new_avrcp_enabled()) {
        log::verbose(
                "newavrcp is the owner of the AVRCP Target SDP record. Don't dereg the SDP record");
      } else {
        log::verbose("newavrcp is not enabled. Remove SDP record");
        bta_ar_dereg_avrc(UUID_SERVCLASS_AV_REMOTE_CONTROL);
      }

      if (p_cb->sdp_a2dp_handle) {
        bta_av_del_sdp_rec(&p_cb->sdp_a2dp_handle);
        p_cb->sdp_a2dp_handle = 0;
        bta_sys_remove_uuid(UUID_SERVCLASS_AUDIO_SOURCE);
      }

      if (p_cb->sdp_a2dp_snk_handle) {
        bta_av_del_sdp_rec(&p_cb->sdp_a2dp_snk_handle);
        p_cb->sdp_a2dp_snk_handle = 0;
        bta_sys_remove_uuid(UUID_SERVCLASS_AUDIO_SINK);
      }
    }

    bta_av_free_scb(p_scb);
  }

  log::verbose("audio 0x{:x}, disable:{}", p_cb->reg_audio, p_cb->disabling);
  /* if no stream control block is active */
  if (p_cb->reg_audio == 0) {
    /* deregister from AVDT */
    bta_ar_dereg_avdt();

    /* deregister from AVCT */
    bta_ar_dereg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET);
    bta_ar_dereg_avct();

    if (p_cb->disabling) {
      p_cb->disabling = false;
      // reset enabling parameters
      p_cb->features = 0;
      p_cb->sec_mask = 0;
      bta_av_cb.sink_features = 0;
      bta_av_cb.reg_role = 0;
    }

    /* Clear the Capturing service class bit */
    cod.service = BTM_COD_SERVICE_CAPTURING;
    utl_set_device_class(&cod, BTA_UTL_CLR_COD_SERVICE_CLASS);
  }
}
