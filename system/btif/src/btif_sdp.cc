/******************************************************************************
 *
 * Copyright 2014 Samsung System LSI
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

/*******************************************************************************
 *
 *  Filename:      btif_sdp.c
 *  Description:   SDP Bluetooth Interface.
 *                 Implements the generic message handling and search
 *                 functionality.
 *                 References btif_sdp_server.c for SDP record creation.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_sdp"

#include "btif/include/btif_sdp.h"

#include <bluetooth/log.h>
#include <hardware/bluetooth.h>
#include <hardware/bt_sdp.h>
#include <stdlib.h>
#include <string.h>

#include "bta/include/bta_sdp_api.h"
#include "bta_api.h"
#include "btif_common.h"
#include "types/bluetooth/uuid.h"
#include "types/raw_address.h"

using bluetooth::Uuid;
using namespace bluetooth;

/*****************************************************************************
 *  Functions implemented in sdp_server.c
 *****************************************************************************/
bt_status_t sdp_server_init();
void sdp_server_cleanup();
bt_status_t create_sdp_record(bluetooth_sdp_record* records, int* record_handles);
bt_status_t remove_sdp_record(int record_handle);
void on_create_record_event(int handle);
void on_remove_record_event(int handle);

// Utility functions:
int get_sdp_records_size(bluetooth_sdp_record* in_record, int count);
void copy_sdp_records(bluetooth_sdp_record* in_records, bluetooth_sdp_record* out_records,
                      int count);

/*****************************************************************************
 *  Static variables
 *****************************************************************************/

static btsdp_callbacks_t* bt_sdp_callbacks = NULL;

static void btif_sdp_search_comp_evt(uint16_t event, char* p_param) {
  tBTA_SDP_SEARCH_COMP* evt_data = (tBTA_SDP_SEARCH_COMP*)p_param;
  log::verbose("event = {}", event);

  if (event != BTA_SDP_SEARCH_COMP_EVT) {
    return;
  }

  HAL_CBACK(bt_sdp_callbacks, sdp_search_cb, (bt_status_t)evt_data->status, evt_data->remote_addr,
            evt_data->uuid, evt_data->record_count, evt_data->records);
}

static void sdp_search_comp_copy_cb(uint16_t event, char* p_dest, const char* p_src) {
  tBTA_SDP_SEARCH_COMP* p_dest_data = (tBTA_SDP_SEARCH_COMP*)p_dest;
  tBTA_SDP_SEARCH_COMP* p_src_data = (tBTA_SDP_SEARCH_COMP*)p_src;

  if (!p_src) {
    return;
  }

  if (event != BTA_SDP_SEARCH_COMP_EVT) {
    return;
  }

  maybe_non_aligned_memcpy(p_dest_data, p_src_data, sizeof(*p_src_data));

  copy_sdp_records(p_src_data->records, p_dest_data->records, p_src_data->record_count);
}

static void sdp_dm_cback(tBTA_SDP_EVT event, tBTA_SDP* p_data, void* user_data) {
  switch (event) {
    case BTA_SDP_SEARCH_COMP_EVT: {
      int size = sizeof(tBTA_SDP);
      size += get_sdp_records_size(p_data->sdp_search_comp.records,
                                   p_data->sdp_search_comp.record_count);

      /* need to deep copy the record content */
      btif_transfer_context(btif_sdp_search_comp_evt, event, (char*)p_data, size,
                            sdp_search_comp_copy_cb);
      break;
    }
    case BTA_SDP_CREATE_RECORD_USER_EVT: {
      on_create_record_event(PTR_TO_INT(user_data));
      break;
    }
    case BTA_SDP_REMOVE_RECORD_USER_EVT: {
      on_remove_record_event(PTR_TO_INT(user_data));
      break;
    }
    default:
      break;
  }
}

static bt_status_t init(btsdp_callbacks_t* callbacks) {
  log::verbose("Sdp Search Init");

  bt_sdp_callbacks = callbacks;
  sdp_server_init();

  btif_enable_service(BTA_SDP_SERVICE_ID);

  return BT_STATUS_SUCCESS;
}

static bt_status_t deinit() {
  log::verbose("Sdp Search Deinit");

  bt_sdp_callbacks = NULL;
  sdp_server_cleanup();
  btif_disable_service(BTA_SDP_SERVICE_ID);

  return BT_STATUS_SUCCESS;
}

static bt_status_t search(RawAddress* bd_addr, const Uuid& uuid) {
  BTA_SdpSearch(*bd_addr, uuid);
  return BT_STATUS_SUCCESS;
}

static const btsdp_interface_t sdp_if = {
        sizeof(btsdp_interface_t), init, deinit, search, create_sdp_record, remove_sdp_record};

const btsdp_interface_t* btif_sdp_get_interface(void) {
  log::verbose("");
  return &sdp_if;
}

/*******************************************************************************
 *
 * Function         btif_sdp_execute_service
 *
 * Description      Initializes/Shuts down the service
 *
 * Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
 *
 ******************************************************************************/
bt_status_t btif_sdp_execute_service(bool b_enable) {
  log::verbose("enable:{}", b_enable);

  if (b_enable) {
    BTA_SdpEnable(sdp_dm_cback);
  } else {
    /* This is called on BT disable so no need to extra cleanup */
  }
  return BT_STATUS_SUCCESS;
}
