/*
 * Copyright 2015 The Android Open Source Project
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

/*****************************************************************************
 *
 *  Filename:      btif_rc.cc
 *
 *  Description:   Bluetooth AVRC implementation
 *
 *****************************************************************************/

#define LOG_TAG "bt_btif_avrc"

#include "btif_rc.h"

#include <base/functional/bind.h>
#include <bluetooth/log.h>
#include <hardware/bluetooth.h>
#include <hardware/bt_rc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

#include "bta/include/bta_av_api.h"
#include "btif/avrcp/avrcp_service.h"
#include "btif_av.h"
#include "btif_common.h"
#include "btif_util.h"
#include "device/include/interop.h"
#include "os/logging/log_adapter.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/list.h"
#include "osi/include/osi.h"
#include "osi/include/properties.h"
#include "stack/include/avrc_api.h"
#include "stack/include/avrc_defs.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_types.h"
#include "types/raw_address.h"

#define RC_INVALID_TRACK_ID (0xFFFFFFFFFFFFFFFFULL)

/*****************************************************************************
 *  Constants & Macros
 *****************************************************************************/

/* cod value for Headsets */
#define COD_AV_HEADSETS 0x0404
/* for AVRC 1.4 need to change this */
#define MAX_RC_NOTIFICATIONS AVRC_EVT_VOLUME_CHANGE

#define IDX_GET_PLAY_STATUS_RSP 0
#define IDX_LIST_APP_ATTR_RSP 1
#define IDX_LIST_APP_VALUE_RSP 2
#define IDX_GET_CURR_APP_VAL_RSP 3
#define IDX_SET_APP_VAL_RSP 4
#define IDX_GET_APP_ATTR_TXT_RSP 5
#define IDX_GET_APP_VAL_TXT_RSP 6
#define IDX_GET_ELEMENT_ATTR_RSP 7
#define IDX_SET_ADDR_PLAYER_RSP 8
#define IDX_SET_BROWSED_PLAYER_RSP 9
#define IDX_GET_FOLDER_ITEMS_RSP 10
#define IDX_CHG_PATH_RSP 11
#define IDX_GET_ITEM_ATTR_RSP 12
#define IDX_PLAY_ITEM_RSP 13
#define IDX_GET_TOTAL_NUM_OF_ITEMS_RSP 14
#define IDX_SEARCH_RSP 15
#define IDX_ADD_TO_NOW_PLAYING_RSP 16

/* Update MAX value whenever IDX will be changed */
#define MAX_CMD_QUEUE_LEN 17

#define MAX_VOLUME 128
#define MAX_LABEL 16
#define MAX_TRANSACTIONS_PER_SESSION 16
#define PLAY_STATUS_PLAYING 1
#define BTIF_RC_NUM_CONN BT_RC_NUM_APP

#define CHECK_RC_CONNECTED(p_dev)                    \
  do {                                               \
    if ((p_dev) == NULL || !(p_dev)->rc_connected) { \
      log::warn("called when RC is not connected");  \
      return BT_STATUS_NOT_READY;                    \
    }                                                \
  } while (0)

#define CHECK_BR_CONNECTED(p_dev)                    \
  do {                                               \
    if ((p_dev) == NULL || !(p_dev)->br_connected) { \
      log::warn("called when BR is not connected");  \
      return BT_STATUS_NOT_READY;                    \
    }                                                \
  } while (0)

using namespace bluetooth;

/*****************************************************************************
 *  Local type definitions
 *****************************************************************************/
typedef struct {
  uint8_t bNotify;
  uint8_t label;
} btif_rc_reg_notifications_t;

typedef struct {
  uint8_t label;
  uint8_t ctype;
  bool is_rsp_pending;
} btif_rc_cmd_ctxt_t;

/* 2 second timeout to get command response, then we free label */
#define BTIF_RC_TIMEOUT_MS (2 * 1000)

typedef enum { eNOT_REGISTERED, eREGISTERED, eINTERIM } btif_rc_nfn_reg_status_t;

typedef struct {
  uint8_t event_id;
  uint8_t label;
  btif_rc_nfn_reg_status_t status;
} btif_rc_supported_event_t;

#define BTIF_RC_STS_TIMEOUT 0xFE

typedef struct {
  bool query_started;
  uint8_t num_attrs;
  uint8_t num_ext_attrs;

  uint8_t attr_index;
  uint8_t ext_attr_index;
  uint8_t ext_val_index;
  btrc_player_app_attr_t attrs[AVRC_MAX_APP_ATTR_SIZE];
  btrc_player_app_ext_attr_t ext_attrs[AVRC_MAX_APP_ATTR_SIZE];
} btif_rc_player_app_settings_t;

// The context associated with a passthru command
typedef struct {
  uint8_t rc_id;
  uint8_t key_state;
  uint8_t custom_id;
} rc_passthru_context_t;

// The context associated with a vendor command
typedef struct {
  uint8_t pdu_id;
  uint8_t event_id;
} rc_vendor_context_t;

// The context associated with a browsing command
typedef struct {
  uint8_t pdu_id;
} rc_browse_context_t;

typedef union {
  rc_vendor_context_t vendor;
  rc_browse_context_t browse;
  rc_passthru_context_t passthru;
} rc_command_context_t;

// The context associated with any command transaction requiring a label.
// The opcode determines how to determine the data in the union. Context is
// used to track which requests have which labels
typedef struct {
  RawAddress rc_addr;
  uint8_t label;
  uint8_t opcode;
  rc_command_context_t command;
} rc_transaction_context_t;
typedef struct {
  bool in_use;
  uint8_t label;
  rc_transaction_context_t context;
  alarm_t* timer;
} rc_transaction_t;

typedef struct {
  std::recursive_mutex label_lock;
  rc_transaction_t transaction[MAX_TRANSACTIONS_PER_SESSION];
} rc_transaction_set_t;

/* TODO : Merge btif_rc_reg_notifications_t and btif_rc_cmd_ctxt_t to a single
 * struct */
typedef struct {
  bool rc_connected;
  bool br_connected;  // Browsing channel.
  uint8_t rc_handle;
  tBTA_AV_FEAT rc_features;
  uint16_t rc_cover_art_psm;  // AVRCP-BIP psm
  btrc_connection_state_t rc_state;
  RawAddress rc_addr;
  uint16_t rc_pending_play;
  btif_rc_cmd_ctxt_t rc_pdu_info[MAX_CMD_QUEUE_LEN];
  btif_rc_reg_notifications_t rc_notif[MAX_RC_NOTIFICATIONS];
  unsigned int rc_volume;
  uint8_t rc_vol_label;
  list_t* rc_supported_event_list;
  btif_rc_player_app_settings_t rc_app_settings;
  alarm_t* rc_play_status_timer;
  bool rc_features_processed;
  uint64_t rc_playing_uid;
  bool rc_procedure_complete;
  rc_transaction_set_t transaction_set;
  tBTA_AV_FEAT peer_ct_features;
  tBTA_AV_FEAT peer_tg_features;
  uint8_t launch_cmd_pending; /* true: getcap/regvolume */
} btif_rc_device_cb_t;

#define RC_PENDING_ACT_GET_CAP (1 << 0)
#define RC_PENDING_ACT_REG_VOL (1 << 1)
#define RC_PENDING_ACT_REPORT_CONN (1 << 2)

typedef struct {
  std::mutex lock;
  btif_rc_device_cb_t rc_multi_cb[BTIF_RC_NUM_CONN];
} rc_cb_t;

typedef struct {
  uint8_t handle;
} btif_rc_handle_t;

static void sleep_ms(uint64_t timeout_ms);

/* Response status code - Unknown Error - this is changed to "reserved" */
#define BTIF_STS_GEN_ERROR 0x06

/* Utility table to map hal status codes to bta status codes for the response
 * status */
static const uint8_t status_code_map[] = {
        /* BTA_Status codes        HAL_Status codes */
        AVRC_STS_BAD_CMD,         /* BTRC_STS_BAD_CMD */
        AVRC_STS_BAD_PARAM,       /* BTRC_STS_BAD_PARAM */
        AVRC_STS_NOT_FOUND,       /* BTRC_STS_NOT_FOUND */
        AVRC_STS_INTERNAL_ERR,    /* BTRC_STS_INTERNAL_ERR */
        AVRC_STS_NO_ERROR,        /* BTRC_STS_NO_ERROR */
        AVRC_STS_UID_CHANGED,     /* BTRC_STS_UID_CHANGED */
        BTIF_STS_GEN_ERROR,       /* BTRC_STS_RESERVED */
        AVRC_STS_BAD_DIR,         /* BTRC_STS_INV_DIRN */
        AVRC_STS_NOT_DIR,         /* BTRC_STS_INV_DIRECTORY */
        AVRC_STS_NOT_EXIST,       /* BTRC_STS_INV_ITEM */
        AVRC_STS_BAD_SCOPE,       /* BTRC_STS_INV_SCOPE */
        AVRC_STS_BAD_RANGE,       /* BTRC_STS_INV_RANGE */
        AVRC_STS_UID_IS_DIR,      /* BTRC_STS_DIRECTORY */
        AVRC_STS_IN_USE,          /* BTRC_STS_MEDIA_IN_USE */
        AVRC_STS_NOW_LIST_FULL,   /* BTRC_STS_PLAY_LIST_FULL */
        AVRC_STS_SEARCH_NOT_SUP,  /* BTRC_STS_SRCH_NOT_SPRTD */
        AVRC_STS_SEARCH_BUSY,     /* BTRC_STS_SRCH_IN_PROG */
        AVRC_STS_BAD_PLAYER_ID,   /* BTRC_STS_INV_PLAYER */
        AVRC_STS_PLAYER_N_BR,     /* BTRC_STS_PLAY_NOT_BROW */
        AVRC_STS_PLAYER_N_ADDR,   /* BTRC_STS_PLAY_NOT_ADDR */
        AVRC_STS_BAD_SEARCH_RES,  /* BTRC_STS_INV_RESULTS */
        AVRC_STS_NO_AVAL_PLAYER,  /* BTRC_STS_NO_AVBL_PLAY */
        AVRC_STS_ADDR_PLAYER_CHG, /* BTRC_STS_ADDR_PLAY_CHGD */
};

static void initialize_device(btif_rc_device_cb_t* p_dev);
static void send_reject_response(uint8_t rc_handle, uint8_t label, uint8_t pdu, uint8_t status,
                                 uint8_t opcode);
static uint8_t opcode_from_pdu(uint8_t pdu);
static void send_metamsg_rsp(btif_rc_device_cb_t* p_dev, int index, uint8_t label,
                             tBTA_AV_CODE code, tAVRC_RESPONSE* pmetamsg_resp);
static void register_volumechange(btif_rc_device_cb_t* p_dev);
static void init_all_transactions(btif_rc_device_cb_t* p_dev);
static bt_status_t get_transaction(btif_rc_device_cb_t* p_dev, rc_transaction_context_t& context,
                                   rc_transaction_t** ptransaction);
static void start_transaction_timer(btif_rc_device_cb_t* p_dev, uint8_t label, uint64_t timeout_ms);
static void btif_rc_transaction_timer_timeout(void* data);
static void release_transaction(btif_rc_device_cb_t* p_dev, uint8_t label);
static std::string dump_transaction(const rc_transaction_t* const transaction);
static rc_transaction_t* get_transaction_by_lbl(btif_rc_device_cb_t* p_dev, uint8_t label);
static void handle_rc_metamsg_rsp(tBTA_AV_META_MSG* pmeta_msg, btif_rc_device_cb_t* p_dev);

static void handle_avk_rc_metamsg_cmd(tBTA_AV_META_MSG* pmeta_msg);
static void handle_avk_rc_metamsg_rsp(tBTA_AV_META_MSG* pmeta_msg);
static void btif_rc_ctrl_upstreams_rsp_cmd(uint8_t event, tAVRC_COMMAND* pavrc_cmd, uint8_t label,
                                           btif_rc_device_cb_t* p_dev);
static void rc_ctrl_procedure_complete(btif_rc_device_cb_t* p_dev);
static void register_for_event_notification(btif_rc_supported_event_t* p_event,
                                            btif_rc_device_cb_t* p_dev);
static void handle_get_capability_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_GET_CAPS_RSP* p_rsp);
static void handle_app_attr_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_LIST_APP_ATTR_RSP* p_rsp);
static void handle_app_val_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_LIST_APP_VALUES_RSP* p_rsp);
static void handle_app_cur_val_response(tBTA_AV_META_MSG* pmeta_msg,
                                        tAVRC_GET_CUR_APP_VALUE_RSP* p_rsp);
static void handle_app_attr_txt_response(tBTA_AV_META_MSG* pmeta_msg,
                                         tAVRC_GET_APP_ATTR_TXT_RSP* p_rsp);
static void handle_app_attr_val_txt_response(tBTA_AV_META_MSG* pmeta_msg,
                                             tAVRC_GET_APP_ATTR_TXT_RSP* p_rsp);
static void cleanup_app_attr_val_txt_response(btif_rc_player_app_settings_t* p_app_settings);
static void handle_get_playstatus_response(tBTA_AV_META_MSG* pmeta_msg,
                                           tAVRC_GET_PLAY_STATUS_RSP* p_rsp);
static void handle_set_addressed_player_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_RSP* p_rsp);
static void cleanup_btrc_folder_items(btrc_folder_items_t* btrc_items, uint8_t item_count);
static void handle_get_metadata_attr_response(tBTA_AV_META_MSG* pmeta_msg,
                                              tAVRC_GET_ATTRS_RSP* p_rsp);
static void handle_set_app_attr_val_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_RSP* p_rsp);
static bt_status_t get_play_status_cmd(btif_rc_device_cb_t* p_dev);
static bt_status_t get_player_app_setting_attr_text_cmd(uint8_t* attrs, uint8_t num_attrs,
                                                        btif_rc_device_cb_t* p_dev);
static bt_status_t get_player_app_setting_value_text_cmd(uint8_t* vals, uint8_t num_vals,
                                                         btif_rc_device_cb_t* p_dev);
static bt_status_t register_notification_cmd(uint8_t event_id, uint32_t event_value,
                                             btif_rc_device_cb_t* p_dev);
static bt_status_t get_metadata_attribute_cmd(uint8_t num_attribute, const uint32_t* p_attr_ids,
                                              btif_rc_device_cb_t* p_dev);
static bt_status_t get_element_attribute_cmd(uint8_t num_attribute, const uint32_t* p_attr_ids,
                                             btif_rc_device_cb_t* p_dev);
static bt_status_t get_item_attribute_cmd(uint64_t uid, int scope, uint8_t num_attribute,
                                          const uint32_t* p_attr_ids, btif_rc_device_cb_t* p_dev);
static bt_status_t getcapabilities_cmd(uint8_t cap_id, btif_rc_device_cb_t* p_dev);
static bt_status_t list_player_app_setting_attrib_cmd(btif_rc_device_cb_t* p_dev);
static bt_status_t list_player_app_setting_value_cmd(uint8_t attrib_id, btif_rc_device_cb_t* p_dev);
static bt_status_t get_player_app_setting_cmd(uint8_t num_attrib, uint8_t* attrib_ids,
                                              btif_rc_device_cb_t* p_dev);
static void get_folder_item_type_media(const tAVRC_ITEM* avrc_item, btrc_folder_items_t* btrc_item);
static void get_folder_item_type_folder(const tAVRC_ITEM* avrc_item,
                                        btrc_folder_items_t* btrc_item);
static void get_folder_item_type_player(const tAVRC_ITEM* avrc_item,
                                        btrc_folder_items_t* btrc_item);
static bt_status_t get_folder_items_cmd(const RawAddress& bd_addr, uint8_t scope,
                                        uint32_t start_item, uint32_t end_item);

static void btif_rc_upstreams_evt(uint16_t event, tAVRC_COMMAND* p_param, uint8_t ctype,
                                  uint8_t label, btif_rc_device_cb_t* p_dev);

static void btif_rc_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE* pavrc_resp, uint8_t ctype,
                                      uint8_t label, btif_rc_device_cb_t* p_dev);

static bool absolute_volume_disabled(void);

/*****************************************************************************
 *  Static variables
 *****************************************************************************/
static rc_cb_t btif_rc_cb;
static btrc_callbacks_t* bt_rc_callbacks = NULL;
static btrc_ctrl_callbacks_t* bt_rc_ctrl_callbacks = NULL;

// List of desired media attribute keys to request by default
static const uint32_t media_attr_list[] = {
        AVRC_MEDIA_ATTR_ID_TITLE,        AVRC_MEDIA_ATTR_ID_ARTIST,
        AVRC_MEDIA_ATTR_ID_ALBUM,        AVRC_MEDIA_ATTR_ID_TRACK_NUM,
        AVRC_MEDIA_ATTR_ID_NUM_TRACKS,   AVRC_MEDIA_ATTR_ID_GENRE,
        AVRC_MEDIA_ATTR_ID_PLAYING_TIME, AVRC_MEDIA_ATTR_ID_COVER_ARTWORK_HANDLE};
static const uint8_t media_attr_list_size = sizeof(media_attr_list) / sizeof(uint32_t);

// List of desired media attribute keys to request if cover artwork is not a
// supported feature
static const uint32_t media_attr_list_no_cover_art[] = {
        AVRC_MEDIA_ATTR_ID_TITLE,       AVRC_MEDIA_ATTR_ID_ARTIST,     AVRC_MEDIA_ATTR_ID_ALBUM,
        AVRC_MEDIA_ATTR_ID_TRACK_NUM,   AVRC_MEDIA_ATTR_ID_NUM_TRACKS, AVRC_MEDIA_ATTR_ID_GENRE,
        AVRC_MEDIA_ATTR_ID_PLAYING_TIME};
static const uint8_t media_attr_list_no_cover_art_size =
        sizeof(media_attr_list_no_cover_art) / sizeof(uint32_t);

/*****************************************************************************
 *  Static functions
 *****************************************************************************/

/*****************************************************************************
 *  Externs
 *****************************************************************************/

void btif_rc_get_addr_by_handle(uint8_t handle, RawAddress& rc_addr) {
  log::verbose("handle: 0x{:x}", handle);
  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    if ((btif_rc_cb.rc_multi_cb[idx].rc_state != BTRC_CONNECTION_STATE_DISCONNECTED) &&
        (btif_rc_cb.rc_multi_cb[idx].rc_handle == handle)) {
      log::verbose("btif_rc_cb.rc_multi_cb[idx].rc_handle: 0x{:x}",
                   btif_rc_cb.rc_multi_cb[idx].rc_handle);
      rc_addr = btif_rc_cb.rc_multi_cb[idx].rc_addr;
      return;
    }
  }
  log::error("returning NULL");
  rc_addr = RawAddress::kEmpty;
  return;
}

/*****************************************************************************
 *  Functions
 *****************************************************************************/
static btif_rc_device_cb_t* alloc_device() {
  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    if (btif_rc_cb.rc_multi_cb[idx].rc_state == BTRC_CONNECTION_STATE_DISCONNECTED) {
      return &btif_rc_cb.rc_multi_cb[idx];
    }
  }
  return NULL;
}

static void initialize_device(btif_rc_device_cb_t* p_dev) {
  if (p_dev == nullptr) {
    return;
  }

  p_dev->rc_connected = false;
  p_dev->br_connected = false;
  p_dev->rc_handle = 0;
  p_dev->rc_features = 0;
  p_dev->rc_cover_art_psm = 0;
  p_dev->rc_state = BTRC_CONNECTION_STATE_DISCONNECTED;
  p_dev->rc_addr = RawAddress::kEmpty;
  p_dev->rc_pending_play = false;
  for (int i = 0; i < MAX_CMD_QUEUE_LEN; ++i) {
    p_dev->rc_pdu_info[i].ctype = 0;
    p_dev->rc_pdu_info[i].label = 0;
    p_dev->rc_pdu_info[i].is_rsp_pending = false;
  }
  if (p_dev->rc_supported_event_list != nullptr) {
    list_clear(p_dev->rc_supported_event_list);
  }
  p_dev->rc_supported_event_list = nullptr;
  p_dev->rc_volume = MAX_VOLUME;
  p_dev->rc_vol_label = MAX_LABEL;
  memset(&p_dev->rc_app_settings, 0, sizeof(btif_rc_player_app_settings_t));
  p_dev->rc_play_status_timer = nullptr;
  p_dev->rc_features_processed = false;
  p_dev->rc_playing_uid = 0;
  p_dev->rc_procedure_complete = false;
  p_dev->peer_ct_features = 0;
  p_dev->peer_tg_features = 0;
  p_dev->launch_cmd_pending = 0;

  // Reset the transaction set for this device. If this initialize_device() call
  // is made due to a disconnect event, this cancels any pending timers too.
  init_all_transactions(p_dev);
}

static btif_rc_device_cb_t* get_connected_device(int index) {
  log::verbose("index: {}", index);
  if (index >= BTIF_RC_NUM_CONN) {
    log::error("can't support more than {} connections", BTIF_RC_NUM_CONN);
    return NULL;
  }
  if (btif_rc_cb.rc_multi_cb[index].rc_state != BTRC_CONNECTION_STATE_CONNECTED) {
    log::error("returning NULL");
    return NULL;
  }
  return &btif_rc_cb.rc_multi_cb[index];
}

static btif_rc_device_cb_t* btif_rc_get_device_by_bda(const RawAddress& bd_addr) {
  log::verbose("bd_addr: {}", bd_addr);

  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    if ((btif_rc_cb.rc_multi_cb[idx].rc_state != BTRC_CONNECTION_STATE_DISCONNECTED) &&
        btif_rc_cb.rc_multi_cb[idx].rc_addr == bd_addr) {
      return &btif_rc_cb.rc_multi_cb[idx];
    }
  }
  log::error("device not found, returning NULL!");
  return NULL;
}

static btif_rc_device_cb_t* btif_rc_get_device_by_handle(uint8_t handle) {
  log::verbose("handle: 0x{:x}", handle);
  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    if ((btif_rc_cb.rc_multi_cb[idx].rc_state != BTRC_CONNECTION_STATE_DISCONNECTED) &&
        (btif_rc_cb.rc_multi_cb[idx].rc_handle == handle)) {
      log::verbose("btif_rc_cb.rc_multi_cb[idx].rc_handle: 0x{:x}",
                   btif_rc_cb.rc_multi_cb[idx].rc_handle);
      return &btif_rc_cb.rc_multi_cb[idx];
    }
  }
  log::error("returning NULL");
  return NULL;
}

static const uint32_t* get_requested_attributes_list(btif_rc_device_cb_t* p_dev) {
  return p_dev->rc_features & BTA_AV_FEAT_COVER_ARTWORK ? media_attr_list
                                                        : media_attr_list_no_cover_art;
}

static uint8_t get_requested_attributes_list_size(btif_rc_device_cb_t* p_dev) {
  return p_dev->rc_features & BTA_AV_FEAT_COVER_ARTWORK ? media_attr_list_size
                                                        : media_attr_list_no_cover_art_size;
}

static void fill_pdu_queue(int index, uint8_t ctype, uint8_t label, bool pending,
                           btif_rc_device_cb_t* p_dev) {
  p_dev->rc_pdu_info[index].ctype = ctype;
  p_dev->rc_pdu_info[index].label = label;
  p_dev->rc_pdu_info[index].is_rsp_pending = pending;
}

static void fill_avrc_attr_entry(tAVRC_ATTR_ENTRY* attr_vals, int num_attrs,
                                 btrc_element_attr_val_t* p_attrs) {
  for (int attr_cnt = 0; attr_cnt < num_attrs; attr_cnt++) {
    attr_vals[attr_cnt].attr_id = p_attrs[attr_cnt].attr_id;
    attr_vals[attr_cnt].name.charset_id = AVRC_CHARSET_ID_UTF8;
    attr_vals[attr_cnt].name.str_len = (uint16_t)strlen((char*)p_attrs[attr_cnt].text);
    attr_vals[attr_cnt].name.p_str = p_attrs[attr_cnt].text;
    log::verbose("attr_id: 0x{:x}, charset_id: 0x{:x}, str_len: {}, str: {}",
                 (unsigned int)attr_vals[attr_cnt].attr_id, attr_vals[attr_cnt].name.charset_id,
                 attr_vals[attr_cnt].name.str_len,
                 reinterpret_cast<char const*>(attr_vals[attr_cnt].name.p_str));
  }
}

static void handle_rc_ctrl_features_all(btif_rc_device_cb_t* p_dev) {
  if (!(p_dev->peer_tg_features & BTA_AV_FEAT_RCTG) &&
      (!(p_dev->peer_tg_features & BTA_AV_FEAT_RCCT) ||
       !(p_dev->peer_tg_features & BTA_AV_FEAT_ADV_CTRL))) {
    return;
  }

  int rc_features = 0;

  log::verbose(
          "peer_tg_features: 0x{:x}, rc_features_processed={}, connected={}, "
          "peer_is_src:{}",
          p_dev->peer_tg_features, p_dev->rc_features_processed,
          btif_av_is_connected_addr(p_dev->rc_addr, A2dpType::kSink),
          btif_av_peer_is_source(p_dev->rc_addr));

  if ((p_dev->peer_tg_features & BTA_AV_FEAT_ADV_CTRL) &&
      (p_dev->peer_tg_features & BTA_AV_FEAT_RCCT)) {
    rc_features |= BTRC_FEAT_ABSOLUTE_VOLUME;
  }

  if ((p_dev->peer_tg_features & BTA_AV_FEAT_METADATA) &&
      (p_dev->peer_tg_features & BTA_AV_FEAT_VENDOR) && (p_dev->rc_features_processed != true)) {
    rc_features |= BTRC_FEAT_METADATA;

    /* Mark rc features processed to avoid repeating
     * the AVRCP procedure every time on receiving this
     * update.
     */
    p_dev->rc_features_processed = true;
  }

  if (btif_av_is_connected_addr(p_dev->rc_addr, A2dpType::kSink)) {
    if (btif_av_peer_is_source(p_dev->rc_addr)) {
      p_dev->rc_features = p_dev->peer_tg_features;
      if ((p_dev->peer_tg_features & BTA_AV_FEAT_METADATA) &&
          (p_dev->peer_tg_features & BTA_AV_FEAT_VENDOR)) {
        getcapabilities_cmd(AVRC_CAP_COMPANY_ID, p_dev);
      }
    }
  } else {
    log::verbose("{} is not connected, pending", p_dev->rc_addr);
    p_dev->launch_cmd_pending |= (RC_PENDING_ACT_GET_CAP | RC_PENDING_ACT_REG_VOL);
  }

  /* Add browsing feature capability */
  if (p_dev->peer_tg_features & BTA_AV_FEAT_BROWSE) {
    rc_features |= BTRC_FEAT_BROWSE;
  }

  /* Add cover art feature capability */
  if (p_dev->peer_tg_features & BTA_AV_FEAT_COVER_ARTWORK) {
    rc_features |= BTRC_FEAT_COVER_ARTWORK;
  }

  if (bt_rc_ctrl_callbacks != NULL) {
    log::verbose("Update rc features to CTRL: {}", rc_features);
    do_in_jni_thread(
            base::BindOnce(bt_rc_ctrl_callbacks->getrcfeatures_cb, p_dev->rc_addr, rc_features));
  }
}

static void handle_rc_ctrl_features(btif_rc_device_cb_t* p_dev) {
  if (btif_av_src_sink_coexist_enabled() && btif_av_both_enable()) {
    handle_rc_ctrl_features_all(p_dev);
    return;
  }

  if (!(p_dev->rc_features & BTA_AV_FEAT_RCTG) &&
      (!(p_dev->rc_features & BTA_AV_FEAT_RCCT) || !(p_dev->rc_features & BTA_AV_FEAT_ADV_CTRL))) {
    return;
  }

  int rc_features = 0;

  if ((p_dev->rc_features & BTA_AV_FEAT_ADV_CTRL) && (p_dev->rc_features & BTA_AV_FEAT_RCCT)) {
    rc_features |= BTRC_FEAT_ABSOLUTE_VOLUME;
  }

  if (p_dev->rc_features & BTA_AV_FEAT_METADATA) {
    rc_features |= BTRC_FEAT_METADATA;
  }

  if ((p_dev->rc_features & BTA_AV_FEAT_VENDOR) && (p_dev->rc_features_processed != true)) {
    /* Mark rc features processed to avoid repeating
     * the AVRCP procedure every time on receiving this
     * update.
     */
    p_dev->rc_features_processed = true;
    if (btif_av_is_sink_enabled()) {
      getcapabilities_cmd(AVRC_CAP_COMPANY_ID, p_dev);
    }
  }

  /* Add browsing feature capability */
  if (p_dev->rc_features & BTA_AV_FEAT_BROWSE) {
    rc_features |= BTRC_FEAT_BROWSE;
  }

  /* Add cover art feature capability */
  if (p_dev->rc_features & BTA_AV_FEAT_COVER_ARTWORK) {
    rc_features |= BTRC_FEAT_COVER_ARTWORK;
  }

  log::verbose("Update rc features to CTRL: {}", rc_features);
  do_in_jni_thread(
          base::BindOnce(bt_rc_ctrl_callbacks->getrcfeatures_cb, p_dev->rc_addr, rc_features));
}
void btif_rc_check_pending_cmd(const RawAddress& peer_address) {
  btif_rc_device_cb_t* p_dev = NULL;
  p_dev = btif_rc_get_device_by_bda(peer_address);
  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  log::verbose(
          "launch_cmd_pending={}, rc_connected={}, peer_ct_features=0x{:x}, "
          "peer_tg_features=0x{:x}",
          p_dev->launch_cmd_pending, p_dev->rc_connected, p_dev->peer_ct_features,
          p_dev->peer_tg_features);
  if (p_dev->launch_cmd_pending && p_dev->rc_connected) {
    if ((p_dev->launch_cmd_pending & RC_PENDING_ACT_REG_VOL) &&
        btif_av_peer_is_sink(p_dev->rc_addr)) {
      if (bluetooth::avrcp::AvrcpService::Get() != nullptr) {
        bluetooth::avrcp::AvrcpService::Get()->RegisterVolChanged(peer_address);
      }
    }
    if ((p_dev->launch_cmd_pending & RC_PENDING_ACT_GET_CAP) &&
        btif_av_peer_is_source(p_dev->rc_addr)) {
      p_dev->rc_features = p_dev->peer_tg_features;
      getcapabilities_cmd(AVRC_CAP_COMPANY_ID, p_dev);
    }
    if ((p_dev->launch_cmd_pending & RC_PENDING_ACT_REPORT_CONN) &&
        btif_av_peer_is_source(p_dev->rc_addr)) {
      if (bt_rc_ctrl_callbacks != NULL) {
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->connection_state_cb, true, false,
                                        p_dev->rc_addr));
      }
    }
  }
  p_dev->launch_cmd_pending = 0;
}

static void handle_rc_ctrl_psm(btif_rc_device_cb_t* p_dev) {
  uint16_t cover_art_psm = p_dev->rc_cover_art_psm;
  log::verbose("Update rc cover art psm to CTRL: {}", cover_art_psm);
  if (bt_rc_ctrl_callbacks != NULL) {
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->get_cover_art_psm_cb, p_dev->rc_addr,
                                    cover_art_psm));
  }
}

static void handle_rc_features(btif_rc_device_cb_t* p_dev) {
  log::assert_that(bt_rc_callbacks != nullptr, "assert failed: bt_rc_callbacks != nullptr");

  btrc_remote_features_t rc_features = BTRC_FEAT_NONE;
  RawAddress avdtp_source_active_peer_addr = btif_av_source_active_peer();
  RawAddress avdtp_sink_active_peer_addr = btif_av_sink_active_peer();

  log::verbose(
          "AVDTP Source Active Peer Address: {} AVDTP Sink Active Peer Address: {} "
          "AVCTP address: {}",
          avdtp_source_active_peer_addr, avdtp_sink_active_peer_addr, p_dev->rc_addr);

  if (interop_match_addr(INTEROP_DISABLE_ABSOLUTE_VOLUME, &p_dev->rc_addr) ||
      absolute_volume_disabled() ||
      (avdtp_source_active_peer_addr != p_dev->rc_addr &&
       avdtp_sink_active_peer_addr != p_dev->rc_addr)) {
    p_dev->rc_features &= ~BTA_AV_FEAT_ADV_CTRL;
  }

  if (p_dev->rc_features & BTA_AV_FEAT_BROWSE) {
    rc_features = (btrc_remote_features_t)(rc_features | BTRC_FEAT_BROWSE);
  }

  if (p_dev->rc_features & BTA_AV_FEAT_METADATA) {
    rc_features = (btrc_remote_features_t)(rc_features | BTRC_FEAT_METADATA);
  }

  if (!avrcp_absolute_volume_is_enabled()) {
    return;
  }

  if ((p_dev->rc_features & BTA_AV_FEAT_ADV_CTRL) && (p_dev->rc_features & BTA_AV_FEAT_RCTG)) {
    rc_features = (btrc_remote_features_t)(rc_features | BTRC_FEAT_ABSOLUTE_VOLUME);
  }

  log::verbose("rc_features: 0x{:x}", rc_features);
  HAL_CBACK(bt_rc_callbacks, remote_features_cb, p_dev->rc_addr, rc_features);

  log::verbose("Checking for feature flags in btif_rc_handler with label: {}", p_dev->rc_vol_label);
  // Register for volume change on connect
  if (p_dev->rc_features & BTA_AV_FEAT_ADV_CTRL && p_dev->rc_features & BTA_AV_FEAT_RCTG) {
    register_volumechange(p_dev);
  }
}

/***************************************************************************
 *  Function       handle_rc_browse_connect
 *
 *  - Argument:    tBTA_AV_RC_OPEN  browse RC open data structure
 *
 *  - Description: browse RC connection event handler
 *
 ***************************************************************************/
static void handle_rc_browse_connect(tBTA_AV_RC_BROWSE_OPEN* p_rc_br_open) {
  log::verbose("rc_handle {} status {}", p_rc_br_open->rc_handle, p_rc_br_open->status);
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(p_rc_br_open->rc_handle);

  if (!p_dev) {
    log::error("p_dev is null");
    return;
  }

  /* check that we are already connected to this address since being connected
   * to a browse when not connected to the control channel over AVRCP is
   * probably not preferred anyways. */
  if (p_rc_br_open->status == BTA_AV_SUCCESS) {
    p_dev->br_connected = true;
    if (btif_av_src_sink_coexist_enabled()) {
      if (btif_av_peer_is_connected_source(p_dev->rc_addr)) {
        if (bt_rc_ctrl_callbacks != NULL) {
          do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->connection_state_cb, true, true,
                                          p_dev->rc_addr));
        }
      } else {
        p_dev->launch_cmd_pending |= RC_PENDING_ACT_REPORT_CONN;
        log::verbose("pending rc browse connection event");
      }
    } else {
      if (bt_rc_ctrl_callbacks != NULL) {
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->connection_state_cb, true, true,
                                        p_dev->rc_addr));
      } else {
        log::warn("bt_rc_ctrl_callbacks is null.");
      }
    }
  }
}

/***************************************************************************
 *  Function       handle_rc_connect
 *
 *  - Argument:    tBTA_AV_RC_OPEN  RC open data structure
 *
 *  - Description: RC connection event handler
 *
 ***************************************************************************/
static void handle_rc_connect(tBTA_AV_RC_OPEN* p_rc_open) {
  log::verbose("rc_handle: {}", p_rc_open->rc_handle);

  btif_rc_device_cb_t* p_dev = alloc_device();
  if (p_dev == NULL) {
    log::error("p_dev is NULL");
    return;
  }

  if (!(p_rc_open->status == BTA_AV_SUCCESS)) {
    log::error("Connect failed with error code: {}", p_rc_open->status);
    p_dev->rc_connected = false;
    BTA_AvCloseRc(p_rc_open->rc_handle);
    p_dev->rc_handle = 0;
    p_dev->rc_state = BTRC_CONNECTION_STATE_DISCONNECTED;
    p_dev->rc_features = 0;
    p_dev->peer_ct_features = 0;
    p_dev->peer_tg_features = 0;
    p_dev->launch_cmd_pending = 0;
    p_dev->rc_vol_label = MAX_LABEL;
    p_dev->rc_volume = MAX_VOLUME;
    p_dev->rc_addr = RawAddress::kEmpty;
    return;
  }

  // check if already some RC is connected
  if (p_dev->rc_connected) {
    log::error("Got RC OPEN in connected state, Connected RC: {} and Current RC: {}",
               p_dev->rc_handle, p_rc_open->rc_handle);
    if (p_dev->rc_handle != p_rc_open->rc_handle && p_dev->rc_addr != p_rc_open->peer_addr) {
      log::verbose("Got RC connected for some other handle");
      BTA_AvCloseRc(p_rc_open->rc_handle);
      return;
    }
  }
  p_dev->rc_addr = p_rc_open->peer_addr;
  p_dev->rc_features = p_rc_open->peer_features;
  p_dev->peer_ct_features = p_rc_open->peer_ct_features;
  p_dev->peer_tg_features = p_rc_open->peer_tg_features;
  p_dev->rc_cover_art_psm = p_rc_open->cover_art_psm;
  p_dev->rc_vol_label = MAX_LABEL;
  p_dev->rc_volume = MAX_VOLUME;

  log::verbose(
          "handle_rc_connect in features={:#x}, out features={:#x}, "
          "ct_feature={:#x}, tg_feature={:#x}, cover art psm={:#x}",
          p_rc_open->peer_features, p_dev->rc_features, p_dev->peer_ct_features,
          p_dev->peer_tg_features, p_dev->rc_cover_art_psm);

  p_dev->rc_connected = true;
  p_dev->rc_handle = p_rc_open->rc_handle;
  p_dev->rc_state = BTRC_CONNECTION_STATE_CONNECTED;

  p_dev->rc_playing_uid = RC_INVALID_TRACK_ID;

  if (btif_av_src_sink_coexist_enabled() && !btif_av_peer_is_connected_source(p_dev->rc_addr)) {
    p_dev->launch_cmd_pending |= RC_PENDING_ACT_REPORT_CONN;
    log::verbose("pending rc connection event");
    return;
  }
  if (bt_rc_ctrl_callbacks != NULL) {
    do_in_jni_thread(
            base::BindOnce(bt_rc_ctrl_callbacks->connection_state_cb, true, false, p_dev->rc_addr));
    /* report connection state if remote device is AVRCP target */
    handle_rc_ctrl_features(p_dev);

    /* report psm if remote device is AVRCP target */
    handle_rc_ctrl_psm(p_dev);
  }
}

/***************************************************************************
 *  Function       handle_rc_disconnect
 *
 *  - Argument:    tBTA_AV_RC_CLOSE     RC close data structure
 *
 *  - Description: RC disconnection event handler
 *
 ***************************************************************************/
static void handle_rc_disconnect(tBTA_AV_RC_CLOSE* p_rc_close) {
  btif_rc_device_cb_t* p_dev = NULL;
  log::verbose("rc_handle: {}", p_rc_close->rc_handle);

  p_dev = btif_rc_get_device_by_handle(p_rc_close->rc_handle);
  if (p_dev == NULL) {
    log::error("Got disconnect from invalid rc handle");
    return;
  }

  if (p_rc_close->rc_handle != p_dev->rc_handle && p_dev->rc_addr != p_rc_close->peer_addr) {
    log::error("Got disconnect of unknown device");
    return;
  }

  /* Report connection state if device is AVRCP target */
  if (bt_rc_ctrl_callbacks != NULL) {
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->connection_state_cb, false, false,
                                    p_dev->rc_addr));
  }

  // We'll re-initialize the device state back to what it looked like before
  // the connection. This will free ongoing transaction labels and clear any
  // running label timers
  initialize_device(p_dev);
}

/***************************************************************************
 *  Function       handle_rc_passthrough_cmd
 *
 *  - Argument:    tBTA_AV_RC rc_id   remote control command ID
 *                 tBTA_AV_STATE key_state status of key press
 *
 *  - Description: Remote control command handler
 *
 ***************************************************************************/
static void handle_rc_passthrough_cmd(tBTA_AV_REMOTE_CMD* p_remote_cmd) {
  if (p_remote_cmd == NULL) {
    log::error("No remote command!");
    return;
  }

  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(p_remote_cmd->rc_handle);
  if (p_dev == NULL) {
    log::error("Got passthrough command from invalid rc handle");
    return;
  }

  log::verbose("p_remote_cmd->rc_id: {}", p_remote_cmd->rc_id);

  /* If AVRC is open and peer sends PLAY but there is no AVDT, then we queue-up
   * this PLAY */
  if ((p_remote_cmd->rc_id == AVRC_ID_PLAY) && (!btif_av_is_connected(A2dpType::kSink))) {
    if (p_remote_cmd->key_state == AVRC_STATE_PRESS) {
      log::warn("AVDT not open, queuing the PLAY command");
      p_dev->rc_pending_play = true;
    }
    return;
  }

  /* If we previously queued a play and we get a PAUSE, clear it. */
  if ((p_remote_cmd->rc_id == AVRC_ID_PAUSE) && (p_dev->rc_pending_play)) {
    log::warn("Clear the pending PLAY on PAUSE received");
    p_dev->rc_pending_play = false;
    return;
  }

  if ((p_remote_cmd->rc_id == AVRC_ID_STOP) && (!btif_av_stream_started_ready(A2dpType::kSink))) {
    log::warn("Stream suspended, ignore STOP cmd");
    return;
  }

  int pressed = (p_remote_cmd->key_state == AVRC_STATE_PRESS) ? 1 : 0;

  /* pass all commands up */
  log::verbose("rc_features: {}, cmd->rc_id: {}, pressed: {}", p_dev->rc_features,
               p_remote_cmd->rc_id, pressed);
  HAL_CBACK(bt_rc_callbacks, passthrough_cmd_cb, p_remote_cmd->rc_id, pressed, p_dev->rc_addr);
}

/***************************************************************************
 *  Function       handle_rc_passthrough_rsp
 *
 *  - Argument:    tBTA_AV_REMOTE_RSP passthrough command response
 *
 *  - Description: Remote control passthrough response handler
 *
 ***************************************************************************/
static void handle_rc_passthrough_rsp(tBTA_AV_REMOTE_RSP* p_remote_rsp) {
  btif_rc_device_cb_t* p_dev = NULL;

  p_dev = btif_rc_get_device_by_handle(p_remote_rsp->rc_handle);
  if (p_dev == NULL) {
    log::error("passthrough response for Invalid rc handle");
    return;
  }

  if (!(p_dev->rc_features & BTA_AV_FEAT_RCTG)) {
    log::error("DUT does not support AVRCP controller role");
    return;
  }

  const char* status = (p_remote_rsp->key_state == 1) ? "released" : "pressed";
  log::verbose("rc_id: {} state: {}", p_remote_rsp->rc_id, status);

  release_transaction(p_dev, p_remote_rsp->label);
  if (bt_rc_ctrl_callbacks != NULL) {
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->passthrough_rsp_cb, p_dev->rc_addr,
                                    p_remote_rsp->rc_id, p_remote_rsp->key_state));
  }
}

/***************************************************************************
 *  Function       handle_rc_vendorunique_rsp
 *
 *  - Argument:    tBTA_AV_REMOTE_RSP  command response
 *
 *  - Description: Remote control vendor unique response handler
 *
 ***************************************************************************/
static void handle_rc_vendorunique_rsp(tBTA_AV_REMOTE_RSP* p_remote_rsp) {
  btif_rc_device_cb_t* p_dev = NULL;
  const char* status;
  uint8_t vendor_id = 0;

  p_dev = btif_rc_get_device_by_handle(p_remote_rsp->rc_handle);
  if (p_dev == NULL) {
    log::error("Got vendorunique rsp from invalid rc handle");
    return;
  }

  if (p_dev->rc_features & BTA_AV_FEAT_RCTG) {
    int key_state;
    if (p_remote_rsp->key_state == AVRC_STATE_RELEASE) {
      status = "released";
      key_state = 1;
    } else {
      status = "pressed";
      key_state = 0;
    }

    if (p_remote_rsp->len > 0 && p_remote_rsp->p_data != NULL) {
      if (p_remote_rsp->len >= AVRC_PASS_THRU_GROUP_LEN) {
        vendor_id = p_remote_rsp->p_data[AVRC_PASS_THRU_GROUP_LEN - 1];
      }
      osi_free_and_reset((void**)&p_remote_rsp->p_data);
    }
    log::verbose("vendor_id: {} status: {}", vendor_id, status);

    release_transaction(p_dev, p_remote_rsp->label);
    do_in_jni_thread(
            base::BindOnce(bt_rc_ctrl_callbacks->groupnavigation_rsp_cb, vendor_id, key_state));
  } else {
    log::error("Remote does not support AVRCP TG role");
  }
}

/***************************************************************************
 *  Function       handle_rc_metamsg_cmd
 *
 *  - Argument:    tBTA_AV_VENDOR Structure containing the received
 *                          metamsg command
 *
 *  - Description: Remote control metamsg command handler (AVRCP 1.3)
 *
 ***************************************************************************/
static void handle_rc_metamsg_cmd(tBTA_AV_META_MSG* pmeta_msg) {
  /* Parse the metamsg command and pass it on to BTL-IFS */
  uint8_t scratch_buf[512] = {0};
  tAVRC_COMMAND avrc_command = {0};
  tAVRC_STS status;
  btif_rc_device_cb_t* p_dev = NULL;

  if (NULL == pmeta_msg) {
    log::verbose("Exiting as pmeta_msg is NULL");
    return;
  }

  if (NULL == pmeta_msg->p_msg) {
    log::verbose("Exiting as pmeta_msg->p_msg is NULL");
    return;
  }

  log::verbose("pmeta_msg: opcode: {:x}, code: {:x}", pmeta_msg->p_msg->hdr.opcode,
               pmeta_msg->code);

  p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);
  if (p_dev == NULL) {
    log::error("Meta msg event for Invalid rc handle");
    return;
  }

  if (pmeta_msg->p_msg->hdr.opcode != AVRC_OP_VENDOR &&
      pmeta_msg->p_msg->hdr.opcode != AVRC_OP_BROWSE) {
    log::warn("Invalid opcode: {:x}", pmeta_msg->p_msg->hdr.opcode);
    return;
  }

  if (pmeta_msg->len < 3) {
    log::warn("Invalid length. opcode: 0x{:x}, len: 0x{:x}", pmeta_msg->p_msg->hdr.opcode,
              pmeta_msg->len);
    return;
  }

  if (pmeta_msg->code >= AVRC_RSP_NOT_IMPL) {
    {
      rc_transaction_t* transaction = NULL;
      transaction = get_transaction_by_lbl(p_dev, pmeta_msg->label);
      if (transaction != NULL) {
        handle_rc_metamsg_rsp(pmeta_msg, p_dev);
      } else {
        log::verbose("Discard vendor dependent rsp. code: {} label: {}.", pmeta_msg->code,
                     pmeta_msg->label);
      }
      return;
    }
  }

  status = AVRC_ParsCommand(pmeta_msg->p_msg, &avrc_command, scratch_buf, sizeof(scratch_buf));
  log::verbose("Received vendor command.code,PDU and label: {}, {}, {}", pmeta_msg->code,
               avrc_command.cmd.pdu, pmeta_msg->label);

  if (status != AVRC_STS_NO_ERROR) {
    /* return error */
    log::warn("Error in parsing received metamsg command. status: 0x{:02x}", status);
    send_reject_response(pmeta_msg->rc_handle, pmeta_msg->label, avrc_command.pdu, status,
                         pmeta_msg->p_msg->hdr.opcode);
  } else {
    /* if RegisterNotification, add it to our registered queue */

    if (avrc_command.cmd.pdu == AVRC_PDU_REGISTER_NOTIFICATION) {
      uint8_t event_id = avrc_command.reg_notif.event_id;

      log::verbose(
              "New register notification received.event_id: {}, label: 0x{:x}, "
              "code: {:x}",
              dump_rc_notification_event_id(event_id), pmeta_msg->label, pmeta_msg->code);
      p_dev->rc_notif[event_id - 1].bNotify = true;
      p_dev->rc_notif[event_id - 1].label = pmeta_msg->label;
      /* this is sink(tg) feature, so it should not handle here */
      if (btif_av_both_enable() && event_id == AVRC_EVT_VOLUME_CHANGE) {
        return;
      }
    }

    /* this is sink(tg) feature, so it should not handle here */
    if (btif_av_both_enable() && avrc_command.cmd.pdu == AVRC_PDU_SET_ABSOLUTE_VOLUME) {
      return;
    }
    log::verbose("Passing received metamsg command to app. pdu: {}",
                 dump_rc_pdu(avrc_command.cmd.pdu));

    /* Since handle_rc_metamsg_cmd() itself is called from
     *btif context, no context switching is required. Invoke
     * btif_rc_upstreams_evt directly from here. */
    btif_rc_upstreams_evt((uint16_t)avrc_command.cmd.pdu, &avrc_command, pmeta_msg->code,
                          pmeta_msg->label, p_dev);
  }
}

/***************************************************************************
 **
 ** Function       btif_rc_handler
 **
 ** Description    RC event handler
 **
 ***************************************************************************/
void btif_rc_handler(tBTA_AV_EVT event, tBTA_AV* p_data) {
  log::verbose("event: {}", dump_rc_event(event));
  btif_rc_device_cb_t* p_dev = NULL;
  switch (event) {
    case BTA_AV_RC_OPEN_EVT: {
      log::verbose("Peer_features: 0x{:x} Cover Art PSM: 0x{:x}", p_data->rc_open.peer_features,
                   p_data->rc_open.cover_art_psm);
      handle_rc_connect(&(p_data->rc_open));
    } break;

    case BTA_AV_RC_BROWSE_OPEN_EVT: {
      /* tell the UL that we have connection to browse channel and that
       * browse commands can be directed accordingly. */
      handle_rc_browse_connect(&p_data->rc_browse_open);
    } break;

    case BTA_AV_RC_CLOSE_EVT: {
      handle_rc_disconnect(&(p_data->rc_close));
    } break;

    case BTA_AV_RC_BROWSE_CLOSE_EVT: {
      log::verbose("BTA_AV_RC_BROWSE_CLOSE_EVT");
    } break;

    case BTA_AV_REMOTE_CMD_EVT: {
      if (bt_rc_callbacks != NULL) {
        log::verbose("rc_id: 0x{:x} key_state: {}", p_data->remote_cmd.rc_id,
                     p_data->remote_cmd.key_state);
        handle_rc_passthrough_cmd(&p_data->remote_cmd);
      } else {
        log::error("AVRCP TG role not up, drop passthrough commands");
      }
    } break;

    case BTA_AV_REMOTE_RSP_EVT: {
      log::verbose("RSP: rc_id: 0x{:x} key_state: {}", p_data->remote_rsp.rc_id,
                   p_data->remote_rsp.key_state);

      if (p_data->remote_rsp.rc_id == AVRC_ID_VENDOR) {
        handle_rc_vendorunique_rsp(&p_data->remote_rsp);
      } else {
        handle_rc_passthrough_rsp(&p_data->remote_rsp);
      }
    } break;

    case BTA_AV_RC_FEAT_EVT: {
      log::verbose("Peer_features: {:x}", p_data->rc_feat.peer_features);
      p_dev = btif_rc_get_device_by_handle(p_data->rc_feat.rc_handle);
      if (p_dev == NULL) {
        log::error("RC Feature event for Invalid rc handle");
        break;
      }
      log::verbose("peer_ct_features:0x{:x}, peer_tg_features=0x{:x}",
                   p_data->rc_feat.peer_ct_features, p_data->rc_feat.peer_tg_features);
      if (btif_av_src_sink_coexist_enabled() &&
          (p_dev->peer_ct_features == p_data->rc_feat.peer_ct_features) &&
          (p_dev->peer_tg_features == p_data->rc_feat.peer_tg_features)) {
        log::error("do SDP twice, no need callback rc_feature to framework again");
        break;
      }

      p_dev->peer_ct_features = p_data->rc_feat.peer_ct_features;
      p_dev->peer_tg_features = p_data->rc_feat.peer_tg_features;
      p_dev->rc_features = p_data->rc_feat.peer_features;
      if (bt_rc_callbacks != NULL) {
        handle_rc_features(p_dev);
      }

      if ((p_dev->rc_connected) && (bt_rc_ctrl_callbacks != NULL)) {
        handle_rc_ctrl_features(p_dev);
      }
    } break;

    case BTA_AV_RC_PSM_EVT: {
      log::verbose("Peer cover art PSM: {:x}", p_data->rc_cover_art_psm.cover_art_psm);
      p_dev = btif_rc_get_device_by_handle(p_data->rc_cover_art_psm.rc_handle);
      if (p_dev == NULL) {
        log::error("RC PSM event for Invalid rc handle");
        break;
      }

      p_dev->rc_cover_art_psm = p_data->rc_cover_art_psm.cover_art_psm;
      if ((p_dev->rc_connected) && (bt_rc_ctrl_callbacks != NULL)) {
        handle_rc_ctrl_psm(p_dev);
      }
    } break;

    case BTA_AV_META_MSG_EVT: {
      if (bt_rc_callbacks != NULL) {
        log::verbose("BTA_AV_META_MSG_EVT code: {} label: {}", p_data->meta_msg.code,
                     p_data->meta_msg.label);
        log::verbose("company_id: 0x{:x} len: {} handle: {}", p_data->meta_msg.company_id,
                     p_data->meta_msg.len, p_data->meta_msg.rc_handle);

        /* handle the metamsg command */
        handle_rc_metamsg_cmd(&(p_data->meta_msg));

        /* Free the Memory allocated for tAVRC_MSG */
      } else if (bt_rc_ctrl_callbacks != NULL) {
        /* This is case of Sink + CT + TG(for abs vol)) */
        log::verbose("BTA_AV_META_MSG_EVT code:{} label:{} opcode {} ctype {}",
                     p_data->meta_msg.code, p_data->meta_msg.label,
                     p_data->meta_msg.p_msg->hdr.opcode, p_data->meta_msg.p_msg->hdr.ctype);
        log::verbose("company_id:0x{:x} len:{} handle:{}", p_data->meta_msg.company_id,
                     p_data->meta_msg.len, p_data->meta_msg.rc_handle);
        switch (p_data->meta_msg.p_msg->hdr.opcode) {
          case AVRC_OP_VENDOR:
            if ((p_data->meta_msg.code >= AVRC_RSP_NOT_IMPL) &&
                (p_data->meta_msg.code <= AVRC_RSP_INTERIM)) {
              /* Its a response */
              handle_avk_rc_metamsg_rsp(&(p_data->meta_msg));
            } else if (p_data->meta_msg.code <= AVRC_CMD_GEN_INQ) {
              /* Its a command  */
              handle_avk_rc_metamsg_cmd(&(p_data->meta_msg));
            }
            break;

          case AVRC_OP_BROWSE:
            if (p_data->meta_msg.p_msg->hdr.ctype == AVRC_CMD) {
              handle_avk_rc_metamsg_cmd(&(p_data->meta_msg));
            } else if (p_data->meta_msg.p_msg->hdr.ctype == AVRC_RSP) {
              handle_avk_rc_metamsg_rsp(&(p_data->meta_msg));
            }
            break;
        }
      } else {
        log::error("Neither CTRL, nor TG is up, drop meta commands");
      }
    } break;

    default:
      log::verbose("Unhandled RC event : 0x{:x}", event);
  }
}

bool btif_rc_is_connected_peer(const RawAddress& peer_addr) {
  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    btif_rc_device_cb_t* p_dev = get_connected_device(idx);
    if (p_dev != NULL && p_dev->rc_connected && peer_addr == p_dev->rc_addr) {
      return true;
    }
  }
  return false;
}

/***************************************************************************
 **
 ** Function       btif_rc_get_connected_peer_handle
 **
 ** Description    Fetches the connected headset's handle if any
 **
 ***************************************************************************/
uint8_t btif_rc_get_connected_peer_handle(const RawAddress& peer_addr) {
  btif_rc_device_cb_t* p_dev = NULL;
  p_dev = btif_rc_get_device_by_bda(peer_addr);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return BTRC_HANDLE_NONE;
  }
  return p_dev->rc_handle;
}

/***************************************************************************
 **
 ** Function       btif_rc_check_handle_pending_play
 **
 ** Description    Clears the queued PLAY command. if |bSendToApp| is true,
 **                forwards to app
 **
 ***************************************************************************/

/* clear the queued PLAY command. if |bSendToApp| is true, forward to app */
void btif_rc_check_handle_pending_play(const RawAddress& peer_addr, bool bSendToApp) {
  btif_rc_device_cb_t* p_dev = NULL;
  p_dev = btif_rc_get_device_by_bda(peer_addr);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  log::verbose("bSendToApp: {}", bSendToApp);
  if (p_dev->rc_pending_play) {
    if (bSendToApp) {
      tBTA_AV_REMOTE_CMD remote_cmd;
      log::verbose("Sending queued PLAYED event to app");

      memset(&remote_cmd, 0, sizeof(tBTA_AV_REMOTE_CMD));
      remote_cmd.rc_handle = p_dev->rc_handle;
      remote_cmd.rc_id = AVRC_ID_PLAY;
      remote_cmd.hdr.ctype = AVRC_CMD_CTRL;
      remote_cmd.hdr.opcode = AVRC_OP_PASS_THRU;

      /* delay sending to app, else there is a timing issue in the framework,
       ** which causes the audio to be on th device's speaker. Delay between
       ** OPEN & RC_PLAYs
       */
      sleep_ms(200);
      /* send to app - both PRESSED & RELEASED */
      remote_cmd.key_state = AVRC_STATE_PRESS;
      handle_rc_passthrough_cmd(&remote_cmd);

      sleep_ms(100);

      remote_cmd.key_state = AVRC_STATE_RELEASE;
      handle_rc_passthrough_cmd(&remote_cmd);
    }
    p_dev->rc_pending_play = false;
  }
}

/* Generic reject response */
static void send_reject_response(uint8_t rc_handle, uint8_t label, uint8_t pdu, uint8_t status,
                                 uint8_t opcode) {
  uint8_t ctype = AVRC_RSP_REJ;
  tAVRC_RESPONSE avrc_rsp;
  BT_HDR* p_msg = NULL;
  memset(&avrc_rsp, 0, sizeof(tAVRC_RESPONSE));

  avrc_rsp.rsp.opcode = opcode;
  avrc_rsp.rsp.pdu = pdu;
  avrc_rsp.rsp.status = status;

  status = AVRC_BldResponse(rc_handle, &avrc_rsp, &p_msg);

  if (status != AVRC_STS_NO_ERROR) {
    log::error("status not AVRC_STS_NO_ERROR");
    return;
  }

  log::verbose("Sending error notification to handle: {}. pdu: {},status: 0x{:02x}", rc_handle,
               dump_rc_pdu(pdu), status);
  BTA_AvMetaRsp(rc_handle, label, ctype, p_msg);
}

/***************************************************************************
 *  Function         get_rsp_type_code
 *
 *  - Argument:   status
 *  - Description: Returns response type codes for particular command code and
 *                 status.
 *
 ***************************************************************************/
static tBTA_AV_CODE get_rsp_type_code(tAVRC_STS status, tBTA_AV_CODE code) {
  if (status != AVRC_STS_NO_ERROR) {
    return AVRC_RSP_REJ;
  }

  if (code < AVRC_RSP_NOT_IMPL) {
    if (code == AVRC_CMD_NOTIF) {
      return AVRC_RSP_INTERIM;
    }

    if (code == AVRC_CMD_STATUS) {
      return AVRC_RSP_IMPL_STBL;
    }

    return AVRC_RSP_ACCEPT;
  }

  return code;
}

/***************************************************************************
 *  Function       send_metamsg_rsp
 *
 *  - Argument:
 *                  p_dev           Dev pointer
 *                  index           Command index (= -1 if not used)
 *                  label           Label of the RC response
 *                  code            Response type
 *                  pmetamsg_resp   Vendor response
 *
 *  - Description: Remote control metamsg response handler
 *
 ***************************************************************************/
static void send_metamsg_rsp(btif_rc_device_cb_t* p_dev, int index, uint8_t label,
                             tBTA_AV_CODE code, tAVRC_RESPONSE* pmetamsg_resp) {
  uint8_t ctype;

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  if (pmetamsg_resp == NULL) {
    log::warn("Invalid response received from application");
    return;
  }

  log::verbose("rc_handle: {}, index: {}, label: {}, code: 0x{:02x}, pdu: {}", p_dev->rc_handle,
               index, label, code, dump_rc_pdu(pmetamsg_resp->rsp.pdu));

  if (index >= 0 && !p_dev->rc_pdu_info[index].is_rsp_pending) {
    log::error("is_rsp_pending false, returning");
    return;
  }

  ctype = get_rsp_type_code(pmetamsg_resp->rsp.status, code);

  /* if response is for register_notification, make sure the rc has
  actually registered for this */
  if ((pmetamsg_resp->rsp.pdu == AVRC_PDU_REGISTER_NOTIFICATION) &&
      ((code == AVRC_RSP_CHANGED) || (code == AVRC_RSP_INTERIM))) {
    bool bSent = false;
    uint8_t event_id = pmetamsg_resp->reg_notif.event_id;
    bool bNotify = (p_dev->rc_connected) && (p_dev->rc_notif[event_id - 1].bNotify);

    /* de-register this notification for a CHANGED response */
    p_dev->rc_notif[event_id - 1].bNotify = false;
    log::verbose("rc_handle: {}. event_id: 0x{:02} bNotify: {}", p_dev->rc_handle, event_id,
                 bNotify);
    if (bNotify) {
      BT_HDR* p_msg = NULL;
      tAVRC_STS status;

      if (AVRC_STS_NO_ERROR ==
          (status = AVRC_BldResponse(p_dev->rc_handle, pmetamsg_resp, &p_msg))) {
        log::verbose("Sending notification to rc_handle: {}. event_id: 0x{:02}", p_dev->rc_handle,
                     event_id);
        bSent = true;
        BTA_AvMetaRsp(p_dev->rc_handle, p_dev->rc_notif[event_id - 1].label, ctype, p_msg);
      } else {
        log::warn("failed to build metamsg response. status: 0x{:02x}", status);
      }
    }

    if (!bSent) {
      log::verbose(
              "Notification not sent, as there are no RC connections or the CT has "
              "not subscribed for event_id: {}",
              dump_rc_notification_event_id(event_id));
    }
  } else {
    /* All other commands go here */

    BT_HDR* p_msg = NULL;
    tAVRC_STS status;

    status = AVRC_BldResponse(p_dev->rc_handle, pmetamsg_resp, &p_msg);

    if (status == AVRC_STS_NO_ERROR) {
      BTA_AvMetaRsp(p_dev->rc_handle, label, ctype, p_msg);
    } else {
      log::error("failed to build metamsg response. status: 0x{:02x}", status);
    }
  }

  if (index >= 0) {
    p_dev->rc_pdu_info[index].ctype = 0;
    p_dev->rc_pdu_info[index].label = 0;
    p_dev->rc_pdu_info[index].is_rsp_pending = false;
  }
}

static uint8_t opcode_from_pdu(uint8_t pdu) {
  uint8_t opcode = 0;

  switch (pdu) {
    case AVRC_PDU_SET_BROWSED_PLAYER:
    case AVRC_PDU_GET_FOLDER_ITEMS:
    case AVRC_PDU_CHANGE_PATH:
    case AVRC_PDU_GET_ITEM_ATTRIBUTES:
    case AVRC_PDU_ADD_TO_NOW_PLAYING:
    case AVRC_PDU_SEARCH:
    case AVRC_PDU_GET_TOTAL_NUM_OF_ITEMS:
    case AVRC_PDU_GENERAL_REJECT:
      opcode = AVRC_OP_BROWSE;
      break;

    case AVRC_PDU_NEXT_GROUP:
    case AVRC_PDU_PREV_GROUP: /* pass thru */
      opcode = AVRC_OP_PASS_THRU;
      break;

    default: /* vendor */
      opcode = AVRC_OP_VENDOR;
      break;
  }

  return opcode;
}

/***************************************************************************
 * Function:  fill_attribute_id_array
 *
 * - Argument:
 *     cmd_attribute_number         input attribute number from AVRCP command
 *     cmd_attribute_id_array       input attribute list from AVRCP command
 *     out_array_size               allocated size of out attribute id array
 *     out_attribute_id_array       output attribute list resolved here
 *
 * - Description:
 *     Resolve attribute id array as defined by the AVRCP specification.
 *
 * - Returns:
 *     The number of attributes filled in
 *
 ***************************************************************************/
static uint8_t fill_attribute_id_array(uint8_t cmd_attribute_number,
                                       const uint32_t* cmd_attribute_id_array,
                                       size_t out_array_size,
                                       btrc_media_attr_t* out_attribute_id_array) {
  /* Default case for cmd_attribute_number == 0xFF, No attribute */
  uint8_t out_attribute_number = 0;
  if (cmd_attribute_number == 0) {
    /* All attributes */
    out_attribute_number = out_array_size < AVRC_MAX_NUM_MEDIA_ATTR_ID ? out_array_size
                                                                       : AVRC_MAX_NUM_MEDIA_ATTR_ID;
    for (int i = 0; i < out_attribute_number; i++) {
      out_attribute_id_array[i] = (btrc_media_attr_t)(i + 1);
    }
  } else if (cmd_attribute_number != 0xFF) {
    /* Attribute List */
    out_attribute_number = 0;
    int filled_id_count = 0;
    for (int i = 0; (i < cmd_attribute_number) && (out_attribute_number < out_array_size) &&
                    (out_attribute_number < AVRC_MAX_NUM_MEDIA_ATTR_ID);
         i++) {
      /* Fill only valid entries */
      if (AVRC_IS_VALID_MEDIA_ATTRIBUTE(cmd_attribute_id_array[i])) {
        /* Skip the duplicate entries */
        for (filled_id_count = 0; filled_id_count < out_attribute_number; filled_id_count++) {
          if (out_attribute_id_array[filled_id_count] == cmd_attribute_id_array[i]) {
            break;
          }
        }
        /* New ID */
        if (filled_id_count == out_attribute_number) {
          out_attribute_id_array[out_attribute_number] =
                  (btrc_media_attr_t)cmd_attribute_id_array[i];
          out_attribute_number++;
        }
      }
    }
  }
  return out_attribute_number;
}

/*******************************************************************************
 *
 * Function         btif_rc_upstreams_evt
 *
 * Description      Executes AVRC UPSTREAMS events in btif context.
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_rc_upstreams_evt(uint16_t event, tAVRC_COMMAND* pavrc_cmd, uint8_t ctype,
                                  uint8_t label, btif_rc_device_cb_t* p_dev) {
  log::verbose("pdu: {} handle: 0x{:x} ctype: {:x} label: {:x} event ID: {:x}",
               dump_rc_pdu(pavrc_cmd->pdu), p_dev->rc_handle, ctype, label,
               pavrc_cmd->reg_notif.event_id);

  switch (event) {
    case AVRC_PDU_GET_PLAY_STATUS: {
      fill_pdu_queue(IDX_GET_PLAY_STATUS_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, get_play_status_cb, p_dev->rc_addr);
    } break;
    case AVRC_PDU_LIST_PLAYER_APP_ATTR:
    case AVRC_PDU_LIST_PLAYER_APP_VALUES:
    case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
    case AVRC_PDU_SET_PLAYER_APP_VALUE:
    case AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT:
    case AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT: {
      /* TODO: Add support for Application Settings */
      send_reject_response(p_dev->rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_CMD,
                           pavrc_cmd->cmd.opcode);
    } break;
    case AVRC_PDU_GET_ELEMENT_ATTR: {
      btrc_media_attr_t element_attrs[BTRC_MAX_ELEM_ATTR_SIZE] = {};
      uint8_t num_attr = fill_attribute_id_array(pavrc_cmd->get_elem_attrs.num_attr,
                                                 pavrc_cmd->get_elem_attrs.attrs,
                                                 BTRC_MAX_ELEM_ATTR_SIZE, element_attrs);
      if (num_attr == 0) {
        log::error("No valid attributes requested in GET_ELEMENT_ATTRIBUTES");
        send_reject_response(p_dev->rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_PARAM,
                             pavrc_cmd->cmd.opcode);
        return;
      }
      fill_pdu_queue(IDX_GET_ELEMENT_ATTR_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, get_element_attr_cb, num_attr, element_attrs, p_dev->rc_addr);
    } break;
    case AVRC_PDU_REGISTER_NOTIFICATION: {
      if (pavrc_cmd->reg_notif.event_id == BTRC_EVT_PLAY_POS_CHANGED &&
          pavrc_cmd->reg_notif.param == 0) {
        log::warn("Device registering position changed with illegal param 0.");
        send_reject_response(p_dev->rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_PARAM,
                             pavrc_cmd->cmd.opcode);
        /* de-register this notification for a rejected response */
        p_dev->rc_notif[BTRC_EVT_PLAY_POS_CHANGED - 1].bNotify = false;
        return;
      }
      HAL_CBACK(bt_rc_callbacks, register_notification_cb,
                (btrc_event_id_t)pavrc_cmd->reg_notif.event_id, pavrc_cmd->reg_notif.param,
                p_dev->rc_addr);
    } break;
    case AVRC_PDU_INFORM_DISPLAY_CHARSET: {
      tAVRC_RESPONSE avrc_rsp;
      log::verbose("AVRC_PDU_INFORM_DISPLAY_CHARSET");
      if (p_dev->rc_connected) {
        memset(&(avrc_rsp.inform_charset), 0, sizeof(tAVRC_RSP));
        avrc_rsp.inform_charset.opcode = opcode_from_pdu(AVRC_PDU_INFORM_DISPLAY_CHARSET);
        avrc_rsp.inform_charset.pdu = AVRC_PDU_INFORM_DISPLAY_CHARSET;
        avrc_rsp.inform_charset.status = AVRC_STS_NO_ERROR;
        send_metamsg_rsp(p_dev, -1, label, ctype, &avrc_rsp);
      }
    } break;

    case AVRC_PDU_GET_FOLDER_ITEMS: {
      uint32_t attr_ids[BTRC_MAX_ELEM_ATTR_SIZE] = {0};
      uint8_t num_attr;
      num_attr = pavrc_cmd->get_items.attr_count;

      log::verbose(
              "AVRC_PDU_GET_FOLDER_ITEMS num_attr: {}, start_item [{}] end_item "
              "[{}]",
              num_attr, pavrc_cmd->get_items.start_item, pavrc_cmd->get_items.end_item);

      /* num_attr requested:
       *     0x00: All attributes requested
       *     0xFF: No Attributes requested
       *     0x01 to 0x07: Specified number of attributes
       */
      if (num_attr != 0xFF && num_attr > BTRC_MAX_ELEM_ATTR_SIZE) {
        send_reject_response(p_dev->rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_PARAM,
                             pavrc_cmd->cmd.opcode);
        return;
      }

      /* Except num_attr is None(0xff) / All(0x00), request follows with an
       * Attribute List */
      if ((num_attr != 0xFF) && (num_attr != 0x00)) {
        memcpy(attr_ids, pavrc_cmd->get_items.p_attr_list, sizeof(uint32_t) * num_attr);
      }

      fill_pdu_queue(IDX_GET_FOLDER_ITEMS_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, get_folder_items_cb, pavrc_cmd->get_items.scope,
                pavrc_cmd->get_items.start_item, pavrc_cmd->get_items.end_item, num_attr, attr_ids,
                p_dev->rc_addr);
    } break;

    case AVRC_PDU_SET_ADDRESSED_PLAYER: {
      fill_pdu_queue(IDX_SET_ADDR_PLAYER_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, set_addressed_player_cb, pavrc_cmd->addr_player.player_id,
                p_dev->rc_addr);
    } break;

    case AVRC_PDU_SET_BROWSED_PLAYER: {
      fill_pdu_queue(IDX_SET_BROWSED_PLAYER_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, set_browsed_player_cb, pavrc_cmd->br_player.player_id,
                p_dev->rc_addr);
    } break;

    case AVRC_PDU_REQUEST_CONTINUATION_RSP: {
      log::verbose("REQUEST CONTINUATION: target_pdu: 0x{:02d}", pavrc_cmd->continu.target_pdu);
      tAVRC_RESPONSE avrc_rsp;
      if (p_dev->rc_connected) {
        memset(&(avrc_rsp.continu), 0, sizeof(tAVRC_NEXT_RSP));
        avrc_rsp.continu.opcode = opcode_from_pdu(AVRC_PDU_REQUEST_CONTINUATION_RSP);
        avrc_rsp.continu.pdu = AVRC_PDU_REQUEST_CONTINUATION_RSP;
        avrc_rsp.continu.status = AVRC_STS_NO_ERROR;
        avrc_rsp.continu.target_pdu = pavrc_cmd->continu.target_pdu;
        send_metamsg_rsp(p_dev, -1, label, ctype, &avrc_rsp);
      }
    } break;

    case AVRC_PDU_ABORT_CONTINUATION_RSP: {
      log::verbose("ABORT CONTINUATION: target_pdu: 0x{:02d}", pavrc_cmd->abort.target_pdu);
      tAVRC_RESPONSE avrc_rsp;
      if (p_dev->rc_connected) {
        memset(&(avrc_rsp.abort), 0, sizeof(tAVRC_NEXT_RSP));
        avrc_rsp.abort.opcode = opcode_from_pdu(AVRC_PDU_ABORT_CONTINUATION_RSP);
        avrc_rsp.abort.pdu = AVRC_PDU_ABORT_CONTINUATION_RSP;
        avrc_rsp.abort.status = AVRC_STS_NO_ERROR;
        avrc_rsp.abort.target_pdu = pavrc_cmd->continu.target_pdu;
        send_metamsg_rsp(p_dev, -1, label, ctype, &avrc_rsp);
      }
    } break;

    case AVRC_PDU_CHANGE_PATH: {
      fill_pdu_queue(IDX_CHG_PATH_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, change_path_cb, pavrc_cmd->chg_path.direction,
                pavrc_cmd->chg_path.folder_uid, p_dev->rc_addr);
    } break;

    case AVRC_PDU_SEARCH: {
      fill_pdu_queue(IDX_SEARCH_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, search_cb, pavrc_cmd->search.string.charset_id,
                pavrc_cmd->search.string.str_len, pavrc_cmd->search.string.p_str, p_dev->rc_addr);
    } break;

    case AVRC_PDU_GET_ITEM_ATTRIBUTES: {
      btrc_media_attr_t item_attrs[BTRC_MAX_ELEM_ATTR_SIZE] = {};
      uint8_t num_attr = fill_attribute_id_array(pavrc_cmd->get_attrs.attr_count,
                                                 pavrc_cmd->get_attrs.p_attr_list,
                                                 BTRC_MAX_ELEM_ATTR_SIZE, item_attrs);
      if (num_attr == 0) {
        log::error("No valid attributes requested in GET_ITEM_ATTRIBUTES");
        send_reject_response(p_dev->rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_PARAM,
                             pavrc_cmd->cmd.opcode);
        return;
      }
      fill_pdu_queue(IDX_GET_ITEM_ATTR_RSP, ctype, label, true, p_dev);
      log::verbose("GET_ITEM_ATTRIBUTES: num_attr: {}", num_attr);
      HAL_CBACK(bt_rc_callbacks, get_item_attr_cb, pavrc_cmd->get_attrs.scope,
                pavrc_cmd->get_attrs.uid, pavrc_cmd->get_attrs.uid_counter, num_attr, item_attrs,
                p_dev->rc_addr);
    } break;

    case AVRC_PDU_GET_TOTAL_NUM_OF_ITEMS: {
      fill_pdu_queue(IDX_GET_TOTAL_NUM_OF_ITEMS_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, get_total_num_of_items_cb, pavrc_cmd->get_num_of_items.scope,
                p_dev->rc_addr);
    } break;

    case AVRC_PDU_ADD_TO_NOW_PLAYING: {
      fill_pdu_queue(IDX_ADD_TO_NOW_PLAYING_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, add_to_now_playing_cb, pavrc_cmd->add_to_play.scope,
                pavrc_cmd->add_to_play.uid, pavrc_cmd->add_to_play.uid_counter, p_dev->rc_addr);
    } break;

    case AVRC_PDU_PLAY_ITEM: {
      fill_pdu_queue(IDX_PLAY_ITEM_RSP, ctype, label, true, p_dev);
      HAL_CBACK(bt_rc_callbacks, play_item_cb, pavrc_cmd->play_item.scope,
                pavrc_cmd->play_item.uid_counter, pavrc_cmd->play_item.uid, p_dev->rc_addr);
    } break;

    default: {
      send_reject_response(p_dev->rc_handle, label, pavrc_cmd->pdu, AVRC_STS_BAD_CMD,
                           pavrc_cmd->cmd.opcode);
      return;
    } break;
  }
}

/*******************************************************************************
 *
 * Function         btif_rc_ctrl_upstreams_rsp_cmd
 *
 * Description      Executes AVRC UPSTREAMS response events in btif context.
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_rc_ctrl_upstreams_rsp_cmd(uint8_t event, tAVRC_COMMAND* pavrc_cmd, uint8_t label,
                                           btif_rc_device_cb_t* p_dev) {
  log::verbose("pdu: {}: handle: 0x{:x}", dump_rc_pdu(pavrc_cmd->pdu), p_dev->rc_handle);
  switch (event) {
    case AVRC_PDU_SET_ABSOLUTE_VOLUME:
      do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->setabsvol_cmd_cb, p_dev->rc_addr,
                                      pavrc_cmd->volume.volume, label));
      break;
    case AVRC_PDU_REGISTER_NOTIFICATION:
      if (pavrc_cmd->reg_notif.event_id == AVRC_EVT_VOLUME_CHANGE) {
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->registernotification_absvol_cb,
                                        p_dev->rc_addr, label));
      }
      break;
  }
}

/*******************************************************************************
 *
 * Function         btif_rc_upstreams_rsp_evt
 *
 * Description      Executes AVRC UPSTREAMS response events in btif context.
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_rc_upstreams_rsp_evt(uint16_t event, tAVRC_RESPONSE* pavrc_resp, uint8_t ctype,
                                      uint8_t label, btif_rc_device_cb_t* p_dev) {
  log::verbose("pdu: {}: handle: 0x{:x} ctype: {:x} label: {:x}", dump_rc_pdu(pavrc_resp->pdu),
               p_dev->rc_handle, ctype, label);

  switch (event) {
    case AVRC_PDU_REGISTER_NOTIFICATION: {
      if (AVRC_RSP_CHANGED == ctype) {
        p_dev->rc_volume = pavrc_resp->reg_notif.param.volume;
      }
      HAL_CBACK(bt_rc_callbacks, volume_change_cb, pavrc_resp->reg_notif.param.volume, ctype,
                p_dev->rc_addr);
    } break;

    case AVRC_PDU_SET_ABSOLUTE_VOLUME: {
      log::verbose("Set absolute volume change event received: volume: {}, ctype: {}",
                   pavrc_resp->volume.volume, ctype);
      if (AVRC_RSP_ACCEPT == ctype) {
        p_dev->rc_volume = pavrc_resp->volume.volume;
      }
      HAL_CBACK(bt_rc_callbacks, volume_change_cb, pavrc_resp->volume.volume, ctype,
                p_dev->rc_addr);
    } break;

    default:
      return;
  }
}

/*******************************************************************************
 *  AVRCP API Functions
 ******************************************************************************/

/*******************************************************************************
 *
 * Function         init
 *
 * Description      Initializes the AVRC interface
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t init(btrc_callbacks_t* callbacks) {
  log::verbose("");
  bt_status_t result = BT_STATUS_SUCCESS;

  if (bt_rc_callbacks) {
    return BT_STATUS_DONE;
  }

  bt_rc_callbacks = callbacks;
  if (bt_rc_ctrl_callbacks) {
    return BT_STATUS_SUCCESS;
  }

  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    initialize_device(&btif_rc_cb.rc_multi_cb[idx]);
  }

  return result;
}

/*******************************************************************************
 *
 * Function         init_ctrl
 *
 * Description      Initializes the AVRC interface
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static bt_status_t init_ctrl(btrc_ctrl_callbacks_t* callbacks) {
  log::verbose("");
  bt_status_t result = BT_STATUS_SUCCESS;

  if (bt_rc_ctrl_callbacks) {
    return BT_STATUS_DONE;
  }

  bt_rc_ctrl_callbacks = callbacks;
  if (bt_rc_callbacks) {
    return BT_STATUS_SUCCESS;
  }

  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    initialize_device(&btif_rc_cb.rc_multi_cb[idx]);
  }

  return result;
}

static void rc_ctrl_procedure_complete(btif_rc_device_cb_t* p_dev) {
  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  if (p_dev->rc_procedure_complete) {
    return;
  }
  p_dev->rc_procedure_complete = true;
  const uint32_t* attr_list = get_requested_attributes_list(p_dev);
  const uint8_t attr_list_size = get_requested_attributes_list_size(p_dev);
  get_metadata_attribute_cmd(attr_list_size, attr_list, p_dev);
}

/***************************************************************************
 *
 * Function         get_play_status_rsp
 *
 * Description      Returns the current play status.
 *                      This method is called in response to
 *                      GetPlayStatus request.
 *
 * Returns          bt_status_t
 *
 **************************************************************************/
static bt_status_t get_play_status_rsp(const RawAddress& bd_addr, btrc_play_status_t play_status,
                                       uint32_t song_len, uint32_t song_pos) {
  tAVRC_RESPONSE avrc_rsp;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("song len {} song pos {}", song_len, song_pos);
  CHECK_RC_CONNECTED(p_dev);

  memset(&(avrc_rsp.get_play_status), 0, sizeof(tAVRC_GET_PLAY_STATUS_RSP));

  avrc_rsp.get_play_status.song_len = song_len;
  avrc_rsp.get_play_status.song_pos = song_pos;
  avrc_rsp.get_play_status.play_status = play_status;

  avrc_rsp.get_play_status.pdu = AVRC_PDU_GET_PLAY_STATUS;
  avrc_rsp.get_play_status.opcode = opcode_from_pdu(AVRC_PDU_GET_PLAY_STATUS);
  avrc_rsp.get_play_status.status =
          ((play_status != BTRC_PLAYSTATE_ERROR) ? AVRC_STS_NO_ERROR : AVRC_STS_BAD_PARAM);

  /* Send the response */
  send_metamsg_rsp(p_dev, IDX_GET_PLAY_STATUS_RSP,
                   p_dev->rc_pdu_info[IDX_GET_PLAY_STATUS_RSP].label,
                   p_dev->rc_pdu_info[IDX_GET_PLAY_STATUS_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         get_element_attr_rsp
 *
 * Description      Returns the current songs' element attributes
 *                      in text.
 *
 * Returns          bt_status_t
 *
 **************************************************************************/
static bt_status_t get_element_attr_rsp(const RawAddress& bd_addr, uint8_t num_attr,
                                        btrc_element_attr_val_t* p_attrs) {
  tAVRC_RESPONSE avrc_rsp;
  uint32_t i;
  tAVRC_ATTR_ENTRY element_attrs[BTRC_MAX_ELEM_ATTR_SIZE];
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  if (num_attr > BTRC_MAX_ELEM_ATTR_SIZE) {
    log::warn("Exceeded number attributes:{} max:{}", static_cast<int>(num_attr),
              BTRC_MAX_ELEM_ATTR_SIZE);
    num_attr = BTRC_MAX_ELEM_ATTR_SIZE;
  }
  memset(element_attrs, 0, sizeof(tAVRC_ATTR_ENTRY) * num_attr);

  if (num_attr == 0) {
    avrc_rsp.get_play_status.status = AVRC_STS_BAD_PARAM;
  } else {
    for (i = 0; i < num_attr; i++) {
      element_attrs[i].attr_id = p_attrs[i].attr_id;
      element_attrs[i].name.charset_id = AVRC_CHARSET_ID_UTF8;
      element_attrs[i].name.str_len =
              (uint16_t)strnlen((char*)p_attrs[i].text, BTRC_MAX_ATTR_STR_LEN);
      element_attrs[i].name.p_str = p_attrs[i].text;
      log::verbose("attr_id: 0x{:x}, charset_id: 0x{:x}, str_len: {}, str: {}",
                   (unsigned int)element_attrs[i].attr_id, element_attrs[i].name.charset_id,
                   element_attrs[i].name.str_len,
                   reinterpret_cast<char const*>(element_attrs[i].name.p_str));
    }
    avrc_rsp.get_play_status.status = AVRC_STS_NO_ERROR;
  }
  avrc_rsp.get_attrs.num_attrs = num_attr;
  avrc_rsp.get_attrs.p_attrs = element_attrs;
  avrc_rsp.get_attrs.pdu = AVRC_PDU_GET_ELEMENT_ATTR;
  avrc_rsp.get_attrs.opcode = opcode_from_pdu(AVRC_PDU_GET_ELEMENT_ATTR);

  /* Send the response */
  send_metamsg_rsp(p_dev, IDX_GET_ELEMENT_ATTR_RSP,
                   p_dev->rc_pdu_info[IDX_GET_ELEMENT_ATTR_RSP].label,
                   p_dev->rc_pdu_info[IDX_GET_ELEMENT_ATTR_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         register_notification_rsp
 *
 * Description      Response to the register notification request.
 *
 * Returns          bt_status_t
 *
 **************************************************************************/
static bt_status_t register_notification_rsp(btrc_event_id_t event_id,
                                             btrc_notification_type_t type,
                                             btrc_register_notification_t* p_param) {
  tAVRC_RESPONSE avrc_rsp;
  log::verbose("event_id: {}", dump_rc_notification_event_id(event_id));
  std::unique_lock<std::mutex> lock(btif_rc_cb.lock);

  if (event_id > MAX_RC_NOTIFICATIONS) {
    log::error("Invalid event id");
    return BT_STATUS_PARM_INVALID;
  }

  memset(&(avrc_rsp.reg_notif), 0, sizeof(tAVRC_REG_NOTIF_RSP));

  avrc_rsp.reg_notif.event_id = event_id;
  avrc_rsp.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
  avrc_rsp.reg_notif.opcode = opcode_from_pdu(AVRC_PDU_REGISTER_NOTIFICATION);
  avrc_rsp.get_play_status.status = AVRC_STS_NO_ERROR;

  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    memset(&(avrc_rsp.reg_notif.param), 0, sizeof(tAVRC_NOTIF_RSP_PARAM));

    if (!(btif_rc_cb.rc_multi_cb[idx].rc_connected)) {
      log::error("Avrcp device is not connected, handle: 0x{:x}",
                 btif_rc_cb.rc_multi_cb[idx].rc_handle);
      continue;
    }

    if (!btif_rc_cb.rc_multi_cb[idx].rc_notif[event_id - 1].bNotify) {
      log::warn("Avrcp Event id is not registered: event_id: {:x}, handle: 0x{:x}", event_id,
                btif_rc_cb.rc_multi_cb[idx].rc_handle);
      continue;
    }

    log::verbose("Avrcp Event id is registered: event_id: {:x} handle: 0x{:x}", event_id,
                 btif_rc_cb.rc_multi_cb[idx].rc_handle);

    switch (event_id) {
      case BTRC_EVT_PLAY_STATUS_CHANGED:
        avrc_rsp.reg_notif.param.play_status = p_param->play_status;
        if (avrc_rsp.reg_notif.param.play_status == PLAY_STATUS_PLAYING) {
          btif_av_clear_remote_suspend_flag(A2dpType::kSink);
        }
        break;
      case BTRC_EVT_TRACK_CHANGE:
        memcpy(&(avrc_rsp.reg_notif.param.track), &(p_param->track), sizeof(btrc_uid_t));
        break;
      case BTRC_EVT_PLAY_POS_CHANGED:
        avrc_rsp.reg_notif.param.play_pos = p_param->song_pos;
        break;
      case BTRC_EVT_AVAL_PLAYER_CHANGE:
        break;
      case BTRC_EVT_ADDR_PLAYER_CHANGE:
        avrc_rsp.reg_notif.param.addr_player.player_id = p_param->addr_player_changed.player_id;
        avrc_rsp.reg_notif.param.addr_player.uid_counter = p_param->addr_player_changed.uid_counter;
        break;
      case BTRC_EVT_UIDS_CHANGED:
        avrc_rsp.reg_notif.param.uid_counter = p_param->uids_changed.uid_counter;
        break;
      case BTRC_EVT_NOW_PLAYING_CONTENT_CHANGED:
        break;

      default:
        log::warn("Unhandled event ID: 0x{:x}", event_id);
        return BT_STATUS_UNHANDLED;
    }

    /* Send the response. */
    send_metamsg_rsp(&btif_rc_cb.rc_multi_cb[idx], -1,
                     btif_rc_cb.rc_multi_cb[idx].rc_notif[event_id - 1].label,
                     ((type == BTRC_NOTIFICATION_TYPE_INTERIM) ? AVRC_CMD_NOTIF : AVRC_RSP_CHANGED),
                     &avrc_rsp);
  }
  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         get_folder_items_list_rsp
 *
 * Description      Returns the list of media items in current folder along with
 *                  requested attributes. This is called in response to
 *                  GetFolderItems request.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *                      BT_STATUS_UNHANDLED - when rsp is not pending for
 *                                            get_folder_items_list PDU
 *
 **************************************************************************/
static bt_status_t get_folder_items_list_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status,
                                             uint16_t uid_counter, uint8_t num_items,
                                             btrc_folder_items_t* p_items) {
  tAVRC_RESPONSE avrc_rsp;
  tAVRC_ITEM item;
  tBTA_AV_CODE code = 0, ctype = 0;
  BT_HDR* p_msg = NULL;
  int item_cnt;
  tAVRC_STS status = AVRC_STS_NO_ERROR;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  btrc_folder_items_t* cur_item = NULL;

  log::verbose("uid_counter {} num_items {}", uid_counter, num_items);
  CHECK_RC_CONNECTED(p_dev);

  /* check if rsp to previous cmd was completed */
  if (!p_dev->rc_pdu_info[IDX_GET_FOLDER_ITEMS_RSP].is_rsp_pending) {
    log::warn("Not sending response as no PDU was registered");
    return BT_STATUS_UNHANDLED;
  }

  memset(&avrc_rsp, 0, sizeof(tAVRC_RESPONSE));
  memset(&item, 0, sizeof(tAVRC_ITEM));

  avrc_rsp.get_items.pdu = AVRC_PDU_GET_FOLDER_ITEMS;
  avrc_rsp.get_items.opcode = opcode_from_pdu(AVRC_PDU_GET_FOLDER_ITEMS);
  avrc_rsp.get_items.status = status_code_map[rsp_status];

  if (avrc_rsp.get_items.status != AVRC_STS_NO_ERROR) {
    log::warn("Error in parsing the received getfolderitems cmd. status: 0x{:02x}",
              avrc_rsp.get_items.status);
    status = avrc_rsp.get_items.status;
  } else {
    avrc_rsp.get_items.uid_counter = uid_counter;
    avrc_rsp.get_items.item_count = 1;

    /* create single item and build response iteratively for all num_items */
    for (item_cnt = 0; item_cnt < num_items; item_cnt++) {
      cur_item = &p_items[item_cnt];
      item.item_type = p_items->item_type;
      /* build respective item based on item_type. All items should be of same
       * type within
       * a response */
      switch (p_items->item_type) {
        case AVRC_ITEM_PLAYER: {
          item.u.player.name.charset_id = cur_item->player.charset_id;
          memcpy(&(item.u.player.features), &(cur_item->player.features),
                 sizeof(cur_item->player.features));
          item.u.player.major_type = cur_item->player.major_type;
          item.u.player.sub_type = cur_item->player.sub_type;
          item.u.player.play_status = cur_item->player.play_status;
          item.u.player.player_id = cur_item->player.player_id;
          item.u.player.name.p_str = cur_item->player.name;
          item.u.player.name.str_len = (uint16_t)strlen((char*)(cur_item->player.name));
        } break;

        case AVRC_ITEM_FOLDER: {
          memcpy(item.u.folder.uid, cur_item->folder.uid, sizeof(tAVRC_UID));
          item.u.folder.type = cur_item->folder.type;
          item.u.folder.playable = cur_item->folder.playable;
          item.u.folder.name.charset_id = AVRC_CHARSET_ID_UTF8;
          item.u.folder.name.str_len = strlen((char*)cur_item->folder.name);
          item.u.folder.name.p_str = cur_item->folder.name;
        } break;

        case AVRC_ITEM_MEDIA: {
          tAVRC_ATTR_ENTRY attr_vals[BTRC_MAX_ELEM_ATTR_SIZE] = {};

          memcpy(item.u.media.uid, cur_item->media.uid, sizeof(tAVRC_UID));
          item.u.media.type = cur_item->media.type;
          item.u.media.name.charset_id = cur_item->media.charset_id;
          item.u.media.name.str_len = strlen((char*)cur_item->media.name);
          item.u.media.name.p_str = cur_item->media.name;
          item.u.media.attr_count = cur_item->media.num_attrs;

          /* Handle attributes of given item */
          if (item.u.media.attr_count == 0) {
            item.u.media.p_attr_list = NULL;
          } else {
            memset(&attr_vals, 0, sizeof(tAVRC_ATTR_ENTRY) * BTRC_MAX_ELEM_ATTR_SIZE);
            fill_avrc_attr_entry(attr_vals, item.u.media.attr_count, cur_item->media.p_attrs);
            item.u.media.p_attr_list = attr_vals;
          }
        } break;

        default: {
          log::error("Unknown item_type: {}. Internal Error", p_items->item_type);
          status = AVRC_STS_INTERNAL_ERR;
        } break;
      }

      avrc_rsp.get_items.p_item_list = &item;

      /* Add current item to buffer and build response if no error in item type
       */
      if (status != AVRC_STS_NO_ERROR) {
        /* Reject response due to error occurred for unknown item_type, break the
         * loop */
        break;
      }

      int len_before = p_msg ? p_msg->len : 0;
      log::verbose("item_cnt: {} len: {}", item_cnt, len_before);
      status = AVRC_BldResponse(p_dev->rc_handle, &avrc_rsp, &p_msg);
      log::verbose("Build rsp status: {} len: {}", status, p_msg ? p_msg->len : 0);
      int len_after = p_msg ? p_msg->len : 0;
      if (status != AVRC_STS_NO_ERROR || len_before == len_after) {
        /* Error occurred in build response or we ran out of buffer so break the
         * loop */
        break;
      }
    }

    /* setting the error status */
    avrc_rsp.get_items.status = status;
  }

  /* if packet built successfully, send the built items to BTA layer */
  if (status == AVRC_STS_NO_ERROR) {
    code = p_dev->rc_pdu_info[IDX_GET_FOLDER_ITEMS_RSP].ctype;
    ctype = get_rsp_type_code(avrc_rsp.get_items.status, code);
    BTA_AvMetaRsp(p_dev->rc_handle, p_dev->rc_pdu_info[IDX_GET_FOLDER_ITEMS_RSP].label, ctype,
                  p_msg);
  } else {
    /* Error occurred, send reject response */
    log::error("Error status: 0x{:02X}. Sending reject rsp", avrc_rsp.rsp.status);
    send_reject_response(p_dev->rc_handle, p_dev->rc_pdu_info[IDX_GET_FOLDER_ITEMS_RSP].label,
                         avrc_rsp.pdu, avrc_rsp.get_items.status, avrc_rsp.get_items.opcode);
  }

  /* Reset values for current pdu. */
  p_dev->rc_pdu_info[IDX_GET_FOLDER_ITEMS_RSP].ctype = 0;
  p_dev->rc_pdu_info[IDX_GET_FOLDER_ITEMS_RSP].label = 0;
  p_dev->rc_pdu_info[IDX_GET_FOLDER_ITEMS_RSP].is_rsp_pending = false;

  return status == AVRC_STS_NO_ERROR ? BT_STATUS_SUCCESS : BT_STATUS_FAIL;
}

/***************************************************************************
 *
 * Function         set_addressed_player_rsp
 *
 * Description      Response to set the addressed player for specified media
 *                  player based on id in the media player list.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *
 **************************************************************************/
static bt_status_t set_addressed_player_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status) {
  tAVRC_RESPONSE avrc_rsp;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  avrc_rsp.addr_player.pdu = AVRC_PDU_SET_ADDRESSED_PLAYER;
  avrc_rsp.addr_player.opcode = opcode_from_pdu(AVRC_PDU_SET_ADDRESSED_PLAYER);
  avrc_rsp.addr_player.status = status_code_map[rsp_status];

  /* Send the response. */
  send_metamsg_rsp(p_dev, IDX_SET_ADDR_PLAYER_RSP,
                   p_dev->rc_pdu_info[IDX_SET_ADDR_PLAYER_RSP].label,
                   p_dev->rc_pdu_info[IDX_SET_ADDR_PLAYER_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         set_browsed_player_rsp
 *
 * Description      Response to set the browsed player command which contains
 *                  current browsed path of the media player. By default,
 *                  current_path = root and folder_depth = 0 for
 *                  every set_browsed_player request.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - if RC is connected and response
 *                                            sent successfully
 *                      BT_STATUS_UNHANDLED - when rsp is not pending for
 *                                            set_browsed_player PDU
 *
 **************************************************************************/
static bt_status_t set_browsed_player_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status,
                                          uint32_t num_items, uint16_t charset_id,
                                          uint8_t folder_depth, btrc_br_folder_name_t* p_folders) {
  tAVRC_RESPONSE avrc_rsp;
  tAVRC_NAME item;
  BT_HDR* p_msg = NULL;
  tBTA_AV_CODE code = 0;
  tBTA_AV_CODE ctype = 0;
  unsigned int item_cnt;
  tAVRC_STS status = AVRC_STS_NO_ERROR;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  CHECK_RC_CONNECTED(p_dev);

  memset(&avrc_rsp, 0, sizeof(tAVRC_RESPONSE));
  memset(&item, 0, sizeof(tAVRC_NAME));

  avrc_rsp.br_player.status = status_code_map[rsp_status];
  avrc_rsp.br_player.pdu = AVRC_PDU_SET_BROWSED_PLAYER;
  avrc_rsp.br_player.opcode = opcode_from_pdu(AVRC_PDU_SET_BROWSED_PLAYER);

  log::verbose("rsp_status: 0x{:02X} avrc_rsp.br_player.status: 0x{:02X}", rsp_status,
               avrc_rsp.br_player.status);

  /* check if rsp to previous cmd was completed */
  if (!p_dev->rc_pdu_info[IDX_SET_BROWSED_PLAYER_RSP].is_rsp_pending) {
    log::warn("Not sending response as no PDU was registered");
    return BT_STATUS_UNHANDLED;
  }

  if (AVRC_STS_NO_ERROR == avrc_rsp.get_items.status) {
    avrc_rsp.br_player.num_items = num_items;
    avrc_rsp.br_player.charset_id = charset_id;
    avrc_rsp.br_player.folder_depth = folder_depth;
    avrc_rsp.br_player.p_folders = (tAVRC_NAME*)p_folders;

    log::verbose("folder_depth: 0x{:02X} num_items: {}", folder_depth, num_items);

    if (folder_depth > 0) {
      /* Iteratively build response for all folders across folder depth upto
       * current path */
      avrc_rsp.br_player.folder_depth = 1;
      for (item_cnt = 0; item_cnt < folder_depth; item_cnt++) {
        log::verbose("iteration: {}", item_cnt);
        item.str_len = p_folders[item_cnt].str_len;
        item.p_str = p_folders[item_cnt].p_str;
        avrc_rsp.br_player.p_folders = &item;

        /* Add current item to buffer and build response */
        status = AVRC_BldResponse(p_dev->rc_handle, &avrc_rsp, &p_msg);
        if (AVRC_STS_NO_ERROR != status) {
          log::warn("Build rsp status: {}", status);
          /* if the build fails, it is likely that we ran out of buffer. so if
           * we have
           * some items to send, reset this error to no error for sending what we
           * have */
          if (item_cnt > 0) {
            status = AVRC_STS_NO_ERROR;
          }

          /* Error occurred in build response so break the loop */
          break;
        }
      }
    } else /* current path is root folder, no folders navigated yet */
    {
      status = AVRC_BldResponse(p_dev->rc_handle, &avrc_rsp, &p_msg);
    }

    /* setting the error status */
    avrc_rsp.br_player.status = status;
  } else /* error received from above layer */
  {
    log::warn("Error in parsing the received setbrowsed command. status: 0x{:02x}",
              avrc_rsp.br_player.status);
    status = avrc_rsp.br_player.status;
  }

  /* if packet built successfully, send the built items to BTA layer */
  if (status == AVRC_STS_NO_ERROR) {
    code = p_dev->rc_pdu_info[IDX_SET_BROWSED_PLAYER_RSP].ctype;
    ctype = get_rsp_type_code(avrc_rsp.br_player.status, code);
    BTA_AvMetaRsp(p_dev->rc_handle, p_dev->rc_pdu_info[IDX_SET_BROWSED_PLAYER_RSP].label, ctype,
                  p_msg);
  } else {
    /* Error occurred, send reject response */
    log::error("Error status: 0x{:02X}. Sending reject rsp", avrc_rsp.br_player.status);
    send_reject_response(p_dev->rc_handle, p_dev->rc_pdu_info[IDX_SET_BROWSED_PLAYER_RSP].label,
                         avrc_rsp.pdu, avrc_rsp.br_player.status, avrc_rsp.get_items.opcode);
  }

  /* Reset values for set_browsed_player pdu.*/
  p_dev->rc_pdu_info[IDX_SET_BROWSED_PLAYER_RSP].ctype = 0;
  p_dev->rc_pdu_info[IDX_SET_BROWSED_PLAYER_RSP].label = 0;
  p_dev->rc_pdu_info[IDX_SET_BROWSED_PLAYER_RSP].is_rsp_pending = false;

  return status == AVRC_STS_NO_ERROR ? BT_STATUS_SUCCESS : BT_STATUS_FAIL;
}

/*******************************************************************************
 *
 * Function         change_path_rsp
 *
 * Description      Response to the change path command which
 *                  contains number of items in the changed path.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *
 **************************************************************************/
static bt_status_t change_path_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status,
                                   uint32_t num_items) {
  tAVRC_RESPONSE avrc_rsp;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  avrc_rsp.chg_path.pdu = AVRC_PDU_CHANGE_PATH;
  avrc_rsp.chg_path.opcode = opcode_from_pdu(AVRC_PDU_CHANGE_PATH);
  avrc_rsp.chg_path.num_items = num_items;
  avrc_rsp.chg_path.status = status_code_map[rsp_status];

  /* Send the response. */
  send_metamsg_rsp(p_dev, IDX_CHG_PATH_RSP, p_dev->rc_pdu_info[IDX_CHG_PATH_RSP].label,
                   p_dev->rc_pdu_info[IDX_CHG_PATH_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         search_rsp
 *
 * Description      Response to search a string from media content command.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *
 **************************************************************************/
static bt_status_t search_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status,
                              uint32_t uid_counter, uint32_t num_items) {
  tAVRC_RESPONSE avrc_rsp;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  avrc_rsp.search.pdu = AVRC_PDU_SEARCH;
  avrc_rsp.search.opcode = opcode_from_pdu(AVRC_PDU_SEARCH);
  avrc_rsp.search.num_items = num_items;
  avrc_rsp.search.uid_counter = uid_counter;
  avrc_rsp.search.status = status_code_map[rsp_status];

  /* Send the response. */
  send_metamsg_rsp(p_dev, IDX_SEARCH_RSP, p_dev->rc_pdu_info[IDX_SEARCH_RSP].label,
                   p_dev->rc_pdu_info[IDX_SEARCH_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}
/***************************************************************************
 *
 * Function         get_item_attr_rsp
 *
 * Description      Response to the get item's attributes command which
 *                  contains number of attributes and values list in text.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *
 **************************************************************************/
static bt_status_t get_item_attr_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status,
                                     uint8_t num_attr, btrc_element_attr_val_t* p_attrs) {
  tAVRC_RESPONSE avrc_rsp;
  tAVRC_ATTR_ENTRY item_attrs[BTRC_MAX_ELEM_ATTR_SIZE];
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  memset(item_attrs, 0, sizeof(tAVRC_ATTR_ENTRY) * num_attr);

  avrc_rsp.get_attrs.status = status_code_map[rsp_status];
  if (rsp_status == BTRC_STS_NO_ERROR) {
    fill_avrc_attr_entry(item_attrs, num_attr, p_attrs);
  }

  avrc_rsp.get_attrs.num_attrs = num_attr;
  avrc_rsp.get_attrs.p_attrs = item_attrs;
  avrc_rsp.get_attrs.pdu = AVRC_PDU_GET_ITEM_ATTRIBUTES;
  avrc_rsp.get_attrs.opcode = opcode_from_pdu(AVRC_PDU_GET_ITEM_ATTRIBUTES);

  /* Send the response. */
  send_metamsg_rsp(p_dev, IDX_GET_ITEM_ATTR_RSP, p_dev->rc_pdu_info[IDX_GET_ITEM_ATTR_RSP].label,
                   p_dev->rc_pdu_info[IDX_GET_ITEM_ATTR_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         add_to_now_playing_rsp
 *
 * Description      Response to command for adding speciafied media item
 *                  to Now Playing queue.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *
 **************************************************************************/
static bt_status_t add_to_now_playing_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status) {
  tAVRC_RESPONSE avrc_rsp;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  avrc_rsp.add_to_play.pdu = AVRC_PDU_ADD_TO_NOW_PLAYING;
  avrc_rsp.add_to_play.opcode = opcode_from_pdu(AVRC_PDU_ADD_TO_NOW_PLAYING);
  avrc_rsp.add_to_play.status = status_code_map[rsp_status];

  /* Send the response. */
  send_metamsg_rsp(p_dev, IDX_ADD_TO_NOW_PLAYING_RSP,
                   p_dev->rc_pdu_info[IDX_ADD_TO_NOW_PLAYING_RSP].label,
                   p_dev->rc_pdu_info[IDX_ADD_TO_NOW_PLAYING_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         play_item_rsp
 *
 * Description      Response to command for playing the specified media item.
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *
 **************************************************************************/
static bt_status_t play_item_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status) {
  tAVRC_RESPONSE avrc_rsp;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  avrc_rsp.play_item.pdu = AVRC_PDU_PLAY_ITEM;
  avrc_rsp.play_item.opcode = opcode_from_pdu(AVRC_PDU_PLAY_ITEM);
  avrc_rsp.play_item.status = status_code_map[rsp_status];

  /* Send the response. */
  send_metamsg_rsp(p_dev, IDX_PLAY_ITEM_RSP, p_dev->rc_pdu_info[IDX_PLAY_ITEM_RSP].label,
                   p_dev->rc_pdu_info[IDX_PLAY_ITEM_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         get_total_num_of_items_rsp
 *
 * Description      response to command to get the Number of Items
 *                  in the selected folder at the selected scope
 *
 * Returns          bt_status_t
 *                      BT_STATUS_NOT_READY - when RC is not connected.
 *                      BT_STATUS_SUCCESS   - always if RC is connected
 *
 **************************************************************************/
static bt_status_t get_total_num_of_items_rsp(const RawAddress& bd_addr, btrc_status_t rsp_status,
                                              uint32_t uid_counter, uint32_t num_items) {
  tAVRC_RESPONSE avrc_rsp;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  avrc_rsp.get_num_of_items.pdu = AVRC_PDU_GET_TOTAL_NUM_OF_ITEMS;
  avrc_rsp.get_num_of_items.opcode = opcode_from_pdu(AVRC_PDU_GET_TOTAL_NUM_OF_ITEMS);
  avrc_rsp.get_num_of_items.num_items = num_items;
  avrc_rsp.get_num_of_items.uid_counter = uid_counter;
  avrc_rsp.get_num_of_items.status = status_code_map[rsp_status];

  /* Send the response. */
  send_metamsg_rsp(p_dev, IDX_GET_TOTAL_NUM_OF_ITEMS_RSP,
                   p_dev->rc_pdu_info[IDX_GET_TOTAL_NUM_OF_ITEMS_RSP].label,
                   p_dev->rc_pdu_info[IDX_GET_TOTAL_NUM_OF_ITEMS_RSP].ctype, &avrc_rsp);

  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         set_volume
 *
 * Description      Send current volume setting to remote side.
 *                  Support limited to SetAbsoluteVolume
 *                  This can be enhanced to support Relative Volume (AVRCP 1.0).
 *                  With RelateVolume, we will send VOLUME_UP/VOLUME_DOWN
 *                  as opposed to absolute volume level
 * volume: Should be in the range 0-127. bit7 is reserved and cannot be set
 *
 * Returns          bt_status_t
 *
 **************************************************************************/
static bt_status_t set_volume(uint8_t volume) {
  log::verbose("volume: {}", volume);
  tAVRC_STS status = BT_STATUS_UNSUPPORTED;

  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    btif_rc_device_cb_t* p_dev = &btif_rc_cb.rc_multi_cb[idx];
    if (!p_dev->rc_connected) {
      continue;
    }

    if (p_dev->rc_volume == volume) {
      status = BT_STATUS_DONE;
      log::error("volume value already set earlier: 0x{:02x}", volume);
      continue;
    }

    if ((p_dev->rc_volume == volume) || p_dev->rc_state != BTRC_CONNECTION_STATE_CONNECTED) {
      continue;
    }

    if ((p_dev->rc_features & BTA_AV_FEAT_RCTG) == 0) {
      status = BT_STATUS_NOT_READY;
      continue;
    }

    if (!(p_dev->rc_features & BTA_AV_FEAT_ADV_CTRL)) {
      continue;
    }

    log::verbose("Peer supports absolute volume. newVolume: {}", volume);

    tAVRC_COMMAND avrc_cmd = {.volume = {.pdu = AVRC_PDU_SET_ABSOLUTE_VOLUME,
                                         .status = AVRC_STS_NO_ERROR,
                                         .opcode = AVRC_OP_VENDOR,
                                         .volume = volume}};

    BT_HDR* p_msg = NULL;
    if (AVRC_BldCommand(&avrc_cmd, &p_msg) != AVRC_STS_NO_ERROR) {
      log::error("failed to build absolute volume command. status: 0x{:02x}", status);
      status = BT_STATUS_FAIL;
      continue;
    }

    rc_transaction_context_t context = {
            .rc_addr = p_dev->rc_addr,
            .label = MAX_LABEL,
            .opcode = AVRC_OP_VENDOR,
            .command = {.vendor = {AVRC_PDU_SET_ABSOLUTE_VOLUME, AVRC_EVT_INVALID}}};
    rc_transaction_t* p_transaction = NULL;
    bt_status_t tran_status = get_transaction(p_dev, context, &p_transaction);

    if (tran_status != BT_STATUS_SUCCESS || !p_transaction) {
      osi_free_and_reset((void**)&p_msg);
      log::error("failed to get label, pdu_id={}, status=0x{:02x}", dump_rc_pdu(avrc_cmd.pdu),
                 tran_status);
      status = BT_STATUS_FAIL;
      continue;
    }

    log::verbose("msgreq being sent out with label: {}", p_transaction->label);
    BTA_AvMetaCmd(p_dev->rc_handle, p_transaction->label, AVRC_CMD_CTRL, p_msg);
    status = BT_STATUS_SUCCESS;
    start_transaction_timer(p_dev, p_transaction->label, BTIF_RC_TIMEOUT_MS);
  }
  return (bt_status_t)status;
}

/***************************************************************************
 *
 * Function         register_volumechange
 *
 * Description     Register for volume change notification from remote side.
 *
 * Returns          void
 *
 **************************************************************************/

static void register_volumechange(btif_rc_device_cb_t* p_dev) {
  if (p_dev == nullptr) {
    log::error("device was null");
    return;
  }

  tAVRC_COMMAND avrc_cmd = {0};
  BT_HDR* p_msg = NULL;
  tAVRC_STS BldResp = AVRC_STS_BAD_CMD;

  rc_transaction_t* p_transaction = NULL;
  rc_transaction_context_t context = {
          .rc_addr = p_dev->rc_addr,
          .label = MAX_LABEL,
          .opcode = AVRC_OP_VENDOR,
          .command = {.vendor = {AVRC_PDU_REGISTER_NOTIFICATION, AVRC_EVT_VOLUME_CHANGE}}};
  bt_status_t status = BT_STATUS_NOT_READY;
  if (MAX_LABEL == p_dev->rc_vol_label) {
    status = get_transaction(p_dev, context, &p_transaction);
  } else {
    p_transaction = get_transaction_by_lbl(p_dev, p_dev->rc_vol_label);
    if (NULL != p_transaction) {
      log::verbose("already in progress for label: {}", p_dev->rc_vol_label);
      return;
    }
    status = get_transaction(p_dev, context, &p_transaction);
  }

  if (BT_STATUS_SUCCESS == status && NULL != p_transaction) {
    p_dev->rc_vol_label = p_transaction->label;
  } else {
    log::error("failed to get a transaction label");
    return;
  }

  log::verbose("label: {}", p_dev->rc_vol_label);

  avrc_cmd.cmd.opcode = 0x00;
  avrc_cmd.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
  avrc_cmd.reg_notif.event_id = AVRC_EVT_VOLUME_CHANGE;
  avrc_cmd.reg_notif.status = AVRC_STS_NO_ERROR;
  avrc_cmd.reg_notif.param = 0;

  BldResp = AVRC_BldCommand(&avrc_cmd, &p_msg);
  if (AVRC_STS_NO_ERROR == BldResp && p_msg) {
    BTA_AvMetaCmd(p_dev->rc_handle, p_transaction->label, AVRC_CMD_NOTIF, p_msg);
    log::verbose("BTA_AvMetaCmd called");
  } else {
    log::error("failed to build command: {}", BldResp);
  }
}

/***************************************************************************
 *
 * Function         handle_rc_metamsg_rsp
 *
 * Description      Handle RC metamessage response
 *
 * Returns          void
 *
 **************************************************************************/
static void handle_rc_metamsg_rsp(tBTA_AV_META_MSG* pmeta_msg, btif_rc_device_cb_t* p_dev) {
  tAVRC_RESPONSE avrc_response = {0};
  uint8_t scratch_buf[512] = {0};
  tAVRC_STS status = BT_STATUS_UNSUPPORTED;

  log::verbose("");

  if (AVRC_OP_VENDOR == pmeta_msg->p_msg->hdr.opcode &&
      (AVRC_RSP_CHANGED == pmeta_msg->code || AVRC_RSP_INTERIM == pmeta_msg->code ||
       AVRC_RSP_ACCEPT == pmeta_msg->code || AVRC_RSP_REJ == pmeta_msg->code ||
       AVRC_RSP_NOT_IMPL == pmeta_msg->code)) {
    status = AVRC_ParsResponse(pmeta_msg->p_msg, &avrc_response, scratch_buf, sizeof(scratch_buf));
    log::verbose("code:{}, event ID: {}, PDU: {:x}, parsing status: {}, label: {}", pmeta_msg->code,
                 avrc_response.reg_notif.event_id, avrc_response.reg_notif.pdu, status,
                 pmeta_msg->label);

    if (status != AVRC_STS_NO_ERROR) {
      if (AVRC_PDU_REGISTER_NOTIFICATION == avrc_response.rsp.pdu &&
          AVRC_EVT_VOLUME_CHANGE == avrc_response.reg_notif.event_id &&
          p_dev->rc_vol_label == pmeta_msg->label) {
        p_dev->rc_vol_label = MAX_LABEL;
        release_transaction(p_dev, p_dev->rc_vol_label);
      } else if (AVRC_PDU_SET_ABSOLUTE_VOLUME == avrc_response.rsp.pdu) {
        release_transaction(p_dev, pmeta_msg->label);
      }
      return;
    }

    if (AVRC_PDU_REGISTER_NOTIFICATION == avrc_response.rsp.pdu &&
        AVRC_EVT_VOLUME_CHANGE == avrc_response.reg_notif.event_id &&
        p_dev->rc_vol_label != pmeta_msg->label) {
      // Just discard the message, if the device sends back with an incorrect
      // label
      log::verbose("Discarding register notification in rsp.code: {} and label: {}",
                   pmeta_msg->code, pmeta_msg->label);
      return;
    }

    if (AVRC_PDU_REGISTER_NOTIFICATION == avrc_response.rsp.pdu &&
        AVRC_EVT_VOLUME_CHANGE == avrc_response.reg_notif.event_id &&
        (AVRC_RSP_REJ == pmeta_msg->code || AVRC_RSP_NOT_IMPL == pmeta_msg->code)) {
      log::verbose("remove AbsoluteVolume feature flag.");
      p_dev->rc_features &= ~BTA_AV_FEAT_ADV_CTRL;
      handle_rc_features(p_dev);
      return;
    }
  } else {
    log::verbose(
            "Received vendor dependent in adv ctrl rsp. code: {} len: {}. Not "
            "processing it.",
            pmeta_msg->code, pmeta_msg->len);
    return;
  }

  if (AVRC_PDU_REGISTER_NOTIFICATION == avrc_response.rsp.pdu &&
      AVRC_EVT_VOLUME_CHANGE == avrc_response.reg_notif.event_id &&
      AVRC_RSP_CHANGED == pmeta_msg->code) {
    /* re-register for volume change notification */
    // Do not re-register for rejected case, as it might get into endless loop
    register_volumechange(p_dev);
  } else if (AVRC_PDU_SET_ABSOLUTE_VOLUME == avrc_response.rsp.pdu) {
    /* free up the label here */
    release_transaction(p_dev, pmeta_msg->label);
  }

  log::verbose("Passing received metamsg response to app. pdu: {}", dump_rc_pdu(avrc_response.pdu));
  btif_rc_upstreams_rsp_evt((uint16_t)avrc_response.rsp.pdu, &avrc_response, pmeta_msg->code,
                            pmeta_msg->label, p_dev);
}

/***************************************************************************
 *
 * Function         iterate_supported_event_list_for_interim_rsp
 *
 * Description      iterator callback function to match the event and handle
 *                  timer cleanup
 * Returns          true to continue iterating, false to stop
 *
 **************************************************************************/
static bool iterate_supported_event_list_for_interim_rsp(void* data, void* cb_data) {
  uint8_t* p_event_id;
  btif_rc_supported_event_t* p_event = (btif_rc_supported_event_t*)data;

  p_event_id = (uint8_t*)cb_data;

  if (p_event->event_id == *p_event_id) {
    p_event->status = eINTERIM;
    return false;
  }
  return true;
}

/***************************************************************************
 *
 * Function         rc_notification_interim_timeout
 *
 * Description      Interim response timeout handler.
 *                  Runs the iterator to check and clear the timed out event.
 *                  Proceeds to register for the unregistered events.
 * Returns          None
 *
 **************************************************************************/
static void rc_notification_interim_timeout(btif_rc_device_cb_t* p_dev, uint8_t event_id) {
  /* Device disconnections clear the event list but can't free the timer */
  if (p_dev == NULL || p_dev->rc_supported_event_list == NULL) {
    log::warn("timeout for null device or event list");
    return;
  }

  // Remove the timed out event from the supported events list
  list_node_t* node = list_begin(p_dev->rc_supported_event_list);
  while (node != NULL) {
    btif_rc_supported_event_t* p_event = (btif_rc_supported_event_t*)list_node(node);
    if (p_event != nullptr && p_event->event_id == event_id) {
      list_remove(p_dev->rc_supported_event_list, p_event);
      break;
    }
    node = list_next(node);
  }

  /* Timeout happened for interim response for the registered event,
   * check if there are any pending for registration
   */
  node = list_begin(p_dev->rc_supported_event_list);
  while (node != NULL) {
    btif_rc_supported_event_t* p_event;

    p_event = (btif_rc_supported_event_t*)list_node(node);
    if ((p_event != NULL) && (p_event->status == eNOT_REGISTERED)) {
      register_for_event_notification(p_event, p_dev);
      break;
    }
    node = list_next(node);
  }
  /* Todo. Need to initiate application settings query if this
   * is the last event registration.
   */
}

/***************************************************************************
 *
 * Function         register_for_event_notification
 *
 * Description      Helper function registering notification events
 *                  sets an interim response timeout to handle if the remote
 *                  does not respond.
 * Returns          None
 *
 **************************************************************************/
static void register_for_event_notification(btif_rc_supported_event_t* p_event,
                                            btif_rc_device_cb_t* p_dev) {
  // interval is only valid for AVRC_EVT_PLAY_POS_CHANGED
  uint32_t interval_in_seconds = 0;
  if (p_event->event_id == AVRC_EVT_PLAY_POS_CHANGED) {
    interval_in_seconds = 2;
  }
  bt_status_t status = register_notification_cmd(p_event->event_id, interval_in_seconds, p_dev);
  if (status != BT_STATUS_SUCCESS) {
    log::error("failed, status={}", status);
    return;
  }

  p_event->status = eREGISTERED;
}

/***************************************************************************
 *
 * Function         build_and_send_vendor_cmd
 *
 * Description      Send a command to a device on the browsing channel
 *
 * Parameters       avrc_cmd: The command you're sending
 *                  p_dev: Device control block
 *
 * Returns          BT_STATUS_SUCCESS if command is issued successfully
 *                  otherwise BT_STATUS_FAIL
 *
 **************************************************************************/
static bt_status_t build_and_send_vendor_cmd(tAVRC_COMMAND* avrc_cmd, tBTA_AV_CODE cmd_code,
                                             btif_rc_device_cb_t* p_dev) {
  rc_transaction_t* p_transaction = NULL;
  rc_transaction_context_t context = {.rc_addr = p_dev->rc_addr,
                                      .label = MAX_LABEL,
                                      .opcode = AVRC_OP_VENDOR,
                                      .command = {.vendor = {avrc_cmd->pdu, AVRC_EVT_INVALID}}};

  // Set the event ID in the context if this is a notification registration
  if (avrc_cmd->pdu == AVRC_PDU_REGISTER_NOTIFICATION) {
    context.command.vendor.event_id = avrc_cmd->reg_notif.event_id;
  }

  bt_status_t tran_status = get_transaction(p_dev, context, &p_transaction);
  if (BT_STATUS_SUCCESS != tran_status || p_transaction == NULL) {
    log::error("failed to get label, pdu_id={}, status=0x{:02x}", dump_rc_pdu(avrc_cmd->pdu),
               tran_status);
    return BT_STATUS_FAIL;
  }

  BT_HDR* p_msg = NULL;
  tAVRC_STS status = AVRC_BldCommand(avrc_cmd, &p_msg);
  if (status == AVRC_STS_NO_ERROR && p_msg != NULL) {
    uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
    log::verbose("{} msgreq being sent out with label: {}", dump_rc_pdu(avrc_cmd->pdu),
                 p_transaction->label);
    BTA_AvVendorCmd(p_dev->rc_handle, p_transaction->label, cmd_code, data_start, p_msg->len);
    status = BT_STATUS_SUCCESS;
    start_transaction_timer(p_dev, p_transaction->label, BTIF_RC_TIMEOUT_MS);
  } else {
    log::error("failed to build command. status: 0x{:02x}", status);
    release_transaction(p_dev, p_transaction->label);
  }
  osi_free(p_msg);
  return (bt_status_t)status;
}

/***************************************************************************
 *
 * Function         build_and_send_browsing_cmd
 *
 * Description      Send a command to a device on the browsing channel
 *
 * Parameters       avrc_cmd: The command you're sending
 *                  p_dev: Device control block
 *
 * Returns          BT_STATUS_SUCCESS if command is issued successfully
 *                  otherwise BT_STATUS_FAIL
 *
 **************************************************************************/
static bt_status_t build_and_send_browsing_cmd(tAVRC_COMMAND* avrc_cmd,
                                               btif_rc_device_cb_t* p_dev) {
  rc_transaction_t* p_transaction = NULL;
  rc_transaction_context_t context = {.rc_addr = p_dev->rc_addr,
                                      .label = MAX_LABEL,
                                      .opcode = AVRC_OP_BROWSE,
                                      .command = {.browse = {avrc_cmd->pdu}}};

  bt_status_t tran_status = get_transaction(p_dev, context, &p_transaction);
  if (tran_status != BT_STATUS_SUCCESS || p_transaction == NULL) {
    log::error("failed to get label, pdu_id={}, status=0x{:02x}", dump_rc_pdu(avrc_cmd->pdu),
               tran_status);
    return BT_STATUS_FAIL;
  }

  BT_HDR* p_msg = NULL;
  tAVRC_STS status = AVRC_BldCommand(avrc_cmd, &p_msg);
  if (status != AVRC_STS_NO_ERROR) {
    log::error("failed to build command status {}", status);
    release_transaction(p_dev, p_transaction->label);
    return BT_STATUS_FAIL;
  }

  log::verbose("Send pdu_id={}, label={}", dump_rc_pdu(avrc_cmd->pdu), p_transaction->label);
  BTA_AvMetaCmd(p_dev->rc_handle, p_transaction->label, AVRC_CMD_CTRL, p_msg);
  start_transaction_timer(p_dev, p_transaction->label, BTIF_RC_TIMEOUT_MS);
  return BT_STATUS_SUCCESS;
}

/***************************************************************************
 *
 * Function         handle_get_capability_response
 *
 * Description      Handles the get_cap_response to populate company id info
 *                  and query the supported events.
 *                  Initiates Notification registration for events supported
 * Returns          None
 *
 **************************************************************************/
static void handle_get_capability_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_GET_CAPS_RSP* p_rsp) {
  int xx = 0;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  /* Todo: Do we need to retry on command timeout */
  if (p_rsp->status != AVRC_STS_NO_ERROR) {
    log::error("Error capability response: 0x{:02X}", p_rsp->status);
    return;
  }

  if (p_rsp->capability_id == AVRC_CAP_EVENTS_SUPPORTED) {
    btif_rc_supported_event_t* p_event;

    /* Todo: Check if list can be active when we hit here */
    p_dev->rc_supported_event_list = list_new(osi_free);
    for (xx = 0; xx < p_rsp->count; xx++) {
      /* Skip registering for Play position change notification */
      if ((p_rsp->param.event_id[xx] == AVRC_EVT_PLAY_STATUS_CHANGE) ||
          (p_rsp->param.event_id[xx] == AVRC_EVT_TRACK_CHANGE) ||
          (p_rsp->param.event_id[xx] == AVRC_EVT_PLAY_POS_CHANGED) ||
          (p_rsp->param.event_id[xx] == AVRC_EVT_APP_SETTING_CHANGE) ||
          (p_rsp->param.event_id[xx] == AVRC_EVT_NOW_PLAYING_CHANGE) ||
          (p_rsp->param.event_id[xx] == AVRC_EVT_ADDR_PLAYER_CHANGE) ||
          (p_rsp->param.event_id[xx] == AVRC_EVT_UIDS_CHANGE) ||
          (p_rsp->param.event_id[xx] == AVRC_EVT_AVAL_PLAYERS_CHANGE)) {
        p_event = (btif_rc_supported_event_t*)osi_malloc(sizeof(btif_rc_supported_event_t));
        p_event->event_id = p_rsp->param.event_id[xx];
        p_event->status = eNOT_REGISTERED;
        list_append(p_dev->rc_supported_event_list, p_event);
      }
    }

    // On occasion a remote device can intermittently send a poorly configured
    // packet with 0 capabilities. This check ensures the stack does not crash.
    // Typically the remote device will send a proper packet in the future and
    // continue operation.
    if (list_is_empty(p_dev->rc_supported_event_list)) {
      return;
    }

    p_event = (btif_rc_supported_event_t*)list_front(p_dev->rc_supported_event_list);
    if (p_event != NULL) {
      register_for_event_notification(p_event, p_dev);
    }
  } else if (p_rsp->capability_id == AVRC_CAP_COMPANY_ID) {
    getcapabilities_cmd(AVRC_CAP_EVENTS_SUPPORTED, p_dev);
    log::verbose("AVRC_CAP_COMPANY_ID:");
    for (xx = 0; xx < p_rsp->count; xx++) {
      log::verbose("company_id: {}", p_rsp->param.company_id[xx]);
    }
  }
}

static bool rc_is_track_id_valid(tAVRC_UID uid) {
  tAVRC_UID invalid_uid = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  if (memcmp(uid, invalid_uid, sizeof(tAVRC_UID)) == 0) {
    return false;
  } else {
    return true;
  }
}

/***************************************************************************
 *
 * Function         handle_notification_response
 *
 * Description      Main handler for notification responses to registered events
 *                  1. Register for unregistered event(in interim response path)
 *                  2. After registering for all supported events, start
 *                     retrieving application settings and values
 *                  3. Reregister for events on getting changed response
 *                  4. Run play status timer for getting position when the
 *                     status changes to playing
 *                  5. Get the Media details when the track change happens
 *                     or track change interim response is received with
 *                     valid track id
 *                  6. HAL callback for play status change and application
 *                     setting change
 * Returns          None
 *
 **************************************************************************/
static void handle_notification_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_REG_NOTIF_RSP* p_rsp) {
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  if (btif_av_src_sink_coexist_enabled() && p_rsp->event_id == AVRC_EVT_VOLUME_CHANGE) {
    log::error("legacy TG don't handle absolute volume change. leave it to new avrcp");
    return;
  }

  const uint32_t* attr_list = get_requested_attributes_list(p_dev);
  const uint8_t attr_list_size = get_requested_attributes_list_size(p_dev);

  if (pmeta_msg->code == AVRC_RSP_INTERIM) {
    btif_rc_supported_event_t* p_event;
    list_node_t* node;

    log::verbose("Interim response: 0x{:2X}", p_rsp->event_id);
    switch (p_rsp->event_id) {
      case AVRC_EVT_PLAY_STATUS_CHANGE:
        get_play_status_cmd(p_dev);
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->play_status_changed_cb,
                                        p_dev->rc_addr,
                                        (btrc_play_status_t)p_rsp->param.play_status));
        break;

      case AVRC_EVT_TRACK_CHANGE:
        if (rc_is_track_id_valid(p_rsp->param.track) != true) {
          break;
        } else {
          uint8_t* p_data = p_rsp->param.track;
          BE_STREAM_TO_UINT64(p_dev->rc_playing_uid, p_data);
          get_play_status_cmd(p_dev);
          get_metadata_attribute_cmd(attr_list_size, attr_list, p_dev);
        }
        break;

      case AVRC_EVT_APP_SETTING_CHANGE:
        break;

      case AVRC_EVT_NOW_PLAYING_CHANGE:
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->now_playing_contents_changed_cb,
                                        p_dev->rc_addr));
        break;

      case AVRC_EVT_AVAL_PLAYERS_CHANGE:
        log::verbose("AVRC_EVT_AVAL_PLAYERS_CHANGE");
        do_in_jni_thread(
                base::BindOnce(bt_rc_ctrl_callbacks->available_player_changed_cb, p_dev->rc_addr));
        break;

      case AVRC_EVT_ADDR_PLAYER_CHANGE:
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->addressed_player_changed_cb,
                                        p_dev->rc_addr, p_rsp->param.addr_player.player_id));
        break;

      case AVRC_EVT_PLAY_POS_CHANGED:
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->play_position_changed_cb,
                                        p_dev->rc_addr, 0, p_rsp->param.play_pos));

        break;
      case AVRC_EVT_UIDS_CHANGE:
        break;

      case AVRC_EVT_TRACK_REACHED_END:
      case AVRC_EVT_TRACK_REACHED_START:
      case AVRC_EVT_BATTERY_STATUS_CHANGE:
      case AVRC_EVT_SYSTEM_STATUS_CHANGE:
      default:
        log::error("Unhandled interim response: 0x{:2X}", p_rsp->event_id);
        return;
    }

    list_foreach(p_dev->rc_supported_event_list, iterate_supported_event_list_for_interim_rsp,
                 &p_rsp->event_id);

    node = list_begin(p_dev->rc_supported_event_list);

    while (node != NULL) {
      p_event = (btif_rc_supported_event_t*)list_node(node);
      if ((p_event != NULL) && (p_event->status == eNOT_REGISTERED)) {
        register_for_event_notification(p_event, p_dev);
        break;
      }
      node = list_next(node);
      p_event = NULL;
    }
    /* Registered for all events, we can request application settings */
    if (p_event == NULL && !p_dev->rc_app_settings.query_started) {
      /* we need to do this only if remote TG supports
       * player application settings
       */
      p_dev->rc_app_settings.query_started = true;
      if (p_dev->rc_features & BTA_AV_FEAT_APP_SETTING) {
        list_player_app_setting_attrib_cmd(p_dev);
      } else {
        log::verbose("App setting not supported, complete procedure");
        rc_ctrl_procedure_complete(p_dev);
      }
    }
  } else if (pmeta_msg->code == AVRC_RSP_CHANGED) {
    btif_rc_supported_event_t* p_event;
    list_node_t* node;

    log::verbose("Notification completed: 0x{:2X}", p_rsp->event_id);

    node = list_begin(p_dev->rc_supported_event_list);

    while (node != NULL) {
      p_event = (btif_rc_supported_event_t*)list_node(node);
      if (p_event != NULL && p_event->event_id == p_rsp->event_id) {
        p_event->status = eNOT_REGISTERED;
        register_for_event_notification(p_event, p_dev);
        break;
      }
      node = list_next(node);
    }

    switch (p_rsp->event_id) {
      case AVRC_EVT_PLAY_STATUS_CHANGE:
        /* Start timer to get play status periodically
         * if the play state is playing.
         */
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->play_status_changed_cb,
                                        p_dev->rc_addr,
                                        (btrc_play_status_t)p_rsp->param.play_status));

        break;

      case AVRC_EVT_TRACK_CHANGE:
        if (rc_is_track_id_valid(p_rsp->param.track) != true) {
          break;
        }
        get_metadata_attribute_cmd(attr_list_size, attr_list, p_dev);
        break;

      case AVRC_EVT_APP_SETTING_CHANGE: {
        btrc_player_settings_t app_settings;
        uint16_t xx;

        app_settings.num_attr = p_rsp->param.player_setting.num_attr;
        for (xx = 0; xx < app_settings.num_attr; xx++) {
          app_settings.attr_ids[xx] = p_rsp->param.player_setting.attr_id[xx];
          app_settings.attr_values[xx] = p_rsp->param.player_setting.attr_value[xx];
        }
        do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->playerapplicationsetting_changed_cb,
                                        p_dev->rc_addr, app_settings));
      } break;

      case AVRC_EVT_NOW_PLAYING_CHANGE:
        break;

      case AVRC_EVT_AVAL_PLAYERS_CHANGE:
        break;

      case AVRC_EVT_ADDR_PLAYER_CHANGE:
        break;

      case AVRC_EVT_PLAY_POS_CHANGED:
        // handle on interim
        break;

      case AVRC_EVT_UIDS_CHANGE:
        break;

      case AVRC_EVT_TRACK_REACHED_END:
      case AVRC_EVT_TRACK_REACHED_START:
      case AVRC_EVT_BATTERY_STATUS_CHANGE:
      case AVRC_EVT_SYSTEM_STATUS_CHANGE:
      default:
        log::error("Unhandled completion response: 0x{:2X}", p_rsp->event_id);
        return;
    }
  }
}

/***************************************************************************
 *
 * Function         handle_app_attr_response
 *
 * Description      handles the the application attributes response and
 *                  initiates procedure to fetch the attribute values
 * Returns          None
 *
 **************************************************************************/
static void handle_app_attr_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_LIST_APP_ATTR_RSP* p_rsp) {
  uint8_t xx;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL || p_rsp->status != AVRC_STS_NO_ERROR) {
    log::error("Error getting Player application settings: 0x{:2X}", p_rsp->status);
    rc_ctrl_procedure_complete(p_dev);
    return;
  }
  p_dev->rc_app_settings.num_attrs = 0;
  p_dev->rc_app_settings.num_ext_attrs = 0;

  for (xx = 0; xx < p_rsp->num_attr; xx++) {
    uint8_t st_index;

    if (p_rsp->attrs[xx] > AVRC_PLAYER_SETTING_LOW_MENU_EXT) {
      st_index = p_dev->rc_app_settings.num_ext_attrs;
      p_dev->rc_app_settings.ext_attrs[st_index].attr_id = p_rsp->attrs[xx];
      p_dev->rc_app_settings.num_ext_attrs++;
    } else {
      st_index = p_dev->rc_app_settings.num_attrs;
      p_dev->rc_app_settings.attrs[st_index].attr_id = p_rsp->attrs[xx];
      p_dev->rc_app_settings.num_attrs++;
    }
  }
  p_dev->rc_app_settings.attr_index = 0;
  p_dev->rc_app_settings.ext_attr_index = 0;
  p_dev->rc_app_settings.ext_val_index = 0;
  if (p_rsp->num_attr) {
    list_player_app_setting_value_cmd(p_dev->rc_app_settings.attrs[0].attr_id, p_dev);
  } else {
    log::error("No Player application settings found");
  }
}

/***************************************************************************
 *
 * Function         handle_app_val_response
 *
 * Description      handles the the attributes value response and if extended
 *                  menu is available, it initiates query for the attribute
 *                  text. If not, it initiates procedure to get the current
 *                  attribute values and calls the HAL callback for provding
 *                  application settings information.
 * Returns          None
 *
 **************************************************************************/
static void handle_app_val_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_LIST_APP_VALUES_RSP* p_rsp) {
  uint8_t xx, attr_index;
  uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];
  btif_rc_player_app_settings_t* p_app_settings;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  /* Todo: Do we need to retry on command timeout */
  if (p_dev == NULL || p_rsp->status != AVRC_STS_NO_ERROR) {
    log::error("Error fetching attribute values: 0x{:02X}", p_rsp->status);
    return;
  }

  p_app_settings = &p_dev->rc_app_settings;

  if (p_app_settings->attr_index < p_app_settings->num_attrs) {
    attr_index = p_app_settings->attr_index;
    p_app_settings->attrs[attr_index].num_val = p_rsp->num_val;
    for (xx = 0; xx < p_rsp->num_val; xx++) {
      p_app_settings->attrs[attr_index].attr_val[xx] = p_rsp->vals[xx];
    }
    attr_index++;
    p_app_settings->attr_index++;
    if (attr_index < p_app_settings->num_attrs) {
      list_player_app_setting_value_cmd(p_app_settings->attrs[p_app_settings->attr_index].attr_id,
                                        p_dev);
    } else if (p_app_settings->ext_attr_index < p_app_settings->num_ext_attrs) {
      attr_index = 0;
      p_app_settings->ext_attr_index = 0;
      list_player_app_setting_value_cmd(p_app_settings->ext_attrs[attr_index].attr_id, p_dev);
    } else {
      for (xx = 0; xx < p_app_settings->num_attrs; xx++) {
        attrs[xx] = p_app_settings->attrs[xx].attr_id;
      }
      get_player_app_setting_cmd(p_app_settings->num_attrs, attrs, p_dev);
      do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->playerapplicationsetting_cb,
                                      p_dev->rc_addr, p_app_settings->num_attrs,
                                      p_app_settings->attrs, 0, nullptr));
    }
  } else if (p_app_settings->ext_attr_index < p_app_settings->num_ext_attrs) {
    attr_index = p_app_settings->ext_attr_index;
    p_app_settings->ext_attrs[attr_index].num_val = p_rsp->num_val;
    for (xx = 0; xx < p_rsp->num_val; xx++) {
      p_app_settings->ext_attrs[attr_index].ext_attr_val[xx].val = p_rsp->vals[xx];
    }
    attr_index++;
    p_app_settings->ext_attr_index++;
    if (attr_index < p_app_settings->num_ext_attrs) {
      list_player_app_setting_value_cmd(
              p_app_settings->ext_attrs[p_app_settings->ext_attr_index].attr_id, p_dev);
    } else {
      uint8_t attr[AVRC_MAX_APP_ATTR_SIZE];

      for (uint8_t xx = 0; xx < p_app_settings->num_ext_attrs; xx++) {
        attr[xx] = p_app_settings->ext_attrs[xx].attr_id;
      }
      get_player_app_setting_attr_text_cmd(attr, p_app_settings->num_ext_attrs, p_dev);
    }
  }
}

/***************************************************************************
 *
 * Function         handle_app_cur_val_response
 *
 * Description      handles the the get attributes value response.
 *
 * Returns          None
 *
 **************************************************************************/
static void handle_app_cur_val_response(tBTA_AV_META_MSG* pmeta_msg,
                                        tAVRC_GET_CUR_APP_VALUE_RSP* p_rsp) {
  btrc_player_settings_t app_settings;
  uint16_t xx;
  btif_rc_device_cb_t* p_dev = NULL;

  /* Todo: Do we need to retry on command timeout */
  if (p_rsp->status != AVRC_STS_NO_ERROR) {
    log::error("Error fetching current settings: 0x{:02X}", p_rsp->status);
    return;
  }
  p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);
  if (p_dev == NULL) {
    log::error("Error in getting Device Address");
    osi_free_and_reset((void**)&p_rsp->p_vals);
    return;
  }

  app_settings.num_attr = p_rsp->num_val;

  if (app_settings.num_attr > BTRC_MAX_APP_SETTINGS) {
    app_settings.num_attr = BTRC_MAX_APP_SETTINGS;
  }

  for (xx = 0; xx < app_settings.num_attr; xx++) {
    app_settings.attr_ids[xx] = p_rsp->p_vals[xx].attr_id;
    app_settings.attr_values[xx] = p_rsp->p_vals[xx].attr_val;
  }

  do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->playerapplicationsetting_changed_cb,
                                  p_dev->rc_addr, app_settings));
  /* Application settings are fetched only once for initial values
   * initiate anything that follows after RC procedure.
   * Defer it if browsing is supported till players query
   */
  rc_ctrl_procedure_complete(p_dev);
  osi_free_and_reset((void**)&p_rsp->p_vals);
}

/***************************************************************************
 *
 * Function         handle_app_attr_txt_response
 *
 * Description      handles the the get attributes text response, if fails
 *                  calls HAL callback with just normal settings and initiates
 *                  query for current settings else initiates query for value
 *                  text
 * Returns          None
 *
 **************************************************************************/
static void handle_app_attr_txt_response(tBTA_AV_META_MSG* pmeta_msg,
                                         tAVRC_GET_APP_ATTR_TXT_RSP* p_rsp) {
  uint8_t xx;
  uint8_t vals[AVRC_MAX_APP_ATTR_SIZE];
  btif_rc_player_app_settings_t* p_app_settings;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  p_app_settings = &p_dev->rc_app_settings;

  /* Todo: Do we need to retry on command timeout */
  if (p_rsp->status != AVRC_STS_NO_ERROR) {
    uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];

    log::error("Error fetching attribute text: 0x{:02X}", p_rsp->status);
    /* Not able to fetch Text for extended Menu, skip the process
     * and cleanup used memory. Proceed to get the current settings
     * for standard attributes.
     */
    p_app_settings->num_ext_attrs = 0;
    for (xx = 0; xx < p_app_settings->ext_attr_index && xx < AVRC_MAX_APP_ATTR_SIZE; xx++) {
      osi_free_and_reset((void**)&p_app_settings->ext_attrs[xx].p_str);
    }
    p_app_settings->ext_attr_index = 0;

    for (xx = 0; xx < p_app_settings->num_attrs && xx < AVRC_MAX_APP_ATTR_SIZE; xx++) {
      attrs[xx] = p_app_settings->attrs[xx].attr_id;
    }

    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->playerapplicationsetting_cb,
                                    p_dev->rc_addr, p_app_settings->num_attrs,
                                    p_app_settings->attrs, 0, nullptr));
    get_player_app_setting_cmd(xx, attrs, p_dev);

    return;
  }

  for (xx = 0; xx < p_rsp->num_attr; xx++) {
    uint8_t x;
    for (x = 0; x < p_app_settings->num_ext_attrs && x < AVRC_MAX_APP_ATTR_SIZE; x++) {
      if (p_app_settings->ext_attrs[x].attr_id == p_rsp->p_attrs[xx].attr_id) {
        p_app_settings->ext_attrs[x].charset_id = p_rsp->p_attrs[xx].charset_id;
        p_app_settings->ext_attrs[x].str_len = p_rsp->p_attrs[xx].str_len;
        p_app_settings->ext_attrs[x].p_str = p_rsp->p_attrs[xx].p_str;
        break;
      }
    }
  }

  for (xx = 0; xx < p_app_settings->ext_attrs[0].num_val && xx < BTRC_MAX_APP_ATTR_SIZE; xx++) {
    vals[xx] = p_app_settings->ext_attrs[0].ext_attr_val[xx].val;
  }
  get_player_app_setting_value_text_cmd(vals, xx, p_dev);
}

/***************************************************************************
 *
 * Function         handle_app_attr_val_txt_response
 *
 * Description      handles the the get attributes value text response, if fails
 *                  calls HAL callback with just normal settings and initiates
 *                  query for current settings
 * Returns          None
 *
 **************************************************************************/
static void handle_app_attr_val_txt_response(tBTA_AV_META_MSG* pmeta_msg,
                                             tAVRC_GET_APP_ATTR_TXT_RSP* p_rsp) {
  uint8_t xx, attr_index;
  uint8_t vals[AVRC_MAX_APP_ATTR_SIZE];
  uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];
  btif_rc_player_app_settings_t* p_app_settings;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  p_app_settings = &p_dev->rc_app_settings;

  /* Todo: Do we need to retry on command timeout */
  if (p_rsp->status != AVRC_STS_NO_ERROR) {
    uint8_t attrs[AVRC_MAX_APP_ATTR_SIZE];

    log::error("Error fetching attribute value text: 0x{:02X}", p_rsp->status);

    /* Not able to fetch Text for extended Menu, skip the process
     * and cleanup used memory. Proceed to get the current settings
     * for standard attributes.
     */
    p_app_settings->num_ext_attrs = 0;
    for (xx = 0; xx < p_app_settings->ext_attr_index && xx < AVRC_MAX_APP_ATTR_SIZE; xx++) {
      int x;
      btrc_player_app_ext_attr_t* p_ext_attr = &p_app_settings->ext_attrs[xx];

      for (x = 0; x < p_ext_attr->num_val && x < BTRC_MAX_APP_ATTR_SIZE; x++) {
        osi_free_and_reset((void**)&p_ext_attr->ext_attr_val[x].p_str);
      }
      p_ext_attr->num_val = 0;
      osi_free_and_reset((void**)&p_app_settings->ext_attrs[xx].p_str);
    }
    p_app_settings->ext_attr_index = 0;

    for (xx = 0; xx < p_app_settings->num_attrs; xx++) {
      attrs[xx] = p_app_settings->attrs[xx].attr_id;
    }
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->playerapplicationsetting_cb,
                                    p_dev->rc_addr, p_app_settings->num_attrs,
                                    p_app_settings->attrs, 0, nullptr));

    get_player_app_setting_cmd(xx, attrs, p_dev);
    return;
  }

  if (p_app_settings->ext_val_index >= AVRC_MAX_APP_ATTR_SIZE) {
    log::error("ext_val_index is 0x{:02x}, overflow!", p_app_settings->ext_val_index);
    return;
  }

  for (xx = 0; xx < p_rsp->num_attr; xx++) {
    uint8_t x;
    btrc_player_app_ext_attr_t* p_ext_attr;
    p_ext_attr = &p_app_settings->ext_attrs[p_app_settings->ext_val_index];
    for (x = 0; x < p_rsp->num_attr && x < BTRC_MAX_APP_ATTR_SIZE; x++) {
      if (p_ext_attr->ext_attr_val[x].val == p_rsp->p_attrs[xx].attr_id) {
        p_ext_attr->ext_attr_val[x].charset_id = p_rsp->p_attrs[xx].charset_id;
        p_ext_attr->ext_attr_val[x].str_len = p_rsp->p_attrs[xx].str_len;
        p_ext_attr->ext_attr_val[x].p_str = p_rsp->p_attrs[xx].p_str;
        break;
      }
    }
  }
  p_app_settings->ext_val_index++;

  if (p_app_settings->ext_val_index < p_app_settings->num_ext_attrs) {
    attr_index = p_app_settings->ext_val_index;
    for (xx = 0; xx < p_app_settings->ext_attrs[attr_index].num_val; xx++) {
      vals[xx] = p_app_settings->ext_attrs[attr_index].ext_attr_val[xx].val;
    }
    get_player_app_setting_value_text_cmd(vals, xx, p_dev);
  } else {
    uint8_t x;

    for (xx = 0; xx < p_app_settings->num_attrs; xx++) {
      attrs[xx] = p_app_settings->attrs[xx].attr_id;
    }
    for (x = 0; x < p_app_settings->num_ext_attrs; x++) {
      attrs[xx + x] = p_app_settings->ext_attrs[x].attr_id;
    }
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->playerapplicationsetting_cb,
                                    p_dev->rc_addr, p_app_settings->num_attrs,
                                    p_app_settings->attrs, p_app_settings->num_ext_attrs,
                                    p_app_settings->ext_attrs));
    get_player_app_setting_cmd(xx + x, attrs, p_dev);

    /* Free the application settings information after sending to
     * application.
     */
    do_in_jni_thread(base::BindOnce(cleanup_app_attr_val_txt_response, p_app_settings));
    p_app_settings->num_attrs = 0;
  }
}

/***************************************************************************
 *
 * Function         cleanup_app_attr_val_txt_response
 *
 * Description      Frees the memory that was allocated for reporting player
 *                  application settings.
 * Returns          None
 **************************************************************************/
static void cleanup_app_attr_val_txt_response(btif_rc_player_app_settings_t* p_app_settings) {
  for (uint8_t xx = 0; xx < p_app_settings->ext_attr_index && xx < AVRC_MAX_APP_ATTR_SIZE; xx++) {
    int x;
    btrc_player_app_ext_attr_t* p_ext_attr = &p_app_settings->ext_attrs[xx];
    for (x = 0; x < p_ext_attr->num_val && x < BTRC_MAX_APP_ATTR_SIZE; x++) {
      osi_free_and_reset((void**)&p_ext_attr->ext_attr_val[x].p_str);
    }
    p_ext_attr->num_val = 0;
    osi_free_and_reset((void**)&p_app_settings->ext_attrs[xx].p_str);
  }
}

/***************************************************************************
 *
 * Function         handle_set_app_attr_val_response
 *
 * Description      handles the the set attributes value response, if fails
 *                  calls HAL callback to indicate the failure
 * Returns          None
 *
 **************************************************************************/
static void handle_set_app_attr_val_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_RSP* /*p_rsp*/) {
  uint8_t accepted = 0;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  /* For timeout pmeta_msg will be NULL, else we need to
   * check if this is accepted by TG
   */
  if (pmeta_msg && (pmeta_msg->code == AVRC_RSP_ACCEPT)) {
    accepted = 1;
  }
  do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->setplayerappsetting_rsp_cb, p_dev->rc_addr,
                                  accepted));
}

/***************************************************************************
 *
 * Function         handle_get_metadata_attr_response
 *
 * Description      handles the the element attributes response, calls
 *                  HAL callback to update track change information.
 * Returns          None
 *
 **************************************************************************/
static void handle_get_metadata_attr_response(tBTA_AV_META_MSG* pmeta_msg,
                                              tAVRC_GET_ATTRS_RSP* p_rsp) {
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_rsp->status == AVRC_STS_NO_ERROR) {
    size_t buf_size = p_rsp->num_attrs * sizeof(btrc_element_attr_val_t);
    btrc_element_attr_val_t* p_attr = (btrc_element_attr_val_t*)osi_calloc(buf_size);

    if (p_dev == NULL) {
      log::error("p_dev NULL");
      return;
    }

    for (int i = 0; i < p_rsp->num_attrs; i++) {
      p_attr[i].attr_id = p_rsp->p_attrs[i].attr_id;
      /* Todo. Length limit check to include null */
      if (p_rsp->p_attrs[i].name.str_len && p_rsp->p_attrs[i].name.p_str) {
        memcpy(p_attr[i].text, p_rsp->p_attrs[i].name.p_str, p_rsp->p_attrs[i].name.str_len);
        osi_free_and_reset((void**)&p_rsp->p_attrs[i].name.p_str);
      }
    }

    osi_free_and_reset((void**)&p_rsp->p_attrs);

    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->track_changed_cb, p_dev->rc_addr,
                                    p_rsp->num_attrs, p_attr));
    do_in_jni_thread(base::BindOnce(osi_free, p_attr));
  } else if (p_rsp->status == BTIF_RC_STS_TIMEOUT) {
    /* Retry for timeout case, this covers error handling
     * for continuation failure also.
     */
    const uint32_t* attr_list = get_requested_attributes_list(p_dev);
    const uint8_t attr_list_size = get_requested_attributes_list_size(p_dev);
    get_metadata_attribute_cmd(attr_list_size, attr_list, p_dev);
  } else {
    log::error("Error in get element attr procedure: {}", p_rsp->status);
  }
}

/***************************************************************************
 *
 * Function         handle_get_playstatus_response
 *
 * Description      handles the the play status response, calls
 *                  HAL callback to update play position.
 * Returns          None
 *
 **************************************************************************/
static void handle_get_playstatus_response(tBTA_AV_META_MSG* pmeta_msg,
                                           tAVRC_GET_PLAY_STATUS_RSP* p_rsp) {
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  if (p_rsp->status == AVRC_STS_NO_ERROR) {
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->play_status_changed_cb, p_dev->rc_addr,
                                    (btrc_play_status_t)p_rsp->play_status));
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->play_position_changed_cb, p_dev->rc_addr,
                                    p_rsp->song_len, p_rsp->song_pos));
  } else {
    log::error("Error in get play status procedure: {}", p_rsp->status);
  }
}

/***************************************************************************
 *
 * Function         handle_set_addressed_player_response
 *
 * Description      handles the the set addressed player response, calls
 *                  HAL callback
 * Returns          None
 *
 **************************************************************************/
static void handle_set_addressed_player_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_RSP* p_rsp) {
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  if (p_rsp->status == AVRC_STS_NO_ERROR) {
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->set_addressed_player_cb, p_dev->rc_addr,
                                    p_rsp->status));
  } else {
    log::error("Error in get play status procedure {}", p_rsp->status);
  }
}

/***************************************************************************
 *
 * Function         handle_get_folder_items_response
 *
 * Description      handles the the get folder items response, calls
 *                  HAL callback to send the folder items.
 * Returns          None
 *
 **************************************************************************/
static void handle_get_folder_items_response(tBTA_AV_META_MSG* pmeta_msg,
                                             tAVRC_GET_ITEMS_RSP* p_rsp) {
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_rsp->status == AVRC_STS_NO_ERROR) {
    /* Convert the internal folder listing into a response that can
     * be passed onto JNI via HAL_CBACK
     */
    uint8_t item_count = p_rsp->item_count;
    btrc_folder_items_t* btrc_items =
            (btrc_folder_items_t*)osi_malloc(sizeof(btrc_folder_items_t) * item_count);
    for (uint8_t i = 0; i < item_count; i++) {
      const tAVRC_ITEM* avrc_item = &(p_rsp->p_item_list[i]);
      btrc_folder_items_t* btrc_item = &(btrc_items[i]);
      log::verbose("folder item type {}", avrc_item->item_type);
      switch (avrc_item->item_type) {
        case AVRC_ITEM_MEDIA:
          log::verbose("setting type to {}", BTRC_ITEM_MEDIA);
          /* Allocate Space for Attributes */
          btrc_item->media.num_attrs = avrc_item->u.media.attr_count;
          btrc_item->media.p_attrs = (btrc_element_attr_val_t*)osi_malloc(
                  btrc_item->media.num_attrs * sizeof(btrc_element_attr_val_t));
          get_folder_item_type_media(avrc_item, btrc_item);
          break;

        case AVRC_ITEM_FOLDER:
          log::verbose("setting type to BTRC_ITEM_FOLDER");
          get_folder_item_type_folder(avrc_item, btrc_item);
          break;

        case AVRC_ITEM_PLAYER:
          log::verbose("setting type to BTRC_ITEM_PLAYER");
          get_folder_item_type_player(avrc_item, btrc_item);
          break;

        default:
          log::error("cannot understand folder item type {}", avrc_item->item_type);
      }
    }

    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->get_folder_items_cb, p_dev->rc_addr,
                                    BTRC_STS_NO_ERROR,
                                    /* We want to make the ownership explicit in native */
                                    btrc_items, item_count));

    if (item_count > 0) {
      if (btrc_items[0].item_type == AVRC_ITEM_PLAYER &&
          (p_dev->rc_features & BTA_AV_FEAT_APP_SETTING)) {
        list_player_app_setting_attrib_cmd(p_dev);
      }
    }
    /* Release the memory block for items and attributes allocated here.
     * Since the executor for do_in_jni_thread is a Single Thread Task Runner it
     * is okay to queue up the cleanup of btrc_items */
    do_in_jni_thread(base::BindOnce(cleanup_btrc_folder_items, btrc_items, item_count));

    log::verbose("get_folder_items_cb sent to JNI thread");
  } else {
    log::error("Error {}", p_rsp->status);
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->get_folder_items_cb, p_dev->rc_addr,
                                    (btrc_status_t)p_rsp->status, nullptr, 0));
  }
}
/***************************************************************************
 *
 * Function         cleanup_btrc_folder_items
 *
 * Description      Frees the memory that was allocated for a list of folder
 *                  items.
 * Returns          None
 **************************************************************************/
static void cleanup_btrc_folder_items(btrc_folder_items_t* btrc_items, uint8_t item_count) {
  for (uint8_t i = 0; i < item_count; i++) {
    btrc_folder_items_t* btrc_item = &(btrc_items[i]);
    switch (btrc_item->item_type) {
      case BTRC_ITEM_MEDIA:
        osi_free(btrc_item->media.p_attrs);
        break;
      case BTRC_ITEM_PLAYER:
      case BTRC_ITEM_FOLDER:
        /*Nothing to free*/
        break;
      default:
        log::warn("free unspecified type");
    }
  }
  osi_free(btrc_items);
}

/***************************************************************************
 *
 * Function         get_folder_item_type_media
 *
 * Description      Converts the AVRC representation of a folder item with
 *                  TYPE media to BTIF representation.
 * Returns          None
 *
 **************************************************************************/
static void get_folder_item_type_media(const tAVRC_ITEM* avrc_item,
                                       btrc_folder_items_t* btrc_item) {
  btrc_item->item_type = BTRC_ITEM_MEDIA;
  const tAVRC_ITEM_MEDIA* avrc_item_media = &(avrc_item->u.media);
  btrc_item_media_t* btrc_item_media = &(btrc_item->media);
  /* UID */
  memset(btrc_item_media->uid, 0, BTRC_UID_SIZE * sizeof(uint8_t));
  memcpy(btrc_item_media->uid, avrc_item_media->uid, sizeof(uint8_t) * BTRC_UID_SIZE);

  /* Audio/Video type */
  switch (avrc_item_media->type) {
    case AVRC_MEDIA_TYPE_AUDIO:
      btrc_item_media->type = BTRC_MEDIA_TYPE_AUDIO;
      break;
    case AVRC_MEDIA_TYPE_VIDEO:
      btrc_item_media->type = BTRC_MEDIA_TYPE_VIDEO;
      break;
  }

  /* Charset ID */
  btrc_item_media->charset_id = avrc_item_media->name.charset_id;

  /* Copy the name */
  log::verbose("max len {} str len {}", BTRC_MAX_ATTR_STR_LEN, avrc_item_media->name.str_len);
  memset(btrc_item_media->name, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
  memcpy(btrc_item_media->name, avrc_item_media->name.p_str,
         sizeof(uint8_t) * (avrc_item_media->name.str_len));

  /* Extract each attribute */
  for (int i = 0; i < avrc_item_media->attr_count; i++) {
    btrc_element_attr_val_t* btrc_attr_pair = &(btrc_item_media->p_attrs[i]);
    tAVRC_ATTR_ENTRY* avrc_attr_pair = &(avrc_item_media->p_attr_list[i]);

    log::verbose("media attr id 0x{:x}", avrc_attr_pair->attr_id);

    switch (avrc_attr_pair->attr_id) {
      case AVRC_MEDIA_ATTR_ID_TITLE:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_TITLE;
        break;
      case AVRC_MEDIA_ATTR_ID_ARTIST:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_ARTIST;
        break;
      case AVRC_MEDIA_ATTR_ID_ALBUM:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_ALBUM;
        break;
      case AVRC_MEDIA_ATTR_ID_TRACK_NUM:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_TRACK_NUM;
        break;
      case AVRC_MEDIA_ATTR_ID_NUM_TRACKS:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_NUM_TRACKS;
        break;
      case AVRC_MEDIA_ATTR_ID_GENRE:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_GENRE;
        break;
      case AVRC_MEDIA_ATTR_ID_PLAYING_TIME:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_PLAYING_TIME;
        break;
      case AVRC_MEDIA_ATTR_ID_COVER_ARTWORK_HANDLE:
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_COVER_ARTWORK_HANDLE;
        break;
      default:
        log::error("invalid media attr id: 0x{:x}", avrc_attr_pair->attr_id);
        btrc_attr_pair->attr_id = BTRC_MEDIA_ATTR_ID_INVALID;
    }

    memset(btrc_attr_pair->text, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
    memcpy(btrc_attr_pair->text, avrc_attr_pair->name.p_str, avrc_attr_pair->name.str_len);
  }
}

/***************************************************************************
 *
 * Function         get_folder_item_type_folder
 *
 * Description      Converts the AVRC representation of a folder item with
 *                  TYPE folder to BTIF representation.
 * Returns          None
 *
 **************************************************************************/
static void get_folder_item_type_folder(const tAVRC_ITEM* avrc_item,
                                        btrc_folder_items_t* btrc_item) {
  btrc_item->item_type = BTRC_ITEM_FOLDER;
  const tAVRC_ITEM_FOLDER* avrc_item_folder = &(avrc_item->u.folder);
  btrc_item_folder_t* btrc_item_folder = &(btrc_item->folder);
  /* Copy the UID */
  memset(btrc_item_folder->uid, 0, BTRC_UID_SIZE * sizeof(uint8_t));
  memcpy(btrc_item_folder->uid, avrc_item_folder->uid, sizeof(uint8_t) * BTRC_UID_SIZE);

  /* Copy the type */
  switch (avrc_item_folder->type) {
    case AVRC_FOLDER_TYPE_MIXED:
      btrc_item_folder->type = BTRC_FOLDER_TYPE_MIXED;
      break;
    case AVRC_FOLDER_TYPE_TITLES:
      btrc_item_folder->type = BTRC_FOLDER_TYPE_TITLES;
      break;
    case AVRC_FOLDER_TYPE_ALNUMS:
      btrc_item_folder->type = BTRC_FOLDER_TYPE_ALBUMS;
      break;
    case AVRC_FOLDER_TYPE_ARTISTS:
      btrc_item_folder->type = BTRC_FOLDER_TYPE_ARTISTS;
      break;
    case AVRC_FOLDER_TYPE_GENRES:
      btrc_item_folder->type = BTRC_FOLDER_TYPE_GENRES;
      break;
    case AVRC_FOLDER_TYPE_PLAYLISTS:
      btrc_item_folder->type = BTRC_FOLDER_TYPE_PLAYLISTS;
      break;
    case AVRC_FOLDER_TYPE_YEARS:
      btrc_item_folder->type = BTRC_FOLDER_TYPE_YEARS;
      break;
  }

  /* Copy if playable */
  btrc_item_folder->playable = avrc_item_folder->playable;

  /* Copy name */
  log::verbose("max len {} str len {}", BTRC_MAX_ATTR_STR_LEN, avrc_item_folder->name.str_len);
  memset(btrc_item_folder->name, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
  memcpy(btrc_item_folder->name, avrc_item_folder->name.p_str,
         avrc_item_folder->name.str_len * sizeof(uint8_t));

  /* Copy charset */
  btrc_item_folder->charset_id = avrc_item_folder->name.charset_id;
}

/***************************************************************************
 *
 * Function         get_folder_item_type_player
 *
 * Description      Converts the AVRC representation of a folder item with
 *                  TYPE player to BTIF representation.
 * Returns          None
 *
 **************************************************************************/
static void get_folder_item_type_player(const tAVRC_ITEM* avrc_item,
                                        btrc_folder_items_t* btrc_item) {
  btrc_item->item_type = BTRC_ITEM_PLAYER;
  const tAVRC_ITEM_PLAYER* avrc_item_player = &(avrc_item->u.player);
  btrc_item_player_t* btrc_item_player = &(btrc_item->player);
  /* Player ID */
  btrc_item_player->player_id = avrc_item_player->player_id;
  /* Major type */
  btrc_item_player->major_type = avrc_item_player->major_type;
  /* Sub type */
  btrc_item_player->sub_type = avrc_item_player->sub_type;
  /* Play status */
  btrc_item_player->play_status = avrc_item_player->play_status;
  /* Features */
  memcpy(btrc_item_player->features, avrc_item_player->features, BTRC_FEATURE_BIT_MASK_SIZE);

  memset(btrc_item_player->name, 0, BTRC_MAX_ATTR_STR_LEN * sizeof(uint8_t));
  memcpy(btrc_item_player->name, avrc_item_player->name.p_str, avrc_item_player->name.str_len);
}

/***************************************************************************
 *
 * Function         handle_change_path_response
 *
 * Description      handles the the change path response, calls
 *                  HAL callback to send the updated folder
 * Returns          None
 *
 **************************************************************************/
static void handle_change_path_response(tBTA_AV_META_MSG* pmeta_msg, tAVRC_CHG_PATH_RSP* p_rsp) {
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("Invalid rc handle");
    return;
  }

  if (p_rsp->status == AVRC_STS_NO_ERROR) {
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->change_folder_path_cb, p_dev->rc_addr,
                                    p_rsp->num_items));
  } else {
    log::error("error in handle_change_path_response {}", p_rsp->status);
  }
}

/***************************************************************************
 *
 * Function         handle_set_browsed_player_response
 *
 * Description      handles the the change path response, calls
 *                  HAL callback to send the updated folder
 * Returns          None
 *
 **************************************************************************/
static void handle_set_browsed_player_response(tBTA_AV_META_MSG* pmeta_msg,
                                               tAVRC_SET_BR_PLAYER_RSP* p_rsp) {
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);

  if (p_dev == NULL) {
    log::error("Invalid rc handle");
    return;
  }

  if (p_rsp->status == AVRC_STS_NO_ERROR) {
    do_in_jni_thread(base::BindOnce(bt_rc_ctrl_callbacks->set_browsed_player_cb, p_dev->rc_addr,
                                    p_rsp->num_items, p_rsp->folder_depth));
  } else {
    log::error("error {}", p_rsp->status);
  }
}

/***************************************************************************
 *
 * Function         clear_cmd_timeout
 *
 * Description      helper function to stop the command timeout timer
 * Returns          None
 *
 **************************************************************************/
static void clear_cmd_timeout(btif_rc_device_cb_t* p_dev, uint8_t label) {
  rc_transaction_t* p_txn;

  p_txn = get_transaction_by_lbl(p_dev, label);
  if (p_txn == NULL) {
    log::error("Error in transaction label lookup");
    return;
  }

  if (p_txn->timer != NULL) {
    // Free also calls alarm_cancel() in its implementation
    alarm_free(p_txn->timer);
  }
  p_txn->timer = nullptr;
}

/***************************************************************************
 *
 * Function         handle_avk_rc_metamsg_rsp
 *
 * Description      Handle RC metamessage response
 *
 * Returns          void
 *
 **************************************************************************/
static void handle_avk_rc_metamsg_rsp(tBTA_AV_META_MSG* pmeta_msg) {
  tAVRC_RESPONSE avrc_response = {0};
  uint8_t scratch_buf[512] = {0};  // this variable is unused
  uint16_t buf_len;
  tAVRC_STS status;
  btif_rc_device_cb_t* p_dev = NULL;

  log::verbose("opcode: {} rsp_code: {}", pmeta_msg->p_msg->hdr.opcode, pmeta_msg->code);

  p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);
  status = AVRC_Ctrl_ParsResponse(pmeta_msg->p_msg, &avrc_response, scratch_buf, &buf_len);
  if ((AVRC_OP_VENDOR == pmeta_msg->p_msg->hdr.opcode) && (pmeta_msg->code >= AVRC_RSP_NOT_IMPL) &&
      (pmeta_msg->code <= AVRC_RSP_INTERIM)) {
    log::verbose("parse status {} pdu = {} rsp_status = {}", status, avrc_response.pdu,
                 pmeta_msg->p_msg->vendor.hdr.ctype);

    switch (avrc_response.pdu) {
      case AVRC_PDU_REGISTER_NOTIFICATION:
        handle_notification_response(pmeta_msg, &avrc_response.reg_notif);
        if (pmeta_msg->code == AVRC_RSP_INTERIM) {
          /* Don't free the transaction Id */
          clear_cmd_timeout(p_dev, pmeta_msg->label);
          return;
        }
        break;

      case AVRC_PDU_GET_CAPABILITIES:
        handle_get_capability_response(pmeta_msg, &avrc_response.get_caps);
        break;

      case AVRC_PDU_LIST_PLAYER_APP_ATTR:
        handle_app_attr_response(pmeta_msg, &avrc_response.list_app_attr);
        break;

      case AVRC_PDU_LIST_PLAYER_APP_VALUES:
        handle_app_val_response(pmeta_msg, &avrc_response.list_app_values);
        break;

      case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
        handle_app_cur_val_response(pmeta_msg, &avrc_response.get_cur_app_val);
        break;

      case AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT:
        handle_app_attr_txt_response(pmeta_msg, &avrc_response.get_app_attr_txt);
        break;

      case AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT:
        handle_app_attr_val_txt_response(pmeta_msg, &avrc_response.get_app_val_txt);
        break;

      case AVRC_PDU_SET_PLAYER_APP_VALUE:
        handle_set_app_attr_val_response(pmeta_msg, &avrc_response.set_app_val);
        break;

      case AVRC_PDU_GET_ELEMENT_ATTR:
        handle_get_metadata_attr_response(pmeta_msg, &avrc_response.get_attrs);
        break;

      case AVRC_PDU_GET_PLAY_STATUS:
        handle_get_playstatus_response(pmeta_msg, &avrc_response.get_play_status);
        break;

      case AVRC_PDU_SET_ADDRESSED_PLAYER:
        handle_set_addressed_player_response(pmeta_msg, &avrc_response.rsp);
        break;
    }
  } else if (AVRC_OP_BROWSE == pmeta_msg->p_msg->hdr.opcode) {
    log::verbose("AVRC_OP_BROWSE pdu {}", avrc_response.pdu);
    /* check what kind of command it is for browsing */
    switch (avrc_response.pdu) {
      case AVRC_PDU_GET_FOLDER_ITEMS:
        handle_get_folder_items_response(pmeta_msg, &avrc_response.get_items);
        break;
      case AVRC_PDU_CHANGE_PATH:
        handle_change_path_response(pmeta_msg, &avrc_response.chg_path);
        break;
      case AVRC_PDU_SET_BROWSED_PLAYER:
        handle_set_browsed_player_response(pmeta_msg, &avrc_response.br_player);
        break;
      case AVRC_PDU_GET_ITEM_ATTRIBUTES:
        handle_get_metadata_attr_response(pmeta_msg, &avrc_response.get_attrs);
        break;
      default:
        log::error("cannot handle browse pdu {}", pmeta_msg->p_msg->hdr.opcode);
    }
  } else {
    log::verbose("Invalid Vendor Command code: {} len: {}. Not processing it.", pmeta_msg->code,
                 pmeta_msg->len);
    return;
  }
  log::verbose("release transaction {}", pmeta_msg->label);
  release_transaction(p_dev, pmeta_msg->label);
}

/***************************************************************************
 *
 * Function         handle_avk_rc_metamsg_cmd
 *
 * Description      Handle RC metamessage response
 *
 * Returns          void
 *
 **************************************************************************/
static void handle_avk_rc_metamsg_cmd(tBTA_AV_META_MSG* pmeta_msg) {
  tAVRC_COMMAND avrc_cmd = {0};
  tAVRC_STS status = BT_STATUS_UNSUPPORTED;
  btif_rc_device_cb_t* p_dev = NULL;

  log::verbose("opcode: {} rsp_code: {}", pmeta_msg->p_msg->hdr.opcode, pmeta_msg->code);
  status = AVRC_Ctrl_ParsCommand(pmeta_msg->p_msg, &avrc_cmd);
  if ((AVRC_OP_VENDOR == pmeta_msg->p_msg->hdr.opcode) && (pmeta_msg->code <= AVRC_CMD_GEN_INQ)) {
    log::verbose("Received vendor command.code {}, PDU {} label {}", pmeta_msg->code, avrc_cmd.pdu,
                 pmeta_msg->label);

    if (status != AVRC_STS_NO_ERROR) {
      /* return error */
      log::warn("Error in parsing received metamsg command. status: 0x{:02x}", status);
      if (true == btif_av_both_enable()) {
        if (AVRC_PDU_GET_CAPABILITIES == avrc_cmd.pdu ||
            AVRC_PDU_GET_ELEMENT_ATTR == avrc_cmd.pdu || AVRC_PDU_GET_PLAY_STATUS == avrc_cmd.pdu ||
            AVRC_PDU_GET_FOLDER_ITEMS == avrc_cmd.pdu ||
            AVRC_PDU_GET_ITEM_ATTRIBUTES == avrc_cmd.pdu) {
          return;
        }
      }
      send_reject_response(pmeta_msg->rc_handle, pmeta_msg->label, avrc_cmd.pdu, status,
                           pmeta_msg->p_msg->hdr.opcode);
    } else {
      p_dev = btif_rc_get_device_by_handle(pmeta_msg->rc_handle);
      if (p_dev == NULL) {
        log::error("avk rc meta msg cmd for Invalid rc handle");
        return;
      }

      if (avrc_cmd.pdu == AVRC_PDU_REGISTER_NOTIFICATION) {
        uint8_t event_id = avrc_cmd.reg_notif.event_id;
        log::verbose("Register notification event_id: {}", dump_rc_notification_event_id(event_id));
      } else if (avrc_cmd.pdu == AVRC_PDU_SET_ABSOLUTE_VOLUME) {
        log::verbose("Abs Volume Cmd Recvd");
      }

      btif_rc_ctrl_upstreams_rsp_cmd(avrc_cmd.pdu, &avrc_cmd, pmeta_msg->label, p_dev);
    }
  } else {
    log::verbose("Invalid Vendor Command  code: {} len: {}. Not processing it.", pmeta_msg->code,
                 pmeta_msg->len);
    return;
  }
}

/***************************************************************************
 *
 * Function         cleanup
 *
 * Description      Closes the AVRC interface
 *
 * Returns          void
 *
 **************************************************************************/
static void cleanup() {
  log::verbose("");
  if (bt_rc_callbacks) {
    bt_rc_callbacks = NULL;
  }

  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    alarm_free(btif_rc_cb.rc_multi_cb[idx].rc_play_status_timer);
    memset(&btif_rc_cb.rc_multi_cb[idx], 0, sizeof(btif_rc_cb.rc_multi_cb[idx]));
  }

  log::verbose("completed");
}

/***************************************************************************
 *
 * Function         cleanup_ctrl
 *
 * Description      Closes the AVRC Controller interface
 *
 * Returns          void
 *
 **************************************************************************/
static void cleanup_ctrl() {
  log::verbose("");

  if (bt_rc_ctrl_callbacks) {
    bt_rc_ctrl_callbacks = NULL;
  }

  for (int idx = 0; idx < BTIF_RC_NUM_CONN; idx++) {
    alarm_free(btif_rc_cb.rc_multi_cb[idx].rc_play_status_timer);
    memset(&btif_rc_cb.rc_multi_cb[idx], 0, sizeof(btif_rc_cb.rc_multi_cb[idx]));
  }

  memset(&btif_rc_cb.rc_multi_cb, 0, sizeof(btif_rc_cb.rc_multi_cb));
  log::verbose("completed");
}

/***************************************************************************
 *
 * Function         getcapabilities_cmd
 *
 * Description      GetCapabilties from Remote(Company_ID, Events_Supported)
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t getcapabilities_cmd(uint8_t cap_id, btif_rc_device_cb_t* p_dev) {
  log::verbose("cap_id: {}", cap_id);
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.get_caps.opcode = AVRC_OP_VENDOR;
  avrc_cmd.get_caps.capability_id = cap_id;
  avrc_cmd.get_caps.pdu = AVRC_PDU_GET_CAPABILITIES;
  avrc_cmd.get_caps.status = AVRC_STS_NO_ERROR;

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         list_player_app_setting_attrib_cmd
 *
 * Description      Get supported List Player Attributes
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t list_player_app_setting_attrib_cmd(btif_rc_device_cb_t* p_dev) {
  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.list_app_attr.opcode = AVRC_OP_VENDOR;
  avrc_cmd.list_app_attr.pdu = AVRC_PDU_LIST_PLAYER_APP_ATTR;
  avrc_cmd.list_app_attr.status = AVRC_STS_NO_ERROR;

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         list_player_app_setting_value_cmd
 *
 * Description      Get values of supported Player Attributes
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t list_player_app_setting_value_cmd(uint8_t attrib_id,
                                                     btif_rc_device_cb_t* p_dev) {
  log::verbose("attrib_id: {}", attrib_id);
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.list_app_values.attr_id = attrib_id;
  avrc_cmd.list_app_values.opcode = AVRC_OP_VENDOR;
  avrc_cmd.list_app_values.pdu = AVRC_PDU_LIST_PLAYER_APP_VALUES;
  avrc_cmd.list_app_values.status = AVRC_STS_NO_ERROR;

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         get_player_app_setting_cmd
 *
 * Description      Get current values of Player Attributes
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t get_player_app_setting_cmd(uint8_t num_attrib, uint8_t* attrib_ids,
                                              btif_rc_device_cb_t* p_dev) {
  log::verbose("num_attrib: {}", num_attrib);
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.get_cur_app_val.opcode = AVRC_OP_VENDOR;
  avrc_cmd.get_cur_app_val.status = AVRC_STS_NO_ERROR;
  avrc_cmd.get_cur_app_val.num_attr = num_attrib;
  avrc_cmd.get_cur_app_val.pdu = AVRC_PDU_GET_CUR_PLAYER_APP_VALUE;

  for (int count = 0; count < num_attrib; count++) {
    avrc_cmd.get_cur_app_val.attrs[count] = attrib_ids[count];
  }

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         get_current_metadata_cmd
 *
 * Description      Fetch the current track metadata for the device
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t get_current_metadata_cmd(const RawAddress& bd_addr) {
  log::verbose("");
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return BT_STATUS_DEVICE_NOT_FOUND;
  }
  const uint32_t* attr_list = get_requested_attributes_list(p_dev);
  const uint8_t attr_list_size = get_requested_attributes_list_size(p_dev);
  return get_metadata_attribute_cmd(attr_list_size, attr_list, p_dev);
}

/***************************************************************************
 *
 * Function         get_playback_state_cmd
 *
 * Description      Fetch the current playback state for the device
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t get_playback_state_cmd(const RawAddress& bd_addr) {
  log::verbose("");
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  return get_play_status_cmd(p_dev);
}

/***************************************************************************
 *
 * Function         get_now_playing_list_cmd
 *
 * Description      Fetch the now playing list
 *
 * Paramters        start_item: First item to fetch (0 to fetch from beganning)
 *                  end_item: Last item to fetch (0xffffffff to fetch until end)
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t get_now_playing_list_cmd(const RawAddress& bd_addr, uint32_t start_item,
                                            uint32_t end_item) {
  log::verbose("start, end: ({}, {})", start_item, end_item);
  return get_folder_items_cmd(bd_addr, AVRC_SCOPE_NOW_PLAYING, start_item, end_item);
}

/***************************************************************************
 *
 * Function         get_item_attribute_cmd
 *
 * Description      Fetch the item attributes for a given uid.
 *
 * Parameters       uid: Track UID you want attributes for
 *                  scope: Constant representing which scope you're querying
 *                         (i.e AVRC_SCOPE_FILE_SYSTEM)
 *                  p_dev: Device control block
 *
 * Returns          BT_STATUS_SUCCESS if command is issued successfully
 *                  otherwise BT_STATUS_FAIL
 *
 **************************************************************************/
static bt_status_t get_item_attribute_cmd(uint64_t uid, int scope, uint8_t /*num_attribute*/,
                                          const uint32_t* /*p_attr_ids*/,
                                          btif_rc_device_cb_t* p_dev) {
  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.pdu = AVRC_PDU_GET_ITEM_ATTRIBUTES;
  avrc_cmd.get_attrs.scope = scope;
  memcpy(avrc_cmd.get_attrs.uid, &uid, 8);
  avrc_cmd.get_attrs.uid_counter = 0;
  avrc_cmd.get_attrs.attr_count = 0;

  return build_and_send_browsing_cmd(&avrc_cmd, p_dev);
}

/***************************************************************************
 *
 * Function         get_folder_list_cmd
 *
 * Description      Fetch the currently selected folder list
 *
 * Paramters        start_item: First item to fetch (0 to fetch from beganning)
 *                  end_item: Last item to fetch (0xffffffff to fetch until end)
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t get_folder_list_cmd(const RawAddress& bd_addr, uint32_t start_item,
                                       uint32_t end_item) {
  log::verbose("start, end: ({}, {})", start_item, end_item);
  return get_folder_items_cmd(bd_addr, AVRC_SCOPE_FILE_SYSTEM, start_item, end_item);
}

/***************************************************************************
 *
 * Function         get_player_list_cmd
 *
 * Description      Fetch the player list
 *
 * Paramters        start_item: First item to fetch (0 to fetch from beganning)
 *                  end_item: Last item to fetch (0xffffffff to fetch until end)
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t get_player_list_cmd(const RawAddress& bd_addr, uint32_t start_item,
                                       uint32_t end_item) {
  log::verbose("start, end: ({}, {})", start_item, end_item);
  return get_folder_items_cmd(bd_addr, AVRC_SCOPE_PLAYER_LIST, start_item, end_item);
}

/***************************************************************************
 *
 * Function         change_folder_path_cmd
 *
 * Description      Change the folder.
 *
 * Paramters        direction: Direction (Up/Down) to change folder
 *                  uid: The UID of folder to move to
 *                  start_item: First item to fetch (0 to fetch from beganning)
 *                  end_item: Last item to fetch (0xffffffff to fetch until end)
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t change_folder_path_cmd(const RawAddress& bd_addr, uint8_t direction,
                                          uint8_t* uid) {
  log::verbose("direction {}", direction);
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  CHECK_RC_CONNECTED(p_dev);
  CHECK_BR_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};

  avrc_cmd.chg_path.pdu = AVRC_PDU_CHANGE_PATH;
  avrc_cmd.chg_path.status = AVRC_STS_NO_ERROR;
  // TODO(sanketa): Improve for database aware clients.
  avrc_cmd.chg_path.uid_counter = 0;
  avrc_cmd.chg_path.direction = direction;

  memset(avrc_cmd.chg_path.folder_uid, 0, AVRC_UID_SIZE * sizeof(uint8_t));
  memcpy(avrc_cmd.chg_path.folder_uid, uid, AVRC_UID_SIZE * sizeof(uint8_t));

  return build_and_send_browsing_cmd(&avrc_cmd, p_dev);
}

/***************************************************************************
 *
 * Function         set_browsed_player_cmd
 *
 * Description      Change the browsed player.
 *
 * Paramters        id: The UID of player to move to
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t set_browsed_player_cmd(const RawAddress& bd_addr, uint16_t id) {
  log::verbose("id {}", id);
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  CHECK_RC_CONNECTED(p_dev);
  CHECK_BR_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.br_player.pdu = AVRC_PDU_SET_BROWSED_PLAYER;
  avrc_cmd.br_player.status = AVRC_STS_NO_ERROR;
  // TODO(sanketa): Improve for database aware clients.
  avrc_cmd.br_player.player_id = id;

  return build_and_send_browsing_cmd(&avrc_cmd, p_dev);
}

/***************************************************************************
 **
 ** Function         set_addressed_player_cmd
 **
 ** Description      Change the addressed player.
 **
 ** Paramters        id: The UID of player to move to
 **
 ** Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 **                  BT_STATUS_FAIL.
 **
 ***************************************************************************/
static bt_status_t set_addressed_player_cmd(const RawAddress& bd_addr, uint16_t id) {
  log::verbose("id {}", id);

  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  CHECK_RC_CONNECTED(p_dev);
  CHECK_BR_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.addr_player.pdu = AVRC_PDU_SET_ADDRESSED_PLAYER;
  avrc_cmd.addr_player.status = AVRC_STS_NO_ERROR;
  // TODO(sanketa): Improve for database aware clients.
  avrc_cmd.addr_player.player_id = id;

  return build_and_send_browsing_cmd(&avrc_cmd, p_dev);
}

/***************************************************************************
 *
 * Function         get_folder_items_cmd
 *
 * Description      Helper function to browse the content hierarchy of the
 *                  TG device.
 *
 * Paramters        scope: AVRC_SCOPE_NOW_PLAYING (etc) for various browseable
 *                  content
 *                  start_item: First item to fetch (0 to fetch from beganning)
 *                  end_item: Last item to fetch (0xffff to fetch until end)
 *
 * Returns          BT_STATUS_SUCCESS if command issued successfully otherwise
 *                  BT_STATUS_FAIL.
 *
 **************************************************************************/
static bt_status_t get_folder_items_cmd(const RawAddress& bd_addr, uint8_t scope,
                                        uint32_t start_item, uint32_t end_item) {
  /* Check that both avrcp and browse channel are connected. */
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);
  CHECK_BR_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};

  /* Set the layer specific to point to browse although this should really
   * be done by lower layers and looking at the PDU
   */
  avrc_cmd.get_items.pdu = AVRC_PDU_GET_FOLDER_ITEMS;
  avrc_cmd.get_items.status = AVRC_STS_NO_ERROR;
  avrc_cmd.get_items.scope = scope;
  avrc_cmd.get_items.start_item = start_item;
  avrc_cmd.get_items.end_item = end_item;
  avrc_cmd.get_items.attr_count = 0; /* p_attr_list does not matter hence */

  return build_and_send_browsing_cmd(&avrc_cmd, p_dev);
}

/***************************************************************************
 *
 * Function         change_player_app_setting
 *
 * Description      Set current values of Player Attributes
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t change_player_app_setting(const RawAddress& bd_addr, uint8_t num_attrib,
                                             uint8_t* attrib_ids, uint8_t* attrib_vals) {
  log::verbose("num_attrib: {}", num_attrib);
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.set_app_val.opcode = AVRC_OP_VENDOR;
  avrc_cmd.set_app_val.status = AVRC_STS_NO_ERROR;
  avrc_cmd.set_app_val.num_val = num_attrib;
  avrc_cmd.set_app_val.pdu = AVRC_PDU_SET_PLAYER_APP_VALUE;
  avrc_cmd.set_app_val.p_vals =
          (tAVRC_APP_SETTING*)osi_malloc(sizeof(tAVRC_APP_SETTING) * num_attrib);
  for (int count = 0; count < num_attrib; count++) {
    avrc_cmd.set_app_val.p_vals[count].attr_id = attrib_ids[count];
    avrc_cmd.set_app_val.p_vals[count].attr_val = attrib_vals[count];
  }

  bt_status_t st = build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_CTRL, p_dev);
  osi_free_and_reset((void**)&avrc_cmd.set_app_val.p_vals);
  return st;
}

/***************************************************************************
 *
 * Function         play_item_cmd
 *
 * Description      Play the item specified by UID & scope
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t play_item_cmd(const RawAddress& bd_addr, uint8_t scope, uint8_t* uid,
                                 uint16_t uid_counter) {
  log::verbose("scope {} uid_counter {}", scope, uid_counter);
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);
  CHECK_RC_CONNECTED(p_dev);
  CHECK_BR_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.pdu = AVRC_PDU_PLAY_ITEM;
  avrc_cmd.play_item.opcode = AVRC_OP_VENDOR;
  avrc_cmd.play_item.status = AVRC_STS_NO_ERROR;
  avrc_cmd.play_item.scope = scope;
  memcpy(avrc_cmd.play_item.uid, uid, AVRC_UID_SIZE);
  avrc_cmd.play_item.uid_counter = uid_counter;

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_CTRL, p_dev);
}

/***************************************************************************
 *
 * Function         get_player_app_setting_attr_text_cmd
 *
 * Description      Get text description for app attribute
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t get_player_app_setting_attr_text_cmd(uint8_t* attrs, uint8_t num_attrs,
                                                        btif_rc_device_cb_t* p_dev) {
  log::verbose("num attrs: {}", num_attrs);
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.pdu = AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT;
  avrc_cmd.get_app_attr_txt.opcode = AVRC_OP_VENDOR;
  avrc_cmd.get_app_attr_txt.num_attr = num_attrs;

  for (int count = 0; count < num_attrs; count++) {
    avrc_cmd.get_app_attr_txt.attrs[count] = attrs[count];
  }

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         get_player_app_setting_val_text_cmd
 *
 * Description      Get text description for app attribute values
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t get_player_app_setting_value_text_cmd(uint8_t* vals, uint8_t num_vals,
                                                         btif_rc_device_cb_t* p_dev) {
  log::verbose("num_vals: {}", num_vals);
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.pdu = AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT;
  avrc_cmd.get_app_val_txt.opcode = AVRC_OP_VENDOR;
  avrc_cmd.get_app_val_txt.num_val = num_vals;

  for (int count = 0; count < num_vals; count++) {
    avrc_cmd.get_app_val_txt.vals[count] = vals[count];
  }

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         register_notification_cmd
 *
 * Description      Send Command to register for a Notification ID
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t register_notification_cmd(uint8_t event_id, uint32_t event_value,
                                             btif_rc_device_cb_t* p_dev) {
  log::verbose("event_id: {} event_value {}", event_id, event_value);
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.reg_notif.opcode = AVRC_OP_VENDOR;
  avrc_cmd.reg_notif.status = AVRC_STS_NO_ERROR;
  avrc_cmd.reg_notif.event_id = event_id;
  avrc_cmd.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
  avrc_cmd.reg_notif.param = event_value;

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_NOTIF, p_dev);
}

/***************************************************************************
 *
 * Function         get_metadata_attribute_cmd
 *
 * Description      Get metadata attributes for attributeIds. This function
 *                  will make the right determination of whether to use the
 *                  control or browsing channel for the request
 *
 * Returns          BT_STATUS_SUCCESS if the command is successfully issued
 *                  otherwise BT_STATUS_FAIL
 *
 **************************************************************************/
static bt_status_t get_metadata_attribute_cmd(uint8_t num_attribute, const uint32_t* p_attr_ids,
                                              btif_rc_device_cb_t* p_dev) {
  log::verbose("num_attribute: {} attribute_id: {}", num_attribute, p_attr_ids[0]);

  // If browsing is connected then send the command out that channel
  if (p_dev->br_connected) {
    return get_item_attribute_cmd(p_dev->rc_playing_uid, AVRC_SCOPE_NOW_PLAYING, num_attribute,
                                  p_attr_ids, p_dev);
  }

  // Otherwise, default to the control channel
  return get_element_attribute_cmd(num_attribute, p_attr_ids, p_dev);
}

/***************************************************************************
 *
 * Function         get_element_attribute_cmd
 *
 * Description      Get Element Attribute for  attributeIds
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t get_element_attribute_cmd(uint8_t num_attribute, const uint32_t* p_attr_ids,
                                             btif_rc_device_cb_t* p_dev) {
  log::verbose("num_attribute: {} attribute_id: {}", num_attribute, p_attr_ids[0]);
  CHECK_RC_CONNECTED(p_dev);
  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.get_elem_attrs.opcode = AVRC_OP_VENDOR;
  avrc_cmd.get_elem_attrs.status = AVRC_STS_NO_ERROR;
  avrc_cmd.get_elem_attrs.num_attr = num_attribute;
  avrc_cmd.get_elem_attrs.pdu = AVRC_PDU_GET_ELEMENT_ATTR;
  for (int count = 0; count < num_attribute; count++) {
    avrc_cmd.get_elem_attrs.attrs[count] = p_attr_ids[count];
  }

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         get_play_status_cmd
 *
 * Description      Get Playing Status of a Device
 *
 * Returns          bt_status_t
 *
 **************************************************************************/
static bt_status_t get_play_status_cmd(btif_rc_device_cb_t* p_dev) {
  log::verbose("");
  CHECK_RC_CONNECTED(p_dev);

  tAVRC_COMMAND avrc_cmd = {0};
  avrc_cmd.get_play_status.opcode = AVRC_OP_VENDOR;
  avrc_cmd.get_play_status.pdu = AVRC_PDU_GET_PLAY_STATUS;
  avrc_cmd.get_play_status.status = AVRC_STS_NO_ERROR;

  return build_and_send_vendor_cmd(&avrc_cmd, AVRC_CMD_STATUS, p_dev);
}

/***************************************************************************
 *
 * Function         set_volume_rsp
 *
 * Description      Rsp for SetAbsoluteVolume Command
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t set_volume_rsp(const RawAddress& bd_addr, uint8_t abs_vol, uint8_t label) {
  tAVRC_STS status = BT_STATUS_UNSUPPORTED;
  tAVRC_RESPONSE avrc_rsp;
  BT_HDR* p_msg = NULL;
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  CHECK_RC_CONNECTED(p_dev);

  log::verbose("abs_vol: {}", abs_vol);

  avrc_rsp.volume.opcode = AVRC_OP_VENDOR;
  avrc_rsp.volume.pdu = AVRC_PDU_SET_ABSOLUTE_VOLUME;
  avrc_rsp.volume.status = AVRC_STS_NO_ERROR;
  avrc_rsp.volume.volume = abs_vol;
  status = AVRC_BldResponse(p_dev->rc_handle, &avrc_rsp, &p_msg);
  if (status == AVRC_STS_NO_ERROR) {
    uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
    log::verbose("msgreq being sent out with label: {}", p_dev->rc_vol_label);
    if (p_msg != NULL) {
      BTA_AvVendorRsp(p_dev->rc_handle, label, AVRC_RSP_ACCEPT, data_start, p_msg->len, 0);
      status = BT_STATUS_SUCCESS;
    }
  } else {
    log::error("failed to build command. status: 0x{:02x}", status);
  }
  osi_free(p_msg);
  return (bt_status_t)status;
}

/***************************************************************************
 *
 * Function         send_register_abs_vol_rsp
 *
 * Description      Rsp for Notification of Absolute Volume
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t volume_change_notification_rsp(const RawAddress& bd_addr,
                                                  btrc_notification_type_t rsp_type,
                                                  uint8_t abs_vol, uint8_t label) {
  tAVRC_STS status = BT_STATUS_UNSUPPORTED;
  tAVRC_RESPONSE avrc_rsp;
  BT_HDR* p_msg = NULL;
  log::verbose("rsp_type: {} abs_vol: {}", rsp_type, abs_vol);

  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  CHECK_RC_CONNECTED(p_dev);

  avrc_rsp.reg_notif.opcode = AVRC_OP_VENDOR;
  avrc_rsp.reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
  avrc_rsp.reg_notif.status = AVRC_STS_NO_ERROR;
  avrc_rsp.reg_notif.param.volume = abs_vol;
  avrc_rsp.reg_notif.event_id = AVRC_EVT_VOLUME_CHANGE;

  status = AVRC_BldResponse(p_dev->rc_handle, &avrc_rsp, &p_msg);
  if (status == AVRC_STS_NO_ERROR) {
    log::verbose("msgreq being sent out with label: {}", label);
    uint8_t* data_start = (uint8_t*)(p_msg + 1) + p_msg->offset;
    BTA_AvVendorRsp(
            p_dev->rc_handle, label,
            (rsp_type == BTRC_NOTIFICATION_TYPE_INTERIM) ? AVRC_RSP_INTERIM : AVRC_RSP_CHANGED,
            data_start, p_msg->len, 0);
    status = BT_STATUS_SUCCESS;
  } else {
    log::error("failed to build command. status: 0x{:02x}", status);
  }
  osi_free(p_msg);

  return (bt_status_t)status;
}

/***************************************************************************
 *
 * Function         send_groupnavigation_cmd
 *
 * Description      Send Pass-Through command
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t send_groupnavigation_cmd(const RawAddress& bd_addr, uint8_t key_code,
                                            uint8_t key_state) {
  tAVRC_STS status = BT_STATUS_UNSUPPORTED;
  rc_transaction_t* p_transaction = NULL;
  log::verbose("key-code: {}, key-state: {}", key_code, key_state);
  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(bd_addr);

  CHECK_RC_CONNECTED(p_dev);

  if (p_dev->rc_features & BTA_AV_FEAT_RCTG) {
    rc_transaction_context_t context = {
            .rc_addr = p_dev->rc_addr,
            .label = MAX_LABEL,
            .opcode = AVRC_OP_PASS_THRU,
            .command = {.passthru = {AVRC_ID_VENDOR, key_state, key_code}}};
    bt_status_t tran_status = get_transaction(p_dev, context, &p_transaction);
    if ((BT_STATUS_SUCCESS == tran_status) && (NULL != p_transaction)) {
      uint8_t buffer[AVRC_PASS_THRU_GROUP_LEN] = {0};
      uint8_t* start = buffer;
      UINT24_TO_BE_STREAM(start, AVRC_CO_METADATA);
      *(start)++ = 0;
      UINT8_TO_BE_STREAM(start, key_code);
      BTA_AvRemoteVendorUniqueCmd(p_dev->rc_handle, p_transaction->label, (tBTA_AV_STATE)key_state,
                                  buffer, AVRC_PASS_THRU_GROUP_LEN);
      status = BT_STATUS_SUCCESS;
      start_transaction_timer(p_dev, p_transaction->label, BTIF_RC_TIMEOUT_MS);
      log::verbose("Send command, key-code={}, key-state={}, label={}", key_code, key_state,
                   p_transaction->label);
    } else {
      status = BT_STATUS_FAIL;
      log::error("failed to get label, key-code={}, key-state={}, status={}", key_code, key_state,
                 tran_status);
    }
  } else {
    status = BT_STATUS_UNSUPPORTED;
    log::verbose("feature not supported");
  }
  return (bt_status_t)status;
}

/***************************************************************************
 *
 * Function         send_passthrough_cmd
 *
 * Description      Send Pass-Through command
 *
 * Returns          void
 *
 **************************************************************************/
static bt_status_t send_passthrough_cmd(const RawAddress& bd_addr, uint8_t key_code,
                                        uint8_t key_state) {
  tAVRC_STS status = BT_STATUS_UNSUPPORTED;
  btif_rc_device_cb_t* p_dev = NULL;
  log::error("calling btif_rc_get_device_by_bda");
  p_dev = btif_rc_get_device_by_bda(bd_addr);

  CHECK_RC_CONNECTED(p_dev);

  rc_transaction_t* p_transaction = NULL;
  log::verbose("key-code: {}, key-state: {}", key_code, key_state);
  if (p_dev->rc_features & BTA_AV_FEAT_RCTG) {
    rc_transaction_context_t context = {
            .rc_addr = p_dev->rc_addr,
            .label = MAX_LABEL,
            .opcode = AVRC_OP_PASS_THRU,
            .command = {.passthru = {AVRC_ID_VENDOR, key_state, key_code}}};
    bt_status_t tran_status = get_transaction(p_dev, context, &p_transaction);
    if (BT_STATUS_SUCCESS == tran_status && NULL != p_transaction) {
      BTA_AvRemoteCmd(p_dev->rc_handle, p_transaction->label, (tBTA_AV_RC)key_code,
                      (tBTA_AV_STATE)key_state);
      status = BT_STATUS_SUCCESS;
      start_transaction_timer(p_dev, p_transaction->label, BTIF_RC_TIMEOUT_MS);
      log::verbose("Send command, key-code={}, key-state={}, label={}", key_code, key_state,
                   p_transaction->label);
    } else {
      status = BT_STATUS_FAIL;
      log::error("failed to get label, key-code={}, key-state={}, status={}", key_code, key_state,
                 tran_status);
    }
  } else {
    status = BT_STATUS_UNSUPPORTED;
    log::verbose("feature not supported");
  }
  return (bt_status_t)status;
}

static const btrc_interface_t bt_rc_interface = {
        sizeof(bt_rc_interface),
        init,
        get_play_status_rsp,
        NULL, /* list_player_app_attr_rsp */
        NULL, /* list_player_app_value_rsp */
        NULL, /* get_player_app_value_rsp */
        NULL, /* get_player_app_attr_text_rsp */
        NULL, /* get_player_app_value_text_rsp */
        get_element_attr_rsp,
        NULL, /* set_player_app_value_rsp */
        register_notification_rsp,
        set_volume,
        set_addressed_player_rsp,
        set_browsed_player_rsp,
        get_folder_items_list_rsp,
        change_path_rsp,
        get_item_attr_rsp,
        play_item_rsp,
        get_total_num_of_items_rsp,
        search_rsp,
        add_to_now_playing_rsp,
        cleanup,
};

static const btrc_ctrl_interface_t bt_rc_ctrl_interface = {
        sizeof(bt_rc_ctrl_interface),
        init_ctrl,
        send_passthrough_cmd,
        send_groupnavigation_cmd,
        change_player_app_setting,
        play_item_cmd,
        get_current_metadata_cmd,
        get_playback_state_cmd,
        get_now_playing_list_cmd,
        get_folder_list_cmd,
        get_player_list_cmd,
        change_folder_path_cmd,
        set_browsed_player_cmd,
        set_addressed_player_cmd,
        set_volume_rsp,
        volume_change_notification_rsp,
        cleanup_ctrl,
};

/*******************************************************************************
 *
 * Function         btif_rc_get_interface
 *
 * Description      Get the AVRCP Target callback interface
 *
 * Returns          btrc_interface_t
 *
 ******************************************************************************/
const btrc_interface_t* btif_rc_get_interface(void) { return &bt_rc_interface; }

/*******************************************************************************
 *
 * Function         btif_rc_ctrl_get_interface
 *
 * Description      Get the AVRCP Controller callback interface
 *
 * Returns          btrc_ctrl_interface_t
 *
 ******************************************************************************/
const btrc_ctrl_interface_t* btif_rc_ctrl_get_interface(void) { return &bt_rc_ctrl_interface; }

/*******************************************************************************
 *      Function         initialize_transaction
 *
 *      Description    Initializes fields of the transaction structure
 *
 *      Returns          void
 ******************************************************************************/
static void initialize_transaction(btif_rc_device_cb_t* p_dev, uint8_t lbl) {
  if (p_dev == nullptr) {
    return;
  }
  rc_transaction_set_t* transaction_set = &(p_dev->transaction_set);
  std::unique_lock<std::recursive_mutex> lock(transaction_set->label_lock);
  if (lbl < MAX_TRANSACTIONS_PER_SESSION) {
    log::verbose("initialize transaction, dev={}, label={}", p_dev->rc_addr, lbl);
    if (alarm_is_scheduled(transaction_set->transaction[lbl].timer)) {
      log::warn("clearing pending timer event, dev={}, label={}", p_dev->rc_addr, lbl);
      clear_cmd_timeout(p_dev, lbl);
    }
    transaction_set->transaction[lbl] = {
            .in_use = false,
            .label = lbl,
            .context =
                    {
                            .rc_addr = RawAddress::kEmpty,
                            .label = MAX_LABEL,
                            .opcode = AVRC_OP_INVALID,
                            .command = {},
                    },
            .timer = nullptr,
    };
  }
}

/*******************************************************************************
 *
 * Function         init_all_transactions
 *
 * Description    Initializes all transactions
 *
 * Returns          void
 ******************************************************************************/
void init_all_transactions(btif_rc_device_cb_t* p_dev) {
  if (p_dev == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < MAX_TRANSACTIONS_PER_SESSION; ++i) {
    initialize_transaction(p_dev, i);
  }
}

/*******************************************************************************
 *
 * Function         get_transaction_by_lbl
 *
 * Description    Will return a transaction based on the label. If not inuse
 *                     will return an error.
 *
 * Returns          bt_status_t
 ******************************************************************************/
rc_transaction_t* get_transaction_by_lbl(btif_rc_device_cb_t* p_dev, uint8_t lbl) {
  if (p_dev == nullptr) {
    return nullptr;
  }

  rc_transaction_t* transaction = NULL;
  rc_transaction_set_t* transaction_set = &(p_dev->transaction_set);
  std::unique_lock<std::recursive_mutex> lock(transaction_set->label_lock);

  /* Determine if this is a valid label */
  if (lbl < MAX_TRANSACTIONS_PER_SESSION) {
    if (!transaction_set->transaction[lbl].in_use) {
      transaction = NULL;
    } else {
      transaction = &(transaction_set->transaction[lbl]);
    }
  }
  return transaction;
}

/*******************************************************************************
 *
 * Function         get_transaction
 *
 * Description    Obtains the transaction details.
 *
 * Returns          bt_status_t
 ******************************************************************************/
static bt_status_t get_transaction(btif_rc_device_cb_t* p_dev, rc_transaction_context_t& context,
                                   rc_transaction_t** ptransaction) {
  if (p_dev == NULL) {
    return BT_STATUS_PARM_INVALID;
  }
  rc_transaction_set_t* transaction_set = &(p_dev->transaction_set);
  std::unique_lock<std::recursive_mutex> lock(transaction_set->label_lock);

  // Check for unused transactions in the device's transaction set
  for (uint8_t i = 0; i < MAX_TRANSACTIONS_PER_SESSION; i++) {
    if (!transaction_set->transaction[i].in_use) {
      context.label = i;
      transaction_set->transaction[i].context = context;
      transaction_set->transaction[i].in_use = true;
      *ptransaction = &(transaction_set->transaction[i]);
      log::verbose("Assigned transaction, dev={}, transaction={}", p_dev->rc_addr,
                   dump_transaction(*ptransaction));
      return BT_STATUS_SUCCESS;
    }
  }
  log::error("p_dev={}, failed to find free transaction", p_dev->rc_addr);
  return BT_STATUS_NOMEM;
}

/*******************************************************************************
 *
 * Function       start_transaction_timer
 *
 * Description    Starts a timer to release the label in case we don't get a
 *                response. Uses the central timeout handler, which will route
 *                timeout events based on context opcode and pdu_id
 *
 * Returns        void
 ******************************************************************************/
static void start_transaction_timer(btif_rc_device_cb_t* p_dev, uint8_t label,
                                    uint64_t timeout_ms) {
  rc_transaction_t* transaction = get_transaction_by_lbl(p_dev, label);
  if (transaction == nullptr) {
    log::error("transaction is null");
    return;
  }

  if (alarm_is_scheduled(transaction->timer)) {
    log::warn("Restarting timer that's already scheduled");
  }

  std::stringstream ss;
  ss << "btif_rc." << p_dev->rc_addr.ToColonSepHexString() << "." << transaction->label;
  alarm_free(transaction->timer);
  transaction->timer = alarm_new(ss.str().c_str());
  alarm_set_on_mloop(transaction->timer, timeout_ms, btif_rc_transaction_timer_timeout,
                     &transaction->context);
}

/*******************************************************************************
 *
 * Function         release_transaction
 *
 * Description    Will release a transaction for reuse
 *
 * Returns          bt_status_t
 ******************************************************************************/
void release_transaction(btif_rc_device_cb_t* p_dev, uint8_t lbl) {
  if (p_dev == nullptr) {
    log::warn("Failed to release transaction, dev=null, label={}", lbl);
    return;
  }
  rc_transaction_set_t* transaction_set = &(p_dev->transaction_set);
  std::unique_lock<std::recursive_mutex> lock(transaction_set->label_lock);

  rc_transaction_t* transaction = get_transaction_by_lbl(p_dev, lbl);

  /* If the transaction is in use... */
  if (transaction != NULL) {
    log::verbose("Released transaction, dev={}, label={}", p_dev->rc_addr, lbl);
    initialize_transaction(p_dev, lbl);
  } else {
    log::warn("Failed to release transaction, could not find dev={}, label={}", p_dev->rc_addr,
              lbl);
  }
}

/*******************************************************************************
 *
 * Function       dump_transaction
 *
 * Description    Dump transactions info for debugging
 *
 * Returns        String of transaction info
 ******************************************************************************/
static std::string dump_transaction(const rc_transaction_t* const transaction) {
  std::stringstream ss;

  ss << "label=" << (int)transaction->label;
  ss << " in_use=" << (transaction->in_use ? "true" : "false");

  rc_transaction_context_t context = transaction->context;
  ss << " context=(";
  uint8_t opcode_id = context.opcode;
  ss << "opcode=" << dump_rc_opcode(opcode_id);
  switch (opcode_id) {
    case AVRC_OP_VENDOR:
      ss << " pdu_id=" << dump_rc_pdu(context.command.vendor.pdu_id);
      if (context.command.vendor.pdu_id == AVRC_PDU_REGISTER_NOTIFICATION) {
        ss << " event_id=" << dump_rc_notification_event_id(context.command.vendor.event_id);
      }
      break;
    case AVRC_OP_BROWSE:
      ss << " pdu_id=" << dump_rc_pdu(context.command.browse.pdu_id);
      break;
    case AVRC_OP_PASS_THRU:
      ss << " rc_id=" << context.command.passthru.rc_id;
      ss << " key_state=" << context.command.passthru.key_state;
      break;
  }
  ss << ")";

  ss << " alarm=";
  alarm_t* alarm = transaction->timer;
  if (alarm != nullptr) {
    ss << "(set=" << alarm_is_scheduled(alarm) << " left=" << alarm_get_remaining_ms(alarm) << ")";
  } else {
    ss << "null";
  }
  return ss.str();
}

/***************************************************************************
 *
 * Function         vendor_cmd_timeout_handler
 *
 * Description      vendor dependent command timeout handler
 * Returns          None
 *
 **************************************************************************/
static void vendor_cmd_timeout_handler(btif_rc_device_cb_t* p_dev, uint8_t label,
                                       rc_vendor_context_t* p_context) {
  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  tAVRC_RESPONSE avrc_response = {0};
  tBTA_AV_META_MSG meta_msg = {.rc_handle = p_dev->rc_handle};

  log::warn("timeout, addr={}, label={}, pdu_id={}, event_id={}", p_dev->rc_addr, label,
            dump_rc_pdu(p_context->pdu_id), dump_rc_notification_event_id(p_context->event_id));

  switch (p_context->pdu_id) {
    case AVRC_PDU_REGISTER_NOTIFICATION:
      rc_notification_interim_timeout(p_dev, p_context->event_id);
      break;

    case AVRC_PDU_GET_CAPABILITIES:
      avrc_response.get_caps.status = BTIF_RC_STS_TIMEOUT;
      handle_get_capability_response(&meta_msg, &avrc_response.get_caps);
      break;

    case AVRC_PDU_LIST_PLAYER_APP_ATTR:
      avrc_response.list_app_attr.status = BTIF_RC_STS_TIMEOUT;
      handle_app_attr_response(&meta_msg, &avrc_response.list_app_attr);
      break;

    case AVRC_PDU_LIST_PLAYER_APP_VALUES:
      avrc_response.list_app_values.status = BTIF_RC_STS_TIMEOUT;
      handle_app_val_response(&meta_msg, &avrc_response.list_app_values);
      break;

    case AVRC_PDU_GET_CUR_PLAYER_APP_VALUE:
      avrc_response.get_cur_app_val.status = BTIF_RC_STS_TIMEOUT;
      handle_app_cur_val_response(&meta_msg, &avrc_response.get_cur_app_val);
      break;

    case AVRC_PDU_GET_PLAYER_APP_ATTR_TEXT:
      avrc_response.get_app_attr_txt.status = BTIF_RC_STS_TIMEOUT;
      handle_app_attr_txt_response(&meta_msg, &avrc_response.get_app_attr_txt);
      break;

    case AVRC_PDU_GET_PLAYER_APP_VALUE_TEXT:
      avrc_response.get_app_val_txt.status = BTIF_RC_STS_TIMEOUT;
      handle_app_attr_txt_response(&meta_msg, &avrc_response.get_app_val_txt);
      break;

    case AVRC_PDU_GET_ELEMENT_ATTR:
      avrc_response.get_attrs.status = BTIF_RC_STS_TIMEOUT;
      handle_get_metadata_attr_response(&meta_msg, &avrc_response.get_attrs);
      break;

    case AVRC_PDU_GET_PLAY_STATUS:
      avrc_response.get_play_status.status = BTIF_RC_STS_TIMEOUT;
      handle_get_playstatus_response(&meta_msg, &avrc_response.get_play_status);
      break;

    case AVRC_PDU_SET_PLAYER_APP_VALUE:
      avrc_response.set_app_val.status = BTIF_RC_STS_TIMEOUT;
      handle_set_app_attr_val_response(&meta_msg, &avrc_response.set_app_val);
      break;

    case AVRC_PDU_PLAY_ITEM:
      // Nothing to notify on, just release the label
      break;

    default:
      log::warn("timeout for unknown pdu_id={}", p_context->pdu_id);
      break;
  }
}

/***************************************************************************
 *
 * Function         browse_cmd_timeout_handler
 *
 * Description      Browse command timeout handler
 * Returns          None
 *
 **************************************************************************/
static void browse_cmd_timeout_handler(btif_rc_device_cb_t* p_dev, uint8_t label,
                                       rc_browse_context_t* p_context) {
  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  tAVRC_RESPONSE avrc_response = {0};
  tBTA_AV_META_MSG meta_msg = {
          .rc_handle = p_dev->rc_handle,
          .len = 0,
          .label = 0,
          .code = 0,
          .company_id = 0,
          .p_data = nullptr,
          .p_msg = nullptr,
  };

  log::warn("timeout, addr={}, label={}, pdu_id={}", p_dev->rc_addr, label,
            dump_rc_pdu(p_context->pdu_id));

  switch (p_context->pdu_id) {
    case AVRC_PDU_GET_FOLDER_ITEMS:
      avrc_response.get_items.status = BTIF_RC_STS_TIMEOUT;
      handle_get_folder_items_response(&meta_msg, &avrc_response.get_items);
      break;
    case AVRC_PDU_CHANGE_PATH:
      avrc_response.chg_path.status = BTIF_RC_STS_TIMEOUT;
      handle_change_path_response(&meta_msg, &avrc_response.chg_path);
      break;
    case AVRC_PDU_SET_BROWSED_PLAYER:
      avrc_response.br_player.status = BTIF_RC_STS_TIMEOUT;
      handle_set_browsed_player_response(&meta_msg, &avrc_response.br_player);
      break;
    case AVRC_PDU_GET_ITEM_ATTRIBUTES:
      avrc_response.get_attrs.status = BTIF_RC_STS_TIMEOUT;
      handle_get_metadata_attr_response(&meta_msg, &avrc_response.get_attrs);
      break;
    default:
      log::warn("timeout for unknown pdu_id={}", p_context->pdu_id);
      break;
  }
}

/***************************************************************************
 *
 * Function         passthru_cmd_timeout_handler
 *
 * Description      Pass-thru command timeout handler
 * Returns          None
 *
 **************************************************************************/
static void passthru_cmd_timeout_handler(btif_rc_device_cb_t* p_dev, uint8_t label,
                                         rc_passthru_context_t* p_context) {
  if (p_dev == NULL) {
    log::error("p_dev NULL");
    return;
  }

  log::warn("timeout, addr={}, label={}, rc_id={}, key_state={}", p_dev->rc_addr, label,
            p_context->rc_id, p_context->key_state);

  // Other requests are wrapped in a tAVRC_RESPONSE response object, but these
  // passthru events are not in there. As well, the upper layers don't handle
  // these events anyways. If that were to change, we could check the rc_id and
  // choose to route either the passthrough handler or vendorunique handler here
  return;
}

/***************************************************************************
 *
 * Function         btif_rc_transaction_timeout_handler
 *
 * Description      RC transaction timeout handler (Runs in BTIF context).
 * Returns          None
 *
 **************************************************************************/
static void btif_rc_transaction_timeout_handler(uint16_t /* event */, char* data) {
  rc_transaction_context_t* p_context = (rc_transaction_context_t*)data;
  if (p_context == nullptr) {
    log::error("p_context is null");
    return;
  }

  btif_rc_device_cb_t* p_dev = btif_rc_get_device_by_bda(p_context->rc_addr);
  if (p_dev == NULL) {
    log::error("p_dev is null");
    return;
  }

  uint8_t label = p_context->label;
  switch (p_context->opcode) {
    case AVRC_OP_VENDOR:
      vendor_cmd_timeout_handler(p_dev, label, &(p_context->command.vendor));
      break;
    case AVRC_OP_BROWSE:
      browse_cmd_timeout_handler(p_dev, label, &(p_context->command.browse));
      break;
    case AVRC_OP_PASS_THRU:
      passthru_cmd_timeout_handler(p_dev, label, &(p_context->command.passthru));
      break;
    default:
      log::warn("received timeout for unknown opcode={}", p_context->opcode);
      return;
  }
  release_transaction(p_dev, label);
}

/***************************************************************************
 *
 * Function         btif_rc_transaction_timer_timeout
 *
 * Description      RC transaction timeout callback.
 *                  This is called from BTU context and switches to BTIF
 *                  context to handle the timeout events
 * Returns          None
 *
 **************************************************************************/
static void btif_rc_transaction_timer_timeout(void* data) {
  rc_transaction_context_t* p_data = (rc_transaction_context_t*)data;

  btif_transfer_context(btif_rc_transaction_timeout_handler, 0, (char*)p_data,
                        sizeof(rc_transaction_context_t), NULL);
}

/*******************************************************************************
 *      Function       sleep_ms
 *
 *      Description    Sleep the calling thread unconditionally for
 *                     |timeout_ms| milliseconds.
 *
 *      Returns        void
 ******************************************************************************/
static void sleep_ms(uint64_t timeout_ms) {
  struct timespec delay;
  delay.tv_sec = timeout_ms / 1000;
  delay.tv_nsec = 1000 * 1000 * (timeout_ms % 1000);

  OSI_NO_INTR(nanosleep(&delay, &delay));
}

/*******************************************************************************
 *      Function       btif_debug_rc_dump
 *
 *      Description    Dumps the state of the btif_rc subsytem
 *
 *      Returns        void
 ******************************************************************************/
void btif_debug_rc_dump(int fd) {
  dprintf(fd, "\nAVRCP Native State:\n");

  int connected_count = 0;
  for (int i = 0; i < BTIF_RC_NUM_CONN; ++i) {
    const btrc_connection_state_t state = btif_rc_cb.rc_multi_cb[i].rc_state;
    if (state != BTRC_CONNECTION_STATE_DISCONNECTED) {
      ++connected_count;
    }
  }

  dprintf(fd, "  Devices (%d / %d):\n", connected_count, BTIF_RC_NUM_CONN - 1);
  for (int i = 0; i < BTIF_RC_NUM_CONN; ++i) {
    btif_rc_device_cb_t* p_dev = &btif_rc_cb.rc_multi_cb[i];
    if (p_dev->rc_state != BTRC_CONNECTION_STATE_DISCONNECTED) {
      dprintf(fd, "    %s:\n", ADDRESS_TO_LOGGABLE_CSTR(p_dev->rc_addr));

      rc_transaction_set_t* transaction_set = &(p_dev->transaction_set);
      std::unique_lock<std::recursive_mutex> lock(transaction_set->label_lock);
      dprintf(fd, "      Transaction Labels:\n");
      for (auto j = 0; j < MAX_TRANSACTIONS_PER_SESSION; ++j) {
        dprintf(fd, "        %s\n", dump_transaction(&transaction_set->transaction[j]).c_str());
      }
    }
  }
}

static bool absolute_volume_disabled() {
  char volume_disabled[PROPERTY_VALUE_MAX] = {0};
  osi_property_get("persist.bluetooth.disableabsvol", volume_disabled, "false");
  if (strncmp(volume_disabled, "true", 4) == 0) {
    log::warn("Absolute volume disabled by property");
    return true;
  }
  return false;
}
