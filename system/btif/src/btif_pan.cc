/******************************************************************************
 *
 *  Copyright 2009-2012 Broadcom Corporation
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
 *  Filename:      btif_pan.c
 *
 *  Description:   PAN Profile Bluetooth Interface
 *
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_pan"

#include "btif/include/btif_pan.h"

#include <arpa/inet.h>
#include <base/functional/bind.h>
#include <base/location.h>
#include <bluetooth/log.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bta/include/bta_pan_api.h"
#include "btif/include/btif_common.h"
#include "btif/include/btif_pan_internal.h"
#include "btif/include/btif_sock_thread.h"
#include "hardware/bluetooth.h"
#include "hci/controller_interface.h"
#include "include/hardware/bt_pan.h"
#include "internal_include/bt_target.h"
#include "main/shim/entry.h"
#include "main/shim/helpers.h"
#include "osi/include/allocator.h"
#include "osi/include/compat.h"
#include "osi/include/osi.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/main_thread.h"
#include "stack/include/pan_api.h"
#include "types/raw_address.h"

#ifdef __ANDROID__
#include <android/sysprop/BluetoothProperties.sysprop.h>
#endif

#define FORWARD_IGNORE 1
#define FORWARD_SUCCESS 0
#define FORWARD_FAILURE (-1)
#define FORWARD_CONGEST (-2)

#define asrt(s)                                           \
  do {                                                    \
    if (!(s))                                             \
      log::error("btif_pan: ## assert {} failed ##", #s); \
  } while (0)

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

using namespace bluetooth;

btpan_cb_t btpan_cb;

static bool jni_initialized;
static bool stack_initialized;

static bt_status_t btpan_jni_init(const btpan_callbacks_t* callbacks);
static void btpan_jni_cleanup();
static bt_status_t btpan_connect(const RawAddress* bd_addr, int local_role, int remote_role);
static bt_status_t btpan_disconnect(const RawAddress* bd_addr);
static bt_status_t btpan_enable(int local_role);
static int btpan_get_local_role(void);

static void btpan_tap_fd_signaled(int fd, int type, int flags, uint32_t user_id);
static void btpan_cleanup_conn(btpan_conn_t* conn);
static void bta_pan_callback(tBTA_PAN_EVT event, tBTA_PAN* p_data);
static void btu_exec_tap_fd_read(const int fd);

static btpan_interface_t pan_if = {sizeof(pan_if),       btpan_jni_init, nullptr,
                                   btpan_get_local_role, btpan_connect,  btpan_disconnect,
                                   btpan_jni_cleanup};

const btpan_interface_t* btif_pan_get_interface() { return &pan_if; }

/*******************************************************************************
 **
 ** Function        btif_pan_init
 **
 ** Description     initializes the pan interface
 **
 ** Returns         bt_status_t
 **
 ******************************************************************************/
void btif_pan_init() {
  log::verbose("jni_initialized = {}, btpan_cb.enabled:{}", jni_initialized, btpan_cb.enabled);
  stack_initialized = true;

  if (jni_initialized && !btpan_cb.enabled) {
    log::verbose("Enabling PAN....");
    memset(&btpan_cb, 0, sizeof(btpan_cb));
    btpan_cb.tap_fd = INVALID_FD;
    btpan_cb.flow = 1;
    for (int i = 0; i < MAX_PAN_CONNS; i++) {
      btpan_cleanup_conn(&btpan_cb.conns[i]);
    }
    BTA_PanEnable(bta_pan_callback);
    btpan_cb.enabled = 1;

    int role = BTPAN_ROLE_NONE;
#ifdef __ANDROID__
    if (android::sysprop::BluetoothProperties::isProfilePanNapEnabled().value_or(false)) {
      role |= BTPAN_ROLE_PANNAP;
    }
#endif
    role |= BTPAN_ROLE_PANU;
    btpan_enable(role);
  }
}

static void pan_disable() {
  if (btpan_cb.enabled) {
    btpan_cb.enabled = 0;
    BTA_PanDisable();
    if (btpan_cb.tap_fd != INVALID_FD) {
      btpan_tap_close(btpan_cb.tap_fd);
      btpan_cb.tap_fd = INVALID_FD;
    }
  }
}

void btif_pan_cleanup() {
  if (!stack_initialized) {
    return;
  }

  // Bluetooth is shutting down, invalidate all BTA PAN handles
  for (int i = 0; i < MAX_PAN_CONNS; i++) {
    btpan_cleanup_conn(&btpan_cb.conns[i]);
  }

  pan_disable();
  stack_initialized = false;
}

static btpan_callbacks_t callback;
static bt_status_t btpan_jni_init(const btpan_callbacks_t* callbacks) {
  log::verbose("stack_initialized = {}, btpan_cb.enabled:{}", stack_initialized, btpan_cb.enabled);
  callback = *callbacks;
  jni_initialized = true;
  if (stack_initialized && !btpan_cb.enabled) {
    btif_pan_init();
  }
  return BT_STATUS_SUCCESS;
}

static void btpan_jni_cleanup() {
  pan_disable();
  jni_initialized = false;
}

static inline int bta_role_to_btpan(tBTA_PAN_ROLE bta_pan_role) {
  int btpan_role = 0;
  if (bta_pan_role & PAN_ROLE_NAP_SERVER) {
    btpan_role |= BTPAN_ROLE_PANNAP;
  }
  if (bta_pan_role & PAN_ROLE_CLIENT) {
    btpan_role |= BTPAN_ROLE_PANU;
  }
  return btpan_role;
}

static inline tBTA_PAN_ROLE btpan_role_to_bta(int btpan_role) {
  tBTA_PAN_ROLE bta_pan_role = PAN_ROLE_INACTIVE;
  if (btpan_role & BTPAN_ROLE_PANNAP) {
    bta_pan_role |= PAN_ROLE_NAP_SERVER;
  }
  if (btpan_role & BTPAN_ROLE_PANU) {
    bta_pan_role |= PAN_ROLE_CLIENT;
  }
  return bta_pan_role;
}

static tBTA_PAN_ROLE btpan_dev_local_role;
static tBTA_PAN_ROLE_INFO bta_panu_info = {std::string(PANU_SERVICE_NAME), 0};
static tBTA_PAN_ROLE_INFO bta_pan_nap_info = {std::string(PAN_NAP_SERVICE_NAME), 1};

static bt_status_t btpan_enable(int local_role) {
  const tBTA_PAN_ROLE bta_pan_role = btpan_role_to_bta(local_role);
  BTA_PanSetRole(bta_pan_role, bta_panu_info, bta_pan_nap_info);
  btpan_dev_local_role = local_role;
  return BT_STATUS_SUCCESS;
}

static int btpan_get_local_role() { return static_cast<int>(btpan_dev_local_role); }

static bt_status_t btpan_connect(const RawAddress* bd_addr, int local_role, int remote_role) {
  tBTA_PAN_ROLE bta_local_role = btpan_role_to_bta(local_role);
  tBTA_PAN_ROLE bta_remote_role = btpan_role_to_bta(remote_role);
  btpan_new_conn(-1, *bd_addr, bta_local_role, bta_remote_role);
  BTA_PanOpen(*bd_addr, bta_local_role, bta_remote_role);
  return BT_STATUS_SUCCESS;
}

constexpr uint16_t BTIF_PAN_CB_DISCONNECTING = 0x8401;
static void btif_in_pan_generic_evt(uint16_t event, char* p_param) {
  log::verbose("event={}", event);
  switch (event) {
    case BTIF_PAN_CB_DISCONNECTING: {
      RawAddress* bd_addr = (RawAddress*)p_param;
      btpan_conn_t* conn = btpan_find_conn_addr(*bd_addr);
      int btpan_conn_local_role;
      int btpan_remote_role;
      asrt(conn != NULL);
      if (conn) {
        btpan_conn_local_role = bta_role_to_btpan(conn->local_role);
        btpan_remote_role = bta_role_to_btpan(conn->remote_role);
        callback.connection_state_cb(BTPAN_STATE_DISCONNECTING, BT_STATUS_SUCCESS, &conn->peer,
                                     btpan_conn_local_role, btpan_remote_role);
      }
    } break;
    default: {
      log::warn("Unknown event 0x{:x}", event);
    } break;
  }
}

static bt_status_t btpan_disconnect(const RawAddress* bd_addr) {
  btpan_conn_t* conn = btpan_find_conn_addr(*bd_addr);
  if (conn && conn->handle >= 0) {
    /* Inform the application that the disconnect has been initiated
     * successfully */
    btif_transfer_context(btif_in_pan_generic_evt, BTIF_PAN_CB_DISCONNECTING, (char*)bd_addr,
                          sizeof(RawAddress), NULL);
    BTA_PanClose(conn->handle);
    return BT_STATUS_SUCCESS;
  }
  return BT_STATUS_DEVICE_NOT_FOUND;
}

static int pan_pth = -1;
void create_tap_read_thread(int tap_fd) {
  if (pan_pth < 0) {
    pan_pth = btsock_thread_create(btpan_tap_fd_signaled, NULL);
  }
  if (pan_pth >= 0) {
    btsock_thread_add_fd(pan_pth, tap_fd, 0, SOCK_THREAD_FD_RD, 0);
  }
}

void destroy_tap_read_thread(void) {
  if (pan_pth >= 0) {
    btsock_thread_exit(pan_pth);
    pan_pth = -1;
  }
}

static int tap_if_up(const char* devname, const RawAddress& addr) {
  struct ifreq ifr;
  int sk, err;

  sk = socket(AF_INET, SOCK_DGRAM, 0);
  if (sk < 0) {
    return -1;
  }

  // set mac addr
  memset(&ifr, 0, sizeof(ifr));
  osi_strlcpy(ifr.ifr_name, devname, IFNAMSIZ);
  err = ioctl(sk, SIOCGIFHWADDR, &ifr);
  if (err < 0) {
    log::error("Could not get network hardware for interface:{}, errno:{}", devname,
               strerror(errno));
    close(sk);
    return -1;
  }

  osi_strlcpy(ifr.ifr_name, devname, IFNAMSIZ);
  memcpy(ifr.ifr_hwaddr.sa_data, addr.address, 6);

  /* The IEEE has specified that the most significant bit of the most
   * significant byte is used to
   * determine a multicast address. If its a 1, that means multicast, 0 means
   * unicast.
   * Kernel returns an error if we try to set a multicast address for the
   * tun-tap ethernet interface.
   * Mask this bit to avoid any issue with auto generated address.
   */
  if (ifr.ifr_hwaddr.sa_data[0] & 0x01) {
    log::warn("Not a unicast MAC address, force multicast bit flipping");
    ifr.ifr_hwaddr.sa_data[0] &= ~0x01;
  }

  err = ioctl(sk, SIOCSIFHWADDR, (caddr_t)&ifr);

  if (err < 0) {
    log::error("Could not set bt address for interface:{}, errno:{}", devname, strerror(errno));
    close(sk);
    return -1;
  }

  // bring it up
  memset(&ifr, 0, sizeof(ifr));
  osi_strlcpy(ifr.ifr_name, devname, IF_NAMESIZE);

  ifr.ifr_flags |= IFF_UP;
  ifr.ifr_flags |= IFF_MULTICAST;

  err = ioctl(sk, SIOCSIFFLAGS, (caddr_t)&ifr);

  if (err < 0) {
    log::error("Could not bring up network interface:{}, errno:{}", devname, errno);
    close(sk);
    return -1;
  }
  close(sk);
  log::verbose("network interface: {} is up", devname);
  return 0;
}

static int tap_if_down(const char* devname) {
  struct ifreq ifr;
  int sk;

  sk = socket(AF_INET, SOCK_DGRAM, 0);
  if (sk < 0) {
    return -1;
  }

  memset(&ifr, 0, sizeof(ifr));
  osi_strlcpy(ifr.ifr_name, devname, IF_NAMESIZE);

  ifr.ifr_flags &= ~IFF_UP;

  ioctl(sk, SIOCSIFFLAGS, (caddr_t)&ifr);

  close(sk);

  return 0;
}

void btpan_set_flow_control(bool enable) {
  if (btpan_cb.tap_fd == -1) {
    return;
  }

  btpan_cb.flow = enable;
  if (enable) {
    btsock_thread_add_fd(pan_pth, btpan_cb.tap_fd, 0, SOCK_THREAD_FD_RD, 0);
    do_in_main_thread(base::BindOnce(btu_exec_tap_fd_read, btpan_cb.tap_fd));
  }
}

int btpan_tap_open() {
  struct ifreq ifr;
  int fd, err;
  const char* clonedev = "/dev/tun";

  /* open the clone device */

  fd = open(clonedev, O_RDWR);
  if (fd < 0) {
    log::verbose("could not open {}, err:{}", clonedev, errno);
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

  osi_strlcpy(ifr.ifr_name, TAP_IF_NAME, IFNAMSIZ);

  /* try to create the device */
  err = ioctl(fd, TUNSETIFF, (void*)&ifr);
  if (err < 0) {
    log::verbose("ioctl error:{}, errno:{}", err, strerror(errno));
    close(fd);
    return err;
  }
  if (tap_if_up(TAP_IF_NAME,
                bluetooth::ToRawAddress(bluetooth::shim::GetController()->GetMacAddress())) == 0) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
  }
  log::error("can not bring up tap interface:{}", TAP_IF_NAME);
  close(fd);
  return INVALID_FD;
}

int btpan_tap_send(int tap_fd, const RawAddress& src, const RawAddress& dst, uint16_t proto,
                   const char* buf, uint16_t len, bool /* ext */, bool /* forward */) {
  if (tap_fd != INVALID_FD) {
    tETH_HDR eth_hdr;
    eth_hdr.h_dest = dst;
    eth_hdr.h_src = src;
    eth_hdr.h_proto = htons(proto);
    char packet[TAP_MAX_PKT_WRITE_LEN + sizeof(tETH_HDR)];
    memcpy(packet, &eth_hdr, sizeof(tETH_HDR));
    if (len > TAP_MAX_PKT_WRITE_LEN) {
      log::error("btpan_tap_send eth packet size:{} is exceeded limit!", len);
      return -1;
    }
    memcpy(packet + sizeof(tETH_HDR), buf, len);

    /* Send data to network interface */
    ssize_t ret;
    OSI_NO_INTR(ret = write(tap_fd, packet, len + sizeof(tETH_HDR)));
    log::verbose("ret:{}", ret);
    return (int)ret;
  }
  return -1;
}

int btpan_tap_close(int fd) {
  if (tap_if_down(TAP_IF_NAME) == 0) {
    close(fd);
  }
  if (pan_pth >= 0) {
    btsock_thread_wakeup(pan_pth);
  }
  return 0;
}

btpan_conn_t* btpan_find_conn_handle(uint16_t handle) {
  for (int i = 0; i < MAX_PAN_CONNS; i++) {
    if (btpan_cb.conns[i].handle == handle) {
      return &btpan_cb.conns[i];
    }
  }
  return NULL;
}

btpan_conn_t* btpan_find_conn_addr(const RawAddress& addr) {
  for (int i = 0; i < MAX_PAN_CONNS; i++) {
    if (btpan_cb.conns[i].peer == addr) {
      return &btpan_cb.conns[i];
    }
  }
  return NULL;
}

static void btpan_open_conn(btpan_conn_t* conn, tBTA_PAN* p_data) {
  log::verbose("btpan_open_conn: local_role:{}, peer_role: {},  handle:{}, conn: {}",
               p_data->open.local_role, p_data->open.peer_role, p_data->open.handle,
               std::format_ptr(conn));

  if (conn == NULL) {
    conn = btpan_new_conn(p_data->open.handle, p_data->open.bd_addr, p_data->open.local_role,
                          p_data->open.peer_role);
  }
  if (conn) {
    log::verbose(
            "btpan_open_conn:tap_fd:{}, open_count:{}, conn->handle:{} should = "
            "handle:{}, local_role:{}, remote_role:{}",
            btpan_cb.tap_fd, btpan_cb.open_count, conn->handle, p_data->open.handle,
            conn->local_role, conn->remote_role);

    btpan_cb.open_count++;
    conn->handle = p_data->open.handle;
    if (btpan_cb.tap_fd < 0) {
      btpan_cb.tap_fd = btpan_tap_open();
      if (btpan_cb.tap_fd >= 0) {
        create_tap_read_thread(btpan_cb.tap_fd);
      }
    }

    if (btpan_cb.tap_fd >= 0) {
      btpan_cb.flow = 1;
      conn->state = PAN_STATE_OPEN;
    }
  }
}

static void btpan_close_conn(btpan_conn_t* conn) {
  log::verbose("btpan_close_conn: {}", std::format_ptr(conn));

  if (conn && conn->state == PAN_STATE_OPEN) {
    log::verbose("btpan_close_conn: PAN_STATE_OPEN");

    conn->state = PAN_STATE_CLOSE;
    btpan_cb.open_count--;

    if (btpan_cb.open_count == 0) {
      destroy_tap_read_thread();
      if (btpan_cb.tap_fd != INVALID_FD) {
        btpan_tap_close(btpan_cb.tap_fd);
        btpan_cb.tap_fd = INVALID_FD;
      }
    }
  }
}

static void btpan_cleanup_conn(btpan_conn_t* conn) {
  if (conn) {
    conn->handle = -1;
    conn->state = -1;
    memset(&conn->peer, 0, sizeof(conn->peer));
    memset(&conn->eth_addr, 0, sizeof(conn->eth_addr));
    conn->local_role = conn->remote_role = 0;
  }
}

btpan_conn_t* btpan_new_conn(int handle, const RawAddress& addr, tBTA_PAN_ROLE local_role,
                             tBTA_PAN_ROLE remote_role) {
  for (int i = 0; i < MAX_PAN_CONNS; i++) {
    if (btpan_cb.conns[i].handle == -1) {
      log::debug("Allocated new pan connection handle:{} local_role:{} remote_role:{}", handle,
                 local_role, remote_role);
      btpan_cb.conns[i].handle = handle;
      btpan_cb.conns[i].peer = addr;
      btpan_cb.conns[i].local_role = local_role;
      btpan_cb.conns[i].remote_role = remote_role;
      return &btpan_cb.conns[i];
    }
  }
  log::warn("Unable to create new pan connection max:{}", MAX_PAN_CONNS);
  return nullptr;
}

static inline bool should_forward(tETH_HDR* hdr) {
  uint16_t proto = ntohs(hdr->h_proto);
  if (proto == ETH_P_IP || proto == ETH_P_ARP || proto == ETH_P_IPV6) {
    return true;
  }
  log::verbose("unknown proto:{:x}", proto);
  return false;
}

static int forward_bnep(tETH_HDR* eth_hdr, BT_HDR* hdr) {
  int broadcast = eth_hdr->h_dest.address[0] & 1;

  // Find the right connection to send this frame over.
  for (int i = 0; i < MAX_PAN_CONNS; i++) {
    uint16_t handle = btpan_cb.conns[i].handle;
    if (handle != (uint16_t)-1 && (broadcast || btpan_cb.conns[i].eth_addr == eth_hdr->h_dest ||
                                   btpan_cb.conns[i].peer == eth_hdr->h_dest)) {
      int result = PAN_WriteBuf(handle, eth_hdr->h_dest, eth_hdr->h_src, ntohs(eth_hdr->h_proto),
                                hdr, 0);
      switch (result) {
        case PAN_Q_SIZE_EXCEEDED:
          return FORWARD_CONGEST;
        case PAN_SUCCESS:
          return FORWARD_SUCCESS;
        default:
          return FORWARD_FAILURE;
      }
    }
  }
  osi_free(hdr);
  return FORWARD_IGNORE;
}

static void bta_pan_callback_transfer(uint16_t event, char* p_param) {
  tBTA_PAN* p_data = (tBTA_PAN*)p_param;

  switch (event) {
    case BTA_PAN_ENABLE_EVT:
      log::verbose("BTA_PAN_ENABLE_EVT");
      break;
    case BTA_PAN_SET_ROLE_EVT: {
      int btpan_role = bta_role_to_btpan(p_data->set_role.role);
      bt_status_t status = (p_data->set_role.status) ? BT_STATUS_SUCCESS : BT_STATUS_FAIL;
      btpan_control_state_t state = btpan_role == 0 ? BTPAN_STATE_DISABLED : BTPAN_STATE_ENABLED;
      callback.control_state_cb(state, btpan_role, status, TAP_IF_NAME);
      break;
    }
    case BTA_PAN_OPENING_EVT: {
      btpan_conn_t* conn;
      log::verbose("BTA_PAN_OPENING_EVT handle {}, addr: {}", p_data->opening.handle,
                   p_data->opening.bd_addr);
      conn = btpan_find_conn_addr(p_data->opening.bd_addr);

      asrt(conn != NULL);
      if (conn) {
        conn->handle = p_data->opening.handle;
        int btpan_conn_local_role = bta_role_to_btpan(conn->local_role);
        int btpan_remote_role = bta_role_to_btpan(conn->remote_role);
        callback.connection_state_cb(BTPAN_STATE_CONNECTING, BT_STATUS_SUCCESS,
                                     &p_data->opening.bd_addr, btpan_conn_local_role,
                                     btpan_remote_role);
      } else {
        log::error("connection not found");
      }
      break;
    }
    case BTA_PAN_OPEN_EVT: {
      btpan_connection_state_t state;
      bt_status_t status;
      btpan_conn_t* conn = btpan_find_conn_handle(p_data->open.handle);

      log::verbose("pan connection open status: {}", p_data->open.status);
      if (p_data->open.status) {
        state = BTPAN_STATE_CONNECTED;
        status = BT_STATUS_SUCCESS;
        btpan_open_conn(conn, p_data);
      } else {
        state = BTPAN_STATE_DISCONNECTED;
        status = BT_STATUS_FAIL;
        btpan_cleanup_conn(conn);
      }
      /* debug("BTA_PAN_OPEN_EVT handle:%d, conn:%p",  p_data->open.handle,
       * conn); */
      /* debug("conn bta local_role:%d, bta remote role:%d", conn->local_role,
       * conn->remote_role); */
      int btpan_conn_local_role = bta_role_to_btpan(p_data->open.local_role);
      int btpan_remote_role = bta_role_to_btpan(p_data->open.peer_role);
      callback.connection_state_cb(state, status, &p_data->open.bd_addr, btpan_conn_local_role,
                                   btpan_remote_role);
      break;
    }
    case BTA_PAN_CLOSE_EVT: {
      log::info("event = BTA_PAN_CLOSE_EVT handle {}", p_data->close.handle);
      btpan_conn_t* conn = btpan_find_conn_handle(p_data->close.handle);
      btpan_close_conn(conn);

      if (conn && conn->handle >= 0) {
        int btpan_conn_local_role = bta_role_to_btpan(conn->local_role);
        int btpan_remote_role = bta_role_to_btpan(conn->remote_role);
        callback.connection_state_cb(BTPAN_STATE_DISCONNECTED, (bt_status_t)0, &conn->peer,
                                     btpan_conn_local_role, btpan_remote_role);
        btpan_cleanup_conn(conn);
      } else {
        log::error("pan handle not found ({})", p_data->close.handle);
      }
      break;
    }
    default:
      log::warn("Unknown pan event {}", event);
      break;
  }
}

static void bta_pan_callback(tBTA_PAN_EVT event, tBTA_PAN* p_data) {
  btif_transfer_context(bta_pan_callback_transfer, event, (char*)p_data, sizeof(tBTA_PAN), NULL);
}

#define IS_EXCEPTION(e) ((e) & (POLLHUP | POLLRDHUP | POLLERR | POLLNVAL))
static void btu_exec_tap_fd_read(int fd) {
  struct pollfd ufd;

  if (fd == INVALID_FD || fd != btpan_cb.tap_fd) {
    return;
  }

  // Don't occupy BTU context too long, avoid buffer overruns and
  // give other profiles a chance to run by limiting the amount of memory
  // PAN can use.
  for (int i = 0; i < PAN_BUF_MAX && btif_is_enabled() && btpan_cb.flow; i++) {
    BT_HDR* buffer = (BT_HDR*)osi_malloc(PAN_BUF_SIZE);
    buffer->offset = PAN_MINIMUM_OFFSET;
    buffer->len = PAN_BUF_SIZE - sizeof(BT_HDR) - buffer->offset;

    uint8_t* packet = (uint8_t*)buffer + sizeof(BT_HDR) + buffer->offset;

    // If we don't have an undelivered packet left over, pull one from the TAP
    // driver.
    // We save it in the congest_packet right away in case we can't deliver it
    // in this
    // attempt.
    if (!btpan_cb.congest_packet_size) {
      ssize_t ret;
      OSI_NO_INTR(ret = read(fd, btpan_cb.congest_packet, sizeof(btpan_cb.congest_packet)));
      switch (ret) {
        case -1:
          log::error("unable to read from driver: {}", strerror(errno));
          osi_free(buffer);
          // add fd back to monitor thread to try it again later
          btsock_thread_add_fd(pan_pth, fd, 0, SOCK_THREAD_FD_RD, 0);
          return;
        case 0:
          log::warn("end of file reached.");
          osi_free(buffer);
          // add fd back to monitor thread to process the exception
          btsock_thread_add_fd(pan_pth, fd, 0, SOCK_THREAD_FD_RD, 0);
          return;
        default:
          btpan_cb.congest_packet_size = ret;
          break;
      }
    }

    memcpy(packet, btpan_cb.congest_packet, MIN(btpan_cb.congest_packet_size, buffer->len));
    buffer->len = MIN(btpan_cb.congest_packet_size, buffer->len);

    if (buffer->len > sizeof(tETH_HDR) && should_forward((tETH_HDR*)packet)) {
      // Extract the ethernet header from the buffer since the PAN_WriteBuf
      // inside
      // forward_bnep can't handle two pointers that point inside the same GKI
      // buffer.
      tETH_HDR hdr;
      memcpy(&hdr, packet, sizeof(tETH_HDR));

      // Skip the ethernet header.
      buffer->len -= sizeof(tETH_HDR);
      buffer->offset += sizeof(tETH_HDR);
      if (forward_bnep(&hdr, buffer) != FORWARD_CONGEST) {
        btpan_cb.congest_packet_size = 0;
      }
    } else {
      log::warn("dropping packet of length {}", buffer->len);
      btpan_cb.congest_packet_size = 0;
      osi_free(buffer);
    }

    // Bail out of the loop if reading from the TAP fd would block.
    ufd.fd = fd;
    ufd.events = POLLIN;
    ufd.revents = 0;

    int ret;
    OSI_NO_INTR(ret = poll(&ufd, 1, 0));
    if (ret <= 0 || IS_EXCEPTION(ufd.revents)) {
      break;
    }
  }

  if (btpan_cb.flow) {
    // add fd back to monitor thread when the flow is on
    btsock_thread_add_fd(pan_pth, fd, 0, SOCK_THREAD_FD_RD, 0);
  }
}

static void btif_pan_close_all_conns() {
  if (!stack_initialized) {
    return;
  }

  for (int i = 0; i < MAX_PAN_CONNS; ++i) {
    if (btpan_cb.conns[i].handle != -1) {
      BTA_PanClose(btpan_cb.conns[i].handle);
    }
  }
}

static void btpan_tap_fd_signaled(int fd, int /*type*/, int flags, uint32_t /*user_id*/) {
  log::assert_that(btpan_cb.tap_fd == INVALID_FD || btpan_cb.tap_fd == fd,
                   "assert failed: btpan_cb.tap_fd == INVALID_FD || btpan_cb.tap_fd == fd");

  if (btpan_cb.tap_fd != fd) {
    log::warn("Signaled on mismatched fds exp:{} act:{}", btpan_cb.tap_fd, fd);
    return;
  }

  if (flags & SOCK_THREAD_FD_EXCEPTION) {
    btpan_cb.tap_fd = INVALID_FD;
    btpan_tap_close(fd);
    btif_pan_close_all_conns();
  } else if (flags & SOCK_THREAD_FD_RD) {
    do_in_main_thread(base::BindOnce(btu_exec_tap_fd_read, fd));
  }
}
