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
 *  this file contains the PAN API definitions
 *
 ******************************************************************************/
#ifndef PAN_API_H
#define PAN_API_H

#include <base/strings/stringprintf.h>
#include <bluetooth/log.h>

#include <cstdint>

#include "bnep_api.h"
#include "macros.h"
#include "stack/include/bt_hdr.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  Constants
 ****************************************************************************/

/* Define the minimum offset needed in a GKI buffer for
 * sending PAN packets. Note, we are currently not sending
 * extension headers, but may in the future, so allow
 * space for them
 */
#define PAN_MINIMUM_OFFSET BNEP_MINIMUM_OFFSET

/*
 * The handle is passed from BNEP to PAN. The same handle is used
 * between PAN and application as well
 */
#define PAN_INVALID_HANDLE BNEP_INVALID_HANDLE

/* Bit map for PAN roles */
#define PAN_ROLE_CLIENT 0x01     /* PANU role */
#define PAN_ROLE_GROUP 0x02      /* Adhoc network group role */
#define PAN_ROLE_NAP_SERVER 0x04 /* NAP role */
typedef uint8_t tPAN_ROLE;

inline const std::string pan_role_to_text(const tPAN_ROLE& role) {
  return base::StringPrintf("%c%c%c[0x%x]", (role & PAN_ROLE_CLIENT) ? 'C' : '.',
                            (role & PAN_ROLE_GROUP) ? 'G' : '.',
                            (role & PAN_ROLE_NAP_SERVER) ? 'N' : '.', role);
}

/*****************************************************************************
 *  Type Definitions
 ****************************************************************************/

/* Define the result codes from PAN */
typedef enum : uint8_t {
  PAN_SUCCESS,                                      /* Success                           */
  PAN_DISCONNECTED = BNEP_CONN_DISCONNECTED,        /* Connection terminated   */
  PAN_CONN_FAILED = BNEP_CONN_FAILED,               /* Connection failed                 */
  PAN_NO_RESOURCES = BNEP_NO_RESOURCES,             /* No resources                      */
  PAN_MTU_EXCEEDED = BNEP_MTU_EXCEEDED,             /* Attempt to write long data        */
  PAN_INVALID_OFFSET = BNEP_INVALID_OFFSET,         /* Insufficient offset in GKI buffer */
  PAN_CONN_FAILED_CFG = BNEP_CONN_FAILED_CFG,       /* Connection failed cos of config   */
  PAN_INVALID_SRC_ROLE = BNEP_CONN_FAILED_SRC_UUID, /* Connection failed wrong source UUID   */
  PAN_INVALID_DST_ROLE = BNEP_CONN_FAILED_DST_UUID, /* Connection failed wrong destination UUID */
  PAN_CONN_FAILED_UUID_SIZE = BNEP_CONN_FAILED_UUID_SIZE, /* Connection failed wrong size UUID   */
  PAN_Q_SIZE_EXCEEDED = BNEP_Q_SIZE_EXCEEDED,             /* Too many buffers to dest */
  PAN_TOO_MANY_FILTERS = BNEP_TOO_MANY_FILTERS,           /* Too many local filters specified  */
  PAN_SET_FILTER_FAIL = BNEP_SET_FILTER_FAIL,             /* Set Filter failed  */
  PAN_WRONG_HANDLE = BNEP_WRONG_HANDLE,                   /* Wrong handle for the connection  */
  PAN_WRONG_STATE = BNEP_WRONG_STATE,                     /* Connection is in wrong state */
  PAN_SECURITY_FAIL = BNEP_SECURITY_FAIL,                 /* Failed because of security */
  PAN_IGNORE_CMD = BNEP_IGNORE_CMD,                       /* To ignore the rcvd command */
  PAN_TX_FLOW_ON = BNEP_TX_FLOW_ON,                       /* tx data flow enabled */
  PAN_TX_FLOW_OFF = BNEP_TX_FLOW_OFF,                     /* tx data flow disabled */
  PAN_FAILURE = 19,                                       /* Failure                      */
  PAN_HOTSPOT_DISABLED = 20,                              /* Hotspot disabled             */
} tPAN_RESULT;

inline const std::string pan_result_text(const tPAN_RESULT& result) {
  switch (result) {
    CASE_RETURN_TEXT(PAN_SUCCESS);
    CASE_RETURN_TEXT(PAN_DISCONNECTED);
    CASE_RETURN_TEXT(PAN_CONN_FAILED);
    CASE_RETURN_TEXT(PAN_NO_RESOURCES);
    CASE_RETURN_TEXT(PAN_MTU_EXCEEDED);
    CASE_RETURN_TEXT(PAN_INVALID_OFFSET);
    CASE_RETURN_TEXT(PAN_CONN_FAILED_CFG);
    CASE_RETURN_TEXT(PAN_INVALID_SRC_ROLE);
    CASE_RETURN_TEXT(PAN_INVALID_DST_ROLE);
    CASE_RETURN_TEXT(PAN_CONN_FAILED_UUID_SIZE);
    CASE_RETURN_TEXT(PAN_Q_SIZE_EXCEEDED);
    CASE_RETURN_TEXT(PAN_TOO_MANY_FILTERS);
    CASE_RETURN_TEXT(PAN_SET_FILTER_FAIL);
    CASE_RETURN_TEXT(PAN_WRONG_HANDLE);
    CASE_RETURN_TEXT(PAN_WRONG_STATE);
    CASE_RETURN_TEXT(PAN_SECURITY_FAIL);
    CASE_RETURN_TEXT(PAN_IGNORE_CMD);
    CASE_RETURN_TEXT(PAN_TX_FLOW_ON);
    CASE_RETURN_TEXT(PAN_TX_FLOW_OFF);
    CASE_RETURN_TEXT(PAN_FAILURE);
    CASE_RETURN_TEXT(PAN_HOTSPOT_DISABLED);
    default:
      return base::StringPrintf("UNKNOWN[%hhu]", result);
  }
}

/*****************************************************************
 *       Callback Function Prototypes
 ****************************************************************/

/* This is call back function used to report connection status
 *      to the application. The second parameter true means
 *      to create the bridge and false means to remove it.
 */
typedef void(tPAN_CONN_STATE_CB)(uint16_t handle, const RawAddress& bd_addr, tPAN_RESULT state,
                                 bool is_role_change, uint8_t src_role, uint8_t dst_role);

/* This is call back function used to create bridge for the
 *      Connected device. The parameter "state" indicates
 *      whether to create the bridge or remove it. true means
 *      to create the bridge and false means to remove it.
 */
typedef void(tPAN_BRIDGE_REQ_CB)(const RawAddress& bd_addr, bool state);

/* Data received indication callback prototype. Parameters are
 *              Source BD/Ethernet Address
 *              Dest BD/Ethernet address
 *              Protocol
 *              Address of buffer (or data if non-GKI)
 *              Length of data (non-GKI)
 *              ext is flag to indicate whether it has aby extension headers
 *              Flag used to indicate to forward on LAN
 *                      false - Use it for internal stack
 *                      true  - Send it across the ethernet as well
 */
typedef void(tPAN_DATA_IND_CB)(uint16_t handle, const RawAddress& src, const RawAddress& dst,
                               uint16_t protocol, uint8_t* p_data, uint16_t len, bool ext,
                               bool forward);

/* Data buffer received indication callback prototype. Parameters are
 *              Source BD/Ethernet Address
 *              Dest BD/Ethernet address
 *              Protocol
 *              pointer to the data buffer
 *              ext is flag to indicate whether it has aby extension headers
 *              Flag used to indicate to forward on LAN
 *                      false - Use it for internal stack
 *                      true  - Send it across the ethernet as well
 */
typedef void(tPAN_DATA_BUF_IND_CB)(uint16_t handle, const RawAddress& src, const RawAddress& dst,
                                   uint16_t protocol, BT_HDR* p_buf, bool ext, bool forward);

/* Flow control callback for TX data. Parameters are
 *              Handle to the connection
 *              Event  flow status
 */
typedef void(tPAN_TX_DATA_FLOW_CB)(uint16_t handle, tPAN_RESULT event);

/* Filters received indication callback prototype. Parameters are
 *              Handle to the connection
 *              true if the cb is called for indication
 *              Ignore this if it is indication, otherwise it is the result
 *                      for the filter set operation performed by the local
 *                      device
 *              Number of protocol filters present
 *              Pointer to the filters start. Filters are present in pairs
 *                      of start of the range and end of the range.
 *                      They will be present in big endian order. First
 *                      two bytes will be starting of the first range and
 *                      next two bytes will be ending of the range.
 */
typedef void(tPAN_FILTER_IND_CB)(uint16_t handle, bool indication, tPAN_RESULT result,
                                 uint16_t num_filters, uint8_t* p_filters);

/* Multicast Filters received indication callback prototype. Parameters are
 *              Handle to the connection
 *              true if the cb is called for indication
 *              Ignore this if it is indication, otherwise it is the result
 *                      for the filter set operation performed by the local
 *                      device
 *              Number of multicast filters present
 *              Pointer to the filters start. Filters are present in pairs
 *                      of start of the range and end of the range.
 *                      First six bytes will be starting of the first range and
 *                      next six bytes will be ending of the range.
 */
typedef void(tPAN_MFILTER_IND_CB)(uint16_t handle, bool indication, tPAN_RESULT result,
                                  uint16_t num_mfilters, uint8_t* p_mfilters);

/* This structure is used to register with PAN profile
 * It is passed as a parameter to PAN_Register call.
 */
typedef struct {
  tPAN_CONN_STATE_CB* pan_conn_state_cb;     /* Connection state callback */
  tPAN_BRIDGE_REQ_CB* pan_bridge_req_cb;     /* Bridge request callback */
  tPAN_DATA_IND_CB* pan_data_ind_cb;         /* Data indication callback */
  tPAN_DATA_BUF_IND_CB* pan_data_buf_ind_cb; /* Data buffer indication callback */
  tPAN_FILTER_IND_CB* pan_pfilt_ind_cb;      /* protocol filter indication callback */
  tPAN_MFILTER_IND_CB* pan_mfilt_ind_cb;     /* multicast filter indication callback */
  tPAN_TX_DATA_FLOW_CB* pan_tx_data_flow_cb; /* data flow callback */
} tPAN_REGISTER;

/*****************************************************************************
 *  External Function Declarations
 ****************************************************************************/

/*******************************************************************************
 *
 * Function         PAN_Register
 *
 * Description      This function is called by the application to register
 *                  its callbacks with PAN profile. The application then
 *                  should set the PAN role explicitly.
 *
 * Parameters:      p_register - contains all callback function pointers
 *
 *
 * Returns          none
 *
 ******************************************************************************/
void PAN_Register(tPAN_REGISTER* p_register);

/*******************************************************************************
 *
 * Function         PAN_Deregister
 *
 * Description      This function is called by the application to de-register
 *                  its callbacks with PAN profile. This will make the PAN to
 *                  become inactive. This will deregister PAN services from SDP
 *                  and close all active connections
 *
 * Returns          none
 *
 ******************************************************************************/
void PAN_Deregister(void);

/*******************************************************************************
 *
 * Function         PAN_SetRole
 *
 * Description      This function is called by the application to set the PAN
 *                  profile role. This should be called after PAN_Register.
 *                  This can be called any time to change the PAN role
 *
 * Parameters:      role        - is bit map of roles to be active
 *                                      PAN_ROLE_CLIENT is for PANU role
 *                                      PAN_ROLE_NAP_SERVER is for NAP role
 *                  sec_mask    - Security mask for different roles
 *                                      It is array of uint8_t. The bytes
 *                                      represent the security for roles PANU,
 *                                      GN and NAP in order
 *
 *                  p_user_name - Service name for PANU role
 *                  p_nap_name  - Service name for NAP role
 *                                  Can be NULL if user wants it to be default
 *
 * Returns          PAN_SUCCESS     - if the role is set successfully
 *                  PAN_FAILURE     - if the role is not valid
 *
 ******************************************************************************/
tPAN_RESULT PAN_SetRole(uint8_t role, std::string user_name, std::string nap_name);

/*******************************************************************************
 *
 * Function         PAN_Connect
 *
 * Description      This function is called by the application to initiate a
 *                  connection to the remote device
 *
 * Parameters:      rem_bda     - BD Addr of the remote device
 *                  src_role    - Role of the local device for the connection
 *                  dst_role    - Role of the remote device for the connection
 *                                      PAN_ROLE_CLIENT is for PANU role
 *                                      PAN_ROLE_NAP_SERVER is for NAP role
 *                  *handle     - Pointer for returning Handle to the connection
 *
 * Returns          PAN_SUCCESS - if the connection is initiated successfully
 *                  PAN_NO_RESOURCES - resources are not sufficient
 *                  PAN_FAILURE      - if the connection cannot be initiated
 *                                     this can be because of the combination of
 *                                     src and dst roles may not be valid or
 *                                     allowed at that point of time
 *
 ******************************************************************************/
tPAN_RESULT PAN_Connect(const RawAddress& rem_bda, tPAN_ROLE src_role, tPAN_ROLE dst_role,
                        uint16_t* handle);

/*******************************************************************************
 *
 * Function         PAN_Disconnect
 *
 * Description      This is used to disconnect the connection
 *
 * Parameters:      handle           - handle for the connection
 *
 * Returns          PAN_SUCCESS      - if the connection is closed successfully
 *                  PAN_FAILURE      - if the connection is not found or
 *                                           there is an error in disconnecting
 *
 ******************************************************************************/
tPAN_RESULT PAN_Disconnect(uint16_t handle);

/*******************************************************************************
 *
 * Function         PAN_Write
 *
 * Description      This sends data over the PAN connections. If this is called
 *                  on GN or NAP side and the packet is multicast or broadcast
 *                  it will be sent on all the links. Otherwise the correct link
 *                  is found based on the destination address and forwarded on
 *                  it. If the return value is not PAN_SUCCESS the application
 *                  should take care of releasing the message buffer
 *
 * Parameters:      dst      - MAC or BD Addr of the destination device
 *                  src      - MAC or BD Addr of the source who sent this packet
 *                  protocol - protocol of the ethernet packet like IP or ARP
 *                  p_data   - pointer to the data
 *                  len      - length of the data
 *                  ext      - to indicate that extension headers present
 *
 * Returns          PAN_SUCCESS       - if the data is sent successfully
 *                  PAN_FAILURE       - if the connection is not found or
 *                                           there is an error in sending data
 *
 ******************************************************************************/
tPAN_RESULT PAN_Write(uint16_t handle, const RawAddress& dst, const RawAddress& src,
                      uint16_t protocol, uint8_t* p_data, uint16_t len, bool ext);

/*******************************************************************************
 *
 * Function         PAN_WriteBuf
 *
 * Description      This sends data over the PAN connections. If this is called
 *                  on GN or NAP side and the packet is multicast or broadcast
 *                  it will be sent on all the links. Otherwise the correct link
 *                  is found based on the destination address and forwarded on
 *                  it. If the return value is not PAN_SUCCESS the application
 *                  should take care of releasing the message buffer
 *
 * Parameters:      dst      - MAC or BD Addr of the destination device
 *                  src      - MAC or BD Addr of the source who sent this packet
 *                  protocol - protocol of the ethernet packet like IP or ARP
 *                  p_buf    - pointer to the data buffer
 *                  ext      - to indicate that extension headers present
 *
 * Returns          PAN_SUCCESS       - if the data is sent successfully
 *                  PAN_FAILURE       - if the connection is not found or
 *                                           there is an error in sending data
 *
 ******************************************************************************/
tPAN_RESULT PAN_WriteBuf(uint16_t handle, const RawAddress& dst, const RawAddress& src,
                         uint16_t protocol, BT_HDR* p_buf, bool ext);

/*******************************************************************************
 *
 * Function         PAN_SetProtocolFilters
 *
 * Description      This function is used to set protocol filters on the peer
 *
 * Parameters:      handle      - handle for the connection
 *                  num_filters - number of protocol filter ranges
 *                  start       - array of starting protocol numbers
 *                  end         - array of ending protocol numbers
 *
 *
 * Returns          PAN_SUCCESS     if protocol filters are set successfully
 *                  PAN_FAILURE     if connection not found or error in setting
 *
 ******************************************************************************/
tPAN_RESULT PAN_SetProtocolFilters(uint16_t handle, uint16_t num_filters, uint16_t* p_start_array,
                                   uint16_t* p_end_array);

/*******************************************************************************
 *
 * Function         PAN_SetMulticastFilters
 *
 * Description      This function is used to set multicast filters on the peer
 *
 * Parameters:      handle      - handle for the connection
 *                  num_filters - number of multicast filter ranges
 *                  p_start_array - Pointer to sequence of beginings of all
 *                                         multicast address ranges
 *                  p_end_array   - Pointer to sequence of ends of all
 *                                         multicast address ranges
 *
 *
 * Returns          PAN_SUCCESS     if multicast filters are set successfully
 *                  PAN_FAILURE     if connection not found or error in setting
 *
 ******************************************************************************/
tPAN_RESULT PAN_SetMulticastFilters(uint16_t handle, uint16_t num_mcast_filters,
                                    uint8_t* p_start_array, uint8_t* p_end_array);

/*******************************************************************************
 *
 * Function         PAN_Init
 *
 * Description      This function initializes the PAN unit. It should be called
 *                  before accessing any other APIs to initialize the control
 *                  block.
 *
 * Returns          void
 *
 ******************************************************************************/
void PAN_Init(void);

void PAN_Dumpsys(int fd);

namespace std {
template <>
struct formatter<tPAN_RESULT> : enum_formatter<tPAN_RESULT> {};
}  // namespace std

#endif /* PAN_API_H */
