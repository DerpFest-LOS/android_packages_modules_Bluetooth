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
 *  This is the public interface file the BTA Java I/F
 *
 ******************************************************************************/
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "bta/include/bta_api.h"
#include "bta/include/bta_sec_api.h"
#include "include/macros.h"
#include "internal_include/bt_target.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/l2cap_types.h"
#include "stack/include/rfcdefs.h"
#include "types/bluetooth/uuid.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  Constants and data types
 ****************************************************************************/
enum class tBTA_JV_STATUS {
  SUCCESS = 0, /* Successful operation. */
  FAILURE = 1, /* Generic failure. */
  BUSY = 2,    /* Temporarily can not handle this request. */
};

inline std::string bta_jv_status_text(const tBTA_JV_STATUS& status) {
  switch (status) {
    CASE_RETURN_TEXT(tBTA_JV_STATUS::SUCCESS);
    CASE_RETURN_TEXT(tBTA_JV_STATUS::FAILURE);
    CASE_RETURN_TEXT(tBTA_JV_STATUS::BUSY);
  }
}

#define BTA_JV_INTERNAL_ERR (-1) /* internal error. */

/* L2CAP errors from underlying layers propagated via callbacks. */
#define BTA_JV_L2CAP_REASON_EMPTY 0
#define BTA_JV_L2CAP_REASON_UNKNOWN 1
#define BTA_JV_L2CAP_REASON_ACL_FAILURE 2
#define BTA_JV_L2CAP_REASON_CL_SEC_FAILURE 3
#define BTA_JV_L2CAP_REASON_INSUFFICIENT_AUTHENTICATION 4
#define BTA_JV_L2CAP_REASON_INSUFFICIENT_AUTHORIZATION 5
#define BTA_JV_L2CAP_REASON_INSUFFICIENT_ENCRYP_KEY_SIZE 6
#define BTA_JV_L2CAP_REASON_INSUFFICIENT_ENCRYP 7
#define BTA_JV_L2CAP_REASON_INVALID_SOURCE_CID 8
#define BTA_JV_L2CAP_REASON_SOURCE_CID_ALREADY_ALLOCATED 9
#define BTA_JV_L2CAP_REASON_UNACCEPTABLE_PARAMETERS 10
#define BTA_JV_L2CAP_REASON_INVALID_PARAMETERS 11
#define BTA_JV_L2CAP_REASON_NO_RESOURCES 12
#define BTA_JV_L2CAP_REASON_NO_PSM 13
#define BTA_JV_L2CAP_REASON_TIMEOUT 14

typedef uint8_t tBTA_JV_L2CAP_REASON;

#define BTA_JV_MAX_UUIDS SDP_MAX_UUID_FILTERS
#define BTA_JV_MAX_ATTRS SDP_MAX_ATTR_FILTERS
#define BTA_JV_MAX_SDP_REC SDP_MAX_RECORDS
/* GAP handle is used as index, hence do not change this value */
#define BTA_JV_MAX_L2C_CONN GAP_MAX_CONNECTIONS
#define BTA_JV_MAX_RFC_CONN MAX_RFC_PORTS

#ifndef BTA_JV_DEF_RFC_MTU
#define BTA_JV_DEF_RFC_MTU (3 * 330)
#endif

#ifndef BTA_JV_MAX_RFC_SR_SESSION
#define BTA_JV_MAX_RFC_SR_SESSION MAX_BD_CONNECTIONS
#endif

/* BTA_JV_MAX_RFC_SR_SESSION can not be bigger than MAX_BD_CONNECTIONS */
#if (BTA_JV_MAX_RFC_SR_SESSION > MAX_BD_CONNECTIONS)
#undef BTA_JV_MAX_RFC_SR_SESSION
#define BTA_JV_MAX_RFC_SR_SESSION MAX_BD_CONNECTIONS
#endif

#define BTA_JV_FIRST_SERVICE_ID BTA_FIRST_JV_SERVICE_ID
#define BTA_JV_LAST_SERVICE_ID BTA_LAST_JV_SERVICE_ID
#define BTA_JV_NUM_SERVICE_ID (BTA_LAST_JV_SERVICE_ID - BTA_FIRST_JV_SERVICE_ID + 1)

/* Discoverable modes */
enum { BTA_JV_DISC_NONE, BTA_JV_DISC_LIMITED, BTA_JV_DISC_GENERAL };
typedef uint16_t tBTA_JV_DISC;

/* JV ID type */
#define BTA_JV_PM_ID_1 1     /* PM example profile 1 */
#define BTA_JV_PM_ID_2 2     /* PM example profile 2 */
#define BTA_JV_PM_ID_CLEAR 0 /* Special JV ID used to clear PM profile */
#define BTA_JV_PM_ALL 0xFF   /* Generic match all id, see bta_dm_cfg.c */
typedef uint8_t tBTA_JV_PM_ID;

#define BTA_JV_PM_HANDLE_CLEAR 0xFF /* Special JV ID used to clear PM profile  */

/* define maximum number of registered PM entities. should be in sync with bta
 * pm! */
#ifndef BTA_JV_PM_MAX_NUM
#define BTA_JV_PM_MAX_NUM 12
#endif

/* JV pm connection states */
typedef enum : uint8_t {
  BTA_JV_CONN_OPEN = 0, /* Connection opened state */
  BTA_JV_CONN_CLOSE,    /* Connection closed state */
  BTA_JV_APP_OPEN,      /* JV Application opened state */
  BTA_JV_APP_CLOSE,     /* JV Application closed state */
  BTA_JV_SCO_OPEN,      /* SCO connection opened state */
  BTA_JV_SCO_CLOSE,     /* SCO connection opened state */
  BTA_JV_CONN_IDLE,     /* Connection idle state */
  BTA_JV_CONN_BUSY,     /* Connection busy state */
  BTA_JV_MAX_CONN_STATE /* Max number of connection state */
} tBTA_JV_CONN_STATE;

inline std::string bta_jv_conn_state_text(const tBTA_JV_CONN_STATE& state) {
  switch (state) {
    CASE_RETURN_STRING(BTA_JV_CONN_OPEN);
    CASE_RETURN_STRING(BTA_JV_CONN_CLOSE);
    CASE_RETURN_STRING(BTA_JV_APP_OPEN);
    CASE_RETURN_STRING(BTA_JV_APP_CLOSE);
    CASE_RETURN_STRING(BTA_JV_SCO_OPEN);
    CASE_RETURN_STRING(BTA_JV_SCO_CLOSE);
    CASE_RETURN_STRING(BTA_JV_CONN_IDLE);
    CASE_RETURN_STRING(BTA_JV_CONN_BUSY);
    CASE_RETURN_STRING(BTA_JV_MAX_CONN_STATE);
    default:
      RETURN_UNKNOWN_TYPE_STRING(tBTA_JV_CONN_STATE, state);
  }
}

namespace std {
template <>
struct formatter<tBTA_JV_CONN_STATE> : enum_formatter<tBTA_JV_CONN_STATE> {};
}  // namespace std

/* JV Connection types */
enum class tBTA_JV_CONN_TYPE {
  UNKNOWN = -1,
  RFCOMM = 0,
  L2CAP = 1,
  L2CAP_LE = 2,
};

inline std::string bta_jv_conn_type_text(const tBTA_JV_CONN_TYPE& type) {
  switch (type) {
    CASE_RETURN_STRING(tBTA_JV_CONN_TYPE::UNKNOWN);
    CASE_RETURN_STRING(tBTA_JV_CONN_TYPE::RFCOMM);
    CASE_RETURN_STRING(tBTA_JV_CONN_TYPE::L2CAP);
    CASE_RETURN_STRING(tBTA_JV_CONN_TYPE::L2CAP_LE);
  }
  RETURN_UNKNOWN_TYPE_STRING(tBTA_JV_CONN_TYPE, type);
}

namespace std {
template <>
struct formatter<tBTA_JV_CONN_TYPE> : enum_formatter<tBTA_JV_CONN_TYPE> {};
}  // namespace std

enum tBTA_JV_EVT : uint16_t {
  /* Java I/F callback events */
  /* events received by tBTA_JV_DM_CBACK */
  BTA_JV_ENABLE_EVT = 0,          // JV enabled
  BTA_JV_GET_SCN_EVT = 6,         // Reserved an SCN
  BTA_JV_GET_PSM_EVT = 7,         // Reserved a PSM
  BTA_JV_DISCOVERY_COMP_EVT = 8,  // SDP discovery complete
  BTA_JV_CREATE_RECORD_EVT = 11,  // the result for BTA_JvCreateRecord
  /* events received by tBTA_JV_L2CAP_CBACK */
  BTA_JV_L2CAP_OPEN_EVT = 16,      // open status of L2CAP connection
  BTA_JV_L2CAP_CLOSE_EVT = 17,     // L2CAP connection closed
  BTA_JV_L2CAP_START_EVT = 18,     // L2CAP server started
  BTA_JV_L2CAP_CL_INIT_EVT = 19,   // L2CAP client initiated a connection
  BTA_JV_L2CAP_DATA_IND_EVT = 20,  // L2CAP connection received data
  BTA_JV_L2CAP_CONG_EVT = 21,      // L2CAP connection congestion status changed
  BTA_JV_L2CAP_READ_EVT = 22,      // the result for BTA_JvL2capRead
  BTA_JV_L2CAP_WRITE_EVT = 24,     // the result for BTA_JvL2capWrite

  /* events received by tBTA_JV_RFCOMM_CBACK */
  BTA_JV_RFCOMM_OPEN_EVT = 26,      // open status of RFCOMM Client connection
  BTA_JV_RFCOMM_CLOSE_EVT = 27,     // RFCOMM connection closed
  BTA_JV_RFCOMM_START_EVT = 28,     // RFCOMM server started
  BTA_JV_RFCOMM_CL_INIT_EVT = 29,   // RFCOMM client initiated a connection
  BTA_JV_RFCOMM_DATA_IND_EVT = 30,  // RFCOMM connection received data
  BTA_JV_RFCOMM_CONG_EVT = 31,      // RFCOMM connection congestion status changed
  BTA_JV_RFCOMM_WRITE_EVT = 33,     // the result for BTA_JvRfcommWrite
  BTA_JV_RFCOMM_SRV_OPEN_EVT = 34,  // open status of Server RFCOMM connection
  BTA_JV_MAX_EVT = 35,              // max number of JV events
};

inline std::string bta_jv_event_text(const tBTA_JV_EVT& event) {
  switch (event) {
    CASE_RETURN_TEXT(BTA_JV_ENABLE_EVT);
    CASE_RETURN_TEXT(BTA_JV_GET_SCN_EVT);
    CASE_RETURN_TEXT(BTA_JV_GET_PSM_EVT);
    CASE_RETURN_TEXT(BTA_JV_DISCOVERY_COMP_EVT);
    CASE_RETURN_TEXT(BTA_JV_CREATE_RECORD_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_OPEN_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_CLOSE_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_START_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_CL_INIT_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_DATA_IND_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_CONG_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_READ_EVT);
    CASE_RETURN_TEXT(BTA_JV_L2CAP_WRITE_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_OPEN_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_CLOSE_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_START_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_CL_INIT_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_DATA_IND_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_CONG_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_WRITE_EVT);
    CASE_RETURN_TEXT(BTA_JV_RFCOMM_SRV_OPEN_EVT);
    CASE_RETURN_TEXT(BTA_JV_MAX_EVT);
    default:
      return base::StringPrintf("UNKNOWN[%hu]", event);
  }
}

/* data associated with BTA_JV_SET_DISCOVER_EVT */
typedef struct {
  tBTA_JV_STATUS status;  /* Whether the operation succeeded or failed. */
  tBTA_JV_DISC disc_mode; /* The current discoverable mode */
} tBTA_JV_SET_DISCOVER;

/* data associated with BTA_JV_DISCOVERY_COMP_EVT_ */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  int scn;               /* channel # */
} tBTA_JV_DISCOVERY_COMP;

/* data associated with BTA_JV_CREATE_RECORD_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
} tBTA_JV_CREATE_RECORD;

/* data associated with BTA_JV_L2CAP_OPEN_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  RawAddress rem_bda;    /* The peer address */
  int32_t tx_mtu;        /* The transmit MTU */
  uint16_t local_cid;    /* The local CID */
  uint16_t remote_cid;   /* The remote CID */
  uint16_t local_coc_mps;     /* The local COC MPS */
  uint16_t remote_coc_mps;    /* The remote COC MPS */
  uint16_t local_coc_credit;  /* The local COC credit */
  uint16_t remote_coc_credit; /* The remote COC credit */
  uint16_t acl_handle;        /* The ACL handle */
} tBTA_JV_L2CAP_OPEN;

/* data associated with BTA_JV_L2CAP_OPEN_EVT for LE sockets */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  RawAddress rem_bda;    /* The peer address */
  int32_t tx_mtu;        /* The transmit MTU */
  void** p_p_cback;      /* set them for new socket */
  void** p_user_data;    /* set them for new socket */
} tBTA_JV_L2CAP_LE_OPEN;

/* data associated with BTA_JV_L2CAP_CLOSE_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  bool async;            /* false, if local initiates disconnect */
  /* Reason that triggered the L2CAP connection close callback.
  Used when L2CAP close callback was triggered due to a GAP error. */
  tBTA_JV_L2CAP_REASON reason;
} tBTA_JV_L2CAP_CLOSE;

/* data associated with BTA_JV_L2CAP_START_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  uint8_t sec_id;        /* security ID used by this server */
} tBTA_JV_L2CAP_START;

/* data associated with BTA_JV_L2CAP_CL_INIT_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  uint8_t sec_id;        /* security ID used by this client */
} tBTA_JV_L2CAP_CL_INIT;

/* data associated with BTA_JV_L2CAP_CONG_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  bool cong;             /* true, congested. false, uncongested */
} tBTA_JV_L2CAP_CONG;

/* data associated with BTA_JV_L2CAP_READ_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  uint32_t req_id;       /* The req_id in the associated BTA_JvL2capRead() */
  uint8_t* p_data;       /* Points to the same location as p_data parameter in BTA_JvL2capRead() */
  uint16_t len;          /* The length of the data read. */
} tBTA_JV_L2CAP_READ;

/* data associated with BTA_JV_L2CAP_WRITE_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  uint32_t req_id;       /* The req_id in the associated BTA_JvL2capWrite() */
  uint16_t len;          /* The length of the data written. */
  bool cong;             /* congestion status */
} tBTA_JV_L2CAP_WRITE;

/* data associated with BTA_JV_RFCOMM_OPEN_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  RawAddress rem_bda;    /* The peer address */
} tBTA_JV_RFCOMM_OPEN;
/* data associated with BTA_JV_RFCOMM_SRV_OPEN_EVT */
typedef struct {
  tBTA_JV_STATUS status;      /* Whether the operation succeeded or failed. */
  uint32_t handle;            /* The connection handle */
  uint32_t new_listen_handle; /* The new listen handle */
  RawAddress rem_bda;         /* The peer address */
} tBTA_JV_RFCOMM_SRV_OPEN;

/* data associated with BTA_JV_RFCOMM_CLOSE_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t port_status;  /* PORT status */
  uint32_t handle;       /* The connection handle */
  bool async;            /* false, if local initiates disconnect */
} tBTA_JV_RFCOMM_CLOSE;

/* data associated with BTA_JV_RFCOMM_START_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  uint8_t sec_id;        /* security ID used by this server */
  bool use_co;           /* true to use co_rfc_data */
} tBTA_JV_RFCOMM_START;

/* data associated with BTA_JV_RFCOMM_CL_INIT_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  uint8_t sec_id;        /* security ID used by this client */
  bool use_co;           /* true to use co_rfc_data */
} tBTA_JV_RFCOMM_CL_INIT;
/*data associated with BTA_JV_L2CAP_DATA_IND_EVT & BTA_JV_RFCOMM_DATA_IND_EVT */
typedef struct {
  uint32_t handle; /* The connection handle */
} tBTA_JV_DATA_IND;

/*data associated with BTA_JV_L2CAP_DATA_IND_EVT if used for LE */
typedef struct {
  uint32_t handle; /* The connection handle */
  BT_HDR* p_buf;   /* The incoming data */
} tBTA_JV_LE_DATA_IND;

/* data associated with BTA_JV_RFCOMM_CONG_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  bool cong;             /* true, congested. false, uncongested */
} tBTA_JV_RFCOMM_CONG;

/* data associated with BTA_JV_RFCOMM_WRITE_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Whether the operation succeeded or failed. */
  uint32_t handle;       /* The connection handle */
  uint32_t req_id;       /* The req_id in the associated BTA_JvRfcommWrite() */
  int len;               /* The length of the data written. */
  bool cong;             /* congestion status */
} tBTA_JV_RFCOMM_WRITE;

/* data associated with BTA_JV_API_SET_PM_PROFILE_EVT */
typedef struct {
  tBTA_JV_STATUS status; /* Status of the operation */
  uint32_t handle;       /* Connection handle */
  tBTA_JV_PM_ID app_id;  /* JV app ID */
} tBTA_JV_SET_PM_PROFILE;

/* data associated with BTA_JV_API_NOTIFY_PM_STATE_CHANGE_EVT */
typedef struct {
  uint32_t handle;          /* Connection handle */
  tBTA_JV_CONN_STATE state; /* JV connection stata */
} tBTA_JV_NOTIFY_PM_STATE_CHANGE;

/* union of data associated with JV callback */
typedef union {
  tBTA_JV_STATUS status;                /* BTA_JV_ENABLE_EVT */
  tBTA_JV_DISCOVERY_COMP disc_comp;     /* BTA_JV_DISCOVERY_COMP_EVT */
  tBTA_JV_SET_DISCOVER set_discover;    /* BTA_JV_SET_DISCOVER_EVT */
  uint8_t scn;                          /* BTA_JV_GET_SCN_EVT */
  uint16_t psm;                         /* BTA_JV_GET_PSM_EVT */
  tBTA_JV_CREATE_RECORD create_rec;     /* BTA_JV_CREATE_RECORD_EVT */
  tBTA_JV_L2CAP_OPEN l2c_open;          /* BTA_JV_L2CAP_OPEN_EVT */
  tBTA_JV_L2CAP_CLOSE l2c_close;        /* BTA_JV_L2CAP_CLOSE_EVT */
  tBTA_JV_L2CAP_START l2c_start;        /* BTA_JV_L2CAP_START_EVT */
  tBTA_JV_L2CAP_CL_INIT l2c_cl_init;    /* BTA_JV_L2CAP_CL_INIT_EVT */
  tBTA_JV_L2CAP_CONG l2c_cong;          /* BTA_JV_L2CAP_CONG_EVT */
  tBTA_JV_L2CAP_READ l2c_read;          /* BTA_JV_L2CAP_READ_EVT */
  tBTA_JV_L2CAP_WRITE l2c_write;        /* BTA_JV_L2CAP_WRITE_EVT */
  tBTA_JV_RFCOMM_OPEN rfc_open;         /* BTA_JV_RFCOMM_OPEN_EVT */
  tBTA_JV_RFCOMM_SRV_OPEN rfc_srv_open; /* BTA_JV_RFCOMM_SRV_OPEN_EVT */
  tBTA_JV_RFCOMM_CLOSE rfc_close;       /* BTA_JV_RFCOMM_CLOSE_EVT */
  tBTA_JV_RFCOMM_START rfc_start;       /* BTA_JV_RFCOMM_START_EVT */
  tBTA_JV_RFCOMM_CL_INIT rfc_cl_init;   /* BTA_JV_RFCOMM_CL_INIT_EVT */
  tBTA_JV_RFCOMM_CONG rfc_cong;         /* BTA_JV_RFCOMM_CONG_EVT */
  tBTA_JV_RFCOMM_WRITE rfc_write;       /* BTA_JV_RFCOMM_WRITE_EVT */
  tBTA_JV_DATA_IND data_ind;            /* BTA_JV_L2CAP_DATA_IND_EVT, BTA_JV_RFCOMM_DATA_IND_EVT */
  tBTA_JV_LE_DATA_IND le_data_ind;      /* BTA_JV_L2CAP_LE_DATA_IND_EVT */
  tBTA_JV_L2CAP_LE_OPEN l2c_le_open;    /* BTA_JV_L2CAP_OPEN_EVT */
} tBTA_JV;

/* JAVA DM Interface callback */
typedef void(tBTA_JV_DM_CBACK)(tBTA_JV_EVT event, tBTA_JV* p_data, uint32_t id);

/* JAVA RFCOMM interface callback */
typedef uint32_t(tBTA_JV_RFCOMM_CBACK)(tBTA_JV_EVT event, tBTA_JV* p_data, uint32_t rfcomm_slot_id);

/* JAVA L2CAP interface callback */
typedef void(tBTA_JV_L2CAP_CBACK)(tBTA_JV_EVT event, tBTA_JV* p_data, uint32_t l2cap_socket_id);

/*******************************************************************************
 *
 * Function         BTA_JvEnable
 *
 * Description      Enable the Java I/F service. When the enable
 *                  operation is complete the callback function will be
 *                  called with a BTA_JV_ENABLE_EVT. This function must
 *                  be called before other functions in the JV API are
 *                  called.
 *
 * Returns          BTA_JV_SUCCESS if successful.
 *                  BTA_JV_FAIL if internal failure.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvEnable(tBTA_JV_DM_CBACK* p_cback);

/*******************************************************************************
 *
 * Function         BTA_JvDisable
 *
 * Description      Disable the Java I/F
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_JvDisable(void);

/*******************************************************************************
 *
 * Function         BTA_JvGetChannelId
 *
 * Description      This function reserves a SCN/PSM for applications running
 *                  over RFCOMM or L2CAP. It is primarily called by
 *                  server profiles/applications to register their SCN/PSM into
 *                  the SDP database. The SCN is reported by the
 *                  tBTA_JV_DM_CBACK callback with a BTA_JV_GET_SCN_EVT.
 *                  If the SCN/PSM reported is 0, that means all SCN resources
 *                  are exhausted.
 *                  The channel parameter can be used to request a specific
 *                  channel. If the request on the specific channel fails, the
 *                  SCN/PSM returned in the EVT will be 0 - no attempt to
 *                  request a new channel will be made. set channel to <= 0 to
 *                  automatically assign an channel ID.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_JvGetChannelId(tBTA_JV_CONN_TYPE conn_type, uint32_t id, int32_t channel);

/*******************************************************************************
 *
 * Function         BTA_JvFreeChannel
 *
 * Description      This function frees a SCN/PSM that was used
 *                  by an application running over RFCOMM or L2CAP.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvFreeChannel(uint16_t channel, tBTA_JV_CONN_TYPE conn_type);

/*******************************************************************************
 *
 * Function         BTA_JvStartDiscovery
 *
 * Description      This function performs service discovery for the services
 *                  provided by the given peer device. When the operation is
 *                  complete the tBTA_JV_DM_CBACK callback function will be
 *                  called with a BTA_JV_DISCOVERY_COMP_EVT.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvStartDiscovery(const RawAddress& bd_addr, uint16_t num_uuid,
                                    const bluetooth::Uuid* p_uuid_list, uint32_t rfcomm_slot_id);

/*******************************************************************************
 *
 * Function         BTA_JvCancelDiscovery
 *
 * Description      This function cancels the ongoing service discovery and make
 *                  sure the tBTA_JV_DM_CBACK callback function will be called
 *                  with a BTA_JV_DISCOVERY_COMP_EVT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_JvCancelDiscovery(uint32_t rfcomm_slot_id);

/*******************************************************************************
 *
 * Function         BTA_JvCreateRecordByUser
 *
 * Description      Create a service record in the local SDP database by user in
 *                  tBTA_JV_DM_CBACK callback with a BTA_JV_CREATE_RECORD_EVT.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvCreateRecordByUser(uint32_t rfcomm_slot_id);

/*******************************************************************************
 *
 * Function         BTA_JvDeleteRecord
 *
 * Description      Delete a service record in the local SDP database.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvDeleteRecord(uint32_t handle);

/*******************************************************************************
 *
 * Function         BTA_JvL2capConnect
 *
 * Description      Initiate a connection as a L2CAP client to the given BD
 *                  Address.
 *                  When the connection is initiated or failed to initiate,
 *                  tBTA_JV_L2CAP_CBACK is called with BTA_JV_L2CAP_CL_INIT_EVT
 *                  When the connection is established or failed,
 *                  tBTA_JV_L2CAP_CBACK is called with BTA_JV_L2CAP_OPEN_EVT
 *
 ******************************************************************************/
void BTA_JvL2capConnect(tBTA_JV_CONN_TYPE conn_type, tBTA_SEC sec_mask,
                        std::unique_ptr<tL2CAP_ERTM_INFO> ertm_info, uint16_t remote_psm,
                        uint16_t rx_mtu, std::unique_ptr<tL2CAP_CFG_INFO> cfg,
                        const RawAddress& peer_bd_addr, tBTA_JV_L2CAP_CBACK* p_cback,
                        uint32_t l2cap_socket_id);

/*******************************************************************************
 *
 * Function         BTA_JvL2capClose
 *
 * Description      This function closes an L2CAP client connection
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvL2capClose(uint32_t handle);

/*******************************************************************************
 *
 * Function         BTA_JvL2capStartServer
 *
 * Description      This function starts an L2CAP server and listens for an
 *                  L2CAP connection from a remote Bluetooth device.  When the
 *                  server is started successfully, tBTA_JV_L2CAP_CBACK is
 *                  called with BTA_JV_L2CAP_START_EVT.  When the connection is
 *                  established, tBTA_JV_L2CAP_CBACK is called with
 *                  BTA_JV_L2CAP_OPEN_EVT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_JvL2capStartServer(tBTA_JV_CONN_TYPE conn_type, tBTA_SEC sec_mask,
                            std::unique_ptr<tL2CAP_ERTM_INFO> ertm_info, uint16_t local_psm,
                            uint16_t rx_mtu, std::unique_ptr<tL2CAP_CFG_INFO> cfg,
                            tBTA_JV_L2CAP_CBACK* p_cback, uint32_t l2cap_socket_id);

/*******************************************************************************
 *
 * Function         BTA_JvL2capStopServer
 *
 * Description      This function stops the L2CAP server. If the server has
 *                  an active connection, it would be closed.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvL2capStopServer(uint16_t local_psm, uint32_t l2cap_socket_id);

/*******************************************************************************
 *
 * Function         BTA_JvL2capRead
 *
 * Description      This function reads data from an L2CAP connection
 *                  When the operation is complete, tBTA_JV_L2CAP_CBACK is
 *                  called with BTA_JV_L2CAP_READ_EVT.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvL2capRead(uint32_t handle, uint32_t req_id, uint8_t* p_data, uint16_t len);

/*******************************************************************************
 *
 * Function         BTA_JvL2capReady
 *
 * Description      This function determined if there is data to read from
 *                  an L2CAP connection
 *
 * Returns          BTA_JV_SUCCESS, if data queue size is in *p_data_size.
 *                  BTA_JV_FAILURE, if error.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvL2capReady(uint32_t handle, uint32_t* p_data_size);

/*******************************************************************************
 *
 * Function         BTA_JvL2capWrite
 *
 * Description      This function writes data to an L2CAP connection
 *                  When the operation is complete, tBTA_JV_L2CAP_CBACK is
 *                  called with BTA_JV_L2CAP_WRITE_EVT. Works for
 *                  PSM-based connections
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvL2capWrite(uint32_t handle, uint32_t req_id, BT_HDR* msg, uint32_t user_id);

/*******************************************************************************
 *
 * Function         BTA_JvRfcommConnect
 *
 * Description      This function makes an RFCOMM conection to a remote BD
 *                  Address.
 *                  When the connection is initiated or failed to initiate,
 *                  tBTA_JV_RFCOMM_CBACK is called with
 *                  BTA_JV_RFCOMM_CL_INIT_EVT
 *                  When the connection is established or failed,
 *                  tBTA_JV_RFCOMM_CBACK is called with BTA_JV_RFCOMM_OPEN_EVT
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvRfcommConnect(tBTA_SEC sec_mask, uint8_t remote_scn,
                                   const RawAddress& peer_bd_addr, tBTA_JV_RFCOMM_CBACK* p_cback,
                                   uint32_t rfcomm_slot_id);

/*******************************************************************************
 *
 * Function         BTA_JvRfcommClose
 *
 * Description      This function closes an RFCOMM connection
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvRfcommClose(uint32_t handle, uint32_t rfcomm_slot_id);

/*******************************************************************************
 *
 * Function         BTA_JvRfcommStartServer
 *
 * Description      This function starts listening for an RFCOMM connection
 *                  request from a remote Bluetooth device.  When the server is
 *                  started successfully, tBTA_JV_RFCOMM_CBACK is called
 *                  with BTA_JV_RFCOMM_START_EVT.
 *                  When the connection is established, tBTA_JV_RFCOMM_CBACK
 *                  is called with BTA_JV_RFCOMM_OPEN_EVT.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvRfcommStartServer(tBTA_SEC sec_mask, uint8_t local_scn, uint8_t max_session,
                                       tBTA_JV_RFCOMM_CBACK* p_cback, uint32_t rfcomm_slot_id);

/*******************************************************************************
 *
 * Function         BTA_JvRfcommStopServer
 *
 * Description      This function stops the RFCOMM server. If the server has an
 *                  active connection, it would be closed.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvRfcommStopServer(uint32_t handle, uint32_t rfcomm_slot_id);

/*******************************************************************************
 *
 * Function         BTA_JvRfcommWrite
 *
 * Description      This function writes data to an RFCOMM connection
 *                  When the operation is complete, tBTA_JV_RFCOMM_CBACK is
 *                  called with BTA_JV_RFCOMM_WRITE_EVT.
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvRfcommWrite(uint32_t handle, uint32_t req_id);

/*******************************************************************************
 *
 * Function    BTA_JVSetPmProfile
 *
 * Description This function set or free power mode profile for different JV
 *             application
 *
 * Parameters:  handle,  JV handle from RFCOMM or L2CAP
 *              app_id:  app specific pm ID, can be BTA_JV_PM_ALL, see
 *                       bta_dm_cfg.c for details
 *              BTA_JV_PM_ID_CLEAR: removes pm management on the handle. init_st
 *                                  is ignored and BTA_JV_CONN_CLOSE is called
 *                                  implicitly
 *              init_st: state after calling this API. typically it should be
 *                       BTA_JV_CONN_OPEN
 *
 * Returns      BTA_JV_SUCCESS, if the request is being processed.
 *              BTA_JV_FAILURE, otherwise.
 *
 * NOTE:        BTA_JV_PM_ID_CLEAR: In general no need to be called as jv pm
 *                                  calls automatically
 *              BTA_JV_CONN_CLOSE to remove in case of connection close!
 *
 ******************************************************************************/
tBTA_JV_STATUS BTA_JvSetPmProfile(uint32_t handle, tBTA_JV_PM_ID app_id,
                                  tBTA_JV_CONN_STATE init_st);

/*******************************************************************************
 *
 * Function         BTA_JvRfcommGetPortHdl
 *
 * Description    This function fetches the rfcomm port handle
 *
 * Returns          BTA_JV_SUCCESS, if the request is being processed.
 *                  BTA_JV_FAILURE, otherwise.
 *
 ******************************************************************************/
uint16_t BTA_JvRfcommGetPortHdl(uint32_t handle);
