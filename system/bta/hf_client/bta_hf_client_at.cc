/******************************************************************************
 *
 *  Copyright (c) 2014 The Android Open Source Project
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

#define LOG_TAG "bt_hf_client"

#include <bluetooth/log.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "bta/hf_client/bta_hf_client_int.h"
#include "bta_hf_client_api.h"
#include "bta_hfp_api.h"
#include "bta_sys.h"
#include "btm_api_types.h"
#include "os/logging/log_adapter.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/compat.h"
#include "osi/include/properties.h"
#include "power_mode.h"
#include "stack/include/acl_api.h"
#include "stack/include/port_api.h"

/* Uncomment to enable AT traffic dumping */
/* #define BTA_HF_CLIENT_AT_DUMP 1 */

/* minimum length of AT event */
#define BTA_HF_CLIENT_AT_EVENT_MIN_LEN 3

/* timeout (in milliseconds) for AT response */
#define BTA_HF_CLIENT_AT_TIMEOUT 29989

/* timeout (in milliseconds) for AT hold timer */
#define BTA_HF_CLIENT_AT_HOLD_TIMEOUT 41

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using namespace bluetooth;

static constexpr char kPropertyEnhancedDrivingIndicatorEnabled[] =
        "bluetooth.headset_client.indicator.enhanced_driver_safety.enabled";

/******************************************************************************
 *       SUPPORTED EVENT MESSAGES
 ******************************************************************************/

/* CIND: supported indicator names */
#define BTA_HF_CLIENT_INDICATOR_BATTERYCHG "battchg"
#define BTA_HF_CLIENT_INDICATOR_SIGNAL "signal"
#define BTA_HF_CLIENT_INDICATOR_SERVICE "service"
#define BTA_HF_CLIENT_INDICATOR_CALL "call"
#define BTA_HF_CLIENT_INDICATOR_ROAM "roam"
#define BTA_HF_CLIENT_INDICATOR_CALLSETUP "callsetup"
#define BTA_HF_CLIENT_INDICATOR_CALLHELD "callheld"

/* BIND parse mode */
#define BTA_HF_CLIENT_BIND_PARSE_READ_ENABLED_IND 0
#define BTA_HF_CLIENT_BIND_PARSE_READ_SUPPOETED_IND 1

#define MIN(a, b)           \
  ({                        \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    (_a < _b) ? _a : _b;    \
  })

/* CIND: represents each indicators boundaries */
typedef struct {
  const char* name;
  uint8_t min;
  uint8_t max;
  uint8_t namelen;
} tBTA_HF_CLIENT_INDICATOR;

#define BTA_HF_CLIENT_AT_SUPPORTED_INDICATOR_COUNT 7

/* CIND: storage room for indicators value range and their statuses */
static const tBTA_HF_CLIENT_INDICATOR
        bta_hf_client_indicators[BTA_HF_CLIENT_AT_SUPPORTED_INDICATOR_COUNT] = {
                /* name                                | min | max | name length -
                   used by parser */
                {BTA_HF_CLIENT_INDICATOR_BATTERYCHG, 0, 5,
                 sizeof(BTA_HF_CLIENT_INDICATOR_BATTERYCHG)},
                {BTA_HF_CLIENT_INDICATOR_SIGNAL, 0, 5, sizeof(BTA_HF_CLIENT_INDICATOR_SIGNAL)},
                {BTA_HF_CLIENT_INDICATOR_SERVICE, 0, 1, sizeof(BTA_HF_CLIENT_INDICATOR_SERVICE)},
                {BTA_HF_CLIENT_INDICATOR_CALL, 0, 1, sizeof(BTA_HF_CLIENT_INDICATOR_CALL)},
                {BTA_HF_CLIENT_INDICATOR_ROAM, 0, 1, sizeof(BTA_HF_CLIENT_INDICATOR_ROAM)},
                {BTA_HF_CLIENT_INDICATOR_CALLSETUP, 0, 3,
                 sizeof(BTA_HF_CLIENT_INDICATOR_CALLSETUP)},
                {BTA_HF_CLIENT_INDICATOR_CALLHELD, 0, 2, sizeof(BTA_HF_CLIENT_INDICATOR_CALLHELD)}};

/* +VGM/+VGS - gain min/max values  */
#define BTA_HF_CLIENT_VGS_MIN 0
#define BTA_HF_CLIENT_VGS_MAX 15
#define BTA_HF_CLIENT_VGM_MIN 0
#define BTA_HF_CLIENT_VGM_MAX 15

uint32_t service_index = 0;
bool service_availability = true;
/* helper functions for handling AT commands queueing */

static void bta_hf_client_handle_ok(tBTA_HF_CLIENT_CB* client_cb);

static void bta_hf_client_clear_queued_at(tBTA_HF_CLIENT_CB* client_cb) {
  tBTA_HF_CLIENT_AT_QCMD* cur = client_cb->at_cb.queued_cmd;
  tBTA_HF_CLIENT_AT_QCMD* next;

  while (cur != NULL) {
    next = cur->next;
    osi_free(cur);
    cur = next;
  }

  client_cb->at_cb.queued_cmd = NULL;
}

static void bta_hf_client_queue_at(tBTA_HF_CLIENT_CB* client_cb, tBTA_HF_CLIENT_AT_CMD cmd,
                                   const char* buf, uint16_t buf_len) {
  tBTA_HF_CLIENT_AT_QCMD* new_cmd =
          (tBTA_HF_CLIENT_AT_QCMD*)osi_malloc(sizeof(tBTA_HF_CLIENT_AT_QCMD));

  log::verbose("cmd:{}", (int)cmd);

  new_cmd->cmd = cmd;
  new_cmd->buf_len = buf_len;
  new_cmd->next = NULL;
  memcpy(new_cmd->buf, buf, buf_len);

  if (client_cb->at_cb.queued_cmd != NULL) {
    tBTA_HF_CLIENT_AT_QCMD* qcmd = client_cb->at_cb.queued_cmd;

    while (qcmd->next != NULL) {
      qcmd = qcmd->next;
    }

    qcmd->next = new_cmd;
  } else {
    client_cb->at_cb.queued_cmd = new_cmd;
  }
}

static void bta_hf_client_at_resp_timer_cback(void* data) {
  tBTA_HF_CLIENT_CB* client_cb = (tBTA_HF_CLIENT_CB*)data;
  if (client_cb->at_cb.current_cmd == BTA_HF_CLIENT_AT_CNUM) {
    log::info("timed out waiting for AT+CNUM response; spoofing OK.");
    bta_hf_client_handle_ok(client_cb);
  } else {
    log::error("HFPClient: AT response timeout, disconnecting");

    tBTA_HF_CLIENT_DATA msg = {};
    msg.hdr.layer_specific = client_cb->handle;
    bta_hf_client_sm_execute(BTA_HF_CLIENT_API_CLOSE_EVT, &msg);
  }
}

static void bta_hf_client_start_at_resp_timer(tBTA_HF_CLIENT_CB* client_cb) {
  alarm_set_on_mloop(client_cb->at_cb.resp_timer, BTA_HF_CLIENT_AT_TIMEOUT,
                     bta_hf_client_at_resp_timer_cback, (void*)client_cb);
}

static void bta_hf_client_stop_at_resp_timer(tBTA_HF_CLIENT_CB* client_cb) {
  alarm_cancel(client_cb->at_cb.resp_timer);
}

static void bta_hf_client_send_at(tBTA_HF_CLIENT_CB* client_cb, tBTA_HF_CLIENT_AT_CMD cmd,
                                  const char* buf, uint16_t buf_len) {
  log::verbose("{}", cmd);
  if ((client_cb->at_cb.current_cmd == BTA_HF_CLIENT_AT_NONE || !client_cb->svc_conn) &&
      !alarm_is_scheduled(client_cb->at_cb.hold_timer)) {
    uint16_t len;

#ifdef BTA_HF_CLIENT_AT_DUMP
    log::verbose("{:.{}}", buf, buf_len - 1);
#endif

    client_cb->at_cb.current_cmd = cmd;
    /* Generate fake responses for these because they won't reliably work */
    if (!service_availability && (cmd == BTA_HF_CLIENT_AT_CNUM || cmd == BTA_HF_CLIENT_AT_COPS)) {
      log::warn("No service, skipping {} command", cmd);
      bta_hf_client_handle_ok(client_cb);
      return;
    }

    log::verbose("writing port data to {}", client_cb->conn_handle);
    if (PORT_WriteData(client_cb->conn_handle, buf, buf_len, &len) != PORT_SUCCESS) {
      log::warn("Unable to write RFCOMM data peer:{} handle:{} len:{}", client_cb->peer_addr,
                client_cb->conn_handle, buf_len);
    };

    bta_hf_client_start_at_resp_timer(client_cb);

    return;
  }

  log::verbose("busy! queued: {}", cmd);
  bta_hf_client_queue_at(client_cb, cmd, buf, buf_len);
}

static void bta_hf_client_send_queued_at(tBTA_HF_CLIENT_CB* client_cb) {
  tBTA_HF_CLIENT_AT_QCMD* cur = client_cb->at_cb.queued_cmd;

  log::verbose("");

  if (cur != NULL) {
    client_cb->at_cb.queued_cmd = cur->next;

    bta_hf_client_send_at(client_cb, cur->cmd, cur->buf, cur->buf_len);

    osi_free(cur);
  }
}

static void bta_hf_client_at_hold_timer_cback(void* data) {
  tBTA_HF_CLIENT_CB* client_cb = (tBTA_HF_CLIENT_CB*)data;
  log::verbose("");
  bta_hf_client_send_queued_at(client_cb);
}

static void bta_hf_client_stop_at_hold_timer(tBTA_HF_CLIENT_CB* client_cb) {
  log::verbose("");
  alarm_cancel(client_cb->at_cb.hold_timer);
}

static void bta_hf_client_start_at_hold_timer(tBTA_HF_CLIENT_CB* client_cb) {
  log::verbose("");
  alarm_set_on_mloop(client_cb->at_cb.hold_timer, BTA_HF_CLIENT_AT_HOLD_TIMEOUT,
                     bta_hf_client_at_hold_timer_cback, (void*)client_cb);
}

/******************************************************************************
 *
 *          COMMON AT EVENT HANDLING funcS
 *
 *   Receives data (strings, ints, etc.) from the parser and processes this
 *   data. No buffer parsing is being done here.
 ******************************************************************************/

static void bta_hf_client_handle_ok(tBTA_HF_CLIENT_CB* client_cb) {
  log::verbose("current_cmd:{}", client_cb->at_cb.current_cmd);

  bta_hf_client_stop_at_resp_timer(client_cb);

  if (!client_cb->svc_conn) {
    bta_hf_client_slc_seq(client_cb, false);
    return;
  }

  switch (client_cb->at_cb.current_cmd) {
    case BTA_HF_CLIENT_AT_BIA:
    case BTA_HF_CLIENT_AT_BCC:
    case BTA_HF_CLIENT_AT_BIEV:
      break;
    case BTA_HF_CLIENT_AT_BCS:
      bta_hf_client_start_at_hold_timer(client_cb);
      client_cb->at_cb.current_cmd = BTA_HF_CLIENT_AT_NONE;
      return;
    case BTA_HF_CLIENT_AT_CLIP:  // last cmd is post slc seq
      if (!client_cb->send_at_reply) {
        client_cb->send_at_reply = true;
      }
      break;
    case BTA_HF_CLIENT_AT_NONE:
      bta_hf_client_stop_at_hold_timer(client_cb);
      break;
    case BTA_HF_CLIENT_AT_ANDROID:
      bta_hf_client_at_result(client_cb, BTA_HF_CLIENT_AT_RESULT_OK, 0);
      break;
    default:
      if (client_cb->send_at_reply) {
        bta_hf_client_at_result(client_cb, BTA_HF_CLIENT_AT_RESULT_OK, 0);
      }
      break;
  }

  client_cb->at_cb.current_cmd = BTA_HF_CLIENT_AT_NONE;

  bta_hf_client_send_queued_at(client_cb);
}

static void bta_hf_client_handle_error(tBTA_HF_CLIENT_CB* client_cb,
                                       tBTA_HF_CLIENT_AT_RESULT_TYPE type, uint16_t cme) {
  log::verbose("type:{} cme:{} current_cmd:{}", type, cme, client_cb->at_cb.current_cmd);

  bta_hf_client_stop_at_resp_timer(client_cb);

  if (!client_cb->svc_conn) {
    bta_hf_client_slc_seq(client_cb, true);
    return;
  }

  switch (client_cb->at_cb.current_cmd) {
    case BTA_HF_CLIENT_AT_BIA:
      break;
    case BTA_HF_CLIENT_AT_BCC:
    case BTA_HF_CLIENT_AT_BCS:
      bta_hf_client_cback_sco(client_cb, BTA_HF_CLIENT_AUDIO_CLOSE_EVT);
      break;
    case BTA_HF_CLIENT_AT_CLIP:  // last cmd is post slc seq
      if (!client_cb->send_at_reply) {
        client_cb->send_at_reply = true;
      }
      break;
    case BTA_HF_CLIENT_AT_ANDROID:
      bta_hf_client_at_result(client_cb, type, cme);
      break;
    default:
      if (client_cb->send_at_reply) {
        bta_hf_client_at_result(client_cb, type, cme);
      }
      break;
  }

  client_cb->at_cb.current_cmd = BTA_HF_CLIENT_AT_NONE;

  bta_hf_client_send_queued_at(client_cb);
}

static void bta_hf_client_handle_ring(tBTA_HF_CLIENT_CB* client_cb) {
  log::verbose("");

  const bool exit_sniff_while_ring =
          osi_property_get_bool("bluetooth.headset_client.exit_sniff_while_ring", false);

  // Invoke mode change to active mode if feature flag is enabled and current
  // status is sniff
  if (exit_sniff_while_ring) {
    tBTM_PM_MODE mode;
    if (BTM_ReadPowerMode(client_cb->peer_addr, &mode) && mode == BTM_PM_STS_SNIFF) {
      bta_sys_busy(BTA_ID_HS, 1, client_cb->peer_addr);
    }
  }
  bta_hf_client_evt_val(client_cb, BTA_HF_CLIENT_RING_INDICATION, 0);
}

static void bta_hf_client_handle_brsf(tBTA_HF_CLIENT_CB* client_cb, uint32_t value) {
  log::verbose("0x{:x}", value);
  client_cb->peer_features = value;
}

/* handles a single indicator descriptor - registers it for value changing
 * events */
static void bta_hf_client_handle_cind_list_item(tBTA_HF_CLIENT_CB* client_cb, char* name,
                                                uint32_t min, uint32_t max, uint32_t index) {
  uint8_t i = 0;

  log::verbose("{} .{} <{}:{}>", index, name, min, max);

  if (index >= BTA_HF_CLIENT_AT_INDICATOR_COUNT) {
    return;
  }

  /* look for a matching indicator on list of supported ones */
  for (i = 0; i < BTA_HF_CLIENT_AT_SUPPORTED_INDICATOR_COUNT; i++) {
    if (strcmp(name, BTA_HF_CLIENT_INDICATOR_SERVICE) == 0) {
      service_index = index;
    }
    /* look for a match - search one sign further than indicators name to check
     * for string end */
    /* It will distinguish 'callheld' which could be matched by strncmp as
     * 'call'.               */
    if (strncmp(name, bta_hf_client_indicators[i].name, bta_hf_client_indicators[i].namelen) != 0) {
      continue;
    }

    /* index - enumerates value position in the incoming sequence */
    /* if name matches one of the known indicators, add its incoming position */
    /* to lookup table for easy value->indicator matching later, when only
     * values come  */
    client_cb->at_cb.indicator_lookup[index] = i;

    return;
  }
}

static void bta_hf_client_handle_cind_value(tBTA_HF_CLIENT_CB* client_cb, uint32_t index,
                                            uint32_t value) {
  log::verbose("index: {} value: {}", index, value);

  if (index >= BTA_HF_CLIENT_AT_INDICATOR_COUNT) {
    return;
  }

  if (service_index == index) {
    if (value == 0) {
      service_availability = false;
    } else {
      service_availability = true;
    }
  }
  if (client_cb->at_cb.indicator_lookup[index] == -1) {
    return;
  }

  /* get the real array index from lookup table */
  index = client_cb->at_cb.indicator_lookup[index];

  /* Ignore out of range values */
  if (value > bta_hf_client_indicators[index].max || value < bta_hf_client_indicators[index].min) {
    return;
  }

  /* tBTA_HF_CLIENT_IND_TYPE match index in bta_hf_client_indicators */
  bta_hf_client_ind(client_cb, index, value);
}

static void bta_hf_client_handle_chld(tBTA_HF_CLIENT_CB* client_cb, uint32_t mask) {
  log::verbose("0x{:x}", mask);

  client_cb->chld_features |= mask;
}

static void bta_hf_client_handle_bind_read_supported_ind(tBTA_HF_CLIENT_CB* client_cb,
                                                         int indicator_id) {
  log::verbose("{}", indicator_id);

  client_cb->peer_hf_indicators.insert(indicator_id);
}

static void bta_hf_client_handle_bind_read_enabled_ind(tBTA_HF_CLIENT_CB* client_cb,
                                                       int indicator_id, bool enable) {
  log::verbose("{}", indicator_id);

  if (enable) {
    client_cb->enabled_hf_indicators.insert(indicator_id);
  } else {
    client_cb->enabled_hf_indicators.erase(indicator_id);
  }
}

static void bta_hf_client_handle_ciev(tBTA_HF_CLIENT_CB* client_cb, uint32_t index,
                                      uint32_t value) {
  int8_t realind = -1;

  log::verbose("index: {} value: {}", index, value);

  if (index == 0 || index > BTA_HF_CLIENT_AT_INDICATOR_COUNT) {
    return;
  }

  if (service_index == index - 1) {
    service_availability = value == 0 ? false : true;
  }

  realind = client_cb->at_cb.indicator_lookup[index - 1];

  if (realind >= 0 && realind < BTA_HF_CLIENT_AT_SUPPORTED_INDICATOR_COUNT) {
    /* get the real in-array index from lookup table by index it comes at */
    /* if there is no bug it should automatically be correctly calculated    */
    if (value > bta_hf_client_indicators[realind].max ||
        value < bta_hf_client_indicators[realind].min) {
      return;
    }

    /* update service availability on +ciev from AG. */
    if (service_index == (index - 1)) {
      if (value == 1) {
        service_availability = true;
      } else {
        service_availability = false;
      }
    }

    /* tBTA_HF_CLIENT_IND_TYPE match index in bta_hf_client_indicators */
    bta_hf_client_ind(client_cb, realind, value);
  }
}

static void bta_hf_client_handle_bcs(tBTA_HF_CLIENT_CB* client_cb, uint32_t codec) {
  tBTA_AG_UUID_CODEC uuid_codec = static_cast<tBTA_AG_UUID_CODEC>(codec);
  log::verbose("codec: {} sco listen state: {}", codec, client_cb->sco_state);
  if (uuid_codec == tBTA_AG_UUID_CODEC::UUID_CODEC_CVSD ||
      uuid_codec == tBTA_AG_UUID_CODEC::UUID_CODEC_MSBC ||
      (bta_hf_client_cb_arr.is_support_lc3 && uuid_codec == tBTA_AG_UUID_CODEC::UUID_CODEC_LC3)) {
    switch (uuid_codec) {
      case tBTA_AG_UUID_CODEC::UUID_CODEC_CVSD:
        client_cb->negotiated_codec = BTM_SCO_CODEC_CVSD;
        break;
      case tBTA_AG_UUID_CODEC::UUID_CODEC_MSBC:
        client_cb->negotiated_codec = BTM_SCO_CODEC_MSBC;
        break;
      case tBTA_AG_UUID_CODEC::UUID_CODEC_LC3:
        client_cb->negotiated_codec = BTM_SCO_CODEC_LC3;
        break;
      default:
        client_cb->negotiated_codec = BTM_SCO_CODEC_CVSD;
        break;
    }
    bta_hf_client_send_at_bcs(client_cb, codec);
  } else {
    client_cb->negotiated_codec = BTM_SCO_CODEC_CVSD;
    bta_hf_client_send_at_bac(client_cb);
  }
}

static void bta_hf_client_handle_bsir(tBTA_HF_CLIENT_CB* client_cb, uint32_t provided) {
  log::verbose("{}", provided);

  bta_hf_client_evt_val(client_cb, BTA_HF_CLIENT_BSIR_EVT, provided);
}

static void bta_hf_client_handle_cmeerror(tBTA_HF_CLIENT_CB* client_cb, uint32_t code) {
  bta_hf_client_handle_error(client_cb, BTA_HF_CLIENT_AT_RESULT_CME, code);
}

static void bta_hf_client_handle_vgm(tBTA_HF_CLIENT_CB* client_cb, uint32_t value) {
  log::verbose("{}", value);

  if (value <= BTA_HF_CLIENT_VGM_MAX) {
    bta_hf_client_evt_val(client_cb, BTA_HF_CLIENT_MIC_EVT, value);
  }
}

static void bta_hf_client_handle_vgs(tBTA_HF_CLIENT_CB* client_cb, uint32_t value) {
  log::verbose("{}", value);

  if (value <= BTA_HF_CLIENT_VGS_MAX) {
    bta_hf_client_evt_val(client_cb, BTA_HF_CLIENT_SPK_EVT, value);
  }
}

static void bta_hf_client_handle_bvra(tBTA_HF_CLIENT_CB* client_cb, uint32_t value) {
  log::verbose("{}", value);

  if (value > 1) {
    return;
  }

  bta_hf_client_evt_val(client_cb, BTA_HF_CLIENT_VOICE_REC_EVT, value);
}

static void bta_hf_client_handle_clip(tBTA_HF_CLIENT_CB* client_cb, char* numstr, uint32_t type) {
  std::string cell_number(numstr);
  log::verbose("{} {}", type, PRIVATE_CELL(cell_number));

  bta_hf_client_clip(client_cb, numstr);
}

static void bta_hf_client_handle_ccwa(tBTA_HF_CLIENT_CB* client_cb, char* numstr, uint32_t type) {
  std::string cell_number(numstr);
  log::verbose("{} {}", type, PRIVATE_CELL(cell_number));

  bta_hf_client_ccwa(client_cb, numstr);
}

static void bta_hf_client_handle_cops(tBTA_HF_CLIENT_CB* client_cb, char* opstr, uint32_t mode) {
  log::verbose("{} {}", mode, opstr);

  bta_hf_client_operator_name(client_cb, opstr);
}

static void bta_hf_client_handle_binp(tBTA_HF_CLIENT_CB* client_cb, char* numstr) {
  std::string cell_number(numstr);
  log::verbose("{}", PRIVATE_CELL(cell_number));

  bta_hf_client_binp(client_cb, numstr);
}

static void bta_hf_client_handle_clcc(tBTA_HF_CLIENT_CB* client_cb, uint16_t idx, uint16_t dir,
                                      uint16_t status, uint16_t mode, uint16_t mpty, char* numstr,
                                      uint16_t type) {
  log::verbose("idx: {} dir: {} status: {} mode: {} mpty: {}", idx, dir, status, mode, mpty);

  if (numstr) {
    std::string cell_number(numstr);
    log::verbose("number: {}  type: {}", PRIVATE_CELL(cell_number), type);
  }

  bta_hf_client_clcc(client_cb, idx, dir, status, mpty, numstr);
}

static void bta_hf_client_handle_cnum(tBTA_HF_CLIENT_CB* client_cb, char* numstr, uint16_t type,
                                      uint16_t service) {
  std::string cell_number(numstr);
  log::verbose("number: {} type: {} service: {}", PRIVATE_CELL(cell_number), type, service);

  /* TODO: should number be modified according to type? */
  bta_hf_client_cnum(client_cb, numstr, service);
}

static void bta_hf_client_handle_btrh(tBTA_HF_CLIENT_CB* client_cb, uint16_t code) {
  log::verbose("{}", code);

  bta_hf_client_evt_val(client_cb, BTA_HF_CLIENT_BTRH_EVT, code);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_cback_ind
 *
 * Description      Send indicator callback event to application.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_ind(tBTA_HF_CLIENT_CB* client_cb, tBTA_HF_CLIENT_IND_TYPE type, uint16_t value) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  evt.ind.type = type;
  evt.ind.value = value;

  evt.ind.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_IND_EVT, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_evt_val
 *
 * Description      Send event to application.
 *                  This is a generic helper for events with common data.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_evt_val(tBTA_HF_CLIENT_CB* client_cb, tBTA_HF_CLIENT_EVT type, uint16_t value) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  evt.val.bd_addr = client_cb->peer_addr;
  evt.val.value = value;

  bta_hf_client_app_callback(type, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_operator_name
 *
 * Description      Send operator name event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_operator_name(tBTA_HF_CLIENT_CB* client_cb, char* name) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  osi_strlcpy(evt.operator_name.name, name, BTA_HF_CLIENT_OPERATOR_NAME_LEN + 1);
  evt.operator_name.name[BTA_HF_CLIENT_OPERATOR_NAME_LEN] = '\0';

  evt.operator_name.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_OPERATOR_NAME_EVT, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_clip
 *
 * Description      Send CLIP event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_clip(tBTA_HF_CLIENT_CB* client_cb, char* number) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  osi_strlcpy(evt.number.number, number, BTA_HF_CLIENT_NUMBER_LEN + 1);
  evt.number.number[BTA_HF_CLIENT_NUMBER_LEN] = '\0';

  evt.number.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_CLIP_EVT, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_ccwa
 *
 * Description      Send CLIP event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_ccwa(tBTA_HF_CLIENT_CB* client_cb, char* number) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  osi_strlcpy(evt.number.number, number, BTA_HF_CLIENT_NUMBER_LEN + 1);
  evt.number.number[BTA_HF_CLIENT_NUMBER_LEN] = '\0';

  evt.number.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_CCWA_EVT, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_at_result
 *
 * Description      Send AT result event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_at_result(tBTA_HF_CLIENT_CB* client_cb, tBTA_HF_CLIENT_AT_RESULT_TYPE type,
                             uint16_t cme) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  evt.result.type = type;
  evt.result.cme = cme;

  evt.result.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_AT_RESULT_EVT, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_clcc
 *
 * Description      Send clcc event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_clcc(tBTA_HF_CLIENT_CB* client_cb, uint32_t idx, bool incoming, uint8_t status,
                        bool mpty, char* number) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  evt.clcc.idx = idx;
  evt.clcc.inc = incoming;
  evt.clcc.status = status;
  evt.clcc.mpty = mpty;

  if (number) {
    evt.clcc.number_present = true;
    osi_strlcpy(evt.clcc.number, number, BTA_HF_CLIENT_NUMBER_LEN + 1);
    evt.clcc.number[BTA_HF_CLIENT_NUMBER_LEN] = '\0';
  }

  evt.clcc.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_CLCC_EVT, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_cnum
 *
 * Description      Send cnum event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_cnum(tBTA_HF_CLIENT_CB* client_cb, char* number, uint16_t service) {
  tBTA_HF_CLIENT evt = {};

  evt.cnum.service = service;
  osi_strlcpy(evt.cnum.number, number, BTA_HF_CLIENT_NUMBER_LEN + 1);
  evt.cnum.number[BTA_HF_CLIENT_NUMBER_LEN] = '\0';

  evt.cnum.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_CNUM_EVT, &evt);
}

void bta_hf_client_unknown_response(tBTA_HF_CLIENT_CB* client_cb, const char* evt_buffer) {
  tBTA_HF_CLIENT evt = {};

  osi_strlcpy(evt.unknown.event_string, evt_buffer, BTA_HF_CLIENT_UNKNOWN_EVENT_LEN + 1);
  evt.unknown.event_string[BTA_HF_CLIENT_UNKNOWN_EVENT_LEN] = '\0';

  evt.unknown.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_UNKNOWN_EVT, &evt);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_binp
 *
 * Description      Send BINP event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_binp(tBTA_HF_CLIENT_CB* client_cb, char* number) {
  tBTA_HF_CLIENT evt;

  memset(&evt, 0, sizeof(evt));

  osi_strlcpy(evt.number.number, number, BTA_HF_CLIENT_NUMBER_LEN + 1);
  evt.number.number[BTA_HF_CLIENT_NUMBER_LEN] = '\0';

  evt.number.bd_addr = client_cb->peer_addr;
  bta_hf_client_app_callback(BTA_HF_CLIENT_BINP_EVT, &evt);
}

/******************************************************************************
 *
 *          COMMON AT EVENTS PARSING FUNCTIONS
 *
 ******************************************************************************/

/* Check if prefix match and skip spaces if any */
#define AT_CHECK_EVENT(buf, event)                                 \
  do {                                                             \
    if (strncmp("\r\n" event, buf, sizeof("\r\n" event) - 1) != 0) \
      return buf;                                                  \
    (buf) += sizeof("\r\n" event) - 1;                             \
    while (*(buf) == ' ')                                          \
      (buf)++;                                                     \
  } while (0)

/* check for <cr><lf> and forward buffer if match */
#define AT_CHECK_RN(buf)                                 \
  do {                                                   \
    if (strncmp("\r\n", buf, sizeof("\r\n") - 1) != 0) { \
      log::verbose("missing end <cr><lf>");              \
      return NULL;                                       \
    }                                                    \
    (buf) += sizeof("\r\n") - 1;                         \
  } while (0)

/* skip rest of AT string up to <cr> */
#define AT_SKIP_REST(buf)                    \
  do {                                       \
    while (*(buf) != '\r' && *(buf) != '\0') \
      (buf)++;                               \
  } while (0)

static char* bta_hf_client_parse_ok(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "OK");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ok(client_cb);

  return buffer;
}

static char* bta_hf_client_parse_error(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "ERROR");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_error(client_cb, BTA_HF_CLIENT_AT_RESULT_ERROR, 0);

  return buffer;
}

static char* bta_hf_client_parse_ring(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "RING");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ring(client_cb);

  return buffer;
}

/* generic uint32 parser */
static char* bta_hf_client_parse_uint32(tBTA_HF_CLIENT_CB* client_cb, char* buffer,
                                        void (*handler_callback)(tBTA_HF_CLIENT_CB*, uint32_t)) {
  uint32_t value;
  int res;
  int offset;

  res = sscanf(buffer, "%u%n", &value, &offset);
  if (res < 1) {
    return NULL;
  }

  buffer += offset;

  AT_CHECK_RN(buffer);

  handler_callback(client_cb, value);
  return buffer;
}

static char* bta_hf_client_parse_brsf(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+BRSF:");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_brsf);
}

static char* bta_hf_client_parse_cind_values(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  /* value and its position */
  uint16_t index = 0;
  uint32_t value = 0;

  int offset;
  int res;

  while ((res = sscanf(buffer, "%u%n", &value, &offset)) > 0) {
    /* decides if its valid index and value, if yes stores it */
    bta_hf_client_handle_cind_value(client_cb, index, value);

    buffer += offset;

    /* check if more values are present */
    if (*buffer != ',') {
      break;
    }

    index++;
    buffer++;
  }

  if (res > 0) {
    AT_CHECK_RN(buffer);
    return buffer;
  }

  return NULL;
}

static char* bta_hf_client_parse_cind_list(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  int offset = 0;
  char name[129];
  uint32_t min, max;
  uint32_t index = 0;
  int res;

  while ((res = sscanf(buffer, "(\"%128[^\"]\",(%u%*[-,]%u))%n", name, &min, &max, &offset)) > 2) {
    bta_hf_client_handle_cind_list_item(client_cb, name, min, max, index);
    if (offset == 0) {
      log::error("Format Error {}", buffer);
      return NULL;
    }

    buffer += offset;
    index++;

    if (*buffer != ',') {
      break;
    }

    buffer++;
  }

  if (res > 2) {
    AT_CHECK_RN(buffer);
    return buffer;
  }

  return NULL;
}

static char* bta_hf_client_parse_cind(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+CIND:");

  if (*buffer == '(') {
    return bta_hf_client_parse_cind_list(client_cb, buffer);
  }

  return bta_hf_client_parse_cind_values(client_cb, buffer);
}

static char* bta_hf_client_parse_chld(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+CHLD:");

  if (*buffer != '(') {
    return NULL;
  }

  buffer++;

  while (*buffer != '\0') {
    if (strncmp("0", buffer, 1) == 0) {
      bta_hf_client_handle_chld(client_cb, BTA_HF_CLIENT_CHLD_REL);
      buffer++;
    } else if (strncmp("1x", buffer, 2) == 0) {
      bta_hf_client_handle_chld(client_cb, BTA_HF_CLIENT_CHLD_REL_X);
      buffer += 2;
    } else if (strncmp("1", buffer, 1) == 0) {
      bta_hf_client_handle_chld(client_cb, BTA_HF_CLIENT_CHLD_REL_ACC);
      buffer++;
    } else if (strncmp("2x", buffer, 2) == 0) {
      bta_hf_client_handle_chld(client_cb, BTA_HF_CLIENT_CHLD_PRIV_X);
      buffer += 2;
    } else if (strncmp("2", buffer, 1) == 0) {
      bta_hf_client_handle_chld(client_cb, BTA_HF_CLIENT_CHLD_HOLD_ACC);
      buffer++;
    } else if (strncmp("3", buffer, 1) == 0) {
      bta_hf_client_handle_chld(client_cb, BTA_HF_CLIENT_CHLD_MERGE);
      buffer++;
    } else if (strncmp("4", buffer, 1) == 0) {
      bta_hf_client_handle_chld(client_cb, BTA_HF_CLIENT_CHLD_MERGE_DETACH);
      buffer++;
    } else {
      return NULL;
    }

    if (*buffer == ',') {
      buffer++;
      continue;
    }

    if (*buffer == ')') {
      buffer++;
      break;
    }

    return NULL;
  }

  AT_CHECK_RN(buffer);

  return buffer;
}

static char* bta_hf_client_parse_bind(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+BIND:");

  uint8_t mode = BTA_HF_CLIENT_BIND_PARSE_READ_ENABLED_IND;

  int idx = -1;

  while (*buffer != 0) {
    switch (*buffer) {
      case '(':
        mode = BTA_HF_CLIENT_BIND_PARSE_READ_SUPPOETED_IND;
        break;
      case ')':
        break;
      case '0':
      case '1':
      case '2':
        if (mode == BTA_HF_CLIENT_BIND_PARSE_READ_SUPPOETED_IND) {
          // +BIND: (id0, id1, ...)
          bta_hf_client_handle_bind_read_supported_ind(client_cb, (*buffer - '0'));
        } else if (idx == -1) {
          // +BIND: [id]...
          idx = *buffer - '0';
        } else {
          // +BIND: ...[status]
          bta_hf_client_handle_bind_read_enabled_ind(client_cb, idx, *buffer - '0');
        }
        break;
      default:
        break;
    }
    buffer++;
  }

  AT_CHECK_RN(buffer);

  return buffer;
}

static char* bta_hf_client_parse_ciev(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  uint32_t index, value;
  int res;
  int offset = 0;

  AT_CHECK_EVENT(buffer, "+CIEV:");

  res = sscanf(buffer, "%u,%u%n", &index, &value, &offset);
  if (res < 2) {
    return NULL;
  }

  if (offset == 0) {
    log::error("Format Error {}", buffer);
    return NULL;
  }

  buffer += offset;

  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ciev(client_cb, index, value);
  return buffer;
}

static char* bta_hf_client_parse_bcs(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+BCS:");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_bcs);
}

static char* bta_hf_client_parse_bsir(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+BSIR:");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_bsir);
}

static char* bta_hf_client_parse_cmeerror(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+CME ERROR:");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_cmeerror);
}

static char* bta_hf_client_parse_vgm(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+VGM:");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_vgm);
}

static char* bta_hf_client_parse_vgme(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+VGM=");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_vgm);
}

static char* bta_hf_client_parse_vgs(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+VGS:");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_vgs);
}

static char* bta_hf_client_parse_vgse(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+VGS=");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_vgs);
}

static char* bta_hf_client_parse_bvra(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "+BVRA:");

  return bta_hf_client_parse_uint32(client_cb, buffer, bta_hf_client_handle_bvra);
}

static char* bta_hf_client_parse_clip(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  /* spec forces 32 chars, plus \0 here */
  char number[33];
  uint32_t type = 0;
  int res;
  int offset = 0;

  AT_CHECK_EVENT(buffer, "+CLIP:");

  /* there might be something more after %lu but HFP doesn't care */
  res = sscanf(buffer, "\"%32[^\"]\",%u%n", number, &type, &offset);
  if (res < 2) {
    return NULL;
  }

  if (offset == 0) {
    log::error("Format Error {}", buffer);
    return NULL;
  }

  buffer += offset;

  AT_SKIP_REST(buffer);

  AT_CHECK_RN(buffer);

  bta_hf_client_handle_clip(client_cb, number, type);
  return buffer;
}

/* in HFP context there is no difference between ccwa and clip */
static char* bta_hf_client_parse_ccwa(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  /* ac to spec 32 chars max, plus \0 here */
  char number[33];
  uint32_t type = 0;
  int res;
  int offset = 0;

  AT_CHECK_EVENT(buffer, "+CCWA:");

  /* there might be something more after %lu but HFP doesn't care */
  res = sscanf(buffer, "\"%32[^\"]\",%u%n", number, &type, &offset);
  if (res < 2) {
    return NULL;
  }

  if (offset == 0) {
    log::error("Format Error {}", buffer);
    return NULL;
  }

  buffer += offset;

  AT_SKIP_REST(buffer);

  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ccwa(client_cb, number, type);
  return buffer;
}

static char* bta_hf_client_parse_cops(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  uint8_t mode;
  /* spec forces 16 chars max, plus \0 here */
  char opstr[17];
  int res;
  int offset = 0;

  AT_CHECK_EVENT(buffer, "+COPS:");

  /* TODO: Not sure if operator string actually can contain escaped " char
   * inside */
  res = sscanf(buffer, "%hhi,0,\"%16[^\"]\"%n", &mode, opstr, &offset);
  if (res < 2) {
    return NULL;
  }
  /* Abort in case offset not set because of format error */
  if (offset == 0) {
    log::error("Format Error {}", buffer);
    return NULL;
  }

  buffer += offset;

  AT_SKIP_REST(buffer);

  AT_CHECK_RN(buffer);

  bta_hf_client_handle_cops(client_cb, opstr, mode);
  // check for OK Response in end
  AT_CHECK_EVENT(buffer, "OK");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ok(client_cb);

  return buffer;
}

static char* bta_hf_client_parse_binp(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  /* HFP only supports phone number as BINP data */
  /* phone number is 32 chars plus one for \0*/
  char numstr[33];
  int res;
  int offset = 0;

  AT_CHECK_EVENT(buffer, "+BINP:");

  res = sscanf(buffer, "\"%32[^\"]\"\r\n%n", numstr, &offset);
  if (res < 1) {
    return NULL;
  }

  /* Abort in case offset not set because of format error */
  if (offset == 0) {
    log::error("Format Error {}", buffer);
    return NULL;
  }

  buffer += offset;

  /* some phones might sent type as well, just skip it */
  AT_SKIP_REST(buffer);

  AT_CHECK_RN(buffer);

  bta_hf_client_handle_binp(client_cb, numstr);

  // check for OK response in end
  AT_CHECK_EVENT(buffer, "OK");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ok(client_cb);

  return buffer;
}

static char* bta_hf_client_parse_clcc(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  uint16_t idx, dir, status, mode, mpty;
  char numstr[33]; /* spec forces 32 chars, plus one for \0*/
  uint16_t type = 0;
  int res;
  int offset = 0;

  AT_CHECK_EVENT(buffer, "+CLCC:");

  res = sscanf(buffer, "%hu,%hu,%hu,%hu,%hu%n", &idx, &dir, &status, &mode, &mpty, &offset);
  if (res < 5) {
    return NULL;
  }

  /* Abort in case offset not set because of format error */
  if (offset == 0) {
    log::error("Format Error {}", buffer);
    return NULL;
  }

  buffer += offset;
  offset = 0;

  /* check optional part */
  if (*buffer == ',') {
    int res2 = sscanf(buffer, ",\"%32[^\"]\",%hu%n", numstr, &type, &offset);
    if (res2 < 0) {
      return NULL;
    }

    if (res2 == 0) {
      res2 = sscanf(buffer, ",\"\",%hu%n", &type, &offset);
      if (res2 < 0) {
        return NULL;
      }

      /* numstr is not matched in second attempt, correct this */
      res2++;
      numstr[0] = '\0';
    }

    if (res2 >= 2) {
      res += res2;
      /* Abort in case offset not set because of format error */
      if (offset == 0) {
        log::error("Format Error {}", buffer);
        return NULL;
      }

      buffer += offset;
    }
  }

  /* Skip any remaing param,as they are not defined by BT HFP spec */
  AT_SKIP_REST(buffer);
  AT_CHECK_RN(buffer);

  if (res > 6) {
    /* we also have last two optional parameters */
    bta_hf_client_handle_clcc(client_cb, idx, dir, status, mode, mpty, numstr, type);
  } else {
    /* we didn't get the last two parameters */
    bta_hf_client_handle_clcc(client_cb, idx, dir, status, mode, mpty, NULL, 0);
  }

  // check for OK response in end
  AT_CHECK_EVENT(buffer, "OK");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ok(client_cb);
  return buffer;
}

static char* bta_hf_client_parse_cnum(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  char numstr[33]; /* spec forces 32 chars, plus one for \0*/
  uint16_t type;
  uint16_t service = 0; /* 0 in case this optional parameter is not being sent */
  int res;
  int offset = 0;

  AT_CHECK_EVENT(buffer, "+CNUM:");

  res = sscanf(buffer, ",\"%32[^\"]\",%hu,,%hu%n", numstr, &type, &service, &offset);
  if (res < 0) {
    return NULL;
  }

  if (res == 0) {
    res = sscanf(buffer, ",\"\",%hu,,%hu%n", &type, &service, &offset);
    if (res < 0) {
      return NULL;
    }

    /* numstr is not matched in second attempt, correct this */
    res++;
    numstr[0] = '\0';
  }

  if (res < 3) {
    return NULL;
  }

  /* Abort in case offset not set because of format error */
  if (offset == 0) {
    log::error("Format Error {}", buffer);
    return NULL;
  }

  buffer += offset;

  AT_CHECK_RN(buffer);

  /* service is optional */
  if (res == 2) {
    bta_hf_client_handle_cnum(client_cb, numstr, type, service);
    return buffer;
  }

  if (service != 4 && service != 5) {
    return NULL;
  }

  bta_hf_client_handle_cnum(client_cb, numstr, type, service);

  // check for OK response in end
  AT_CHECK_EVENT(buffer, "OK");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_ok(client_cb);
  return buffer;
}

static char* bta_hf_client_parse_btrh(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  uint16_t code = 0;
  int res;
  int offset;

  AT_CHECK_EVENT(buffer, "+BTRH:");

  res = sscanf(buffer, "%hu%n", &code, &offset);
  if (res < 1) {
    return NULL;
  }

  buffer += offset;

  AT_CHECK_RN(buffer);

  bta_hf_client_handle_btrh(client_cb, code);
  return buffer;
}

static char* bta_hf_client_parse_busy(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "BUSY");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_error(client_cb, BTA_HF_CLIENT_AT_RESULT_BUSY, 0);

  return buffer;
}

static char* bta_hf_client_parse_delayed(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "DELAYED");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_error(client_cb, BTA_HF_CLIENT_AT_RESULT_DELAY, 0);

  return buffer;
}

static char* bta_hf_client_parse_no_carrier(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "NO CARRIER");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_error(client_cb, BTA_HF_CLIENT_AT_RESULT_NO_CARRIER, 0);

  return buffer;
}

static char* bta_hf_client_parse_no_answer(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "NO ANSWER");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_error(client_cb, BTA_HF_CLIENT_AT_RESULT_NO_ANSWER, 0);

  return buffer;
}

static char* bta_hf_client_parse_rejectlisted(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  AT_CHECK_EVENT(buffer, "REJECTLISTED");
  AT_CHECK_RN(buffer);

  bta_hf_client_handle_error(client_cb, BTA_HF_CLIENT_AT_RESULT_REJECTLISTED, 0);

  return buffer;
}

static char* bta_hf_client_skip_unknown(tBTA_HF_CLIENT_CB* /*client_cb*/, char* buffer) {
  char* start;
  char* tmp;

  tmp = strstr(buffer, "\r\n");
  if (tmp == NULL) {
    return NULL;
  }

  buffer += 2;
  start = buffer;

  tmp = strstr(buffer, "\r\n");
  if (tmp == NULL) {
    return NULL;
  }

  buffer = tmp + 2;

  log::verbose("{:.{}}", start, (int)(buffer - start - 2));

  return buffer;
}

static char* bta_hf_client_process_unknown(tBTA_HF_CLIENT_CB* client_cb, char* buffer) {
  char* start = strstr(buffer, "\r\n");
  if (start == NULL) {
    return NULL;
  }
  start += sizeof("\r\n") - 1;

  char* end = strstr(start, "\r\n");
  if (end == NULL) {
    return NULL;
  }

  int evt_size = end - start + 1;

  char tmp_buf[BTA_HF_CLIENT_UNKNOWN_EVENT_LEN];
  if (evt_size < BTA_HF_CLIENT_UNKNOWN_EVENT_LEN) {
    osi_strlcpy(tmp_buf, start, evt_size);
    bta_hf_client_unknown_response(client_cb, tmp_buf);
    AT_CHECK_RN(end);
  } else {
    log::error("exceed event buffer size. ({}, {})", evt_size, BTA_HF_CLIENT_UNKNOWN_EVENT_LEN);
  }

  log::verbose("{}", buffer);

  return end;
}

/******************************************************************************
 *       SUPPORTED EVENT MESSAGES
 ******************************************************************************/

/* returned values are as follow:
 * != NULL && != buf  : match and parsed ok
 * == NULL            : match but parse failed
 * != NULL && == buf  : no match
 */
typedef char* (*tBTA_HF_CLIENT_PARSER_CALLBACK)(tBTA_HF_CLIENT_CB*, char*);

static const tBTA_HF_CLIENT_PARSER_CALLBACK bta_hf_client_parser_cb[] = {
        bta_hf_client_parse_ok,        bta_hf_client_parse_error,
        bta_hf_client_parse_ring,      bta_hf_client_parse_brsf,
        bta_hf_client_parse_cind,      bta_hf_client_parse_ciev,
        bta_hf_client_parse_chld,      bta_hf_client_parse_bcs,
        bta_hf_client_parse_bsir,      bta_hf_client_parse_cmeerror,
        bta_hf_client_parse_vgm,       bta_hf_client_parse_vgme,
        bta_hf_client_parse_vgs,       bta_hf_client_parse_vgse,
        bta_hf_client_parse_bvra,      bta_hf_client_parse_clip,
        bta_hf_client_parse_ccwa,      bta_hf_client_parse_cops,
        bta_hf_client_parse_binp,      bta_hf_client_parse_clcc,
        bta_hf_client_parse_cnum,      bta_hf_client_parse_btrh,
        bta_hf_client_parse_bind,      bta_hf_client_parse_busy,
        bta_hf_client_parse_delayed,   bta_hf_client_parse_no_carrier,
        bta_hf_client_parse_no_answer, bta_hf_client_parse_rejectlisted,
        bta_hf_client_process_unknown};

/* calculate supported event list length */
static const uint16_t bta_hf_client_parser_cb_count =
        sizeof(bta_hf_client_parser_cb) / sizeof(bta_hf_client_parser_cb[0]);

#ifdef BTA_HF_CLIENT_AT_DUMP
static void bta_hf_client_dump_at(tBTA_HF_CLIENT_CB* client_cb) {
  char dump[(4 * BTA_HF_CLIENT_AT_PARSER_MAX_LEN) + 1];
  char *p1, *p2;

  p1 = client_cb->at_cb.buf;
  p2 = dump;

  while (*p1 != '\0') {
    if (*p1 == '\r') {
      strncpy(p2, "<cr>", 4);
      p2 += 4;
    } else if (*p1 == '\n') {
      strncpy(p2, "<lf>", 4);
      p2 += 4;
    } else {
      *p2 = *p1;
      p2++;
    }
    p1++;
  }

  *p2 = '\0';

  log::verbose("{}", dump);
}
#endif

static void bta_hf_client_at_parse_start(tBTA_HF_CLIENT_CB* client_cb) {
  char* buf = client_cb->at_cb.buf;

  log::verbose("");

#ifdef BTA_HF_CLIENT_AT_DUMP
  bta_hf_client_dump_at(client_cb);
#endif

  while (*buf != '\0') {
    int i;
    char* tmp = NULL;

    for (i = 0; i < bta_hf_client_parser_cb_count; i++) {
      tmp = bta_hf_client_parser_cb[i](client_cb, buf);
      if (tmp == NULL) {
        log::error("HFPCient: AT event/reply parsing failed, skipping");
        tmp = bta_hf_client_skip_unknown(client_cb, buf);
        break;
      }

      /* matched or unknown skipped, if unknown failed tmp is NULL so
         this is also handled */
      if (tmp != buf) {
        buf = tmp;
        break;
      }
    }

    /* could not skip unknown (received garbage?)... disconnect */
    if (tmp == NULL) {
      log::error("HFPCient: could not skip unknown AT event, disconnecting");
      bta_hf_client_at_reset(client_cb);

      tBTA_HF_CLIENT_DATA msg = {};
      msg.hdr.layer_specific = client_cb->handle;
      bta_hf_client_sm_execute(BTA_HF_CLIENT_API_CLOSE_EVT, &msg);
      return;
    }

    buf = tmp;
  }
}

static bool bta_hf_client_check_at_complete(tBTA_HF_CLIENT_CB* client_cb) {
  bool ret = false;
  tBTA_HF_CLIENT_AT_CB* at_cb = &client_cb->at_cb;

  if (at_cb->offset >= BTA_HF_CLIENT_AT_EVENT_MIN_LEN) {
    if (at_cb->buf[at_cb->offset - 2] == '\r' && at_cb->buf[at_cb->offset - 1] == '\n') {
      ret = true;
    }
  }

  log::verbose("{}", ret);

  return ret;
}

static void bta_hf_client_at_clear_buf(tBTA_HF_CLIENT_CB* client_cb) {
  memset(client_cb->at_cb.buf, 0, sizeof(client_cb->at_cb.buf));
  client_cb->at_cb.offset = 0;
}

/******************************************************************************
 *
 *          MAIN PARSING FUNCTION
 *
 *
 ******************************************************************************/
void bta_hf_client_at_parse(tBTA_HF_CLIENT_CB* client_cb, char* buf, unsigned int len) {
  log::verbose("offset: {} len: {}", client_cb->at_cb.offset, len);

  if (len + client_cb->at_cb.offset > BTA_HF_CLIENT_AT_PARSER_MAX_LEN) {
    char tmp_buff[BTA_HF_CLIENT_AT_PARSER_MAX_LEN];
    unsigned int tmp = client_cb->at_cb.offset;
    unsigned int space_left = BTA_HF_CLIENT_AT_PARSER_MAX_LEN - client_cb->at_cb.offset;

    log::verbose("overrun, trying to recover");

    /* fill up parser buffer */
    memcpy(client_cb->at_cb.buf + client_cb->at_cb.offset, buf, space_left);
    len -= space_left;
    buf += space_left;
    client_cb->at_cb.offset += space_left;

    /* find end of last complete command before proceeding */
    while (!bta_hf_client_check_at_complete(client_cb)) {
      if (client_cb->at_cb.offset == 0) {
        log::error("HFPClient: AT parser buffer overrun, disconnecting");

        bta_hf_client_at_reset(client_cb);

        tBTA_HF_CLIENT_DATA msg = {};
        msg.hdr.layer_specific = client_cb->handle;
        bta_hf_client_sm_execute(BTA_HF_CLIENT_API_CLOSE_EVT, &msg);
        return;
      }

      client_cb->at_cb.offset--;
    }

    /* cut buffer to complete AT event and keep cut data */
    tmp += space_left - client_cb->at_cb.offset;
    memcpy(tmp_buff, client_cb->at_cb.buf + client_cb->at_cb.offset, tmp);
    client_cb->at_cb.buf[client_cb->at_cb.offset] = '\0';

    /* parse */
    bta_hf_client_at_parse_start(client_cb);
    bta_hf_client_at_clear_buf(client_cb);

    /* recover cut data */
    memcpy(client_cb->at_cb.buf, tmp_buff, tmp);
    client_cb->at_cb.offset += tmp;
  }

  /* prevent buffer overflow in cases where LEN exceeds available buffer space
   */
  if (len > BTA_HF_CLIENT_AT_PARSER_MAX_LEN - client_cb->at_cb.offset) {
    return;
  }

  memcpy(client_cb->at_cb.buf + client_cb->at_cb.offset, buf, len);
  client_cb->at_cb.offset += len;

  /* If last event is complete, parsing can be started */
  if (bta_hf_client_check_at_complete(client_cb)) {
    bta_hf_client_at_parse_start(client_cb);
    bta_hf_client_at_clear_buf(client_cb);
  }
}

void bta_hf_client_send_at_brsf(tBTA_HF_CLIENT_CB* client_cb, tBTA_HF_CLIENT_FEAT features) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  at_len = snprintf(buf, sizeof(buf), "AT+BRSF=%u\r", features);
  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BRSF, buf, at_len);
}

void bta_hf_client_send_at_bac(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  if (bta_hf_client_cb_arr.is_support_lc3) {
    buf = "AT+BAC=1,2,3\r";
  } else {
    buf = "AT+BAC=1,2\r";
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BAC, buf, strlen(buf));
}

void bta_hf_client_send_at_bcs(tBTA_HF_CLIENT_CB* client_cb, uint32_t codec) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  at_len = snprintf(buf, sizeof(buf), "AT+BCS=%u\r", codec);
  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BCS, buf, at_len);
}

void bta_hf_client_send_at_cind(tBTA_HF_CLIENT_CB* client_cb, bool status) {
  const char* buf;
  tBTA_HF_CLIENT_AT_CMD cmd;

  log::verbose("");

  if (status) {
    buf = "AT+CIND?\r";
    cmd = BTA_HF_CLIENT_AT_CIND_STATUS;
  } else {
    buf = "AT+CIND=?\r";
    cmd = BTA_HF_CLIENT_AT_CIND;
  }

  bta_hf_client_send_at(client_cb, cmd, buf, strlen(buf));
}

void bta_hf_client_send_at_cmer(tBTA_HF_CLIENT_CB* client_cb, bool activate) {
  const char* buf;

  log::verbose("");

  if (activate) {
    buf = "AT+CMER=3,0,0,1\r";
  } else {
    buf = "AT+CMER=3,0,0,0\r";
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CMER, buf, strlen(buf));
}

void bta_hf_client_send_at_chld(tBTA_HF_CLIENT_CB* client_cb, char cmd, uint32_t idx) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  if (idx > 0) {
    at_len = snprintf(buf, sizeof(buf), "AT+CHLD=%c%u\r", cmd, idx);
  } else {
    at_len = snprintf(buf, sizeof(buf), "AT+CHLD=%c\r", cmd);
  }

  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CHLD, buf, at_len);
}

void bta_hf_client_send_at_bind(tBTA_HF_CLIENT_CB* client_cb, int step) {
  std::string buf;
  tBTA_HF_CLIENT_AT_CMD cmd = BTA_HF_CLIENT_AT_BIND_SET_IND;

  log::verbose("");

  switch (step) {
    case 0:  // List HF supported indicators
      if (osi_property_get_bool(kPropertyEnhancedDrivingIndicatorEnabled, false)) {
        buf = "AT+BIND=1,2\r";
      } else {
        buf = "AT+BIND=2\r";
      }
      cmd = BTA_HF_CLIENT_AT_BIND_SET_IND;
      break;
    case 1:  // Read AG supported indicators
      buf = "AT+BIND=?\r";
      cmd = BTA_HF_CLIENT_AT_BIND_READ_SUPPORTED_IND;
      break;
    case 2:  // Read AG enabled/disabled status of indicators
      buf = "AT+BIND?\r";
      cmd = BTA_HF_CLIENT_AT_BIND_READ_ENABLED_IND;
      break;
    default:
      break;
  }
  bta_hf_client_send_at(client_cb, cmd, buf.c_str(), buf.size());
}

void bta_hf_client_send_at_biev(tBTA_HF_CLIENT_CB* client_cb, int indicator_id,
                                int indicator_value) {
  char buf[32];
  tBTA_HF_CLIENT_AT_CMD cmd = BTA_HF_CLIENT_AT_BIEV;

  if ((client_cb->peer_features & BTA_HF_CLIENT_PEER_HF_IND) == 0) {
    log::error("peer does not support HF Indicators");
    return;
  }

  if (client_cb->enabled_hf_indicators.count(indicator_id) <= 0) {
    log::error("HF indicators {} is disabled", indicator_id);
    return;
  }

  log::verbose("");

  int len = sprintf(buf, "AT+BIEV=%d,%d\r", indicator_id, indicator_value);

  bta_hf_client_send_at(client_cb, cmd, buf, len);
}

void bta_hf_client_send_at_clip(tBTA_HF_CLIENT_CB* client_cb, bool activate) {
  const char* buf;

  log::verbose("");

  if (activate) {
    buf = "AT+CLIP=1\r";
  } else {
    buf = "AT+CLIP=0\r";
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CLIP, buf, strlen(buf));
}

void bta_hf_client_send_at_ccwa(tBTA_HF_CLIENT_CB* client_cb, bool activate) {
  const char* buf;

  log::verbose("");

  if (activate) {
    buf = "AT+CCWA=1\r";
  } else {
    buf = "AT+CCWA=0\r";
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CCWA, buf, strlen(buf));
}

void bta_hf_client_send_at_cmee(tBTA_HF_CLIENT_CB* client_cb, bool activate) {
  const char* buf;

  log::verbose("");

  if (activate) {
    buf = "AT+CMEE=1\r";
  } else {
    buf = "AT+CMEE=0\r";
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CMEE, buf, strlen(buf));
}

void bta_hf_client_send_at_cops(tBTA_HF_CLIENT_CB* client_cb, bool query) {
  const char* buf;

  log::verbose("");

  if (query) {
    buf = "AT+COPS?\r";
  } else {
    buf = "AT+COPS=3,0\r";
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_COPS, buf, strlen(buf));
}

void bta_hf_client_send_at_clcc(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  buf = "AT+CLCC\r";

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CLCC, buf, strlen(buf));
}

void bta_hf_client_send_at_bvra(tBTA_HF_CLIENT_CB* client_cb, bool enable) {
  const char* buf;

  log::verbose("");

  if (enable) {
    buf = "AT+BVRA=1\r";
  } else {
    buf = "AT+BVRA=0\r";
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BVRA, buf, strlen(buf));
}

void bta_hf_client_send_at_vgs(tBTA_HF_CLIENT_CB* client_cb, uint32_t volume) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  at_len = snprintf(buf, sizeof(buf), "AT+VGS=%u\r", volume);
  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_VGS, buf, at_len);
}

void bta_hf_client_send_at_vgm(tBTA_HF_CLIENT_CB* client_cb, uint32_t volume) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  at_len = snprintf(buf, sizeof(buf), "AT+VGM=%u\r", volume);
  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_VGM, buf, at_len);
}

void bta_hf_client_send_at_atd(tBTA_HF_CLIENT_CB* client_cb, char* number, uint32_t memory) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  if (number[0] != '\0') {
    at_len = snprintf(buf, sizeof(buf), "ATD%s;\r", number);
  } else {
    at_len = snprintf(buf, sizeof(buf), "ATD>%u;\r", memory);
  }

  if (at_len < 0) {
    log::error("error preparing ATD command");
    return;
  }

  at_len = MIN((size_t)at_len, sizeof(buf));

  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }
  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_ATD, buf, at_len);
}

void bta_hf_client_send_at_bldn(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  buf = "AT+BLDN\r";

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BLDN, buf, strlen(buf));
}

void bta_hf_client_send_at_ata(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  buf = "ATA\r";

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_ATA, buf, strlen(buf));
}

void bta_hf_client_send_at_chup(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  buf = "AT+CHUP\r";

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CHUP, buf, strlen(buf));
}

void bta_hf_client_send_at_btrh(tBTA_HF_CLIENT_CB* client_cb, bool query, uint32_t val) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  if (query) {
    at_len = snprintf(buf, sizeof(buf), "AT+BTRH?\r");
  } else {
    at_len = snprintf(buf, sizeof(buf), "AT+BTRH=%u\r", val);
  }

  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BTRH, buf, at_len);
}

void bta_hf_client_send_at_vts(tBTA_HF_CLIENT_CB* client_cb, char code) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  at_len = snprintf(buf, sizeof(buf), "AT+VTS=%c\r", code);

  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_VTS, buf, at_len);
}

void bta_hf_client_send_at_bcc(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  buf = "AT+BCC\r";

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BCC, buf, strlen(buf));
}

void bta_hf_client_send_at_cnum(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  buf = "AT+CNUM\r";

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_CNUM, buf, strlen(buf));
}

void bta_hf_client_send_at_nrec(tBTA_HF_CLIENT_CB* client_cb) {
  const char* buf;

  log::verbose("");

  if (!(client_cb->peer_features & BTA_HF_CLIENT_PEER_FEAT_ECNR)) {
    log::error("Remote does not support NREC.");
    return;
  }

  buf = "AT+NREC=0\r";

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_NREC, buf, strlen(buf));
}

void bta_hf_client_send_at_binp(tBTA_HF_CLIENT_CB* client_cb, uint32_t action) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  at_len = snprintf(buf, sizeof(buf), "AT+BINP=%u\r", action);

  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BINP, buf, at_len);
}

void bta_hf_client_send_at_bia(tBTA_HF_CLIENT_CB* client_cb) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;
  int i;

  log::verbose("");
  if (client_cb->peer_version < HFP_VERSION_1_6) {
    log::verbose("Remote does not Support AT+BIA");
    return;
  }

  at_len = snprintf(buf, sizeof(buf), "AT+BIA=");

  const int32_t position =
          osi_property_get_int32("bluetooth.headset_client.disable_indicator.position", -1);

  for (i = 0; i < BTA_HF_CLIENT_AT_INDICATOR_COUNT; i++) {
    int sup = client_cb->at_cb.indicator_lookup[i] == -1 ? 0 : 1;

    /* If this value matches the position of SIGNAL in the indicators array,
     * then hardcode disable signal strength indicators.
     * indicator_lookup[i] points to the position in the
     * bta_hf_client_indicators array defined at the top of this file */
    if (client_cb->at_cb.indicator_lookup[i] == position) {
      sup = 0;
    }

    at_len += snprintf(buf + at_len, sizeof(buf) - at_len, "%u,", sup);
  }

  buf[at_len - 1] = '\r';

  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_BIA, buf, at_len);
}

void bta_hf_client_send_at_vendor_specific_cmd(tBTA_HF_CLIENT_CB* client_cb, const char* str) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];

  log::verbose("");

  int at_len = snprintf(buf, sizeof(buf), "AT%s", str);

  if (at_len < 1) {
    log::error("AT command Framing error");
    return;
  }

  buf[at_len - 1] = '\r';

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_VENDOR_SPECIFIC, buf, at_len);
}

void bta_hf_client_send_at_android(tBTA_HF_CLIENT_CB* client_cb, const char* str) {
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];
  int at_len;

  log::verbose("");

  at_len = snprintf(buf, sizeof(buf), "AT%s\r", str);
  if (at_len < 0) {
    log::error("AT command Framing error");
    return;
  }

  bta_hf_client_send_at(client_cb, BTA_HF_CLIENT_AT_ANDROID, buf, at_len);
}

void bta_hf_client_at_init(tBTA_HF_CLIENT_CB* client_cb) {
  alarm_free(client_cb->at_cb.resp_timer);
  alarm_free(client_cb->at_cb.hold_timer);
  memset(&(client_cb->at_cb), 0, sizeof(tBTA_HF_CLIENT_AT_CB));
  client_cb->at_cb.resp_timer = alarm_new("bta_hf_client.scb_at_resp_timer");
  client_cb->at_cb.hold_timer = alarm_new("bta_hf_client.scb_at_hold_timer");
  bta_hf_client_at_reset(client_cb);
}

void bta_hf_client_at_reset(tBTA_HF_CLIENT_CB* client_cb) {
  int i;

  bta_hf_client_stop_at_resp_timer(client_cb);
  bta_hf_client_stop_at_hold_timer(client_cb);

  bta_hf_client_clear_queued_at(client_cb);

  bta_hf_client_at_clear_buf(client_cb);

  for (i = 0; i < BTA_HF_CLIENT_AT_INDICATOR_COUNT; i++) {
    client_cb->at_cb.indicator_lookup[i] = -1;
  }

  client_cb->at_cb.current_cmd = BTA_HF_CLIENT_AT_NONE;
}

void bta_hf_client_send_at_cmd(tBTA_HF_CLIENT_DATA* p_data) {
  tBTA_HF_CLIENT_CB* client_cb = bta_hf_client_find_cb_by_handle(p_data->hdr.layer_specific);
  if (!client_cb) {
    log::error("cb not found for handle {}", p_data->hdr.layer_specific);
    return;
  }

  tBTA_HF_CLIENT_DATA_VAL* p_val = (tBTA_HF_CLIENT_DATA_VAL*)p_data;
  char buf[BTA_HF_CLIENT_AT_MAX_LEN];

  log::verbose("at cmd: {}", p_val->uint8_val);
  switch (p_val->uint8_val) {
    case BTA_HF_CLIENT_AT_CMD_VTS:
      bta_hf_client_send_at_vts(client_cb, (char)p_val->uint32_val1);
      break;
    case BTA_HF_CLIENT_AT_CMD_BTRH:
      bta_hf_client_send_at_btrh(client_cb, false, p_val->uint32_val1);
      break;
    case BTA_HF_CLIENT_AT_CMD_CHUP:
      bta_hf_client_send_at_chup(client_cb);
      break;
    case BTA_HF_CLIENT_AT_CMD_CHLD:
      /* expects ascii code for command */
      bta_hf_client_send_at_chld(client_cb, '0' + p_val->uint32_val1, p_val->uint32_val2);
      break;
    case BTA_HF_CLIENT_AT_CMD_BIEV:
      /* expects ascii code for command */
      bta_hf_client_send_at_biev(client_cb, p_val->uint32_val1, p_val->uint32_val2);
      break;
    case BTA_HF_CLIENT_AT_CMD_BCC:
      bta_hf_client_send_at_bcc(client_cb);
      break;
    case BTA_HF_CLIENT_AT_CMD_CNUM:
      bta_hf_client_send_at_cnum(client_cb);
      break;
    case BTA_HF_CLIENT_AT_CMD_ATA:
      bta_hf_client_send_at_ata(client_cb);
      break;
    case BTA_HF_CLIENT_AT_CMD_COPS:
      bta_hf_client_send_at_cops(client_cb, true);
      break;
    case BTA_HF_CLIENT_AT_CMD_ATD:
      bta_hf_client_send_at_atd(client_cb, p_val->str, p_val->uint32_val1);
      break;
    case BTA_HF_CLIENT_AT_CMD_VGM:
      bta_hf_client_send_at_vgm(client_cb, p_val->uint32_val1);
      break;
    case BTA_HF_CLIENT_AT_CMD_VGS:
      bta_hf_client_send_at_vgs(client_cb, p_val->uint32_val1);
      break;
    case BTA_HF_CLIENT_AT_CMD_BVRA:
      bta_hf_client_send_at_bvra(client_cb, p_val->uint32_val1 == 0 ? false : true);
      break;
    case BTA_HF_CLIENT_AT_CMD_CLCC:
      bta_hf_client_send_at_clcc(client_cb);
      break;
    case BTA_HF_CLIENT_AT_CMD_BINP:
      bta_hf_client_send_at_binp(client_cb, p_val->uint32_val1);
      break;
    case BTA_HF_CLIENT_AT_CMD_BLDN:
      bta_hf_client_send_at_bldn(client_cb);
      break;
    case BTA_HF_CLIENT_AT_CMD_NREC:
      bta_hf_client_send_at_nrec(client_cb);
      break;
    case BTA_HF_CLIENT_AT_CMD_VENDOR_SPECIFIC_CMD:
      bta_hf_client_send_at_vendor_specific_cmd(client_cb, p_val->str);
      break;
    case BTA_HF_CLIENT_AT_CMD_ANDROID:
      bta_hf_client_send_at_android(client_cb, p_val->str);
      break;
    default:
      log::error("Default case");
      snprintf(buf, BTA_HF_CLIENT_AT_MAX_LEN, "Cmd %d 1st arg %u 2nd arg %u string arg %s",
               p_val->uint8_val, p_val->uint32_val1, p_val->uint32_val2, p_val->str);
      log::error("AT buffer: {}", buf);
      break;
  }
}
