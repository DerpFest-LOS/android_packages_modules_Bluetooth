/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "hci/le_advertising_manager.h"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <iterator>
#include <memory>
#include <mutex>

#include "common/strings.h"
#include "hardware/ble_advertiser.h"
#include "hci/acl_manager.h"
#include "hci/controller.h"
#include "hci/event_checkers.h"
#include "hci/hci_layer.h"
#include "hci/hci_packets.h"
#include "hci/le_advertising_interface.h"
#include "module.h"
#include "os/handler.h"
#include "os/system_properties.h"
#include "packet/fragmenting_inserter.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

namespace bluetooth {
namespace hci {

const ModuleFactory LeAdvertisingManager::Factory =
        ModuleFactory([]() { return new LeAdvertisingManager(); });
constexpr int kIdLocal = 0xff;  // Id for advertiser not register from Java layer
constexpr uint16_t kLenOfFlags = 0x03;
constexpr int64_t kLeAdvertisingTxPowerMin = -127;
constexpr int64_t kLeAdvertisingTxPowerMax = 20;
constexpr int64_t kLeTxPathLossCompMin = -128;
constexpr int64_t kLeTxPathLossCompMax = 127;

// system properties
const std::string kLeTxPathLossCompProperty = "bluetooth.hardware.radio.le_tx_path_loss_comp_db";

enum class AdvertisingApiType {
  LEGACY = 1,
  ANDROID_HCI = 2,
  EXTENDED = 3,
};

enum class AdvertisingFlag : uint8_t {
  LE_LIMITED_DISCOVERABLE = 0x01,
  LE_GENERAL_DISCOVERABLE = 0x02,
  BR_EDR_NOT_SUPPORTED = 0x04,
  SIMULTANEOUS_LE_AND_BR_EDR_CONTROLLER = 0x08,
  SIMULTANEOUS_LE_AND_BR_EDR_HOST = 0x10,
};

struct Advertiser {
  os::Handler* handler;
  AddressWithType current_address;
  // note: may not be the same as the requested_address_type, depending on the address policy
  AdvertiserAddressType address_type;
  base::OnceCallback<void(uint8_t /* status */)> status_callback;
  base::OnceCallback<void(uint8_t /* status */)> timeout_callback;
  common::Callback<void(Address, AddressType)> scan_callback;
  common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback;
  int8_t tx_power;
  uint16_t duration;
  uint8_t max_extended_advertising_events;
  bool started = false;
  bool is_legacy = false;
  bool connectable = false;
  bool discoverable = false;
  bool directed = false;
  bool in_use = false;
  bool is_periodic = false;
  std::unique_ptr<os::Alarm> address_rotation_wake_alarm_;
  std::unique_ptr<os::Alarm> address_rotation_non_wake_alarm_;

  // Only used for logging error in address rotation time.
  std::optional<std::chrono::time_point<std::chrono::system_clock>> address_rotation_interval_min;
  std::optional<std::chrono::time_point<std::chrono::system_clock>> address_rotation_interval_max;
};

/**
 * Determines the address type to use, based on the requested type and the address manager policy,
 * by selecting the "strictest" of the two. Strictness is defined in ascending order as
 * RPA -> NRPA -> Public. Thus:
 * (1) if the host only supports the public/static address policy, all advertisements will be public
 * (2) if the host supports only non-resolvable addresses, then advertisements will never use RPA
 * (3) if the host supports RPAs, then the requested type will always be honored
 */
AdvertiserAddressType GetAdvertiserAddressTypeFromRequestedTypeAndPolicy(
        AdvertiserAddressType requested_address_type,
        LeAddressManager::AddressPolicy address_policy) {
  switch (address_policy) {
    case LeAddressManager::AddressPolicy::USE_PUBLIC_ADDRESS:
    case LeAddressManager::AddressPolicy::USE_STATIC_ADDRESS:
      return AdvertiserAddressType::PUBLIC;
    case LeAddressManager::AddressPolicy::USE_RESOLVABLE_ADDRESS:
      return requested_address_type;
    case LeAddressManager::AddressPolicy::USE_NON_RESOLVABLE_ADDRESS:
      return requested_address_type == AdvertiserAddressType::RESOLVABLE_RANDOM
                     ? AdvertiserAddressType::NONRESOLVABLE_RANDOM
                     : requested_address_type;
    default:
      log::fatal("unreachable");
      return AdvertiserAddressType::PUBLIC;
  }
}

/**
 * Determines the address type to use for non-connectable advertisement.
 * (1) if the host only supports public/static address policy, non-connectable advertisement
 *     can use both Public and NRPA if requested. Use NRPA if RPA is requested.
 * (2) in other cases, based on the requested type and the address manager policy.
 */
AdvertiserAddressType GetAdvertiserAddressTypeNonConnectable(
        AdvertiserAddressType requested_address_type,
        LeAddressManager::AddressPolicy address_policy) {
  switch (address_policy) {
    case LeAddressManager::AddressPolicy::USE_PUBLIC_ADDRESS:
    case LeAddressManager::AddressPolicy::USE_STATIC_ADDRESS:
      return requested_address_type == AdvertiserAddressType::RESOLVABLE_RANDOM
                     ? AdvertiserAddressType::NONRESOLVABLE_RANDOM
                     : requested_address_type;
    default:
      return GetAdvertiserAddressTypeFromRequestedTypeAndPolicy(requested_address_type,
                                                                address_policy);
  }
}

struct LeAdvertisingManager::impl : public bluetooth::hci::LeAddressManagerCallback {
  impl(Module* module) : module_(module), le_advertising_interface_(nullptr), num_instances_(0) {}

  ~impl() {
    if (address_manager_registered) {
      le_address_manager_->Unregister(this);
    }
    advertising_sets_.clear();
  }

  void start(os::Handler* handler, hci::HciLayer* hci_layer, hci::Controller* controller,
             hci::AclManager* acl_manager) {
    module_handler_ = handler;
    hci_layer_ = hci_layer;
    controller_ = controller;
    le_maximum_advertising_data_length_ = controller_->GetLeMaximumAdvertisingDataLength();
    acl_manager_ = acl_manager;
    le_address_manager_ = acl_manager->GetLeAddressManager();
    num_instances_ = controller_->GetLeNumberOfSupportedAdverisingSets();

    le_advertising_interface_ = hci_layer_->GetLeAdvertisingInterface(
            module_handler_->BindOn(this, &LeAdvertisingManager::impl::handle_event));
    hci_layer_->RegisterVendorSpecificEventHandler(
            hci::VseSubeventCode::BLE_STCHANGE,
            handler->BindOn(this, &LeAdvertisingManager::impl::multi_advertising_state_change));

    if (controller_->SupportsBleExtendedAdvertising()) {
      advertising_api_type_ = AdvertisingApiType::EXTENDED;
    } else if (controller_->IsSupported(hci::OpCode::LE_MULTI_ADVT)) {
      advertising_api_type_ = AdvertisingApiType::ANDROID_HCI;
      num_instances_ = controller_->GetVendorCapabilities().max_advt_instances_;
      // number of LE_MULTI_ADVT start from 1
      num_instances_ += 1;
    } else {
      advertising_api_type_ = AdvertisingApiType::LEGACY;
      int vendor_version = os::GetAndroidVendorReleaseVersion();
      if (vendor_version != 0 && vendor_version <= 11 && os::IsRootCanalEnabled()) {
        log::info(
                "LeReadAdvertisingPhysicalChannelTxPower is not supported on Android R RootCanal, "
                "default to 0");
        le_physical_channel_tx_power_ = 0;
      } else {
        hci_layer_->EnqueueCommand(
                LeReadAdvertisingPhysicalChannelTxPowerBuilder::Create(),
                handler->BindOnceOn(this, &impl::on_read_advertising_physical_channel_tx_power));
      }
    }
    enabled_sets_ = std::vector<EnabledSet>(num_instances_);
    for (size_t i = 0; i < enabled_sets_.size(); i++) {
      enabled_sets_[i].advertising_handle_ = kInvalidHandle;
    }
    le_tx_path_loss_comp_ = get_tx_path_loss_compensation();
  }

  int8_t get_tx_path_loss_compensation() {
    int8_t compensation = 0;
    auto compensation_prop = os::GetSystemProperty(kLeTxPathLossCompProperty);
    if (compensation_prop) {
      auto compensation_number = common::Int64FromString(compensation_prop.value());
      if (compensation_number) {
        int64_t number = compensation_number.value();
        if (number < kLeTxPathLossCompMin || number > kLeTxPathLossCompMax) {
          log::error("Invalid number for tx path loss compensation: {}", number);
        } else {
          compensation = number;
        }
      }
    }
    log::info("Tx path loss compensation: {}", compensation);
    return compensation;
  }

  int8_t get_tx_power_after_calibration(int8_t tx_power) {
    if (le_tx_path_loss_comp_ == 0) {
      return tx_power;
    }
    int8_t calibrated_tx_power = tx_power;
    int64_t number = tx_power + le_tx_path_loss_comp_;
    if (number < kLeAdvertisingTxPowerMin || number > kLeAdvertisingTxPowerMax) {
      log::error("Invalid number for calibrated tx power: {}", number);
    } else {
      calibrated_tx_power = number;
    }
    log::info("tx_power: {}, calibrated_tx_power: {}", tx_power, calibrated_tx_power);
    return calibrated_tx_power;
  }

  size_t GetNumberOfAdvertisingInstances() const { return num_instances_; }

  size_t GetNumberOfAdvertisingInstancesInUse() const {
    return std::count_if(advertising_sets_.begin(), advertising_sets_.end(),
                         [](const auto& set) { return set.second.in_use; });
  }

  int get_advertiser_reg_id(AdvertiserId advertiser_id) { return id_map_[advertiser_id]; }

  AdvertisingApiType get_advertising_api_type() const { return advertising_api_type_; }

  void register_advertising_callback(AdvertisingCallback* advertising_callback) {
    advertising_callbacks_ = advertising_callback;
  }

  void multi_advertising_state_change(hci::VendorSpecificEventView event) {
    auto view = hci::LEAdvertiseStateChangeEventView::Create(event);
    log::assert_that(view.IsValid(), "assert failed: view.IsValid()");

    auto advertiser_id = view.GetAdvertisingInstance();

    log::info("Instance: 0x{:x} StateChangeReason: {} Handle: 0x{:x} Address: {}", advertiser_id,
              VseStateChangeReasonText(view.GetStateChangeReason()), view.GetConnectionHandle(),
              advertising_sets_[view.GetAdvertisingInstance()].current_address.ToString());

    if (view.GetStateChangeReason() == VseStateChangeReason::CONNECTION_RECEIVED) {
      acl_manager_->OnAdvertisingSetTerminated(ErrorCode::SUCCESS, view.GetConnectionHandle(),
                                               advertiser_id,
                                               advertising_sets_[advertiser_id].current_address,
                                               advertising_sets_[advertiser_id].discoverable);

      enabled_sets_[advertiser_id].advertising_handle_ = kInvalidHandle;

      if (!advertising_sets_[advertiser_id].directed) {
        // TODO(250666237) calculate remaining duration and advertising events
        log::info("Resuming advertising, since not directed");
        enable_advertiser(advertiser_id, true, 0, 0);
      }
    }
  }

  void handle_event(LeMetaEventView event) {
    switch (event.GetSubeventCode()) {
      case hci::SubeventCode::SCAN_REQUEST_RECEIVED:
        handle_scan_request(LeScanRequestReceivedView::Create(event));
        break;
      case hci::SubeventCode::ADVERTISING_SET_TERMINATED:
        handle_set_terminated(LeAdvertisingSetTerminatedView::Create(event));
        break;
      default:
        log::info("Unknown subevent in scanner {}", hci::SubeventCodeText(event.GetSubeventCode()));
    }
  }

  void handle_scan_request(LeScanRequestReceivedView event_view) {
    if (!event_view.IsValid()) {
      log::info("Dropping invalid scan request event");
      return;
    }
    registered_handler_->Post(common::BindOnce(scan_callback_, event_view.GetScannerAddress(),
                                               event_view.GetScannerAddressType()));
  }

  void handle_set_terminated(LeAdvertisingSetTerminatedView event_view) {
    if (!event_view.IsValid()) {
      log::info("Dropping invalid advertising event");
      return;
    }

    auto status = event_view.GetStatus();
    log::verbose("Received LE Advertising Set Terminated with status {}", ErrorCodeText(status));

    /* The Bluetooth Core 5.3 specification clearly states that this event
     * shall not be sent when the Host disables the advertising set. So in
     * case of HCI_ERROR_CANCELLED_BY_HOST, just ignore the event.
     */
    if (status == ErrorCode::OPERATION_CANCELLED_BY_HOST) {
      log::warn("Unexpected advertising set terminated event status: {}", ErrorCodeText(status));
      return;
    }

    uint8_t advertiser_id = event_view.GetAdvertisingHandle();

    bool was_rotating_address = false;
    if (advertising_sets_[advertiser_id].address_rotation_wake_alarm_ != nullptr) {
      was_rotating_address = true;
      advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Cancel();
      advertising_sets_[advertiser_id].address_rotation_wake_alarm_.reset();
    }
    if (advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_ != nullptr) {
      advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_->Cancel();
      advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_.reset();
    }
    if (advertising_sets_[advertiser_id].address_rotation_interval_min.has_value()) {
      advertising_sets_[advertiser_id].address_rotation_interval_min.reset();
    }
    if (advertising_sets_[advertiser_id].address_rotation_interval_max.has_value()) {
      advertising_sets_[advertiser_id].address_rotation_interval_max.reset();
    }

    enabled_sets_[advertiser_id].advertising_handle_ = kInvalidHandle;

    AddressWithType advertiser_address =
            advertising_sets_[event_view.GetAdvertisingHandle()].current_address;
    bool is_discoverable = advertising_sets_[event_view.GetAdvertisingHandle()].discoverable;

    acl_manager_->OnAdvertisingSetTerminated(status, event_view.GetConnectionHandle(),
                                             advertiser_id, advertiser_address, is_discoverable);

    if (status == ErrorCode::LIMIT_REACHED || status == ErrorCode::ADVERTISING_TIMEOUT) {
      if (id_map_[advertiser_id] == kIdLocal) {
        if (!advertising_sets_[advertiser_id].timeout_callback.is_null()) {
          std::move(advertising_sets_[advertiser_id].timeout_callback).Run((uint8_t)status);
          advertising_sets_[advertiser_id].timeout_callback.Reset();
        }
      } else {
        if (status == ErrorCode::LIMIT_REACHED) {
          advertising_callbacks_->OnAdvertisingEnabled(advertiser_id, false,
                                                       AdvertisingCallback::TOO_MANY_ADVERTISERS);
        } else {
          advertising_callbacks_->OnAdvertisingEnabled(advertiser_id, false,
                                                       AdvertisingCallback::TIMEOUT);
        }
      }
      return;
    }

    if (!advertising_sets_[advertiser_id].directed) {
      // TODO calculate remaining duration and advertising events
      if (advertising_sets_[advertiser_id].duration == 0 &&
          advertising_sets_[advertiser_id].max_extended_advertising_events == 0) {
        log::info("Reenable advertising");
        if (was_rotating_address) {
          log::info("Scheduling address rotation for advertiser_id={}", advertiser_id);
          if (com::android::bluetooth::flags::non_wake_alarm_for_rpa_rotation()) {
            advertising_sets_[advertiser_id].address_rotation_wake_alarm_ =
                    std::make_unique<os::Alarm>(module_handler_, true);
            advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_ =
                    std::make_unique<os::Alarm>(module_handler_, false);

            std::string client_name = "advertising_set_" + std::to_string(advertiser_id);
            auto privateAddressIntervalRange =
                    le_address_manager_->GetNextPrivateAddressIntervalRange(client_name);

            advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Schedule(
                    common::BindOnce(
                            []() { log::info("deadline wakeup in handle_set_terminated"); }),
                    privateAddressIntervalRange.max);
            advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_->Schedule(
                    common::BindOnce(&impl::set_advertising_set_random_address_on_timer,
                                     common::Unretained(this), advertiser_id),
                    privateAddressIntervalRange.min);

            // Update the expected range here.
            auto now = std::chrono::system_clock::now();
            advertising_sets_[advertiser_id].address_rotation_interval_min.emplace(
                    now + privateAddressIntervalRange.min);
            advertising_sets_[advertiser_id].address_rotation_interval_max.emplace(
                    now + privateAddressIntervalRange.max);
          } else {
            advertising_sets_[advertiser_id].address_rotation_wake_alarm_ =
                    std::make_unique<os::Alarm>(module_handler_);
            advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Schedule(
                    common::BindOnce(&impl::set_advertising_set_random_address_on_timer,
                                     common::Unretained(this), advertiser_id),
                    le_address_manager_->GetNextPrivateAddressIntervalMs());
          }
        }
        enable_advertiser(advertiser_id, true, 0, 0);
      }
    }
  }

  AdvertiserId allocate_advertiser() {
    // number of LE_MULTI_ADVT start from 1
    AdvertiserId id = advertising_api_type_ == AdvertisingApiType::ANDROID_HCI ? 1 : 0;
    while (id < num_instances_ && advertising_sets_.count(id) != 0) {
      id++;
    }
    if (id == num_instances_) {
      log::warn("Number of max instances {} reached", (uint16_t)num_instances_);
      return kInvalidId;
    }
    advertising_sets_[id].in_use = true;
    return id;
  }

  void remove_advertiser(AdvertiserId advertiser_id) {
    stop_advertising(advertiser_id);
    std::unique_lock lock(id_mutex_);
    if (advertising_sets_.count(advertiser_id) == 0) {
      return;
    }
    if (advertising_api_type_ == AdvertisingApiType::EXTENDED) {
      le_advertising_interface_->EnqueueCommand(
              hci::LeRemoveAdvertisingSetBuilder::Create(advertiser_id),
              module_handler_->BindOnce(check_complete<LeRemoveAdvertisingSetCompleteView>));

      if (advertising_sets_[advertiser_id].address_rotation_wake_alarm_ != nullptr) {
        advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Cancel();
        advertising_sets_[advertiser_id].address_rotation_wake_alarm_.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_ != nullptr) {
        advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_->Cancel();
        advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_interval_min.has_value()) {
        advertising_sets_[advertiser_id].address_rotation_interval_min.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_interval_max.has_value()) {
        advertising_sets_[advertiser_id].address_rotation_interval_max.reset();
      }
    }
    advertising_sets_.erase(advertiser_id);
    if (advertising_sets_.empty() && address_manager_registered) {
      le_address_manager_->Unregister(this);
      address_manager_registered = false;
      paused = false;
    }
  }

  /// Generates an address for the advertiser
  AddressWithType new_advertiser_address(AdvertiserId id) {
    switch (advertising_sets_[id].address_type) {
      case AdvertiserAddressType::PUBLIC:
        if (le_address_manager_->GetAddressPolicy() ==
            LeAddressManager::AddressPolicy::USE_STATIC_ADDRESS) {
          return le_address_manager_->GetInitiatorAddress();
        } else {
          return AddressWithType(controller_->GetMacAddress(), AddressType::PUBLIC_DEVICE_ADDRESS);
        }
      case AdvertiserAddressType::RESOLVABLE_RANDOM:
        if (advertising_api_type_ == AdvertisingApiType::LEGACY) {
          // we reuse the initiator address if we are a legacy advertiser using privacy,
          // since there's no way to use a different address
          return le_address_manager_->GetInitiatorAddress();
        }
        return le_address_manager_->NewResolvableAddress();
      case AdvertiserAddressType::NONRESOLVABLE_RANDOM:
        return le_address_manager_->NewNonResolvableAddress();
      default:
        log::fatal("unreachable");
    }
  }

  void create_advertiser(
          int reg_id, const AdvertisingConfig config,
          common::Callback<void(Address, AddressType)> scan_callback,
          common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback,
          os::Handler* handler) {
    AdvertiserId id = allocate_advertiser();
    if (id == kInvalidId) {
      log::warn("Number of max instances reached");
      start_advertising_fail(reg_id, AdvertisingCallback::AdvertisingStatus::TOO_MANY_ADVERTISERS);
      return;
    }

    create_advertiser_with_id(reg_id, id, config, scan_callback, set_terminated_callback, handler);
  }

  void create_advertiser_with_id(
          int reg_id, AdvertiserId id, const AdvertisingConfig config,
          common::Callback<void(Address, AddressType)> scan_callback,
          common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback,
          os::Handler* handler) {
    // check advertising data is valid before start advertising
    if (!check_advertising_data(config.advertisement, config.connectable && config.discoverable) ||
        !check_advertising_data(config.scan_response, false)) {
      advertising_callbacks_->OnAdvertisingSetStarted(
              reg_id, id, le_physical_channel_tx_power_,
              AdvertisingCallback::AdvertisingStatus::DATA_TOO_LARGE);
      return;
    }

    id_map_[id] = reg_id;
    advertising_sets_[id].scan_callback = scan_callback;
    advertising_sets_[id].set_terminated_callback = set_terminated_callback;
    advertising_sets_[id].handler = handler;

    if (!address_manager_registered) {
      le_address_manager_->Register(this);
      address_manager_registered = true;
    }

    if (com::android::bluetooth::flags::nrpa_non_connectable_adv() && !config.connectable) {
      advertising_sets_[id].address_type = GetAdvertiserAddressTypeNonConnectable(
              config.requested_advertiser_address_type, le_address_manager_->GetAddressPolicy());
    } else {
      advertising_sets_[id].address_type = GetAdvertiserAddressTypeFromRequestedTypeAndPolicy(
              config.requested_advertiser_address_type, le_address_manager_->GetAddressPolicy());
    }
    advertising_sets_[id].current_address = new_advertiser_address(id);
    set_parameters(id, config);

    switch (advertising_api_type_) {
      case (AdvertisingApiType::LEGACY): {
        if (config.advertising_type == AdvertisingType::ADV_IND ||
            config.advertising_type == AdvertisingType::ADV_NONCONN_IND) {
          set_data(id, true, config.scan_response);
        }
        set_data(id, false, config.advertisement);
        if (!paused) {
          enable_advertiser(id, true, 0, 0);
        } else {
          enabled_sets_[id].advertising_handle_ = id;
        }
      } break;
      case (AdvertisingApiType::ANDROID_HCI): {
        if (config.advertising_type == AdvertisingType::ADV_IND ||
            config.advertising_type == AdvertisingType::ADV_NONCONN_IND) {
          set_data(id, true, config.scan_response);
        }
        set_data(id, false, config.advertisement);
        if (advertising_sets_[id].address_type != AdvertiserAddressType::PUBLIC) {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeMultiAdvtSetRandomAddrBuilder::Create(
                          advertising_sets_[id].current_address.GetAddress(), id),
                  module_handler_->BindOnce(check_complete<LeMultiAdvtCompleteView>));
        }
        if (!paused) {
          enable_advertiser(id, true, 0, 0);
        } else {
          enabled_sets_[id].advertising_handle_ = id;
        }
      } break;
      case (AdvertisingApiType::EXTENDED): {
        log::warn("Unexpected AdvertisingApiType EXTENDED");
      } break;
    }
  }

  void start_advertising(
          AdvertiserId id, const AdvertisingConfig config, uint16_t duration,
          base::OnceCallback<void(uint8_t /* status */)> status_callback,
          base::OnceCallback<void(uint8_t /* status */)> timeout_callback,
          const common::Callback<void(Address, AddressType)> scan_callback,
          const common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback,
          os::Handler* handler) {
    advertising_sets_[id].status_callback = std::move(status_callback);
    advertising_sets_[id].timeout_callback = std::move(timeout_callback);

    // legacy start_advertising use default jni client id
    create_extended_advertiser_with_id(kAdvertiserClientIdJni, kIdLocal, id, config, scan_callback,
                                       set_terminated_callback, duration, 0, handler);
  }

  void create_extended_advertiser(
          uint8_t client_id, int reg_id, const AdvertisingConfig config,
          common::Callback<void(Address, AddressType)> scan_callback,
          common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback,
          uint16_t duration, uint8_t max_ext_adv_events, os::Handler* handler) {
    AdvertiserId id = allocate_advertiser();
    if (id == kInvalidId) {
      log::warn("Number of max instances reached");
      start_advertising_fail(reg_id, AdvertisingCallback::AdvertisingStatus::TOO_MANY_ADVERTISERS);
      return;
    }
    create_extended_advertiser_with_id(client_id, reg_id, id, config, scan_callback,
                                       set_terminated_callback, duration, max_ext_adv_events,
                                       handler);
  }

  void create_extended_advertiser_with_id(
          uint8_t client_id, int reg_id, AdvertiserId id, const AdvertisingConfig config,
          common::Callback<void(Address, AddressType)> scan_callback,
          common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback,
          uint16_t duration, uint8_t max_ext_adv_events, os::Handler* handler) {
    id_map_[id] = reg_id;

    if (advertising_api_type_ != AdvertisingApiType::EXTENDED) {
      create_advertiser_with_id(reg_id, id, config, scan_callback, set_terminated_callback,
                                handler);
      return;
    }

    // check extended advertising data is valid before start advertising
    if (!check_extended_advertising_data(config.advertisement,
                                         config.connectable && config.discoverable) ||
        !check_extended_advertising_data(config.scan_response, false)) {
      advertising_callbacks_->OnAdvertisingSetStarted(
              reg_id, id, le_physical_channel_tx_power_,
              AdvertisingCallback::AdvertisingStatus::DATA_TOO_LARGE);
      return;
    }

    if (!address_manager_registered) {
      le_address_manager_->Register(this);
      address_manager_registered = true;
    }

    advertising_sets_[id].scan_callback = scan_callback;
    advertising_sets_[id].set_terminated_callback = set_terminated_callback;
    advertising_sets_[id].duration = duration;
    advertising_sets_[id].max_extended_advertising_events = max_ext_adv_events;
    advertising_sets_[id].handler = handler;
    if (com::android::bluetooth::flags::nrpa_non_connectable_adv() && !config.connectable) {
      advertising_sets_[id].address_type = GetAdvertiserAddressTypeNonConnectable(
              config.requested_advertiser_address_type, le_address_manager_->GetAddressPolicy());
    } else {
      advertising_sets_[id].address_type = GetAdvertiserAddressTypeFromRequestedTypeAndPolicy(
              config.requested_advertiser_address_type, le_address_manager_->GetAddressPolicy());
    }
    advertising_sets_[id].current_address = new_advertiser_address(id);

    set_parameters(id, config);

    if (advertising_sets_[id].current_address.GetAddressType() !=
        AddressType::PUBLIC_DEVICE_ADDRESS) {
      // if we aren't using the public address type at the HCI level, we need to set the random
      // address
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetAdvertisingSetRandomAddressBuilder::Create(
                      id, advertising_sets_[id].current_address.GetAddress()),
              module_handler_->BindOnceOn(this,
                                          &impl::on_set_advertising_set_random_address_complete<
                                                  LeSetAdvertisingSetRandomAddressCompleteView>,
                                          id, advertising_sets_[id].current_address));

      bool leaudio_requested_nrpa = false;
      if (client_id == kAdvertiserClientIdLeAudio &&
          advertising_sets_[id].address_type == AdvertiserAddressType::NONRESOLVABLE_RANDOM) {
        log::info("Advertiser started by le audio client with address type: {}",
                  advertising_sets_[id].address_type);
        leaudio_requested_nrpa = true;
      }

      // but we only rotate if the AdvertiserAddressType is non-public
      // or non-rpa requested by leaudio(since static random addresses don't rotate)
      if (advertising_sets_[id].address_type != AdvertiserAddressType::PUBLIC &&
          !leaudio_requested_nrpa && (!controller_->IsRpaGenerationSupported())) {
        // start timer for random address
        log::info("Scheduling address rotation for advertiser_id={}", id);
        if (com::android::bluetooth::flags::non_wake_alarm_for_rpa_rotation()) {
          advertising_sets_[id].address_rotation_wake_alarm_ =
                  std::make_unique<os::Alarm>(module_handler_, true);
          advertising_sets_[id].address_rotation_non_wake_alarm_ =
                  std::make_unique<os::Alarm>(module_handler_, false);

          std::string client_name = "advertising_set_" + std::to_string(id);
          auto privateAddressIntervalRange =
                  le_address_manager_->GetNextPrivateAddressIntervalRange(client_name);

          advertising_sets_[id].address_rotation_wake_alarm_->Schedule(
                  common::BindOnce([]() {
                    log::info("deadline wakeup in create_extended_advertiser_with_id");
                  }),
                  privateAddressIntervalRange.max);
          advertising_sets_[id].address_rotation_non_wake_alarm_->Schedule(
                  common::BindOnce(&impl::set_advertising_set_random_address_on_timer,
                                  common::Unretained(this), id),
                  privateAddressIntervalRange.min);

          // Update the expected range here.
          auto now = std::chrono::system_clock::now();
          advertising_sets_[id].address_rotation_interval_min.emplace(
                  now + privateAddressIntervalRange.min);
          advertising_sets_[id].address_rotation_interval_max.emplace(
                  now + privateAddressIntervalRange.max);
        } else {
          advertising_sets_[id].address_rotation_wake_alarm_ =
                  std::make_unique<os::Alarm>(module_handler_);
          advertising_sets_[id].address_rotation_wake_alarm_->Schedule(
                  common::BindOnce(&impl::set_advertising_set_random_address_on_timer,
                                  common::Unretained(this), id),
                  le_address_manager_->GetNextPrivateAddressIntervalMs());
        }
      }
    }
    if (config.advertising_type == AdvertisingType::ADV_IND ||
        config.advertising_type == AdvertisingType::ADV_NONCONN_IND) {
      set_data(id, true, config.scan_response);
    }
    set_data(id, false, config.advertisement);

    if (!config.periodic_data.empty()) {
      set_periodic_parameter(id, config.periodic_advertising_parameters);
      set_periodic_data(id, config.periodic_data);
      enable_periodic_advertising(id, config.periodic_advertising_parameters.enable,
                                  config.periodic_advertising_parameters.include_adi);
    }

    if (!paused) {
      enable_advertiser(id, true, duration, max_ext_adv_events);
    } else {
      EnabledSet curr_set;
      curr_set.advertising_handle_ = id;
      curr_set.duration_ = duration;
      curr_set.max_extended_advertising_events_ = max_ext_adv_events;
      std::vector<EnabledSet> enabled_sets = {curr_set};
      enabled_sets_[id] = curr_set;
    }
  }

  void stop_advertising(AdvertiserId advertiser_id) {
    auto advertising_iter = advertising_sets_.find(advertiser_id);
    if (advertising_iter == advertising_sets_.end()) {
      log::info("Unknown advertising set {}", advertiser_id);
      return;
    }
    EnabledSet curr_set;
    curr_set.advertising_handle_ = advertiser_id;
    std::vector<EnabledSet> enabled_vector{curr_set};

    // If advertising or periodic advertising on the advertising set is enabled,
    // then the Controller will return the error code Command Disallowed (0x0C).
    // Thus, we should disable it before removing it.
    switch (advertising_api_type_) {
      case (AdvertisingApiType::LEGACY):
        le_advertising_interface_->EnqueueCommand(
                hci::LeSetAdvertisingEnableBuilder::Create(Enable::DISABLED),
                module_handler_->BindOnce(check_complete<LeSetAdvertisingEnableCompleteView>));
        break;
      case (AdvertisingApiType::ANDROID_HCI):
        le_advertising_interface_->EnqueueCommand(
                hci::LeMultiAdvtSetEnableBuilder::Create(Enable::DISABLED, advertiser_id),
                module_handler_->BindOnce(check_complete<LeMultiAdvtCompleteView>));
        break;
      case (AdvertisingApiType::EXTENDED): {
        le_advertising_interface_->EnqueueCommand(
                hci::LeSetExtendedAdvertisingEnableBuilder::Create(Enable::DISABLED,
                                                                   enabled_vector),
                module_handler_->BindOnce(
                        check_complete<LeSetExtendedAdvertisingEnableCompleteView>));

        bool is_periodic = advertising_iter->second.is_periodic;
        log::debug("advertiser_id: {} is_periodic: {}", advertiser_id, is_periodic);

        // Only set periodic advertising if supported.
        if (is_periodic && controller_->SupportsBlePeriodicAdvertising()) {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeSetPeriodicAdvertisingEnableBuilder::Create(false, false, advertiser_id),
                  module_handler_->BindOnce(
                          check_complete<LeSetPeriodicAdvertisingEnableCompleteView>));
        }
      } break;
    }

    std::unique_lock lock(id_mutex_);
    enabled_sets_[advertiser_id].advertising_handle_ = kInvalidHandle;
  }

  void rotate_advertiser_address(AdvertiserId advertiser_id) {
    if (advertising_api_type_ == AdvertisingApiType::EXTENDED) {
      AddressWithType address_with_type = new_advertiser_address(advertiser_id);
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetAdvertisingSetRandomAddressBuilder::Create(advertiser_id,
                                                                   address_with_type.GetAddress()),
              module_handler_->BindOnceOn(this,
                                          &impl::on_set_advertising_set_random_address_complete<
                                                  LeSetAdvertisingSetRandomAddressCompleteView>,
                                          advertiser_id, address_with_type));
    }
  }

  void set_advertising_set_random_address_on_timer(AdvertiserId advertiser_id) {
    // This function should only be trigger by enabled advertising set or IRK rotation
    if (enabled_sets_[advertiser_id].advertising_handle_ == kInvalidHandle) {
      if (advertising_sets_[advertiser_id].address_rotation_wake_alarm_ != nullptr) {
        advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Cancel();
        advertising_sets_[advertiser_id].address_rotation_wake_alarm_.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_ != nullptr) {
        advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_->Cancel();
        advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_interval_min.has_value()) {
        advertising_sets_[advertiser_id].address_rotation_interval_min.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_interval_max.has_value()) {
        advertising_sets_[advertiser_id].address_rotation_interval_max.reset();
      }
      return;
    }

    // TODO handle duration and max_extended_advertising_events_
    EnabledSet curr_set;
    curr_set.advertising_handle_ = advertiser_id;
    curr_set.duration_ = advertising_sets_[advertiser_id].duration;
    curr_set.max_extended_advertising_events_ =
            advertising_sets_[advertiser_id].max_extended_advertising_events;
    std::vector<EnabledSet> enabled_sets = {curr_set};

    // For connectable advertising, we should disable it first
    if (advertising_sets_[advertiser_id].connectable) {
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetExtendedAdvertisingEnableBuilder::Create(Enable::DISABLED, enabled_sets),
              module_handler_->BindOnce(
                      check_complete<LeSetExtendedAdvertisingEnableCompleteView>));
    }

    rotate_advertiser_address(advertiser_id);

    // If we are paused, we will be enabled in OnResume(), so don't resume now.
    // Note that OnResume() can never re-enable us while we are changing our address, since the
    // DISABLED and ENABLED commands are enqueued synchronously, so OnResume() doesn't need an
    // analogous check.
    if (advertising_sets_[advertiser_id].connectable && !paused) {
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetExtendedAdvertisingEnableBuilder::Create(Enable::ENABLED, enabled_sets),
              module_handler_->BindOnce(
                      check_complete<LeSetExtendedAdvertisingEnableCompleteView>));
    }

    log::info("Scheduling address rotation for advertiser_id={}", advertiser_id);
    if (com::android::bluetooth::flags::non_wake_alarm_for_rpa_rotation()) {
      std::string client_name = "advertising_set_" + std::to_string(advertiser_id);
      auto privateAddressIntervalRange =
              le_address_manager_->GetNextPrivateAddressIntervalRange(client_name);
      advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Schedule(
              common::BindOnce([]() {
                log::info("deadline wakeup in set_advertising_set_random_address_on_timer");
              }),
              privateAddressIntervalRange.max);
      advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_->Schedule(
              common::BindOnce(&impl::set_advertising_set_random_address_on_timer,
                               common::Unretained(this), advertiser_id),
              privateAddressIntervalRange.min);

      auto now = std::chrono::system_clock::now();
      if (advertising_sets_[advertiser_id].address_rotation_interval_min.has_value()) {
        le_address_manager_->CheckAddressRotationHappenedInExpectedTimeInterval(
                *(advertising_sets_[advertiser_id].address_rotation_interval_min),
                *(advertising_sets_[advertiser_id].address_rotation_interval_max), now,
                client_name);
      }

      // Update the expected range here.
      advertising_sets_[advertiser_id].address_rotation_interval_min.emplace(
              now + privateAddressIntervalRange.min);
      advertising_sets_[advertiser_id].address_rotation_interval_max.emplace(
              now + privateAddressIntervalRange.max);
    } else {
      advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Schedule(
              common::BindOnce(&impl::set_advertising_set_random_address_on_timer,
                               common::Unretained(this), advertiser_id),
              le_address_manager_->GetNextPrivateAddressIntervalMs());
    }
  }

  void register_advertiser(
          common::ContextualOnceCallback<void(uint8_t /* inst_id */,
                                              AdvertisingCallback::AdvertisingStatus /* status */)>
                  callback) {
    AdvertiserId id = allocate_advertiser();
    if (id == kInvalidId) {
      callback(kInvalidId, AdvertisingCallback::AdvertisingStatus::TOO_MANY_ADVERTISERS);
    } else {
      callback(id, AdvertisingCallback::AdvertisingStatus::SUCCESS);
    }
  }

  void get_own_address(AdvertiserId advertiser_id) {
    if (advertising_sets_.find(advertiser_id) == advertising_sets_.end()) {
      log::info("Unknown advertising id {}", advertiser_id);
      return;
    }
    auto current_address = advertising_sets_[advertiser_id].current_address;
    advertising_callbacks_->OnOwnAddressRead(advertiser_id,
                                             static_cast<uint8_t>(current_address.GetAddressType()),
                                             current_address.GetAddress());
  }

  void set_parameters(AdvertiserId advertiser_id, AdvertisingConfig config) {
    config.tx_power = get_tx_power_after_calibration(static_cast<int8_t>(config.tx_power));
    advertising_sets_[advertiser_id].is_legacy = config.legacy_pdus;
    advertising_sets_[advertiser_id].connectable = config.connectable;
    advertising_sets_[advertiser_id].discoverable = config.discoverable;
    advertising_sets_[advertiser_id].tx_power = config.tx_power;
    advertising_sets_[advertiser_id].directed = config.directed;
    advertising_sets_[advertiser_id].is_periodic = config.periodic_advertising_parameters.enable;

    // based on logic in new_advertiser_address
    auto own_address_type = static_cast<OwnAddressType>(
            advertising_sets_[advertiser_id].current_address.GetAddressType());

    if (controller_->IsRpaGenerationSupported() &&
        own_address_type != OwnAddressType::PUBLIC_DEVICE_ADDRESS) {
      log::info("Support RPA offload, set own address type RESOLVABLE_OR_RANDOM_ADDRESS");
      own_address_type = OwnAddressType::RESOLVABLE_OR_RANDOM_ADDRESS;
    }

    switch (advertising_api_type_) {
      case (AdvertisingApiType::LEGACY): {
        le_advertising_interface_->EnqueueCommand(
                hci::LeSetAdvertisingParametersBuilder::Create(
                        config.interval_min, config.interval_max, config.advertising_type,
                        own_address_type, config.peer_address_type, config.peer_address,
                        config.channel_map, config.filter_policy),
                module_handler_->BindOnceOn(
                        this, &impl::check_status_with_id<LeSetAdvertisingParametersCompleteView>,
                        true, advertiser_id));
      } break;
      case (AdvertisingApiType::ANDROID_HCI): {
        le_advertising_interface_->EnqueueCommand(
                hci::LeMultiAdvtParamBuilder::Create(
                        config.interval_min, config.interval_max, config.advertising_type,
                        own_address_type,
                        advertising_sets_[advertiser_id].current_address.GetAddress(),
                        config.peer_address_type, config.peer_address, config.channel_map,
                        config.filter_policy, advertiser_id, config.tx_power),
                module_handler_->BindOnceOn(this,
                                            &impl::check_status_with_id<LeMultiAdvtCompleteView>,
                                            true, advertiser_id));
      } break;
      case (AdvertisingApiType::EXTENDED): {
        // sid must be in range 0x00 to 0x0F. Since no controller supports more than
        // 16 advertisers, it's safe to make sid equal to id.
        config.sid = advertiser_id % kAdvertisingSetIdMask;

        if (config.legacy_pdus) {
          LegacyAdvertisingEventProperties legacy_properties =
                  LegacyAdvertisingEventProperties::ADV_IND;
          if (config.connectable && config.directed) {
            if (config.high_duty_cycle) {
              legacy_properties = LegacyAdvertisingEventProperties::ADV_DIRECT_IND_HIGH;
            } else {
              legacy_properties = LegacyAdvertisingEventProperties::ADV_DIRECT_IND_LOW;
            }
          }
          if (config.scannable && !config.connectable) {
            legacy_properties = LegacyAdvertisingEventProperties::ADV_SCAN_IND;
          }
          if (!config.scannable && !config.connectable) {
            legacy_properties = LegacyAdvertisingEventProperties::ADV_NONCONN_IND;
          }

          le_advertising_interface_->EnqueueCommand(
                  LeSetExtendedAdvertisingParametersLegacyBuilder::Create(
                          advertiser_id, legacy_properties, config.interval_min,
                          config.interval_max, config.channel_map, own_address_type,
                          config.peer_address_type, config.peer_address, config.filter_policy,
                          config.tx_power, config.sid, config.enable_scan_request_notifications),
                  module_handler_->BindOnceOn(
                          this,
                          &impl::on_set_extended_advertising_parameters_complete<
                                  LeSetExtendedAdvertisingParametersCompleteView>,
                          advertiser_id));
        } else {
          AdvertisingEventProperties extended_properties;
          extended_properties.connectable_ = config.connectable;
          extended_properties.scannable_ = config.scannable;
          extended_properties.directed_ = config.directed;
          extended_properties.high_duty_cycle_ = config.high_duty_cycle;
          extended_properties.legacy_ = false;
          extended_properties.anonymous_ = config.anonymous;
          extended_properties.tx_power_ = config.include_tx_power;

          le_advertising_interface_->EnqueueCommand(
                  hci::LeSetExtendedAdvertisingParametersBuilder::Create(
                          advertiser_id, extended_properties, config.interval_min,
                          config.interval_max, config.channel_map, own_address_type,
                          config.peer_address_type, config.peer_address, config.filter_policy,
                          config.tx_power,
                          (config.use_le_coded_phy ? PrimaryPhyType::LE_CODED
                                                   : PrimaryPhyType::LE_1M),
                          config.secondary_max_skip, config.secondary_advertising_phy, config.sid,
                          config.enable_scan_request_notifications),
                  module_handler_->BindOnceOn(
                          this,
                          &impl::on_set_extended_advertising_parameters_complete<
                                  LeSetExtendedAdvertisingParametersCompleteView>,
                          advertiser_id));
        }
      } break;
    }
  }

  bool data_has_flags(std::vector<GapData> data) {
    for (auto& gap_data : data) {
      if (gap_data.data_type_ == GapDataType::FLAGS) {
        return true;
      }
    }
    return false;
  }

  bool check_advertising_data(std::vector<GapData> data, bool include_flag) {
    uint16_t data_len = 0;
    // check data size
    for (size_t i = 0; i < data.size(); i++) {
      data_len += data[i].size();
    }

    // The Flags data type shall be included when any of the Flag bits are non-zero and the
    // advertising packet is connectable and discoverable. It will be added by set_data() function,
    // we should count it here.
    if (include_flag && !data_has_flags(data)) {
      data_len += kLenOfFlags;
    }

    if (data_len > le_maximum_advertising_data_length_) {
      log::warn("advertising data len {} exceeds le_maximum_advertising_data_length_ {}", data_len,
                le_maximum_advertising_data_length_);
      return false;
    }
    return true;
  }

  bool check_extended_advertising_data(std::vector<GapData> data, bool include_flag) {
    uint16_t data_len = 0;
    // check data size
    for (size_t i = 0; i < data.size(); i++) {
      if (data[i].size() > kLeMaximumGapDataLength) {
        log::warn("AD data len shall not greater than {}", kLeMaximumGapDataLength);
        return false;
      }
      data_len += data[i].size();
    }

    // The Flags data type shall be included when any of the Flag bits are non-zero and the
    // advertising packet is connectable and discoverable. It will be added by set_data() function,
    // we should count it here.
    if (include_flag && !data_has_flags(data)) {
      data_len += kLenOfFlags;
    }

    if (data_len > le_maximum_advertising_data_length_) {
      log::warn("advertising data len {} exceeds le_maximum_advertising_data_length_ {}", data_len,
                le_maximum_advertising_data_length_);
      return false;
    }
    return true;
  }

  void set_data(AdvertiserId advertiser_id, bool set_scan_rsp, std::vector<GapData> data) {
    // The Flags data type shall be included when any of the Flag bits are non-zero and the
    // advertising packet is connectable and discoverable.
    if (!set_scan_rsp && advertising_sets_[advertiser_id].connectable &&
        advertising_sets_[advertiser_id].discoverable && !data_has_flags(data)) {
      GapData gap_data;
      gap_data.data_type_ = GapDataType::FLAGS;
      if (advertising_sets_[advertiser_id].duration == 0) {
        gap_data.data_.push_back(static_cast<uint8_t>(AdvertisingFlag::LE_GENERAL_DISCOVERABLE));
      } else {
        gap_data.data_.push_back(static_cast<uint8_t>(AdvertisingFlag::LE_LIMITED_DISCOVERABLE));
      }
      data.insert(data.begin(), gap_data);
    }

    // Find and fill TX Power with the correct value.
    for (auto& gap_data : data) {
      if (gap_data.data_type_ == GapDataType::TX_POWER_LEVEL) {
        gap_data.data_[0] = advertising_sets_[advertiser_id].tx_power;
        break;
      }
    }

    if (advertising_api_type_ != AdvertisingApiType::EXTENDED &&
        !check_advertising_data(data, false)) {
      if (set_scan_rsp) {
        advertising_callbacks_->OnScanResponseDataSet(
                advertiser_id, AdvertisingCallback::AdvertisingStatus::DATA_TOO_LARGE);
      } else {
        advertising_callbacks_->OnAdvertisingDataSet(
                advertiser_id, AdvertisingCallback::AdvertisingStatus::DATA_TOO_LARGE);
      }
      return;
    }

    switch (advertising_api_type_) {
      case (AdvertisingApiType::LEGACY): {
        if (set_scan_rsp) {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeSetScanResponseDataBuilder::Create(data),
                  module_handler_->BindOnceOn(
                          this, &impl::check_status_with_id<LeSetScanResponseDataCompleteView>,
                          true, advertiser_id));
        } else {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeSetAdvertisingDataBuilder::Create(data),
                  module_handler_->BindOnceOn(
                          this, &impl::check_status_with_id<LeSetAdvertisingDataCompleteView>, true,
                          advertiser_id));
        }
      } break;
      case (AdvertisingApiType::ANDROID_HCI): {
        if (set_scan_rsp) {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeMultiAdvtSetScanRespBuilder::Create(data, advertiser_id),
                  module_handler_->BindOnceOn(this,
                                              &impl::check_status_with_id<LeMultiAdvtCompleteView>,
                                              true, advertiser_id));
        } else {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeMultiAdvtSetDataBuilder::Create(data, advertiser_id),
                  module_handler_->BindOnceOn(this,
                                              &impl::check_status_with_id<LeMultiAdvtCompleteView>,
                                              true, advertiser_id));
        }
      } break;
      case (AdvertisingApiType::EXTENDED): {
        uint16_t data_len = 0;
        // check data size
        for (size_t i = 0; i < data.size(); i++) {
          if (data[i].size() > kLeMaximumGapDataLength) {
            log::warn("AD data len shall not greater than {}", kLeMaximumGapDataLength);
            if (advertising_callbacks_ != nullptr) {
              if (set_scan_rsp) {
                advertising_callbacks_->OnScanResponseDataSet(
                        advertiser_id, AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
              } else {
                advertising_callbacks_->OnAdvertisingDataSet(
                        advertiser_id, AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
              }
            }
            return;
          }
          data_len += data[i].size();
        }

        int maxDataLength =
                (com::android::bluetooth::flags::ble_check_data_length_on_legacy_advertising() &&
                 advertising_sets_[advertiser_id].is_legacy)
                        ? kLeMaximumLegacyAdvertisingDataLength
                        : le_maximum_advertising_data_length_;

        if (data_len > maxDataLength) {
          log::warn("advertising data len {} exceeds maxDataLength {}", data_len, maxDataLength);
          if (advertising_callbacks_ != nullptr) {
            if (set_scan_rsp) {
              advertising_callbacks_->OnScanResponseDataSet(
                      advertiser_id, AdvertisingCallback::AdvertisingStatus::DATA_TOO_LARGE);
            } else {
              advertising_callbacks_->OnAdvertisingDataSet(
                      advertiser_id, AdvertisingCallback::AdvertisingStatus::DATA_TOO_LARGE);
            }
          }
          return;
        }

        if (data_len <= kLeMaximumFragmentLength) {
          send_data_fragment(advertiser_id, set_scan_rsp, data, Operation::COMPLETE_ADVERTISEMENT);
        } else {
          std::vector<GapData> sub_data;
          Operation operation = Operation::FIRST_FRAGMENT;

          std::vector<std::unique_ptr<packet::RawBuilder>> fragments;
          packet::FragmentingInserter it(kLeMaximumFragmentLength,
                                         std::back_insert_iterator(fragments));
          for (auto gap_data : data) {
            gap_data.Serialize(it);
          }
          it.finalize();

          for (size_t i = 0; i < fragments.size(); i++) {
            send_data_fragment_with_raw_builder(
                    advertiser_id, set_scan_rsp, std::move(fragments[i]),
                    (i == fragments.size() - 1) ? Operation::LAST_FRAGMENT : operation);
            operation = Operation::INTERMEDIATE_FRAGMENT;
          }
        }
      } break;
    }
  }

  void send_data_fragment(AdvertiserId advertiser_id, bool set_scan_rsp, std::vector<GapData> data,
                          Operation operation) {
    // For first and intermediate fragment, do not trigger advertising_callbacks_.
    bool send_callback = (operation == Operation::COMPLETE_ADVERTISEMENT ||
                          operation == Operation::LAST_FRAGMENT);
    if (set_scan_rsp) {
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetExtendedScanResponseDataBuilder::Create(advertiser_id, operation,
                                                                kFragment_preference, data),
              module_handler_->BindOnceOn(
                      this, &impl::check_status_with_id<LeSetExtendedScanResponseDataCompleteView>,
                      send_callback, advertiser_id));
    } else {
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetExtendedAdvertisingDataBuilder::Create(advertiser_id, operation,
                                                               kFragment_preference, data),
              module_handler_->BindOnceOn(
                      this, &impl::check_status_with_id<LeSetExtendedAdvertisingDataCompleteView>,
                      send_callback, advertiser_id));
    }
  }

  void send_data_fragment_with_raw_builder(AdvertiserId advertiser_id, bool set_scan_rsp,
                                           std::unique_ptr<packet::RawBuilder> data,
                                           Operation operation) {
    // For first and intermediate fragment, do not trigger advertising_callbacks_.
    bool send_callback = (operation == Operation::COMPLETE_ADVERTISEMENT ||
                          operation == Operation::LAST_FRAGMENT);
    if (set_scan_rsp) {
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetExtendedScanResponseDataRawBuilder::Create(
                      advertiser_id, operation, kFragment_preference, std::move(data)),
              module_handler_->BindOnceOn(
                      this, &impl::check_status_with_id<LeSetExtendedScanResponseDataCompleteView>,
                      send_callback, advertiser_id));
    } else {
      le_advertising_interface_->EnqueueCommand(
              hci::LeSetExtendedAdvertisingDataRawBuilder::Create(
                      advertiser_id, operation, kFragment_preference, std::move(data)),
              module_handler_->BindOnceOn(
                      this, &impl::check_status_with_id<LeSetExtendedAdvertisingDataCompleteView>,
                      send_callback, advertiser_id));
    }
  }

  void enable_advertiser(AdvertiserId advertiser_id, bool enable, uint16_t duration,
                         uint8_t max_extended_advertising_events) {
    EnabledSet curr_set;
    curr_set.advertising_handle_ = advertiser_id;
    curr_set.duration_ = duration;
    curr_set.max_extended_advertising_events_ = max_extended_advertising_events;
    std::vector<EnabledSet> enabled_sets = {curr_set};
    Enable enable_value = enable ? Enable::ENABLED : Enable::DISABLED;

    if (!advertising_sets_.count(advertiser_id)) {
      log::warn("No advertising set with key: {}", advertiser_id);
      return;
    }

    switch (advertising_api_type_) {
      case (AdvertisingApiType::LEGACY): {
        le_advertising_interface_->EnqueueCommand(
                hci::LeSetAdvertisingEnableBuilder::Create(enable_value),
                module_handler_->BindOnceOn(this,
                                            &impl::on_set_advertising_enable_complete<
                                                    LeSetAdvertisingEnableCompleteView>,
                                            enable, enabled_sets, true /* trigger callbacks */));
      } break;
      case (AdvertisingApiType::ANDROID_HCI): {
        le_advertising_interface_->EnqueueCommand(
                hci::LeMultiAdvtSetEnableBuilder::Create(enable_value, advertiser_id),
                module_handler_->BindOnceOn(
                        this, &impl::on_set_advertising_enable_complete<LeMultiAdvtCompleteView>,
                        enable, enabled_sets, true /* trigger callbacks */));
      } break;
      case (AdvertisingApiType::EXTENDED): {
        le_advertising_interface_->EnqueueCommand(
                hci::LeSetExtendedAdvertisingEnableBuilder::Create(enable_value, enabled_sets),
                module_handler_->BindOnceOn(this,
                                            &impl::on_set_extended_advertising_enable_complete<
                                                    LeSetExtendedAdvertisingEnableCompleteView>,
                                            enable, enabled_sets, true /* trigger callbacks */));
      } break;
    }

    if (enable) {
      enabled_sets_[advertiser_id].advertising_handle_ = advertiser_id;
      if (advertising_api_type_ == AdvertisingApiType::EXTENDED) {
        enabled_sets_[advertiser_id].duration_ = duration;
        enabled_sets_[advertiser_id].max_extended_advertising_events_ =
                max_extended_advertising_events;
      }

      advertising_sets_[advertiser_id].duration = duration;
      advertising_sets_[advertiser_id].max_extended_advertising_events =
              max_extended_advertising_events;
    } else {
      enabled_sets_[advertiser_id].advertising_handle_ = kInvalidHandle;
      if (advertising_sets_[advertiser_id].address_rotation_wake_alarm_ != nullptr) {
        advertising_sets_[advertiser_id].address_rotation_wake_alarm_->Cancel();
        advertising_sets_[advertiser_id].address_rotation_wake_alarm_.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_ != nullptr) {
        advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_->Cancel();
        advertising_sets_[advertiser_id].address_rotation_non_wake_alarm_.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_interval_min.has_value()) {
        advertising_sets_[advertiser_id].address_rotation_interval_min.reset();
      }
      if (advertising_sets_[advertiser_id].address_rotation_interval_max.has_value()) {
        advertising_sets_[advertiser_id].address_rotation_interval_max.reset();
      }
    }
  }

  void set_periodic_parameter(AdvertiserId advertiser_id,
                              PeriodicAdvertisingParameters periodic_advertising_parameters) {
    uint8_t include_tx_power = periodic_advertising_parameters.properties >>
                               PeriodicAdvertisingParameters::AdvertisingProperty::INCLUDE_TX_POWER;

    le_advertising_interface_->EnqueueCommand(
            hci::LeSetPeriodicAdvertisingParametersBuilder::Create(
                    advertiser_id, periodic_advertising_parameters.min_interval,
                    periodic_advertising_parameters.max_interval, include_tx_power),
            module_handler_->BindOnceOn(
                    this,
                    &impl::check_status_with_id<LeSetPeriodicAdvertisingParametersCompleteView>,
                    true, advertiser_id));
  }

  void set_periodic_data(AdvertiserId advertiser_id, std::vector<GapData> data) {
    uint16_t data_len = 0;
    // check data size
    for (size_t i = 0; i < data.size(); i++) {
      if (data[i].size() > kLeMaximumGapDataLength) {
        log::warn("AD data len shall not greater than {}", kLeMaximumGapDataLength);
        if (advertising_callbacks_ != nullptr) {
          advertising_callbacks_->OnPeriodicAdvertisingDataSet(
                  advertiser_id, AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
        }
        return;
      }
      data_len += data[i].size();
    }

    if (data_len > le_maximum_advertising_data_length_) {
      log::warn("advertising data len exceeds le_maximum_advertising_data_length_ {}",
                le_maximum_advertising_data_length_);
      if (advertising_callbacks_ != nullptr) {
        advertising_callbacks_->OnPeriodicAdvertisingDataSet(
                advertiser_id, AdvertisingCallback::AdvertisingStatus::DATA_TOO_LARGE);
      }
      return;
    }

    if (data_len <= kLeMaximumPeriodicDataFragmentLength) {
      send_periodic_data_fragment(advertiser_id, data, Operation::COMPLETE_ADVERTISEMENT);
    } else {
      std::vector<GapData> sub_data;
      Operation operation = Operation::FIRST_FRAGMENT;

      std::vector<std::unique_ptr<packet::RawBuilder>> fragments;
      packet::FragmentingInserter it(kLeMaximumPeriodicDataFragmentLength,
                                     std::back_insert_iterator(fragments));
      for (auto gap_data : data) {
        gap_data.Serialize(it);
      }
      it.finalize();

      for (size_t i = 0; i < fragments.size(); i++) {
        send_periodic_data_fragment_with_raw_builder(
                advertiser_id, std::move(fragments[i]),
                (i == fragments.size() - 1) ? Operation::LAST_FRAGMENT : operation);
        operation = Operation::INTERMEDIATE_FRAGMENT;
      }
    }
  }

  void send_periodic_data_fragment(AdvertiserId advertiser_id, std::vector<GapData> data,
                                   Operation operation) {
    // For first and intermediate fragment, do not trigger advertising_callbacks_.
    bool send_callback = (operation == Operation::COMPLETE_ADVERTISEMENT ||
                          operation == Operation::LAST_FRAGMENT);
    le_advertising_interface_->EnqueueCommand(
            hci::LeSetPeriodicAdvertisingDataBuilder::Create(advertiser_id, operation, data),
            module_handler_->BindOnceOn(
                    this, &impl::check_status_with_id<LeSetPeriodicAdvertisingDataCompleteView>,
                    send_callback, advertiser_id));
  }

  void send_periodic_data_fragment_with_raw_builder(AdvertiserId advertiser_id,
                                                    std::unique_ptr<packet::RawBuilder> data,
                                                    Operation operation) {
    // For first and intermediate fragment, do not trigger advertising_callbacks_.
    bool send_callback = (operation == Operation::COMPLETE_ADVERTISEMENT ||
                          operation == Operation::LAST_FRAGMENT);
    le_advertising_interface_->EnqueueCommand(
            hci::LeSetPeriodicAdvertisingDataRawBuilder::Create(advertiser_id, operation,
                                                                std::move(data)),
            module_handler_->BindOnceOn(
                    this, &impl::check_status_with_id<LeSetPeriodicAdvertisingDataCompleteView>,
                    send_callback, advertiser_id));
  }

  void enable_periodic_advertising(AdvertiserId advertiser_id, bool enable, bool include_adi) {
    if (!controller_->SupportsBlePeriodicAdvertising()) {
      return;
    }

    if (include_adi && !controller_->SupportsBlePeriodicAdvertisingAdi()) {
      include_adi = false;
    }
    le_advertising_interface_->EnqueueCommand(
            hci::LeSetPeriodicAdvertisingEnableBuilder::Create(enable, include_adi, advertiser_id),
            module_handler_->BindOnceOn(this,
                                        &impl::on_set_periodic_advertising_enable_complete<
                                                LeSetPeriodicAdvertisingEnableCompleteView>,
                                        enable, advertiser_id));
  }

  void OnPause() override {
    if (!address_manager_registered) {
      log::warn("Unregistered!");
      return;
    }
    paused = true;
    if (!advertising_sets_.empty()) {
      std::vector<EnabledSet> enabled_sets = {};
      for (size_t i = 0; i < enabled_sets_.size(); i++) {
        EnabledSet curr_set = enabled_sets_[i];
        if (enabled_sets_[i].advertising_handle_ != kInvalidHandle) {
          enabled_sets.push_back(enabled_sets_[i]);
        }
      }

      switch (advertising_api_type_) {
        case (AdvertisingApiType::LEGACY): {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeSetAdvertisingEnableBuilder::Create(Enable::DISABLED),
                  module_handler_->BindOnce(check_complete<LeSetAdvertisingEnableCompleteView>));
        } break;
        case (AdvertisingApiType::ANDROID_HCI): {
          for (size_t i = 0; i < enabled_sets_.size(); i++) {
            uint8_t id = enabled_sets_[i].advertising_handle_;
            if (id != kInvalidHandle) {
              le_advertising_interface_->EnqueueCommand(
                      hci::LeMultiAdvtSetEnableBuilder::Create(Enable::DISABLED, id),
                      module_handler_->BindOnce(check_complete<LeMultiAdvtCompleteView>));
            }
          }
        } break;
        case (AdvertisingApiType::EXTENDED): {
          if (enabled_sets.size() != 0) {
            le_advertising_interface_->EnqueueCommand(
                    hci::LeSetExtendedAdvertisingEnableBuilder::Create(Enable::DISABLED,
                                                                       enabled_sets),
                    module_handler_->BindOnce(
                            check_complete<LeSetExtendedAdvertisingEnableCompleteView>));
          }
        } break;
      }
    }
    le_address_manager_->AckPause(this);
  }

  void OnResume() override {
    if (!address_manager_registered) {
      log::warn("Unregistered!");
      return;
    }
    paused = false;
    if (!advertising_sets_.empty()) {
      std::vector<EnabledSet> enabled_sets = {};
      for (size_t i = 0; i < enabled_sets_.size(); i++) {
        EnabledSet curr_set = enabled_sets_[i];
        if (enabled_sets_[i].advertising_handle_ != kInvalidHandle) {
          enabled_sets.push_back(enabled_sets_[i]);
        }
      }

      switch (advertising_api_type_) {
        case (AdvertisingApiType::LEGACY): {
          le_advertising_interface_->EnqueueCommand(
                  hci::LeSetAdvertisingEnableBuilder::Create(Enable::ENABLED),
                  module_handler_->BindOnceOn(this,
                                              &impl::on_set_advertising_enable_complete<
                                                      LeSetAdvertisingEnableCompleteView>,
                                              true, enabled_sets, false /* trigger_callbacks */));
        } break;
        case (AdvertisingApiType::ANDROID_HCI): {
          for (size_t i = 0; i < enabled_sets_.size(); i++) {
            uint8_t id = enabled_sets_[i].advertising_handle_;
            if (id != kInvalidHandle) {
              le_advertising_interface_->EnqueueCommand(
                      hci::LeMultiAdvtSetEnableBuilder::Create(Enable::ENABLED, id),
                      module_handler_->BindOnceOn(
                              this,
                              &impl::on_set_advertising_enable_complete<LeMultiAdvtCompleteView>,
                              true, enabled_sets, false /* trigger_callbacks */));
            }
          }
        } break;
        case (AdvertisingApiType::EXTENDED): {
          if (enabled_sets.size() != 0) {
            le_advertising_interface_->EnqueueCommand(
                    hci::LeSetExtendedAdvertisingEnableBuilder::Create(Enable::ENABLED,
                                                                       enabled_sets),
                    module_handler_->BindOnceOn(this,
                                                &impl::on_set_extended_advertising_enable_complete<
                                                        LeSetExtendedAdvertisingEnableCompleteView>,
                                                true, enabled_sets, false /* trigger_callbacks */));
          }
        } break;
      }
    }
    le_address_manager_->AckResume(this);
  }

  // Note: this needs to be synchronous (i.e. NOT on a handler) for two reasons:
  // 1. For parity with OnPause() and OnResume()
  // 2. If we don't enqueue our HCI commands SYNCHRONOUSLY, then it is possible that we OnResume()
  // in addressManager before our commands complete. So then our commands reach the HCI layer
  // *after* the resume commands from address manager, which is racey (even if it might not matter).
  //
  // If you are a future developer making this asynchronous, you need to add some kind of
  // ->AckIRKChange() method to the address manager so we can defer resumption to after this
  // completes.
  void NotifyOnIRKChange() override {
    for (size_t i = 0; i < enabled_sets_.size(); i++) {
      if (enabled_sets_[i].advertising_handle_ != kInvalidHandle) {
        rotate_advertiser_address(i);
      }
    }
  }

  common::Callback<void(Address, AddressType)> scan_callback_;
  common::ContextualCallback<void(ErrorCode, uint16_t, hci::AddressWithType)>
          set_terminated_callback_{};
  AdvertisingCallback* advertising_callbacks_ = nullptr;
  os::Handler* registered_handler_{nullptr};
  Module* module_;
  os::Handler* module_handler_;
  hci::HciLayer* hci_layer_;
  hci::Controller* controller_;
  uint16_t le_maximum_advertising_data_length_;
  int8_t le_physical_channel_tx_power_ = 0;
  int8_t le_tx_path_loss_comp_ = 0;
  hci::LeAdvertisingInterface* le_advertising_interface_;
  std::map<AdvertiserId, Advertiser> advertising_sets_;
  hci::LeAddressManager* le_address_manager_;
  hci::AclManager* acl_manager_;
  bool address_manager_registered = false;
  bool paused = false;

  std::mutex id_mutex_;
  size_t num_instances_;
  std::vector<hci::EnabledSet> enabled_sets_;
  // map to mapping the id from java layer and advertier id
  std::map<uint8_t, int> id_map_;

  AdvertisingApiType advertising_api_type_{0};

  void on_read_advertising_physical_channel_tx_power(CommandCompleteView view) {
    auto complete_view = LeReadAdvertisingPhysicalChannelTxPowerCompleteView::Create(view);
    if (!complete_view.IsValid()) {
      auto payload = view.GetPayload();
      if (payload.size() == 1 &&
          payload[0] == static_cast<uint8_t>(ErrorCode::UNKNOWN_HCI_COMMAND)) {
        log::info("Unknown command, not setting tx power");
        return;
      }
    }
    log::assert_that(complete_view.IsValid(), "assert failed: complete_view.IsValid()");
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      log::info("Got a command complete with status {}", ErrorCodeText(complete_view.GetStatus()));
      return;
    }
    le_physical_channel_tx_power_ = complete_view.GetTransmitPowerLevel();
  }

  template <class View>
  void on_set_advertising_enable_complete(bool enable, std::vector<EnabledSet> enabled_sets,
                                          bool trigger_callbacks, CommandCompleteView view) {
    log::assert_that(view.IsValid(), "assert failed: view.IsValid()");
    auto complete_view = View::Create(view);
    log::assert_that(complete_view.IsValid(), "assert failed: complete_view.IsValid()");
    AdvertisingCallback::AdvertisingStatus advertising_status =
            AdvertisingCallback::AdvertisingStatus::SUCCESS;
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      log::info("Got a command complete with status {}", ErrorCodeText(complete_view.GetStatus()));
    }

    if (advertising_callbacks_ == nullptr) {
      return;
    }
    for (EnabledSet enabled_set : enabled_sets) {
      bool started = advertising_sets_[enabled_set.advertising_handle_].started;
      uint8_t id = enabled_set.advertising_handle_;
      if (id == kInvalidHandle) {
        continue;
      }

      int reg_id = id_map_[id];
      if (reg_id == kIdLocal) {
        if (!advertising_sets_[enabled_set.advertising_handle_].status_callback.is_null()) {
          std::move(advertising_sets_[enabled_set.advertising_handle_].status_callback)
                  .Run(advertising_status);
          advertising_sets_[enabled_set.advertising_handle_].status_callback.Reset();
        }
        continue;
      }

      if (started) {
        if (trigger_callbacks) {
          advertising_callbacks_->OnAdvertisingEnabled(id, enable, advertising_status);
        }
      } else {
        advertising_sets_[enabled_set.advertising_handle_].started = true;
        advertising_callbacks_->OnAdvertisingSetStarted(reg_id, id, le_physical_channel_tx_power_,
                                                        advertising_status);
      }
    }
  }

  template <class View>
  void on_set_extended_advertising_enable_complete(bool enable,
                                                   std::vector<EnabledSet> enabled_sets,
                                                   bool trigger_callbacks,
                                                   CommandCompleteView view) {
    log::assert_that(view.IsValid(), "assert failed: view.IsValid()");
    auto complete_view = LeSetExtendedAdvertisingEnableCompleteView::Create(view);
    log::assert_that(complete_view.IsValid(), "assert failed: complete_view.IsValid()");
    AdvertisingCallback::AdvertisingStatus advertising_status =
            AdvertisingCallback::AdvertisingStatus::SUCCESS;
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      log::info("Got a command complete with status {}", ErrorCodeText(complete_view.GetStatus()));
      advertising_status = AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR;
    }

    if (advertising_callbacks_ == nullptr) {
      return;
    }

    for (EnabledSet enabled_set : enabled_sets) {
      int8_t tx_power = advertising_sets_[enabled_set.advertising_handle_].tx_power;
      bool started = advertising_sets_[enabled_set.advertising_handle_].started;
      uint8_t id = enabled_set.advertising_handle_;
      if (id == kInvalidHandle) {
        continue;
      }

      int reg_id = id_map_[id];
      if (reg_id == kIdLocal) {
        if (!advertising_sets_[enabled_set.advertising_handle_].status_callback.is_null()) {
          std::move(advertising_sets_[enabled_set.advertising_handle_].status_callback)
                  .Run(advertising_status);
          advertising_sets_[enabled_set.advertising_handle_].status_callback.Reset();
        }
        continue;
      }

      if (started) {
        if (trigger_callbacks) {
          advertising_callbacks_->OnAdvertisingEnabled(id, enable, advertising_status);
        }
      } else {
        advertising_sets_[enabled_set.advertising_handle_].started = true;
        advertising_callbacks_->OnAdvertisingSetStarted(reg_id, id, tx_power, advertising_status);
      }
    }
  }

  template <class View>
  void on_set_extended_advertising_parameters_complete(AdvertiserId id, CommandCompleteView view) {
    log::assert_that(view.IsValid(), "assert failed: view.IsValid()");
    auto complete_view = LeSetExtendedAdvertisingParametersCompleteView::Create(view);
    log::assert_that(complete_view.IsValid(), "assert failed: complete_view.IsValid()");
    AdvertisingCallback::AdvertisingStatus advertising_status =
            AdvertisingCallback::AdvertisingStatus::SUCCESS;
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      log::info("Got a command complete with status {}", ErrorCodeText(complete_view.GetStatus()));
      advertising_status = AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR;
    }
    advertising_sets_[id].tx_power = complete_view.GetSelectedTxPower();

    if (advertising_sets_[id].started && id_map_[id] != kIdLocal) {
      advertising_callbacks_->OnAdvertisingParametersUpdated(id, advertising_sets_[id].tx_power,
                                                             advertising_status);
    }
  }

  template <class View>
  void on_set_periodic_advertising_enable_complete(bool enable, AdvertiserId id,
                                                   CommandCompleteView view) {
    log::assert_that(view.IsValid(), "assert failed: view.IsValid()");
    auto complete_view = LeSetPeriodicAdvertisingEnableCompleteView::Create(view);
    log::assert_that(complete_view.IsValid(), "assert failed: complete_view.IsValid()");
    AdvertisingCallback::AdvertisingStatus advertising_status =
            AdvertisingCallback::AdvertisingStatus::SUCCESS;
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      log::info("Got a command complete with status {}", ErrorCodeText(complete_view.GetStatus()));
      advertising_status = AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR;
    }

    if (advertising_callbacks_ == nullptr || !advertising_sets_[id].started ||
        id_map_[id] == kIdLocal) {
      return;
    }

    advertising_callbacks_->OnPeriodicAdvertisingEnabled(id, enable, advertising_status);
  }

  template <class View>
  void on_set_advertising_set_random_address_complete(AdvertiserId advertiser_id,
                                                      AddressWithType address_with_type,
                                                      CommandCompleteView view) {
    log::assert_that(view.IsValid(), "assert failed: view.IsValid()");
    auto complete_view = LeSetAdvertisingSetRandomAddressCompleteView::Create(view);
    log::assert_that(complete_view.IsValid(), "assert failed: complete_view.IsValid()");
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      log::error("Got a command complete with status {}", ErrorCodeText(complete_view.GetStatus()));
    } else {
      log::info("update random address for advertising set {} : {}", advertiser_id,
                address_with_type.GetAddress());
      advertising_sets_[advertiser_id].current_address = address_with_type;
    }
  }

  template <class View>
  void check_status_with_id(bool send_callback, AdvertiserId id, CommandCompleteView view) {
    log::assert_that(view.IsValid(), "assert failed: view.IsValid()");
    auto status_view = View::Create(view);
    log::assert_that(status_view.IsValid(), "assert failed: status_view.IsValid()");
    if (status_view.GetStatus() != ErrorCode::SUCCESS) {
      log::info("Got a Command complete {}, status {}", OpCodeText(view.GetCommandOpCode()),
                ErrorCodeText(status_view.GetStatus()));
    }
    AdvertisingCallback::AdvertisingStatus advertising_status =
            AdvertisingCallback::AdvertisingStatus::SUCCESS;
    if (status_view.GetStatus() != ErrorCode::SUCCESS) {
      log::info("Got a command complete with status {}", ErrorCodeText(status_view.GetStatus()));
      advertising_status = AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR;
    }

    // Do not trigger callback if the advertiser not stated yet, or the advertiser is not register
    // from Java layer
    if (advertising_callbacks_ == nullptr || !advertising_sets_[id].started ||
        id_map_[id] == kIdLocal) {
      return;
    }

    // Do not trigger callback if send_callback is false
    if (!send_callback) {
      return;
    }

    OpCode opcode = view.GetCommandOpCode();

    switch (opcode) {
      case OpCode::LE_SET_ADVERTISING_PARAMETERS:
        advertising_callbacks_->OnAdvertisingParametersUpdated(id, le_physical_channel_tx_power_,
                                                               advertising_status);
        break;
      case OpCode::LE_SET_ADVERTISING_DATA:
      case OpCode::LE_SET_EXTENDED_ADVERTISING_DATA:
        advertising_callbacks_->OnAdvertisingDataSet(id, advertising_status);
        break;
      case OpCode::LE_SET_SCAN_RESPONSE_DATA:
      case OpCode::LE_SET_EXTENDED_SCAN_RESPONSE_DATA:
        advertising_callbacks_->OnScanResponseDataSet(id, advertising_status);
        break;
      case OpCode::LE_SET_PERIODIC_ADVERTISING_PARAMETERS:
        advertising_callbacks_->OnPeriodicAdvertisingParametersUpdated(id, advertising_status);
        break;
      case OpCode::LE_SET_PERIODIC_ADVERTISING_DATA:
        advertising_callbacks_->OnPeriodicAdvertisingDataSet(id, advertising_status);
        break;
      case OpCode::LE_MULTI_ADVT: {
        auto command_view = LeMultiAdvtCompleteView::Create(view);
        log::assert_that(command_view.IsValid(), "assert failed: command_view.IsValid()");
        auto sub_opcode = command_view.GetSubCmd();
        switch (sub_opcode) {
          case SubOcf::SET_PARAM:
            advertising_callbacks_->OnAdvertisingParametersUpdated(
                    id, le_physical_channel_tx_power_, advertising_status);
            break;
          case SubOcf::SET_DATA:
            advertising_callbacks_->OnAdvertisingDataSet(id, advertising_status);
            break;
          case SubOcf::SET_SCAN_RESP:
            advertising_callbacks_->OnScanResponseDataSet(id, advertising_status);
            break;
          default:
            log::warn("Unexpected sub event type {}", SubOcfText(command_view.GetSubCmd()));
        }
      } break;
      default:
        log::warn("Unexpected event type {}", OpCodeText(view.GetCommandOpCode()));
    }
  }

  void start_advertising_fail(int reg_id, AdvertisingCallback::AdvertisingStatus status) {
    log::assert_that(status != AdvertisingCallback::AdvertisingStatus::SUCCESS,
                     "assert failed: status != AdvertisingCallback::AdvertisingStatus::SUCCESS");
    advertising_callbacks_->OnAdvertisingSetStarted(reg_id, kInvalidId, 0, status);
  }
};

LeAdvertisingManager::LeAdvertisingManager() { pimpl_ = std::make_unique<impl>(this); }

void LeAdvertisingManager::ListDependencies(ModuleList* list) const {
  list->add<hci::HciLayer>();
  list->add<hci::Controller>();
  list->add<hci::AclManager>();
}

void LeAdvertisingManager::Start() {
  pimpl_->start(GetHandler(), GetDependency<hci::HciLayer>(), GetDependency<hci::Controller>(),
                GetDependency<AclManager>());
}

void LeAdvertisingManager::Stop() { pimpl_.reset(); }

std::string LeAdvertisingManager::ToString() const { return "Le Advertising Manager"; }

size_t LeAdvertisingManager::GetNumberOfAdvertisingInstances() const {
  return pimpl_->GetNumberOfAdvertisingInstances();
}

size_t LeAdvertisingManager::GetNumberOfAdvertisingInstancesInUse() const {
  return pimpl_->GetNumberOfAdvertisingInstancesInUse();
}

int LeAdvertisingManager::GetAdvertiserRegId(AdvertiserId advertiser_id) {
  return pimpl_->get_advertiser_reg_id(advertiser_id);
}

void LeAdvertisingManager::ExtendedCreateAdvertiser(
        uint8_t client_id, int reg_id, const AdvertisingConfig config,
        common::Callback<void(Address, AddressType)> scan_callback,
        common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback,
        uint16_t duration, uint8_t max_extended_advertising_events, os::Handler* handler) {
  AdvertisingApiType advertising_api_type = pimpl_->get_advertising_api_type();
  if (advertising_api_type != AdvertisingApiType::EXTENDED) {
    if (config.peer_address == Address::kEmpty) {
      if (config.advertising_type == hci::AdvertisingType::ADV_DIRECT_IND_HIGH ||
          config.advertising_type == hci::AdvertisingType::ADV_DIRECT_IND_LOW) {
        log::warn("Peer address can not be empty for directed advertising");
        CallOn(pimpl_.get(), &impl::start_advertising_fail, reg_id,
               AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
        return;
      }
    }
    GetHandler()->Post(common::BindOnce(&impl::create_advertiser, common::Unretained(pimpl_.get()),
                                        reg_id, config, scan_callback, set_terminated_callback,
                                        handler));

    return;
  };

  if (config.directed) {
    if (config.peer_address == Address::kEmpty) {
      log::info("Peer address can not be empty for directed advertising");
      CallOn(pimpl_.get(), &impl::start_advertising_fail, reg_id,
             AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
      return;
    }
  }
  if (config.channel_map == 0) {
    log::info("At least one channel must be set in the map");
    CallOn(pimpl_.get(), &impl::start_advertising_fail, reg_id,
           AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
    return;
  }
  if (!config.legacy_pdus) {
    if (config.connectable && config.scannable) {
      log::info("Extended advertising PDUs can not be connectable and scannable");
      CallOn(pimpl_.get(), &impl::start_advertising_fail, reg_id,
             AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
      return;
    }
    if (config.high_duty_cycle) {
      log::info("Extended advertising PDUs can not be high duty cycle");
      CallOn(pimpl_.get(), &impl::start_advertising_fail, reg_id,
             AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
      return;
    }
  }
  if (config.interval_min > config.interval_max) {
    log::info("Advertising interval: min ({}) > max ({})", config.interval_min,
              config.interval_max);
    CallOn(pimpl_.get(), &impl::start_advertising_fail, reg_id,
           AdvertisingCallback::AdvertisingStatus::INTERNAL_ERROR);
    return;
  }
  CallOn(pimpl_.get(), &impl::create_extended_advertiser, client_id, reg_id, config, scan_callback,
         set_terminated_callback, duration, max_extended_advertising_events, handler);
  return;
}

void LeAdvertisingManager::StartAdvertising(
        AdvertiserId advertiser_id, const AdvertisingConfig config, uint16_t duration,
        base::OnceCallback<void(uint8_t /* status */)> status_callback,
        base::OnceCallback<void(uint8_t /* status */)> timeout_callback,
        common::Callback<void(Address, AddressType)> scan_callback,
        common::Callback<void(ErrorCode, uint8_t, uint8_t)> set_terminated_callback,
        os::Handler* handler) {
  CallOn(pimpl_.get(), &impl::start_advertising, advertiser_id, config, duration,
         std::move(status_callback), std::move(timeout_callback), scan_callback,
         set_terminated_callback, handler);
}

void LeAdvertisingManager::RegisterAdvertiser(
        common::ContextualOnceCallback<void(uint8_t /* inst_id */,
                                            AdvertisingCallback::AdvertisingStatus /* status */)>
                callback) {
  CallOn(pimpl_.get(), &impl::register_advertiser, std::move(callback));
}

void LeAdvertisingManager::GetOwnAddress(uint8_t advertiser_id) {
  CallOn(pimpl_.get(), &impl::get_own_address, advertiser_id);
}

void LeAdvertisingManager::SetParameters(AdvertiserId advertiser_id, AdvertisingConfig config) {
  CallOn(pimpl_.get(), &impl::set_parameters, advertiser_id, config);
}

void LeAdvertisingManager::SetData(AdvertiserId advertiser_id, bool set_scan_rsp,
                                   std::vector<GapData> data) {
  CallOn(pimpl_.get(), &impl::set_data, advertiser_id, set_scan_rsp, data);
}

void LeAdvertisingManager::EnableAdvertiser(AdvertiserId advertiser_id, bool enable,
                                            uint16_t duration,
                                            uint8_t max_extended_advertising_events) {
  CallOn(pimpl_.get(), &impl::enable_advertiser, advertiser_id, enable, duration,
         max_extended_advertising_events);
}

void LeAdvertisingManager::SetPeriodicParameters(
        AdvertiserId advertiser_id, PeriodicAdvertisingParameters periodic_advertising_parameters) {
  CallOn(pimpl_.get(), &impl::set_periodic_parameter, advertiser_id,
         periodic_advertising_parameters);
}

void LeAdvertisingManager::SetPeriodicData(AdvertiserId advertiser_id, std::vector<GapData> data) {
  CallOn(pimpl_.get(), &impl::set_periodic_data, advertiser_id, data);
}

void LeAdvertisingManager::EnablePeriodicAdvertising(AdvertiserId advertiser_id, bool enable,
                                                     bool include_adi) {
  CallOn(pimpl_.get(), &impl::enable_periodic_advertising, advertiser_id, enable, include_adi);
}

void LeAdvertisingManager::RemoveAdvertiser(AdvertiserId advertiser_id) {
  CallOn(pimpl_.get(), &impl::remove_advertiser, advertiser_id);
}

void LeAdvertisingManager::RegisterAdvertisingCallback(AdvertisingCallback* advertising_callback) {
  CallOn(pimpl_.get(), &impl::register_advertising_callback, advertising_callback);
}

}  // namespace hci
}  // namespace bluetooth
