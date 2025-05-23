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
 *  This file contains the HID HOST API in the subsystem of BTA.
 *
 ******************************************************************************/

#define LOG_TAG "bt_bta_hh"

#include "bta_hh_api.h"

#include <bluetooth/log.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bta/hh/bta_hh_int.h"
#include "bta/sys/bta_sys.h"
#include "hiddefs.h"
#include "osi/include/allocator.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/main_thread.h"
#include "types/ble_address_with_type.h"
#include "types/bluetooth/uuid.h"

using namespace bluetooth;

/*****************************************************************************
 *  Constants
 ****************************************************************************/

/**
 * Android Headtracker Service UUIDs
 */
const Uuid ANDROID_HEADTRACKER_SERVICE_UUID =
        Uuid::FromString(ANDROID_HEADTRACKER_SERVICE_UUID_STRING);
const Uuid ANDROID_HEADTRACKER_VERSION_CHARAC_UUID =
        Uuid::FromString(ANDROID_HEADTRACKER_VERSION_CHARAC_UUID_STRING);
const Uuid ANDROID_HEADTRACKER_CONTROL_CHARAC_UUID =
        Uuid::FromString(ANDROID_HEADTRACKER_CONTROL_CHARAC_UUID_STRING);
const Uuid ANDROID_HEADTRACKER_REPORT_CHARAC_UUID =
        Uuid::FromString(ANDROID_HEADTRACKER_REPORT_CHARAC_UUID_STRING);

static const tBTA_SYS_REG bta_hh_reg = {bta_hh_hdl_event, BTA_HhDisable};

/*******************************************************************************
 *
 * Function         BTA_HhEnable
 *
 * Description      Enable the HID host.  This function must be called before
 *                  any other functions in the HID host API are called. When the
 *                  enable operation is complete the callback function will be
 *                  called with BTA_HH_ENABLE_EVT.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhEnable(tBTA_HH_CBACK* p_cback, bool enable_hid, bool enable_hogp) {
  /* register with BTA system manager */
  bta_sys_register(BTA_ID_HH, &bta_hh_reg);

  post_on_bt_main([p_cback, enable_hid, enable_hogp]() {
    bta_hh_api_enable(p_cback, enable_hid, enable_hogp);
  });
}

/*******************************************************************************
 *
 * Function         BTA_HhDisable
 *
 * Description      Disable the HID host. If the server is currently
 *                  connected, the connection will be closed.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhDisable(void) {
  bta_sys_deregister(BTA_ID_HH);

  post_on_bt_main([]() { bta_hh_api_disable(); });
}

/*******************************************************************************
 *
 * Function         BTA_HhClose
 *
 * Description      Disconnect a connection.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhClose(uint8_t dev_handle) {
  BT_HDR* p_buf = (BT_HDR*)osi_calloc(sizeof(BT_HDR));

  p_buf->event = BTA_HH_API_CLOSE_EVT;
  p_buf->layer_specific = (uint16_t)dev_handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_HhOpen
 *
 * Description      Connect to a device of specified BD address in specified
 *                  protocol mode and security level.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhOpen(const tAclLinkSpec& link_spec) {
  tBTA_HH_API_CONN* p_buf = (tBTA_HH_API_CONN*)osi_calloc(sizeof(tBTA_HH_API_CONN));
  tBTA_HH_PROTO_MODE mode = BTA_HH_PROTO_RPT_MODE;

  p_buf->hdr.event = BTA_HH_API_OPEN_EVT;
  p_buf->hdr.layer_specific = BTA_HH_INVALID_HANDLE;
  p_buf->mode = mode;
  p_buf->link_spec = link_spec;

  bta_sys_sendmsg((void*)p_buf);
}

/*******************************************************************************
 *
 * Function  bta_hh_snd_write_dev
 *
 ******************************************************************************/
static void bta_hh_snd_write_dev(uint8_t dev_handle, uint8_t t_type, uint8_t param, uint16_t data,
                                 uint8_t rpt_id, BT_HDR* p_data) {
  tBTA_HH_CMD_DATA* p_buf = (tBTA_HH_CMD_DATA*)osi_calloc(sizeof(tBTA_HH_CMD_DATA));

  p_buf->hdr.event = BTA_HH_API_WRITE_DEV_EVT;
  p_buf->hdr.layer_specific = (uint16_t)dev_handle;
  p_buf->t_type = t_type;
  p_buf->data = data;
  p_buf->param = param;
  p_buf->p_data = p_data;
  p_buf->rpt_id = rpt_id;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_HhSetReport
 *
 * Description      send SET_REPORT to device.
 *
 * Parameter        dev_handle: device handle
 *                  r_type:     report type, could be BTA_HH_RPTT_OUTPUT or
 *                              BTA_HH_RPTT_FEATURE.
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhSetReport(uint8_t dev_handle, tBTA_HH_RPT_TYPE r_type, BT_HDR* p_data) {
  bta_hh_snd_write_dev(dev_handle, HID_TRANS_SET_REPORT, r_type, 0, 0, p_data);
}
/*******************************************************************************
 *
 * Function         BTA_HhGetReport
 *
 * Description      Send a GET_REPORT to HID device.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhGetReport(uint8_t dev_handle, tBTA_HH_RPT_TYPE r_type, uint8_t rpt_id,
                     uint16_t buf_size) {
  uint8_t param = (buf_size) ? (r_type | 0x08) : r_type;

  bta_hh_snd_write_dev(dev_handle, HID_TRANS_GET_REPORT, param, buf_size, rpt_id, NULL);
}
/*******************************************************************************
 *
 * Function         BTA_HhSetProtoMode
 *
 * Description      This function set the protocol mode at specified HID handle
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhSetProtoMode(uint8_t dev_handle, tBTA_HH_PROTO_MODE p_type) {
  bta_hh_snd_write_dev(dev_handle, HID_TRANS_SET_PROTOCOL, (uint8_t)p_type, 0, 0, NULL);
}
/*******************************************************************************
 *
 * Function         BTA_HhGetProtoMode
 *
 * Description      This function get protocol mode information.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhGetProtoMode(uint8_t dev_handle) {
  bta_hh_snd_write_dev(dev_handle, HID_TRANS_GET_PROTOCOL, 0, 0, 0, NULL);
}
/*******************************************************************************
 *
 * Function         BTA_HhSetIdle
 *
 * Description      send SET_IDLE to device.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhSetIdle(uint8_t dev_handle, uint16_t idle_rate) {
  bta_hh_snd_write_dev(dev_handle, HID_TRANS_SET_IDLE, 0, idle_rate, 0, NULL);
}

/*******************************************************************************
 *
 * Function         BTA_HhGetIdle
 *
 * Description      Send a GET_IDLE from HID device.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhGetIdle(uint8_t dev_handle) {
  bta_hh_snd_write_dev(dev_handle, HID_TRANS_GET_IDLE, 0, 0, 0, NULL);
}
/*******************************************************************************
 *
 * Function         BTA_HhSendCtrl
 *
 * Description      Send a control command to HID device.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhSendCtrl(uint8_t dev_handle, tBTA_HH_TRANS_CTRL_TYPE c_type) {
  bta_hh_snd_write_dev(dev_handle, HID_TRANS_CONTROL, (uint8_t)c_type, 0, 0, NULL);
}
/*******************************************************************************
 *
 * Function         BTA_HhSendData
 *
 * Description      This function send DATA transaction to HID device.
 *
 * Parameter        dev_handle: device handle
 *                  link_spec : remote device acl link specification
 *                  p_data: data to be sent in the DATA transaction; or
 *                          the data to be write into the Output Report of a LE
 *                          HID device. The report is identified the report ID
 *                          which is the value of the byte
 *                          (uint8_t *)(p_buf + 1) + *p_buf->offset.
 *                          p_data->layer_specific needs to be set to the report
 *                          type. It can be OUTPUT report, or FEATURE report.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhSendData(uint8_t dev_handle, const tAclLinkSpec& /* link_spec */, BT_HDR* p_data) {
  if (p_data->layer_specific != BTA_HH_RPTT_OUTPUT) {
    log::error("ERROR! Wrong report type! Write Command only valid for output report!");
    return;
  }
  bta_hh_snd_write_dev(dev_handle, HID_TRANS_DATA, (uint8_t)p_data->layer_specific, 0, 0, p_data);
}

/*******************************************************************************
 *
 * Function         BTA_HhGetDscpInfo
 *
 * Description      Get HID device report descriptor
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhGetDscpInfo(uint8_t dev_handle) {
  BT_HDR* p_buf = (BT_HDR*)osi_calloc(sizeof(BT_HDR));

  p_buf->event = BTA_HH_API_GET_DSCP_EVT;
  p_buf->layer_specific = (uint16_t)dev_handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_HhAddDev
 *
 * Description      Add a virtually cabled device into HID-Host device list
 *                  to manage and assign a device handle for future API call,
 *                  host applciation call this API at start-up to initialize its
 *                  virtually cabled devices.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhAddDev(const tAclLinkSpec& link_spec, tBTA_HH_ATTR_MASK attr_mask, uint8_t sub_class,
                  uint8_t app_id, tBTA_HH_DEV_DSCP_INFO dscp_info) {
  size_t len = sizeof(tBTA_HH_MAINT_DEV) + dscp_info.descriptor.dl_len;
  tBTA_HH_MAINT_DEV* p_buf = (tBTA_HH_MAINT_DEV*)osi_calloc(len);

  p_buf->hdr.event = BTA_HH_API_MAINT_DEV_EVT;
  p_buf->sub_event = BTA_HH_ADD_DEV_EVT;
  p_buf->hdr.layer_specific = BTA_HH_INVALID_HANDLE;

  p_buf->attr_mask = (uint16_t)attr_mask;
  p_buf->sub_class = sub_class;
  p_buf->app_id = app_id;
  p_buf->link_spec = link_spec;

  memcpy(&p_buf->dscp_info, &dscp_info, sizeof(tBTA_HH_DEV_DSCP_INFO));
  if (dscp_info.descriptor.dl_len != 0 && dscp_info.descriptor.dsc_list) {
    p_buf->dscp_info.descriptor.dl_len = dscp_info.descriptor.dl_len;
    p_buf->dscp_info.descriptor.dsc_list = (uint8_t*)(p_buf + 1);
    memcpy(p_buf->dscp_info.descriptor.dsc_list, dscp_info.descriptor.dsc_list,
           dscp_info.descriptor.dl_len);
  } else {
    p_buf->dscp_info.descriptor.dsc_list = NULL;
    p_buf->dscp_info.descriptor.dl_len = 0;
  }

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_HhRemoveDev
 *
 * Description      Remove a device from the HID host devices list.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhRemoveDev(uint8_t dev_handle) {
  tBTA_HH_MAINT_DEV* p_buf = (tBTA_HH_MAINT_DEV*)osi_calloc(sizeof(tBTA_HH_MAINT_DEV));

  p_buf->hdr.event = BTA_HH_API_MAINT_DEV_EVT;
  p_buf->sub_event = BTA_HH_RMV_DEV_EVT;
  p_buf->hdr.layer_specific = (uint16_t)dev_handle;

  bta_sys_sendmsg(p_buf);
}

/*******************************************************************************
 *
 * Function         BTA_HhDump
 *
 * Description      Dump BTA HH control block
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_HhDump(int fd) { bta_hh_dump(fd); }
