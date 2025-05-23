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
 *  This module contains the link control state machine and functions which
 *  operate on the link control block.
 *
 ******************************************************************************/

#include <bluetooth/log.h>
#include <string.h>

#include <cstdint>
#include <string>

#include "avct_api.h"
#include "avct_int.h"
#include "device/include/device_iot_conf_defs.h"
#include "device/include/device_iot_config.h"
#include "include/macros.h"
#include "internal_include/bt_target.h"
#include "l2cap_types.h"
#include "osi/include/allocator.h"
#include "osi/include/fixed_queue.h"
#include "types/raw_address.h"

using namespace bluetooth;

/*****************************************************************************
 * state machine constants and types
 ****************************************************************************/

/* verbose state strings for trace */
const char* const avct_lcb_st_str[] = {"LCB_IDLE_ST", "LCB_OPENING_ST", "LCB_OPEN_ST",
                                       "LCB_CLOSING_ST"};

/* verbose event strings for trace */
const char* const avct_lcb_evt_str[] = {"UL_BIND_EVT",   "UL_UNBIND_EVT", "UL_MSG_EVT",
                                        "INT_CLOSE_EVT", "LL_OPEN_EVT",   "LL_CLOSE_EVT",
                                        "LL_MSG_EVT",    "LL_CONG_EVT"};

/* lcb state machine states */
enum { AVCT_LCB_IDLE_ST, AVCT_LCB_OPENING_ST, AVCT_LCB_OPEN_ST, AVCT_LCB_CLOSING_ST };

std::string avct_sm_state_text(const int& state) {
  switch (state) {
    CASE_RETURN_STRING(AVCT_LCB_IDLE_ST);
    CASE_RETURN_STRING(AVCT_LCB_OPENING_ST);
    CASE_RETURN_STRING(AVCT_LCB_OPEN_ST);
    CASE_RETURN_STRING(AVCT_LCB_CLOSING_ST);
  }
  RETURN_UNKNOWN_TYPE_STRING(int, state);
}

/* state machine action enumeration list */
enum {
  AVCT_LCB_CHNL_OPEN,
  AVCT_LCB_CHNL_DISC,
  AVCT_LCB_SEND_MSG,
  AVCT_LCB_OPEN_IND,
  AVCT_LCB_OPEN_FAIL,
  AVCT_LCB_CLOSE_IND,
  AVCT_LCB_CLOSE_CFM,
  AVCT_LCB_MSG_IND,
  AVCT_LCB_CONG_IND,
  AVCT_LCB_BIND_CONN,
  AVCT_LCB_BIND_FAIL,
  AVCT_LCB_UNBIND_DISC,
  AVCT_LCB_CHK_DISC,
  AVCT_LCB_DISCARD_MSG,
  AVCT_LCB_DEALLOC,
  AVCT_LCB_FREE_MSG_IND,
  AVCT_LCB_NUM_ACTIONS
};

#define AVCT_LCB_IGNORE AVCT_LCB_NUM_ACTIONS

/* type for action functions */
typedef void (*tAVCT_LCB_ACTION)(tAVCT_LCB* p_ccb, tAVCT_LCB_EVT* p_data);

/* action function list */
const tAVCT_LCB_ACTION avct_lcb_action[] = {
        avct_lcb_chnl_open,    // AVCT_LCB_CHNL_OPEN
        avct_lcb_chnl_disc,    // AVCT_LCB_CHNL_DISC
        avct_lcb_send_msg,     // AVCT_LCB_SEND_MSG
        avct_lcb_open_ind,     // AVCT_LCB_OPEN_IND
        avct_lcb_open_fail,    // AVCT_LCB_OPEN_FAIL
        avct_lcb_close_ind,    // AVCT_LCB_CLOSE_IND
        avct_lcb_close_cfm,    // AVCT_LCB_CLOSE_CFM
        avct_lcb_msg_ind,      // AVCT_LCB_MSG_IND
        avct_lcb_cong_ind,     // AVCT_LCB_CONG_IND
        avct_lcb_bind_conn,    // AVCT_LCB_BIND_CONN
        avct_lcb_bind_fail,    // AVCT_LCB_BIND_FAIL
        avct_lcb_unbind_disc,  // AVCT_LCB_UNBIND_DISC
        avct_lcb_chk_disc,     // AVCT_LCB_CHK_DISC
        avct_lcb_discard_msg,  // AVCT_LCB_DISCARD_MSG
        avct_lcb_dealloc,      // AVCT_LCB_DEALLOC
        avct_lcb_free_msg_ind  // AVCT_LCB_FREE_MSG_IND
};

/* state table information */
#define AVCT_LCB_ACTIONS 2    /* number of actions */
#define AVCT_LCB_NEXT_STATE 2 /* position of next state */
#define AVCT_LCB_NUM_COLS 3   /* number of columns in state tables */

/* state table for idle state */
const uint8_t avct_lcb_st_idle[][AVCT_LCB_NUM_COLS] = {
        /* Event        Action 1                Action 2             Next state */
        /* UL_BIND */ {AVCT_LCB_CHNL_OPEN, AVCT_LCB_IGNORE, AVCT_LCB_OPENING_ST},
        /* UL_UNBIND */ {AVCT_LCB_UNBIND_DISC, AVCT_LCB_IGNORE, AVCT_LCB_IDLE_ST},
        /* UL_MSG */ {AVCT_LCB_DISCARD_MSG, AVCT_LCB_IGNORE, AVCT_LCB_IDLE_ST},
        /* INT_CLOSE */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_IDLE_ST},
        /* LL_OPEN */ {AVCT_LCB_OPEN_IND, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST},
        /* LL_CLOSE */ {AVCT_LCB_CLOSE_IND, AVCT_LCB_DEALLOC, AVCT_LCB_IDLE_ST},
        /* LL_MSG */ {AVCT_LCB_FREE_MSG_IND, AVCT_LCB_IGNORE, AVCT_LCB_IDLE_ST},
        /* LL_CONG */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_IDLE_ST}};

/* state table for opening state */
const uint8_t avct_lcb_st_opening[][AVCT_LCB_NUM_COLS] = {
        /* Event        Action 1                Action 2             Next state */
        /* UL_BIND */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_OPENING_ST},
        /* UL_UNBIND */ {AVCT_LCB_UNBIND_DISC, AVCT_LCB_IGNORE, AVCT_LCB_OPENING_ST},
        /* UL_MSG */ {AVCT_LCB_DISCARD_MSG, AVCT_LCB_IGNORE, AVCT_LCB_OPENING_ST},
        /* INT_CLOSE */ {AVCT_LCB_CHNL_DISC, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* LL_OPEN */ {AVCT_LCB_OPEN_IND, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST},
        /* LL_CLOSE */ {AVCT_LCB_OPEN_FAIL, AVCT_LCB_DEALLOC, AVCT_LCB_IDLE_ST},
        /* LL_MSG */ {AVCT_LCB_FREE_MSG_IND, AVCT_LCB_IGNORE, AVCT_LCB_OPENING_ST},
        /* LL_CONG */ {AVCT_LCB_CONG_IND, AVCT_LCB_IGNORE, AVCT_LCB_OPENING_ST}};

/* state table for open state */
const uint8_t avct_lcb_st_open[][AVCT_LCB_NUM_COLS] = {
        /* Event         Action 1             Action 2             Next state */
        /* UL_BIND */ {AVCT_LCB_BIND_CONN, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST},
        /* UL_UNBIND */ {AVCT_LCB_CHK_DISC, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST},
        /* UL_MSG */ {AVCT_LCB_SEND_MSG, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST},
        /* INT_CLOSE */ {AVCT_LCB_CHNL_DISC, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* LL_OPEN */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST},
        /* LL_CLOSE */ {AVCT_LCB_CLOSE_IND, AVCT_LCB_DEALLOC, AVCT_LCB_IDLE_ST},
        /* LL_MSG */ {AVCT_LCB_MSG_IND, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST},
        /* LL_CONG */ {AVCT_LCB_CONG_IND, AVCT_LCB_IGNORE, AVCT_LCB_OPEN_ST}};

/* state table for closing state */
const uint8_t avct_lcb_st_closing[][AVCT_LCB_NUM_COLS] = {
        /* Event         Action 1               Action 2          Next state */
        /* UL_BIND */ {AVCT_LCB_BIND_FAIL, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* UL_UNBIND */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* UL_MSG */ {AVCT_LCB_DISCARD_MSG, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* INT_CLOSE */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* LL_OPEN */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* LL_CLOSE */ {AVCT_LCB_CLOSE_CFM, AVCT_LCB_DEALLOC, AVCT_LCB_IDLE_ST},
        /* LL_MSG */ {AVCT_LCB_FREE_MSG_IND, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST},
        /* LL_CONG */ {AVCT_LCB_IGNORE, AVCT_LCB_IGNORE, AVCT_LCB_CLOSING_ST}};

/* type for state table */
typedef const uint8_t (*tAVCT_LCB_ST_TBL)[AVCT_LCB_NUM_COLS];

/* state table */
const tAVCT_LCB_ST_TBL avct_lcb_st_tbl[] = {avct_lcb_st_idle, avct_lcb_st_opening, avct_lcb_st_open,
                                            avct_lcb_st_closing};

/*******************************************************************************
 *
 * Function         avct_lcb_event
 *
 * Description      State machine event handling function for lcb
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avct_lcb_event(tAVCT_LCB* p_lcb, uint8_t event, tAVCT_LCB_EVT* p_data) {
  tAVCT_LCB_ST_TBL state_table;
  uint8_t action;
  int i;

  log::verbose("LCB lcb_allocated={} event={} state={}", p_lcb->allocated, avct_lcb_evt_str[event],
               avct_lcb_st_str[p_lcb->state]);

  /* look up the state table for the current state */
  state_table = avct_lcb_st_tbl[p_lcb->state];

  if (p_lcb->state == AVCT_LCB_IDLE_ST && event == AVCT_LCB_LL_OPEN_EVT) {
    DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(p_lcb->peer_addr, IOT_CONF_KEY_AVRCP_CONN_COUNT);
  }

  /* set next state */
  p_lcb->state = state_table[event][AVCT_LCB_NEXT_STATE];

  /* execute action functions */
  for (i = 0; i < AVCT_LCB_ACTIONS; i++) {
    action = state_table[event][i];
    if (action != AVCT_LCB_IGNORE) {
      (*avct_lcb_action[action])(p_lcb, p_data);
    } else {
      break;
    }
  }
}

/*******************************************************************************
 *
 * Function         avct_bcb_event
 *
 * Description      State machine event handling function for lcb
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avct_bcb_event(tAVCT_BCB* p_bcb, uint8_t event, tAVCT_LCB_EVT* p_data) {
  log::info("BCB bcb_allocated={} event={} state={}", p_bcb->allocated, avct_lcb_evt_str[event],
            avct_lcb_st_str[p_bcb->state]);

  /* look up the state table for the current state */
  tAVCT_LCB_ST_TBL state_table = avct_lcb_st_tbl[p_bcb->state];

  /* set next state */
  p_bcb->state = state_table[event][AVCT_LCB_NEXT_STATE];

  /* execute action functions */
  for (int i = 0; i < AVCT_LCB_ACTIONS; i++) {
    uint8_t action = state_table[event][i];
    if (action != AVCT_LCB_IGNORE) {
      (*avct_bcb_action[action])(p_bcb, p_data);
    } else {
      break;
    }
  }
}

/*******************************************************************************
 *
 * Function         avct_lcb_by_bd
 *
 * Description      This lookup function finds the lcb for a BD address.
 *
 *
 * Returns          pointer to the lcb, or NULL if none found.
 *
 ******************************************************************************/
tAVCT_LCB* avct_lcb_by_bd(const RawAddress& bd_addr) {
  tAVCT_LCB* p_lcb = &avct_cb.lcb[0];
  int i;

  for (i = 0; i < AVCT_NUM_LINKS; i++, p_lcb++) {
    /* if allocated lcb has matching lcb */
    if (p_lcb->allocated && p_lcb->peer_addr == bd_addr) {
      break;
    }
  }

  if (i == AVCT_NUM_LINKS) {
    /* if no lcb found */
    p_lcb = NULL;

    log::verbose("No lcb for addr:{}", bd_addr);
  }
  return p_lcb;
}

/*******************************************************************************
 *
 * Function         avct_lcb_alloc
 *
 * Description      Allocate a link control block.
 *
 *
 * Returns          pointer to the lcb, or NULL if none could be allocated.
 *
 ******************************************************************************/
tAVCT_LCB* avct_lcb_alloc(const RawAddress& bd_addr) {
  tAVCT_LCB* p_lcb = &avct_cb.lcb[0];
  int i;

  for (i = 0; i < AVCT_NUM_LINKS; i++, p_lcb++) {
    if (!p_lcb->allocated) {
      p_lcb->allocated = (uint8_t)(i + 1);
      p_lcb->peer_addr = bd_addr;
      log::verbose("lcb_allocated:{}", p_lcb->allocated);
      p_lcb->tx_q = fixed_queue_new(SIZE_MAX);
      p_lcb->peer_mtu = L2CAP_LE_MIN_MTU;
      break;
    }
  }

  if (i == AVCT_NUM_LINKS) {
    /* out of lcbs */
    p_lcb = NULL;
    log::warn("Out of lcbs");
  }
  return p_lcb;
}

/*******************************************************************************
 *
 * Function         avct_lcb_dealloc
 *
 * Description      Deallocate a link control block.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avct_lcb_dealloc(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* /* p_data */) {
  log::verbose("lcb_allocated:{}", p_lcb->allocated);

  // Check if the LCB is still referenced

  tAVCT_CCB* p_ccb = &avct_cb.ccb[0];
  for (size_t i = 0; i < AVCT_NUM_CONN; i++, p_ccb++) {
    if (p_ccb->allocated && p_ccb->p_lcb == p_lcb) {
      log::verbose("LCB in use; lcb index:{}", i);
      return;
    }
  }

  // If not, de-allocate now...

  log::verbose("Freeing LCB");
  osi_free_and_reset((void**)&(p_lcb->p_rx_msg));
  fixed_queue_free(p_lcb->tx_q, NULL);
  memset(p_lcb, 0, sizeof(tAVCT_LCB));
}

/*******************************************************************************
 *
 * Function         avct_lcb_by_lcid
 *
 * Description      Find the LCB associated with the L2CAP LCID
 *
 *
 * Returns          pointer to the lcb, or NULL if none found.
 *
 ******************************************************************************/
tAVCT_LCB* avct_lcb_by_lcid(uint16_t lcid) {
  tAVCT_LCB* p_lcb = &avct_cb.lcb[0];
  int i;

  for (i = 0; i < AVCT_NUM_LINKS; i++, p_lcb++) {
    if (p_lcb->allocated && ((p_lcb->ch_lcid == lcid) || (p_lcb->conflict_lcid == lcid))) {
      break;
    }
  }

  if (i == AVCT_NUM_LINKS) {
    /* out of lcbs */
    p_lcb = nullptr;
    log::warn("No lcb for lcid 0x{:04x}", lcid);
  }

  return p_lcb;
}

/*******************************************************************************
 *
 * Function         avct_lcb_has_pid
 *
 * Description      See if any ccbs on this lcb have a particular pid.
 *
 *
 * Returns          Pointer to CCB if PID found, NULL otherwise.
 *
 ******************************************************************************/
tAVCT_CCB* avct_lcb_has_pid(tAVCT_LCB* p_lcb, uint16_t pid) {
  tAVCT_CCB* p_ccb = &avct_cb.ccb[0];
  int i;

  for (i = 0; i < AVCT_NUM_CONN; i++, p_ccb++) {
    if (p_ccb->allocated && (p_ccb->p_lcb == p_lcb) && (p_ccb->cc.pid == pid)) {
      return p_ccb;
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         avct_lcb_last_ccb
 *
 * Description      See if given ccb is only one on the lcb.
 *
 *
 * Returns          true if ccb is last, false otherwise.
 *
 ******************************************************************************/
bool avct_lcb_last_ccb(tAVCT_LCB* p_lcb, tAVCT_CCB* p_ccb_last) {
  tAVCT_CCB* p_ccb = &avct_cb.ccb[0];
  int i;

  log::warn("avct_lcb_last_ccb");
  for (i = 0; i < AVCT_NUM_CONN; i++, p_ccb++) {
    log::warn("index:{} allocated:{}, ", i, p_ccb->allocated);
    if (p_ccb->allocated && (p_ccb->p_lcb == p_lcb) && (p_ccb != p_ccb_last)) {
      return false;
    }
  }
  return true;
}
