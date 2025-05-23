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
 *  This is the private interface file for the BTA data gateway.
 *
 ******************************************************************************/
#ifndef BTA_PAN_INT_H
#define BTA_PAN_INT_H

#include <cstdint>

#include "bta/include/bta_api.h"
#include "bta/include/bta_pan_api.h"
#include "bta/include/bta_sec_api.h"
#include "bta/sys/bta_sys.h"
#include "osi/include/fixed_queue.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/pan_api.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  Constants
 ****************************************************************************/

/* PAN events */
enum {
  /* these events are handled by the state machine */
  BTA_PAN_API_CLOSE_EVT = BTA_SYS_EVT_START(BTA_ID_PAN),
  BTA_PAN_CI_TX_READY_EVT,
  BTA_PAN_CI_RX_READY_EVT,
  BTA_PAN_CI_TX_FLOW_EVT,
  BTA_PAN_CI_RX_WRITE_EVT,
  BTA_PAN_CI_RX_WRITEBUF_EVT,
  BTA_PAN_CONN_OPEN_EVT,
  BTA_PAN_CONN_CLOSE_EVT,
  BTA_PAN_BNEP_FLOW_ENABLE_EVT,
  BTA_PAN_RX_FROM_BNEP_READY_EVT,

  /* these events are handled outside of the state machine */
  BTA_PAN_API_ENABLE_EVT,
  BTA_PAN_API_DISABLE_EVT,
  BTA_PAN_API_SET_ROLE_EVT,
  BTA_PAN_API_OPEN_EVT
};

/* state machine states */
enum { BTA_PAN_IDLE_ST, BTA_PAN_OPEN_ST, BTA_PAN_CLOSING_ST };

/*****************************************************************************
 *  Data types
 ****************************************************************************/

/* data type for BTA_PAN_API_ENABLE_EVT */
typedef struct {
  BT_HDR_RIGID hdr;        /* Event header */
  tBTA_PAN_CBACK* p_cback; /* PAN callback function */
} tBTA_PAN_API_ENABLE;

/* data type for BTA_PAN_API_REG_ROLE_EVT */
typedef struct {
  BT_HDR_RIGID hdr;                         /* Event header */
  char user_name[BTA_SERVICE_NAME_LEN + 1]; /* Service name */
  char nap_name[BTA_SERVICE_NAME_LEN + 1];  /* Service name */
  tBTA_PAN_ROLE role;
  uint8_t user_app_id;
  uint8_t nap_app_id;
} tBTA_PAN_API_SET_ROLE;

/* data type for BTA_PAN_API_OPEN_EVT */
typedef struct {
  BT_HDR_RIGID hdr;         /* Event header */
  tBTA_PAN_ROLE local_role; /* local role */
  tBTA_PAN_ROLE peer_role;  /* peer role */
  RawAddress bd_addr;       /* peer bdaddr */
} tBTA_PAN_API_OPEN;

/* data type for BTA_PAN_CI_TX_FLOW_EVT */
typedef struct {
  BT_HDR_RIGID hdr; /* Event header */
  bool enable;      /* Flow control setting */
} tBTA_PAN_CI_TX_FLOW;

/* data type for BTA_PAN_CONN_OPEN_EVT */
typedef struct {
  BT_HDR_RIGID hdr; /* Event header */
  tPAN_RESULT result;
} tBTA_PAN_CONN;

/* pan data param */
typedef struct {
  BT_HDR_RIGID hdr;
  RawAddress src;
  RawAddress dst;
  uint16_t protocol;
  bool ext;
  bool forward;
} tBTA_PAN_DATA_PARAMS;

/* union of all data types */
typedef union {
  BT_HDR_RIGID hdr;
  tBTA_PAN_API_ENABLE api_enable;
  tBTA_PAN_API_SET_ROLE api_set_role;
  tBTA_PAN_API_OPEN api_open;
  tBTA_PAN_CI_TX_FLOW ci_tx_flow;
  tBTA_PAN_CONN conn;
  tBTA_PAN_DATA_PARAMS params;
} tBTA_PAN_DATA;

/* state machine control block */
typedef struct {
  RawAddress bd_addr;        /* peer bdaddr */
  fixed_queue_t* data_queue; /* Queue of buffers waiting to be passed to application */
  uint16_t handle;           /* BTA PAN/BNEP handle */
  bool in_use;               /* scb in use */
  tBTA_SEC sec_mask;         /* Security mask */
  bool pan_flow_enable;      /* BNEP flow control state */
  bool app_flow_enable;      /* Application flow control state */
  uint8_t state;             /* State machine state */
  tBTA_PAN_ROLE local_role;  /* local role */
  tBTA_PAN_ROLE peer_role;   /* peer role */
  uint8_t app_id;            /* application id for the connection */
} tBTA_PAN_SCB;

/* main control block */
typedef struct {
  tBTA_PAN_SCB scb[BTA_PAN_NUM_CONN]; /* state machine control blocks */
  tBTA_PAN_CBACK* p_cback;            /* PAN callback function */
  uint8_t app_id[3];                  /* application id for PAN roles */
  uint8_t flow_mask;                  /* Data flow mask */
  uint8_t q_level;                    /* queue level set by application for TX data */
} tBTA_PAN_CB;

/*****************************************************************************
 *  Global data
 ****************************************************************************/

/* PAN control block */
extern tBTA_PAN_CB bta_pan_cb;

/*****************************************************************************
 *  Function prototypes
 ****************************************************************************/
tBTA_PAN_SCB* bta_pan_scb_alloc(void);
void bta_pan_scb_dealloc(tBTA_PAN_SCB* p_scb);
uint8_t bta_pan_scb_to_idx(tBTA_PAN_SCB* p_scb);
tBTA_PAN_SCB* bta_pan_scb_by_handle(uint16_t handle);
bool bta_pan_hdl_event(const BT_HDR_RIGID* p_msg);

/* action functions */
void bta_pan_enable(tBTA_PAN_DATA* p_data);
void bta_pan_disable(void);
void bta_pan_set_role(tBTA_PAN_DATA* p_data);
void bta_pan_open(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_api_close(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_rx_path(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_tx_path(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_tx_flow(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_conn_open(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_conn_close(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_write_buf(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);
void bta_pan_free_buf(tBTA_PAN_SCB* p_scb, tBTA_PAN_DATA* p_data);

#endif /* BTA_PAN_INT_H */
