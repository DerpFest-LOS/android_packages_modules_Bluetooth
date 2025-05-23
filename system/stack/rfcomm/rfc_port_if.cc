/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
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

/*****************************************************************************
 *
 * This file contains functions callable by an application
 * running on top of RFCOMM
 *
 *****************************************************************************/

#define LOG_TAG "rfcomm"

#include <bluetooth/log.h>

#include <cstdint>
#include <unordered_map>

#include "stack/include/bt_hdr.h"
#include "stack/rfcomm/port_int.h"
#include "stack/rfcomm/rfc_int.h"
#include "stack/rfcomm/rfc_state.h"

using namespace bluetooth;

tRFC_CB rfc_cb;
std::unordered_map<uint16_t /* sci */, tRFC_MCB*> rfc_lcid_mcb;

/*******************************************************************************
 *
 * Function         RFCOMM_StartReq
 *
 * Description      This function handles Start Request from the upper layer.
 *                  If RFCOMM multiplexer channel can not be allocated
 *                  send start not accepted confirmation.  Otherwise dispatch
 *                  start event to the state machine.
 *
 ******************************************************************************/
void RFCOMM_StartReq(tRFC_MCB* p_mcb) { rfc_mx_sm_execute(p_mcb, RFC_MX_EVENT_START_REQ, nullptr); }

/*******************************************************************************
 *
 * Function         RFCOMM_StartRsp
 *
 * Description      This function handles Start Response from the upper layer.
 *                  Save upper layer handle and result of the Start Indication
 *                  in the control block and dispatch event to the FSM.
 *
 ******************************************************************************/
void RFCOMM_StartRsp(tRFC_MCB* p_mcb, uint16_t result) {
  rfc_mx_sm_execute(p_mcb, RFC_MX_EVENT_START_RSP, &result);
}

/*******************************************************************************
 *
 * Function         RFCOMM_DlcEstablishReq
 *
 * Description      This function is called by the user app to establish
 *                  connection with the specific dlci on a specific bd device.
 *                  It will allocate RFCOMM connection control block if not
 *                  allocated before and dispatch open event to the state
 *                  machine.
 *
 ******************************************************************************/
void RFCOMM_DlcEstablishReq(tRFC_MCB* p_mcb, uint8_t dlci, uint16_t /* mtu */) {
  if (p_mcb->state != RFC_MX_STATE_CONNECTED) {
    PORT_DlcEstablishCnf(p_mcb, dlci, 0, RFCOMM_ERROR);
    return;
  }

  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if (p_port == nullptr) {
    log::warn("Unable to find DLCI port dlci:{}", dlci);
    return;
  }

  rfc_port_sm_execute(p_port, RFC_PORT_EVENT_OPEN, nullptr);
}

/*******************************************************************************
 *
 * Function         RFCOMM_DlcEstablishRsp
 *
 * Description      This function is called by the port emulation entity
 *                  acks Establish Indication.
 *
 ******************************************************************************/
void RFCOMM_DlcEstablishRsp(tRFC_MCB* p_mcb, uint8_t dlci, uint16_t /* mtu */, uint16_t result) {
  if ((p_mcb->state != RFC_MX_STATE_CONNECTED) && (result == RFCOMM_SUCCESS)) {
    PORT_DlcReleaseInd(p_mcb, dlci);
    return;
  }

  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if (p_port == nullptr) {
    log::warn("Unable to find DLCI port dlci:{}", dlci);
    return;
  }
  rfc_port_sm_execute(p_port, RFC_PORT_EVENT_ESTABLISH_RSP, &result);
}

/*******************************************************************************
 *
 * Function         RFCOMM_ParameterNegotiationRequest
 *
 * Description      This function is called by the user app to start
 *                  DLC parameter negotiation.  Port emulation can send this
 *                  request before actually establishing the DLC.  In this
 *                  case the function will allocate RFCOMM connection control
 *                  block.
 *
 ******************************************************************************/
void RFCOMM_ParameterNegotiationRequest(tRFC_MCB* p_mcb, uint8_t dlci, uint16_t mtu) {
  uint8_t flow;
  uint8_t cl;
  uint8_t k;

  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if (p_port == nullptr) {
    log::warn("Unable to find DLCI port dlci:{}", dlci);
    return;
  }

  if (p_mcb->state != RFC_MX_STATE_CONNECTED) {
    log::warn("Multiplexer is in unexpected dlci:{} state:{}", dlci,
              rfcomm_mx_state_text(p_mcb->state).c_str());
    return;
  }

  /* Negotiate the flow control mechanism.  If flow control mechanism for the
   * mux has not been set yet, use credits.  If it has been set, use that value.
   */
  flow = (p_mcb->flow == PORT_FC_UNDEFINED) ? PORT_FC_CREDIT : p_mcb->flow;

  /* Set convergence layer and number of credits (k) */
  if (flow == PORT_FC_CREDIT) {
    cl = RFCOMM_PN_CONV_LAYER_CBFC_I;
    k = (p_port->credit_rx_max < RFCOMM_K_MAX) ? p_port->credit_rx_max : RFCOMM_K_MAX;
    p_port->credit_rx = k;
  } else {
    cl = RFCOMM_PN_CONV_LAYER_TYPE_1;
    k = 0;
  }

  /* Send Parameter Negotiation Command UIH frame */
  p_port->rfc.expected_rsp |= RFC_RSP_PN;

  rfc_send_pn(p_mcb, dlci, true, mtu, cl, k);

  rfc_port_timer_start(p_port, RFC_T2_TIMEOUT);
}

/*******************************************************************************
 *
 * Function         RFCOMM_ParameterNegotiationResponse
 *
 * Description      This function is called by the user app to acknowledge
 *                  DLC parameter negotiation.
 *
 ******************************************************************************/
void RFCOMM_ParameterNegotiationResponse(tRFC_MCB* p_mcb, uint8_t dlci, uint16_t mtu, uint8_t cl,
                                         uint8_t k) {
  if (p_mcb->state != RFC_MX_STATE_CONNECTED) {
    return;
  }

  /* Send Parameter Negotiation Response UIH frame */
  rfc_send_pn(p_mcb, dlci, false, mtu, cl, k);
}

/*******************************************************************************
 *
 * Function         RFCOMM_PortParameterNegotiationRequest
 *
 * Description      This function is called by the user app to start
 *                  Remote Port parameter negotiation.  Port emulation can
 *                  send this request before actually establishing the DLC.
 *                  In this case the function will allocate RFCOMM connection
 *                  control block.
 *
 ******************************************************************************/
void RFCOMM_PortParameterNegotiationRequest(tRFC_MCB* p_mcb, uint8_t dlci,
                                            PortSettings* p_settings) {
  if (p_mcb->state != RFC_MX_STATE_CONNECTED) {
    PORT_PortNegCnf(p_mcb, dlci, nullptr, RFCOMM_ERROR);
    return;
  }

  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if (p_port == nullptr) {
    log::warn("Unable to find DLCI port dlci:{}", dlci);
    return;
  }

  /* Send Parameter Negotiation Command UIH frame */
  if (!p_settings) {
    p_port->rfc.expected_rsp |= RFC_RSP_RPN_REPLY;
  } else {
    p_port->rfc.expected_rsp |= RFC_RSP_RPN;
  }

  rfc_send_rpn(p_mcb, dlci, true, p_settings, RFCOMM_RPN_PM_MASK);
  rfc_port_timer_start(p_port, RFC_T2_TIMEOUT);
}

/*******************************************************************************
 *
 * Function         RFCOMM_PortParameterNegotiationResponse
 *
 * Description      This function is called by the user app to acknowledge
 *                  Port parameters negotiation.
 *
 ******************************************************************************/
void RFCOMM_PortParameterNegotiationResponse(tRFC_MCB* p_mcb, uint8_t dlci,
                                             PortSettings* p_settings, uint16_t param_mask) {
  if (p_mcb->state != RFC_MX_STATE_CONNECTED) {
    return;
  }

  rfc_send_rpn(p_mcb, dlci, false, p_settings, param_mask);
}

/*******************************************************************************
 *
 * Function         RFCOMM_ControlReq
 *
 * Description      This function is called by the port entity to send control
 *                  parameters to remote port emulation entity.
 *
 ******************************************************************************/
void RFCOMM_ControlReq(tRFC_MCB* p_mcb, uint8_t dlci, tPORT_CTRL* p_pars) {
  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if (p_port == nullptr) {
    log::warn("Unable to find DLCI port dlci:{}", dlci);
    return;
  }

  if ((p_port->state != PORT_CONNECTION_STATE_OPENED) || (p_port->rfc.state != RFC_STATE_OPENED)) {
    return;
  }

  p_port->port_ctrl |= PORT_CTRL_REQ_SENT;

  p_port->rfc.expected_rsp |= RFC_RSP_MSC;

  rfc_send_msc(p_mcb, dlci, true, p_pars);
  rfc_port_timer_start(p_port, RFC_T2_TIMEOUT);
}

/*******************************************************************************
 *
 * Function         RFCOMM_FlowReq
 *
 * Description      This function is called by the port entity when flow
 *                  control state has changed.  Enable flag passed shows if
 *                  port can accept more data.
 *
 ******************************************************************************/
void RFCOMM_FlowReq(tRFC_MCB* p_mcb, uint8_t dlci, bool enable) {
  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if (p_port == nullptr) {
    log::warn("Unable to find DLCI port dlci:{}", dlci);
    return;
  }

  if ((p_port->state != PORT_CONNECTION_STATE_OPENED) || (p_port->rfc.state != RFC_STATE_OPENED)) {
    return;
  }

  p_port->local_ctrl.fc = !enable;

  p_port->rfc.expected_rsp |= RFC_RSP_MSC;

  rfc_send_msc(p_mcb, dlci, true, &p_port->local_ctrl);
  rfc_port_timer_start(p_port, RFC_T2_TIMEOUT);
}

/*******************************************************************************
 *
 * Function         RFCOMM_LineStatusReq
 *
 * Description      This function is called by the port entity when line
 *                  status should be delivered to the peer.
 *
 ******************************************************************************/
void RFCOMM_LineStatusReq(tRFC_MCB* p_mcb, uint8_t dlci, uint8_t status) {
  tPORT* p_port = port_find_mcb_dlci_port(p_mcb, dlci);
  if (p_port == nullptr) {
    log::warn("Unable to find DLCI port dlci:{}", dlci);
    return;
  }

  if ((p_port->state != PORT_CONNECTION_STATE_OPENED) || (p_port->rfc.state != RFC_STATE_OPENED)) {
    return;
  }

  p_port->rfc.expected_rsp |= RFC_RSP_RLS;

  rfc_send_rls(p_mcb, dlci, true, status);
  rfc_port_timer_start(p_port, RFC_T2_TIMEOUT);
}

/*******************************************************************************
 *
 * Function         RFCOMM_DlcReleaseReq
 *
 * Description      This function is called by the PORT unit to close DLC
 *
 ******************************************************************************/
void RFCOMM_DlcReleaseReq(tRFC_MCB* p_mcb, uint8_t dlci) {
  rfc_port_sm_execute(port_find_mcb_dlci_port(p_mcb, dlci), RFC_PORT_EVENT_CLOSE, nullptr);
}

/*******************************************************************************
 *
 * Function         RFCOMM_DataReq
 *
 * Description      This function is called by the user app to send data buffer
 *
 ******************************************************************************/
void RFCOMM_DataReq(tRFC_MCB* p_mcb, uint8_t dlci, BT_HDR* p_buf) {
  rfc_port_sm_execute(port_find_mcb_dlci_port(p_mcb, dlci), RFC_PORT_EVENT_DATA, p_buf);
}
