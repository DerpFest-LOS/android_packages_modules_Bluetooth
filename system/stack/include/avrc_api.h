/******************************************************************************
 *
 *  Copyright 2006-2012 Broadcom Corporation
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
 *  nterface to AVRCP Application Programming Interface
 *
 ******************************************************************************/
#ifndef AVRC_API_H
#define AVRC_API_H

#include <base/functional/callback.h>

#include <cstdint>

#include "stack/include/avct_api.h"
#include "stack/include/avrc_defs.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/sdp_status.h"
#include "stack/sdp/sdp_discovery_db.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  constants
 ****************************************************************************/

/* API function return value result codes. */
/* 0 Function successful */
#define AVRC_SUCCESS AVCT_SUCCESS
/* 1 Not enough resources */
#define AVRC_NO_RESOURCES AVCT_NO_RESOURCES
/* 2 Bad handle */
#define AVRC_BAD_HANDLE AVCT_BAD_HANDLE
/* 3 PID already in use */
#define AVRC_PID_IN_USE AVCT_PID_IN_USE
/* 4 Connection not open */
#define AVRC_NOT_OPEN AVCT_NOT_OPEN
/* 5 the message length exceed the MTU of the browsing channel */
#define AVRC_MSG_TOO_BIG 5
/* 0x10 generic failure */
#define AVRC_FAIL 0x10
/* 0x11 bad parameter   */
#define AVRC_BAD_PARAM 0x11

/* Control role - same as AVCT_TARGET/AVCT_CONTROL */
/* target  */
#define AVRC_CT_TARGET 1
/* controller  */
#define AVRC_CT_CONTROL 2
/* If conflict, allow the other side to succeed  */
#define AVRC_CT_PASSIVE 4

/* AVRC CTRL events */
/* AVRC_OPEN_IND_EVT event is sent when the connection is successfully opened.
 * This eventis sent in response to an AVRC_Open(). */
#define AVRC_OPEN_IND_EVT 0

/* AVRC_CLOSE_IND_EVT event is sent when a connection is closed.
 * This event can result from a call to AVRC_Close() or when the peer closes
 * the connection.  It is also sent when a connection attempted through
 * AVRC_Open() fails. */
#define AVRC_CLOSE_IND_EVT 1

/* AVRC_CONG_IND_EVT event indicates that AVCTP is congested and cannot send
 * any more messages. */
#define AVRC_CONG_IND_EVT 2

/* AVRC_UNCONG_IND_EVT event indicates that AVCTP is uncongested and ready to
 * send messages. */
#define AVRC_UNCONG_IND_EVT 3

/* AVRC_BROWSE_OPEN_IND_EVT event is sent when the browse channel is
 * successfully opened.
 * This eventis sent in response to an AVRC_Open() or AVRC_OpenBrowse() . */
#define AVRC_BROWSE_OPEN_IND_EVT 4

/* AVRC_BROWSE_CLOSE_IND_EVT event is sent when a browse channel is closed.
 * This event can result from a call to AVRC_Close(), AVRC_CloseBrowse() or
 * when the peer closes the connection.  It is also sent when a connection
 * attempted through AVRC_OpenBrowse() fails. */
#define AVRC_BROWSE_CLOSE_IND_EVT 5

/* AVRC_BROWSE_CONG_IND_EVT event indicates that AVCTP browse channel is
 * congested and cannot send any more messages. */
#define AVRC_BROWSE_CONG_IND_EVT 6

/* AVRC_BROWSE_UNCONG_IND_EVT event indicates that AVCTP browse channel is
 * uncongested and ready to send messages. */
#define AVRC_BROWSE_UNCONG_IND_EVT 7

/* AVRC_CMD_TIMEOUT_EVT event indicates timeout waiting for AVRC command
 * response from the peer */
#define AVRC_CMD_TIMEOUT_EVT 8

/* Configurable avrcp version key and constant values */
#ifndef AVRC_VERSION_PROPERTY
#define AVRC_VERSION_PROPERTY "persist.bluetooth.avrcpversion"
#endif

/* Configurable avrcp control version key */
#ifndef AVRC_CONTROL_VERSION_PROPERTY
#define AVRC_CONTROL_VERSION_PROPERTY "persist.bluetooth.avrcpcontrolversion"
#endif

#ifndef AVRC_1_6_STRING
#define AVRC_1_6_STRING "avrcp16"
#endif

#ifndef AVRC_1_5_STRING
#define AVRC_1_5_STRING "avrcp15"
#endif

#ifndef AVRC_1_4_STRING
#define AVRC_1_4_STRING "avrcp14"
#endif

#ifndef AVRC_1_3_STRING
#define AVRC_1_3_STRING "avrcp13"
#endif

#ifndef AVRC_DEFAULT_VERSION
#define AVRC_DEFAULT_VERSION AVRC_1_5_STRING
#endif

/* Configurable dynamic avrcp version enable key*/
#ifndef AVRC_DYNAMIC_AVRCP_ENABLE_PROPERTY
#define AVRC_DYNAMIC_AVRCP_ENABLE_PROPERTY "persist.bluetooth.dynamic_avrcp.enable"
#endif

/* Supported categories */
#define AVRC_SUPF_CT_CAT1 0x0001         /* Category 1 */
#define AVRC_SUPF_CT_CAT2 0x0002         /* Category 2 */
#define AVRC_SUPF_CT_CAT3 0x0004         /* Category 3 */
#define AVRC_SUPF_CT_CAT4 0x0008         /* Category 4 */
#define AVRC_SUPF_CT_APP_SETTINGS 0x0010 /* Player Application Settings */
#define AVRC_SUPF_CT_GROUP_NAVI 0x0020   /* Group Navigation */
#define AVRC_SUPF_CT_BROWSE 0x0040       /* Browsing */

/* Cover Art, get image property */
#define AVRC_SUPF_CT_COVER_ART_GET_IMAGE_PROP 0x0080
/* Cover Art, get image */
#define AVRC_SUPF_CT_COVER_ART_GET_IMAGE 0x0100
/* Cover Art, get Linked Thumbnail */
#define AVRC_SUPF_CT_COVER_ART_GET_THUMBNAIL 0x0200

#define AVRC_SUPF_TG_CAT1 0x0001             /* Category 1 */
#define AVRC_SUPF_TG_CAT2 0x0002             /* Category 2 */
#define AVRC_SUPF_TG_CAT3 0x0004             /* Category 3 */
#define AVRC_SUPF_TG_CAT4 0x0008             /* Category 4 */
#define AVRC_SUPF_TG_APP_SETTINGS 0x0010     /* Player Application Settings */
#define AVRC_SUPF_TG_GROUP_NAVI 0x0020       /* Group Navigation */
#define AVRC_SUPF_TG_BROWSE 0x0040           /* Browsing */
#define AVRC_SUPF_TG_MULTI_PLAYER 0x0080     /* Muliple Media Player */
#define AVRC_SUPF_TG_PLAYER_COVER_ART 0x0100 /* Cover Art */

#define AVRC_META_SUCCESS AVRC_SUCCESS
#define AVRC_META_FAIL AVRC_FAIL
#define AVRC_METADATA_CMD 0x0000
#define AVRC_METADATA_RESP 0x0001

#define AVRCP_SUPPORTED_FEATURES_POSITION 1
#define AVRCP_BROWSE_SUPPORT_BITMASK 0x40
#define AVRCP_MULTI_PLAYER_SUPPORT_BITMASK 0x80
#define AVRCP_CA_SUPPORT_BITMASK 0x01

#define AVRCP_FEAT_CA_BIT 0x0180
#define AVRCP_FEAT_BRW_BIT 0x0040

/*****************************************************************************
 *  data type definitions
 ****************************************************************************/

/* This data type is used in AVRC_FindService() to initialize the SDP database
 * to hold the result service search. */
typedef struct {
  uint32_t db_len;         /* Length, in bytes, of the discovery database */
  tSDP_DISCOVERY_DB* p_db; /* Pointer to the discovery database */
  uint16_t num_attr;       /* The number of attributes in p_attrs */
  uint16_t* p_attrs;       /* The attributes filter. If NULL, AVRCP API sets the
                            * attribute filter
                            * to be ATTR_ID_SERVICE_CLASS_ID_LIST,
                            * ATTR_ID_BT_PROFILE_DESC_LIST,
                            * ATTR_ID_SUPPORTED_FEATURES, ATTR_ID_SERVICE_NAME,
                            * ATTR_ID_PROVIDER_NAME.
                            * If not NULL, the input is taken as the filter. */
} tAVRC_SDP_DB_PARAMS;

/* This callback function returns service discovery information to the
 * application after the AVRC_FindService() API function is called.  The
 * implementation of this callback function must copy the p_service_name
 * and p_provider_name parameters passed to it as they are not guaranteed
 * to remain after the callback function exits. */
using tAVRC_FIND_CBACK = base::Callback<void(tSDP_STATUS status)>;

/* This is the control callback function.  This function passes events
 * listed in Table 20 to the application. */
using tAVRC_CTRL_CBACK = base::Callback<void(uint8_t handle, uint8_t event, uint16_t result,
                                             const RawAddress* peer_addr)>;

/* This is the message callback function.  It is executed when AVCTP has
 * a message packet ready for the application.  The implementation of this
 * callback function must copy the tAVRC_MSG structure passed to it as it
 * is not guaranteed to remain after the callback function exits. */
using tAVRC_MSG_CBACK =
        base::Callback<void(uint8_t handle, uint8_t label, uint8_t opcode, tAVRC_MSG* p_msg)>;

typedef struct {
  tAVRC_CTRL_CBACK ctrl_cback; /* application control callback */
  tAVRC_MSG_CBACK msg_cback;   /* application message callback */
  uint32_t company_id;         /* the company ID  */
  tAVCT_ROLE conn;             /* Connection role (Initiator/acceptor) */
  uint8_t control;             /* Control role (Control/Target) */
} tAVRC_CONN_CB;

typedef struct {
  uint8_t handle;
  uint8_t label;
  uint8_t msg_mask;
} tAVRC_PARAM;

/*****************************************************************************
 *  external function declarations
 ****************************************************************************/
/******************************************************************************
 *
 * Function         avrcp_absolute_volume_is_enabled
 *
 * Description      Check if config support advance control (absolute volume)
 *
 * Returns          return true if absolute_volume is enabled
 *
 *****************************************************************************/
bool avrcp_absolute_volume_is_enabled();

/******************************************************************************
 *
 * Function         AVRC_GetControlProfileVersion
 *
 * Description      Get the overlaid AVRCP control profile version
 *
 * Returns          The AVRCP control profile version
 *
 *****************************************************************************/
uint16_t AVRC_GetControlProfileVersion();

/******************************************************************************
 *
 * Function         ARVC_GetProfileVersion
 *
 * Description      Get the user assigned AVRCP profile version
 *
 * Returns          The AVRCP profile version
 *
 *****************************************************************************/
uint16_t AVRC_GetProfileVersion();

/******************************************************************************
 *
 * Function         AVRC_AddRecord
 *
 * Description      This function is called to build an AVRCP SDP record.
 *                  Prior to calling this function the application must
 *                  call SDP_CreateRecord() to create an SDP record.
 *
 *                  Input Parameters:
 *                      service_uuid:  Indicates
 *                                       TG(UUID_SERVCLASS_AV_REM_CTRL_TARGET)
 *                                    or CT(UUID_SERVCLASS_AV_REMOTE_CONTROL)
 *
 *                      p_service_name:  Pointer to a null-terminated character
 *                      string containing the service name.
 *                      If service name is not used set this to NULL.
 *
 *                      p_provider_name:  Pointer to a null-terminated character
 *                      string containing the provider name.
 *                      If provider name is not used set this to NULL.
 *
 *                      categories:  Supported categories.
 *
 *                      sdp_handle:  SDP handle returned by SDP_CreateRecord().
 *
 *                      browse_supported:  browse support info.
 *
 *                      profile_version:  profile version of avrcp record.
 *
 *                      cover_art_psm: The PSM of a cover art service, if
 *                      supported. Use 0 Otherwise. Ignored on controller
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_NO_RESOURCES if not enough resources to build the SDP
 *                                    record.
 *
 *****************************************************************************/
uint16_t AVRC_AddRecord(uint16_t service_uuid, const char* p_service_name,
                        const char* p_provider_name, uint16_t categories, uint32_t sdp_handle,
                        bool browse_supported, uint16_t profile_version, uint16_t cover_art_psm);

/*******************************************************************************
 *
 * Function          AVRC_RemoveRecord
 *
 * Description       This function is called to remove an AVRCP SDP record.
 *
 *                   Input Parameters:
 *                       sdp_handle:  Handle you used with AVRC_AddRecord
 *
 * Returns           AVRC_SUCCESS if successful.
 *                   AVRC_FAIL otherwise
 *
 *******************************************************************************/
uint16_t AVRC_RemoveRecord(uint32_t sdp_handle);

/******************************************************************************
 *
 * Function         AVRC_FindService
 *
 * Description      This function is called by the application to perform
 *                  service discovery and retrieve AVRCP SDP record information
 *                  from a peer device.  Information is returned for the first
 *                  service record found on the server that matches the service
 *                  UUID. The callback function will be executed when service
 *                  discovery is complete.  There can only be one outstanding
 *                  call to AVRC_FindService() at a time; the application must
 *                  wait for the callback before it makes another call to the
 *                  function. The application is responsible for allocating
 *                  memory for the discovery database.  It is recommended that
 *                  the size of the discovery database be at least 300 bytes.
 *                  The application can deallocate the memory after the
 *                  callback function has executed.
 *
 *                  Input Parameters:
 *                      service_uuid: Indicates
 *                                       TG(UUID_SERVCLASS_AV_REM_CTRL_TARGET)
 *                                    or CT(UUID_SERVCLASS_AV_REMOTE_CONTROL)
 *
 *                      bd_addr:  BD address of the peer device.
 *
 *                      p_db:  SDP discovery database parameters.
 *
 *                      p_cback:  Pointer to the callback function.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_PARAMS if discovery database parameters are
 *                                  invalid.
 *                  AVRC_NO_RESOURCES if there are not enough resources to
 *                                    perform the service search.
 *
 *****************************************************************************/
uint16_t AVRC_FindService(uint16_t service_uuid, const RawAddress& bd_addr,
                          tAVRC_SDP_DB_PARAMS* p_db, const tAVRC_FIND_CBACK& cback);

/******************************************************************************
 *
 * Function         AVRC_Open
 *
 * Description      This function is called to open a connection to AVCTP.
 *                  The connection can be either an initiator or acceptor, as
 *                  determined by the p_ccb->stream parameter.
 *                  The connection can be a target, a controller or for both
 *                  roles, as determined by the p_ccb->control parameter.
 *                  By definition, a target connection is an acceptor connection
 *                  that waits for an incoming AVCTP connection from the peer.
 *                  The connection remains available to the application until
 *                  the application closes it by calling AVRC_Close().  The
 *                  application does not need to reopen the connection after an
 *                  AVRC_CLOSE_IND_EVT is received.
 *
 *                  Input Parameters:
 *                      p_ccb->company_id: Company Identifier.
 *
 *                      p_ccb->p_ctrl_cback:  Pointer to the control callback
 *                                            function.
 *
 *                      p_ccb->p_msg_cback:  Pointer to the message callback
 *                                           function.
 *
 *                      p_ccb->conn: AVCTP connection role.  This is set to
 *                      AVCTP_INT for initiator connections and AVCTP_ACP
 *                      for acceptor connections.
 *
 *                      p_ccb->control: Control role.  This is set to
 *                      AVRC_CT_TARGET for target connections, AVRC_CT_CONTROL
 *                      for control connections or
 *                      (AVRC_CT_TARGET|AVRC_CT_CONTROL) for connections that
 *                      support both roles.
 *
 *                      peer_addr: BD address of peer device.  This value is
 *                      only used for initiator connections; for acceptor
 *                      connections it can be set to NULL.
 *
 *                  Output Parameters:
 *                      p_handle: Pointer to handle.  This parameter is only
 *                                valid if AVRC_SUCCESS is returned.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_NO_RESOURCES if there are not enough resources to open
 *                  the connection.
 *
 *****************************************************************************/
uint16_t AVRC_Open(uint8_t* p_handle, tAVRC_CONN_CB* p_ccb, const RawAddress& peer_addr);

/******************************************************************************
 *
 * Function         AVRC_Close
 *
 * Description      Close a connection opened with AVRC_Open().
 *                  This function is called when the
 *                  application is no longer using a connection.
 *
 *                  Input Parameters:
 *                      handle: Handle of this connection.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_Close(uint8_t handle);

/******************************************************************************
 *
 * Function         AVRC_OpenBrowse
 *
 * Description      This function is called to open a browsing connection to
 *                  AVCTP. The connection can be either an initiator or
 *                  acceptor, as determined by the conn_role.
 *                  The handle is returned by a previous call to AVRC_Open.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_NO_RESOURCES if there are not enough resources to open
 *                  the connection.
 *
 *****************************************************************************/
uint16_t AVRC_OpenBrowse(uint8_t handle, tAVCT_ROLE conn_role);

/******************************************************************************
 *
 * Function         AVRC_CloseBrowse
 *
 * Description      Close a connection opened with AVRC_OpenBrowse().
 *                  This function is called when the
 *                  application is no longer using a connection.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_CloseBrowse(uint8_t handle);

/******************************************************************************
 *
 * Function         AVRC_MsgReq
 *
 * Description      This function is used to send the AVRCP byte stream in p_pkt
 *                  down to AVCTP.
 *
 *                  It is expected that:
 *                  p_pkt->offset is at least AVCT_MSG_OFFSET
 *                  p_pkt->layer_specific is AVCT_DATA_CTRL or AVCT_DATA_BROWSE
 *                  p_pkt->event is AVRC_OP_VENDOR, AVRC_OP_PASS_THRU or
 *                                  AVRC_OP_BROWSING
 *                  The above BT_HDR settings are set by the AVRC_Bld*
 *                  functions.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_MsgReq(uint8_t handle, uint8_t label, uint8_t ctype, BT_HDR* p_pkt,
                     bool is_new_avrcp);

/******************************************************************************
 *
 * Function         AVRC_SaveControllerVersion
 *
 * Description      Save AVRC controller version of peer device into bt_config.
 *                  This version is used to send same AVRC target version to
 *                  peer device to avoid version mismatch IOP issue.
 *
 *                  Input Parameters:
 *                      bdaddr: BD address of peer device.
 *
 *                      version: AVRC controller version of peer device.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          Nothing
 *
 *****************************************************************************/
void AVRC_SaveControllerVersion(const RawAddress& bdaddr, uint16_t new_version);

/******************************************************************************
 *
 * Function         AVRC_UnitCmd
 *
 * Description      Send a UNIT INFO command to the peer device.  This
 *                  function can only be called for controller role connections.
 *                  Any response message from the peer is passed back through
 *                  the tAVRC_MSG_CBACK callback function.
 *
 *                  Input Parameters:
 *                      handle: Handle of this connection.
 *
 *                      label: Transaction label.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_UnitCmd(uint8_t handle, uint8_t label);

/******************************************************************************
 *
 * Function         AVRC_SubCmd
 *
 * Description      Send a SUBUNIT INFO command to the peer device.  This
 *                  function can only be called for controller role connections.
 *                  Any response message from the peer is passed back through
 *                  the tAVRC_MSG_CBACK callback function.
 *
 *                  Input Parameters:
 *                      handle: Handle of this connection.
 *
 *                      label: Transaction label.
 *
 *                      page: Specifies which part of the subunit type table
 *                      is requested.  For AVRCP it is typically zero.
 *                      Value range is 0-7.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_SubCmd(uint8_t handle, uint8_t label, uint8_t page);

/******************************************************************************
 *
 * Function         AVRC_PassCmd
 *
 * Description      Send a PASS THROUGH command to the peer device.  This
 *                  function can only be called for controller role connections.
 *                  Any response message from the peer is passed back through
 *                  the tAVRC_MSG_CBACK callback function.
 *
 *                  Input Parameters:
 *                      handle: Handle of this connection.
 *
 *                      label: Transaction label.
 *
 *                      p_msg: Pointer to PASS THROUGH message structure.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_PassCmd(uint8_t handle, uint8_t label, tAVRC_MSG_PASS* p_msg);

/******************************************************************************
 *
 * Function         AVRC_PassRsp
 *
 * Description      Send a PASS THROUGH response to the peer device.  This
 *                  function can only be called for target role connections.
 *                  This function must be called when a PASS THROUGH command
 *                  message is received from the peer through the
 *                  tAVRC_MSG_CBACK callback function.
 *
 *                  Input Parameters:
 *                      handle: Handle of this connection.
 *
 *                      label: Transaction label.  Must be the same value as
 *                      passed with the command message in the callback
 *                      function.
 *
 *                      p_msg: Pointer to PASS THROUGH message structure.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_PassRsp(uint8_t handle, uint8_t label, tAVRC_MSG_PASS* p_msg);

/******************************************************************************
 *
 * Function         AVRC_VendorCmd
 *
 * Description      Send a VENDOR DEPENDENT command to the peer device.  This
 *                  function can only be called for controller role connections.
 *                  Any response message from the peer is passed back through
 *                  the tAVRC_MSG_CBACK callback function.
 *
 *                  Input Parameters:
 *                      handle: Handle of this connection.
 *
 *                      label: Transaction label.
 *
 *                      p_msg: Pointer to VENDOR DEPENDENT message structure.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_VendorCmd(uint8_t handle, uint8_t label, tAVRC_MSG_VENDOR* p_msg);

/******************************************************************************
 *
 * Function         AVRC_VendorRsp
 *
 * Description      Send a VENDOR DEPENDENT response to the peer device.  This
 *                  function can only be called for target role connections.
 *                  This function must be called when a VENDOR DEPENDENT
 *                  command message is received from the peer through the
 *                  tAVRC_MSG_CBACK callback function.
 *
 *                  Input Parameters:
 *                      handle: Handle of this connection.
 *
 *                      label: Transaction label.  Must be the same value as
 *                      passed with the command message in the callback
 *                      function.
 *
 *                      p_msg: Pointer to VENDOR DEPENDENT message structure.
 *
 *                  Output Parameters:
 *                      None.
 *
 * Returns          AVRC_SUCCESS if successful.
 *                  AVRC_BAD_HANDLE if handle is invalid.
 *
 *****************************************************************************/
uint16_t AVRC_VendorRsp(uint8_t handle, uint8_t label, tAVRC_MSG_VENDOR* p_msg);

/*******************************************************************************
 *
 * Function         AVRC_Init
 *
 * Description      This function is called at stack startup to allocate the
 *                  control block (if using dynamic memory), and initializes the
 *                  control block and tracing level.
 *
 * Returns          void
 *
 ******************************************************************************/
void AVRC_Init(void);

/*******************************************************************************
 *
 * Function         AVRC_Ctrl_ParsCommand
 *
 * Description      This function is used to parse cmds received for CTRL
 *                  Currently it is for SetAbsVolume and Volume Change
 *                      Notification..
 *
 * Returns          AVRC_STS_NO_ERROR, if the message in p_data is parsed
 *                      successfully.
 *                  Otherwise, the error code defined by AVRCP 1.4
 *
 ******************************************************************************/
tAVRC_STS AVRC_Ctrl_ParsCommand(tAVRC_MSG* p_msg, tAVRC_COMMAND* p_result);

/*******************************************************************************
 *
 * Function         AVRC_ParsCommand
 *
 * Description      This function is used to parse the received command.
 *
 * Returns          AVRC_STS_NO_ERROR, if the message in p_data is parsed
 *                      successfully.
 *                  Otherwise, the error code defined by AVRCP 1.4
 *
 ******************************************************************************/
tAVRC_STS AVRC_ParsCommand(tAVRC_MSG* p_msg, tAVRC_COMMAND* p_result, uint8_t* p_buf,
                           uint16_t buf_len);

/*******************************************************************************
 *
 * Function         AVRC_ParsResponse
 *
 * Description      This function is used to parse the received response.
 *
 * Returns          AVRC_STS_NO_ERROR, if the message in p_data is parsed
 *                      successfully.
 *                  Otherwise, the error code defined by AVRCP 1.4
 *
 ******************************************************************************/
tAVRC_STS AVRC_ParsResponse(tAVRC_MSG* p_msg, tAVRC_RESPONSE* p_result, uint8_t* p_buf,
                            uint16_t buf_len);

/*******************************************************************************
 *
 * Function         AVRC_Ctrl_ParsResponse
 *
 * Description      This function is a parse response for AVRCP Controller.
 *
 * Returns          AVRC_STS_NO_ERROR, if the message in p_data is parsed
 *                      successfully.
 *                  Otherwise, the error code defined by AVRCP 1.4
 *
 ******************************************************************************/
tAVRC_STS AVRC_Ctrl_ParsResponse(tAVRC_MSG* p_msg, tAVRC_RESPONSE* p_result, uint8_t* p_buf,
                                 uint16_t* buf_len);

/*******************************************************************************
 *
 * Function         AVRC_BldCommand
 *
 * Description      This function builds the given AVRCP command to the given
 *                  GKI buffer
 *
 * Returns          AVRC_STS_NO_ERROR, if the command is built successfully
 *                  Otherwise, the error code.
 *
 ******************************************************************************/
tAVRC_STS AVRC_BldCommand(tAVRC_COMMAND* p_cmd, BT_HDR** pp_pkt);

/*******************************************************************************
 *
 * Function         AVRC_BldResponse
 *
 * Description      This function builds the given AVRCP response to the given
 *                  GKI buffer
 *
 * Returns          AVRC_STS_NO_ERROR, if the response is built successfully
 *                  Otherwise, the error code.
 *
 ******************************************************************************/
tAVRC_STS AVRC_BldResponse(uint8_t handle, tAVRC_RESPONSE* p_rsp, BT_HDR** pp_pkt);

/**************************************************************************
 *
 * Function         AVRC_IsValidAvcType
 *
 * Description      Check if correct AVC type is specified
 *
 * Returns          returns true if it is valid
 *
 *
 ******************************************************************************/
bool AVRC_IsValidAvcType(uint8_t pdu_id, uint8_t avc_type);

/*******************************************************************************
 *
 * Function         AVRC_IsValidPlayerAttr
 *
 * Description      Check if the given attrib value is a valid one
 *
 *
 * Returns          returns true if it is valid
 *
 ******************************************************************************/
bool AVRC_IsValidPlayerAttr(uint8_t attr);

void AVRC_UpdateCcb(RawAddress* addr, uint32_t company_id);

#endif /* AVRC_API_H */
