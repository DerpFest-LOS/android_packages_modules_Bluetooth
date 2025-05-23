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
 *  This is the main implementation file for the BTA advanced audio/video.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth-a2dp"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>
#include <stdio.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "a2dp_api.h"
#include "a2dp_codec_api.h"
#include "a2dp_constants.h"
#include "avct_api.h"
#include "avdt_api.h"
#include "avrc_api.h"
#include "avrc_defs.h"
#include "bt_dev_class.h"
#include "bta/av/bta_av_int.h"
#include "bta/include/bta_ar_api.h"
#include "bta/include/bta_av_co.h"
#include "bta/include/utl.h"
#include "bta/sys/bta_sys.h"
#include "bta_av_api.h"
#include "btif/avrcp/avrcp_service.h"
#include "btif/include/btif_av.h"
#include "btif/include/btif_av_co.h"
#include "btif/include/btif_config.h"
#include "hardware/bt_av.h"
#include "internal_include/bt_target.h"
#include "os/logging/log_adapter.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/list.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/btm_status.h"
#include "stack/include/hci_error_code.h"
#include "stack/include/sdp_api.h"
#include "storage/config_keys.h"
#include "types/hci_role.h"
#include "types/raw_address.h"

using namespace bluetooth::legacy::stack::sdp;
using namespace bluetooth;

/*****************************************************************************
 * Constants and types
 ****************************************************************************/

#ifndef BTA_AV_RET_TOUT
#define BTA_AV_RET_TOUT 4
#endif

#ifndef BTA_AV_SIG_TOUT
#define BTA_AV_SIG_TOUT 4
#endif

#ifndef BTA_AV_IDLE_TOUT
#define BTA_AV_IDLE_TOUT 10
#endif

/* the delay time in milliseconds to retry role switch */
#ifndef BTA_AV_RS_TIME_VAL
#define BTA_AV_RS_TIME_VAL 1000
#endif

/* state machine states */
enum { BTA_AV_INIT_ST, BTA_AV_OPEN_ST };

typedef void (*tBTA_AV_NSM_ACT)(tBTA_AV_DATA* p_data);
static void bta_av_api_enable(tBTA_AV_DATA* p_data);
static void bta_av_api_register(tBTA_AV_DATA* p_data);
static void bta_av_ci_data(tBTA_AV_DATA* p_data);
static void bta_av_rpc_conn(tBTA_AV_DATA* p_data);

static void bta_av_sco_chg_cback(tBTA_SYS_CONN_STATUS status, uint8_t num_sco_links, uint8_t app_id,
                                 const RawAddress& peer_addr);
static void bta_av_sys_rs_cback(tBTA_SYS_CONN_STATUS status, tHCI_ROLE new_role,
                                tHCI_STATUS hci_status, const RawAddress& peer_addr);

/*****************************************************************************
 * Global data
 ****************************************************************************/

/* AV control block */
tBTA_AV_CB bta_av_cb = {};

static const char* bta_av_st_code(uint8_t state);

/*******************************************************************************
 *
 * Function         bta_av_api_enable
 *
 * Description      Handle an API enable event.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_api_enable(tBTA_AV_DATA* p_data) {
  if (btif_av_src_sink_coexist_enabled() && bta_av_cb.features != 0) {
    tBTA_AV_ENABLE enable;
    tBTA_AV bta_av_data;
    bta_av_cb.sink_features = p_data->api_enable.features;

    enable.features = p_data->api_enable.features;
    bta_av_data.enable = enable;
    (*bta_av_cb.p_cback)(BTA_AV_ENABLE_EVT, &bta_av_data);

    /* if this is source feature, then exchange them */
    if (p_data->api_enable.features & BTA_AV_FEAT_SRC) {
      tBTA_AV_FEAT tmp_feature = bta_av_cb.features;
      bta_av_cb.features = bta_av_cb.sink_features;
      bta_av_cb.sink_features = tmp_feature;
    }
    return;
  }

  if (bta_av_cb.disabling) {
    log::warn("previous (reg_audio={:#x}) is still disabling (attempts={})", bta_av_cb.reg_audio,
              bta_av_cb.enabling_attempts);
    if (++bta_av_cb.enabling_attempts <= kEnablingAttemptsCountMaximum) {
      tBTA_AV_API_ENABLE* p_buf = (tBTA_AV_API_ENABLE*)osi_malloc(sizeof(tBTA_AV_API_ENABLE));
      memcpy(p_buf, &p_data->api_enable, sizeof(tBTA_AV_API_ENABLE));
      bta_sys_sendmsg_delayed(p_buf, std::chrono::milliseconds(kEnablingAttemptsIntervalMs));
      return;
    }
    if (bta_av_cb.sdp_a2dp_handle) {
      if (!get_legacy_stack_sdp_api()->handle.SDP_DeleteRecord(bta_av_cb.sdp_a2dp_handle)) {
        log::warn("Unable to delete SDP record handle:{}", bta_av_cb.sdp_a2dp_handle);
      }
      bta_sys_remove_uuid(UUID_SERVCLASS_AUDIO_SOURCE);
    }
    if (bta_av_cb.sdp_a2dp_snk_handle) {
      if (!get_legacy_stack_sdp_api()->handle.SDP_DeleteRecord(bta_av_cb.sdp_a2dp_snk_handle)) {
        log::warn("Unable to delete SDP record handle:{}", bta_av_cb.sdp_a2dp_snk_handle);
      }
      bta_sys_remove_uuid(UUID_SERVCLASS_AUDIO_SINK);
    }
    // deregister from AVDT
    bta_ar_dereg_avdt();

    // deregister from AVRC
    bta_ar_dereg_avrc(UUID_SERVCLASS_AV_REMOTE_CONTROL);
    bta_ar_dereg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET);
    // deregister from AVCT
    bta_ar_dereg_avct();
  }

  /* initialize control block */
  memset(&bta_av_cb, 0, sizeof(tBTA_AV_CB));

  for (int i = 0; i < BTA_AV_NUM_RCB; i++) {
    bta_av_cb.rcb[i].handle = BTA_AV_RC_HANDLE_NONE;
  }

  bta_av_cb.rc_acp_handle = BTA_AV_RC_HANDLE_NONE;

  /* store parameters */
  bta_av_cb.p_cback = p_data->api_enable.p_cback;
  bta_av_cb.features = p_data->api_enable.features;
  bta_av_cb.offload_start_pending_hndl = BTA_AV_INVALID_HANDLE;
  bta_av_cb.offload_started_hndl = BTA_AV_INVALID_HANDLE;

  tBTA_AV_ENABLE enable;
  enable.features = bta_av_cb.features;

  /* Register for SCO change event */
  bta_sys_sco_register(bta_av_sco_chg_cback);

  /* call callback with enable event */
  tBTA_AV bta_av_data;
  bta_av_data.enable = enable;
  (*bta_av_cb.p_cback)(BTA_AV_ENABLE_EVT, &bta_av_data);
}

/*******************************************************************************
 *
 * Function         bta_av_addr_to_scb
 *
 * Description      find the stream control block by the peer addr
 *
 * Returns          void
 *
 ******************************************************************************/
tBTA_AV_SCB* bta_av_addr_to_scb(const RawAddress& bd_addr) {
  tBTA_AV_SCB* p_scb = NULL;
  int xx;

  for (xx = 0; xx < BTA_AV_NUM_STRS; xx++) {
    if (bta_av_cb.p_scb[xx]) {
      if (bd_addr == bta_av_cb.p_scb[xx]->PeerAddress()) {
        p_scb = bta_av_cb.p_scb[xx];
        break;
      }
    }
  }
  return p_scb;
}

int BTA_AvObtainPeerChannelIndex(const RawAddress& peer_address) {
  // Find the entry for the peer (if exists)
  tBTA_AV_SCB* p_scb = bta_av_addr_to_scb(peer_address);
  if (p_scb != nullptr) {
    return p_scb->hdi;
  }

  // Find the index for an entry that is not used
  for (int index = 0; index < BTA_AV_NUM_STRS; index++) {
    tBTA_AV_SCB* p_scb = bta_av_cb.p_scb[index];
    if (p_scb == nullptr) {
      continue;
    }
    if (p_scb->PeerAddress().IsEmpty()) {
      const RawAddress& btif_addr = btif_av_find_by_handle(p_scb->hndl);
      if (!btif_addr.IsEmpty() && btif_addr != peer_address) {
        log::verbose("btif_addr = {}, index={}!", btif_addr.ToString(), index);
        continue;
      }
      return p_scb->hdi;
    }
  }

  return -1;
}

/*******************************************************************************
 *
 * Function         bta_av_hndl_to_scb
 *
 * Description      find the stream control block by the handle
 *
 * Returns          void
 *
 ******************************************************************************/
tBTA_AV_SCB* bta_av_hndl_to_scb(uint16_t handle) {
  tBTA_AV_HNDL hndl = (tBTA_AV_HNDL)handle;
  tBTA_AV_SCB* p_scb = NULL;
  uint8_t idx = (hndl & BTA_AV_HNDL_MSK);

  if (idx && (idx <= BTA_AV_NUM_STRS)) {
    p_scb = bta_av_cb.p_scb[idx - 1];
  }
  return p_scb;
}

/*******************************************************************************
 *
 * Function         bta_av_alloc_scb
 *
 * Description      allocate stream control block,
 *                  register the service to stack
 *                  create SDP record
 *
 * Returns          void
 *
 ******************************************************************************/
static tBTA_AV_SCB* bta_av_alloc_scb(tBTA_AV_CHNL chnl) {
  if (chnl != BTA_AV_CHNL_AUDIO) {
    log::error("bad channel: {}", chnl);
    return nullptr;
  }

  for (int xx = 0; xx < BTA_AV_NUM_STRS; xx++) {
    if (bta_av_cb.p_scb[xx] != nullptr) {
      continue;
    }
    // Found an empty spot
    // TODO: After tBTA_AV_SCB is changed to a proper class, the entry
    // here should be allocated by C++ 'new' statement.
    tBTA_AV_SCB* p_ret = (tBTA_AV_SCB*)osi_calloc(sizeof(tBTA_AV_SCB));
    p_ret->rc_handle = BTA_AV_RC_HANDLE_NONE;
    p_ret->chnl = chnl;
    p_ret->hndl = (tBTA_AV_HNDL)((xx + 1) | chnl);
    p_ret->hdi = xx;
    p_ret->a2dp_list = list_new(nullptr);
    p_ret->avrc_ct_timer = alarm_new("bta_av.avrc_ct_timer");
    bta_av_cb.p_scb[xx] = p_ret;
    return p_ret;
  }

  return nullptr;
}

static tBTA_AV_SCB* bta_av_find_scb(tBTA_AV_CHNL chnl, uint8_t app_id) {
  if (chnl != BTA_AV_CHNL_AUDIO) {
    log::error("bad channel: {}", chnl);
    return nullptr;
  }

  for (int xx = 0; xx < BTA_AV_NUM_STRS; xx++) {
    if ((bta_av_cb.p_scb[xx] != nullptr) && (bta_av_cb.p_scb[xx]->chnl == chnl) &&
        (bta_av_cb.p_scb[xx]->app_id == app_id)) {
      log::verbose("found at: {}", xx);
      return bta_av_cb.p_scb[xx];
    }
  }

  return nullptr;
}

void bta_av_free_scb(tBTA_AV_SCB* p_scb) {
  if (p_scb == nullptr) {
    return;
  }
  uint8_t scb_index = p_scb->hdi;
  log::assert_that(scb_index < BTA_AV_NUM_STRS, "assert failed: scb_index < BTA_AV_NUM_STRS");

  log::assert_that(p_scb == bta_av_cb.p_scb[scb_index],
                   "assert failed: p_scb == bta_av_cb.p_scb[scb_index]");
  bta_av_cb.p_scb[scb_index] = nullptr;
  alarm_free(p_scb->avrc_ct_timer);
  list_free(p_scb->a2dp_list);
  p_scb->a2dp_list = NULL;
  // TODO: After tBTA_AV_SCB is changed to a proper class, the entry
  // here should be de-allocated by C++ 'delete' statement.
  osi_free(p_scb);
}

void tBTA_AV_SCB::OnConnected(const RawAddress& peer_address) {
  peer_address_ = peer_address;

  if (peer_address.IsEmpty()) {
    log::error("Invalid peer address: {}", peer_address);
    return;
  }

  // Read and restore the AVDTP version from local storage
  uint16_t avdtp_version = 0;
  size_t version_value_size = sizeof(avdtp_version);
  if (!btif_config_get_bin(peer_address_.ToString(), BTIF_STORAGE_KEY_AVDTP_VERSION,
                           (uint8_t*)&avdtp_version, &version_value_size)) {
    log::warn("Failed to read cached peer AVDTP version for {}", peer_address_);
  } else {
    SetAvdtpVersion(avdtp_version);
  }
}

void tBTA_AV_SCB::OnDisconnected() {
  peer_address_ = RawAddress::kEmpty;
  SetAvdtpVersion(0);
}

void tBTA_AV_SCB::SetAvdtpVersion(uint16_t avdtp_version) {
  avdtp_version_ = avdtp_version;
  log::info("AVDTP version for {} set to 0x{:x}", peer_address_, avdtp_version_);
}

/*******************************************************************************
 ******************************************************************************/
void bta_av_conn_cback(uint8_t /* handle */, const RawAddress& bd_addr, uint8_t event,
                       tAVDT_CTRL* p_data, uint8_t scb_index) {
  uint16_t evt = 0;
  tBTA_AV_SCB* p_scb = NULL;

  if (event == BTA_AR_AVDT_CONN_EVT || event == AVDT_CONNECT_IND_EVT ||
      event == AVDT_DISCONNECT_IND_EVT) {
    evt = BTA_AV_SIG_CHG_EVT;
    if (event == AVDT_DISCONNECT_IND_EVT) {
      p_scb = bta_av_addr_to_scb(bd_addr);
    } else if (event == AVDT_CONNECT_IND_EVT) {
      log::verbose("CONN_IND is ACP:{}", p_data->hdr.err_param);
    }

    tBTA_AV_STR_MSG* p_msg = (tBTA_AV_STR_MSG*)osi_malloc(sizeof(tBTA_AV_STR_MSG));
    p_msg->hdr.event = evt;
    p_msg->hdr.layer_specific = event;
    p_msg->hdr.offset = p_data->hdr.err_param;
    p_msg->bd_addr = bd_addr;
    p_msg->scb_index = scb_index;
    if (p_scb) {
      log::verbose("bta_handle x{:x}, role x{:x}", p_scb->hndl, p_scb->role);
    }
    log::info("conn_cback bd_addr: {}, scb_index: {}", bd_addr, scb_index);
    bta_sys_sendmsg(p_msg);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_a2dp_report_cback
 *
 * Description      A2DP report callback.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_a2dp_report_cback(uint8_t /* handle */, AVDT_REPORT_TYPE /* type */,
                                     tAVDT_REPORT_DATA* /* p_data */) {
  /* Do not need to handle report data for now.
   * This empty function is here for conformance reasons. */
}

/*******************************************************************************
 *
 * Function         bta_av_api_register
 *
 * Description      allocate stream control block,
 *                  register the service to stack
 *                  create SDP record
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_api_register(tBTA_AV_DATA* p_data) {
  tBTA_AV_REGISTER reg_data;
  tBTA_AV_SCB* p_scb; /* stream control block */
  AvdtpRcb reg;
  AvdtpStreamConfig avdtp_stream_config;
  char* p_service_name;
  tBTA_UTL_COD cod;
  uint8_t local_role = 0;

  if (bta_av_cb.disabling || (bta_av_cb.features == 0)) {
    log::warn("AV instance (features={:#x}, reg_audio={:#x}) is not ready for app_id {}",
              bta_av_cb.features, bta_av_cb.reg_audio, p_data->api_reg.app_id);
    tBTA_AV_API_REG* p_buf = (tBTA_AV_API_REG*)osi_malloc(sizeof(tBTA_AV_API_REG));
    memcpy(p_buf, &p_data->api_reg, sizeof(tBTA_AV_API_REG));
    bta_sys_sendmsg_delayed(p_buf, std::chrono::milliseconds(kEnablingAttemptsIntervalMs));
    return;
  }

  avdtp_stream_config.Reset();
  if (btif_av_src_sink_coexist_enabled()) {
    local_role = (p_data->api_reg.service_uuid == UUID_SERVCLASS_AUDIO_SINK) ? AVDT_TSEP_SNK
                                                                             : AVDT_TSEP_SRC;
  }

  reg_data.status = BTA_AV_FAIL_RESOURCES;
  reg_data.app_id = p_data->api_reg.app_id;
  reg_data.chnl = (tBTA_AV_CHNL)p_data->hdr.layer_specific;

  const uint16_t avrcp_version = AVRC_GetProfileVersion();
  log::info("AVRCP version used for sdp: 0x{:x}", avrcp_version);
  uint16_t profile_initialized = p_data->api_reg.service_uuid;
  if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK) {
    p_bta_av_cfg = get_bta_avk_cfg();
  } else if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE) {
    p_bta_av_cfg = &bta_av_cfg;

    if (avrcp_version == AVRC_REV_1_3) {
      log::info("AVRCP 1.3 capabilites used");
      p_bta_av_cfg = &bta_av_cfg_compatibility;
    }
  }

  log::verbose("profile: 0x{:x}", profile_initialized);
  if (p_bta_av_cfg == NULL) {
    log::error("AV configuration is null!");
    return;
  }

  do {
    p_scb = nullptr;
    if (btif_av_src_sink_coexist_enabled()) {
      p_scb = bta_av_find_scb(reg_data.chnl, reg_data.app_id);
    }
    if (p_scb == nullptr) {
      p_scb = bta_av_alloc_scb(reg_data.chnl);
    }
    if (p_scb == NULL) {
      log::error("failed to alloc SCB");
      break;
    }

    reg_data.hndl = p_scb->hndl;
    p_scb->app_id = reg_data.app_id;

    /* initialize the stream control block */
    reg_data.status = BTA_AV_SUCCESS;

    if ((btif_av_src_sink_coexist_enabled() && !(bta_av_cb.reg_role & (1 << local_role))) ||
        (!btif_av_src_sink_coexist_enabled() && bta_av_cb.reg_audio == 0)) {
      /* the first channel registered. register to AVDTP */
      reg.ctrl_mtu = 672;
      reg.ret_tout = BTA_AV_RET_TOUT;
      reg.sig_tout = BTA_AV_SIG_TOUT;
      reg.idle_tout = BTA_AV_IDLE_TOUT;
      reg.scb_index = p_scb->hdi;
      bta_ar_reg_avdt(&reg, bta_av_conn_cback);
      bta_sys_role_chg_register(&bta_av_sys_rs_cback);

      /* create remote control TG service if required */
      if (bta_av_cb.features & (BTA_AV_FEAT_RCTG)) {
        /* register with no authorization; let AVDTP use authorization instead
         */
        bta_ar_reg_avct();

        if (com::android::bluetooth::flags::avrcp_sdp_records()) {
          // Add target record for
          // a) A2DP sink profile. or
          // b) A2DP source profile only if new avrcp service is disabled.
          if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK ||
              (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE && !is_new_avrcp_enabled())) {
            bta_ar_reg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET, "AV Remote Control Target", "",
                            p_bta_av_cfg->avrc_tg_cat, (bta_av_cb.features & BTA_AV_FEAT_BROWSE),
                            avrcp_version);
          }
        } else {
          /* For the Audio Sink role we support additional TG to support
           * absolute volume.
           */
          if (is_new_avrcp_enabled()) {
            log::verbose(
                    "newavrcp is the owner of the AVRCP Target SDP record. Don't "
                    "create the SDP record");
          } else {
            log::verbose("newavrcp is not enabled. Create SDP record");

            if (btif_av_src_sink_coexist_enabled()) {
              bta_ar_reg_avrc_for_src_sink_coexist(
                      UUID_SERVCLASS_AV_REM_CTRL_TARGET, "AV Remote Control Target", NULL,
                      p_bta_av_cfg->avrc_tg_cat, static_cast<tBTA_SYS_ID>(BTA_ID_AV + local_role),
                      (bta_av_cb.features & BTA_AV_FEAT_BROWSE), avrcp_version);
            } else {
              bta_ar_reg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET, "AV Remote Control Target", NULL,
                              p_bta_av_cfg->avrc_tg_cat, (bta_av_cb.features & BTA_AV_FEAT_BROWSE),
                              avrcp_version);
            }
          }
        }
      }

      /* Set the Capturing service class bit */
      if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE) {
        cod.service = BTM_COD_SERVICE_CAPTURING;
      } else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK) {
        cod.service = BTM_COD_SERVICE_RENDERING;
      }
      utl_set_device_class(&cod, BTA_UTL_SET_COD_SERVICE_CLASS);
    } /* if 1st channel */

    /* get stream configuration and create stream */
    avdtp_stream_config.cfg.num_codec = 1;
    avdtp_stream_config.nsc_mask = AvdtpStreamConfig::AVDT_NSC_RECONFIG;
    if (!(bta_av_cb.features & BTA_AV_FEAT_PROTECT)) {
      avdtp_stream_config.nsc_mask |= AvdtpStreamConfig::AVDT_NSC_SECURITY;
    }
    log::verbose("nsc_mask: 0x{:x}", avdtp_stream_config.nsc_mask);

    if (p_data->api_reg.p_service_name[0] == 0) {
      p_service_name = NULL;
    } else {
      p_service_name = p_data->api_reg.p_service_name;
    }

    p_scb->suspend_sup = true;
    p_scb->recfg_sup = true;

    avdtp_stream_config.scb_index = p_scb->hdi;
    avdtp_stream_config.p_avdt_ctrl_cback = &bta_av_proc_stream_evt;

    /* set up the audio stream control block */
    p_scb->p_cos = &bta_av_a2dp_cos;
    p_scb->media_type = AVDT_MEDIA_TYPE_AUDIO;
    avdtp_stream_config.cfg.psc_mask = AVDT_PSC_TRANS;
    avdtp_stream_config.media_type = AVDT_MEDIA_TYPE_AUDIO;
    avdtp_stream_config.mtu = MAX_3MBPS_AVDTP_MTU;
    btav_a2dp_codec_index_t codec_index_min = BTAV_A2DP_CODEC_INDEX_SOURCE_MIN;
    btav_a2dp_codec_index_t codec_index_max = BTAV_A2DP_CODEC_INDEX_SOURCE_MAX;

    if (bta_av_cb.features & BTA_AV_FEAT_REPORT) {
      avdtp_stream_config.cfg.psc_mask |= AVDT_PSC_REPORT;
      avdtp_stream_config.p_report_cback = bta_av_a2dp_report_cback;
    }
    if (bta_av_cb.features & BTA_AV_FEAT_DELAY_RPT) {
      avdtp_stream_config.cfg.psc_mask |= AVDT_PSC_DELAY_RPT;
    }

    if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE) {
      avdtp_stream_config.tsep = AVDT_TSEP_SRC;
      codec_index_min = BTAV_A2DP_CODEC_INDEX_SOURCE_MIN;
      codec_index_max = BTAV_A2DP_CODEC_INDEX_SOURCE_MAX;
    } else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK) {
      avdtp_stream_config.tsep = AVDT_TSEP_SNK;
      avdtp_stream_config.p_sink_data_cback = bta_av_sink_data_cback;
      codec_index_min = BTAV_A2DP_CODEC_INDEX_SINK_MIN;
      codec_index_max = BTAV_A2DP_CODEC_INDEX_SINK_MAX;
    }

    if (btif_av_src_sink_coexist_enabled()) {
      for (int xx = codec_index_min; xx < codec_index_max; xx++) {
        p_scb->seps[xx].av_handle = 0;
      }
    } else {
      for (int xx = 0; xx < BTAV_A2DP_CODEC_INDEX_MAX; xx++) {
        p_scb->seps[xx].av_handle = 0;
      }
    }

    /* keep the configuration in the stream control block */
    p_scb->cfg = avdtp_stream_config.cfg;
    for (int i = codec_index_min; i < codec_index_max; i++) {
      btav_a2dp_codec_index_t codec_index = static_cast<btav_a2dp_codec_index_t>(i);
      if (!bta_av_co_is_supported_codec(codec_index)) {
        log::warn("Skipping the codec index for codec index {}", i);
        continue;
      }
      if (!(*bta_av_a2dp_cos.init)(codec_index, &avdtp_stream_config.cfg)) {
        continue;
      }
      if (AVDT_CreateStream(p_scb->app_id, &p_scb->seps[codec_index].av_handle,
                            avdtp_stream_config) != AVDT_SUCCESS) {
        log::warn("bta_handle=0x{:x} (app_id {}) failed to alloc an SEP index:{}", p_scb->hndl,
                  p_scb->app_id, codec_index);
        continue;
      }
      /* Save a copy of the codec */
      memcpy(p_scb->seps[codec_index].codec_info, avdtp_stream_config.cfg.codec_info,
             AVDT_CODEC_SIZE);
      p_scb->seps[codec_index].tsep = avdtp_stream_config.tsep;
      if (avdtp_stream_config.tsep == AVDT_TSEP_SNK) {
        p_scb->seps[codec_index].p_app_sink_data_cback = p_data->api_reg.p_app_sink_data_cback;
      } else {
        /* In case of A2DP SOURCE we don't need a callback to
         * handle media packets.
         */
        p_scb->seps[codec_index].p_app_sink_data_cback = NULL;
      }
    }
    if ((btif_av_src_sink_coexist_enabled() && !(bta_av_cb.reg_role & (1 << local_role))) ||
        (!btif_av_src_sink_coexist_enabled() && !bta_av_cb.reg_audio)) {
      bta_av_cb.sdp_a2dp_handle = 0;
      bta_av_cb.sdp_a2dp_snk_handle = 0;
      if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE) {
        /* create the SDP records on the 1st audio channel */
        bta_av_cb.sdp_a2dp_handle = get_legacy_stack_sdp_api()->handle.SDP_CreateRecord();
        A2DP_AddRecord(UUID_SERVCLASS_AUDIO_SOURCE, p_service_name, NULL, A2DP_SUPF_PLAYER,
                       bta_av_cb.sdp_a2dp_handle);
        bta_sys_add_uuid(UUID_SERVCLASS_AUDIO_SOURCE);
      } else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK) {
        bta_av_cb.sdp_a2dp_snk_handle = get_legacy_stack_sdp_api()->handle.SDP_CreateRecord();
        A2DP_AddRecord(UUID_SERVCLASS_AUDIO_SINK, p_service_name, NULL, A2DP_SUPF_PLAYER,
                       bta_av_cb.sdp_a2dp_snk_handle);
        bta_sys_add_uuid(UUID_SERVCLASS_AUDIO_SINK);
      }
      /* start listening when A2DP is registered */
      if (bta_av_cb.features & BTA_AV_FEAT_RCTG) {
        bta_av_rc_create(&bta_av_cb, AVCT_ROLE_ACCEPTOR, 0, BTA_AV_NUM_LINKS + 1);
      }

      /* if the AV and AVK are both supported, it cannot support the CT role
       */
      if (bta_av_cb.features & (BTA_AV_FEAT_RCCT)) {
        /* if TG is not supported, we need to register to AVCT now */
        if ((bta_av_cb.features & (BTA_AV_FEAT_RCTG)) == 0) {
          bta_ar_reg_avct();
          bta_av_rc_create(&bta_av_cb, AVCT_ROLE_ACCEPTOR, 0, BTA_AV_NUM_LINKS + 1);
        }
        if (com::android::bluetooth::flags::avrcp_sdp_records()) {
          // Add control record for sink profile.
          // Also adds control record for source profile when new avrcp service is not enabled.
          if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK ||
              (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE && !is_new_avrcp_enabled())) {
            uint16_t control_version = AVRC_GetControlProfileVersion();
            /* Create an SDP record as AVRC CT. We create 1.3 for SOURCE
             * because we rely on feature bits being scanned by external
             * devices more than the profile version itself.
             */
            if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE && !is_new_avrcp_enabled()) {
              control_version = AVRC_REV_1_3;
            }
            if (!btif_av_src_sink_coexist_enabled() &&
                profile_initialized == UUID_SERVCLASS_AUDIO_SINK) {
              control_version = AVRC_REV_1_6;
            }
            bta_ar_reg_avrc(UUID_SERVCLASS_AV_REMOTE_CONTROL, "AV Remote Control", "",
                            p_bta_av_cfg->avrc_ct_cat, (bta_av_cb.features & BTA_AV_FEAT_BROWSE),
                            control_version);
          }
        } else {
          /* create an SDP record as AVRC CT. We create 1.3 for SOURCE
           * because we rely on feature bits being scanned by external
           * devices more than the profile version itself.
           *
           * We create 1.4 for SINK since we support browsing.
           */
          if (btif_av_src_sink_coexist_enabled()) {
            if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE) {
              bta_ar_reg_avrc_for_src_sink_coexist(
                      UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL, NULL, p_bta_av_cfg->avrc_ct_cat,
                      BTA_ID_AV, (bta_av_cb.features & BTA_AV_FEAT_BROWSE), AVRC_REV_1_5);
            } else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK) {
              bta_ar_reg_avrc_for_src_sink_coexist(UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL, NULL,
                                                   p_bta_av_cfg->avrc_ct_cat, BTA_ID_AVK,
                                                   (bta_av_cb.features & BTA_AV_FEAT_BROWSE),
                                                   AVRC_GetControlProfileVersion());
            }
          } else {
            if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE && !is_new_avrcp_enabled()) {
              bta_ar_reg_avrc(UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL, NULL,
                              p_bta_av_cfg->avrc_ct_cat, (bta_av_cb.features & BTA_AV_FEAT_BROWSE),
                              AVRC_REV_1_3);
            } else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK) {
              bta_ar_reg_avrc(UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL, NULL,
                              p_bta_av_cfg->avrc_ct_cat, (bta_av_cb.features & BTA_AV_FEAT_BROWSE),
                              AVRC_REV_1_6);
            }
          }
        }
      }
    }
    bta_av_cb.reg_audio |= BTA_AV_HNDL_TO_MSK(p_scb->hdi);
    log::verbose("reg_audio: 0x{:x}", bta_av_cb.reg_audio);
  } while (0);

  if (btif_av_src_sink_coexist_enabled()) {
    bta_av_cb.reg_role |= (1 << local_role);
    reg_data.peer_sep =
            (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE) ? AVDT_TSEP_SNK : AVDT_TSEP_SRC;

    /* there are too much check depend on it's only source */
    if ((profile_initialized == UUID_SERVCLASS_AUDIO_SINK) &&
        (bta_av_cb.reg_role & (1 << AVDT_TSEP_SRC))) {
      p_bta_av_cfg = &bta_av_cfg;

      if (avrcp_version == AVRC_REV_1_3) {  // ver if need
        log::verbose("AVRCP 1.3 capabilites used");
        p_bta_av_cfg = &bta_av_cfg_compatibility;
      }
    }
  }

  /* call callback with register event */
  tBTA_AV bta_av_data;
  bta_av_data.reg = reg_data;
  (*bta_av_cb.p_cback)(BTA_AV_REGISTER_EVT, &bta_av_data);
}

/*******************************************************************************
 *
 * Function         bta_av_api_deregister
 *
 * Description      de-register a channel
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_api_deregister(tBTA_AV_DATA* p_data) {
  tBTA_AV_SCB* p_scb = bta_av_hndl_to_scb(p_data->hdr.layer_specific);

  if (p_scb) {
    p_scb->deregistering = true;
    bta_av_ssm_execute(p_scb, BTA_AV_API_CLOSE_EVT, p_data);
  } else {
    bta_av_dereg_comp(p_data);
  }
}

/*******************************************************************************
 *
 * Function         bta_av_ci_data
 *
 * Description      Forward the BTA_AV_CI_SRC_DATA_READY_EVT to stream state
 *                  machine.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_ci_data(tBTA_AV_DATA* p_data) {
  tBTA_AV_SCB* p_scb;
  int i;
  uint8_t chnl = (uint8_t)p_data->hdr.layer_specific;

  for (i = 0; i < BTA_AV_NUM_STRS; i++) {
    p_scb = bta_av_cb.p_scb[i];

    if (p_scb && p_scb->chnl == chnl) {
      bta_av_ssm_execute(p_scb, BTA_AV_SRC_DATA_READY_EVT, p_data);
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_rpc_conn
 *
 * Description      report report channel open
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_rpc_conn(tBTA_AV_DATA* /* p_data */) {}

/*******************************************************************************
 *
 * Function         bta_av_chk_start
 *
 * Description      if this is audio channel, check if more than one audio
 *                  channel is connected & already started.
 *
 * Returns          true, if need api_start
 *
 ******************************************************************************/
bool bta_av_chk_start(tBTA_AV_SCB* p_scb) {
  bool start = false;

  if ((p_scb->chnl == BTA_AV_CHNL_AUDIO) && (bta_av_cb.audio_open_cnt >= 2) &&
      (((p_scb->role & BTA_AV_ROLE_AD_ACP) == 0) ||      // Outgoing connection or
       (bta_av_cb.features & BTA_AV_FEAT_ACP_START))) {  // Auto-starting option
    // More than one audio channel is connected.
    // If this is the 2nd stream as ACP, give INT a chance to issue the START
    // command.
    for (int i = 0; i < BTA_AV_NUM_STRS; i++) {
      tBTA_AV_SCB* p_scbi = bta_av_cb.p_scb[i];
      if (p_scbi && p_scbi->chnl == BTA_AV_CHNL_AUDIO && p_scbi->co_started) {
        start = true;
        // May need to update the flush timeout of this already started stream
        if (p_scbi->co_started != bta_av_cb.audio_open_cnt) {
          p_scbi->co_started = bta_av_cb.audio_open_cnt;
        }
      }
    }
  }

  log::info("peer {} channel:{} bta_av_cb.audio_open_cnt:{} role:0x{:x} features:0x{:x} start:{}",
            p_scb->PeerAddress(), p_scb->chnl, bta_av_cb.audio_open_cnt, p_scb->role,
            bta_av_cb.features, start);
  return start;
}

/*******************************************************************************
 *
 * Function         bta_av_restore_switch
 *
 * Description      assume that the caller of this function already makes
 *                  sure that there's only one ACL connection left
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_restore_switch(void) {
  tBTA_AV_CB* p_cb = &bta_av_cb;
  int i;
  uint8_t mask;

  log::verbose("reg_audio: 0x{:x}", bta_av_cb.reg_audio);
  for (i = 0; i < BTA_AV_NUM_STRS; i++) {
    mask = BTA_AV_HNDL_TO_MSK(i);
    if (p_cb->conn_audio == mask) {
      if (p_cb->p_scb[i]) {
        get_btm_client_interface().link_policy.BTM_unblock_role_switch_for(
                p_cb->p_scb[i]->PeerAddress());
      }
      break;
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_sys_rs_cback
 *
 * Description      Receives the role change event from dm
 *
 * Returns          (BTA_SYS_ROLE_CHANGE, new_role, hci_status, p_bda)
 *
 ******************************************************************************/
static void bta_av_sys_rs_cback(tBTA_SYS_CONN_STATUS /* status */, tHCI_ROLE new_role,
                                tHCI_STATUS hci_status, const RawAddress& peer_addr) {
  int i;
  tBTA_AV_SCB* p_scb = NULL;
  tHCI_ROLE cur_role;
  uint8_t peer_idx = 0;

  log::verbose("peer {} new_role:{} hci_status:0x{:x} bta_av_cb.rs_idx:{}", peer_addr, new_role,
               hci_status, bta_av_cb.rs_idx);

  for (i = 0; i < BTA_AV_NUM_STRS; i++) {
    /* loop through all the SCBs to find matching peer addresses and report the
     * role change event */
    /* note that more than one SCB (a2dp & vdp) maybe waiting for this event */
    p_scb = bta_av_cb.p_scb[i];
    if (p_scb && p_scb->PeerAddress() == peer_addr) {
      tBTA_AV_ROLE_RES* p_buf = (tBTA_AV_ROLE_RES*)osi_malloc(sizeof(tBTA_AV_ROLE_RES));
      log::verbose("peer {} found: new_role:{}, hci_status:0x{:x} bta_handle:0x{:x}", peer_addr,
                   new_role, hci_status, p_scb->hndl);
      p_buf->hdr.event = BTA_AV_ROLE_CHANGE_EVT;
      p_buf->hdr.layer_specific = p_scb->hndl;
      p_buf->new_role = new_role;
      p_buf->hci_status = hci_status;
      bta_sys_sendmsg(p_buf);

      peer_idx = p_scb->hdi + 1; /* Handle index for the peer_addr */
    }
  }

  /* restore role switch policy, if role switch failed */
  if ((HCI_SUCCESS != hci_status) &&
      (get_btm_client_interface().link_policy.BTM_GetRole(peer_addr, &cur_role) ==
       tBTM_STATUS::BTM_SUCCESS) &&
      (cur_role == HCI_ROLE_PERIPHERAL)) {
    get_btm_client_interface().link_policy.BTM_unblock_role_switch_for(peer_addr);
  }

  /* if BTA_AvOpen() was called for other device, which caused the role switch
   * of the peer_addr,  */
  /* we need to continue opening process for the BTA_AvOpen(). */
  if ((bta_av_cb.rs_idx != 0) && (bta_av_cb.rs_idx != peer_idx)) {
    if ((bta_av_cb.rs_idx - 1) < BTA_AV_NUM_STRS) {
      p_scb = bta_av_cb.p_scb[bta_av_cb.rs_idx - 1];
    }
    if (p_scb && p_scb->q_tag == BTA_AV_Q_TAG_OPEN) {
      log::verbose("peer {} rs_idx:{}, bta_handle:0x{:x} q_tag:{}", p_scb->PeerAddress(),
                   bta_av_cb.rs_idx, p_scb->hndl, p_scb->q_tag);

      if (HCI_SUCCESS == hci_status || HCI_ERR_NO_CONNECTION == hci_status) {
        p_scb->q_info.open.switch_res = BTA_AV_RS_OK;
      } else {
        log::error("peer {} (p_scb peer {}) role switch failed: new_role:{} hci_status:0x{:x}",
                   peer_addr, p_scb->PeerAddress(), new_role, hci_status);
        p_scb->q_info.open.switch_res = BTA_AV_RS_FAIL;
      }

      /* Continue av open process */
      bta_av_do_disc_a2dp(p_scb, (tBTA_AV_DATA*)&(p_scb->q_info.open));
    }

    bta_av_cb.rs_idx = 0;
  }
}

/*******************************************************************************
 *
 * Function         bta_av_sco_chg_cback
 *
 * Description      receive & process the SCO connection up/down event from sys.
 *                  call setup also triggers this callback, to suspend av before
 *                  SCO activity happens, or to resume av once call ends.
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_av_sco_chg_cback(tBTA_SYS_CONN_STATUS status, uint8_t num_sco_links,
                                 uint8_t /* app_id */, const RawAddress& peer_addr) {
  tBTA_AV_SCB* p_scb;
  int i;
  tBTA_AV_API_STOP stop;

  log::info("status={}, num_links={}", bta_sys_conn_status_text(status), num_sco_links);
  if (num_sco_links) {
    bta_av_cb.sco_occupied = true;
    log::debug("SCO occupied peer:{} status:{}", peer_addr, bta_sys_conn_status_text(status));

    if (bta_av_cb.features & BTA_AV_FEAT_NO_SCO_SSPD) {
      return;
    }

    /* either BTA_SYS_SCO_OPEN or BTA_SYS_SCO_CLOSE with remaining active SCO */
    for (i = 0; i < BTA_AV_NUM_STRS; i++) {
      p_scb = bta_av_cb.p_scb[i];

      if (p_scb && p_scb->co_started && (!p_scb->sco_suspend)) {
        log::verbose("suspending scb:{}", i);
        /* scb is used and started, not suspended automatically */
        p_scb->sco_suspend = true;
        stop.flush = false;
        stop.suspend = true;
        stop.reconfig_stop = false;
        bta_av_ssm_execute(p_scb, BTA_AV_AP_STOP_EVT, (tBTA_AV_DATA*)&stop);
      }
    }
  } else {
    bta_av_cb.sco_occupied = false;
    log::debug("SCO unoccupied peer:{} status:{}", peer_addr, bta_sys_conn_status_text(status));

    if (bta_av_cb.features & BTA_AV_FEAT_NO_SCO_SSPD) {
      return;
    }

    for (i = 0; i < BTA_AV_NUM_STRS; i++) {
      p_scb = bta_av_cb.p_scb[i];

      if (p_scb && p_scb->sco_suspend) /* scb is used and suspended for SCO */
      {
        log::verbose("starting scb:{}", i);
        bta_av_ssm_execute(p_scb, BTA_AV_AP_START_EVT, NULL);
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_av_switch_if_needed
 *
 * Description      This function checks if there is another existing AV
 *                  channel that is local as peripheral role.
 *                  If so, role switch and remove it from link policy.
 *
 * Returns          true, if role switch is done
 *
 ******************************************************************************/
bool bta_av_switch_if_needed(tBTA_AV_SCB* /*p_scb*/) {
  // TODO: A workaround for devices that are connected first, become
  // Central, and block follow-up role changes - b/72122792 .
  return false;
#if 0
  uint8_t role;
  bool needed = false;
  tBTA_AV_SCB* p_scbi;
  int i;
  uint8_t mask;

  for (i = 0; i < BTA_AV_NUM_STRS; i++) {
    mask = BTA_AV_HNDL_TO_MSK(i);
    p_scbi = bta_av_cb.p_scb[i];
    if (p_scbi && (p_scb->hdi != i) &&   /* not the original channel */
        ((bta_av_cb.conn_audio & mask))) /* connected audio */
    {
      get_btm_client_interface().link_policy.BTM_GetRole(p_scbi->PeerAddress(), &role);
      /* this channel is open - clear the role switch link policy for this link
       */
      if (HCI_ROLE_CENTRAL != role) {
        if (bta_av_cb.features & BTA_AV_FEAT_CENTRAL)
          get_btm_client_interface().link_policy.BTM_block_role_switch_for(p_scbi->PeerAddress());
        if (BTM_CMD_STARTED !=
            BTM_SwitchRole(p_scbi->PeerAddress(), HCI_ROLE_CENTRAL)) {
          /* can not switch role on SCBI
           * start the timer on SCB - because this function is ONLY called when
           * SCB gets API_OPEN */
          bta_sys_start_timer(p_scb->avrc_ct_timer, BTA_AV_RS_TIME_VAL,
                              BTA_AV_AVRC_TIMER_EVT, p_scb->hndl);
        }
        needed = true;
        /* mark the original channel as waiting for RS result */
        bta_av_cb.rs_idx = p_scb->hdi + 1;
        break;
      }
    }
  }
  return needed;
#endif
}

/*******************************************************************************
 *
 * Function         bta_av_link_role_ok
 *
 * Description      This function checks if the SCB has existing ACL connection
 *                  If so, check if the link role fits the requirements.
 *
 * Returns          true, if role is ok
 *
 ******************************************************************************/
bool bta_av_link_role_ok(tBTA_AV_SCB* p_scb, uint8_t bits) {
  tHCI_ROLE role;
  if (get_btm_client_interface().link_policy.BTM_GetRole(p_scb->PeerAddress(), &role) !=
      tBTM_STATUS::BTM_SUCCESS) {
    log::warn("Unable to find link role for device:{}", p_scb->PeerAddress());
    return true;
  }

  if (role != HCI_ROLE_CENTRAL && (A2DP_BitsSet(bta_av_cb.conn_audio) > bits)) {
    log::info(
            "Switch link role to central peer:{} bta_handle:0x{:x} current_role:{} "
            "conn_audio:0x{:x} bits:{} features:0x{:x}",
            p_scb->PeerAddress(), p_scb->hndl, RoleText(role), bta_av_cb.conn_audio, bits,
            bta_av_cb.features);
    const tBTM_STATUS status =
            get_btm_client_interface().link_policy.BTM_SwitchRoleToCentral(p_scb->PeerAddress());
    switch (status) {
      case tBTM_STATUS::BTM_CMD_STARTED:
        break;
      case tBTM_STATUS::BTM_MODE_UNSUPPORTED:
      case tBTM_STATUS::BTM_DEV_RESTRICT_LISTED:
        // Role switch can never happen, but indicate to caller
        // a result such that a timer will not start to repeatedly
        // try something not possible.
        log::error("Link can never role switch to central device:{}", p_scb->PeerAddress());
        break;
      default:
        /* can not switch role on SCB - start the timer on SCB */
        p_scb->wait |= BTA_AV_WAIT_ROLE_SW_RES_START;
        log::error("Unable to switch role to central device:{} error:{}", p_scb->PeerAddress(),
                   btm_status_text(status));
        return false;
    }
  }
  return true;
}

/*******************************************************************************
 *
 * Function         bta_av_dup_audio_buf
 *
 * Description      dup the audio data to the q_info.a2dp of other audio
 *                  channels
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_av_dup_audio_buf(tBTA_AV_SCB* p_scb, BT_HDR* p_buf) {
  /* Test whether there is more than one audio channel connected */
  if ((p_buf == NULL) || (bta_av_cb.audio_open_cnt < 2)) {
    return;
  }

  uint16_t copy_size = BT_HDR_SIZE + p_buf->len + p_buf->offset;
  for (int i = 0; i < BTA_AV_NUM_STRS; i++) {
    tBTA_AV_SCB* p_scbi = bta_av_cb.p_scb[i];

    if (i == p_scb->hdi) {
      continue; /* Ignore the original channel */
    }
    if ((p_scbi == NULL) || !p_scbi->co_started) {
      continue; /* Ignore if SCB is not used or started */
    }
    if (!(bta_av_cb.conn_audio & BTA_AV_HNDL_TO_MSK(i))) {
      continue; /* Audio is not connected */
    }

    /* Enqueue the data */
    BT_HDR* p_new = (BT_HDR*)osi_malloc(copy_size);
    memcpy(p_new, p_buf, copy_size);
    list_append(p_scbi->a2dp_list, p_new);

    if (list_length(p_scbi->a2dp_list) > p_bta_av_cfg->audio_mqs) {
      // Drop the oldest packet
      bta_av_co_audio_drop(p_scbi->hndl, p_scbi->PeerAddress());
      BT_HDR* p_buf_drop = static_cast<BT_HDR*>(list_front(p_scbi->a2dp_list));
      list_remove(p_scbi->a2dp_list, p_buf_drop);
      osi_free(p_buf_drop);
    }
  }
}

static void bta_av_non_state_machine_event(uint16_t event, tBTA_AV_DATA* p_data) {
  switch (event) {
    case BTA_AV_API_ENABLE_EVT:
      bta_av_api_enable(p_data);
      break;
    case BTA_AV_API_REGISTER_EVT:
      bta_av_api_register(p_data);
      break;
    case BTA_AV_API_DEREGISTER_EVT:
      bta_av_api_deregister(p_data);
      break;
    case BTA_AV_API_DISCONNECT_EVT:
      bta_av_api_disconnect(p_data);
      break;
    case BTA_AV_API_SET_LATENCY_EVT:
      bta_av_api_set_latency(p_data);
      break;
    case BTA_AV_CI_SRC_DATA_READY_EVT:
      bta_av_ci_data(p_data);
      break;
    case BTA_AV_SIG_CHG_EVT:
      bta_av_sig_chg(p_data);
      break;
    case BTA_AV_SIGNALLING_TIMER_EVT:
      bta_av_signalling_timer(p_data);
      break;
    case BTA_AV_SDP_AVRC_DISC_EVT:
      bta_av_rc_disc_done(p_data);
      break;
    case BTA_AV_AVRC_CLOSE_EVT:
      bta_av_rc_closed(p_data);
      break;
    case BTA_AV_AVRC_BROWSE_OPEN_EVT:
      bta_av_rc_browse_opened(p_data);
      break;
    case BTA_AV_AVRC_BROWSE_CLOSE_EVT:
      bta_av_rc_browse_closed(p_data);
      break;
    case BTA_AV_CONN_CHG_EVT:
      bta_av_conn_chg(p_data);
      break;
    case BTA_AV_DEREG_COMP_EVT:
      bta_av_dereg_comp(p_data);
      break;
    case BTA_AV_AVDT_RPT_CONN_EVT:
      bta_av_rpc_conn(p_data);
      break;
    case BTA_AV_API_PEER_SEP_EVT:
      bta_av_api_set_peer_sep(p_data);
      break;
  }
}

void bta_av_sm_execute(tBTA_AV_CB* p_cb, uint16_t event, tBTA_AV_DATA* p_data) {
  log::verbose("AV event=0x{:x}({}) state={}({})", event, bta_av_evt_code(event), p_cb->state,
               bta_av_st_code(p_cb->state));
  switch (p_cb->state) {
    case BTA_AV_INIT_ST:
      switch (event) {
        case BTA_AV_API_DISABLE_EVT:
          bta_av_disable(p_cb, p_data);
          break;
        case BTA_AV_API_META_RSP_EVT:
          bta_av_rc_free_rsp(p_cb, p_data);
          break;
        case BTA_AV_AVRC_OPEN_EVT:
          p_cb->state = BTA_AV_OPEN_ST;
          bta_av_rc_opened(p_cb, p_data);
          break;
        case BTA_AV_AVRC_MSG_EVT:
          bta_av_rc_free_browse_msg(p_cb, p_data);
          break;
      }
      break;
    case BTA_AV_OPEN_ST:
      switch (event) {
        case BTA_AV_API_DISABLE_EVT:
          p_cb->state = BTA_AV_INIT_ST;
          bta_av_disable(p_cb, p_data);
          break;
        case BTA_AV_API_REMOTE_CMD_EVT:
          bta_av_rc_remote_cmd(p_cb, p_data);
          break;
        case BTA_AV_API_VENDOR_CMD_EVT:
          bta_av_rc_vendor_cmd(p_cb, p_data);
          break;
        case BTA_AV_API_VENDOR_RSP_EVT:
          bta_av_rc_vendor_rsp(p_cb, p_data);
          break;
        case BTA_AV_API_META_RSP_EVT:
          bta_av_rc_meta_rsp(p_cb, p_data);
          break;
        case BTA_AV_API_RC_CLOSE_EVT:
          bta_av_rc_close(p_cb, p_data);
          break;
        case BTA_AV_AVRC_OPEN_EVT:
          bta_av_rc_opened(p_cb, p_data);
          break;
        case BTA_AV_AVRC_MSG_EVT:
          bta_av_rc_msg(p_cb, p_data);
          break;
        case BTA_AV_AVRC_NONE_EVT:
          p_cb->state = BTA_AV_INIT_ST;
          break;
      }
      break;
  }
}

/*******************************************************************************
 *
 * Function         bta_av_hdl_event
 *
 * Description      Advanced audio/video main event handling function.
 *
 *
 * Returns          bool
 *
 ******************************************************************************/
bool bta_av_hdl_event(const BT_HDR_RIGID* p_msg) {
  if (p_msg->event > BTA_AV_LAST_EVT) {
    return true; /* to free p_msg */
  }
  if (p_msg->event >= BTA_AV_FIRST_NSM_EVT) {
    log::verbose("AV nsm event=0x{:x}({})", p_msg->event, bta_av_evt_code(p_msg->event));
    bta_av_non_state_machine_event(p_msg->event, (tBTA_AV_DATA*)p_msg);
  } else if (p_msg->event >= BTA_AV_FIRST_SM_EVT && p_msg->event <= BTA_AV_LAST_SM_EVT) {
    log::verbose("AV sm event=0x{:x}({})", p_msg->event, bta_av_evt_code(p_msg->event));
    /* state machine events */
    bta_av_sm_execute(&bta_av_cb, p_msg->event, (tBTA_AV_DATA*)p_msg);
  } else {
    log::verbose("bta_handle=0x{:x}", p_msg->layer_specific);
    /* stream state machine events */
    bta_av_ssm_execute(bta_av_hndl_to_scb(p_msg->layer_specific), p_msg->event,
                       (tBTA_AV_DATA*)p_msg);
  }
  return true;
}

/*****************************************************************************
 *  Debug Functions
 ****************************************************************************/
/*******************************************************************************
 *
 * Function         bta_av_st_code
 *
 * Description
 *
 * Returns          char *
 *
 ******************************************************************************/
static const char* bta_av_st_code(uint8_t state) {
  switch (state) {
    case BTA_AV_INIT_ST:
      return "INIT";
    case BTA_AV_OPEN_ST:
      return "OPEN";
    default:
      return "unknown";
  }
}
/*******************************************************************************
 *
 * Function         bta_av_evt_code
 *
 * Description
 *
 * Returns          char *
 *
 ******************************************************************************/
const char* bta_av_evt_code(uint16_t evt_code) {
  switch (evt_code) {
    case BTA_AV_API_DISABLE_EVT:
      return "API_DISABLE";
    case BTA_AV_API_REMOTE_CMD_EVT:
      return "API_REMOTE_CMD";
    case BTA_AV_API_VENDOR_CMD_EVT:
      return "API_VENDOR_CMD";
    case BTA_AV_API_VENDOR_RSP_EVT:
      return "API_VENDOR_RSP";
    case BTA_AV_API_META_RSP_EVT:
      return "API_META_RSP_EVT";
    case BTA_AV_API_RC_CLOSE_EVT:
      return "API_RC_CLOSE";
    case BTA_AV_AVRC_OPEN_EVT:
      return "AVRC_OPEN";
    case BTA_AV_AVRC_MSG_EVT:
      return "AVRC_MSG";
    case BTA_AV_AVRC_NONE_EVT:
      return "AVRC_NONE";

    case BTA_AV_API_OPEN_EVT:
      return "API_OPEN";
    case BTA_AV_API_CLOSE_EVT:
      return "API_CLOSE";
    case BTA_AV_AP_START_EVT:
      return "AP_START";
    case BTA_AV_AP_STOP_EVT:
      return "AP_STOP";
    case BTA_AV_API_RECONFIG_EVT:
      return "API_RECONFIG";
    case BTA_AV_API_PROTECT_REQ_EVT:
      return "API_PROTECT_REQ";
    case BTA_AV_API_PROTECT_RSP_EVT:
      return "API_PROTECT_RSP";
    case BTA_AV_API_RC_OPEN_EVT:
      return "API_RC_OPEN";
    case BTA_AV_SRC_DATA_READY_EVT:
      return "SRC_DATA_READY";
    case BTA_AV_CI_SETCONFIG_OK_EVT:
      return "CI_SETCONFIG_OK";
    case BTA_AV_CI_SETCONFIG_FAIL_EVT:
      return "CI_SETCONFIG_FAIL";
    case BTA_AV_SDP_DISC_OK_EVT:
      return "SDP_DISC_OK";
    case BTA_AV_SDP_DISC_FAIL_EVT:
      return "SDP_DISC_FAIL";
    case BTA_AV_STR_DISC_OK_EVT:
      return "STR_DISC_OK";
    case BTA_AV_STR_DISC_FAIL_EVT:
      return "STR_DISC_FAIL";
    case BTA_AV_STR_GETCAP_OK_EVT:
      return "STR_GETCAP_OK";
    case BTA_AV_STR_GETCAP_FAIL_EVT:
      return "STR_GETCAP_FAIL";
    case BTA_AV_STR_OPEN_OK_EVT:
      return "STR_OPEN_OK";
    case BTA_AV_STR_OPEN_FAIL_EVT:
      return "STR_OPEN_FAIL";
    case BTA_AV_STR_START_OK_EVT:
      return "STR_START_OK";
    case BTA_AV_STR_START_FAIL_EVT:
      return "STR_START_FAIL";
    case BTA_AV_STR_CLOSE_EVT:
      return "STR_CLOSE";
    case BTA_AV_STR_CONFIG_IND_EVT:
      return "STR_CONFIG_IND";
    case BTA_AV_STR_SECURITY_IND_EVT:
      return "STR_SECURITY_IND";
    case BTA_AV_STR_SECURITY_CFM_EVT:
      return "STR_SECURITY_CFM";
    case BTA_AV_STR_WRITE_CFM_EVT:
      return "STR_WRITE_CFM";
    case BTA_AV_STR_SUSPEND_CFM_EVT:
      return "STR_SUSPEND_CFM";
    case BTA_AV_STR_RECONFIG_CFM_EVT:
      return "STR_RECONFIG_CFM";
    case BTA_AV_AVRC_TIMER_EVT:
      return "AVRC_TIMER";
    case BTA_AV_AVDT_CONNECT_EVT:
      return "AVDT_CONNECT";
    case BTA_AV_AVDT_DISCONNECT_EVT:
      return "AVDT_DISCONNECT";
    case BTA_AV_ROLE_CHANGE_EVT:
      return "ROLE_CHANGE";
    case BTA_AV_AVDT_DELAY_RPT_EVT:
      return "AVDT_DELAY_RPT";
    case BTA_AV_ACP_CONNECT_EVT:
      return "ACP_CONNECT";
    case BTA_AV_API_OFFLOAD_START_EVT:
      return "OFFLOAD_START";
    case BTA_AV_API_OFFLOAD_START_RSP_EVT:
      return "OFFLOAD_START_RSP";

    case BTA_AV_API_ENABLE_EVT:
      return "API_ENABLE";
    case BTA_AV_API_REGISTER_EVT:
      return "API_REG";
    case BTA_AV_API_DEREGISTER_EVT:
      return "API_DEREG";
    case BTA_AV_API_DISCONNECT_EVT:
      return "API_DISCNT";
    case BTA_AV_CI_SRC_DATA_READY_EVT:
      return "CI_DATA_READY";
    case BTA_AV_SIG_CHG_EVT:
      return "SIG_CHG";
    case BTA_AV_SIGNALLING_TIMER_EVT:
      return "SIGNALLING_TIMER";
    case BTA_AV_SDP_AVRC_DISC_EVT:
      return "SDP_AVRC_DISC";
    case BTA_AV_AVRC_CLOSE_EVT:
      return "AVRC_CLOSE";
    case BTA_AV_AVRC_BROWSE_OPEN_EVT:
      return "AVRC_BROWSE_OPEN";
    case BTA_AV_AVRC_BROWSE_CLOSE_EVT:
      return "AVRC_BROWSE_CLOSE";
    case BTA_AV_CONN_CHG_EVT:
      return "CONN_CHG";
    case BTA_AV_DEREG_COMP_EVT:
      return "DEREG_COMP";
    case BTA_AV_AVDT_RPT_CONN_EVT:
      return "RPT_CONN";
    default:
      return "unknown";
  }
}

void bta_debug_av_dump(int fd) {
  dprintf(fd, "\nBTA AV State:\n");
  dprintf(fd, "  State Machine State: %s\n", bta_av_st_code(bta_av_cb.state));
  dprintf(fd, "  SDP A2DP source handle: %d\n", bta_av_cb.sdp_a2dp_handle);
  dprintf(fd, "  SDP A2DP sink handle: %d\n", bta_av_cb.sdp_a2dp_snk_handle);
  dprintf(fd, "  Features: 0x%x\n", bta_av_cb.features);
  dprintf(fd, "  SDP handle: %d\n", bta_av_cb.handle);
  dprintf(fd, "  Disabling: %s\n", bta_av_cb.disabling ? "true" : "false");
  dprintf(fd, "  SCO occupied: %s\n", bta_av_cb.sco_occupied ? "true" : "false");
  dprintf(fd, "  Connected audio channels: %d\n", bta_av_cb.audio_open_cnt);
  dprintf(fd, "  Connected audio channels mask: 0x%x\n", bta_av_cb.conn_audio);
  dprintf(fd, "  Registered audio channels mask: 0x%x\n", bta_av_cb.reg_audio);
  dprintf(fd, "  Connected LCBs mask: 0x%x\n", bta_av_cb.conn_lcb);
  dprintf(fd, "  Offload start pending handle: %d\n", bta_av_cb.offload_start_pending_hndl);
  dprintf(fd, "  Offload started handle: %d\n", bta_av_cb.offload_started_hndl);

  for (size_t i = 0; i < sizeof(bta_av_cb.lcb) / sizeof(bta_av_cb.lcb[0]); i++) {
    const tBTA_AV_LCB& lcb = bta_av_cb.lcb[i];
    if (lcb.addr.IsEmpty()) {
      continue;
    }
    dprintf(fd, "\n  Link control block: %zu peer: %s\n", i, ADDRESS_TO_LOGGABLE_CSTR(lcb.addr));
    dprintf(fd, "    Connected stream handle mask: 0x%x\n", lcb.conn_msk);
    dprintf(fd, "    Index(+1) to LCB: %d\n", lcb.lidx);
  }
  for (size_t i = 0; i < BTA_AV_NUM_STRS; i++) {
    const tBTA_AV_SCB* p_scb = bta_av_cb.p_scb[i];
    if (p_scb == nullptr) {
      continue;
    }
    if (p_scb->PeerAddress().IsEmpty()) {
      continue;
    }
    dprintf(fd, "\n  BTA ID: %zu peer: %s\n", i, ADDRESS_TO_LOGGABLE_CSTR(p_scb->PeerAddress()));
    dprintf(fd, "    SDP discovery started: %s\n", p_scb->sdp_discovery_started ? "true" : "false");
    for (size_t j = 0; j < BTAV_A2DP_CODEC_INDEX_MAX; j++) {
      const tBTA_AV_SEP& sep = p_scb->seps[j];
      if (sep.av_handle == 0) {
        continue;
      }
      dprintf(fd, "    SEP ID: %zu\n", j);
      dprintf(fd, "      SEP AVDTP handle: %d\n", sep.av_handle);
      dprintf(fd, "      Local SEP type: %d\n", sep.tsep);
      dprintf(fd, "      Codec: %s\n", A2DP_CodecName(sep.codec_info));
    }
    dprintf(fd, "    BTA info tag: %d\n", p_scb->q_tag);
    dprintf(fd, "    API Open peer: %s\n", ADDRESS_TO_LOGGABLE_CSTR(p_scb->q_info.open.bd_addr));
    dprintf(fd, "      Use AVRCP: %s\n", p_scb->q_info.open.use_rc ? "true" : "false");
    dprintf(fd, "      Switch result: %d\n", p_scb->q_info.open.switch_res);
    dprintf(fd, "      Initiator UUID: 0x%x\n", p_scb->q_info.open.uuid);
    dprintf(fd, "    Saved API Open peer: %s\n", ADDRESS_TO_LOGGABLE_CSTR(p_scb->open_api.bd_addr));
    dprintf(fd, "      Use AVRCP: %s\n", p_scb->open_api.use_rc ? "true" : "false");
    dprintf(fd, "      Switch result: %d\n", p_scb->open_api.switch_res);
    dprintf(fd, "      Initiator UUID: 0x%x\n", p_scb->open_api.uuid);
    dprintf(fd, "  Link signalling timer: %s\n",
            alarm_is_scheduled(p_scb->link_signalling_timer) ? "Scheduled" : "Not scheduled");
    dprintf(fd, "  Accept signalling timer: %s\n",
            alarm_is_scheduled(p_scb->accept_signalling_timer) ? "Scheduled" : "Not scheduled");
    // TODO: Print p_scb->sep_info[], cfg, avrc_ct_timer, current_codec ?
    dprintf(fd, "    L2CAP Channel ID: %d\n", p_scb->l2c_cid);
    dprintf(fd, "    Stream MTU: %d\n", p_scb->stream_mtu);
    dprintf(fd, "    AVDTP version: 0x%x\n", p_scb->AvdtpVersion());
    dprintf(fd, "    Media type: %d\n", p_scb->media_type);
    dprintf(fd, "    Congested: %s\n", p_scb->cong ? "true" : "false");
    dprintf(fd, "    Open status: %d\n", p_scb->open_status);
    dprintf(fd, "    Channel: %d\n", p_scb->chnl);
    dprintf(fd, "    BTA handle: 0x%x\n", p_scb->hndl);
    dprintf(fd, "    Protocol service capabilities mask: 0x%x\n", p_scb->cur_psc_mask);
    dprintf(fd, "    AVDTP handle: %d\n", p_scb->avdt_handle);
    dprintf(fd, "    Stream control block index: %d\n", p_scb->hdi);
    dprintf(fd, "    State machine state: %s(%d)\n", bta_av_sst_code(p_scb->state), p_scb->state);
    dprintf(fd, "    AVDTP label: 0x%x\n", p_scb->avdt_label);
    dprintf(fd, "    Application ID: %d\n", p_scb->app_id);
    dprintf(fd, "    Role: 0x%x\n", p_scb->role);
    dprintf(fd, "    Queued L2CAP buffers: %d\n", p_scb->l2c_bufs);
    dprintf(fd, "    AVRCP allowed: %s\n", p_scb->use_rc ? "true" : "false");
    dprintf(fd, "    Stream started: %s\n", p_scb->started ? "true" : "false");
    dprintf(fd, "    Stream call-out started: %d\n", p_scb->co_started);
    dprintf(fd, "    AVDTP Reconfig supported: %s\n", p_scb->recfg_sup ? "true" : "false");
    dprintf(fd, "    AVDTP Suspend supported: %s\n", p_scb->suspend_sup ? "true" : "false");
    dprintf(fd, "    Deregistering: %s\n", p_scb->deregistering ? "true" : "false");
    dprintf(fd, "    SCO automatic Suspend: %s\n", p_scb->sco_suspend ? "true" : "false");
    dprintf(fd, "    Incoming/outgoing connection collusion mask: 0x%x\n", p_scb->coll_mask);
    dprintf(fd, "    Wait mask: 0x%x\n", p_scb->wait);
    dprintf(fd, "    Don't use RTP header: %s\n", p_scb->no_rtp_header ? "true" : "false");
    dprintf(fd, "    Intended UUID of Initiator to connect to: 0x%x\n", p_scb->uuid_int);
  }
}
