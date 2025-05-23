/******************************************************************************
 *
 *  Copyright 2018 The Android Open Source Project
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

#include "connection_manager.h"

#include <base/functional/bind.h>
#include <base/functional/callback.h>
#include <base/location.h>
#include <bluetooth/log.h>

#include <map>
#include <memory>
#include <set>

#include "main/shim/acl_api.h"
#include "main/shim/le_scanning_manager.h"
#include "os/logging/log_adapter.h"
#include "osi/include/alarm.h"
#include "stack/btm/btm_dev.h"
#include "stack/include/advertise_data_parser.h"
#include "stack/include/bt_types.h"
#include "stack/include/btm_ble_api.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/btm_log_history.h"
#include "stack/include/main_thread.h"
#include "types/raw_address.h"

#define DIRECT_CONNECT_TIMEOUT (30 * 1000) /* 30 seconds */

using namespace bluetooth;

constexpr char kBtmLogTag[] = "TA";

struct closure_data {
  base::OnceClosure user_task;
  base::Location posted_from;
};

static void alarm_closure_cb(void* p) {
  closure_data* data = (closure_data*)p;
  log::verbose("executing timer scheduled at {}", data->posted_from.ToString());
  std::move(data->user_task).Run();
  delete data;
}

// Periodic alarms are not supported, because we clean up data in callback
static void alarm_set_closure(const base::Location& posted_from, alarm_t* alarm,
                              uint64_t interval_ms, base::OnceClosure user_task) {
  closure_data* data = new closure_data;
  data->posted_from = posted_from;
  data->user_task = std::move(user_task);
  log::verbose("scheduling timer {}", data->posted_from.ToString());
  alarm_set_on_mloop(alarm, interval_ms, alarm_closure_cb, data);
}

using unique_alarm_ptr = std::unique_ptr<alarm_t, decltype(&alarm_free)>;

namespace connection_manager {

struct tAPPS_CONNECTING {
  // ids of clients doing background connection to given device
  std::set<tAPP_ID> doing_bg_conn;
  std::set<tAPP_ID> doing_targeted_announcements_conn;
  bool is_in_accept_list;

  // Apps trying to do direct connection.
  std::map<tAPP_ID, unique_alarm_ptr> doing_direct_conn;
};

namespace {
// Maps address to apps trying to connect to it
std::map<RawAddress, tAPPS_CONNECTING> bgconn_dev;

int num_of_targeted_announcements_users(void) {
  return std::count_if(bgconn_dev.begin(), bgconn_dev.end(), [](const auto& pair) {
    return !pair.second.is_in_accept_list && !pair.second.doing_targeted_announcements_conn.empty();
  });
}

bool is_anyone_interested_to_use_accept_list(
        const std::map<RawAddress, tAPPS_CONNECTING>::iterator it) {
  if (!it->second.doing_targeted_announcements_conn.empty()) {
    return !it->second.doing_direct_conn.empty();
  }
  return !it->second.doing_bg_conn.empty() || !it->second.doing_direct_conn.empty();
}

bool is_anyone_connecting(const std::map<RawAddress, tAPPS_CONNECTING>::iterator it) {
  return !it->second.doing_bg_conn.empty() || !it->second.doing_direct_conn.empty() ||
         !it->second.doing_targeted_announcements_conn.empty();
}

}  // namespace

/** background connection device from the list. Returns pointer to the device
 * record, or nullptr if not found */
std::set<tAPP_ID> get_apps_connecting_to(const RawAddress& address) {
  log::debug("address={}", address);
  auto it = bgconn_dev.find(address);
  return (it != bgconn_dev.end()) ? it->second.doing_bg_conn : std::set<tAPP_ID>();
}

static bool IsTargetedAnnouncement(const uint8_t* p_eir, uint16_t eir_len) {
  const uint8_t* p_service_data = p_eir;
  uint8_t service_data_len = 0;

  while ((p_service_data = AdvertiseDataParser::GetFieldByType(
                  p_service_data + service_data_len,
                  eir_len - (p_service_data - p_eir) - service_data_len,
                  BTM_BLE_AD_TYPE_SERVICE_DATA_TYPE, &service_data_len))) {
    uint16_t uuid;
    uint8_t announcement_type;
    const uint8_t* p_tmp = p_service_data;

    if (service_data_len < 3) {
      continue;
    }

    STREAM_TO_UINT16(uuid, p_tmp);
    log::debug("Found UUID 0x{:04x}", uuid);

    if (uuid != 0x184E && uuid != 0x1853) {
      continue;
    }

    STREAM_TO_UINT8(announcement_type, p_tmp);
    log::debug("Found announcement_type 0x{:02x}", announcement_type);
    if (announcement_type == 0x01) {
      return true;
    }
  }
  return false;
}

static void schedule_direct_connect_add(uint8_t app_id, const RawAddress& address);

static void target_announcement_observe_results_cb(tBTM_INQ_RESULTS* p_inq, const uint8_t* p_eir,
                                                   uint16_t eir_len) {
  auto addr = p_inq->remote_bd_addr;
  auto it = bgconn_dev.find(addr);
  if (it == bgconn_dev.end() || it->second.doing_targeted_announcements_conn.empty()) {
    return;
  }

  if (!IsTargetedAnnouncement(p_eir, eir_len)) {
    log::debug("Not a targeted announcement for device {}", addr);
    return;
  }

  log::info("Found targeted announcement for device {}", addr);

  if (it->second.is_in_accept_list) {
    log::info("Device {} is already connecting", addr);
    return;
  }

  if (get_btm_client_interface().peer.BTM_GetHCIConnHandle(addr, BT_TRANSPORT_LE) != 0xFFFF) {
    log::debug("Device {} already connected", addr);
    return;
  }

  BTM_LogHistory(kBtmLogTag, addr, "Found TA from");

  /* Take fist app_id and use it for direct_connect */
  auto app_id = *(it->second.doing_targeted_announcements_conn.begin());

  /* If scan is ongoing lets stop it */
  do_in_main_thread(base::BindOnce(schedule_direct_connect_add, app_id, addr));
}

static void target_announcements_filtering_set(bool enable) {
  log::debug("enable {}", enable);
  BTM_LogHistory(kBtmLogTag, RawAddress::kEmpty, (enable ? "Start filtering" : "Stop filtering"));

  /* Safe to call as if there is no support for filtering, this call will be
   * ignored. */
  bluetooth::shim::set_target_announcements_filter(enable);
  BTM_BleTargetAnnouncementObserve(enable, target_announcement_observe_results_cb);
}

/** Add a device to the background connection list for targeted announcements.
 * Returns
 *   true if device added to the list, or already in list,
 *   false otherwise
 */
bool background_connect_targeted_announcement_add(tAPP_ID app_id, const RawAddress& address) {
  log::info("app_id={}, address={}", static_cast<int>(app_id), address);

  bool disable_accept_list = false;

  auto it = bgconn_dev.find(address);
  if (it != bgconn_dev.end()) {
    // check if filtering already enabled
    if (it->second.doing_targeted_announcements_conn.count(app_id)) {
      log::info("app_id={}, already doing targeted announcement filtering to address={}",
                static_cast<int>(app_id), address);
      return true;
    }

    bool targeted_filtering_enabled = !it->second.doing_targeted_announcements_conn.empty();

    // Check if connecting
    if (!it->second.doing_direct_conn.empty()) {
      log::info("app_id={}, address={}, already in direct connection", static_cast<int>(app_id),
                address);

    } else if (!targeted_filtering_enabled && !it->second.doing_bg_conn.empty()) {
      // device is already in the acceptlist so we would have to remove it
      log::info("already doing background connection to address={}. Need to disable it.", address);
      disable_accept_list = true;
    }
  }

  if (disable_accept_list) {
    bluetooth::shim::ACL_IgnoreLeConnectionFrom(BTM_Sec_GetAddressWithType(address));
    bgconn_dev[address].is_in_accept_list = false;
  }

  bgconn_dev[address].doing_targeted_announcements_conn.insert(app_id);
  if (bgconn_dev[address].doing_targeted_announcements_conn.size() == 1) {
    BTM_LogHistory(kBtmLogTag, address, "Allow connection from");
  }

  if (num_of_targeted_announcements_users() == 1) {
    target_announcements_filtering_set(true);
  }

  return true;
}

/** Add a device from the background connection list.  Returns true if device
 * added to the list, or already in list, false otherwise */
bool background_connect_add(uint8_t app_id, const RawAddress& address) {
  log::debug("app_id={}, address={}", static_cast<int>(app_id), address);
  auto it = bgconn_dev.find(address);
  bool in_acceptlist = false;
  bool is_targeted_announcement_enabled = false;
  if (it != bgconn_dev.end()) {
    // device already in the acceptlist, just add interested app to the list
    if (it->second.doing_bg_conn.count(app_id)) {
      log::debug("app_id={}, already doing background connection to address={}",
                 static_cast<int>(app_id), address);
      return true;
    }

    // Already in acceptlist ?
    if (it->second.is_in_accept_list) {
      log::debug("app_id={}, address={}, already in accept list", static_cast<int>(app_id),
                 address);
      in_acceptlist = true;
    } else {
      is_targeted_announcement_enabled = !it->second.doing_targeted_announcements_conn.empty();
    }
  }

  if (!in_acceptlist) {
    // the device is not in the acceptlist
    if (is_targeted_announcement_enabled) {
      log::debug("Targeted announcement enabled, do not add to AcceptList");
    } else {
      if (!bluetooth::shim::ACL_AcceptLeConnectionFrom(BTM_Sec_GetAddressWithType(address),
                                                       false)) {
        log::warn("Failed to add device {} to accept list for app {}", address,
                  static_cast<int>(app_id));
        return false;
      }
      bgconn_dev[address].is_in_accept_list = true;
    }
  }

  // create entry for address, and insert app_id.
  // new tAPPS_CONNECTING will be default constructed if not exist
  bgconn_dev[address].doing_bg_conn.insert(app_id);
  return true;
}

/** Removes all registrations for connection for given device.
 * Returns true if anything was removed, false otherwise */
bool remove_unconditional(const RawAddress& address) {
  log::debug("address={}", address);
  auto it = bgconn_dev.find(address);
  if (it == bgconn_dev.end()) {
    log::warn("address {} is not found", address);
    return false;
  }

  bluetooth::shim::ACL_IgnoreLeConnectionFrom(BTM_Sec_GetAddressWithType(address));
  bgconn_dev.erase(it);
  return true;
}

/** Remove device from the background connection device list or listening to
 * advertising list.  Returns true if device was on the list and was
 * successfully removed */
bool background_connect_remove(uint8_t app_id, const RawAddress& address) {
  log::debug("app_id={}, address={}", static_cast<int>(app_id), address);
  auto it = bgconn_dev.find(address);
  if (it == bgconn_dev.end()) {
    log::warn("address {} is not found", address);
    return false;
  }

  bool accept_list_enabled = it->second.is_in_accept_list;
  auto num_of_targeted_announcements_before_remove =
          it->second.doing_targeted_announcements_conn.size();

  bool removed_from_bg_conn = (it->second.doing_bg_conn.erase(app_id) > 0);
  bool removed_from_ta = (it->second.doing_targeted_announcements_conn.erase(app_id) > 0);
  if (!removed_from_bg_conn && !removed_from_ta) {
    log::warn("Failed to remove background connection app {} for address {}",
              static_cast<int>(app_id), address);
    return false;
  }

  if (removed_from_ta && it->second.doing_targeted_announcements_conn.size() == 0) {
    BTM_LogHistory(kBtmLogTag, address, "Ignore connection from");
  }

  if (is_anyone_connecting(it)) {
    log::debug("some device is still connecting, app_id={}, address={}", static_cast<int>(app_id),
               address);
    /* Check which method should be used now.*/
    if (!accept_list_enabled) {
      /* Accept list was not used */
      if (!it->second.doing_targeted_announcements_conn.empty()) {
        /* Keep using filtering */
        log::debug("Keep using target announcement filtering");
      } else if (!it->second.doing_bg_conn.empty()) {
        if (!bluetooth::shim::ACL_AcceptLeConnectionFrom(BTM_Sec_GetAddressWithType(address),
                                                         false)) {
          log::warn("Could not re add device to accept list");
        } else {
          bgconn_dev[address].is_in_accept_list = true;
        }
      }
    }
    return true;
  }

  bgconn_dev.erase(it);

  // no more apps interested - remove from accept list and delete record
  if (accept_list_enabled) {
    bluetooth::shim::ACL_IgnoreLeConnectionFrom(BTM_Sec_GetAddressWithType(address));
    return true;
  }

  if ((num_of_targeted_announcements_before_remove > 0) &&
      num_of_targeted_announcements_users() == 0) {
    target_announcements_filtering_set(true);
  }

  return true;
}

bool is_background_connection(const RawAddress& address) {
  auto it = bgconn_dev.find(address);
  if (it == bgconn_dev.end()) {
    return false;
  }
  return it->second.is_in_accept_list;
}

/** deregister all related background connetion device. */
void on_app_deregistered(uint8_t app_id) {
  log::debug("app_id={}", static_cast<int>(app_id));
  auto it = bgconn_dev.begin();
  auto end = bgconn_dev.end();
  /* update the BG conn device list */
  while (it != end) {
    it->second.doing_bg_conn.erase(app_id);

    it->second.doing_direct_conn.erase(app_id);

    if (is_anyone_connecting(it)) {
      it++;
      continue;
    }

    bluetooth::shim::ACL_IgnoreLeConnectionFrom(BTM_Sec_GetAddressWithType(it->first));
    it = bgconn_dev.erase(it);
  }
}

static void remove_all_clients_with_pending_connections(const RawAddress& address) {
  log::debug("address={}", address);
  auto it = bgconn_dev.find(address);
  while (it != bgconn_dev.end() && !it->second.doing_direct_conn.empty()) {
    uint8_t app_id = it->second.doing_direct_conn.begin()->first;
    direct_connect_remove(app_id, address);
    it = bgconn_dev.find(address);
  }
}

void on_connection_complete(const RawAddress& address) {
  log::info("Le connection completed to device:{}", address);

  remove_all_clients_with_pending_connections(address);
}

void on_connection_timed_out_from_shim(const RawAddress& address) {
  log::info("Connection failed {}", address);
  on_connection_timed_out(0x00, address);
}

/** Reset bg device list. If called after controller reset, set |after_reset|
 * to true, as there is no need to wipe controller acceptlist in this case. */
void reset(bool after_reset) {
  bgconn_dev.clear();
  if (!after_reset) {
    target_announcements_filtering_set(false);
    bluetooth::shim::ACL_IgnoreAllLeConnections();
  }
}

static void wl_direct_connect_timeout_cb(uint8_t app_id, const RawAddress& address) {
  log::debug("app_id={}, address={}", static_cast<int>(app_id), address);
  on_connection_timed_out(app_id, address);

  // TODO: this would free the timer, from within the timer callback, which is
  // bad.
  direct_connect_remove(app_id, address, true);
}

static void find_in_device_record(const RawAddress& bd_addr, tBLE_BD_ADDR* address_with_type) {
  const tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  if (p_dev_rec == nullptr) {
    return;
  }

  if (p_dev_rec->device_type & BT_DEVICE_TYPE_BLE) {
    if (p_dev_rec->ble.identity_address_with_type.bda.IsEmpty()) {
      *address_with_type = {.type = p_dev_rec->ble.AddressType(), .bda = bd_addr};
      return;
    }
    *address_with_type = p_dev_rec->ble.identity_address_with_type;
    return;
  }
  *address_with_type = {.type = BLE_ADDR_PUBLIC, .bda = bd_addr};
  return;
}

bool create_le_connection(uint8_t /* id */, const RawAddress& bd_addr, tBLE_ADDR_TYPE addr_type) {
  tBLE_BD_ADDR address_with_type{
          .type = addr_type,
          .bda = bd_addr,
  };

  find_in_device_record(bd_addr, &address_with_type);

  log::debug("Creating le direct connection to:{} type:{} (initial type: {})", address_with_type,
             AddressTypeText(address_with_type.type), AddressTypeText(addr_type));

  if (address_with_type.type == BLE_ADDR_ANONYMOUS) {
    log::warn(
            "Creating le direct connection to:{}, address type 'anonymous' is "
            "invalid",
            address_with_type);
    return false;
  }

  bluetooth::shim::ACL_AcceptLeConnectionFrom(address_with_type,
                                              /* is_direct */ true);
  return true;
}

/** Add a device to the direct connection list. Returns true if device
 * added to the list, false otherwise */
bool direct_connect_add(uint8_t app_id, const RawAddress& address) {
  log::debug("app_id={}, address={}", static_cast<int>(app_id), address);
  bool in_acceptlist = false;
  auto it = bgconn_dev.find(address);
  if (it != bgconn_dev.end()) {
    // app already trying to connect to this particular device
    if (it->second.doing_direct_conn.count(app_id)) {
      log::info("direct connect attempt from app_id=0x{:x} already in progress", app_id);
      return false;
    }

    // are we already in the acceptlist ?
    if (it->second.is_in_accept_list) {
      log::warn("Background connection attempt already in progress app_id={:x}", app_id);
      in_acceptlist = true;
    }
  }

  if (!in_acceptlist) {
    if (!bluetooth::shim::ACL_AcceptLeConnectionFrom(BTM_Sec_GetAddressWithType(address), true)) {
      // if we can't add to acceptlist, turn parameters back to slow.
      log::warn("Unable to add le device to acceptlist");
      return false;
    }
    bgconn_dev[address].is_in_accept_list = true;
  }

  // Setup a timer
  alarm_t* timeout = alarm_new("wl_conn_params_30s");
  alarm_set_closure(FROM_HERE, timeout, DIRECT_CONNECT_TIMEOUT,
                    base::BindOnce(&wl_direct_connect_timeout_cb, app_id, address));

  bgconn_dev[address].doing_direct_conn.emplace(app_id, unique_alarm_ptr(timeout, &alarm_free));

  return true;
}

static void schedule_direct_connect_add(uint8_t app_id, const RawAddress& address) {
  direct_connect_add(app_id, address);
}

bool direct_connect_remove(uint8_t app_id, const RawAddress& address, bool connection_timeout) {
  log::debug("app_id={}, address={}", static_cast<int>(app_id), address);
  auto it = bgconn_dev.find(address);
  if (it == bgconn_dev.end()) {
    log::warn("Unable to find background connection to remove peer:{}", address);
    return false;
  }

  auto app_it = it->second.doing_direct_conn.find(app_id);
  if (app_it == it->second.doing_direct_conn.end()) {
    log::warn("Unable to find direct connection to remove peer:{}", address);
    return false;
  }

  /* Let see if the device was connected due to Target Announcements.*/
  bool is_targeted_announcement_enabled = !it->second.doing_targeted_announcements_conn.empty();

  // this will free the alarm
  it->second.doing_direct_conn.erase(app_it);

  if (is_anyone_interested_to_use_accept_list(it)) {
    if (connection_timeout) {
      /* In such case we need to add device back to allow list because,
       * when connection timeout out, the lower layer removes device from
       * the allow list.
       */
      if (!bluetooth::shim::ACL_AcceptLeConnectionFrom(BTM_Sec_GetAddressWithType(address),
                                                       false)) {
        log::warn("Failed to re-add device {} to accept list after connection timeout", address);
      }
    }
    return true;
  }

  // no more apps interested - remove from acceptlist
  bluetooth::shim::ACL_IgnoreLeConnectionFrom(BTM_Sec_GetAddressWithType(address));

  if (!is_targeted_announcement_enabled) {
    bgconn_dev.erase(it);
  } else {
    it->second.is_in_accept_list = false;
  }

  return true;
}

void dump(int fd) {
  dprintf(fd, "\nconnection_manager state:\n");
  if (bgconn_dev.empty()) {
    dprintf(fd, "\tno Low Energy connection attempts\n");
    return;
  }

  dprintf(fd, "\tdevices attempting connection: %d", (int)bgconn_dev.size());
  for (const auto& entry : bgconn_dev) {
    // TODO: confirm whether we need to replace this
    dprintf(fd, "\n\t * %s:\t\tin_accept_list: %s\t cap_targeted_announcements: %s",
            ADDRESS_TO_LOGGABLE_CSTR(entry.first),
            entry.second.is_in_accept_list ? "true" : "false",
            entry.second.doing_targeted_announcements_conn.empty() ? "false" : "true");

    if (!entry.second.doing_direct_conn.empty()) {
      dprintf(fd, "\n\t\tapps doing direct connect: ");
      for (const auto& id : entry.second.doing_direct_conn) {
        dprintf(fd, "%d, ", id.first);
      }
    }

    if (!entry.second.doing_bg_conn.empty()) {
      dprintf(fd, "\n\t\tapps doing background connect: ");
      for (const auto& id : entry.second.doing_bg_conn) {
        dprintf(fd, "%d, ", id);
      }
    }
  }
  dprintf(fd, "\n");
}

}  // namespace connection_manager
