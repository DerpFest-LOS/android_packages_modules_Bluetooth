/******************************************************************************
 *
 *  Copyright 2011-2012 Broadcom Corporation
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
 *  This is the implementation of the API for the advanced audio/video (AV)
 *  subsystem of BTA, Broadcom's Bluetooth application layer for mobile
 *  phones.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth-a2dp"

#include "bta_av_api.h"

#include <bluetooth/log.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "avdt_api.h"
#include "avrc_defs.h"
#include "bta/av/bta_av_int.h"
#include "bta_api.h"
#include "bta_sys.h"
#include "btif/include/btif_av.h"
#include "internal_include/bt_target.h"
#include "osi/include/allocator.h"
#include "osi/include/compat.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_uuid16.h"
#include "types/raw_address.h"

using namespace bluetooth;

/*****************************************************************************
 *  Constants
 ****************************************************************************/

static const tBTA_SYS_REG bta_av_reg = {bta_av_hdl_event, BTA_AvDisable};

/*******************************************************************************
 *
 * Function         BTA_AvEnable
 *
 * Description      Enable the advanced audio/video service. When the enable
 *                  operation is complete the callback function will be
 *                  called with a BTA_AV_ENABLE_EVT. This function must
 *                  be called before other function in the AV API are
 *                  called.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvEnable(tBTA_AV_FEAT features, tBTA_AV_CBACK* p_cback) {
  tBTA_AV_API_ENABLE* p_buf = (tBTA_AV_API_ENABLE*)osi_malloc(sizeof(tBTA_AV_API_ENABLE));

  /* register with BTA system manager */
  bta_sys_register(BTA_ID_AV, &bta_av_reg);

  p_buf->hdr.event = BTA_AV_API_ENABLE_EVT;
  p_buf->p_cback = p_cback;
  p_buf->features = features;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvDisable
 *
 * Description      Disable the advanced audio/video service.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvDisable(void) {
  BT_HDR_RIGID* p_buf = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));

  bta_sys_deregister(BTA_ID_AV);
  p_buf->event = BTA_AV_API_DISABLE_EVT;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvRegister
 *
 * Description      Register the audio or video service to stack. When the
 *                  operation is complete the callback function will be
 *                  called with a BTA_AV_REGISTER_EVT. This function must
 *                  be called before AVDT stream is open.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvRegister(tBTA_AV_CHNL chnl, const char* p_service_name, uint8_t app_id,
                    tBTA_AV_SINK_DATA_CBACK* p_sink_data_cback, uint16_t service_uuid) {
  tBTA_AV_API_REG* p_buf = (tBTA_AV_API_REG*)osi_malloc(sizeof(tBTA_AV_API_REG));

  p_buf->hdr.layer_specific = chnl;
  p_buf->hdr.event = BTA_AV_API_REGISTER_EVT;
  if (p_service_name) {
    osi_strlcpy(p_buf->p_service_name, p_service_name, BTA_SERVICE_NAME_LEN);
  } else {
    p_buf->p_service_name[0] = 0;
  }
  p_buf->app_id = app_id;
  p_buf->p_app_sink_data_cback = p_sink_data_cback;
  p_buf->service_uuid = service_uuid;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvDeregister
 *
 * Description      Deregister the audio or video service
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvDeregister(tBTA_AV_HNDL hndl) {
  BT_HDR_RIGID* p_buf = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));

  p_buf->layer_specific = hndl;
  p_buf->event = BTA_AV_API_DEREGISTER_EVT;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvOpen
 *
 * Description      Opens an advanced audio/video connection to a peer device.
 *                  When connection is open callback function is called
 *                  with a BTA_AV_OPEN_EVT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvOpen(const RawAddress& bd_addr, tBTA_AV_HNDL handle, bool use_rc, uint16_t uuid) {
  log::info("peer {} bta_handle:0x{:x} use_rc={} uuid=0x{:x}", bd_addr, handle, use_rc, uuid);

  tBTA_AV_API_OPEN* p_buf = (tBTA_AV_API_OPEN*)osi_malloc(sizeof(tBTA_AV_API_OPEN));

  p_buf->hdr.event = BTA_AV_API_OPEN_EVT;
  p_buf->hdr.layer_specific = handle;
  p_buf->bd_addr = bd_addr;
  p_buf->use_rc = use_rc;
  p_buf->switch_res = BTA_AV_RS_NONE;
  p_buf->uuid = uuid;
  if (btif_av_src_sink_coexist_enabled()) {
    if (p_buf->uuid == AVDT_TSEP_SRC) {
      p_buf->uuid = UUID_SERVCLASS_AUDIO_SOURCE;
      p_buf->incoming = TRUE;
    } else if (p_buf->uuid == AVDT_TSEP_SNK) {
      p_buf->uuid = UUID_SERVCLASS_AUDIO_SINK;
      p_buf->incoming = TRUE;
    } else {
      p_buf->incoming = FALSE;
    }
  }

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvClose
 *
 * Description      Close the current streams.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvClose(tBTA_AV_HNDL handle) {
  log::info("bta_handle:0x{:x}", handle);

  BT_HDR_RIGID* p_buf = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));

  p_buf->event = BTA_AV_API_CLOSE_EVT;
  p_buf->layer_specific = handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvDisconnect
 *
 * Description      Close the connection to the address.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvDisconnect(tBTA_AV_HNDL handle) {
  log::info("bta_handle=0x{:x}", handle);

  tBTA_AV_API_DISCNT* p_buf = (tBTA_AV_API_DISCNT*)osi_malloc(sizeof(tBTA_AV_API_DISCNT));

  p_buf->hdr.event = BTA_AV_API_DISCONNECT_EVT;
  p_buf->hdr.layer_specific = handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvStart
 *
 * Description      Start audio/video stream data transfer.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvStart(tBTA_AV_HNDL handle, bool use_latency_mode) {
  log::info("Starting audio/video stream data transfer bta_handle:{}, use_latency_mode:{}", handle,
            use_latency_mode);

  tBTA_AV_DO_START* p_buf = (tBTA_AV_DO_START*)osi_malloc(sizeof(tBTA_AV_DO_START));
  p_buf->hdr.event = BTA_AV_AP_START_EVT;
  p_buf->hdr.layer_specific = handle;
  p_buf->use_latency_mode = use_latency_mode;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvOffloadStart
 *
 * Description      Start a2dp audio offloading.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvOffloadStart(tBTA_AV_HNDL hndl) {
  log::info("bta_handle=0x{:x}", hndl);

  BT_HDR_RIGID* p_buf = (BT_HDR_RIGID*)osi_malloc(sizeof(BT_HDR_RIGID));

  p_buf->event = BTA_AV_API_OFFLOAD_START_EVT;
  p_buf->layer_specific = hndl;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvStop
 *
 * Description      Stop audio/video stream data transfer.
 *                  If suspend is true, this function sends AVDT suspend signal
 *                  to the connected peer(s).
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvStop(tBTA_AV_HNDL handle, bool suspend) {
  log::info("bta_handle=0x{:x} suspend={}", handle, suspend);

  tBTA_AV_API_STOP* p_buf = (tBTA_AV_API_STOP*)osi_malloc(sizeof(tBTA_AV_API_STOP));

  p_buf->hdr.event = BTA_AV_AP_STOP_EVT;
  p_buf->hdr.layer_specific = handle;
  p_buf->flush = true;
  p_buf->suspend = suspend;
  p_buf->reconfig_stop = false;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvReconfig
 *
 * Description      Reconfigure the audio/video stream.
 *                  If suspend is true, this function tries the
 *                  suspend/reconfigure procedure first.
 *                  If suspend is false or when suspend/reconfigure fails,
 *                  this function closes and re-opens the AVDT connection.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvReconfig(tBTA_AV_HNDL hndl, bool suspend, uint8_t sep_info_idx, uint8_t* p_codec_info,
                    uint8_t num_protect, const uint8_t* p_protect_info) {
  log::info("bta_handle=0x{:x} suspend={} sep_info_idx={}", hndl, suspend, sep_info_idx);

  tBTA_AV_API_RCFG* p_buf = (tBTA_AV_API_RCFG*)osi_malloc(sizeof(tBTA_AV_API_RCFG) + num_protect);

  p_buf->hdr.layer_specific = hndl;
  p_buf->hdr.event = BTA_AV_API_RECONFIG_EVT;
  p_buf->num_protect = num_protect;
  p_buf->suspend = suspend;
  p_buf->sep_info_idx = sep_info_idx;
  p_buf->p_protect_info = (uint8_t*)(p_buf + 1);
  memcpy(p_buf->codec_info, p_codec_info, AVDT_CODEC_SIZE);
  memcpy(p_buf->p_protect_info, p_protect_info, num_protect);

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvProtectReq
 *
 * Description      Send a content protection request.  This function can only
 *                  be used if AV is enabled with feature BTA_AV_FEAT_PROTECT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvProtectReq(tBTA_AV_HNDL hndl, uint8_t* p_data, uint16_t len) {
  tBTA_AV_API_PROTECT_REQ* p_buf =
          (tBTA_AV_API_PROTECT_REQ*)osi_malloc(sizeof(tBTA_AV_API_PROTECT_REQ) + len);

  p_buf->hdr.layer_specific = hndl;
  p_buf->hdr.event = BTA_AV_API_PROTECT_REQ_EVT;
  p_buf->len = len;
  if (p_data == NULL) {
    p_buf->p_data = NULL;
  } else {
    p_buf->p_data = (uint8_t*)(p_buf + 1);
    memcpy(p_buf->p_data, p_data, len);
  }

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvProtectRsp
 *
 * Description      Send a content protection response.  This function must
 *                  be called if a BTA_AV_PROTECT_REQ_EVT is received.
 *                  This function can only be used if AV is enabled with
 *                  feature BTA_AV_FEAT_PROTECT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvProtectRsp(tBTA_AV_HNDL hndl, uint8_t error_code, uint8_t* p_data, uint16_t len) {
  tBTA_AV_API_PROTECT_RSP* p_buf =
          (tBTA_AV_API_PROTECT_RSP*)osi_malloc(sizeof(tBTA_AV_API_PROTECT_RSP) + len);

  p_buf->hdr.layer_specific = hndl;
  p_buf->hdr.event = BTA_AV_API_PROTECT_RSP_EVT;
  p_buf->len = len;
  p_buf->error_code = error_code;
  if (p_data == NULL) {
    p_buf->p_data = NULL;
  } else {
    p_buf->p_data = (uint8_t*)(p_buf + 1);
    memcpy(p_buf->p_data, p_data, len);
  }

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvRemoteCmd
 *
 * Description      Send a remote control command.  This function can only
 *                  be used if AV is enabled with feature BTA_AV_FEAT_RCCT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvRemoteCmd(uint8_t rc_handle, uint8_t label, tBTA_AV_RC rc_id, tBTA_AV_STATE key_state) {
  tBTA_AV_API_REMOTE_CMD* p_buf =
          (tBTA_AV_API_REMOTE_CMD*)osi_malloc(sizeof(tBTA_AV_API_REMOTE_CMD));

  p_buf->hdr.event = BTA_AV_API_REMOTE_CMD_EVT;
  p_buf->hdr.layer_specific = rc_handle;
  p_buf->msg.op_id = rc_id;
  p_buf->msg.state = key_state;
  p_buf->msg.p_pass_data = NULL;
  p_buf->msg.pass_len = 0;
  p_buf->label = label;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvRemoteVendorUniqueCmd
 *
 * Description      Send a remote control command with Vendor Unique rc_id.
 *                  This function can only be used if AV is enabled with
 *                  feature BTA_AV_FEAT_RCCT.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvRemoteVendorUniqueCmd(uint8_t rc_handle, uint8_t label, tBTA_AV_STATE key_state,
                                 uint8_t* p_msg, uint8_t buf_len) {
  tBTA_AV_API_REMOTE_CMD* p_buf =
          (tBTA_AV_API_REMOTE_CMD*)osi_malloc(sizeof(tBTA_AV_API_REMOTE_CMD) + buf_len);

  p_buf->label = label;
  p_buf->hdr.event = BTA_AV_API_REMOTE_CMD_EVT;
  p_buf->hdr.layer_specific = rc_handle;
  p_buf->msg.op_id = AVRC_ID_VENDOR;
  p_buf->msg.state = key_state;
  p_buf->msg.pass_len = buf_len;
  if (p_msg == NULL) {
    p_buf->msg.p_pass_data = NULL;
  } else {
    p_buf->msg.p_pass_data = (uint8_t*)(p_buf + 1);
    memcpy(p_buf->msg.p_pass_data, p_msg, buf_len);
  }
  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvVendorCmd
 *
 * Description      Send a vendor dependent remote control command.  This
 *                  function can only be used if AV is enabled with feature
 *                  BTA_AV_FEAT_VENDOR.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvVendorCmd(uint8_t rc_handle, uint8_t label, tBTA_AV_CODE cmd_code, uint8_t* p_data,
                     uint16_t len) {
  tBTA_AV_API_VENDOR* p_buf = (tBTA_AV_API_VENDOR*)osi_malloc(sizeof(tBTA_AV_API_VENDOR) + len);

  p_buf->hdr.event = BTA_AV_API_VENDOR_CMD_EVT;
  p_buf->hdr.layer_specific = rc_handle;
  p_buf->msg.hdr.ctype = cmd_code;
  p_buf->msg.hdr.subunit_type = AVRC_SUB_PANEL;
  p_buf->msg.hdr.subunit_id = 0;
  p_buf->msg.company_id = p_bta_av_cfg->company_id;
  p_buf->label = label;
  p_buf->msg.vendor_len = len;
  if (p_data == NULL) {
    p_buf->msg.p_vendor_data = NULL;
  } else {
    p_buf->msg.p_vendor_data = (uint8_t*)(p_buf + 1);
    memcpy(p_buf->msg.p_vendor_data, p_data, len);
  }

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvVendorRsp
 *
 * Description      Send a vendor dependent remote control response.
 *                  This function must be called if a BTA_AV_VENDOR_CMD_EVT
 *                  is received. This function can only be used if AV is
 *                  enabled with feature BTA_AV_FEAT_VENDOR.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvVendorRsp(uint8_t rc_handle, uint8_t label, tBTA_AV_CODE rsp_code, uint8_t* p_data,
                     uint16_t len, uint32_t company_id) {
  tBTA_AV_API_VENDOR* p_buf = (tBTA_AV_API_VENDOR*)osi_malloc(sizeof(tBTA_AV_API_VENDOR) + len);

  p_buf->hdr.event = BTA_AV_API_VENDOR_RSP_EVT;
  p_buf->hdr.layer_specific = rc_handle;
  p_buf->msg.hdr.ctype = rsp_code;
  p_buf->msg.hdr.subunit_type = AVRC_SUB_PANEL;
  p_buf->msg.hdr.subunit_id = 0;
  if (company_id) {
    p_buf->msg.company_id = company_id;
  } else {
    p_buf->msg.company_id = p_bta_av_cfg->company_id;
  }
  p_buf->label = label;
  p_buf->msg.vendor_len = len;
  if (p_data == NULL) {
    p_buf->msg.p_vendor_data = NULL;
  } else {
    p_buf->msg.p_vendor_data = (uint8_t*)(p_buf + 1);
    memcpy(p_buf->msg.p_vendor_data, p_data, len);
  }

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvOpenRc
 *
 * Description      Open an AVRCP connection toward the device with the
 *                  specified handle
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvOpenRc(tBTA_AV_HNDL handle) {
  tBTA_AV_API_OPEN_RC* p_buf = (tBTA_AV_API_OPEN_RC*)osi_malloc(sizeof(tBTA_AV_API_OPEN_RC));

  p_buf->hdr.event = BTA_AV_API_RC_OPEN_EVT;
  p_buf->hdr.layer_specific = handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvCloseRc
 *
 * Description      Close an AVRCP connection
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvCloseRc(uint8_t rc_handle) {
  tBTA_AV_API_CLOSE_RC* p_buf = (tBTA_AV_API_CLOSE_RC*)osi_malloc(sizeof(tBTA_AV_API_CLOSE_RC));

  p_buf->hdr.event = BTA_AV_API_RC_CLOSE_EVT;
  p_buf->hdr.layer_specific = rc_handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvMetaRsp
 *
 * Description      Send a Metadata/Advanced Control response. The message
 *                  contained in p_pkt can be composed with AVRC utility
 *                  functions.
 *                  This function can only be used if AV is enabled with feature
 *                  BTA_AV_FEAT_METADATA.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvMetaRsp(uint8_t rc_handle, uint8_t label, tBTA_AV_CODE rsp_code, BT_HDR* p_pkt) {
  tBTA_AV_API_META_RSP* p_buf = (tBTA_AV_API_META_RSP*)osi_malloc(sizeof(tBTA_AV_API_META_RSP));

  p_buf->hdr.event = BTA_AV_API_META_RSP_EVT;
  p_buf->hdr.layer_specific = rc_handle;
  p_buf->rsp_code = rsp_code;
  p_buf->p_pkt = p_pkt;
  p_buf->is_rsp = true;
  p_buf->label = label;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvMetaCmd
 *
 * Description      Send a Metadata/Advanced Control command. The message
 *contained
 *                  in p_pkt can be composed with AVRC utility functions.
 *                  This function can only be used if AV is enabled with feature
 *                  BTA_AV_FEAT_METADATA.
 *                  This message is sent only when the peer supports the TG
 *role.
 *8                  The only command makes sense right now is the absolute
 *volume command.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvMetaCmd(uint8_t rc_handle, uint8_t label, tBTA_AV_CMD cmd_code, BT_HDR* p_pkt) {
  tBTA_AV_API_META_RSP* p_buf = (tBTA_AV_API_META_RSP*)osi_malloc(sizeof(tBTA_AV_API_META_RSP));

  p_buf->hdr.event = BTA_AV_API_META_RSP_EVT;
  p_buf->hdr.layer_specific = rc_handle;
  p_buf->p_pkt = p_pkt;
  p_buf->rsp_code = cmd_code;
  p_buf->is_rsp = false;
  p_buf->label = label;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvSetLatency
 *
 * Description      Set audio/video stream latency.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvSetLatency(tBTA_AV_HNDL handle, bool is_low_latency) {
  log::info("Set audio/video stream low latency bta_handle:{}, is_low_latency:{}", handle,
            is_low_latency);

  tBTA_AV_API_SET_LATENCY* p_buf =
          (tBTA_AV_API_SET_LATENCY*)osi_malloc(sizeof(tBTA_AV_API_SET_LATENCY));
  p_buf->hdr.event = BTA_AV_API_SET_LATENCY_EVT;
  p_buf->hdr.layer_specific = handle;
  p_buf->is_low_latency = is_low_latency;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_AvSetPeerSep
 *
 * Description      Set peer sep in order to delete wrong avrcp handle
 *                  there are may be two avrcp handle at start, delete the
 *                  wrong when a2dp connected
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AvSetPeerSep(const RawAddress& bdaddr, uint8_t sep) {
  tBTA_AV_API_PEER_SEP* p_buf = (tBTA_AV_API_PEER_SEP*)osi_malloc(sizeof(tBTA_AV_API_PEER_SEP));

  p_buf->hdr.event = BTA_AV_API_PEER_SEP_EVT;
  p_buf->addr = bdaddr;
  p_buf->sep = sep;

  bta_sys_sendmsg(p_buf);
}
