/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
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
 *  This file contains state machine and action routines for a port of the
 *  RFCOMM unit
 *
 ******************************************************************************/

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <cstdint>
#include <cstring>
#include <set>

#include "hal/snoop_logger.h"
#include "main/shim/entry.h"
#include "osi/include/allocator.h"
#include "stack/btm/btm_sec.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/btm_status.h"
#include "stack/l2cap/l2c_int.h"
#include "stack/rfcomm/port_int.h"
#include "stack/rfcomm/rfc_int.h"
#include "stack/rfcomm/rfc_state.h"

using namespace bluetooth;

static const std::set<uint16_t> uuid_logging_acceptlist = {
        UUID_SERVCLASS_HEADSET_AUDIO_GATEWAY,
        UUID_SERVCLASS_AG_HANDSFREE,
};

/******************************************************************************/
/*            L O C A L    F U N C T I O N     P R O T O T Y P E S            */
/******************************************************************************/
static void rfc_port_sm_state_closed(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data);
static void rfc_port_sm_sabme_wait_ua(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data);
static void rfc_port_sm_opened(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data);
static void rfc_port_sm_orig_wait_sec_check(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data);
static void rfc_port_sm_term_wait_sec_check(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data);
static void rfc_port_sm_disc_wait_ua(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data);

static void rfc_port_uplink_data(tPORT* p_port, BT_HDR* p_buf);

static void rfc_set_port_settings(PortSettings* port_settings, MX_FRAME* p_frame);

/*******************************************************************************
 *
 * Function         rfc_port_sm_execute
 *
 * Description      This function sends port events through the state
 *                  machine.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_sm_execute(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data) {
  log::assert_that(p_port != nullptr, "NULL port event {}", event);

  // logs for state RFC_STATE_OPENED handled in rfc_port_sm_opened()
  if (p_port->rfc.state != RFC_STATE_OPENED) {
    log::info("bd_addr:{}, handle:{}, state:{}, event:{}", p_port->bd_addr, p_port->handle,
              rfcomm_port_state_text(p_port->rfc.state), rfcomm_port_event_text(event));
  }
  switch (p_port->rfc.state) {
    case RFC_STATE_CLOSED:
      rfc_port_sm_state_closed(p_port, event, p_data);
      break;

    case RFC_STATE_SABME_WAIT_UA:
      rfc_port_sm_sabme_wait_ua(p_port, event, p_data);
      break;

    case RFC_STATE_ORIG_WAIT_SEC_CHECK:
      rfc_port_sm_orig_wait_sec_check(p_port, event, p_data);
      break;

    case RFC_STATE_TERM_WAIT_SEC_CHECK:
      rfc_port_sm_term_wait_sec_check(p_port, event, p_data);
      break;

    case RFC_STATE_OPENED:
      rfc_port_sm_opened(p_port, event, p_data);
      break;

    case RFC_STATE_DISC_WAIT_UA:
      rfc_port_sm_disc_wait_ua(p_port, event, p_data);
      break;
  }
}

/*******************************************************************************
 *
 * Function         rfc_port_sm_state_closed
 *
 * Description      This function handles events when the port is in
 *                  CLOSED state. This state exists when port is
 *                  being initially established.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_sm_state_closed(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data) {
  switch (event) {
    case RFC_PORT_EVENT_OPEN:
      p_port->rfc.state = RFC_STATE_ORIG_WAIT_SEC_CHECK;
      btm_sec_mx_access_request(p_port->rfc.p_mcb->bd_addr, true, p_port->sec_mask,
                                &rfc_sec_check_complete, p_port);
      return;

    case RFC_PORT_EVENT_CLOSE:
      break;

    case RFC_PORT_EVENT_CLEAR:
      return;

    case RFC_PORT_EVENT_DATA:
      osi_free(p_data);
      break;

    case RFC_PORT_EVENT_SABME:
      /* make sure the multiplexer disconnect timer is not running (reconnect
       * case) */
      rfc_timer_stop(p_port->rfc.p_mcb);

      /* Open will be continued after security checks are passed */
      p_port->rfc.state = RFC_STATE_TERM_WAIT_SEC_CHECK;
      btm_sec_mx_access_request(p_port->rfc.p_mcb->bd_addr, false, p_port->sec_mask,
                                &rfc_sec_check_complete, p_port);
      return;

    case RFC_PORT_EVENT_UA:
      return;

    case RFC_PORT_EVENT_DM:
      log::warn("RFC_EVENT_DM, handle:{}", p_port->handle);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_UIH:
      osi_free(p_data);
      rfc_send_dm(p_port->rfc.p_mcb, p_port->dlci, false);
      return;

    case RFC_PORT_EVENT_DISC:
      rfc_send_dm(p_port->rfc.p_mcb, p_port->dlci, false);
      return;

    case RFC_PORT_EVENT_TIMEOUT:
      PORT_TimeOutCloseMux(p_port->rfc.p_mcb);
      log::error("Port error state {} event {}", p_port->rfc.state, event);
      return;
    default:
      log::error("Received unexpected event:{} in state:{}", rfcomm_port_event_text(event),
                 rfcomm_port_state_text(p_port->rfc.state));
  }

  log::warn("Event ignored {}", rfcomm_port_event_text(event));
  return;
}

/*******************************************************************************
 *
 * Function         rfc_port_sm_sabme_wait_ua
 *
 * Description      This function handles events when SABME on the DLC was
 *                  sent and SM is waiting for UA or DM.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_sm_sabme_wait_ua(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data) {
  switch (event) {
    case RFC_PORT_EVENT_OPEN:
    case RFC_PORT_EVENT_ESTABLISH_RSP:
      log::error("Port error event:{}", event);
      return;

    case RFC_PORT_EVENT_CLOSE:
      rfc_port_timer_start(p_port, RFC_DISC_TIMEOUT);
      rfc_send_disc(p_port->rfc.p_mcb, p_port->dlci);
      p_port->rfc.expected_rsp = 0;
      p_port->rfc.state = RFC_STATE_DISC_WAIT_UA;
      return;

    case RFC_PORT_EVENT_CLEAR:
      log::warn("RFC_PORT_EVENT_CLEAR, handle:{}", p_port->handle);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_DATA:
      osi_free(p_data);
      break;

    case RFC_PORT_EVENT_UA:
      rfc_port_timer_stop(p_port);
      p_port->rfc.state = RFC_STATE_OPENED;

      if (uuid_logging_acceptlist.find(p_port->uuid) != uuid_logging_acceptlist.end()) {
        // Find Channel Control Block by Channel ID
        tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(nullptr, p_port->rfc.p_mcb->lcid);
        if (p_ccb) {
          bluetooth::shim::GetSnoopLogger()->AcceptlistRfcommDlci(
                  p_ccb->p_lcb->Handle(), p_port->rfc.p_mcb->lcid, p_port->dlci);
        }
      }
      if (p_port->rfc.p_mcb) {
        uint16_t lcid;
        tL2C_CCB* ccb;

        lcid = p_port->rfc.p_mcb->lcid;
        ccb = l2cu_find_ccb_by_cid(nullptr, lcid);

        if (ccb) {
          bluetooth::shim::GetSnoopLogger()->SetRfcommPortOpen(
                  ccb->p_lcb->Handle(), lcid, p_port->dlci, p_port->uuid,
                  p_port->rfc.p_mcb->flow == PORT_FC_CREDIT);
        }
      }

      PORT_DlcEstablishCnf(p_port->rfc.p_mcb, p_port->dlci, p_port->rfc.p_mcb->peer_l2cap_mtu,
                           RFCOMM_SUCCESS);
      return;

    case RFC_PORT_EVENT_DM:
      log::warn("RFC_EVENT_DM, handle:{}", p_port->handle);
      p_port->rfc.p_mcb->is_disc_initiator = true;
      PORT_DlcEstablishCnf(p_port->rfc.p_mcb, p_port->dlci, p_port->rfc.p_mcb->peer_l2cap_mtu,
                           RFCOMM_ERROR);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_DISC:
      log::warn("RFC_EVENT_DISC, handle:{}", p_port->handle);
      rfc_send_ua(p_port->rfc.p_mcb, p_port->dlci);
      PORT_DlcEstablishCnf(p_port->rfc.p_mcb, p_port->dlci, p_port->rfc.p_mcb->peer_l2cap_mtu,
                           RFCOMM_ERROR);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_SABME:
      /* Continue to wait for the UA the SABME this side sent */
      rfc_send_ua(p_port->rfc.p_mcb, p_port->dlci);
      return;

    case RFC_PORT_EVENT_UIH:
      osi_free(p_data);
      return;

    case RFC_PORT_EVENT_TIMEOUT:
      p_port->rfc.state = RFC_STATE_CLOSED;
      PORT_DlcEstablishCnf(p_port->rfc.p_mcb, p_port->dlci, p_port->rfc.p_mcb->peer_l2cap_mtu,
                           RFCOMM_ERROR);
      return;
    default:
      log::error("Received unexpected event:{} in state:{}", rfcomm_port_event_text(event),
                 rfcomm_port_state_text(static_cast<tRFC_PORT_STATE>(p_port->rfc.state)));
  }
  log::warn("Event ignored {}", rfcomm_port_event_text(event));
}

/*******************************************************************************
 *
 * Function         rfc_port_sm_term_wait_sec_check
 *
 * Description      This function handles events for the port in the
 *                  WAIT_SEC_CHECK state.  SABME has been received from the
 *                  peer and Security Manager verifes address, before we can
 *                  send ESTABLISH_IND to the Port entity
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_sm_term_wait_sec_check(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data) {
  switch (event) {
    case RFC_PORT_EVENT_SEC_COMPLETE:
      if (*((tBTM_STATUS*)p_data) != tBTM_STATUS::BTM_SUCCESS) {
        log::error("Security check failed result:{} state:{} port_handle:{}",
                   btm_status_text(*((tBTM_STATUS*)p_data)),
                   rfcomm_port_state_text(p_port->rfc.state), p_port->handle);
        /* Authentication/authorization failed.  If link is still  */
        /* up send DM and check if we need to start inactive timer */
        if (p_port->rfc.p_mcb) {
          rfc_send_dm(p_port->rfc.p_mcb, p_port->dlci, true);
          p_port->rfc.p_mcb->is_disc_initiator = true;
          port_rfc_closed(p_port, PORT_SEC_FAILED);
        }
      } else {
        log::debug("Security check succeeded state:{} port_handle:{}",
                   rfcomm_port_state_text(static_cast<tRFC_PORT_STATE>(p_port->rfc.state)),
                   p_port->handle);
        PORT_DlcEstablishInd(p_port->rfc.p_mcb, p_port->dlci, p_port->rfc.p_mcb->peer_l2cap_mtu);
      }
      return;

    case RFC_PORT_EVENT_OPEN:
    case RFC_PORT_EVENT_CLOSE:
      log::error("Port error event {}", rfcomm_port_event_text(event));
      return;

    case RFC_PORT_EVENT_CLEAR:
      log::warn("RFC_PORT_EVENT_CLEAR, handle:{}", p_port->handle);
      btm_sec_abort_access_req(p_port->rfc.p_mcb->bd_addr);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_DATA:
      log::error("Port error event {}", rfcomm_port_event_text(event));
      osi_free(p_data);
      return;

    case RFC_PORT_EVENT_SABME:
      /* Ignore SABME retransmission if client dares to do so */
      return;

    case RFC_PORT_EVENT_DISC:
      btm_sec_abort_access_req(p_port->rfc.p_mcb->bd_addr);
      p_port->rfc.state = RFC_STATE_CLOSED;
      rfc_send_ua(p_port->rfc.p_mcb, p_port->dlci);

      PORT_DlcReleaseInd(p_port->rfc.p_mcb, p_port->dlci);
      return;

    case RFC_PORT_EVENT_UIH:
      osi_free(p_data);
      return;

    case RFC_PORT_EVENT_ESTABLISH_RSP:
      if (*((uint8_t*)p_data) != RFCOMM_SUCCESS) {
        if (p_port->rfc.p_mcb) {
          rfc_send_dm(p_port->rfc.p_mcb, p_port->dlci, true);
        }
      } else {
        rfc_send_ua(p_port->rfc.p_mcb, p_port->dlci);
        p_port->rfc.state = RFC_STATE_OPENED;

        if (uuid_logging_acceptlist.find(p_port->uuid) != uuid_logging_acceptlist.end()) {
          // Find Channel Control Block by Channel ID
          tL2C_CCB* p_ccb = l2cu_find_ccb_by_cid(nullptr, p_port->rfc.p_mcb->lcid);
          if (p_ccb) {
            bluetooth::shim::GetSnoopLogger()->AcceptlistRfcommDlci(
                    p_ccb->p_lcb->Handle(), p_port->rfc.p_mcb->lcid, p_port->dlci);
          }
        }
        if (p_port->rfc.p_mcb) {
          uint16_t lcid;
          tL2C_CCB* ccb;

          lcid = p_port->rfc.p_mcb->lcid;
          ccb = l2cu_find_ccb_by_cid(nullptr, lcid);

          if (ccb) {
            bluetooth::shim::GetSnoopLogger()->SetRfcommPortOpen(
                    ccb->p_lcb->Handle(), lcid, p_port->dlci, p_port->uuid,
                    p_port->rfc.p_mcb->flow == PORT_FC_CREDIT);
          }
        }
      }
      return;
    default:
      log::error("Received unexpected event:{} in state:{}", rfcomm_port_event_text(event),
                 rfcomm_port_state_text(p_port->rfc.state));
  }
  log::warn("Event ignored {}", event);
}

/*******************************************************************************
 *
 * Function         rfc_port_sm_orig_wait_sec_check
 *
 * Description      This function handles events for the port in the
 *                  ORIG_WAIT_SEC_CHECK state.  RFCOMM is waiting for Security
 *                  manager to finish before sending SABME to the peer
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_sm_orig_wait_sec_check(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data) {
  switch (event) {
    case RFC_PORT_EVENT_SEC_COMPLETE:
      if (*((tBTM_STATUS*)p_data) != tBTM_STATUS::BTM_SUCCESS) {
        log::error("Security check failed result:{} state:{} handle:{}",
                   btm_status_text(*((tBTM_STATUS*)p_data)),
                   rfcomm_port_state_text(p_port->rfc.state), p_port->handle);
        p_port->rfc.p_mcb->is_disc_initiator = true;
        PORT_DlcEstablishCnf(p_port->rfc.p_mcb, p_port->dlci, 0, RFCOMM_SECURITY_ERR);
        rfc_port_closed(p_port);
      } else {
        log::debug("Security check succeeded state:{} handle:{}",
                   rfcomm_port_state_text(p_port->rfc.state), p_port->handle);
        rfc_send_sabme(p_port->rfc.p_mcb, p_port->dlci);
        rfc_port_timer_start(p_port, RFC_PORT_T1_TIMEOUT);
        p_port->rfc.state = RFC_STATE_SABME_WAIT_UA;
      }
      return;

    case RFC_PORT_EVENT_OPEN:
    case RFC_PORT_EVENT_SABME: /* Peer should not use the same dlci */
      log::error("Port error event {}", rfcomm_port_event_text(event));
      return;

    case RFC_PORT_EVENT_CLOSE:
      log::warn("RFC_PORT_EVENT_CLOSE, handle:{}", p_port->handle);
      btm_sec_abort_access_req(p_port->rfc.p_mcb->bd_addr);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_DATA:
      log::error("Port error {}", rfcomm_port_event_text(event));
      osi_free(p_data);
      return;

    case RFC_PORT_EVENT_UIH:
      osi_free(p_data);
      return;
    default:
      log::error("Received unexpected event:{} in state:{}", rfcomm_port_event_text(event),
                 rfcomm_port_state_text(p_port->rfc.state));
  }
  log::warn("Event ignored {}", rfcomm_port_event_text(event));
}

/*******************************************************************************
 *
 * Function         rfc_port_sm_opened
 *
 * Description      This function handles events for the port in the OPENED
 *                  state
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_sm_opened(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data) {
  switch (event) {
    case RFC_PORT_EVENT_OPEN:
      log::error("RFC_PORT_EVENT_OPEN bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr,
                 p_port->handle, p_port->dlci, p_port->scn);
      return;

    case RFC_PORT_EVENT_CLOSE:
      log::info("RFC_PORT_EVENT_CLOSE bd_addr:{}, handle:{} dlci:{} scn:{}", p_port->bd_addr,
                p_port->handle, p_port->dlci, p_port->scn);
      rfc_port_timer_start(p_port, RFC_DISC_TIMEOUT);
      rfc_send_disc(p_port->rfc.p_mcb, p_port->dlci);
      p_port->rfc.expected_rsp = 0;
      p_port->rfc.state = RFC_STATE_DISC_WAIT_UA;
      return;

    case RFC_PORT_EVENT_CLEAR:
      log::warn("RFC_PORT_EVENT_CLEAR bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr,
                p_port->handle, p_port->dlci, p_port->scn);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_DATA:
      // Send credits in the frame.  Pass them in the layer specific member of the hdr.
      // There might be an initial case when we reduced rx_max and credit_rx is still bigger.
      // Make sure that we do not send 255
      log::verbose("RFC_PORT_EVENT_DATA bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr,
                   p_port->handle, p_port->dlci, p_port->scn);
      if ((p_port->rfc.p_mcb->flow == PORT_FC_CREDIT) &&
          (((BT_HDR*)p_data)->len < p_port->peer_mtu) && (!p_port->rx.user_fc) &&
          (p_port->credit_rx_max > p_port->credit_rx)) {
        ((BT_HDR*)p_data)->layer_specific = (uint8_t)(p_port->credit_rx_max - p_port->credit_rx);
        p_port->credit_rx = p_port->credit_rx_max;
      } else {
        ((BT_HDR*)p_data)->layer_specific = 0;
      }
      rfc_send_buf_uih(p_port->rfc.p_mcb, p_port->dlci, (BT_HDR*)p_data);
      rfc_dec_credit(p_port);
      return;

    case RFC_PORT_EVENT_UA:
      log::verbose("RFC_PORT_EVENT_UA bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr,
                   p_port->handle, p_port->dlci, p_port->scn);
      return;

    case RFC_PORT_EVENT_SABME:
      log::verbose("RFC_PORT_EVENT_SABME bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr,
                   p_port->handle, p_port->dlci, p_port->scn);
      rfc_send_ua(p_port->rfc.p_mcb, p_port->dlci);
      return;

    case RFC_PORT_EVENT_DM:
      log::info("RFC_EVENT_DM bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr, p_port->handle,
                p_port->dlci, p_port->scn);
      PORT_DlcReleaseInd(p_port->rfc.p_mcb, p_port->dlci);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_DISC:
      log::info("RFC_PORT_EVENT_DISC bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr,
                p_port->handle, p_port->dlci, p_port->scn);
      p_port->rfc.state = RFC_STATE_CLOSED;
      rfc_send_ua(p_port->rfc.p_mcb, p_port->dlci);
      if (!fixed_queue_is_empty(p_port->rx.queue)) {
        /* give a chance to upper stack to close port properly */
        log::verbose("port queue is not empty");
        rfc_port_timer_start(p_port, RFC_DISC_TIMEOUT);
      } else {
        PORT_DlcReleaseInd(p_port->rfc.p_mcb, p_port->dlci);
      }
      return;

    case RFC_PORT_EVENT_UIH:
      log::verbose("RFC_PORT_EVENT_UIH bd_addr:{}, handle:{} dlci:{} scn:{}", p_port->bd_addr,
                   p_port->handle, p_port->dlci, p_port->scn);
      rfc_port_uplink_data(p_port, (BT_HDR*)p_data);
      return;

    case RFC_PORT_EVENT_TIMEOUT:
      PORT_TimeOutCloseMux(p_port->rfc.p_mcb);
      log::error("RFC_PORT_EVENT_TIMEOUT bd_addr:{} handle:{} dlci:{} scn:{}", p_port->bd_addr,
                 p_port->handle, p_port->dlci, p_port->scn);
      return;

    default:
      log::error("Received unexpected event:{} bd_addr:{} handle:{} dlci:{} scn:{}",
                 rfcomm_port_event_text(event), p_port->bd_addr, p_port->handle, p_port->dlci,
                 p_port->scn);
      break;
  }
  log::warn("Event ignored {}", rfcomm_port_event_text(event));
}

/*******************************************************************************
 *
 * Function         rfc_port_sm_disc_wait_ua
 *
 * Description      This function handles events when DISC on the DLC was
 *                  sent and SM is waiting for UA or DM.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_sm_disc_wait_ua(tPORT* p_port, tRFC_PORT_EVENT event, void* p_data) {
  switch (event) {
    case RFC_PORT_EVENT_OPEN:
    case RFC_PORT_EVENT_ESTABLISH_RSP:
      log::error("Port error event {}", rfcomm_port_event_text(event));
      return;

    case RFC_PORT_EVENT_CLEAR:
      log::warn("RFC_PORT_EVENT_CLEAR, handle:{}", p_port->handle);
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_DATA:
      osi_free(p_data);
      return;

    case RFC_PORT_EVENT_UA:
      p_port->rfc.p_mcb->is_disc_initiator = true;
      FALLTHROUGH_INTENDED; /* FALLTHROUGH */

    case RFC_PORT_EVENT_DM:
      log::warn("RFC_EVENT_DM|RFC_EVENT_UA[{}], handle:{}", event, p_port->handle);
      if (com::android::bluetooth::flags::rfcomm_always_disc_initiator_in_disc_wait_ua()) {
        // If we got a DM in RFC_STATE_DISC_WAIT_UA, it's likely that both ends
        // attempt to DISC at the same time and both get a DM.
        // Without setting this flag the both ends would start the same timers,
        // wait, and still DISC the multiplexer at the same time eventually.
        // The wait is meaningless and would block all other services that rely
        // on RFCOMM such as HFP.
        // Thus, setting this flag here to save us a timeout and doesn't
        // introduce further RFCOMM event changes.
        p_port->rfc.p_mcb->is_disc_initiator = true;
      }
      rfc_port_closed(p_port);
      return;

    case RFC_PORT_EVENT_SABME:
      rfc_send_dm(p_port->rfc.p_mcb, p_port->dlci, true);
      return;

    case RFC_PORT_EVENT_DISC:
      rfc_send_dm(p_port->rfc.p_mcb, p_port->dlci, true);
      return;

    case RFC_PORT_EVENT_UIH:
      osi_free(p_data);
      rfc_send_dm(p_port->rfc.p_mcb, p_port->dlci, false);
      return;

    case RFC_PORT_EVENT_TIMEOUT:
      log::error("RFC_EVENT_TIMEOUT, handle:{}", p_port->handle);
      rfc_port_closed(p_port);
      return;
    default:
      log::error("Received unexpected event:{} in state:{}", rfcomm_port_event_text(event),
                 rfcomm_port_state_text(p_port->rfc.state));
  }

  log::warn("Event ignored {}", rfcomm_port_event_text(event));
}

/*******************************************************************************
 *
 * Function         rfc_port_uplink_data
 *
 * Description      This function handles uplink information data frame.
 *
 ******************************************************************************/
void rfc_port_uplink_data(tPORT* p_port, BT_HDR* p_buf) {
  PORT_DataInd(p_port->rfc.p_mcb, p_port->dlci, p_buf);
}

/*******************************************************************************
 *
 * Function         rfc_process_pn
 *
 * Description      This function handles DLC parameter negotiation frame.
 *                  Record MTU and pass indication to the upper layer.
 *
 ******************************************************************************/
void rfc_process_pn(tRFC_MCB* p_mcb, bool is_command, MX_FRAME* p_frame) {
  log::verbose("is_initiator={}, is_cmd={}, state={}, bd_addr={}", p_mcb->is_initiator, is_command,
               p_mcb->state, p_mcb->bd_addr);
  uint8_t dlci = p_frame->dlci;

  if (is_command) {
    /* Ignore if Multiplexer is being shut down */
    if (p_mcb->state != RFC_MX_STATE_DISC_WAIT_UA) {
      PORT_ParNegInd(p_mcb, dlci, p_frame->u.pn.mtu, p_frame->u.pn.conv_layer, p_frame->u.pn.k);
    } else {
      log::warn("MX PN while disconnecting, bd_addr={}, p_mcb={}", p_mcb->bd_addr,
                std::format_ptr(p_mcb));
      rfc_send_dm(p_mcb, dlci, false);
    }

    return;
  }
  /* If we are not awaiting response just ignore it */
  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if ((p_port == nullptr) || !(p_port->rfc.expected_rsp & RFC_RSP_PN)) {
    log::warn(": Ignore unwanted response, p_mcb={}, bd_addr={}, dlci={}", std::format_ptr(p_mcb),
              p_mcb->bd_addr, dlci);
    return;
  }

  p_port->rfc.expected_rsp &= ~RFC_RSP_PN;

  rfc_port_timer_stop(p_port);

  PORT_ParNegCnf(p_mcb, dlci, p_frame->u.pn.mtu, p_frame->u.pn.conv_layer, p_frame->u.pn.k);
}

/*******************************************************************************
 *
 * Function         rfc_process_rpn
 *
 * Description      This function handles Remote DLC parameter negotiation
 *                  command/response.  Pass command to the user.
 *
 ******************************************************************************/
void rfc_process_rpn(tRFC_MCB* p_mcb, bool is_command, bool is_request, MX_FRAME* p_frame) {
  PortSettings port_settings = {};
  tPORT* p_port;

  p_port = port_find_mcb_dlci_port(p_mcb, p_frame->dlci);
  if (p_port == nullptr) {
    /* This is the first command on the port */
    if (is_command) {
      rfc_set_port_settings(&port_settings, p_frame);

      PORT_PortNegInd(p_mcb, p_frame->dlci, &port_settings, p_frame->u.rpn.param_mask);
    }
    return;
  }

  if (is_command && is_request) {
    /* This is the special situation when peer just request local pars */
    rfc_send_rpn(p_mcb, p_frame->dlci, false, &p_port->peer_port_settings, 0);
    return;
  }

  port_settings = p_port->peer_port_settings;

  rfc_set_port_settings(&port_settings, p_frame);

  if (is_command) {
    PORT_PortNegInd(p_mcb, p_frame->dlci, &port_settings, p_frame->u.rpn.param_mask);
    return;
  }

  // If we are not awaiting response just ignore it
  p_port = port_find_mcb_dlci_port(p_mcb, p_frame->dlci);
  if ((p_port == nullptr) || !(p_port->rfc.expected_rsp & (RFC_RSP_RPN | RFC_RSP_RPN_REPLY))) {
    log::warn("ignore DLC parameter negotiation as we are not waiting for any");
    return;
  }

  // If we sent a request for port parameters to the peer it is replying with mask 0.
  rfc_port_timer_stop(p_port);

  if (p_port->rfc.expected_rsp & RFC_RSP_RPN_REPLY) {
    p_port->rfc.expected_rsp &= ~RFC_RSP_RPN_REPLY;

    p_port->peer_port_settings = port_settings;

    if ((port_settings.fc_type == (RFCOMM_FC_RTR_ON_INPUT | RFCOMM_FC_RTR_ON_OUTPUT)) ||
        (port_settings.fc_type == (RFCOMM_FC_RTC_ON_INPUT | RFCOMM_FC_RTC_ON_OUTPUT))) {
      /* This is satisfactory port parameters.  Set mask as it was Ok */
      p_frame->u.rpn.param_mask = RFCOMM_RPN_PM_MASK;
    } else {
      /* Current peer parameters are not good, try to fix them */
      p_port->peer_port_settings.fc_type = (RFCOMM_FC_RTR_ON_INPUT | RFCOMM_FC_RTR_ON_OUTPUT);

      p_port->rfc.expected_rsp |= RFC_RSP_RPN;
      rfc_send_rpn(p_mcb, p_frame->dlci, true, &p_port->peer_port_settings,
                   RFCOMM_RPN_PM_RTR_ON_INPUT | RFCOMM_RPN_PM_RTR_ON_OUTPUT);
      rfc_port_timer_start(p_port, RFC_T2_TIMEOUT);
      return;
    }
  } else {
    p_port->rfc.expected_rsp &= ~RFC_RSP_RPN;
  }

  /* Check if all suggested parameters were accepted */
  if (((p_frame->u.rpn.param_mask & (RFCOMM_RPN_PM_RTR_ON_INPUT | RFCOMM_RPN_PM_RTR_ON_OUTPUT)) ==
       (RFCOMM_RPN_PM_RTR_ON_INPUT | RFCOMM_RPN_PM_RTR_ON_OUTPUT)) ||
      ((p_frame->u.rpn.param_mask & (RFCOMM_RPN_PM_RTC_ON_INPUT | RFCOMM_RPN_PM_RTC_ON_OUTPUT)) ==
       (RFCOMM_RPN_PM_RTC_ON_INPUT | RFCOMM_RPN_PM_RTC_ON_OUTPUT))) {
    PORT_PortNegCnf(p_mcb, p_port->dlci, &port_settings, RFCOMM_SUCCESS);
    return;
  }

  /* If we were proposing RTR flow control try RTC flow control */
  /* If we were proposing RTC flow control try no flow control */
  /* otherwise drop the connection */
  if (p_port->peer_port_settings.fc_type == (RFCOMM_FC_RTR_ON_INPUT | RFCOMM_FC_RTR_ON_OUTPUT)) {
    /* Current peer parameters are not good, try to fix them */
    p_port->peer_port_settings.fc_type = (RFCOMM_FC_RTC_ON_INPUT | RFCOMM_FC_RTC_ON_OUTPUT);

    p_port->rfc.expected_rsp |= RFC_RSP_RPN;

    rfc_send_rpn(p_mcb, p_frame->dlci, true, &p_port->peer_port_settings,
                 RFCOMM_RPN_PM_RTC_ON_INPUT | RFCOMM_RPN_PM_RTC_ON_OUTPUT);
    rfc_port_timer_start(p_port, RFC_T2_TIMEOUT);
    return;
  }

  /* Other side does not support flow control */
  if (p_port->peer_port_settings.fc_type == (RFCOMM_FC_RTC_ON_INPUT | RFCOMM_FC_RTC_ON_OUTPUT)) {
    p_port->peer_port_settings.fc_type = RFCOMM_FC_OFF;
    PORT_PortNegCnf(p_mcb, p_port->dlci, &port_settings, RFCOMM_SUCCESS);
  }
}

/*******************************************************************************
 *
 * Function         rfc_process_msc
 *
 * Description      This function handles Modem Status Command.
 *                  Pass command to the user.
 *
 ******************************************************************************/
void rfc_process_msc(tRFC_MCB* p_mcb, bool is_command, MX_FRAME* p_frame) {
  tPORT_CTRL pars;
  tPORT* p_port;
  uint8_t modem_signals = p_frame->u.msc.signals;
  bool new_peer_fc = false;

  p_port = port_find_mcb_dlci_port(p_mcb, p_frame->dlci);
  if (p_port == NULL) {
    return;
  }

  pars.modem_signal = 0;

  if (modem_signals & RFCOMM_MSC_RTC) {
    pars.modem_signal |= MODEM_SIGNAL_DTRDSR;
  }

  if (modem_signals & RFCOMM_MSC_RTR) {
    pars.modem_signal |= MODEM_SIGNAL_RTSCTS;
  }

  if (modem_signals & RFCOMM_MSC_IC) {
    pars.modem_signal |= MODEM_SIGNAL_RI;
  }

  if (modem_signals & RFCOMM_MSC_DV) {
    pars.modem_signal |= MODEM_SIGNAL_DCD;
  }

  pars.fc = ((modem_signals & RFCOMM_MSC_FC) == RFCOMM_MSC_FC);

  pars.break_signal = (p_frame->u.msc.break_present) ? p_frame->u.msc.break_duration : 0;
  pars.discard_buffers = 0;
  pars.break_signal_seq = RFCOMM_CTRL_BREAK_IN_SEQ; /* this is default */

  /* Check if this command is passed only to indicate flow control */
  if (is_command) {
    rfc_send_msc(p_mcb, p_frame->dlci, false, &pars);

    if (p_port->rfc.p_mcb->flow != PORT_FC_CREDIT) {
      /* Spec 1.1 indicates that only FC bit is used for flow control */
      p_port->peer_ctrl.fc = new_peer_fc = pars.fc;

      if (new_peer_fc != p_port->tx.peer_fc) {
        PORT_FlowInd(p_mcb, p_frame->dlci, (bool)!new_peer_fc);
      }
    }

    PORT_ControlInd(p_mcb, p_frame->dlci, &pars);

    return;
  }

  /* If we are not awaiting response just ignore it */
  if (!(p_port->rfc.expected_rsp & RFC_RSP_MSC)) {
    return;
  }

  p_port->rfc.expected_rsp &= ~RFC_RSP_MSC;

  rfc_port_timer_stop(p_port);

  PORT_ControlCnf(p_port->rfc.p_mcb, p_port->dlci, &pars);
}

/*******************************************************************************
 *
 * Function         rfc_process_rls
 *
 * Description      This function handles Remote Line Status command.
 *                  Pass command to the user.
 *
 ******************************************************************************/
void rfc_process_rls(tRFC_MCB* p_mcb, bool is_command, MX_FRAME* p_frame) {
  tPORT* p_port;

  if (is_command) {
    PORT_LineStatusInd(p_mcb, p_frame->dlci, p_frame->u.rls.line_status);
    rfc_send_rls(p_mcb, p_frame->dlci, false, p_frame->u.rls.line_status);
  } else {
    p_port = port_find_mcb_dlci_port(p_mcb, p_frame->dlci);

    /* If we are not awaiting response just ignore it */
    if (!p_port || !(p_port->rfc.expected_rsp & RFC_RSP_RLS)) {
      return;
    }

    p_port->rfc.expected_rsp &= ~RFC_RSP_RLS;

    rfc_port_timer_stop(p_port);
  }
}

/*******************************************************************************
 *
 * Function         rfc_process_nsc
 *
 * Description      This function handles None Supported Command frame.
 *
 ******************************************************************************/
void rfc_process_nsc(tRFC_MCB* /* p_mcb */, MX_FRAME* /* p_frame */) {}

/*******************************************************************************
 *
 * Function         rfc_process_test
 *
 * Description      This function handles Test frame.  If this is a command
 *                  reply to it.  Otherwise pass response to the user.
 *
 ******************************************************************************/
void rfc_process_test_rsp(tRFC_MCB* /* p_mcb */, BT_HDR* p_buf) { osi_free(p_buf); }

/*******************************************************************************
 *
 * Function         rfc_process_fcon
 *
 * Description      This function handles FCON frame.  The peer entity is able
 *                  to receive new information
 *
 ******************************************************************************/
void rfc_process_fcon(tRFC_MCB* p_mcb, bool is_command) {
  if (is_command) {
    rfc_cb.rfc.peer_rx_disabled = false;

    rfc_send_fcon(p_mcb, false);

    if (!p_mcb->l2cap_congested) {
      PORT_FlowInd(p_mcb, 0, true);
    }
  }
}

/*******************************************************************************
 *
 * Function         rfc_process_fcoff
 *
 * Description      This function handles FCOFF frame.  The peer entity is
 *                  unable to receive new information
 *
 ******************************************************************************/
void rfc_process_fcoff(tRFC_MCB* p_mcb, bool is_command) {
  if (is_command) {
    rfc_cb.rfc.peer_rx_disabled = true;

    if (!p_mcb->l2cap_congested) {
      PORT_FlowInd(p_mcb, 0, false);
    }

    rfc_send_fcoff(p_mcb, false);
  }
}

/*******************************************************************************
 *
 * Function         rfc_process_l2cap_congestion
 *
 * Description      This function handles L2CAP congestion messages
 *
 ******************************************************************************/
void rfc_process_l2cap_congestion(tRFC_MCB* p_mcb, bool is_congested) {
  p_mcb->l2cap_congested = is_congested;

  if (!is_congested) {
    rfc_check_send_cmd(p_mcb, nullptr);
  }

  if (!rfc_cb.rfc.peer_rx_disabled) {
    PORT_FlowInd(p_mcb, 0, !is_congested);
  }
}

/*******************************************************************************
 *
 * Function         rfc_set_port_settings
 *
 * Description      This function sets the PortSettings structure given a
 *                  p_frame.
 *
 ******************************************************************************/

void rfc_set_port_settings(PortSettings* port_settings, MX_FRAME* p_frame) {
  if (p_frame->u.rpn.param_mask & RFCOMM_RPN_PM_BIT_RATE) {
    port_settings->baud_rate = p_frame->u.rpn.baud_rate;
  }
  if (p_frame->u.rpn.param_mask & RFCOMM_RPN_PM_DATA_BITS) {
    port_settings->byte_size = p_frame->u.rpn.byte_size;
  }
  if (p_frame->u.rpn.param_mask & RFCOMM_RPN_PM_STOP_BITS) {
    port_settings->stop_bits = p_frame->u.rpn.stop_bits;
  }
  if (p_frame->u.rpn.param_mask & RFCOMM_RPN_PM_PARITY) {
    port_settings->parity = p_frame->u.rpn.parity;
  }
  if (p_frame->u.rpn.param_mask & RFCOMM_RPN_PM_PARITY_TYPE) {
    port_settings->parity_type = p_frame->u.rpn.parity_type;
  }
  if (p_frame->u.rpn.param_mask &
      (RFCOMM_RPN_PM_XONXOFF_ON_INPUT | RFCOMM_RPN_PM_XONXOFF_ON_OUTPUT |
       RFCOMM_RPN_PM_RTR_ON_INPUT | RFCOMM_RPN_PM_RTR_ON_OUTPUT | RFCOMM_RPN_PM_RTC_ON_INPUT |
       RFCOMM_RPN_PM_RTC_ON_OUTPUT)) {
    port_settings->fc_type = p_frame->u.rpn.fc_type;
  }
  if (p_frame->u.rpn.param_mask & RFCOMM_RPN_PM_XON_CHAR) {
    port_settings->xon_char = p_frame->u.rpn.xon_char;
  }
  if (p_frame->u.rpn.param_mask & RFCOMM_RPN_PM_XOFF_CHAR) {
    port_settings->xoff_char = p_frame->u.rpn.xoff_char;
  }
}
