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
 *  This is the public interface file for the Personal Area Networking (PAN)
 *  subsystem of BTA, Broadcom's Bluetooth application layer for mobile
 *  phones.
 *
 ******************************************************************************/
#ifndef BTA_PAN_API_H
#define BTA_PAN_API_H

#include <cstdint>

#include "types/raw_address.h"

/*****************************************************************************
 *  Constants and data types
 ****************************************************************************/
constexpr bool BTA_PAN_SUCCESS = true;
constexpr bool BTA_PAN_FAIL = false;
typedef bool tBTA_PAN_STATUS;

/* PAN Callback events */
typedef enum : uint8_t {
  BTA_PAN_ENABLE_EVT = 0,   /* PAN service is enabled. */
  BTA_PAN_SET_ROLE_EVT = 1, /* PAN roles registered */
  BTA_PAN_OPENING_EVT = 2,  /* Connection is being opened. */
  BTA_PAN_OPEN_EVT = 3,     /* Connection has been opened. */
  BTA_PAN_CLOSE_EVT = 4,    /* Connection has been closed. */
} tBTA_PAN_EVT;

/* pan roles */
#define BTA_PAN_ROLE_PANU PAN_ROLE_CLIENT
#define BTA_PAN_ROLE_NAP PAN_ROLE_NAP_SERVER

typedef uint8_t tBTA_PAN_ROLE;

/*  information regarding PAN roles */
struct tBTA_PAN_ROLE_INFO {
  const std::string p_srv_name; /* service name for the PAN role */
  const uint8_t app_id;         /* application id */
};

/* Event associated with BTA_PAN_SET_ROLE_EVT */
typedef struct {
  tBTA_PAN_STATUS status; /* status of set role event */
  tBTA_PAN_ROLE role;     /* PAN roles successfully registered */
} tBTA_PAN_SET_ROLE;

/* Event associated with BTA_PAN_OPENING_EVT */
typedef struct {
  RawAddress bd_addr; /* BD address of peer device. */
  uint16_t handle;    /* Handle associated with this connection. */
} tBTA_PAN_OPENING;

/* Event associated with BTA_PAN_OPEN_EVT */
typedef struct {
  RawAddress bd_addr;       /* BD address of peer device. */
  uint16_t handle;          /* Handle associated with this connection. */
  tBTA_PAN_STATUS status;   /* status of open event */
  tBTA_PAN_ROLE local_role; /* Local device PAN role for the connection */
  tBTA_PAN_ROLE peer_role;  /* Peer device PAN role for the connection */
} tBTA_PAN_OPEN;

/* Event associated with BTA_PAN_CLOSE_EVT */
typedef struct {
  uint16_t handle; /* Handle associated with the connection. */
} tBTA_PAN_CLOSE;

/* Union of all PAN callback structures */
typedef union {
  tBTA_PAN_SET_ROLE set_role; /* set_role event */
  tBTA_PAN_OPEN open;         /* Connection has been opened. */
  tBTA_PAN_OPENING opening;   /* Connection being opened */
  tBTA_PAN_CLOSE close;       /* Connection has been closed. */
} tBTA_PAN;

/* Number of PAN connections */
#ifndef BTA_PAN_NUM_CONN
#define BTA_PAN_NUM_CONN 4
#endif

/* PAN callback */
typedef void(tBTA_PAN_CBACK)(tBTA_PAN_EVT event, tBTA_PAN* p_data);

/*****************************************************************************
 *  External Function Declarations
 ****************************************************************************/

/*******************************************************************************
 *
 * Function         BTA_PanEnable
 *
 * Description      Enable PAN service.  This function must be
 *                  called before any other functions in the PAN API are called.
 *                  When the enable operation is complete the callback function
 *                  will be called with a BTA_PAN_ENABLE_EVT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_PanEnable(tBTA_PAN_CBACK p_cback);

/*******************************************************************************
 *
 * Function         BTA_PanDisable
 *
 * Description      Disable PAN service.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_PanDisable(void);

/*******************************************************************************
 *
 * Function         BTA_PanSetRole
 *
 * Description      Sets PAN roles. When the enable operation is complete
 *                  the callback function will be called with a
 *                  BTA_PAN_SET_ROLE_EVT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_PanSetRole(tBTA_PAN_ROLE role, const tBTA_PAN_ROLE_INFO p_user_info,
                    const tBTA_PAN_ROLE_INFO p_nap_info);

/*******************************************************************************
 *
 * Function         BTA_PanOpen
 *
 * Description      Opens a connection to a peer device.
 *                  When connection is open callback function is called
 *                  with a BTA_PAN_OPEN_EVT.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_PanOpen(const RawAddress& bd_addr, tBTA_PAN_ROLE local_role, tBTA_PAN_ROLE peer_role);

/*******************************************************************************
 *
 * Function         BTA_PanClose
 *
 * Description      Close a PAN  connection to a peer device.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_PanClose(uint16_t handle);

#endif /* BTA_PAN_API_H */
