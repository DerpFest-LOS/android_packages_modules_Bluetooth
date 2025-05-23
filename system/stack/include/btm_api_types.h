/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
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

#ifndef BTM_API_TYPES_H
#define BTM_API_TYPES_H

#include <base/strings/stringprintf.h>

#include <cstdint>
#include <string>

#include "stack/include/bt_dev_class.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/hcidefs.h"
#include "stack/include/sdpdefs.h"
#include "types/raw_address.h"

/* Structure returned with Vendor Specific Command complete callback */
typedef struct {
  uint16_t opcode;
  uint16_t param_len;
  uint8_t* p_param_buf;
} tBTM_VSC_CMPL;

/**************************************************
 *  Device Control and General Callback Functions
 **************************************************/
/* General callback function for notifying an application that a synchronous
 * BTM function is complete. The pointer contains the address of any returned
 * data.
 */
typedef void(tBTM_CMPL_CB)(void* p1);

/* VSC callback function for notifying an application that a synchronous
 * BTM function is complete. The pointer contains the address of any returned
 * data.
 */
typedef void(tBTM_VSC_CMPL_CB)(tBTM_VSC_CMPL* p1);

constexpr uint8_t PHY_LE_NO_PACKET = 0x00;
constexpr uint8_t PHY_LE_1M = 0x01;
constexpr uint8_t PHY_LE_2M = 0x02;
constexpr uint8_t PHY_LE_CODED = 0x04;

constexpr uint8_t NO_ADI_PRESENT = 0xFF;
constexpr uint8_t TX_POWER_NOT_PRESENT = 0x7F;

/******************
 *  SCO Constants
 ******************/

/* Define an invalid SCO index and an invalid HCI handle */
#define BTM_INVALID_SCO_INDEX 0xFFFF

#define BTM_SCO_LINK_ONLY_MASK \
  (ESCO_PKT_TYPES_MASK_HV1 | ESCO_PKT_TYPES_MASK_HV2 | ESCO_PKT_TYPES_MASK_HV3)

#define BTM_ESCO_LINK_ONLY_MASK \
  (ESCO_PKT_TYPES_MASK_EV3 | ESCO_PKT_TYPES_MASK_EV4 | ESCO_PKT_TYPES_MASK_EV5)

/***************
 *  SCO Types
 ***************/
#define BTM_LINK_TYPE_SCO HCI_LINK_TYPE_SCO
#define BTM_LINK_TYPE_ESCO HCI_LINK_TYPE_ESCO
typedef uint8_t tBTM_SCO_TYPE;

/*******************
 * SCO Codec Types
 *******************/
// TODO(b/285458890) This should use common definitions
#define BTM_SCO_CODEC_NONE 0x0000
#define BTM_SCO_CODEC_CVSD 0x0001
#define BTM_SCO_CODEC_MSBC 0x0002
#define BTM_SCO_CODEC_LC3 0x0004
#define BTA_AG_SCO_APTX_SWB_SETTINGS_Q0_MASK 0x0008
#define BTA_AG_SCO_APTX_SWB_SETTINGS_Q1_MASK 0x0016
#define BTA_AG_SCO_APTX_SWB_SETTINGS_Q2_MASK 0x0032
#define BTA_AG_SCO_APTX_SWB_SETTINGS_Q3_MASK 0x0064

/**
 * enum value is defined based on HFP spec, Codec ID section
 */
enum class tBTA_AG_UUID_CODEC : uint16_t {
  BTA_AG_SCO_APTX_SWB_SETTINGS_Q0 = 0,
  UUID_CODEC_NONE = 0,
  UUID_CODEC_CVSD = 0x0001 /* CVSD */,
  UUID_CODEC_MSBC = 0x0002 /* mSBC */,
  UUID_CODEC_LC3 = 0x0003 /* LC3 */,
};

typedef uint16_t tBTM_SCO_CODEC_TYPE;

/***************************
 *  SCO Callback Functions
 ***************************/
typedef void(tBTM_SCO_CB)(uint16_t sco_inx);

/***************
 *  eSCO Types
 ***************/
/* tBTM_ESCO_CBACK event types */
#define BTM_ESCO_CONN_REQ_EVT 2
typedef uint8_t tBTM_ESCO_EVT;

/* Returned by BTM_ReadEScoLinkParms() */
struct tBTM_ESCO_DATA {
  RawAddress bd_addr;
  uint8_t link_type; /* BTM_LINK_TYPE_SCO or BTM_LINK_TYPE_ESCO */
};

typedef struct {
  uint16_t sco_inx;
  RawAddress bd_addr;
  DEV_CLASS dev_class;
  tBTM_SCO_TYPE link_type;
} tBTM_ESCO_CONN_REQ_EVT_DATA;

typedef union {
  tBTM_ESCO_CONN_REQ_EVT_DATA conn_evt;
} tBTM_ESCO_EVT_DATA;

/***************************
 *  eSCO Callback Functions
 ***************************/
typedef void(tBTM_ESCO_CBACK)(tBTM_ESCO_EVT event, tBTM_ESCO_EVT_DATA* p_data);

/**************************
 * SCO Types for Debugging and Testing
 **************************/

/* Define the structure for the WBS/SWB packet status dump.  */
typedef struct {
  uint64_t begin_ts_raw_us;
  uint64_t end_ts_raw_us;
  std::string status_in_hex;
  std::string status_in_binary;
} tBTM_SCO_PKT_STATUS_DATA;

/* Returned by BTM_GetScoDebugDump */
typedef struct {
  bool is_active;
  uint16_t codec_id;
  int total_num_decoded_frames;
  double pkt_loss_ratio;
  tBTM_SCO_PKT_STATUS_DATA latest_data;
} tBTM_SCO_DEBUG_DUMP;

inline std::string sco_codec_type_text(tBTM_SCO_CODEC_TYPE codec_type) {
  switch (codec_type) {
    case BTM_SCO_CODEC_CVSD:
      return "CVSD";
    case BTM_SCO_CODEC_MSBC:
      return "MSBC";
    case BTM_SCO_CODEC_LC3:
      return "LC3";
    default:
      return "UNKNOWN";
  }
}

inline uint16_t sco_codec_type_to_id(tBTM_SCO_CODEC_TYPE codec_type) {
  switch (codec_type) {
    case BTM_SCO_CODEC_CVSD:
      return static_cast<std::underlying_type_t<tBTA_AG_UUID_CODEC>>(
              tBTA_AG_UUID_CODEC::UUID_CODEC_CVSD);
    case BTM_SCO_CODEC_MSBC:
      return static_cast<std::underlying_type_t<tBTA_AG_UUID_CODEC>>(
              tBTA_AG_UUID_CODEC::UUID_CODEC_MSBC);
    case BTM_SCO_CODEC_LC3:
      return static_cast<std::underlying_type_t<tBTA_AG_UUID_CODEC>>(
              tBTA_AG_UUID_CODEC::UUID_CODEC_LC3);
    default:
      return 0;
  }
}

inline std::string bta_ag_uuid_codec_text(const tBTA_AG_UUID_CODEC result) {
  switch (result) {
    CASE_RETURN_TEXT(tBTA_AG_UUID_CODEC::UUID_CODEC_NONE);
    CASE_RETURN_TEXT(tBTA_AG_UUID_CODEC::UUID_CODEC_CVSD);
    CASE_RETURN_TEXT(tBTA_AG_UUID_CODEC::UUID_CODEC_MSBC);
    CASE_RETURN_TEXT(tBTA_AG_UUID_CODEC::UUID_CODEC_LC3);
    default:
      return std::format("UNKNOWN Codec with id {}",
                         static_cast<std::underlying_type_t<tBTA_AG_UUID_CODEC>>(result));
  }
}
#endif  // BTM_API_TYPES_H
