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

/*****************************************************************************
 *
 *  This file contains collection of utility functions used the RFCOMM unit
 *
 *****************************************************************************/

#define LOG_TAG "rfcomm"

#include <bluetooth/log.h>

#include <cstdint>

#include "internal_include/bt_target.h"
#include "os/logging/log_adapter.h"
#include "osi/include/allocator.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/port_ext.h"
#include "stack/rfcomm/rfc_int.h"
#include "types/raw_address.h"

using namespace bluetooth;

/*******************************************************************************
 *
 * Function         rfc_calc_fcs
 *
 * Description      Reversed CRC Table , 8-bit, poly=0x07
 *                  (GSM 07.10 TS 101 369 V6.3.0)
 ******************************************************************************/
static const uint8_t rfc_crctable[] = {
        0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75, 0x0E, 0x9F, 0xED, 0x7C, 0x09,
        0x98, 0xEA, 0x7B, 0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69, 0x12, 0x83,
        0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67, 0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC,
        0x4D, 0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43, 0x24, 0xB5, 0xC7, 0x56,
        0x23, 0xB2, 0xC0, 0x51, 0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,

        0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05, 0x7E, 0xEF, 0x9D, 0x0C, 0x79,
        0xE8, 0x9A, 0x0B, 0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19, 0x62, 0xF3,
        0x81, 0x10, 0x65, 0xF4, 0x86, 0x17, 0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC,
        0x3D, 0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33, 0x54, 0xC5, 0xB7, 0x26,
        0x53, 0xC2, 0xB0, 0x21, 0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,

        0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95, 0xEE, 0x7F, 0x0D, 0x9C, 0xE9,
        0x78, 0x0A, 0x9B, 0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89, 0xF2, 0x63,
        0x11, 0x80, 0xF5, 0x64, 0x16, 0x87, 0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C,
        0xAD, 0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3, 0xC4, 0x55, 0x27, 0xB6,
        0xC3, 0x52, 0x20, 0xB1, 0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,

        0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5, 0x9E, 0x0F, 0x7D, 0xEC, 0x99,
        0x08, 0x7A, 0xEB, 0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9, 0x82, 0x13,
        0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7, 0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C,
        0xDD, 0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3, 0xB4, 0x25, 0x57, 0xC6,
        0xB3, 0x22, 0x50, 0xC1, 0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF};

/*******************************************************************************
 *
 * Function         rfc_calc_fcs
 *
 * Description      This function calculate FCS for the RFCOMM frame
 *                  (GSM 07.10 TS 101 369 V6.3.0)
 *
 * Input            len - number of bytes in the message
 *                  p   - points to message
 *
 ******************************************************************************/
uint8_t rfc_calc_fcs(uint16_t len, uint8_t* p) {
  uint8_t fcs = 0xFF;

  while (len--) {
    fcs = rfc_crctable[fcs ^ *p++];
  }

  /* Ones compliment */
  return 0xFF - fcs;
}

/*******************************************************************************
 *
 * Function         rfc_check_fcs
 *
 * Description      This function checks FCS for the RFCOMM frame
 *                  (GSM 07.10 TS 101 369 V6.3.0)
 *
 * Input            len          - number of bytes in the message
 *                  p            - points to message
 *                  received_fcs - received FCS
 *
 ******************************************************************************/
bool rfc_check_fcs(uint16_t len, uint8_t* p, uint8_t received_fcs) {
  uint8_t fcs = 0xFF;

  while (len--) {
    fcs = rfc_crctable[fcs ^ *p++];
  }

  /* Ones compliment */
  fcs = rfc_crctable[fcs ^ received_fcs];

  /*0xCF is the reversed order of 11110011.*/
  return fcs == 0xCF;
}

/*******************************************************************************
 *
 * Function         rfc_alloc_multiplexer_channel
 *
 * Description      This function returns existing or new control block for
 *                  the address.
 *
 ******************************************************************************/
tRFC_MCB* rfc_alloc_multiplexer_channel(const RawAddress& bd_addr, bool is_initiator) {
  int i, j;
  tRFC_MCB* p_mcb = NULL;
  log::verbose("bd_addr:{}, is_initiator:{}", bd_addr, is_initiator);

  for (i = 0; i < MAX_BD_CONNECTIONS; i++) {
    log::verbose("rfc_cb.port.rfc_mcb[{}] - state:{}, bd_addr:{}", i, rfc_cb.port.rfc_mcb[i].state,
                 rfc_cb.port.rfc_mcb[i].bd_addr);

    if ((rfc_cb.port.rfc_mcb[i].state != RFC_MX_STATE_IDLE) &&
        rfc_cb.port.rfc_mcb[i].bd_addr == bd_addr) {
      /* Multiplexer channel found do not change anything */
      /* If there was an inactivity timer running stop it now */
      if (rfc_cb.port.rfc_mcb[i].state == RFC_MX_STATE_CONNECTED) {
        rfc_timer_stop(&rfc_cb.port.rfc_mcb[i]);
      }
      log::verbose(
              "rfc_alloc_multiplexer_channel:is_initiator:{}, found, state:{}, "
              "p_mcb:{}",
              is_initiator, rfc_cb.port.rfc_mcb[i].state, std::format_ptr(&rfc_cb.port.rfc_mcb[i]));
      return &rfc_cb.port.rfc_mcb[i];
    }
  }

  /* connection with bd_addr does not exist */
  for (i = 0, j = rfc_cb.rfc.last_mux + 1; i < MAX_BD_CONNECTIONS; i++, j++) {
    if (j >= MAX_BD_CONNECTIONS) {
      j = 0;
    }

    p_mcb = &rfc_cb.port.rfc_mcb[j];
    if (rfc_cb.port.rfc_mcb[j].state == RFC_MX_STATE_IDLE) {
      /* New multiplexer control block */
      alarm_free(p_mcb->mcb_timer);
      fixed_queue_free(p_mcb->cmd_q, NULL);
      memset(p_mcb, 0, sizeof(tRFC_MCB));
      p_mcb->bd_addr = bd_addr;
      log::verbose(
              "rfc_alloc_multiplexer_channel:is_initiator:{}, create new p_mcb:{}, "
              "index:{}",
              is_initiator, std::format_ptr(&rfc_cb.port.rfc_mcb[j]), j);

      p_mcb->mcb_timer = alarm_new("rfcomm_mcb.mcb_timer");
      p_mcb->cmd_q = fixed_queue_new(SIZE_MAX);

      p_mcb->is_initiator = is_initiator;

      rfc_timer_start(p_mcb, RFC_MCB_INIT_INACT_TIMER);

      rfc_cb.rfc.last_mux = (uint8_t)j;
      return p_mcb;
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         rfc_release_multiplexer_channel
 *
 * Description      Release a multiplexer control block
 *
 ******************************************************************************/
void rfc_release_multiplexer_channel(tRFC_MCB* p_mcb) {
  /* Remove the MCB from the mapping table */
  rfc_save_lcid_mcb(NULL, p_mcb->lcid);

  /* Remove the MCB from the ports */
  for (int i = 0; i < MAX_RFC_PORTS; i++) {
    if (rfc_cb.port.port[i].rfc.p_mcb == p_mcb) {
      rfc_cb.port.port[i].rfc.p_mcb = NULL;
    }
  }

  rfc_timer_stop(p_mcb);
  alarm_free(p_mcb->mcb_timer);

  fixed_queue_free(p_mcb->cmd_q, osi_free);

  memset(p_mcb, 0, sizeof(tRFC_MCB));
  p_mcb->state = RFC_MX_STATE_IDLE;
}

/*******************************************************************************
 *
 * Function         rfc_timer_start
 *
 * Description      Start RFC Timer
 *
 ******************************************************************************/
void rfc_timer_start(tRFC_MCB* p_mcb, uint16_t timeout) {
  log::verbose("- timeout:{} seconds", timeout);

  uint64_t interval_ms = timeout * 1000;
  alarm_set_on_mloop(p_mcb->mcb_timer, interval_ms, rfcomm_mcb_timer_timeout, p_mcb);
}

/*******************************************************************************
 *
 * Function         rfc_timer_stop
 *
 * Description      Stop RFC Timer
 *
 ******************************************************************************/
void rfc_timer_stop(tRFC_MCB* p_mcb) {
  log::verbose("");

  alarm_cancel(p_mcb->mcb_timer);
}

/*******************************************************************************
 *
 * Function         rfc_port_timer_start
 *
 * Description      Start RFC Timer
 *
 ******************************************************************************/
void rfc_port_timer_start(tPORT* p_port, uint16_t timeout) {
  log::verbose("- timeout:{} seconds", timeout);

  uint64_t interval_ms = timeout * 1000;
  alarm_set_on_mloop(p_port->rfc.port_timer, interval_ms, rfcomm_port_timer_timeout, p_port);
}

/*******************************************************************************
 *
 * Function         rfc_port_timer_stop
 *
 * Description      Stop RFC Timer
 *
 ******************************************************************************/
void rfc_port_timer_stop(tPORT* p_port) {
  log::verbose("");

  alarm_cancel(p_port->rfc.port_timer);
}

/*******************************************************************************
 *
 * Function         rfc_check_mcb_active
 *
 * Description      Check if there are any opened ports on the MCB if not
 *                  start MCB Inact timer.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_check_mcb_active(tRFC_MCB* p_mcb) {
  uint16_t i;

  for (i = 0; i < RFCOMM_MAX_DLCI; i++) {
    if (p_mcb->port_handles[i] != 0) {
      p_mcb->is_disc_initiator = false;
      return;
    }
  }
  /* The last port was DISCed.  On the client side start disconnecting Mx */
  /* On the server side start inactivity timer */
  if (p_mcb->is_disc_initiator) {
    p_mcb->is_disc_initiator = false;
    rfc_mx_sm_execute(p_mcb, RFC_MX_EVENT_CLOSE_REQ, NULL);
  } else {
    rfc_timer_start(p_mcb, RFC_MCB_RELEASE_INACT_TIMER);
  }
}

void rfcomm_port_timer_timeout(void* data) {
  tPORT* p_port = (tPORT*)data;

  rfc_port_sm_execute(p_port, RFC_PORT_EVENT_TIMEOUT, NULL);
}

void rfcomm_mcb_timer_timeout(void* data) {
  tRFC_MCB* p_mcb = (tRFC_MCB*)data;

  rfc_mx_sm_execute(p_mcb, RFC_MX_EVENT_TIMEOUT, NULL);
}

/*******************************************************************************
 *
 * Function         rfc_sec_check_complete
 *
 * Description      The function called when Security Manager finishes
 *                  verification of the service side connection
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_sec_check_complete(RawAddress /* bd_addr */, tBT_TRANSPORT /* transport */,
                            void* p_ref_data, tBTM_STATUS res) {
  log::assert_that(p_ref_data != nullptr, "assert failed: p_ref_data != nullptr");
  tPORT* p_port = (tPORT*)p_ref_data;

  /* Verify that PORT is still waiting for Security to complete */
  if (!p_port->in_use || ((p_port->rfc.state != RFC_STATE_ORIG_WAIT_SEC_CHECK) &&
                          (p_port->rfc.state != RFC_STATE_TERM_WAIT_SEC_CHECK))) {
    return;
  }

  rfc_port_sm_execute((tPORT*)p_ref_data, RFC_PORT_EVENT_SEC_COMPLETE, &res);
}

/*******************************************************************************
 *
 * Function         rfc_port_closed
 *
 * Description      The function is called when port is released based on the
 *                  event received from the lower layer, typically L2CAP
 *                  connection down, DISC, or DM frame.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_port_closed(tPORT* p_port) {
  tRFC_MCB* p_mcb = p_port->rfc.p_mcb;
  rfc_port_timer_stop(p_port);
  p_port->rfc.state = RFC_STATE_CLOSED;

  /* If multiplexer channel was up mark it as down */
  if (p_mcb) {
    p_mcb->port_handles[p_port->dlci] = 0;

    /* If there are no more ports opened on this MCB release it */
    rfc_check_mcb_active(p_mcb);
  }

  /* Notify port that RFC connection is gone */
  port_rfc_closed(p_port, PORT_CLOSED);
}

/*******************************************************************************
 *
 * Function         rfc_inc_credit
 *
 * Description      The function is called when a credit is received in a UIH
 *                  frame.  It increments the TX credit count, and if data
 *                  flow had halted, it restarts it.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_inc_credit(tPORT* p_port, uint8_t credit) {
  if (p_port->rfc.p_mcb->flow == PORT_FC_CREDIT) {
    p_port->credit_tx += credit;

    log::verbose("rfc_inc_credit:{}", p_port->credit_tx);

    if (p_port->tx.peer_fc) {
      PORT_FlowInd(p_port->rfc.p_mcb, p_port->dlci, true);
    }
  }
}

/*******************************************************************************
 *
 * Function         rfc_dec_credit
 *
 * Description      The function is called when a UIH frame of user data is
 *                  sent.  It decrements the credit count.  If credit count
 *                  Reaches zero, peer_fc is set.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_dec_credit(tPORT* p_port) {
  if (p_port->rfc.p_mcb->flow == PORT_FC_CREDIT) {
    if (p_port->credit_tx > 0) {
      p_port->credit_tx--;
    }

    if (p_port->credit_tx == 0) {
      p_port->tx.peer_fc = true;
    }
  }
}

/*******************************************************************************
 *
 * Function         rfc_check_send_cmd
 *
 * Description      This function is called to send an RFCOMM command message
 *                  or to handle the RFCOMM command message queue.
 *
 * Returns          void
 *
 ******************************************************************************/
void rfc_check_send_cmd(tRFC_MCB* p_mcb, BT_HDR* p_buf) {
  /* if passed a buffer queue it */
  if (p_buf != NULL) {
    if (p_mcb->cmd_q == NULL) {
      log::error("empty queue: p_mcb = {} p_mcb->lcid = {} cached p_mcb = {}",
                 std::format_ptr(p_mcb), p_mcb->lcid,
                 std::format_ptr(rfc_find_lcid_mcb(p_mcb->lcid)));
    }
    fixed_queue_enqueue(p_mcb->cmd_q, p_buf);
  }

  /* handle queue if L2CAP not congested */
  while (!p_mcb->l2cap_congested) {
    BT_HDR* p = (BT_HDR*)fixed_queue_try_dequeue(p_mcb->cmd_q);
    if (p == NULL) {
      break;
    }
    uint16_t len = p->len;
    if (stack::l2cap::get_interface().L2CA_DataWrite(p_mcb->lcid, p) != tL2CAP_DW_RESULT::SUCCESS) {
      log::warn("Unable to write L2CAP data peer:{} cid:{} len:{}", p_mcb->bd_addr, p_mcb->lcid,
                len);
    }
  }
}
