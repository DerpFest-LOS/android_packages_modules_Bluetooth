/******************************************************************************
 *
 *  Copyright 2005-2012 Broadcom Corporation
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
 *  This is the implementation file for advanced audio/video call-in
 *  functions.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth-a2dp"

#include "bta/include/bta_av_ci.h"

#include <bluetooth/log.h>

#include <cstdint>

#include "a2dp_constants.h"
#include "bta/av/bta_av_int.h"
#include "bta_av_api.h"
#include "bta_sys.h"
#include "osi/include/allocator.h"
#include "stack/include/bt_hdr.h"

using namespace bluetooth;

/*******************************************************************************
 *
 * Function         bta_av_ci_src_data_ready
 *
 * Description      This function sends an event to the AV indicating that
 *                  the phone has audio stream data ready to send and AV
 *                  should call bta_av_co_audio_source_data_path().
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_ci_src_data_ready(tBTA_AV_CHNL chnl) {
  BT_HDR_RIGID* p_buf = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));

  p_buf->layer_specific = chnl;
  p_buf->event = BTA_AV_CI_SRC_DATA_READY_EVT;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         bta_av_ci_setconfig
 *
 * Description      This function must be called in response to function
 *                  bta_av_co_audio_setconfig().
 *                  Parameter err_code is set to an AVDTP status value;
 *                  AVDT_SUCCESS if the codec configuration is ok,
 *                  otherwise error.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_ci_setconfig(tBTA_AV_HNDL bta_av_handle, uint8_t err_code, uint8_t category,
                         bool recfg_needed, uint8_t avdt_handle) {
  log::info("bta_av_handle=0x{:x} err_code={} category={} recfg_needed={} avdt_handle={}",
            bta_av_handle, err_code, category, recfg_needed, avdt_handle);

  tBTA_AV_CI_SETCONFIG* p_buf = (tBTA_AV_CI_SETCONFIG*)osi_malloc(sizeof(tBTA_AV_CI_SETCONFIG));

  p_buf->hdr.layer_specific = bta_av_handle;
  p_buf->hdr.event =
          (err_code == A2DP_SUCCESS) ? BTA_AV_CI_SETCONFIG_OK_EVT : BTA_AV_CI_SETCONFIG_FAIL_EVT;
  p_buf->err_code = err_code;
  p_buf->category = category;
  p_buf->recfg_needed = recfg_needed;
  p_buf->avdt_handle = avdt_handle;

  bta_sys_sendmsg(p_buf);
}
