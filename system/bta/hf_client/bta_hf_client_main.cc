/******************************************************************************
 *
 *  Copyright (c) 2016 The Android Open Source Project
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

#include <bluetooth/log.h>
#include <stdio.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bt_dev_class.h"
#include "bta/hf_client/bta_hf_client_int.h"
#include "bta/include/utl.h"
#include "bta_api_data_types.h"
#include "bta_hf_client_api.h"
#include "bta_sys.h"
#include "btm_api_types.h"
#include "internal_include/bt_target.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/sdp_api.h"
#include "types/raw_address.h"

using namespace bluetooth::legacy::stack::sdp;
using namespace bluetooth;

static const char* bta_hf_client_evt_str(uint16_t event);
static const char* bta_hf_client_state_str(uint8_t state);
void bta_hf_client_cb_init(tBTA_HF_CLIENT_CB* client_cb, uint16_t handle);

/* state machine states */
enum {
  BTA_HF_CLIENT_INIT_ST,
  BTA_HF_CLIENT_OPENING_ST,
  BTA_HF_CLIENT_OPEN_ST,
  BTA_HF_CLIENT_CLOSING_ST
};

/* state machine action enumeration list */
enum {
  BTA_HF_CLIENT_RFC_DO_CLOSE,
  BTA_HF_CLIENT_START_CLOSE,
  BTA_HF_CLIENT_START_OPEN,
  BTA_HF_CLIENT_RFC_ACP_OPEN,
  BTA_HF_CLIENT_SCO_LISTEN,
  BTA_HF_CLIENT_SCO_CONN_OPEN,
  BTA_HF_CLIENT_SCO_CONN_CLOSE,
  BTA_HF_CLIENT_SCO_OPEN,
  BTA_HF_CLIENT_SCO_CLOSE,
  BTA_HF_CLIENT_FREE_DB,
  BTA_HF_CLIENT_OPEN_FAIL,
  BTA_HF_CLIENT_RFC_OPEN,
  BTA_HF_CLIENT_RFC_FAIL,
  BTA_HF_CLIENT_DISC_INT_RES,
  BTA_HF_CLIENT_RFC_DO_OPEN,
  BTA_HF_CLIENT_DISC_FAIL,
  BTA_HF_CLIENT_RFC_CLOSE,
  BTA_HF_CLIENT_RFC_DATA,
  BTA_HF_CLIENT_DISC_ACP_RES,
  BTA_HF_CLIENT_SVC_CONN_OPEN,
  BTA_HF_CLIENT_SEND_AT_CMD,
  BTA_HF_CLIENT_NUM_ACTIONS,
};

#define BTA_HF_CLIENT_IGNORE BTA_HF_CLIENT_NUM_ACTIONS

/* type for action functions */
typedef void (*tBTA_HF_CLIENT_ACTION)(tBTA_HF_CLIENT_DATA* p_data);

/* action functions table, indexed with action enum */
const tBTA_HF_CLIENT_ACTION bta_hf_client_action[] = {
        /* BTA_HF_CLIENT_RFC_DO_CLOSE */ bta_hf_client_rfc_do_close,
        /* BTA_HF_CLIENT_START_CLOSE */ bta_hf_client_start_close,
        /* BTA_HF_CLIENT_START_OPEN */ bta_hf_client_start_open,
        /* BTA_HF_CLIENT_RFC_ACP_OPEN */ bta_hf_client_rfc_acp_open,
        /* BTA_HF_CLIENT_SCO_LISTEN */ NULL,
        /* BTA_HF_CLIENT_SCO_CONN_OPEN */ bta_hf_client_sco_conn_open,
        /* BTA_HF_CLIENT_SCO_CONN_CLOSE*/ bta_hf_client_sco_conn_close,
        /* BTA_HF_CLIENT_SCO_OPEN */ bta_hf_client_sco_open,
        /* BTA_HF_CLIENT_SCO_CLOSE */ bta_hf_client_sco_close,
        /* BTA_HF_CLIENT_FREE_DB */ bta_hf_client_free_db,
        /* BTA_HF_CLIENT_OPEN_FAIL */ bta_hf_client_open_fail,
        /* BTA_HF_CLIENT_RFC_OPEN */ bta_hf_client_rfc_open,
        /* BTA_HF_CLIENT_RFC_FAIL */ bta_hf_client_rfc_fail,
        /* BTA_HF_CLIENT_DISC_INT_RES */ bta_hf_client_disc_int_res,
        /* BTA_HF_CLIENT_RFC_DO_OPEN */ bta_hf_client_rfc_do_open,
        /* BTA_HF_CLIENT_DISC_FAIL */ bta_hf_client_disc_fail,
        /* BTA_HF_CLIENT_RFC_CLOSE */ bta_hf_client_rfc_close,
        /* BTA_HF_CLIENT_RFC_DATA */ bta_hf_client_rfc_data,
        /* BTA_HF_CLIENT_DISC_ACP_RES */ bta_hf_client_disc_acp_res,
        /* BTA_HF_CLIENT_SVC_CONN_OPEN */ bta_hf_client_svc_conn_open,
        /* BTA_HF_CLIENT_SEND_AT_CMD */ bta_hf_client_send_at_cmd,
};

/* state table information */
#define BTA_HF_CLIENT_ACTIONS 2    /* number of actions */
#define BTA_HF_CLIENT_NEXT_STATE 2 /* position of next state */
#define BTA_HF_CLIENT_NUM_COLS 3   /* number of columns in state tables */

/* state table for init state */
const uint8_t bta_hf_client_st_init[][BTA_HF_CLIENT_NUM_COLS] = {
        /* Event                    Action 1                       Action 2
           Next state */
        /* API_OPEN_EVT */ {BTA_HF_CLIENT_START_OPEN, BTA_HF_CLIENT_IGNORE,
                            BTA_HF_CLIENT_OPENING_ST},
        /* API_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* API_AUDIO_OPEN_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* API_AUDIO_CLOSE_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* RFC_OPEN_EVT */
        {BTA_HF_CLIENT_RFC_ACP_OPEN, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* RFC_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* RFC_SRV_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* RFC_DATA_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* DISC_ACP_RES_EVT */ {BTA_HF_CLIENT_FREE_DB, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* DISC_INT_RES_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* DISC_OK_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* DISC_FAIL_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* SCO_OPEN_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* SCO_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* SEND_AT_CMD_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
};

/* state table for opening state */
const uint8_t bta_hf_client_st_opening[][BTA_HF_CLIENT_NUM_COLS] = {
        /* Event                    Action 1                       Action 2
           Next state */
        /* API_OPEN_EVT */ {BTA_HF_CLIENT_OPEN_FAIL, BTA_HF_CLIENT_IGNORE,
                            BTA_HF_CLIENT_OPENING_ST},
        /* API_CLOSE_EVT */
        {BTA_HF_CLIENT_RFC_DO_CLOSE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* API_AUDIO_OPEN_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* API_AUDIO_CLOSE_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* RFC_OPEN_EVT */ {BTA_HF_CLIENT_RFC_OPEN, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* RFC_CLOSE_EVT */ {BTA_HF_CLIENT_RFC_FAIL, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* RFC_SRV_CLOSE_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* RFC_DATA_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* DISC_ACP_RES_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* DISC_INT_RES_EVT */
        {BTA_HF_CLIENT_DISC_INT_RES, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* DISC_OK_EVT */
        {BTA_HF_CLIENT_RFC_DO_OPEN, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* DISC_FAIL_EVT */ {BTA_HF_CLIENT_DISC_FAIL, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* SCO_OPEN_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* SCO_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
        /* SEND_AT_CMD_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPENING_ST},
};

/* state table for open state */
const uint8_t bta_hf_client_st_open[][BTA_HF_CLIENT_NUM_COLS] = {
        /* Event                    Action 1                       Action 2
           Next state */
        /* API_OPEN_EVT */ {BTA_HF_CLIENT_OPEN_FAIL, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* API_CLOSE_EVT */
        {BTA_HF_CLIENT_START_CLOSE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* API_AUDIO_OPEN_EVT */
        {BTA_HF_CLIENT_SCO_OPEN, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* API_AUDIO_CLOSE_EVT */
        {BTA_HF_CLIENT_SCO_CLOSE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* RFC_OPEN_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* RFC_CLOSE_EVT */ {BTA_HF_CLIENT_RFC_CLOSE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* RFC_SRV_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* RFC_DATA_EVT */ {BTA_HF_CLIENT_RFC_DATA, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* DISC_ACP_RES_EVT */
        {BTA_HF_CLIENT_DISC_ACP_RES, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* DISC_INT_RES_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* DISC_OK_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* DISC_FAIL_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* SCO_OPEN_EVT */
        {BTA_HF_CLIENT_SCO_CONN_OPEN, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* SCO_CLOSE_EVT */
        {BTA_HF_CLIENT_SCO_CONN_CLOSE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
        /* SEND_AT_CMD_EVT */
        {BTA_HF_CLIENT_SEND_AT_CMD, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_OPEN_ST},
};

/* state table for closing state */
const uint8_t bta_hf_client_st_closing[][BTA_HF_CLIENT_NUM_COLS] = {
        /* Event                    Action 1                       Action 2
           Next state */
        /* API_OPEN_EVT */ {BTA_HF_CLIENT_OPEN_FAIL, BTA_HF_CLIENT_IGNORE,
                            BTA_HF_CLIENT_CLOSING_ST},
        /* API_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* API_AUDIO_OPEN_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* API_AUDIO_CLOSE_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* RFC_OPEN_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* RFC_CLOSE_EVT */ {BTA_HF_CLIENT_RFC_CLOSE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* RFC_SRV_CLOSE_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* RFC_DATA_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* DISC_ACP_RES_EVT */
        {BTA_HF_CLIENT_FREE_DB, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* DISC_INT_RES_EVT */ {BTA_HF_CLIENT_FREE_DB, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_INIT_ST},
        /* DISC_OK_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* DISC_FAIL_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* SCO_OPEN_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* SCO_CLOSE_EVT */ {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
        /* SEND_AT_CMD_EVT */
        {BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_IGNORE, BTA_HF_CLIENT_CLOSING_ST},
};

/* type for state table */
typedef const uint8_t (*tBTA_HF_CLIENT_ST_TBL)[BTA_HF_CLIENT_NUM_COLS];

/* state table */
const tBTA_HF_CLIENT_ST_TBL bta_hf_client_st_tbl[] = {
        bta_hf_client_st_init, bta_hf_client_st_opening, bta_hf_client_st_open,
        bta_hf_client_st_closing};

/* HF Client control block */
tBTA_HF_CLIENT_CB_ARR bta_hf_client_cb_arr;

/* Event handler for the state machine */
static const tBTA_SYS_REG bta_hf_client_reg = {bta_hf_client_hdl_event, BTA_HfClientDisable};

/*******************************************************************************
 *
 * Function         bta_hf_client_cb_arr_init
 *
 * Description      Initialize entire control block array set
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_cb_arr_init() {
  memset(&bta_hf_client_cb_arr, 0, sizeof(tBTA_HF_CLIENT_CB_ARR));

  // reset the handles and make the CBs non-allocated
  for (int i = 0; i < HF_CLIENT_MAX_DEVICES; i++) {
    // Allocate the handles in increasing order of indices
    bta_hf_client_cb_init(&(bta_hf_client_cb_arr.cb[i]), i);
    bta_hf_client_cb_arr.cb[i].handle = BTA_HF_CLIENT_CB_FIRST_HANDLE + i;
  }
}

/*******************************************************************************
 *
 * Function         bta_hf_client_cb_init
 *
 * Description      Initialize an HF_Client service control block. Assign the
 *                  handle to cb->handle.
 *
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_cb_init(tBTA_HF_CLIENT_CB* client_cb, uint16_t handle) {
  log::verbose("");

  // Free any memory we need to explicity release
  alarm_free(client_cb->collision_timer);

  // release unique pointers
  client_cb->enabled_hf_indicators.clear();
  client_cb->peer_hf_indicators.clear();

  // Memset the rest of the block
  // memset(client_cb, 0, sizeof(tBTA_HF_CLIENT_CB));
  *client_cb = {};

  // Re allocate any variables required
  client_cb->collision_timer = alarm_new("bta_hf_client.scb_collision_timer");
  client_cb->handle = handle;
  client_cb->sco_idx = BTM_INVALID_SCO_INDEX;
}

/*******************************************************************************
 *
 * Function         bta_hf_client_resume_open
 *
 * Description      Resume opening process.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_resume_open(tBTA_HF_CLIENT_CB* client_cb) {
  log::verbose("");

  /* resume opening process.  */
  if (client_cb->state == BTA_HF_CLIENT_INIT_ST) {
    client_cb->state = BTA_HF_CLIENT_OPENING_ST;
    tBTA_HF_CLIENT_DATA msg;
    msg.hdr.layer_specific = client_cb->handle;
    msg.api_open.bd_addr = client_cb->peer_addr;
    bta_hf_client_start_open(&msg);
  }
}

/*******************************************************************************
 *
 * Function         bta_hf_client_collision_timer_cback
 *
 * Description      HF Client connection collision timer callback
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_hf_client_collision_timer_cback(void* data) {
  log::verbose("");
  tBTA_HF_CLIENT_CB* client_cb = (tBTA_HF_CLIENT_CB*)data;

  /* If the peer haven't opened connection, restart opening process */
  bta_hf_client_resume_open(client_cb);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_collision_cback
 *
 * Description      Get notified about collision.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_collision_cback(tBTA_SYS_CONN_STATUS /* status */, tBTA_SYS_ID id,
                                   uint8_t /* app_id */, const RawAddress& peer_addr) {
  tBTA_HF_CLIENT_CB* client_cb = bta_hf_client_find_cb_by_bda(peer_addr);
  if (client_cb != NULL && client_cb->state == BTA_HF_CLIENT_OPENING_ST) {
    if (id == BTA_ID_SYS) /* ACL collision */
    {
      log::warn("HF Client found collision (ACL) ...");
    } else if (id == BTA_ID_HS) /* RFCOMM collision */
    {
      log::warn("HF Client found collision (RFCOMM) ...");
    } else {
      log::warn("HF Client found collision (\?\?\?) ...");
    }

    client_cb->state = BTA_HF_CLIENT_INIT_ST;

    /* Cancel SDP if it had been started. */
    if (client_cb->p_disc_db) {
      if (!get_legacy_stack_sdp_api()->service.SDP_CancelServiceSearch(client_cb->p_disc_db)) {
        log::warn("Unable to cancel SDP service discovery peer:{}", peer_addr);
      }
      osi_free_and_reset((void**)&client_cb->p_disc_db);
    }

    /* reopen registered server */
    /* Collision may be detected before or after we close servers. */
    bta_hf_client_start_server();

    /* Start timer to handle connection opening restart */
    alarm_set_on_mloop(client_cb->collision_timer, BTA_HF_CLIENT_COLLISION_TIMER_MS,
                       bta_hf_client_collision_timer_cback, (void*)client_cb);
  }
}

/*******************************************************************************
 *
 * Function         bta_hf_client_api_enable
 *
 * Description      Handle an API enable event.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
tBTA_STATUS bta_hf_client_api_enable(tBTA_HF_CLIENT_CBACK* p_cback, tBTA_HF_CLIENT_FEAT features,
                                     const char* p_service_name) {
  /* If already registered then return error */
  if (bta_sys_is_register(BTA_ID_HS)) {
    log::error("BTA HF Client is already enabled, ignoring ...");
    return BTA_FAILURE;
  }

  /* register with BTA system manager */
  bta_sys_register(BTA_ID_HS, &bta_hf_client_reg);

  /* reset the control blocks */
  bta_hf_client_cb_arr_init();

  bta_hf_client_cb_arr.p_cback = p_cback;
  bta_hf_client_cb_arr.features = features;
  bta_hf_client_cb_arr.is_support_lc3 = features & BTA_HF_CLIENT_FEAT_SWB;

  /* create SDP records */
  bta_hf_client_create_record(&bta_hf_client_cb_arr, p_service_name);

  /* set same setting as AG does */
  get_btm_client_interface().sco.BTM_WriteVoiceSettings(AG_VOICE_SETTINGS);

  bta_sys_collision_register(BTA_ID_HS, bta_hf_client_collision_cback);

  /* Set the Audio service class bit */
  tBTA_UTL_COD cod = {
          .minor = BTM_COD_MINOR_UNCLASSIFIED,
          .major = BTM_COD_MAJOR_UNCLASSIFIED,
          .service = BTM_COD_SERVICE_AUDIO,
  };
  utl_set_device_class(&cod, BTA_UTL_SET_COD_SERVICE_CLASS);

  /* start RFCOMM server */
  bta_hf_client_start_server();

  return BTA_SUCCESS;
}

/*******************************************************************************
 *
 * Function         bta_hf_client_find_cb_by_handle
 *
 * Description      Finds the control block by handle provided
 *
 *                  handle: Handle as obtained from BTA_HfClientOpen call
 *
 *
 * Returns          Control block corresponding to the handle and NULL if
 *                  none exists
 *
 ******************************************************************************/
tBTA_HF_CLIENT_CB* bta_hf_client_find_cb_by_handle(uint16_t handle) {
  // Handles are limited from 1 through HF_CLIENT_MAX_DEVICES
  if (handle < 1 || handle > HF_CLIENT_MAX_DEVICES) {
    log::error("handle out of range ({}, {}) {}", 1, HF_CLIENT_MAX_DEVICES, handle);
    return NULL;
  }

  // Check if the associated index is allocated. Index is (handle - 1).
  if (bta_hf_client_cb_arr.cb[handle - 1].is_allocated) {
    return &(bta_hf_client_cb_arr.cb[handle - 1]);
  }

  log::error("block not found for handle {}", handle);
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_hf_client_find_cb_by_bda
 *
 * Description      Finds the control block by handle provided
 *
 *                  bda: address of the device to find the handle for.
 *                  Since there can only be one HF connection for a device
 *                  we should always find a unique block
 *
 * Returns          Control block corresponding to the address and NULL if
 *                  none exists
 *
 ******************************************************************************/
tBTA_HF_CLIENT_CB* bta_hf_client_find_cb_by_bda(const RawAddress& peer_addr) {
  for (int i = 0; i < HF_CLIENT_MAX_DEVICES; i++) {
    // Check if the associated index is allocated and that BD ADDR matches
    tBTA_HF_CLIENT_CB* client_cb = &bta_hf_client_cb_arr.cb[i];
    if (client_cb->is_allocated && peer_addr == client_cb->peer_addr) {
      return client_cb;
    } else {
      log::warn("bdaddr mismatch for handle {} alloc {}", i, client_cb->is_allocated);
    }
  }
  log::error("block not found");
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_hf_client_find_cb_by_rfc_handle
 *
 * Description      Finds the control block by RFC handle provided.
 *
 *                  handle: RFC handle for the established connection
 *
 *
 * Returns          Control block corresponding to the handle and NULL if none
 *                  exists
 *
 ******************************************************************************/
tBTA_HF_CLIENT_CB* bta_hf_client_find_cb_by_rfc_handle(uint16_t handle) {
  for (int i = 0; i < HF_CLIENT_MAX_DEVICES; i++) {
    tBTA_HF_CLIENT_CB* client_cb = &bta_hf_client_cb_arr.cb[i];
    bool is_allocated = client_cb->is_allocated;
    uint16_t conn_handle = client_cb->conn_handle;

    log::verbose("cb rfc_handle {} alloc {} conn_handle {}", handle, is_allocated, conn_handle);

    if (is_allocated && conn_handle == handle) {
      return client_cb;
    }

    log::warn("no cb yet {} alloc {} conn_handle {}", handle, is_allocated, conn_handle);
  }

  log::error("no cb found for rfc handle {}", handle);
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_hf_client_find_cb_by_sco_handle
 *
 * Description      Finds the control block by sco handle provided
 *
 *                  handle: sco handle
 *
 *
 * Returns          Control block corresponding to the sco handle and
 *                  none if none exists
 *
 ******************************************************************************/
tBTA_HF_CLIENT_CB* bta_hf_client_find_cb_by_sco_handle(uint16_t handle) {
  for (int i = 0; i < HF_CLIENT_MAX_DEVICES; i++) {
    tBTA_HF_CLIENT_CB* client_cb = &bta_hf_client_cb_arr.cb[i];
    if (client_cb->is_allocated && client_cb->sco_idx == handle) {
      return client_cb;
    }
  }
  log::error("block not found for handle {}", handle);
  return NULL;
}

/*******************************************************************************
 *
 * Function         bta_hf_client_allocate_handle
 *
 * Description      Allocates a handle for the new BD ADDR that needs a new RF
 *                  channel for HF connection. If the channel cannot be created
 *                  for a reason then false is returned
 *
 *                  bd_addr: Address of the device for which this block is
 *                  being created. Single device can only have one block.
 *                  p_handle: OUT variable to store the outcome of allocate. If
 *                  allocate failed then value is not valid
 *
 *
 * Returns          true if the creation of p_handle succeeded, false otherwise
 *
 ******************************************************************************/
bool bta_hf_client_allocate_handle(const RawAddress& bd_addr, uint16_t* p_handle) {
  tBTA_HF_CLIENT_CB* existing_cb = bta_hf_client_find_cb_by_bda(bd_addr);
  if (existing_cb != NULL) {
    log::error("cannot allocate handle since BDADDR already exists");
    return false;
  }
  /* Check that we do not have a request to for same device in the control
   * blocks */
  for (int i = 0; i < HF_CLIENT_MAX_DEVICES; i++) {
    tBTA_HF_CLIENT_CB* client_cb = &bta_hf_client_cb_arr.cb[i];
    if (client_cb->is_allocated) {
      log::warn("control block already used index {}", i);
      continue;
    }

    // Reset the client control block
    bta_hf_client_cb_init(client_cb, client_cb->handle);

    *p_handle = client_cb->handle;
    log::verbose("marking CB handle {} to true", client_cb->handle);

    client_cb->is_allocated = true;
    client_cb->peer_addr = bd_addr;
    bta_hf_client_at_init(client_cb);
    return true;
  }

  return false;
  log::error("all control blocks in use!");
}

/*******************************************************************************
 *
 * Function         bta_hf_client_app_callback
 *
 * Description      Calls the application callback
 *
 *
 * Returns          Void
 *
 ******************************************************************************/
void bta_hf_client_app_callback(uint16_t event, tBTA_HF_CLIENT* data) {
  if (bta_hf_client_cb_arr.p_cback != NULL) {
    bta_hf_client_cb_arr.p_cback(event, data);
  }
}

/*******************************************************************************
 *
 * Function         bta_hf_client_api_disable
 *
 * Description      Handle an API disable event.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_api_disable() {
  if (!bta_sys_is_register(BTA_ID_HS)) {
    log::warn("BTA HF Client is already disabled, ignoring ...");
    return;
  }

  /* Remove the collision handler */
  bta_sys_collision_register(BTA_ID_HS, NULL);

  bta_hf_client_cb_arr.deregister = true;

  /* remove sdp record */
  bta_hf_client_del_record(&bta_hf_client_cb_arr);

  /* remove rfcomm server */
  bta_hf_client_close_server();

  /* reinit the control block */
  for (int i = 0; i < HF_CLIENT_MAX_DEVICES; i++) {
    if (bta_hf_client_cb_arr.cb[i].is_allocated) {
      bta_hf_client_cb_init(&(bta_hf_client_cb_arr.cb[i]), i);
    }
  }

  /* De-register with BTA system manager */
  bta_sys_deregister(BTA_ID_HS);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_hdl_event
 *
 * Description      Data HF Client main event handling function.
 *
 *
 * Returns          bool
 *
 ******************************************************************************/
bool bta_hf_client_hdl_event(const BT_HDR_RIGID* p_msg) {
  log::verbose("{} (0x{:x})", bta_hf_client_evt_str(p_msg->event), p_msg->event);
  bta_hf_client_sm_execute(p_msg->event, (tBTA_HF_CLIENT_DATA*)p_msg);
  return true;
}

/*******************************************************************************
 *
 * Function         bta_hf_client_sm_execute
 *
 * Description      State machine event handling function for HF Client
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_sm_execute(uint16_t event, tBTA_HF_CLIENT_DATA* p_data) {
  tBTA_HF_CLIENT_CB* client_cb = bta_hf_client_find_cb_by_handle(p_data->hdr.layer_specific);
  if (client_cb == NULL) {
    log::error("cb not found for handle {}", p_data->hdr.layer_specific);
    return;
  }

  tBTA_HF_CLIENT_ST_TBL state_table;
  uint8_t action;
  int i;

  uint16_t in_event = event;
  uint8_t in_state = client_cb->state;

  /* Ignore displaying of AT results when not connected (Ignored in state
   * machine) */
  if (client_cb->state == BTA_HF_CLIENT_OPEN_ST) {
    log::verbose("HF Client evt : State {} ({}), Event 0x{:04x} ({})", client_cb->state,
                 bta_hf_client_state_str(client_cb->state), event, bta_hf_client_evt_str(event));
  }

  event &= 0x00FF;
  if (event >= (BTA_HF_CLIENT_MAX_EVT & 0x00FF)) {
    log::error("HF Client evt out of range, ignoring...");
    return;
  }

  /* look up the state table for the current state */
  state_table = bta_hf_client_st_tbl[client_cb->state];

  /* set next state */
  client_cb->state = state_table[event][BTA_HF_CLIENT_NEXT_STATE];

  /* execute action functions */
  for (i = 0; i < BTA_HF_CLIENT_ACTIONS; i++) {
    action = state_table[event][i];
    if (action != BTA_HF_CLIENT_IGNORE) {
      (*bta_hf_client_action[action])(p_data);
    } else {
      break;
    }
  }

  /* If the state has changed then notify the app of the corresponding change */
  if (in_state != client_cb->state) {
    log::verbose("notifying state change to {} -> {} device {}", in_state, client_cb->state,
                 client_cb->peer_addr);
    tBTA_HF_CLIENT evt;
    memset(&evt, 0, sizeof(evt));
    evt.bd_addr = client_cb->peer_addr;
    if (client_cb->state == BTA_HF_CLIENT_INIT_ST) {
      bta_hf_client_app_callback(BTA_HF_CLIENT_CLOSE_EVT, &evt);
      log::verbose("marking CB handle {} to false", client_cb->handle);
      client_cb->is_allocated = false;
    } else if (client_cb->state == BTA_HF_CLIENT_OPEN_ST) {
      evt.open.handle = client_cb->handle;
      bta_hf_client_app_callback(BTA_HF_CLIENT_OPEN_EVT, &evt);
    }
  }

  log::verbose("device {} state change: [{}] -> [{}] after Event [{}]", client_cb->peer_addr,
               bta_hf_client_state_str(in_state), bta_hf_client_state_str(client_cb->state),
               bta_hf_client_evt_str(in_event));
}

static void send_post_slc_cmd(tBTA_HF_CLIENT_CB* client_cb) {
  client_cb->at_cb.current_cmd = BTA_HF_CLIENT_AT_NONE;

  tBTA_HF_CLIENT_DATA p_data;
  p_data.hdr.layer_specific = client_cb->handle;
  bta_hf_client_sco_listen(&p_data);
  bta_hf_client_send_at_bia(client_cb);
  bta_hf_client_send_at_ccwa(client_cb, true);
  bta_hf_client_send_at_cmee(client_cb, true);
  bta_hf_client_send_at_cops(client_cb, false);
  bta_hf_client_send_at_btrh(client_cb, true, 0);
  bta_hf_client_send_at_clip(client_cb, true);
}

/*******************************************************************************
 *
 * Function         bta_hf_client_slc_seq
 *
 * Description      Handles AT commands sequence required for SLC creation
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_hf_client_slc_seq(tBTA_HF_CLIENT_CB* client_cb, bool error) {
  log::verbose("bta_hf_client_slc_seq cmd: {}", client_cb->at_cb.current_cmd);

  if (error) {
    /* SLC establishment error, sent close rfcomm event */
    log::error("HFPClient: Failed to create SLC due to AT error, disconnecting ({})",
               client_cb->at_cb.current_cmd);

    tBTA_HF_CLIENT_DATA msg;
    msg.hdr.layer_specific = client_cb->handle;
    bta_hf_client_sm_execute(BTA_HF_CLIENT_API_CLOSE_EVT, &msg);
    return;
  }

  if (client_cb->svc_conn) {
    log::warn("SLC already connected for CB handle {}", client_cb->handle);
    return;
  }

  switch (client_cb->at_cb.current_cmd) {
    case BTA_HF_CLIENT_AT_NONE:
      bta_hf_client_send_at_brsf(client_cb, bta_hf_client_cb_arr.features);
      break;

    case BTA_HF_CLIENT_AT_BRSF:
      if ((bta_hf_client_cb_arr.features & BTA_HF_CLIENT_FEAT_CODEC) &&
          (client_cb->peer_features & BTA_HF_CLIENT_PEER_CODEC)) {
        bta_hf_client_send_at_bac(client_cb);
        break;
      }

      bta_hf_client_send_at_cind(client_cb, false);
      break;

    case BTA_HF_CLIENT_AT_BAC:
      bta_hf_client_send_at_cind(client_cb, false);
      break;

    case BTA_HF_CLIENT_AT_CIND:
      bta_hf_client_send_at_cind(client_cb, true);
      break;

    case BTA_HF_CLIENT_AT_CIND_STATUS:
      bta_hf_client_send_at_cmer(client_cb, true);
      break;

    case BTA_HF_CLIENT_AT_CMER:
      if (client_cb->peer_features & BTA_HF_CLIENT_PEER_FEAT_3WAY &&
          bta_hf_client_cb_arr.features & BTA_HF_CLIENT_FEAT_3WAY) {
        bta_hf_client_send_at_chld(client_cb, '?', 0);
      } else if (bta_hf_client_cb_arr.features & BTA_HF_CLIENT_FEAT_HF_IND &&
                 client_cb->peer_features & BTA_HF_CLIENT_PEER_HF_IND) {
        bta_hf_client_send_at_bind(client_cb, 0);
      } else {
        tBTA_HF_CLIENT_DATA msg;
        msg.hdr.layer_specific = client_cb->handle;
        bta_hf_client_svc_conn_open(&msg);
        send_post_slc_cmd(client_cb);
      }
      break;

    case BTA_HF_CLIENT_AT_CHLD:
      if (bta_hf_client_cb_arr.features & BTA_HF_CLIENT_FEAT_HF_IND &&
          client_cb->peer_features & BTA_HF_CLIENT_PEER_HF_IND) {
        bta_hf_client_send_at_bind(client_cb, 0);
      } else {
        tBTA_HF_CLIENT_DATA msg;
        msg.hdr.layer_specific = client_cb->handle;
        bta_hf_client_svc_conn_open(&msg);
        send_post_slc_cmd(client_cb);
      }
      break;

    case BTA_HF_CLIENT_AT_BIND_SET_IND:
      bta_hf_client_send_at_bind(client_cb, 1);
      break;

    case BTA_HF_CLIENT_AT_BIND_READ_SUPPORTED_IND:
      bta_hf_client_send_at_bind(client_cb, 2);
      break;

    case BTA_HF_CLIENT_AT_BIND_READ_ENABLED_IND:
      tBTA_HF_CLIENT_DATA msg;
      msg.hdr.layer_specific = client_cb->handle;
      bta_hf_client_svc_conn_open(&msg);
      send_post_slc_cmd(client_cb);
      break;

    default: {
      /* If happen there is a bug in SLC creation procedure... */
      log::error(
              "HFPClient: Failed to create SLCdue to unexpected AT command, "
              "disconnecting ({})",
              client_cb->at_cb.current_cmd);

      tBTA_HF_CLIENT_DATA msg;
      msg.hdr.layer_specific = client_cb->handle;
      bta_hf_client_sm_execute(BTA_HF_CLIENT_API_CLOSE_EVT, &msg);
      break;
    }
  }
}

#ifndef CASE_RETURN_STR
#define CASE_RETURN_STR(const) \
  case const:                  \
    return #const;
#endif

static const char* bta_hf_client_evt_str(uint16_t event) {
  switch (event) {
    CASE_RETURN_STR(BTA_HF_CLIENT_API_OPEN_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_API_CLOSE_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_API_AUDIO_OPEN_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_API_AUDIO_CLOSE_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_RFC_OPEN_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_RFC_CLOSE_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_RFC_SRV_CLOSE_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_RFC_DATA_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_DISC_ACP_RES_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_DISC_INT_RES_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_DISC_OK_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_DISC_FAIL_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_API_ENABLE_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_API_DISABLE_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_SCO_OPEN_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_SCO_CLOSE_EVT)
    CASE_RETURN_STR(BTA_HF_CLIENT_SEND_AT_CMD_EVT)
    default:
      return "Unknown HF Client Event";
  }
}

static const char* bta_hf_client_state_str(uint8_t state) {
  switch (state) {
    CASE_RETURN_STR(BTA_HF_CLIENT_INIT_ST)
    CASE_RETURN_STR(BTA_HF_CLIENT_OPENING_ST)
    CASE_RETURN_STR(BTA_HF_CLIENT_OPEN_ST)
    CASE_RETURN_STR(BTA_HF_CLIENT_CLOSING_ST)
    default:
      return "Unknown HF Client State";
  }
}

void bta_hf_client_dump_statistics(int fd) {
  dprintf(fd, "\nBluetooth HF Client BTA Statistics\n");

  // We dump statistics for all control blocks
  for (int i = 0; i < HF_CLIENT_MAX_DEVICES; i++) {
    tBTA_HF_CLIENT_CB* client_cb = &bta_hf_client_cb_arr.cb[i];
    if (!client_cb->is_allocated) {
      // Skip the blocks which are not allocated
      continue;
    }

    dprintf(fd, "  Control block #%d\n", i + 1);

    uint8_t* a = client_cb->peer_addr.address;
    // Device name
    dprintf(fd, "    Peer Device: %02x:%02x:%02x:%02x:%02x:%02x\n", a[0], a[1], a[2], a[3], a[4],
            a[5]);

    // State machine state
    dprintf(fd, "    State Machine State: %s\n", bta_hf_client_state_str(client_cb->state));

    // Local RFC channelfor communication
    dprintf(fd, "    RFCOMM Channel (local) %d\n", client_cb->conn_handle);

    // BTA Handle shared between BTA and client (ex BTIF)
    dprintf(fd, "    BTA Generated handle %d\n", client_cb->handle);
  }
}
