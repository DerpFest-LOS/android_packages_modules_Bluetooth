/******************************************************************************
 *
 *  Copyright 2002-2012 Broadcom Corporation
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
 *  This module contains the AVDTP adaptation layer.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth-a2dp"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>
#include <string.h>

#include <cstdint>

#include "avdt_api.h"
#include "avdt_int.h"
#include "internal_include/bt_target.h"
#include "l2cap_types.h"
#include "l2cdefs.h"
#include "osi/include/allocator.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/btm_sec_api_types.h"
#include "stack/include/l2cap_interface.h"

using namespace bluetooth;

AvdtpScb* AvdtpAdaptationLayer::LookupAvdtpScb(const AvdtpTransportChannel& tc) {
  if (tc.ccb_idx >= AVDT_NUM_LINKS) {
    log::error("AvdtpScb entry not found: invalid ccb_idx: {}", tc.ccb_idx);
    return nullptr;
  }
  if (tc.tcid >= AVDT_NUM_RT_TBL) {
    log::error("AvdtpScb entry not found: invalid tcid: {}", tc.tcid);
    return nullptr;
  }
  const AvdtpRoutingEntry& re = rt_tbl[tc.ccb_idx][tc.tcid];
  log::verbose("ccb_idx: {} tcid: {} scb_hdl: {}", tc.ccb_idx, tc.tcid, re.scb_hdl);
  return avdt_scb_by_hdl(re.scb_hdl);
}

/*******************************************************************************
 *
 * Function         avdt_ad_type_to_tcid
 *
 * Description      Derives the TCID from the channel type and SCB.
 *
 *
 * Returns          TCID value.
 *
 ******************************************************************************/
uint8_t avdt_ad_type_to_tcid(uint8_t type, AvdtpScb* p_scb) {
  if (type == AVDT_CHAN_SIG) {
    return 0;
  }
  // The SCB Handle is unique in the [1, AVDT_NUM_LINKS * AVDT_NUM_SEPS]
  // range. The scb_idx computed here is the SCB index for the corresponding
  // SEP, and it is in the range [0, AVDT_NUM_SEPS) for a particular link.
  uint8_t scb_idx = (avdt_scb_to_hdl(p_scb) - 1) % AVDT_NUM_LINKS;
  // There are AVDT_CHAN_NUM_TYPES channel types per SEP. Here we compute
  // the type index (TCID) from the SEP index and the type itself.
  return (scb_idx * (AVDT_CHAN_NUM_TYPES - 1)) + type;
}

/*******************************************************************************
 *
 * Function         avdt_ad_tcid_to_type
 *
 * Description      Derives the channel type from the TCID.
 *
 *
 * Returns          Channel type value.
 *
 ******************************************************************************/
static uint8_t avdt_ad_tcid_to_type(uint8_t tcid) {
  if (tcid == 0) {
    return AVDT_CHAN_SIG;
  }
  /* tcid translates to type based on number of channels, as follows:
  ** only media channel   :  tcid=1,2,3,4,5,6...  type=1,1,1,1,1,1...
  ** media and report     :  tcid=1,2,3,4,5,6...  type=1,2,1,2,1,2...
  ** media, report, recov :  tcid=1,2,3,4,5,6...  type=1,2,3,1,2,3...
  */
  return ((tcid + AVDT_CHAN_NUM_TYPES - 2) % (AVDT_CHAN_NUM_TYPES - 1)) + 1;
}

/*******************************************************************************
 *
 * Function         avdt_ad_init
 *
 * Description      Initialize adaptation layer.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_ad_init(void) {
  int i;
  AvdtpTransportChannel* p_tbl = avdtp_cb.ad.tc_tbl;
  avdtp_cb.ad.Reset();

  /* make sure the peer_mtu is a valid value */
  for (i = 0; i < AVDT_NUM_TC_TBL; i++, p_tbl++) {
    p_tbl->peer_mtu = L2CAP_DEFAULT_MTU;
  }
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_tbl_by_st
 *
 * Description      Find adaptation layer transport channel table entry matching
 *                  the given state.
 *
 *
 * Returns          Pointer to matching entry.  For control channel it returns
 *                  the matching entry.  For media or other it returns the
 *                  first matching entry (there could be more than one).
 *
 ******************************************************************************/
AvdtpTransportChannel* avdt_ad_tc_tbl_by_st(uint8_t type, AvdtpCcb* p_ccb, uint8_t state) {
  int i;
  AvdtpTransportChannel* p_tbl = avdtp_cb.ad.tc_tbl;
  uint8_t ccb_idx;

  if (p_ccb == NULL) {
    /* resending security req */
    for (i = 0; i < AVDT_NUM_TC_TBL; i++, p_tbl++) {
      /* must be AVDT_CHAN_SIG - tcid always zero */
      if ((p_tbl->tcid == 0) && (p_tbl->state == state)) {
        break;
      }
    }
  } else {
    ccb_idx = avdt_ccb_to_idx(p_ccb);

    for (i = 0; i < AVDT_NUM_TC_TBL; i++, p_tbl++) {
      if (type == AVDT_CHAN_SIG) {
        /* if control channel, tcid always zero */
        if ((p_tbl->tcid == 0) && (p_tbl->ccb_idx == ccb_idx) && (p_tbl->state == state)) {
          break;
        }
      } else {
        /* if other channel, tcid is always > zero */
        if ((p_tbl->tcid > 0) && (p_tbl->ccb_idx == ccb_idx) && (p_tbl->state == state)) {
          break;
        }
      }
    }
  }

  /* if nothing found return null */
  if (i == AVDT_NUM_TC_TBL) {
    p_tbl = NULL;
  }

  return p_tbl;
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_tbl_by_lcid
 *
 * Description      Find adaptation layer transport channel table entry by LCID.
 *
 *
 * Returns          Pointer to entry.
 *
 ******************************************************************************/
AvdtpTransportChannel* avdt_ad_tc_tbl_by_lcid(uint16_t lcid) {
  if (avdtp_cb.ad.lcid_tbl.count(lcid) != 0) {
    uint8_t idx = avdtp_cb.ad.lcid_tbl[lcid];
    return &avdtp_cb.ad.tc_tbl[idx];
  } else {
    return nullptr;
  }
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_tbl_by_type
 *
 * Description      This function retrieves the transport channel table entry
 *                  for a particular channel.
 *
 *
 * Returns          Pointer to transport channel table entry.
 *
 ******************************************************************************/
AvdtpTransportChannel* avdt_ad_tc_tbl_by_type(uint8_t type, AvdtpCcb* p_ccb, AvdtpScb* p_scb) {
  uint8_t tcid;
  int i;
  AvdtpTransportChannel* p_tbl = avdtp_cb.ad.tc_tbl;
  uint8_t ccb_idx = avdt_ccb_to_idx(p_ccb);

  /* get tcid from type, scb */
  tcid = avdt_ad_type_to_tcid(type, p_scb);

  for (i = 0; i < AVDT_NUM_TC_TBL; i++, p_tbl++) {
    if ((p_tbl->tcid == tcid) && (p_tbl->ccb_idx == ccb_idx)) {
      break;
    }
  }

  log::assert_that(i != AVDT_NUM_TC_TBL, "assert failed: i != AVDT_NUM_TC_TBL");

  return p_tbl;
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_tbl_alloc
 *
 * Description      Allocate an entry in the traffic channel table.
 *
 *
 * Returns          Pointer to entry.
 *
 ******************************************************************************/
AvdtpTransportChannel* avdt_ad_tc_tbl_alloc(AvdtpCcb* p_ccb) {
  int i;
  AvdtpTransportChannel* p_tbl = avdtp_cb.ad.tc_tbl;

  /* find next free entry in tc table */
  for (i = 0; i < AVDT_NUM_TC_TBL; i++, p_tbl++) {
    if (p_tbl->state == AVDT_AD_ST_UNUSED) {
      break;
    }
  }

  /* sanity check */
  log::assert_that(i != AVDT_NUM_TC_TBL, "assert failed: i != AVDT_NUM_TC_TBL");

  /* initialize entry */
  p_tbl->peer_mtu = L2CAP_DEFAULT_MTU;
  p_tbl->role = tAVDT_ROLE::AVDT_UNKNOWN;
  p_tbl->ccb_idx = avdt_ccb_to_idx(p_ccb);
  p_tbl->state = AVDT_AD_ST_IDLE;
  return p_tbl;
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_tbl_to_idx
 *
 * Description      Convert a transport channel table entry to an index.
 *
 *
 * Returns          Index value.
 *
 ******************************************************************************/
uint8_t avdt_ad_tc_tbl_to_idx(AvdtpTransportChannel* p_tbl) {
  return (uint8_t)(p_tbl - avdtp_cb.ad.tc_tbl);
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_close_ind
 *
 * Description      This function is called by the L2CAP interface when the
 *                  L2CAP channel is closed.  It looks up the CCB or SCB for
 *                  the channel and sends it a close event.  The reason
 *                  parameter is the same value passed by the L2CAP
 *                  callback function.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_ad_tc_close_ind(AvdtpTransportChannel* p_tbl) {
  AvdtpCcb* p_ccb;
  AvdtpScb* p_scb;
  tAVDT_SCB_TC_CLOSE close;

  log::verbose("p_tbl: {} state: {} tcid: {} type: {} ccb_idx: {} scb_hdl: {}",
               std::format_ptr(p_tbl), tc_state_text(p_tbl->state), p_tbl->tcid,
               tc_type_text(avdt_ad_tcid_to_type(p_tbl->tcid)), p_tbl->ccb_idx,
               avdtp_cb.ad.rt_tbl[p_tbl->ccb_idx][p_tbl->tcid].scb_hdl);

  close.old_tc_state = p_tbl->state;
  /* clear avdt_ad_tc_tbl entry */
  p_tbl->state = AVDT_AD_ST_UNUSED;
  p_tbl->role = tAVDT_ROLE::AVDT_UNKNOWN;
  p_tbl->peer_mtu = L2CAP_DEFAULT_MTU;

  /* if signaling channel, notify ccb that channel close */
  if (p_tbl->tcid == 0) {
    p_ccb = avdt_ccb_by_idx(p_tbl->ccb_idx);
    avdt_ccb_event(p_ccb, AVDT_CCB_LL_CLOSE_EVT, NULL);
    return;
  }
  /* if media or other channel, notify scb that channel close */
  /* look up scb in stream routing table by ccb, tcid */
  p_scb = avdtp_cb.ad.LookupAvdtpScb(*p_tbl);
  if (p_scb == nullptr) {
    log::error("Cannot find AvdtScb entry: ccb_idx: {} tcid: {}", p_tbl->ccb_idx, p_tbl->tcid);
    return;
  }
  close.tcid = p_tbl->tcid;
  close.type = avdt_ad_tcid_to_type(p_tbl->tcid);
  tAVDT_SCB_EVT avdt_scb_evt;
  avdt_scb_evt.close = close;
  avdt_scb_event(p_scb, AVDT_SCB_TC_CLOSE_EVT, &avdt_scb_evt);
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_open_ind
 *
 * Description      This function is called by the L2CAP interface when
 *                  the L2CAP channel is opened.  It looks up the CCB or SCB
 *                  for the channel and sends it an open event.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_ad_tc_open_ind(AvdtpTransportChannel* p_tbl) {
  AvdtpCcb* p_ccb;
  AvdtpScb* p_scb;
  tAVDT_OPEN open;
  tAVDT_EVT_HDR evt;

  log::verbose("p_tbl: {} state: {} tcid: {} type: {} ccb_idx: {} scb_hdl: {}",
               std::format_ptr(p_tbl), tc_state_text(p_tbl->state), p_tbl->tcid,
               tc_type_text(avdt_ad_tcid_to_type(p_tbl->tcid)), p_tbl->ccb_idx,
               avdtp_cb.ad.rt_tbl[p_tbl->ccb_idx][p_tbl->tcid].scb_hdl);

  p_tbl->state = AVDT_AD_ST_OPEN;

  /* if signaling channel, notify ccb that channel open */
  if (p_tbl->tcid == 0) {
    /* set the signal channel to use high priority within the ACL link */
    if (!stack::l2cap::get_interface().L2CA_SetTxPriority(
                avdtp_cb.ad.rt_tbl[p_tbl->ccb_idx][AVDT_CHAN_SIG].lcid, L2CAP_CHNL_PRIORITY_HIGH)) {
      log::warn("Unable to set L2CAP transmit high priority cid: 0x{:x}",
                avdtp_cb.ad.rt_tbl[p_tbl->ccb_idx][AVDT_CHAN_SIG].lcid);
    }

    p_ccb = avdt_ccb_by_idx(p_tbl->ccb_idx);
    /* use err_param to indicate the role of connection */
    evt.err_param = static_cast<uint8_t>(p_tbl->role);
    tAVDT_CCB_EVT avdt_ccb_evt;
    avdt_ccb_evt.msg.hdr = evt;
    avdt_ccb_event(p_ccb, AVDT_CCB_LL_OPEN_EVT, &avdt_ccb_evt);
    return;
  }
  /* if media or other channel, notify scb that channel open */
  /* look up scb in stream routing table by ccb, tcid */
  p_scb = avdtp_cb.ad.LookupAvdtpScb(*p_tbl);
  if (p_scb == nullptr) {
    log::error("Cannot find AvdtScb entry: ccb_idx: {} tcid: {}", p_tbl->ccb_idx, p_tbl->tcid);
    return;
  }
  /* put lcid in event data */
  open.peer_mtu = p_tbl->peer_mtu;
  open.lcid = avdtp_cb.ad.rt_tbl[p_tbl->ccb_idx][p_tbl->tcid].lcid;
  open.hdr.err_code = avdt_ad_tcid_to_type(p_tbl->tcid);
  tAVDT_SCB_EVT avdt_scb_evt;
  avdt_scb_evt.open = open;
  avdt_scb_event(p_scb, AVDT_SCB_TC_OPEN_EVT, &avdt_scb_evt);
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_cong_ind
 *
 * Description      This function is called by the L2CAP interface layer when
 *                  L2CAP calls the congestion callback.  It looks up the CCB
 *                  or SCB for the channel and sends it a congestion event.
 *                  The is_congested parameter is the same value passed by
 *                  the L2CAP callback function.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_ad_tc_cong_ind(AvdtpTransportChannel* p_tbl, bool is_congested) {
  AvdtpCcb* p_ccb;
  AvdtpScb* p_scb;

  log::verbose("p_tbl: {} state: {} tcid: {} type: {} ccb_idx: {} scb_hdl: {} is_congested: {}",
               std::format_ptr(p_tbl), tc_state_text(p_tbl->state), p_tbl->tcid,
               tc_type_text(avdt_ad_tcid_to_type(p_tbl->tcid)), p_tbl->ccb_idx,
               avdtp_cb.ad.rt_tbl[p_tbl->ccb_idx][p_tbl->tcid].scb_hdl, is_congested);

  /* if signaling channel, notify ccb of congestion */
  if (p_tbl->tcid == 0) {
    p_ccb = avdt_ccb_by_idx(p_tbl->ccb_idx);
    tAVDT_CCB_EVT avdt_ccb_evt;
    avdt_ccb_evt.llcong = is_congested;
    avdt_ccb_event(p_ccb, AVDT_CCB_LL_CONG_EVT, &avdt_ccb_evt);
    return;
  }
  /* if media or other channel, notify scb that channel open */
  /* look up scb in stream routing table by ccb, tcid */
  p_scb = avdtp_cb.ad.LookupAvdtpScb(*p_tbl);
  if (p_scb == nullptr) {
    log::error("Cannot find AvdtScb entry: ccb_idx: {} tcid: {}", p_tbl->ccb_idx, p_tbl->tcid);
    return;
  }
  tAVDT_SCB_EVT avdt_scb_evt;
  avdt_scb_evt.llcong = is_congested;
  avdt_scb_event(p_scb, AVDT_SCB_TC_CONG_EVT, &avdt_scb_evt);
}

/*******************************************************************************
 *
 * Function         avdt_ad_tc_data_ind
 *
 * Description      This function is called by the L2CAP interface layer when
 *                  incoming data is received from L2CAP.  It looks up the CCB
 *                  or SCB for the channel and routes the data accordingly.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_ad_tc_data_ind(AvdtpTransportChannel* p_tbl, BT_HDR* p_buf) {
  AvdtpCcb* p_ccb;
  AvdtpScb* p_scb;

  /* store type (media, recovery, reporting) */
  p_buf->layer_specific = avdt_ad_tcid_to_type(p_tbl->tcid);

  /* if signaling channel, handle control message */
  if (p_tbl->tcid == 0) {
    p_ccb = avdt_ccb_by_idx(p_tbl->ccb_idx);
    avdt_msg_ind(p_ccb, p_buf);
    return;
  }
  /* if media or other channel, send event to scb */
  p_scb = avdtp_cb.ad.LookupAvdtpScb(*p_tbl);
  if (p_scb == nullptr) {
    log::error("Cannot find AvdtScb entry: ccb_idx: {} tcid: {}", p_tbl->ccb_idx, p_tbl->tcid);
    osi_free(p_buf);
    log::error("buffer freed");
    return;
  }
  avdt_scb_event(p_scb, AVDT_SCB_TC_DATA_EVT, (tAVDT_SCB_EVT*)&p_buf);
}

/*******************************************************************************
 *
 * Function         avdt_ad_write_req
 *
 * Description      This function is called by a CCB or SCB to send data to a
 *                  transport channel.  It looks up the LCID of the channel
 *                  based on the type, CCB, and SCB (if present).  Then it
 *                  passes the data to L2CA_DataWrite().
 *
 *
 * Returns          AVDT_AD_SUCCESS, if data accepted
 *                  AVDT_AD_CONGESTED, if data accepted and the channel is
 *                                     congested
 *                  AVDT_AD_FAILED, if error
 *
 ******************************************************************************/
tL2CAP_DW_RESULT avdt_ad_write_req(uint8_t type, AvdtpCcb* p_ccb, AvdtpScb* p_scb, BT_HDR* p_buf) {
  uint8_t tcid;

  /* get tcid from type, scb */
  tcid = avdt_ad_type_to_tcid(type, p_scb);

  return stack::l2cap::get_interface().L2CA_DataWrite(
          avdtp_cb.ad.rt_tbl[avdt_ccb_to_idx(p_ccb)][tcid].lcid, p_buf);
}

/*******************************************************************************
 *
 * Function         avdt_ad_open_req
 *
 * Description      This function is called by a CCB or SCB to open a transport
 *                  channel.  This function allocates and initializes a
 *                  transport channel table entry.  The channel can be opened
 *                  in two roles:  as an initiator or acceptor.  When opened
 *                  as an initiator the function will start an L2CAP connection.
 *                  When opened as an acceptor the function simply configures
 *                  the table entry to listen for an incoming channel.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_ad_open_req(uint8_t type, AvdtpCcb* p_ccb, AvdtpScb* p_scb, tAVDT_ROLE role) {
  AvdtpTransportChannel* p_tbl;
  uint16_t lcid;

  p_tbl = avdt_ad_tc_tbl_alloc(p_ccb);
  if (p_tbl == NULL) {
    log::error("Cannot allocate p_tbl");
    return;
  }

  p_tbl->tcid = avdt_ad_type_to_tcid(type, p_scb);
  p_tbl->my_mtu = kAvdtpMtu;
  log::verbose("p_tbl: {} state: {} tcid: {} type: {} role: {} my_mtu: {}", std::format_ptr(p_tbl),
               tc_state_text(p_tbl->state), p_tbl->tcid, tc_type_text(type), avdt_role_text(role),
               p_tbl->my_mtu);

  if (type != AVDT_CHAN_SIG) {
    /* also set scb_hdl in rt_tbl */
    avdtp_cb.ad.rt_tbl[avdt_ccb_to_idx(p_ccb)][p_tbl->tcid].scb_hdl = avdt_scb_to_hdl(p_scb);
    log::verbose("For ccb index: {}, tcid: {} store scb_hdl: {}", avdt_ccb_to_idx(p_ccb),
                 p_tbl->tcid, avdt_scb_to_hdl(p_scb));
  }

  if (role == tAVDT_ROLE::AVDT_ACP) {
    /* if we're acceptor, we're done; just sit back and listen */
    p_tbl->state = AVDT_AD_ST_ACP;
  } else {
    /* else we're inititator, start the L2CAP connection */
    p_tbl->state = AVDT_AD_ST_CONN;

    /* call l2cap connect req */
    lcid = stack::l2cap::get_interface().L2CA_ConnectReqWithSecurity(
      AVDT_PSM, p_ccb->peer_addr, BTM_SEC_OUT_AUTHENTICATE | BTM_SEC_OUT_ENCRYPT);

    if (lcid == 0) {
      /* if connect req failed, call avdt_ad_tc_close_ind() */
      avdt_ad_tc_close_ind(p_tbl);
      return;
    }

    /* if connect req ok, store tcid in lcid table  */
    avdtp_cb.ad.lcid_tbl[lcid] = avdt_ad_tc_tbl_to_idx(p_tbl);
    log::verbose("For lcid: 0x{:x} store table index: {}", lcid, avdt_ad_tc_tbl_to_idx(p_tbl));

    avdtp_cb.ad.rt_tbl[avdt_ccb_to_idx(p_ccb)][p_tbl->tcid].lcid = lcid;
    log::verbose("For ccb index: {} and tcid: {} store lcid 0x{:x}", avdt_ccb_to_idx(p_ccb),
                 p_tbl->tcid, lcid);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ad_close_req
 *
 * Description      This function is called by a CCB or SCB to close a
 *                  transport channel.  The function looks up the LCID for the
 *                  channel and calls L2CA_DisconnectReq().
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_ad_close_req(uint8_t type, AvdtpCcb* p_ccb, AvdtpScb* p_scb) {
  uint16_t lcid;
  AvdtpTransportChannel* p_tbl;

  p_tbl = avdt_ad_tc_tbl_by_type(type, p_ccb, p_scb);
  log::verbose("p_tbl: {} state: {} tcid: {} type: {} ccb_idx: {} scb_hdl: {}",
               std::format_ptr(p_tbl), tc_state_text(p_tbl->state), p_tbl->tcid, tc_type_text(type),
               p_tbl->ccb_idx, avdtp_cb.ad.rt_tbl[p_tbl->ccb_idx][p_tbl->tcid].scb_hdl);

  switch (p_tbl->state) {
    case AVDT_AD_ST_UNUSED:
      /* probably for reporting */
      break;
    case AVDT_AD_ST_ACP:
      /* if we're listening on this channel, send ourselves a close ind */
      avdt_ad_tc_close_ind(p_tbl);
      break;
    default:
      lcid = p_tbl->lcid;
      /* call l2cap disconnect req */
      if (!stack::l2cap::get_interface().L2CA_DisconnectReq(lcid)) {
        log::warn("Unable to disconnect L2CAP lcid: 0x{:04x}", lcid);
      }
      avdt_ad_tc_close_ind(p_tbl);
  }
}
