/******************************************************************************
 *
 *  Copyright 1999-2013 Broadcom Corporation
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

#ifndef SRVC_DIS_API_H
#define SRVC_DIS_API_H

#include <cstdint>

#include "gatt_api.h"
#include "gattdefs.h"
#include "internal_include/bt_target.h"
#include "types/bt_transport.h"
#include "types/raw_address.h"

#define DIS_SUCCESS GATT_SUCCESS
#define DIS_ILLEGAL_PARAM GATT_ILLEGAL_PARAMETER
#define DIS_NO_RESOURCES GATT_NO_RESOURCES
typedef uint8_t tDIS_STATUS;

/*****************************************************************************
 *  Data structure for DIS
 ****************************************************************************/

#define DIS_ATTR_SYS_ID_BIT 0x0001
#define DIS_ATTR_MODEL_NUM_BIT 0x0002
#define DIS_ATTR_SERIAL_NUM_BIT 0x0004
#define DIS_ATTR_FW_NUM_BIT 0x0008
#define DIS_ATTR_HW_NUM_BIT 0x0010
#define DIS_ATTR_SW_NUM_BIT 0x0020
#define DIS_ATTR_MANU_NAME_BIT 0x0040
#define DIS_ATTR_IEEE_DATA_BIT 0x0080
#define DIS_ATTR_PNP_ID_BIT 0x0100
typedef uint16_t tDIS_ATTR_MASK;

typedef tDIS_ATTR_MASK tDIS_ATTR_BIT;

typedef struct {
  uint16_t len;
  uint8_t* p_data;
} tDIS_STRING;

typedef struct {
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t product_version;
  uint8_t vendor_id_src;
} tDIS_PNP_ID;

typedef union {
  uint64_t system_id;
  tDIS_PNP_ID pnp_id;
  tDIS_STRING data_str;
} tDIS_ATTR;

#define DIS_MAX_STRING_DATA 7

typedef struct {
  uint16_t attr_mask;
  uint64_t system_id;
  tDIS_PNP_ID pnp_id;
  uint8_t* data_string[DIS_MAX_STRING_DATA];
} tDIS_VALUE;

typedef void(tDIS_READ_CBACK)(const RawAddress& addr, tDIS_VALUE* p_dis_value);

/*****************************************************************************
 *  Data structure used by Battery Service
 ****************************************************************************/
typedef struct {
  RawAddress remote_bda;
  bool need_rsp;
  uint16_t clt_cfg;
} tBA_WRITE_DATA;

#define BA_READ_CLT_CFG_REQ 1
#define BA_READ_PRE_FMT_REQ 2
#define BA_READ_RPT_REF_REQ 3
#define BA_READ_LEVEL_REQ 4
#define BA_WRITE_CLT_CFG_REQ 5

typedef void(tBA_CBACK)(uint8_t app_id, uint8_t event, tBA_WRITE_DATA* p_data);

#define BA_LEVEL_NOTIFY 0x01
#define BA_LEVEL_PRE_FMT 0x02
#define BA_LEVEL_RPT_REF 0x04
typedef uint8_t tBA_LEVEL_DESCR;

typedef struct {
  bool is_pri;
  tBA_LEVEL_DESCR ba_level_descr;
  tBT_TRANSPORT transport;
  tBA_CBACK* p_cback;
} tBA_REG_INFO;

typedef union {
  uint8_t ba_level;
  uint16_t clt_cfg;
  tGATT_CHAR_RPT_REF rpt_ref;
  tGATT_CHAR_PRES pres_fmt;
} tBA_RSP_DATA;

/*****************************************************************************
 *  External Function Declarations
 ****************************************************************************/
/*****************************************************************************
 *  Service Engine API
 ****************************************************************************/
/*******************************************************************************
 *
 * Function         srvc_eng_init
 *
 * Description      Initializa the GATT Service engine, register a GATT
 *                  application as for a central service management.
 *
 ******************************************************************************/
tGATT_STATUS srvc_eng_init(void);

/*****************************************************************************
 *  DIS Client Function
 ****************************************************************************/
/*******************************************************************************
 *
 * Function         DIS_ReadDISInfo
 *
 * Description      Read remote device DIS information.
 *
 * Returns          void
 *
 ******************************************************************************/
bool DIS_ReadDISInfo(const RawAddress& peer_bda, tDIS_READ_CBACK* p_cback, tDIS_ATTR_MASK mask);

#endif
