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
 *  This interface file contains the interface to the Audio Video Control
 *  Transport Protocol (AVCTP).
 *
 ******************************************************************************/
#ifndef AVCT_API_H
#define AVCT_API_H

#include <cstdint>
#include <string>

#include "include/macros.h"
#include "stack/include/bt_hdr.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  Constants
 ****************************************************************************/

/* API function return value result codes. */
#define AVCT_SUCCESS 0      /* Function successful */
#define AVCT_NO_RESOURCES 1 /* Not enough resources */
#define AVCT_BAD_HANDLE 2   /* Bad handle */
#define AVCT_PID_IN_USE 3   /* PID already in use */
#define AVCT_NOT_OPEN 4     /* Connection not open */

/* Protocol revision numbers */
#define AVCT_REV_1_0 0x0100
#define AVCT_REV_1_2 0x0102
#define AVCT_REV_1_3 0x0103
#define AVCT_REV_1_4 0x0104

/* the layer_specific settings */
#define AVCT_DATA_CTRL 0x0001    /* for the control channel */
#define AVCT_DATA_BROWSE 0x0002  /* for the browsing channel */
#define AVCT_DATA_PARTIAL 0x0100 /* Only have room for a partial message */

/* Per the AVRC spec, minimum MTU for the control channel */
#define AVCT_MIN_CONTROL_MTU 48
/* Per the AVRC spec, minimum MTU for the browsing channel */
#define AVCT_MIN_BROWSE_MTU 335

/* Message offset.  The number of bytes needed by the protocol stack for the
 * protocol headers of an AVCTP message packet.
 */
#define AVCT_MSG_OFFSET 15
#define AVCT_BROWSE_OFFSET 17 /* the default offset for browsing channel */

/* Connection role. */
typedef enum {
  AVCT_ROLE_INITIATOR = 0, /* Initiator connection */
  AVCT_ROLE_ACCEPTOR = 1,  /* Acceptor connection */
} tAVCT_ROLE;

inline std::string avct_role_text(const tAVCT_ROLE& role) {
  switch (role) {
    CASE_RETURN_TEXT(AVCT_ROLE_INITIATOR);
    CASE_RETURN_TEXT(AVCT_ROLE_ACCEPTOR);
  }
  RETURN_UNKNOWN_TYPE_STRING(tAVCT_ROLE, role);
}

/* Control role. */
#define AVCT_TARGET 1  /* target  */
#define AVCT_CONTROL 2 /* controller  */
#define AVCT_PASSIVE 4 /* If conflict, allow the other side to succeed  */

/* Command/Response indicator. */
#define AVCT_CMD 0 /* Command message */
#define AVCT_RSP 2 /* Response message */
#define AVCT_REJ 3 /* Message rejected */

/* Control callback events. */
#define AVCT_CONNECT_CFM_EVT 0        /* Connection confirm */
#define AVCT_CONNECT_IND_EVT 1        /* Connection indication */
#define AVCT_DISCONNECT_CFM_EVT 2     /* Disconnect confirm */
#define AVCT_DISCONNECT_IND_EVT 3     /* Disconnect indication */
#define AVCT_CONG_IND_EVT 4           /* Congestion indication */
#define AVCT_UNCONG_IND_EVT 5         /* Uncongestion indication */
#define AVCT_BROWSE_CONN_CFM_EVT 6    /* Browse Connection confirm */
#define AVCT_BROWSE_CONN_IND_EVT 7    /* Browse Connection indication */
#define AVCT_BROWSE_DISCONN_CFM_EVT 8 /* Browse Disconnect confirm */
#define AVCT_BROWSE_DISCONN_IND_EVT 9 /* Browse Disconnect indication */
#define AVCT_BROWSE_CONG_IND_EVT 10   /* Congestion indication */
#define AVCT_BROWSE_UNCONG_IND_EVT 11 /* Uncongestion indication */

/* General purpose failure result code for callback events. */
#define AVCT_RESULT_FAIL 5

/*****************************************************************************
 *  Type Definitions
 ****************************************************************************/

/* Control callback function. */
typedef void(tAVCT_CTRL_CBACK)(uint8_t handle, uint8_t event, uint16_t result,
                               const RawAddress* peer_addr);

/* Message callback function */
/* p_pkt->layer_specific is AVCT_DATA_CTRL or AVCT_DATA_BROWSE */
typedef void(tAVCT_MSG_CBACK)(uint8_t handle, uint8_t label, uint8_t cr, BT_HDR* p_pkt);

/* Structure used by AVCT_CreateConn. */
typedef struct {
  tAVCT_CTRL_CBACK* p_ctrl_cback; /* Control callback */
  tAVCT_MSG_CBACK* p_msg_cback;   /* Message callback */
  uint16_t pid;                   /* Profile ID */
  tAVCT_ROLE role;                /* Initiator/acceptor role */
  uint8_t control;                /* Control role (Control/Target) */
} tAVCT_CC;

/*****************************************************************************
 *  External Function Declarations
 ****************************************************************************/

/*******************************************************************************
 *
 * Function         AVCT_Register
 *
 * Description      This is the system level registration function for the
 *                  AVCTP protocol.  This function initializes AVCTP and
 *                  prepares the protocol stack for its use.  This function
 *                  must be called once by the system or platform using AVCTP
 *                  before the other functions of the API an be used.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void AVCT_Register();

/*******************************************************************************
 *
 * Function         AVCT_Deregister
 *
 * Description      This function is called to deregister use AVCTP protocol.
 *                  It is called when AVCTP is no longer being used by any
 *                  application in the system.  Before this function can be
 *                  called, all connections must be removed with
 *                  AVCT_RemoveConn().
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void AVCT_Deregister(void);

/*******************************************************************************
 *
 * Function         AVCT_CreateConn
 *
 * Description      Create an AVCTP connection.  There are two types of
 *                  connections, initiator and acceptor, as determined by
 *                  the p_cc->role parameter.  When this function is called to
 *                  create an initiator connection, an AVCTP connection to
 *                  the peer device is initiated if one does not already exist.
 *                  If an acceptor connection is created, the connection waits
 *                  passively for an incoming AVCTP connection from a peer
 *                  device.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_CreateConn(uint8_t* p_handle, tAVCT_CC* p_cc, const RawAddress& peer_addr);

/*******************************************************************************
 *
 * Function         AVCT_RemoveConn
 *
 * Description      Remove an AVCTP connection.  This function is called when
 *                  the application is no longer using a connection.  If this
 *                  is the last connection to a peer the L2CAP channel for AVCTP
 *                  will be closed.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_RemoveConn(uint8_t handle);

/*******************************************************************************
 *
 * Function         AVCT_CreateBrowse
 *
 * Description      Create an AVCTP connection.  There are two types of
 *                  connections, initiator and acceptor, as determined by
 *                  the p_cc->role parameter.  When this function is called to
 *                  create an initiator connection, an AVCTP connection to
 *                  the peer device is initiated if one does not already exist.
 *                  If an acceptor connection is created, the connection waits
 *                  passively for an incoming AVCTP connection from a peer
 *                  device.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_CreateBrowse(uint8_t handle, tAVCT_ROLE role);

/*******************************************************************************
 *
 * Function         AVCT_RemoveBrowse
 *
 * Description      Remove an AVCTP connection.  This function is called when
 *                  the application is no longer using a connection.  If this
 *                  is the last connection to a peer the L2CAP channel for AVCTP
 *                  will be closed.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_RemoveBrowse(uint8_t handle);

/*******************************************************************************
 *
 * Function         AVCT_GetBrowseMtu
 *
 * Description      Get the peer_mtu for the AVCTP Browse channel of the given
 *                  connection.
 *
 * Returns          the peer browsing channel MTU.
 *
 ******************************************************************************/
uint16_t AVCT_GetBrowseMtu(uint8_t handle);

/*******************************************************************************
 *
 * Function         AVCT_GetPeerMtu
 *
 * Description      Get the peer_mtu for the AVCTP channel of the given
 *                  connection.
 *
 * Returns          the peer MTU size.
 *
 ******************************************************************************/
uint16_t AVCT_GetPeerMtu(uint8_t handle);

/*******************************************************************************
 *
 * Function         AVCT_MsgReq
 *
 * Description      Send an AVCTP message to a peer device.  In calling
 *                  AVCT_MsgReq(), the application should keep track of the
 *                  congestion state of AVCTP as communicated with events
 *                  AVCT_CONG_IND_EVT and AVCT_UNCONG_IND_EVT.   If the
 *                  application calls AVCT_MsgReq() when AVCTP is congested
 *                  the message may be discarded.  The application may make its
 *                  first call to AVCT_MsgReq() after it receives an
 *                  AVCT_CONNECT_CFM_EVT or AVCT_CONNECT_IND_EVT on control
 *                  channel or
 *                  AVCT_BROWSE_CONN_CFM_EVT or AVCT_BROWSE_CONN_IND_EVT on
 *                  browsing channel.
 *
 *                  p_msg->layer_specific must be set to
 *                  AVCT_DATA_CTRL for control channel traffic;
 *                  AVCT_DATA_BROWSE for for browse channel traffic.
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_MsgReq(uint8_t handle, uint8_t label, uint8_t cr, BT_HDR* p_msg);

/*******************************************************************************
**
** Function         AVCT_Dumpsys
**
** Description      This function provides dumpsys data during the dumpsys
**                  procedure.
**
** Parameters:      fd: Descriptor used to write the AVCT internals
**
** Returns          void
**
*******************************************************************************/
void AVCT_Dumpsys(int fd);

#endif /* AVCT_API_H */
