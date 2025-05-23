/******************************************************************************
 *
 *  Copyright 2002-2012 Broadcom Corporation
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
 *  this file contains the connection interface functions
 *
 ******************************************************************************/

#include <base/functional/callback.h>
#include <base/strings/stringprintf.h>
#include <bluetooth/log.h>
#include <frameworks/proto_logging/stats/enums/bluetooth/enums.pb.h>
#include <string.h>

#include <cstdint>

#include "bta/include/bta_sec_api.h"
#include "hci_error_code.h"
#include "hid_conn.h"
#include "hiddefs.h"
#include "hidh_api.h"
#include "hidh_int.h"
#include "internal_include/bt_target.h"
#include "l2cap_types.h"
#include "l2cdefs.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "stack/include/acl_api.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_psm_types.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/btm_log_history.h"
#include "stack/include/l2cap_interface.h"
#include "stack/include/stack_metrics_logging.h"
#include "types/bt_transport.h"
#include "types/raw_address.h"

using namespace bluetooth;

namespace {
constexpr char kBtmLogTag[] = "HIDH";
constexpr uint8_t kHID_HOST_MAX_DEVICES = HID_HOST_MAX_DEVICES;
}  // namespace

static uint8_t find_conn_by_cid(uint16_t cid);
static void hidh_conn_retry(uint8_t dhandle);

/******************************************************************************/
/*            L O C A L    F U N C T I O N     P R O T O T Y P E S            */
/******************************************************************************/
static void hidh_l2cif_connect_ind(const RawAddress& bd_addr, uint16_t l2cap_cid, uint16_t psm,
                                   uint8_t l2cap_id);
static void hidh_l2cif_connect_cfm(uint16_t l2cap_cid, tL2CAP_CONN result);
static void hidh_l2cif_config_ind(uint16_t l2cap_cid, tL2CAP_CFG_INFO* p_cfg);
static void hidh_l2cif_config_cfm(uint16_t l2cap_cid, uint16_t result, tL2CAP_CFG_INFO* p_cfg);
static void hidh_l2cif_disconnect_ind(uint16_t l2cap_cid, bool ack_needed);
static void hidh_l2cif_data_ind(uint16_t l2cap_cid, BT_HDR* p_msg);
static void hidh_l2cif_disconnect(uint16_t l2cap_cid);
static void hidh_l2cif_cong_ind(uint16_t l2cap_cid, bool congested);
static void hidh_on_l2cap_error(uint16_t l2cap_cid, uint16_t result);

static const tL2CAP_APPL_INFO hst_reg_info = {
        .pL2CA_ConnectInd_Cb = hidh_l2cif_connect_ind,
        .pL2CA_ConnectCfm_Cb = hidh_l2cif_connect_cfm,
        .pL2CA_ConfigInd_Cb = hidh_l2cif_config_ind,
        .pL2CA_ConfigCfm_Cb = hidh_l2cif_config_cfm,
        .pL2CA_DisconnectInd_Cb = hidh_l2cif_disconnect_ind,
        .pL2CA_DataInd_Cb = hidh_l2cif_data_ind,
        .pL2CA_CongestionStatus_Cb = hidh_l2cif_cong_ind,
        .pL2CA_TxComplete_Cb = nullptr,
        .pL2CA_Error_Cb = hidh_on_l2cap_error,
        .pL2CA_CreditBasedConnectInd_Cb = nullptr,
        .pL2CA_CreditBasedConnectCfm_Cb = nullptr,
        .pL2CA_CreditBasedReconfigCompleted_Cb = nullptr,
        .pL2CA_CreditBasedCollisionInd_Cb = nullptr,
};
static void hidh_try_repage(uint8_t dhandle);

/*******************************************************************************
 *
 * Function         hidh_l2cif_reg
 *
 * Description      This function initializes the SDP unit.
 *
 * Returns          void
 *
 ******************************************************************************/
tHID_STATUS hidh_conn_reg(void) {
  int xx;

  /* Initialize the L2CAP configuration. We only care about MTU and flush */
  memset(&hh_cb.l2cap_cfg, 0, sizeof(tL2CAP_CFG_INFO));

  hh_cb.l2cap_cfg.mtu_present = true;
  hh_cb.l2cap_cfg.mtu = HID_HOST_MTU;

  /* Now, register with L2CAP */
  if (!stack::l2cap::get_interface().L2CA_RegisterWithSecurity(
              HID_PSM_CONTROL, hst_reg_info, false /* enable_snoop */, nullptr, HID_HOST_MTU, 0,
              BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT)) {
    log::error("HID-Host Control Registration failed");
    log_counter_metrics(
            android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_L2CAP_FAILED_AT_REGISTER_CONTROL,
            1);
    return HID_ERR_L2CAP_FAILED;
  }
  if (!stack::l2cap::get_interface().L2CA_RegisterWithSecurity(
              HID_PSM_INTERRUPT, hst_reg_info, false /* enable_snoop */, nullptr, HID_HOST_MTU, 0,
              BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT)) {
    stack::l2cap::get_interface().L2CA_Deregister(HID_PSM_CONTROL);
    log::error("HID-Host Interrupt Registration failed");
    log_counter_metrics(
            android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_L2CAP_FAILED_AT_REGISTER_INTERRUPT,
            1);
    return HID_ERR_L2CAP_FAILED;
  }

  for (xx = 0; xx < kHID_HOST_MAX_DEVICES; xx++) {
    hh_cb.devices[xx].in_use = false;
    hh_cb.devices[xx].conn.conn_state = HID_CONN_STATE_UNUSED;
  }

  return HID_SUCCESS;
}

/*******************************************************************************
 *
 * Function         hidh_conn_disconnect
 *
 * Description      This function disconnects a connection.
 *
 * Returns          true if disconnect started, false if already disconnected
 *
 ******************************************************************************/
tHID_STATUS hidh_conn_disconnect(uint8_t dhandle) {
  tHID_CONN* p_hcon = &hh_cb.devices[dhandle].conn;

  if ((p_hcon->ctrl_cid != 0) || (p_hcon->intr_cid != 0)) {
    p_hcon->conn_state = HID_CONN_STATE_DISCONNECTING;

    /* Set l2cap idle timeout to 0 (so ACL link is disconnected
     * immediately after last channel is closed) */
    if (!stack::l2cap::get_interface().L2CA_SetIdleTimeoutByBdAddr(hh_cb.devices[dhandle].addr, 0,
                                                                   BT_TRANSPORT_BR_EDR)) {
      log::warn("Unable to set L2CAP idle timeout peer:{}", hh_cb.devices[dhandle].addr);
    }
    /* Disconnect both interrupt and control channels */
    if (p_hcon->intr_cid) {
      hidh_l2cif_disconnect(p_hcon->intr_cid);
    } else if (p_hcon->ctrl_cid) {
      hidh_l2cif_disconnect(p_hcon->ctrl_cid);
    }

    BTM_LogHistory(kBtmLogTag, hh_cb.devices[dhandle].addr, "Disconnecting", "local initiated");
  } else {
    p_hcon->conn_state = HID_CONN_STATE_UNUSED;
  }
  return HID_SUCCESS;
}

/*******************************************************************************
 *
 * Function         hidh_l2cif_connect_ind
 *
 * Description      This function handles an inbound connection indication
 *                  from L2CAP. This is the case where we are acting as a
 *                  server.
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_l2cif_connect_ind(const RawAddress& bd_addr, uint16_t l2cap_cid, uint16_t psm,
                                   uint8_t /* l2cap_id */) {
  bool bAccept = true;
  uint8_t i = kHID_HOST_MAX_DEVICES;

  log::verbose("HID-Host Rcvd L2CAP conn ind, PSM: 0x{:04x}  CID 0x{:x}", psm, l2cap_cid);

  /* always add incoming connection device into HID database by default */
  if (HID_HostAddDev(bd_addr, HID_SEC_REQUIRED, &i) != HID_SUCCESS) {
    if (!stack::l2cap::get_interface().L2CA_DisconnectReq(l2cap_cid)) {
      log::warn("Unable to send L2CAP disconnect request peer:{} cid:{}", bd_addr, l2cap_cid);
    }
    return;
  }

  tHID_CONN* p_hcon = &hh_cb.devices[i].conn;

  BTM_LogHistory(
          kBtmLogTag, hh_cb.devices[i].addr, "Connect request",
          base::StringPrintf("%s state:%s", (psm == HID_PSM_CONTROL) ? "control" : "interrupt",
                             hid_conn::state_text(p_hcon->conn_state).c_str()));

  /* Check we are in the correct state for this */
  if (psm == HID_PSM_INTERRUPT) {
    if (p_hcon->ctrl_cid == 0) {
      log::warn("HID-Host Rcvd INTR L2CAP conn ind, but no CTL channel");
      bAccept = false;
    }
    if (p_hcon->conn_state != HID_CONN_STATE_CONNECTING_INTR) {
      log::warn("HID-Host Rcvd INTR L2CAP conn ind, wrong state: {}", p_hcon->conn_state);
      bAccept = false;
    }
  } else /* CTRL channel */
  {
#if (HID_HOST_ACPT_NEW_CONN == TRUE)
    p_hcon->ctrl_cid = p_hcon->intr_cid = 0;
    p_hcon->conn_state = HID_CONN_STATE_UNUSED;
#else
    if (p_hcon->conn_state != HID_CONN_STATE_UNUSED) {
      log::warn("HID-Host - Rcvd CTL L2CAP conn ind, wrong state: {}", p_hcon->conn_state);
      bAccept = false;
    }
#endif
  }

  if (!bAccept) {
    if (!stack::l2cap::get_interface().L2CA_DisconnectReq(l2cap_cid)) {
      log::warn("Unable to send L2CAP disconnect request peer:{} cid:{}", bd_addr, l2cap_cid);
    }
    return;
  }

  if (psm == HID_PSM_CONTROL) {
    p_hcon->conn_flags = 0;
    p_hcon->ctrl_cid = l2cap_cid;
    p_hcon->disc_reason = HID_SUCCESS; /* Authentication passed. Reset
                                              disc_reason (from
                                              HID_ERR_AUTH_FAILED) */
    p_hcon->conn_state = HID_CONN_STATE_CONNECTING_INTR;
    BTM_LogHistory(kBtmLogTag, hh_cb.devices[i].addr, "Connecting",
                   "waiting for interrupt channel");
    return;
  }

  /* Transition to the next appropriate state, configuration */
  p_hcon->conn_state = HID_CONN_STATE_CONFIG;
  p_hcon->intr_cid = l2cap_cid;

  log::verbose(
          "HID-Host Rcvd L2CAP conn ind, sent config req, PSM: 0x{:04x}  CID "
          "0x{:x}",
          psm, l2cap_cid);
}

static void hidh_process_repage_timer_timeout(void* data) {
  uint8_t dhandle = PTR_TO_UINT(data);
  hidh_try_repage(dhandle);
}

/*******************************************************************************
 *
 * Function         hidh_try_repage
 *
 * Description      This function processes timeout (to page device).
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_try_repage(uint8_t dhandle) {
  tHID_HOST_DEV_CTB* device;

  hidh_conn_initiate(dhandle);

  device = &hh_cb.devices[dhandle];
  device->conn_tries++;

  hh_cb.callback(dhandle, device->addr, HID_HDEV_EVT_RETRYING, device->conn_tries, NULL);
}

static void hidh_on_l2cap_error(uint16_t l2cap_cid, uint16_t result) {
  auto dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle == kHID_HOST_MAX_DEVICES) {
    log::warn("Received error for unknown device cid:0x{:04x} reason:{}", l2cap_cid,
              hci_reason_code_text(to_hci_reason_code(result)));
    return;
  }

  hidh_conn_disconnect(dhandle);

  if (result != static_cast<uint16_t>(tL2CAP_CFG_RESULT::L2CAP_CFG_FAILED_NO_REASON)) {
#if (HID_HOST_MAX_CONN_RETRY > 0)
    if ((hh_cb.devices[dhandle].conn_tries <= HID_HOST_MAX_CONN_RETRY) &&
        (result == HCI_ERR_CONNECTION_TOUT || result == HCI_ERR_UNSPECIFIED ||
         result == HCI_ERR_PAGE_TIMEOUT)) {
      hidh_conn_retry(dhandle);
    } else
#endif
    {
      uint32_t reason = HID_L2CAP_CONN_FAIL | (uint32_t)result;
      hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_CLOSE, reason, NULL);
    }
  } else {
    uint32_t reason = HID_L2CAP_CFG_FAIL | (uint32_t)result;
    hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_CLOSE, reason, NULL);
  }
}

/*******************************************************************************
 *
 * Function         hidh_l2cif_connect_cfm
 *
 * Description      This function handles the connect confirm events
 *                  from L2CAP. This is the case when we are acting as a
 *                  client and have sent a connect request.
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_l2cif_connect_cfm(uint16_t l2cap_cid, tL2CAP_CONN result) {
  uint8_t dhandle;
  tHID_CONN* p_hcon = NULL;

  /* Find CCB based on CID, and verify we are in a state to accept this message
   */
  dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle < kHID_HOST_MAX_DEVICES) {
    p_hcon = &hh_cb.devices[dhandle].conn;
  }

  if ((p_hcon == NULL) || (!(p_hcon->conn_flags & HID_CONN_FLAGS_IS_ORIG)) ||
      ((l2cap_cid == p_hcon->ctrl_cid) && (p_hcon->conn_state != HID_CONN_STATE_CONNECTING_CTRL)) ||
      ((l2cap_cid == p_hcon->intr_cid) && (p_hcon->conn_state != HID_CONN_STATE_CONNECTING_INTR) &&
       (p_hcon->conn_state != HID_CONN_STATE_DISCONNECTING))) {
    log::warn("HID-Host Rcvd unexpected conn cnf, CID 0x{:x}", l2cap_cid);
    return;
  }

  if (result != tL2CAP_CONN::L2CAP_CONN_OK) {
    // TODO: We need to provide the real HCI status if we want to retry.
    log::error("invoked with non OK status");
    return;
  }
  /* receive Control Channel connect confirmation */
  if (l2cap_cid == p_hcon->ctrl_cid) {
    /* check security requirement */
    p_hcon->disc_reason = HID_SUCCESS; /* Authentication passed. Reset
                                              disc_reason (from
                                              HID_ERR_AUTH_FAILED) */

    /* Transition to the next appropriate state, configuration */
    p_hcon->conn_state = HID_CONN_STATE_CONFIG;
  } else {
    p_hcon->conn_state = HID_CONN_STATE_CONFIG;
  }
  BTM_LogHistory(
          kBtmLogTag, hh_cb.devices[dhandle].addr, "Configuring",
          base::StringPrintf("control:0x%04x interrupt:0x%04x state:%s", p_hcon->ctrl_cid,
                             p_hcon->intr_cid, hid_conn::state_text(p_hcon->conn_state).c_str()));
  return;
}

/*******************************************************************************
 *
 * Function         hidh_l2cif_config_ind
 *
 * Description      This function processes the L2CAP configuration indication
 *                  event.
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_l2cif_config_ind(uint16_t l2cap_cid, tL2CAP_CFG_INFO* p_cfg) {
  uint8_t dhandle;
  tHID_CONN* p_hcon = NULL;

  /* Find CCB based on CID */
  dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle < kHID_HOST_MAX_DEVICES) {
    p_hcon = &hh_cb.devices[dhandle].conn;
  }

  if (p_hcon == NULL) {
    log::warn("HID-Host Rcvd L2CAP cfg ind, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  log::verbose("HID-Host Rcvd cfg ind, sent cfg cfm, CID: 0x{:x}", l2cap_cid);

  /* Remember the remote MTU size */
  if ((!p_cfg->mtu_present) || (p_cfg->mtu > HID_HOST_MTU)) {
    p_hcon->rem_mtu_size = HID_HOST_MTU;
  } else {
    p_hcon->rem_mtu_size = p_cfg->mtu;
  }
}

/*******************************************************************************
 *
 * Function         hidh_l2cif_config_cfm
 *
 * Description      This function processes the L2CAP configuration confirmation
 *                  event.
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_l2cif_config_cfm(uint16_t l2cap_cid, uint16_t /* initiator */,
                                  tL2CAP_CFG_INFO* p_cfg) {
  hidh_l2cif_config_ind(l2cap_cid, p_cfg);

  uint8_t dhandle;
  tHID_CONN* p_hcon = NULL;
  uint32_t reason;

  log::verbose("HID-Host Rcvd cfg cfm, CID: 0x{:x}", l2cap_cid);

  /* Find CCB based on CID */
  dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle < kHID_HOST_MAX_DEVICES) {
    p_hcon = &hh_cb.devices[dhandle].conn;
  }

  if (p_hcon == NULL) {
    log::warn("HID-Host Rcvd L2CAP cfg ind, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  if (l2cap_cid == p_hcon->ctrl_cid) {
    if (p_hcon->conn_flags & HID_CONN_FLAGS_IS_ORIG) {
      /* Connect interrupt channel */
      p_hcon->disc_reason = HID_L2CAP_CONN_FAIL; /* Reset initial reason for
                                                    CLOSE_EVT: Connection
                                                    Attempt was made but failed
                                                    */
      p_hcon->intr_cid = stack::l2cap::get_interface().L2CA_ConnectReqWithSecurity(
              HID_PSM_INTERRUPT, hh_cb.devices[dhandle].addr,
              BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT);
      if (p_hcon->intr_cid == 0) {
        log::warn("HID-Host INTR Originate failed");
        reason = HID_L2CAP_REQ_FAIL;
        p_hcon->conn_state = HID_CONN_STATE_UNUSED;
        BTM_LogHistory(kBtmLogTag, hh_cb.devices[dhandle].addr, "Failed");
        hidh_conn_disconnect(dhandle);
        hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_CLOSE, reason, NULL);
        return;
      } else {
        /* Transition to the next appropriate state, waiting for connection
         * confirm on interrupt channel. */
        p_hcon->conn_state = HID_CONN_STATE_CONNECTING_INTR;
        BTM_LogHistory(kBtmLogTag, hh_cb.devices[dhandle].addr, "Connecting", "interrupt channel");
      }
    }
  }

  /* If all configuration is complete, change state and tell management we are
   * up */
  if (p_hcon->conn_state == HID_CONN_STATE_CONFIG) {
    p_hcon->conn_state = HID_CONN_STATE_CONNECTED;
    /* Reset disconnect reason to success, as connection successful */
    p_hcon->disc_reason = HID_SUCCESS;

    hh_cb.devices[dhandle].state = HID_DEV_CONNECTED;
    hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_OPEN, 0, NULL);
    BTM_LogHistory(
            kBtmLogTag, hh_cb.devices[dhandle].addr, "Connected",
            base::StringPrintf("control:0x%04x interrupt:0x%04x state:%s", p_hcon->ctrl_cid,
                               p_hcon->intr_cid, hid_conn::state_text(p_hcon->conn_state).c_str()));
  }
}

/*******************************************************************************
 *
 * Function         hidh_l2cif_disconnect_ind
 *
 * Description      This function handles a disconnect event from L2CAP. If
 *                  requested to, we ack the disconnect before dropping the CCB
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_l2cif_disconnect_ind(uint16_t l2cap_cid, bool ack_needed) {
  uint8_t dhandle;
  tHID_CONN* p_hcon = NULL;
  tHCI_REASON disc_res = HCI_SUCCESS;
  uint16_t hid_close_evt_reason;

  /* Find CCB based on CID */
  dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle < kHID_HOST_MAX_DEVICES) {
    p_hcon = &hh_cb.devices[dhandle].conn;
  }

  if (p_hcon == NULL) {
    log::warn("HID-Host Rcvd L2CAP disc, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  log::verbose("HID-Host Rcvd L2CAP disc, CID: 0x{:x}", l2cap_cid);

  p_hcon->conn_state = HID_CONN_STATE_DISCONNECTING;
  BTM_LogHistory(kBtmLogTag, hh_cb.devices[dhandle].addr, "Disconnecting",
                 base::StringPrintf("%s channel",
                                    (l2cap_cid == p_hcon->ctrl_cid) ? "control" : "interrupt"));

  if (l2cap_cid == p_hcon->ctrl_cid) {
    p_hcon->ctrl_cid = 0;
  } else {
    p_hcon->intr_cid = 0;
  }

  if ((p_hcon->ctrl_cid == 0) && (p_hcon->intr_cid == 0)) {
    hh_cb.devices[dhandle].state = HID_DEV_NO_CONN;
    p_hcon->conn_state = HID_CONN_STATE_UNUSED;

    if (!ack_needed) {
      disc_res = btm_get_acl_disc_reason_code();
    }

#if (HID_HOST_MAX_CONN_RETRY > 0)
    if ((disc_res == HCI_ERR_CONNECTION_TOUT || disc_res == HCI_ERR_UNSPECIFIED) &&
        (!(hh_cb.devices[dhandle].attr_mask & HID_RECONN_INIT)) &&
        (hh_cb.devices[dhandle].attr_mask & HID_NORMALLY_CONNECTABLE)) {
      hh_cb.devices[dhandle].conn_tries = 0;
      uint64_t interval_ms = HID_HOST_REPAGE_WIN * 1000;
      alarm_set_on_mloop(hh_cb.devices[dhandle].conn.process_repage_timer, interval_ms,
                         hidh_process_repage_timer_timeout, UINT_TO_PTR(dhandle));
      hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_CLOSE, disc_res, NULL);
    } else
#endif
    {
      /* Set reason code for HID_HDEV_EVT_CLOSE */
      hid_close_evt_reason = p_hcon->disc_reason;

      /* If we got baseband sent HCI_DISCONNECT_COMPLETE_EVT due to security
       * failure, then set reason to HID_ERR_AUTH_FAILED */
      if ((disc_res == HCI_ERR_AUTH_FAILURE) || (disc_res == HCI_ERR_KEY_MISSING) ||
          (disc_res == HCI_ERR_HOST_REJECT_SECURITY) || (disc_res == HCI_ERR_PAIRING_NOT_ALLOWED) ||
          (disc_res == HCI_ERR_UNIT_KEY_USED) ||
          (disc_res == HCI_ERR_PAIRING_WITH_UNIT_KEY_NOT_SUPPORTED) ||
          (disc_res == HCI_ERR_ENCRY_MODE_NOT_ACCEPTABLE) ||
          (disc_res == HCI_ERR_REPEATED_ATTEMPTS)) {
        log_counter_metrics(android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_AUTH_FAILED, 1);
        hid_close_evt_reason = HID_ERR_AUTH_FAILED;
      }

      hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_CLOSE, hid_close_evt_reason,
                     NULL);
    }
  }
}

static void hidh_l2cif_disconnect(uint16_t l2cap_cid) {
  if (!stack::l2cap::get_interface().L2CA_DisconnectReq(l2cap_cid)) {
    log::warn("Unable to send L2CAP disconnect request cid:{}", l2cap_cid);
  }

  /* Find CCB based on CID */
  const uint8_t dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle == kHID_HOST_MAX_DEVICES) {
    log::warn("HID-Host Rcvd L2CAP disc cfm, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  tHID_CONN* p_hcon = &hh_cb.devices[dhandle].conn;
  if (l2cap_cid == p_hcon->ctrl_cid) {
    p_hcon->ctrl_cid = 0;
  } else {
    p_hcon->intr_cid = 0;
    if (p_hcon->ctrl_cid) {
      log::verbose("HID-Host Initiating L2CAP Ctrl disconnection");
      if (!stack::l2cap::get_interface().L2CA_DisconnectReq(p_hcon->ctrl_cid)) {
        log::warn("Unable to send L2CAP disconnect request cid:{}", p_hcon->ctrl_cid);
      }
      p_hcon->ctrl_cid = 0;
    }
  }

  if ((p_hcon->ctrl_cid == 0) && (p_hcon->intr_cid == 0)) {
    hh_cb.devices[dhandle].state = HID_DEV_NO_CONN;
    p_hcon->conn_state = HID_CONN_STATE_UNUSED;
    BTM_LogHistory(kBtmLogTag, hh_cb.devices[dhandle].addr, "Disconnected");
    hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_CLOSE, p_hcon->disc_reason,
                   NULL);
  }
}

/*******************************************************************************
 *
 * Function         hidh_l2cif_cong_ind
 *
 * Description      This function handles a congestion status event from L2CAP.
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_l2cif_cong_ind(uint16_t l2cap_cid, bool congested) {
  uint8_t dhandle;
  tHID_CONN* p_hcon = NULL;

  /* Find CCB based on CID */
  dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle < kHID_HOST_MAX_DEVICES) {
    p_hcon = &hh_cb.devices[dhandle].conn;
  }

  if (p_hcon == NULL) {
    log::warn("HID-Host Rcvd L2CAP congestion status, unknown CID: 0x{:x}", l2cap_cid);
    return;
  }

  log::verbose("HID-Host Rcvd L2CAP congestion status, CID: 0x{:x}  Cong: {}", l2cap_cid,
               congested);

  if (congested) {
    p_hcon->conn_flags |= HID_CONN_FLAGS_CONGESTED;
  } else {
    p_hcon->conn_flags &= ~HID_CONN_FLAGS_CONGESTED;
  }
}

/*******************************************************************************
 *
 * Function         hidh_l2cif_data_ind
 *
 * Description      This function is called when data is received from L2CAP.
 *                  if we are the originator of the connection, we are the SDP
 *                  client, and the received message is queued up for the
 *                  client.
 *
 *                  If we are the destination of the connection, we are the SDP
 *                  server, so the message is passed to the server processing
 *                  function.
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_l2cif_data_ind(uint16_t l2cap_cid, BT_HDR* p_msg) {
  uint8_t* p_data = (uint8_t*)(p_msg + 1) + p_msg->offset;
  uint8_t ttype, param, rep_type, evt;
  uint8_t dhandle;
  tHID_CONN* p_hcon = NULL;

  log::verbose("HID-Host hidh_l2cif_data_ind [l2cap_cid=0x{:04x}]", l2cap_cid);

  /* Find CCB based on CID */
  dhandle = find_conn_by_cid(l2cap_cid);
  if (dhandle < kHID_HOST_MAX_DEVICES) {
    p_hcon = &hh_cb.devices[dhandle].conn;
  }

  if (p_hcon == NULL) {
    log::warn("HID-Host Rcvd L2CAP data, unknown CID: 0x{:x}", l2cap_cid);
    osi_free(p_msg);
    return;
  }

  if (p_msg->len < 1) {
    log::warn("Rcvd L2CAP data, invalid length {}, should be >= 1", p_msg->len);
    osi_free(p_msg);
    return;
  }

  ttype = HID_GET_TRANS_FROM_HDR(*p_data);
  param = HID_GET_PARAM_FROM_HDR(*p_data);
  rep_type = param & HID_PAR_REP_TYPE_MASK;
  p_data++;

  /* Get rid of the data type */
  p_msg->len--;
  p_msg->offset++;

  switch (ttype) {
    case HID_TRANS_HANDSHAKE:
      hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_HANDSHAKE, param, NULL);
      osi_free(p_msg);
      break;

    case HID_TRANS_CONTROL:
      switch (param) {
        case HID_PAR_CONTROL_VIRTUAL_CABLE_UNPLUG:
          hidh_conn_disconnect(dhandle);
          /* Device is unplugging from us. Tell USB */
          hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_VC_UNPLUG, 0, NULL);
          break;

        default:
          break;
      }
      osi_free(p_msg);
      break;

    case HID_TRANS_DATA:
      evt = (hh_cb.devices[dhandle].conn.intr_cid == l2cap_cid) ? HID_HDEV_EVT_INTR_DATA
                                                                : HID_HDEV_EVT_CTRL_DATA;
      hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, evt, rep_type, p_msg);
      break;

    case HID_TRANS_DATAC:
      evt = (hh_cb.devices[dhandle].conn.intr_cid == l2cap_cid) ? HID_HDEV_EVT_INTR_DATC
                                                                : HID_HDEV_EVT_CTRL_DATC;
      hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, evt, rep_type, p_msg);
      break;

    default:
      osi_free(p_msg);
      break;
  }
}

/*******************************************************************************
 *
 * Function         hidh_conn_snd_data
 *
 * Description      This function is sends out data.
 *
 * Returns          tHID_STATUS
 *
 ******************************************************************************/
tHID_STATUS hidh_conn_snd_data(uint8_t dhandle, uint8_t trans_type, uint8_t param, uint16_t data,
                               uint8_t report_id, BT_HDR* buf) {
  tHID_CONN* p_hcon = &hh_cb.devices[dhandle].conn;
  BT_HDR* p_buf;
  uint8_t* p_out;
  uint16_t bytes_copied;
  bool seg_req = false;
  uint16_t data_size;
  uint16_t cid;
  uint16_t buf_size;
  uint8_t use_data = 0;
  bool blank_datc = false;

  if (!get_btm_client_interface().peer.BTM_IsAclConnectionUp(hh_cb.devices[dhandle].addr,
                                                             BT_TRANSPORT_BR_EDR)) {
    osi_free(buf);
    log_counter_metrics(
            android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_NO_CONNECTION_AT_SEND_DATA, 1);
    return HID_ERR_NO_CONNECTION;
  }

  if (p_hcon->conn_flags & HID_CONN_FLAGS_CONGESTED) {
    osi_free(buf);
    log_counter_metrics(
            android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_CONGESTED_AT_FLAG_CHECK, 1);
    return HID_ERR_CONGESTED;
  }

  switch (trans_type) {
    case HID_TRANS_CONTROL:
    case HID_TRANS_GET_REPORT:
    case HID_TRANS_SET_REPORT:
    case HID_TRANS_GET_PROTOCOL:
    case HID_TRANS_SET_PROTOCOL:
    case HID_TRANS_GET_IDLE:
    case HID_TRANS_SET_IDLE:
      cid = p_hcon->ctrl_cid;
      buf_size = HID_CONTROL_BUF_SIZE;
      break;
    case HID_TRANS_DATA:
      cid = p_hcon->intr_cid;
      buf_size = HID_INTERRUPT_BUF_SIZE;
      break;
    default:
      log_counter_metrics(
              android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_INVALID_PARAM_AT_SEND_DATA, 1);
      return HID_ERR_INVALID_PARAM;
  }

  if (trans_type == HID_TRANS_SET_IDLE) {
    use_data = 1;
  } else if ((trans_type == HID_TRANS_GET_REPORT) && (param & 0x08)) {
    use_data = 2;
  }

  do {
    if (buf == NULL || blank_datc) {
      p_buf = (BT_HDR*)osi_malloc(buf_size);

      p_buf->offset = L2CAP_MIN_OFFSET;
      seg_req = false;
      data_size = 0;
      bytes_copied = 0;
      blank_datc = false;
    } else if (buf->len > (p_hcon->rem_mtu_size - 1)) {
      p_buf = (BT_HDR*)osi_malloc(buf_size);

      p_buf->offset = L2CAP_MIN_OFFSET;
      seg_req = true;
      data_size = buf->len;
      bytes_copied = p_hcon->rem_mtu_size - 1;
    } else {
      p_buf = buf;
      p_buf->offset -= 1;
      seg_req = false;
      data_size = buf->len;
      bytes_copied = buf->len;
    }

    p_out = (uint8_t*)(p_buf + 1) + p_buf->offset;
    *p_out++ = HID_BUILD_HDR(trans_type, param);

    /* If report ID required for this device */
    if ((trans_type == HID_TRANS_GET_REPORT) && (report_id != 0)) {
      *p_out = report_id;
      data_size = bytes_copied = 1;
    }

    if (seg_req) {
      memcpy(p_out, (((uint8_t*)(buf + 1)) + buf->offset), bytes_copied);
      buf->offset += bytes_copied;
      buf->len -= bytes_copied;
    } else if (use_data == 1) {
      *(p_out + bytes_copied) = data & 0xff;
    } else if (use_data == 2) {
      *(p_out + bytes_copied) = data & 0xff;
      *(p_out + bytes_copied + 1) = (data >> 8) & 0xff;
    }

    p_buf->len = bytes_copied + 1 + use_data;
    data_size -= bytes_copied;

    /* Send the buffer through L2CAP */
    if ((p_hcon->conn_flags & HID_CONN_FLAGS_CONGESTED) ||
        (stack::l2cap::get_interface().L2CA_DataWrite(cid, p_buf) == tL2CAP_DW_RESULT::FAILED)) {
      log_counter_metrics(
              android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_CONGESTED_AT_SEND_DATA, 1);
      return HID_ERR_CONGESTED;
    }

    if (data_size) {
      trans_type = HID_TRANS_DATAC;
    } else if (bytes_copied == (p_hcon->rem_mtu_size - 1)) {
      trans_type = HID_TRANS_DATAC;
      blank_datc = true;
    }
  } while ((data_size != 0) || blank_datc);

  return HID_SUCCESS;
}
/*******************************************************************************
 *
 * Function         hidh_conn_initiate
 *
 * Description      This function is called by the management to create a
 *                  connection.
 *
 * Returns          void
 *
 ******************************************************************************/
tHID_STATUS hidh_conn_initiate(uint8_t dhandle) {
  tHID_HOST_DEV_CTB* p_dev = &hh_cb.devices[dhandle];

  if (p_dev->conn.conn_state != HID_CONN_STATE_UNUSED) {
    log_counter_metrics(android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_CONN_IN_PROCESS, 1);
    return HID_ERR_CONN_IN_PROCESS;
  }

  p_dev->conn.ctrl_cid = 0;
  p_dev->conn.intr_cid = 0;
  p_dev->conn.disc_reason = HID_L2CAP_CONN_FAIL; /* Reset initial reason for CLOSE_EVT: Connection
                                                    Attempt was made but failed */

  /* We are the originator of this connection */
  p_dev->conn.conn_flags = HID_CONN_FLAGS_IS_ORIG;

  /* Check if L2CAP started the connection process */
  p_dev->conn.ctrl_cid = stack::l2cap::get_interface().L2CA_ConnectReqWithSecurity(
          HID_PSM_CONTROL, p_dev->addr, BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT);
  if (p_dev->conn.ctrl_cid == 0) {
    log::warn("HID-Host Originate failed");
    hh_cb.callback(dhandle, hh_cb.devices[dhandle].addr, HID_HDEV_EVT_CLOSE, HID_ERR_L2CAP_FAILED,
                   NULL);
    log_counter_metrics(
            android::bluetooth::CodePathCounterKeyEnum::HIDH_ERR_L2CAP_FAILED_AT_INITIATE, 1);
  } else {
    /* Transition to the next appropriate state, waiting for connection confirm
     * on control channel. */
    p_dev->conn.conn_state = HID_CONN_STATE_CONNECTING_CTRL;
    BTM_LogHistory(kBtmLogTag, hh_cb.devices[dhandle].addr, "Connecting", "control channel");
  }

  return HID_SUCCESS;
}

/*******************************************************************************
 *
 * Function         find_conn_by_cid
 *
 * Description      This function finds a connection control block based on CID.
 *
 * Returns          index of control block, or kHID_HOST_MAX_DEVICES if not
 *                  found.
 *
 ******************************************************************************/
static uint8_t find_conn_by_cid(uint16_t cid) {
  uint8_t xx;

  for (xx = 0; xx < kHID_HOST_MAX_DEVICES; xx++) {
    if ((hh_cb.devices[xx].in_use) &&
        (hh_cb.devices[xx].conn.conn_state != HID_CONN_STATE_UNUSED) &&
        ((hh_cb.devices[xx].conn.ctrl_cid == cid) || (hh_cb.devices[xx].conn.intr_cid == cid))) {
      break;
    }
  }

  return xx;
}

void hidh_conn_dereg(void) {
  stack::l2cap::get_interface().L2CA_Deregister(HID_PSM_CONTROL);
  stack::l2cap::get_interface().L2CA_Deregister(HID_PSM_INTERRUPT);
}

/*******************************************************************************
 *
 * Function         hidh_conn_retry
 *
 * Description      This function is called to retry a failed connection.
 *
 * Returns          void
 *
 ******************************************************************************/
static void hidh_conn_retry(uint8_t dhandle) {
  tHID_HOST_DEV_CTB* p_dev = &hh_cb.devices[dhandle];

  p_dev->conn.conn_state = HID_CONN_STATE_UNUSED;
#if (HID_HOST_REPAGE_WIN > 0)
  uint64_t interval_ms = HID_HOST_REPAGE_WIN * 1000;
  alarm_set_on_mloop(p_dev->conn.process_repage_timer, interval_ms,
                     hidh_process_repage_timer_timeout, UINT_TO_PTR(dhandle));
#else
  hidh_try_repage(dhandle);
#endif
}
