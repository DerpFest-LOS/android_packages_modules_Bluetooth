/******************************************************************************
 *
 *  Copyright 2003-2016 Broadcom Corporation
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
 *  This file contains interfaces which are internal to AVCTP.
 *
 ******************************************************************************/
#ifndef AVCT_INT_H
#define AVCT_INT_H

#include <string>

#include "avct_api.h"
#include "include/macros.h"
#include "internal_include/bt_target.h"
#include "osi/include/fixed_queue.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/l2cap_interface.h"
#include "types/raw_address.h"

/*****************************************************************************
 * constants
 ****************************************************************************/

/* lcb state machine events */
enum {
  AVCT_LCB_UL_BIND_EVT,
  AVCT_LCB_UL_UNBIND_EVT,
  AVCT_LCB_UL_MSG_EVT,
  AVCT_LCB_INT_CLOSE_EVT,
  AVCT_LCB_LL_OPEN_EVT,
  AVCT_LCB_LL_CLOSE_EVT,
  AVCT_LCB_LL_MSG_EVT,
  AVCT_LCB_LL_CONG_EVT
};

/* "states" used for L2CAP channel */
enum tAVCT_CH {
  AVCT_CH_IDLE = 0, /* No connection */
  AVCT_CH_CONN = 1, /* Waiting for connection confirm */
  AVCT_CH_CFG = 2,  /* Waiting for configuration complete */
  AVCT_CH_OPEN = 3, /* Channel opened */
};

inline std::string avct_ch_state_text(const int& state) {
  switch (state) {
    CASE_RETURN_STRING(AVCT_CH_IDLE);
    CASE_RETURN_STRING(AVCT_CH_CONN);
    CASE_RETURN_STRING(AVCT_CH_CFG);
    CASE_RETURN_STRING(AVCT_CH_OPEN);
  }
  RETURN_UNKNOWN_TYPE_STRING(int, state);
}

/* "no event" indicator used by ccb dealloc */
#define AVCT_NO_EVT 0xFF

/*****************************************************************************
 * data types
 ****************************************************************************/
/* link control block type */
typedef struct {
  uint16_t peer_mtu;      /* peer l2c mtu */
  uint16_t ch_result;     /* L2CAP connection result value */
  uint16_t ch_lcid;       /* L2CAP channel LCID */
  uint8_t allocated;      /* 0, not allocated. index+1, otherwise. */
  uint8_t state;          /* The state machine state */
  uint8_t ch_state;       /* L2CAP channel state */
  BT_HDR* p_rx_msg;       /* Message being reassembled */
  uint16_t conflict_lcid; /* L2CAP channel LCID */
  RawAddress peer_addr;   /* BD address of peer */
  fixed_queue_t* tx_q;    /* Transmit data buffer queue       */
  bool cong;              /* true, if congested */
} tAVCT_LCB;

/* browse control block type */
typedef struct {
  uint16_t peer_mtu;      /* peer l2c mtu */
  uint16_t ch_result;     /* L2CAP connection result value */
  uint16_t ch_lcid;       /* L2CAP channel LCID */
  uint8_t allocated;      // 0: no link allocated. otherwise link index+1
  uint8_t state;          /* The state machine state */
  uint8_t ch_state;       /* L2CAP channel state */
  uint16_t conflict_lcid; /* L2CAP channel LCID */
  BT_HDR* p_tx_msg;       /* Message to be sent - in case the browsing channel is not
                             open when MsgReg is called */
  uint8_t ch_close;       /* CCB index+1, if CCB initiated channel close */
  RawAddress peer_addr;   /* BD address of peer */
} tAVCT_BCB;

#define AVCT_ALOC_LCB 0x01
#define AVCT_ALOC_BCB 0x02
/* connection control block */
typedef struct {
  tAVCT_CC cc;       /* parameters from connection creation */
  tAVCT_LCB* p_lcb;  /* Associated LCB */
  tAVCT_BCB* p_bcb;  /* associated BCB */
  bool ch_close;     /* Whether CCB initiated channel close */
  uint8_t allocated; /* Whether LCB/BCB is allocated */
} tAVCT_CCB;

/* data type associated with UL_MSG_EVT */
typedef struct {
  BT_HDR* p_buf;
  tAVCT_CCB* p_ccb;
  uint8_t label;
  uint8_t cr;
} tAVCT_UL_MSG;

/* union associated with lcb state machine events */
typedef union {
  tAVCT_UL_MSG ul_msg;
  BT_HDR* p_buf;
  tAVCT_CCB* p_ccb;
  uint16_t result;
  bool cong;
  uint8_t err_code;
} tAVCT_LCB_EVT;

/* Control block for AVCT */
typedef struct {
  tAVCT_LCB lcb[AVCT_NUM_LINKS]; /* link control blocks */
  tAVCT_BCB bcb[AVCT_NUM_LINKS]; /* browse control blocks */
  tAVCT_CCB ccb[AVCT_NUM_CONN];  /* connection control blocks */
} tAVCT_CB;

/*****************************************************************************
 * function declarations
 ****************************************************************************/

/* LCB function declarations */
void avct_lcb_event(tAVCT_LCB* p_lcb, uint8_t event, tAVCT_LCB_EVT* p_data);
void avct_bcb_event(tAVCT_BCB* p_bcb, uint8_t event, tAVCT_LCB_EVT* p_data);
void avct_close_bcb(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
tAVCT_LCB* avct_lcb_by_bcb(tAVCT_BCB* p_bcb);
tAVCT_BCB* avct_bcb_by_lcb(tAVCT_LCB* p_lcb);
uint8_t avct_bcb_get_last_ccb_index(tAVCT_BCB* p_bcb, tAVCT_CCB* p_ccb_last);
tAVCT_BCB* avct_bcb_by_lcid(uint16_t lcid);
tAVCT_LCB* avct_lcb_by_bd(const RawAddress& bd_addr);
tAVCT_LCB* avct_lcb_alloc(const RawAddress& bd_addr);
void avct_lcb_dealloc(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
tAVCT_LCB* avct_lcb_by_lcid(uint16_t lcid);
tAVCT_CCB* avct_lcb_has_pid(tAVCT_LCB* p_lcb, uint16_t pid);
bool avct_lcb_last_ccb(tAVCT_LCB* p_lcb, tAVCT_CCB* p_ccb_last);

/* LCB action functions */
void avct_lcb_chnl_open(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_unbind_disc(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_open_ind(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_open_fail(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_close_ind(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_close_cfm(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_bind_conn(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_chk_disc(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_chnl_disc(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_bind_fail(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_cong_ind(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_discard_msg(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_send_msg(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_msg_ind(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);
void avct_lcb_free_msg_ind(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data);

/* BCB action functions */
typedef void (*tAVCT_BCB_ACTION)(tAVCT_BCB* p_bcb, tAVCT_LCB_EVT* p_data);

extern const tAVCT_BCB_ACTION avct_bcb_action[];
extern const uint8_t avct_lcb_pkt_type_len[];

/* CCB function declarations */
tAVCT_CCB* avct_ccb_alloc(tAVCT_CC* p_cc);
void avct_ccb_dealloc(tAVCT_CCB* p_ccb, uint8_t event, uint16_t result, const RawAddress* bd_addr);
uint8_t avct_ccb_to_idx(tAVCT_CCB* p_ccb);
tAVCT_CCB* avct_ccb_by_idx(uint8_t idx);

extern bool avct_msg_ind_for_src_sink_coexist(tAVCT_LCB* p_lcb, tAVCT_LCB_EVT* p_data,
                                              uint8_t label, uint8_t cr_ipid, uint16_t pid);

std::string avct_sm_state_text(const int& state);

/*****************************************************************************
 * global data
 ****************************************************************************/

/* Main control block */
extern tAVCT_CB avct_cb;

/* L2CAP callback registration structure */
extern const tL2CAP_APPL_INFO avct_l2c_appl;
extern const tL2CAP_APPL_INFO avct_l2c_br_appl;

void avct_l2c_disconnect(uint16_t lcid, uint16_t result);
void avct_l2c_br_disconnect(uint16_t lcid, uint16_t result);

constexpr uint16_t kAvrcMtu = 512;
constexpr uint16_t kAvrcBrMtu = 1008;

#endif /* AVCT_INT_H */
