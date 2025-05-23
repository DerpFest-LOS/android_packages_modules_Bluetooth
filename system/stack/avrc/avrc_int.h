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
 *  VRCP internal header file.
 *
 ******************************************************************************/

#ifndef AVRC_INT_H
#define AVRC_INT_H

#include "internal_include/bt_target.h"
#include "osi/include/alarm.h"
#include "osi/include/fixed_queue.h"
#include "stack/include/avrc_api.h"
#include "stack/include/bt_hdr.h"

/*      DEBUG FLAGS
 *
 * #define META_DEBUG_ENABLED
 */
/*****************************************************************************
 *  Constants
 ****************************************************************************/

/* Number of attributes in AVRC SDP record. */
#define AVRC_NUM_ATTR 6

/* Number of protocol elements in protocol element list. */
#define AVRC_NUM_PROTO_ELEMS 2

#ifndef AVRC_MIN_CMD_LEN
#define AVRC_MIN_CMD_LEN 20
#endif

#define AVRC_UNIT_OPRND_BYTES 5
#define AVRC_SUB_OPRND_BYTES 4
#define AVRC_SUBRSP_OPRND_BYTES 3
#define AVRC_SUB_PAGE_MASK 7
#define AVRC_SUB_PAGE_SHIFT 4
#define AVRC_SUB_EXT_CODE 7
#define AVRC_PASS_OP_ID_MASK 0x7F
#define AVRC_PASS_STATE_MASK 0x80
#define AVRC_CMD_OPRND_PAD 0xFF

#define AVRC_CTYPE_MASK 0x0F
#define AVRC_SUBTYPE_MASK 0xF8
#define AVRC_SUBTYPE_SHIFT 3
#define AVRC_SUBID_MASK 0x07
#define AVRC_SUBID_IGNORE 0x07

#define AVRC_SINGLE_PARAM_SIZE 1
#define AVRC_METADATA_PKT_TYPE_MASK 0x03
#define AVRC_PASS_THOUGH_MSG_MASK 0x80 /* MSB of msg_type indicates the PAS THROUGH msg */
#define AVRC_VENDOR_UNIQUE_MASK 0x70   /* vendor unique id */

/* Company ID is 24-bit integer We can not use the macros in bt_types.h */
#define AVRC_CO_ID_TO_BE_STREAM(p, u32) \
  {                                     \
    *(p)++ = (uint8_t)((u32) >> 16);    \
    *(p)++ = (uint8_t)((u32) >> 8);     \
    *(p)++ = (uint8_t)(u32);            \
  }
#define AVRC_BE_STREAM_TO_CO_ID(u32, p)                                   \
  {                                                                       \
    (u32) = (((uint32_t)(*((p) + 2))) + (((uint32_t)(*((p) + 1))) << 8) + \
             (((uint32_t)(*(p))) << 16));                                 \
    (p) += 3;                                                             \
  }

#define AVRC_AVC_HDR_SIZE 3 /* ctype, subunit*, opcode */

#define AVRC_MIN_META_HDR_SIZE 4   /* pdu id(1), packet type(1), param len(2) */
#define AVRC_MIN_BROWSE_HDR_SIZE 3 /* pdu id(1), param len(2) */

#define AVRC_VENDOR_HDR_SIZE 6 /* ctype, subunit*, opcode, CO_ID */
#define AVRC_MSG_VENDOR_OFFSET 23
#define AVRC_MIN_VENDOR_SIZE (AVRC_MSG_VENDOR_OFFSET + BT_HDR_SIZE + AVRC_MIN_META_HDR_SIZE)

#define AVRC_PASS_THRU_SIZE 8
#define AVRC_MSG_PASS_THRU_OFFSET 25
#define AVRC_MIN_PASS_THRU_SIZE (AVRC_MSG_PASS_THRU_OFFSET + BT_HDR_SIZE + 4)

#define AVRC_MIN_BROWSE_SIZE (AVCT_BROWSE_OFFSET + BT_HDR_SIZE + AVRC_MIN_BROWSE_HDR_SIZE)

#define AVRC_CTRL_PKT_LEN(pf, pk) \
  { (pf) = (uint8_t*)((pk) + 1) + (pk)->offset + 2; }

#define AVRC_MAX_CTRL_DATA_LEN (AVRC_PACKET_LEN)

/* Timeout for waiting for avrc command responses (in milliseconds) */
#ifndef AVRC_CMD_TOUT_MS
#define AVRC_CMD_TOUT_MS (2 * 1000)
#endif

/* Flags for avrc_cb.ccb_int[].flags */
#define AVRC_CB_FLAGS_RSP_PENDING 0x01 /* Waiting for AVRC response */

/*****************************************************************************
 *  Type definitions
 ****************************************************************************/

/* type for Metadata fragmentation control block */
typedef struct {
  BT_HDR* p_fmsg;    /* the fragmented message */
  uint8_t frag_pdu;  /* the PDU ID for fragmentation */
  bool frag_enabled; /* fragmentation flag */
} tAVRC_FRAG_CB;

/* type for Metadata re-assembly control block */
typedef struct {
  BT_HDR* p_rmsg;       /* the received message */
  uint16_t rasm_offset; /* re-assembly flag, the offset of the start fragment */
  uint8_t rasm_pdu;     /* the PDU ID for re-assembly */
} tAVRC_RASM_CB;

/* AVRC internal connection control block */
typedef struct {
  fixed_queue_t* cmd_q; /* Command queue for serializing vendor specific commands */
  uint8_t flags;        /* See AVRC_CB_FLAGS_* definitions */
  alarm_t* tle;         /* Command timeout timer */
} tAVRC_CONN_INT_CB;

typedef struct {
  tAVRC_CONN_CB ccb[AVCT_NUM_CONN];         /* Connection control block from AVRC_Open API */
  tAVRC_CONN_INT_CB ccb_int[AVCT_NUM_CONN]; /* Internal connection control block  */
  tAVRC_FRAG_CB fcb[AVCT_NUM_CONN];
  tAVRC_RASM_CB rcb[AVCT_NUM_CONN];
  tAVRC_FIND_CBACK find_cback; /* sdp discovery callback */
  tSDP_DISCOVERY_DB* p_db;     /* pointer to discovery database */
  uint16_t service_uuid;       /* service UUID to search */
} tAVRC_CB;

/******************************************************************************
 * Main Control Block
 ******************************************************************************/
extern tAVRC_CB avrc_cb;

bool avrc_is_valid_pdu_id(uint8_t pdu_id);
bool avrc_is_valid_player_attrib_value(uint8_t attrib, uint8_t value);
BT_HDR* avrc_alloc_ctrl_pkt(uint8_t pdu);
tAVRC_STS avrc_pars_pass_thru(tAVRC_MSG_PASS* p_msg, uint16_t* p_vendor_unique_id);
uint8_t avrc_opcode_from_pdu(uint8_t pdu);
bool avrc_is_valid_opcode(uint8_t opcode);
void avrc_flush_cmd_q(uint8_t handle);
void avrc_send_next_vendor_cmd(uint8_t handle);

#endif /* AVRC_INT_H */
