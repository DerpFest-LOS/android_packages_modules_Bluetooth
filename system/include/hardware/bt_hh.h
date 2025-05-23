/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_INCLUDE_BT_HH_H
#define ANDROID_INCLUDE_BT_HH_H

#include <base/strings/stringprintf.h>
#include <stdint.h>

#include <string>

#include "macros.h"
#include "types/ble_address_with_type.h"
#include "types/raw_address.h"

__BEGIN_DECLS

#define BTHH_MAX_DSC_LEN 884

/* HH connection states */
typedef enum {
  BTHH_CONN_STATE_CONNECTED = 0,
  BTHH_CONN_STATE_CONNECTING = 1,
  BTHH_CONN_STATE_DISCONNECTED = 2,
  BTHH_CONN_STATE_DISCONNECTING = 3,
  BTHH_CONN_STATE_ACCEPTING = 4,
  BTHH_CONN_STATE_UNKNOWN = 0xff,
} bthh_connection_state_t;

__END_DECLS

inline std::string bthh_connection_state_text(const bthh_connection_state_t& state) {
  switch (state) {
    CASE_RETURN_TEXT(BTHH_CONN_STATE_CONNECTED);
    CASE_RETURN_TEXT(BTHH_CONN_STATE_CONNECTING);
    CASE_RETURN_TEXT(BTHH_CONN_STATE_DISCONNECTED);
    CASE_RETURN_TEXT(BTHH_CONN_STATE_DISCONNECTING);
    CASE_RETURN_TEXT(BTHH_CONN_STATE_ACCEPTING);
    CASE_RETURN_TEXT(BTHH_CONN_STATE_UNKNOWN);
    default:
      return base::StringPrintf("UNKNOWN[%d]", state);
  }
}

__BEGIN_DECLS

typedef enum {
  BTHH_OK = 0,
  BTHH_HS_HID_NOT_READY,  /* handshake error : device not ready */
  BTHH_HS_INVALID_RPT_ID, /* handshake error : invalid report ID */
  BTHH_HS_TRANS_NOT_SPT,  /* handshake error : transaction not spt */
  BTHH_HS_INVALID_PARAM,  /* handshake error : invalid paremter */
  BTHH_HS_ERROR,          /* handshake error : unspecified HS error */
  BTHH_ERR,               /* general BTA HH error */
  BTHH_ERR_SDP,           /* SDP error */
  BTHH_ERR_PROTO,         /* SET_Protocol error,
                                                        only used in BTA_HH_OPEN_EVT
                             callback */
  BTHH_ERR_DB_FULL,       /* device database full error, used  */
  BTHH_ERR_TOD_UNSPT,     /* type of device not supported */
  BTHH_ERR_NO_RES,        /* out of system resources */
  BTHH_ERR_AUTH_FAILED,   /* authentication fail */
  BTHH_ERR_HDL
} bthh_status_t;

/* Protocol modes */
typedef enum {
  BTHH_REPORT_MODE = 0x00,
  BTHH_BOOT_MODE = 0x01,
  BTHH_UNSUPPORTED_MODE = 0xff
} bthh_protocol_mode_t;

/* Report types */
typedef enum { BTHH_INPUT_REPORT = 1, BTHH_OUTPUT_REPORT, BTHH_FEATURE_REPORT } bthh_report_type_t;

/* Info for which profiles to enable */
typedef struct {
  bool hidp_enabled;
  bool hogp_enabled;
} bthh_profile_enable_t;

typedef struct {
  int attr_mask;
  uint8_t sub_class;
  uint8_t app_id;
  int vendor_id;
  int product_id;
  int version;
  uint8_t ctry_code;
  int dl_len;
  uint8_t dsc_list[BTHH_MAX_DSC_LEN];
} bthh_hid_info_t;

/** Callback for connection state change.
 *  state will have one of the values from bthh_connection_state_t
 */
typedef void (*bthh_connection_state_callback)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                               tBT_TRANSPORT transport,
                                               bthh_connection_state_t state);

/** Callback for vitual unplug api.
 *  the status of the vitual unplug
 */
typedef void (*bthh_virtual_unplug_callback)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                             tBT_TRANSPORT transport, bthh_status_t hh_status);

/** Callback for get hid info
 *  hid_info will contain attr_mask, sub_class, app_id, vendor_id, product_id,
 * version, ctry_code, len
 */
typedef void (*bthh_hid_info_callback)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                       tBT_TRANSPORT transport, bthh_hid_info_t hid_info);

/** Callback for get protocol api.
 *  the protocol mode is one of the value from bthh_protocol_mode_t
 */
typedef void (*bthh_protocol_mode_callback)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                            tBT_TRANSPORT transport, bthh_status_t hh_status,
                                            bthh_protocol_mode_t mode);

/** Callback for get/set_idle_time api.
 */
typedef void (*bthh_idle_time_callback)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                        tBT_TRANSPORT transport, bthh_status_t hh_status,
                                        int idle_rate);

/** Callback for get report api.
 *  if staus is ok rpt_data contains the report data
 */
typedef void (*bthh_get_report_callback)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                         tBT_TRANSPORT transport, bthh_status_t hh_status,
                                         uint8_t* rpt_data, int rpt_size);

/** Callback for set_report/set_protocol api and if error
 *  occurs for get_report/get_protocol api.
 */
typedef void (*bthh_handshake_callback)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                        tBT_TRANSPORT transport, bthh_status_t hh_status);

/** BT-HH callback structure. */
typedef struct {
  /** set to sizeof(BtHfCallbacks) */
  size_t size;
  bthh_connection_state_callback connection_state_cb;
  bthh_hid_info_callback hid_info_cb;
  bthh_protocol_mode_callback protocol_mode_cb;
  bthh_idle_time_callback idle_time_cb;
  bthh_get_report_callback get_report_cb;
  bthh_virtual_unplug_callback virtual_unplug_cb;
  bthh_handshake_callback handshake_cb;
} bthh_callbacks_t;

/** Represents the standard BT-HH interface. */
typedef struct {
  /** set to sizeof(BtHhInterface) */
  size_t size;

  /**
   * Register the BtHh callbacks
   */
  bt_status_t (*init)(bthh_callbacks_t* callbacks);

  /** connect to hid device */
  bt_status_t (*connect)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type, tBT_TRANSPORT transport);

  /** dis-connect from hid device */
  bt_status_t (*disconnect)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type, tBT_TRANSPORT transport,
                            bool reconnect_allowed);

  /** Virtual UnPlug (VUP) the specified HID device */
  bt_status_t (*virtual_unplug)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                tBT_TRANSPORT transport);

  /** Set the HID device descriptor for the specified HID device. */
  bt_status_t (*set_info)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type, tBT_TRANSPORT transport,
                          bthh_hid_info_t hid_info);

  /** Get the HID proto mode. */
  bt_status_t (*get_protocol)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                              tBT_TRANSPORT transport, bthh_protocol_mode_t protocolMode);

  /** Set the HID proto mode. */
  bt_status_t (*set_protocol)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                              tBT_TRANSPORT transport, bthh_protocol_mode_t protocolMode);

  /** Get the HID Idle Time */
  bt_status_t (*get_idle_time)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                               tBT_TRANSPORT transport);

  /** Set the HID Idle Time */
  bt_status_t (*set_idle_time)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                               tBT_TRANSPORT transport, uint8_t idleTime);

  /** Send a GET_REPORT to HID device. */
  bt_status_t (*get_report)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type, tBT_TRANSPORT transport,
                            bthh_report_type_t reportType, uint8_t reportId, int bufferSize);

  /** Send a GET_REPORT_REPLY to HID driver. */
  bt_status_t (*get_report_reply)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                  tBT_TRANSPORT transport, bthh_status_t status, char* report,
                                  uint16_t size);

  /** Send a SET_REPORT to HID device. */
  bt_status_t (*set_report)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type, tBT_TRANSPORT transport,
                            bthh_report_type_t reportType, char* report);

  /** Send data to HID device. */
  bt_status_t (*send_data)(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type, tBT_TRANSPORT transport,
                           char* data);

  /** Closes the interface. */
  void (*cleanup)(void);

  /** Configure which profiles can be enabled. Affected after re-init */
  void (*configure_enabled_profiles)(bool enable_hidp, bool enable_hogp);
} bthh_interface_t;
__END_DECLS

#if __has_include(<bluetooth/log.h>)
#include <bluetooth/log.h>

namespace std {
template <>
struct formatter<bthh_connection_state_t> : enum_formatter<bthh_connection_state_t> {};
template <>
struct formatter<bthh_protocol_mode_t> : enum_formatter<bthh_protocol_mode_t> {};
template <>
struct formatter<bthh_report_type_t> : enum_formatter<bthh_report_type_t> {};
}  // namespace std

#endif  // __has_include(<bluetooth/log.h>)

#endif /* ANDROID_INCLUDE_BT_HH_H */
