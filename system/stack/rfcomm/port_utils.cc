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

/******************************************************************************
 *
 *  Port Emulation entity utilities
 *
 ******************************************************************************/

#define LOG_TAG "rfcomm_port_utils"

#include <bluetooth/log.h>

#include <cstdint>
#include <cstring>

#include "internal_include/bt_target.h"
#include "osi/include/allocator.h"
#include "osi/include/mutex.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/l2cdefs.h"
#include "stack/rfcomm/port_int.h"
#include "stack/rfcomm/rfc_int.h"
#include "types/raw_address.h"

using namespace bluetooth;

static const PortSettings default_port_settings = {
        PORT_BAUD_RATE_9600,
        PORT_8_BITS,
        PORT_ONESTOPBIT,
        PORT_PARITY_NO,
        PORT_ODD_PARITY,
        PORT_FC_OFF,
        0, /* No rx_char */
        PORT_XON_DC1,
        PORT_XOFF_DC3,
};

/*******************************************************************************
 *
 * Function         port_allocate_port
 *
 * Description      Look through the Port Control Blocks for a free one.  Note
 *                  that one server can open several ports with the same SCN
 *                  if it can support simulteneous requests from different
 *                  clients.
 *
 * Returns          Pointer to the PORT or NULL if not found
 *
 ******************************************************************************/
tPORT* port_allocate_port(uint8_t dlci, const RawAddress& bd_addr) {
  uint8_t port_index = rfc_cb.rfc.last_port_index + static_cast<uint8_t>(1);
  // Loop at most MAX_RFC_PORTS items
  for (int loop_counter = 0; loop_counter < MAX_RFC_PORTS; loop_counter++, port_index++) {
    if (port_index >= MAX_RFC_PORTS) {
      port_index = 0;
    }
    tPORT* p_port = &rfc_cb.port.port[port_index];
    if (!p_port->in_use) {
      // Assume that we already called port_release_port on this
      memset(p_port, 0, sizeof(tPORT));
      p_port->in_use = true;
      // handle is a port handle starting from 1
      p_port->handle = port_index + static_cast<uint8_t>(1);
      // During the open set default state for the port connection
      port_set_defaults(p_port);
      p_port->rfc.port_timer = alarm_new("rfcomm_port.port_timer");
      p_port->dlci = dlci;
      p_port->bd_addr = bd_addr;
      rfc_cb.rfc.last_port_index = port_index;
      log::verbose("rfc_cb.port.port[{}]:{} chosen, last_port_index:{}, bd_addr={}", port_index,
                   std::format_ptr(p_port), rfc_cb.rfc.last_port_index, bd_addr);
      return p_port;
    }
  }
  log::warn("running out of free ports for dlci {}, bd_addr {}", dlci, bd_addr);
  return nullptr;
}

/*******************************************************************************
 *
 * Function         port_set_defaults
 *
 * Description      Set defualt port parameters
 *
 *
 ******************************************************************************/
void port_set_defaults(tPORT* p_port) {
  p_port->ev_mask = 0;
  p_port->p_callback = nullptr;
  p_port->port_ctrl = 0;
  p_port->line_status = 0;
  p_port->rx_flag_ev_pending = false;
  p_port->peer_mtu = RFCOMM_DEFAULT_MTU;

  p_port->user_port_settings = default_port_settings;
  p_port->peer_port_settings = default_port_settings;

  p_port->credit_tx = 0;
  p_port->credit_rx = 0;

  memset(&p_port->local_ctrl, 0, sizeof(p_port->local_ctrl));
  memset(&p_port->peer_ctrl, 0, sizeof(p_port->peer_ctrl));
  memset(&p_port->rx, 0, sizeof(p_port->rx));
  memset(&p_port->tx, 0, sizeof(p_port->tx));

  p_port->tx.queue = fixed_queue_new(SIZE_MAX);
  p_port->rx.queue = fixed_queue_new(SIZE_MAX);
}

/*******************************************************************************
 *
 * Function         port_select_mtu
 *
 * Description      Select MTU which will best serve connection from our
 *                  point of view.
 *                  If our device is 1.2 or lower we calculate how many DH5s
 *                  fit into 1 RFCOMM buffer.
 *
 *
 ******************************************************************************/
void port_select_mtu(tPORT* p_port) {
  uint16_t packet_size;

  /* Will select MTU only if application did not setup something */
  if (p_port->mtu == 0) {
    /* find packet size which connection supports */
    packet_size = get_btm_client_interface().peer.BTM_GetMaxPacketSize(p_port->bd_addr);
    if (packet_size == 0) {
      /* something is very wrong */
      log::warn("bad packet size 0 for{}", p_port->bd_addr);
      p_port->mtu = RFCOMM_DEFAULT_MTU;
    } else {
      /* We try to negotiate MTU that each packet can be split into whole
      number of max packets.  For example if link is 1.2 max packet size is 339
      bytes.
      At first calculate how many whole packets it is.  MAX L2CAP is 1691 + 4
      overhead.
      1695, that will be 5 Dh5 packets.  Now maximum RFCOMM packet is
      5 * 339 = 1695. Minus 4 bytes L2CAP header 1691.  Minus RFCOMM 6 bytes
      header overhead 1685

      For EDR 2.0 packet size is 1027.  So we better send RFCOMM packet as 1
      3DH5 packet
      1 * 1027 = 1027.  Minus 4 bytes L2CAP header 1023.  Minus RFCOMM 6 bytes
      header overhead 1017 */
      if ((L2CAP_MTU_SIZE + L2CAP_PKT_OVERHEAD) >= packet_size) {
        p_port->mtu = ((L2CAP_MTU_SIZE + L2CAP_PKT_OVERHEAD) / packet_size * packet_size) -
                      RFCOMM_DATA_OVERHEAD - L2CAP_PKT_OVERHEAD;
        log::verbose("selected {} based on connection speed", p_port->mtu);
      } else {
        p_port->mtu = L2CAP_MTU_SIZE - RFCOMM_DATA_OVERHEAD;
        log::verbose("selected {} based on l2cap PDU size", p_port->mtu);
      }
    }
  } else {
    log::verbose("application selected {}", p_port->mtu);
  }
  p_port->credit_rx_max = (PORT_RX_HIGH_WM / p_port->mtu);
  if (p_port->credit_rx_max > PORT_RX_BUF_HIGH_WM) {
    p_port->credit_rx_max = PORT_RX_BUF_HIGH_WM;
  }
  p_port->credit_rx_low = (PORT_RX_LOW_WM / p_port->mtu);
  if (p_port->credit_rx_low > PORT_RX_BUF_LOW_WM) {
    p_port->credit_rx_low = PORT_RX_BUF_LOW_WM;
  }
  p_port->rx_buf_critical = (PORT_RX_CRITICAL_WM / p_port->mtu);
  if (p_port->rx_buf_critical > PORT_RX_BUF_CRITICAL_WM) {
    p_port->rx_buf_critical = PORT_RX_BUF_CRITICAL_WM;
  }
  log::verbose("credit_rx_max {}, credit_rx_low {}, rx_buf_critical {}", p_port->credit_rx_max,
               p_port->credit_rx_low, p_port->rx_buf_critical);
}

/*******************************************************************************
 *
 * Function         port_release_port
 *
 * Description      Release port control block.
 *
 * Returns          Pointer to the PORT or NULL if not found
 *
 ******************************************************************************/
void port_release_port(tPORT* p_port) {
  log::verbose("p_port: {} state: {} keep_handle: {}", std::format_ptr(p_port), p_port->rfc.state,
               p_port->keep_port_handle);

  mutex_global_lock();
  BT_HDR* p_buf;
  while ((p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_port->rx.queue)) != nullptr) {
    osi_free(p_buf);
  }
  p_port->rx.queue_size = 0;

  while ((p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_port->tx.queue)) != nullptr) {
    osi_free(p_buf);
  }
  p_port->tx.queue_size = 0;
  mutex_global_unlock();

  alarm_cancel(p_port->rfc.port_timer);

  p_port->state = PORT_CONNECTION_STATE_CLOSED;

  if (p_port->rfc.state == RFC_STATE_CLOSED) {
    if (p_port->rfc.p_mcb) {
      p_port->rfc.p_mcb->port_handles[p_port->dlci] = 0;

      /* If there are no more ports opened on this MCB release it */
      rfc_check_mcb_active(p_port->rfc.p_mcb);
    }

    rfc_port_timer_stop(p_port);

    mutex_global_lock();
    fixed_queue_free(p_port->tx.queue, nullptr);
    p_port->tx.queue = nullptr;
    fixed_queue_free(p_port->rx.queue, nullptr);
    p_port->rx.queue = nullptr;
    mutex_global_unlock();

    if (p_port->keep_port_handle) {
      log::verbose("Re-initialize handle: {}", p_port->handle);

      /* save event mask and callback */
      uint32_t mask = p_port->ev_mask;
      tPORT_CALLBACK* p_port_cb = p_port->p_callback;
      PortSettings user_port_settings = p_port->user_port_settings;

      port_set_defaults(p_port);

      /* restore */
      p_port->ev_mask = mask;
      p_port->p_callback = p_port_cb;
      p_port->user_port_settings = user_port_settings;
      p_port->mtu = p_port->keep_mtu;

      p_port->state = PORT_CONNECTION_STATE_OPENING;
      p_port->rfc.p_mcb = nullptr;
      if (p_port->is_server) {
        p_port->dlci &= 0xfe;
      }

      p_port->local_ctrl.modem_signal = p_port->default_signal_state;
      p_port->bd_addr = RawAddress::kAny;
    } else {
      log::verbose("Clean-up handle: {}", p_port->handle);
      alarm_free(p_port->rfc.port_timer);
      memset(p_port, 0, sizeof(tPORT));
    }
  }
}

/*******************************************************************************
 *
 * Function         port_find_mcb
 *
 * Description      This function checks if connection exists to device with
 *                  the address.
 *
 ******************************************************************************/
tRFC_MCB* port_find_mcb(const RawAddress& bd_addr) {
  for (tRFC_MCB& mcb : rfc_cb.port.rfc_mcb) {
    if ((mcb.state != RFC_MX_STATE_IDLE) && (mcb.bd_addr == bd_addr)) {
      /* Multiplexer channel found do not change anything */
      log::verbose("found, bd_addr:{}, rfc_mcb:{}, lcid:0x{:x}", bd_addr, std::format_ptr(&mcb),
                   mcb.lcid);
      return &mcb;
    }
  }
  log::warn("not found, bd_addr:{}", bd_addr);
  return nullptr;
}

/*******************************************************************************
 *
 * Function         port_find_mcb_dlci_port
 *
 * Description      Find port on the multiplexer channel based on DLCI.  If
 *                  this port with DLCI not found try to use even DLCI.  This
 *                  is for the case when client is establishing connection on
 *                  none-initiator MCB.
 *
 * Returns          Pointer to the PORT or NULL if not found
 *
 ******************************************************************************/
tPORT* port_find_mcb_dlci_port(tRFC_MCB* p_mcb, uint8_t dlci) {
  if (!p_mcb) {
    log::error("p_mcb is null, dlci={}", dlci);
    return nullptr;
  }

  if (dlci > RFCOMM_MAX_DLCI) {
    log::warn("DLCI {} is too large, bd_addr={}, p_mcb={}", dlci, p_mcb->bd_addr,
              std::format_ptr(p_mcb));
    return nullptr;
  }

  uint8_t handle = p_mcb->port_handles[dlci];
  if (handle == 0) {
    log::info("Cannot find allocated RFCOMM app port for DLCI {} on {}, p_mcb={}", dlci,
              p_mcb->bd_addr, std::format_ptr(p_mcb));
    return nullptr;
  }
  return &rfc_cb.port.port[handle - 1];
}

/*******************************************************************************
 *
 * Function         port_find_dlci_port
 *
 * Description      Find port with DLCI not assigned to multiplexer channel
 *
 * Returns          Pointer to the PORT or NULL if not found
 *
 ******************************************************************************/
tPORT* port_find_dlci_port(uint8_t dlci) {
  for (tPORT& port : rfc_cb.port.port) {
    if (port.in_use && (port.rfc.p_mcb == nullptr)) {
      if (port.dlci == dlci) {
        return &port;
      } else if ((dlci & 0x01) && (port.dlci == (dlci - 1))) {
        port.dlci++;
        return &port;
      }
    }
  }
  return nullptr;
}

/*******************************************************************************
 *
 * Function         port_find_port
 *
 * Description      Find port with DLCI, address
 *
 * Returns          Pointer to the PORT or NULL if not found
 *
 ******************************************************************************/
tPORT* port_find_port(uint8_t dlci, const RawAddress& bd_addr) {
  for (tPORT& port : rfc_cb.port.port) {
    if (port.in_use && (port.dlci == dlci) && (port.bd_addr == bd_addr)) {
      return &port;
    }
  }
  return nullptr;
}

/*******************************************************************************
 *
 * Function         port_flow_control_user
 *
 * Description      Check the current user flow control and if necessary return
 *                  events to be send to the user based on the user's specified
 *                  flow control type.
 *
 * Returns          event mask to be returned to the application
 *
 ******************************************************************************/
uint32_t port_flow_control_user(tPORT* p_port) {
  uint32_t event = 0;

  /* Flow control to the user can be caused by flow controlling by the peer */
  /* (FlowInd, or flow control by the peer RFCOMM (Fcon) or internally if */
  /* tx_queue is full */
  bool fc = p_port->tx.peer_fc || !p_port->rfc.p_mcb || !p_port->rfc.p_mcb->peer_ready ||
            (p_port->tx.queue_size > PORT_TX_HIGH_WM) ||
            (fixed_queue_length(p_port->tx.queue) > PORT_TX_BUF_HIGH_WM);

  if (p_port->tx.user_fc == fc) {
    return 0;
  }

  p_port->tx.user_fc = fc;

  if (fc) {
    event = PORT_EV_FC;
  } else {
    event = PORT_EV_FC | PORT_EV_FCS;
  }

  return event;
}

/*******************************************************************************
 *
 * Function         port_get_signal_changes
 *
 * Description      Check modem signals that has been changed
 *
 * Returns          event mask to be returned to the application
 *
 ******************************************************************************/
uint32_t port_get_signal_changes(tPORT* p_port, uint8_t old_signals, uint8_t signal) {
  uint8_t changed_signals = (signal ^ old_signals);
  uint32_t events = 0;

  if (changed_signals & PORT_DTRDSR_ON) {
    events |= PORT_EV_DSR;

    if (signal & PORT_DTRDSR_ON) {
      events |= PORT_EV_DSRS;
    }
  }

  if (changed_signals & PORT_CTSRTS_ON) {
    events |= PORT_EV_CTS;

    if (signal & PORT_CTSRTS_ON) {
      events |= PORT_EV_CTSS;
    }
  }

  if (changed_signals & PORT_RING_ON) {
    events |= PORT_EV_RING;
  }

  if (changed_signals & PORT_DCD_ON) {
    events |= PORT_EV_RLSD;

    if (signal & PORT_DCD_ON) {
      events |= PORT_EV_RLSDS;
    }
  }

  return p_port->ev_mask & events;
}

/*******************************************************************************
 *
 * Function         port_flow_control_peer
 *
 * Description      Send flow control messages to the peer for both enabling
 *                  and disabling flow control, for both credit-based and
 *                  TS 07.10 flow control mechanisms.
 *
 * Returns          nothing
 *
 ******************************************************************************/
void port_flow_control_peer(tPORT* p_port, bool enable, uint16_t count) {
  if (!p_port->rfc.p_mcb) {
    return;
  }

  /* If using credit based flow control */
  if (p_port->rfc.p_mcb->flow == PORT_FC_CREDIT) {
    /* if want to enable flow from peer */
    if (enable) {
      /* update rx credits */
      if (count > p_port->credit_rx) {
        p_port->credit_rx = 0;
      } else {
        p_port->credit_rx -= count;
      }

      /* If credit count is less than low credit watermark, and user */
      /* did not force flow control, send a credit update */
      /* There might be a special case when we just adjusted rx_max */
      if ((p_port->credit_rx <= p_port->credit_rx_low) && !p_port->rx.user_fc &&
          (p_port->credit_rx_max > p_port->credit_rx)) {
        rfc_send_credit(p_port->rfc.p_mcb, p_port->dlci,
                        (uint8_t)(p_port->credit_rx_max - p_port->credit_rx));

        p_port->credit_rx = p_port->credit_rx_max;

        p_port->rx.peer_fc = false;
      }
    } else {
      /* else want to disable flow from peer */
      /* if client registered data callback, just do what they want */
      if (p_port->p_data_callback || p_port->p_data_co_callback) {
        p_port->rx.peer_fc = true;
      } else if (fixed_queue_length(p_port->rx.queue) >= p_port->credit_rx_max) {
        /* if queue count reached credit rx max, set peer fc */
        p_port->rx.peer_fc = true;
      }
    }
  } else {
    /* else using TS 07.10 flow control */
    /* if want to enable flow from peer */
    if (enable) {
      /* If rfcomm suspended traffic from the peer based on the rx_queue_size */
      /* check if it can be resumed now */
      if (p_port->rx.peer_fc && (p_port->rx.queue_size < PORT_RX_LOW_WM) &&
          (fixed_queue_length(p_port->rx.queue) < PORT_RX_BUF_LOW_WM)) {
        p_port->rx.peer_fc = false;

        /* If user did not force flow control allow traffic now */
        if (!p_port->rx.user_fc) {
          RFCOMM_FlowReq(p_port->rfc.p_mcb, p_port->dlci, true);
        }
      }
    } else {
      /* else want to disable flow from peer */
      /* if client registered data callback, just do what they want */
      if (p_port->p_data_callback || p_port->p_data_co_callback) {
        p_port->rx.peer_fc = true;
        RFCOMM_FlowReq(p_port->rfc.p_mcb, p_port->dlci, false);
      } else if (((p_port->rx.queue_size > PORT_RX_HIGH_WM) ||
                  (fixed_queue_length(p_port->rx.queue) > PORT_RX_BUF_HIGH_WM)) &&
                 !p_port->rx.peer_fc) {
        /* Check the size of the rx queue.  If it exceeds certain */
        /* level and flow control has not been sent to the peer do it now */
        log::verbose("PORT_DataInd Data reached HW. Sending FC set.");

        p_port->rx.peer_fc = true;
        RFCOMM_FlowReq(p_port->rfc.p_mcb, p_port->dlci, false);
      }
    }
  }
}
