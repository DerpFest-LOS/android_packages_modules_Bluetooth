/*
 * Copyright 2021 HIMSA II K/S - www.himsa.com.
 * Represented by EHIMA - www.ehima.com
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

#include <aics/api.h>
#include <base/functional/bind.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <bluetooth/log.h>
#include <hardware/bt_gatt_types.h>
#include <hardware/bt_vc.h>
#include <stdio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "bta/include/bta_csis_api.h"
#include "bta/include/bta_gatt_api.h"
#include "bta/include/bta_gatt_queue.h"
#include "bta/include/bta_vc_api.h"
#include "bta/le_audio/le_audio_types.h"
#include "bta/vc/devices.h"
#include "bta_groups.h"
#include "btm_ble_api_types.h"
#include "gatt/database.h"
#include "gatt_api.h"
#include "osi/include/alarm.h"
#include "osi/include/osi.h"
#include "stack/btm/btm_sec.h"
#include "stack/include/bt_types.h"
#include "stack/include/btm_status.h"
#include "types/bluetooth/uuid.h"
#include "types/bt_transport.h"
#include "types/raw_address.h"
#include "vc/types.h"

using base::Closure;
using bluetooth::csis::CsisClient;
using bluetooth::vc::ConnectionState;
using bluetooth::vc::VolumeInputStatus;
using bluetooth::vc::VolumeInputType;
using bluetooth::vc::internal::kControlPointOpcodeMute;
using bluetooth::vc::internal::kControlPointOpcodeSetAbsoluteVolume;
using bluetooth::vc::internal::kControlPointOpcodeUnmute;
using bluetooth::vc::internal::kControlPointOpcodeVolumeDown;
using bluetooth::vc::internal::kControlPointOpcodeVolumeUp;
using bluetooth::vc::internal::kVolumeControlUuid;
using bluetooth::vc::internal::kVolumeInputControlPointOpcodeMute;
using bluetooth::vc::internal::kVolumeInputControlPointOpcodeSetAutoGainMode;
using bluetooth::vc::internal::kVolumeInputControlPointOpcodeSetGain;
using bluetooth::vc::internal::kVolumeInputControlPointOpcodeSetManualGainMode;
using bluetooth::vc::internal::kVolumeInputControlPointOpcodeUnmute;
using bluetooth::vc::internal::kVolumeOffsetControlPointOpcodeSet;
using bluetooth::vc::internal::VolumeAudioInput;
using bluetooth::vc::internal::VolumeControlDevice;
using bluetooth::vc::internal::VolumeControlDevices;
using bluetooth::vc::internal::VolumeOffset;
using bluetooth::vc::internal::VolumeOperation;

namespace {
class VolumeControlImpl;
VolumeControlImpl* instance;
std::mutex instance_mutex;

/**
 * Overview:
 *
 * This is Volume Control Implementation class which realize Volume Control
 * Profile (VCP)
 *
 * Each connected peer device supporting Volume Control Service (VCS) is on the
 * list of devices (volume_control_devices_). When VCS is discovered on the peer
 * device, Android does search for all the instances Volume Offset Service
 * (VOCS). Note that AIS and VOCS are optional.
 *
 * Once all the mandatory characteristis for all the services are discovered,
 * Fluoride calls ON_CONNECTED callback.
 *
 * It is assumed that whenever application changes general audio options in this
 * profile e.g. Volume up/down, mute/unmute etc, profile configures all the
 * devices which are active Le Audio devices.
 *
 * Peer devices has at maximum one instance of VCS and 0 or more instance of
 * VOCS. Android gets access to External Audio Outputs using appropriate ID.
 * Also each of the External Device has description
 * characteristic and Type which gives the application hint what it is a device.
 * Examples of such devices:
 *   External Output: 1 instance to controller balance between set of devices
 *   External Output: each of 5.1 speaker set etc.
 */
class VolumeControlImpl : public VolumeControl {
public:
  ~VolumeControlImpl() override = default;

  VolumeControlImpl(bluetooth::vc::VolumeControlCallbacks* callbacks, const base::Closure& initCb)
      : gatt_if_(0), callbacks_(callbacks), latest_operation_id_(0) {
    BTA_GATTC_AppRegister(
            gattc_callback_static,
            base::Bind(
                    [](const base::Closure& initCb, uint8_t client_id, uint8_t status) {
                      if (status != GATT_SUCCESS) {
                        bluetooth::log::error(
                                "Can't start Volume Control profile - no gatt clients "
                                "left!");
                        return;
                      }
                      instance->gatt_if_ = client_id;
                      initCb.Run();
                    },
                    initCb),
            true);
  }

  void StartOpportunisticConnect(const RawAddress& address) {
    /* Oportunistic works only for direct connect,
     * but in fact this is background connect
     */
    bluetooth::log::info(": {}", address);
    BTA_GATTC_Open(gatt_if_, address, BTM_BLE_DIRECT_CONNECTION, true);
  }

  void Connect(const RawAddress& address) override {
    bluetooth::log::info(": {}", address);

    auto device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      if (!BTM_IsLinkKeyKnown(address, BT_TRANSPORT_LE)) {
        bluetooth::log::error("Connecting  {} when not bonded", address);
        callbacks_->OnConnectionState(ConnectionState::DISCONNECTED, address);
        return;
      }
      volume_control_devices_.Add(address, true);
    } else {
      device->connecting_actively = true;

      if (device->IsConnected()) {
        bluetooth::log::warn("address={}, connection_id={} already connected.", address,
                             device->connection_id);

        if (device->IsReady()) {
          callbacks_->OnConnectionState(ConnectionState::CONNECTED, device->address);
        } else {
          OnGattConnected(GATT_SUCCESS, device->connection_id, gatt_if_, device->address,
                          BT_TRANSPORT_LE, GATT_MAX_MTU_SIZE);
        }
        return;
      }
    }

    StartOpportunisticConnect(address);
  }

  void AddFromStorage(const RawAddress& address) {
    bluetooth::log::info("{}", address);
    volume_control_devices_.Add(address, false);
    StartOpportunisticConnect(address);
  }

  void OnGattConnected(tGATT_STATUS status, tCONN_ID connection_id, tGATT_IF /*client_if*/,
                       RawAddress address, tBT_TRANSPORT transport, uint16_t /*mtu*/) {
    bluetooth::log::info("{}, conn_id=0x{:04x}, transport={}, status={}(0x{:02x})", address,
                         connection_id, bt_transport_text(transport), gatt_status_text(status),
                         status);

    if (transport != BT_TRANSPORT_LE) {
      bluetooth::log::warn("Only LE connection is allowed (transport {})",
                           bt_transport_text(transport));
      BTA_GATTC_Close(connection_id);
      return;
    }

    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("Skipping unknown device, address={}", address);
      return;
    }

    if (status != GATT_SUCCESS) {
      bluetooth::log::info("Failed to connect to Volume Control device");
      device_cleanup_helper(device, device->connecting_actively);
      StartOpportunisticConnect(address);
      return;
    }

    device->connection_id = connection_id;

    /* Make sure to remove device from background connect.
     * It will be added back if needed, when device got disconnected
     */
    BTA_GATTC_CancelOpen(gatt_if_, address, true);

    if (device->IsEncryptionEnabled()) {
      OnEncryptionComplete(address, tBTM_STATUS::BTM_SUCCESS);
      return;
    }

    if (!device->EnableEncryption()) {
      bluetooth::log::error("Link key is not known for {}, disconnect profile", address);
      device->Disconnect(gatt_if_);
    }
  }

  void OnEncryptionComplete(const RawAddress& address, tBTM_STATUS success) {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("Skipping unknown device {}", address);
      return;
    }

    if (success != tBTM_STATUS::BTM_SUCCESS) {
      bluetooth::log::error("encryption failed status: {}", btm_status_text(success));
      // If the encryption failed, do not remove the device.
      // Disconnect only, since the Android will try to re-enable encryption
      // after disconnection
      device_cleanup_helper(device, device->connecting_actively);
      return;
    }

    bluetooth::log::info("{} status: {}", address, success);

    if (device->HasHandles()) {
      device->EnqueueInitialRequests(gatt_if_, chrc_read_callback_static, OnGattWriteCccStatic);

    } else {
      BTA_GATTC_ServiceSearchRequest(device->connection_id, kVolumeControlUuid);
    }
  }

  void ClearDeviceInformationAndStartSearch(VolumeControlDevice* device) {
    if (!device) {
      bluetooth::log::error("Device is null");
      return;
    }

    bluetooth::log::info("address={}", device->address);
    if (device->known_service_handles_ == false) {
      bluetooth::log::info("Device already is waiting for new services");
      return;
    }

    std::vector<RawAddress> devices = {device->address};
    device->DeregisterNotifications(gatt_if_);

    RemovePendingVolumeControlOperations(devices, bluetooth::groups::kGroupUnknown);
    device->ResetHandles();
    BTA_GATTC_ServiceSearchRequest(device->connection_id, kVolumeControlUuid);
  }

  void OnServiceChangeEvent(const RawAddress& address) {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("Skipping unknown device {}", address);
      return;
    }

    ClearDeviceInformationAndStartSearch(device);
  }

  void OnServiceDiscDoneEvent(const RawAddress& address) {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("Skipping unknown device {}", address);
      return;
    }

    if (device->known_service_handles_ == false) {
      BTA_GATTC_ServiceSearchRequest(device->connection_id, kVolumeControlUuid);
    }
  }

  void OnServiceSearchComplete(tCONN_ID connection_id, tGATT_STATUS status) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      bluetooth::log::error("Skipping unknown device, connection_id={:#x}", connection_id);
      return;
    }

    /* Known device, nothing to do */
    if (device->IsReady()) {
      return;
    }

    if (status != GATT_SUCCESS) {
      /* close connection and report service discovery complete with error */
      bluetooth::log::error("Service discovery failed");
      device_cleanup_helper(device, device->connecting_actively);
      return;
    }

    if (!device->IsEncryptionEnabled()) {
      bluetooth::log::warn("Device not yet bonded - waiting for encryption");
      return;
    }

    bool success = device->UpdateHandles();
    if (!success) {
      bluetooth::log::error("Incomplete service database");
      device_cleanup_helper(device, device->connecting_actively);
      return;
    }

    device->EnqueueInitialRequests(gatt_if_, chrc_read_callback_static, OnGattWriteCccStatic);
  }

  void OnCharacteristicValueChanged(tCONN_ID conn_id, tGATT_STATUS status, uint16_t handle,
                                    uint16_t len, uint8_t* value, void* /*data*/,
                                    bool is_notification) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(conn_id);
    if (!device) {
      bluetooth::log::error("unknown conn_id={:#x}", conn_id);
      return;
    }

    if (status != GATT_SUCCESS) {
      bluetooth::log::info("status=0x{:02x}", static_cast<int>(status));
      if (status == GATT_DATABASE_OUT_OF_SYNC) {
        bluetooth::log::info("Database out of sync for {}", device->address);
        ClearDeviceInformationAndStartSearch(device);
      }
      return;
    }

    if (handle == device->volume_state_handle) {
      OnVolumeControlStateReadOrNotified(device, len, value, is_notification);
      verify_device_ready(device, handle);
      return;
    }
    if (handle == device->volume_flags_handle) {
      OnVolumeControlFlagsChanged(device, len, value);
      verify_device_ready(device, handle);
      return;
    }

    const gatt::Service* service = BTA_GATTC_GetOwningService(conn_id, handle);
    if (service == nullptr) {
      return;
    }

    VolumeAudioInput* input = device->audio_inputs.FindByServiceHandle(service->handle);
    if (input != nullptr) {
      if (handle == input->state_handle) {
        OnExtAudioInputStateChanged(device, input, len, value);
      } else if (handle == input->type_handle) {
        OnExtAudioInTypeChanged(device, input, len, value);
      } else if (handle == input->status_handle) {
        OnExtAudioInputStatusChanged(device, input, len, value);
      } else if (handle == input->description_handle) {
        OnExtAudioInDescChanged(device, input, len, value);
      } else if (handle == input->gain_setting_handle) {
        OnExtAudioInGainSettingChanged(device, input, len, value);
      } else {
        bluetooth::log::error("{} unknown input handle={:#x}", device->address, handle);
        return;
      }

      verify_device_ready(device, handle);
      return;
    }

    VolumeOffset* offset = device->audio_offsets.FindByServiceHandle(service->handle);
    if (offset != nullptr) {
      if (handle == offset->state_handle) {
        OnExtAudioOutStateChanged(device, offset, len, value);
      } else if (handle == offset->audio_location_handle) {
        OnExtAudioOutLocationChanged(device, offset, len, value);
      } else if (handle == offset->audio_descr_handle) {
        OnOffsetOutputDescChanged(device, offset, len, value);
      } else {
        bluetooth::log::error("{} unknown offset handle={:#x}", device->address, handle);
        return;
      }

      verify_device_ready(device, handle);
      return;
    }

    bluetooth::log::error("{}, unknown handle={:#x}", device->address, handle);
  }

  void OnNotificationEvent(tCONN_ID conn_id, uint16_t handle, uint16_t len, uint8_t* value) {
    bluetooth::log::info("connection_id={:#x}, handle={:#x}", conn_id, handle);
    OnCharacteristicValueChanged(conn_id, GATT_SUCCESS, handle, len, value, nullptr, true);
  }

  void VolumeControlReadCommon(tCONN_ID conn_id, uint16_t handle) {
    BtaGattQueue::ReadCharacteristic(conn_id, handle, chrc_read_callback_static, nullptr);
  }

  void HandleAutonomusVolumeChange(VolumeControlDevice* device, bool is_volume_change,
                                   bool is_mute_change) {
    bluetooth::log::debug("{}, is volume change: {}, is mute change: {}", device->address,
                          is_volume_change, is_mute_change);

    if (!is_volume_change && !is_mute_change) {
      bluetooth::log::error("Autonomous change but volume and mute did not changed.");
      return;
    }

    auto csis_api = CsisClient::Get();
    if (!csis_api) {
      bluetooth::log::warn("Csis module is not available");
      callbacks_->OnVolumeStateChanged(device->address, device->volume, device->mute, device->flags,
                                       true);
      return;
    }

    auto group_id =
            csis_api->GetGroupId(device->address, bluetooth::le_audio::uuid::kCapServiceUuid);
    if (group_id == bluetooth::groups::kGroupUnknown) {
      bluetooth::log::warn("No group for device {}", device->address);
      callbacks_->OnVolumeStateChanged(device->address, device->volume, device->mute, device->flags,
                                       true);
      return;
    }

    auto devices = csis_api->GetDeviceList(group_id);
    for (auto it = devices.begin(); it != devices.end();) {
      auto dev = volume_control_devices_.FindByAddress(*it);
      if (!dev || !dev->IsConnected() || (dev->address == device->address)) {
        it = devices.erase(it);
      } else {
        it++;
      }
    }

    if (devices.empty() && (is_volume_change || is_mute_change)) {
      bluetooth::log::info("No more devices in the group right now");
      callbacks_->OnGroupVolumeStateChanged(group_id, device->volume, device->mute, true);
      return;
    }

    if (is_volume_change) {
      std::vector<uint8_t> arg({device->volume});
      PrepareVolumeControlOperation(devices, group_id, true, kControlPointOpcodeSetAbsoluteVolume,
                                    arg);
    }

    if (is_mute_change) {
      std::vector<uint8_t> arg;
      uint8_t opcode = device->mute ? kControlPointOpcodeMute : kControlPointOpcodeUnmute;
      PrepareVolumeControlOperation(devices, group_id, true, opcode, arg);
    }

    StartQueueOperation();
  }

  void OnVolumeControlStateReadOrNotified(VolumeControlDevice* device, uint16_t len, uint8_t* value,
                                          bool is_notification) {
    if (len != 3) {
      bluetooth::log::error("{}, malformed len={:#x}", device->address, len);
      return;
    }

    uint8_t vol;
    uint8_t mute;
    uint8_t* pp = value;
    STREAM_TO_UINT8(vol, pp);
    STREAM_TO_UINT8(mute, pp);
    STREAM_TO_UINT8(device->change_counter, pp);

    bool is_volume_change = (device->volume != vol);
    device->volume = vol;

    bool is_mute_change = (device->mute != mute);
    device->mute = mute;

    bluetooth::log::info("{}, volume {:#x} mute {:#x} change_counter {:#x}", device->address,
                         device->volume, device->mute, device->change_counter);

    if (!device->IsReady()) {
      bluetooth::log::info("Device: {} is not ready yet.", device->address);
      return;
    }

    /* This is just a read, send single notification */
    if (!is_notification) {
      callbacks_->OnVolumeStateChanged(device->address, device->volume, device->mute, device->flags,
                                       false);
      return;
    }

    auto addr = device->address;
    auto op = find_if(ongoing_operations_.begin(), ongoing_operations_.end(),
                      [addr](auto& operation) {
                        auto it = find(operation.devices_.begin(), operation.devices_.end(), addr);
                        return it != operation.devices_.end();
                      });
    if (op == ongoing_operations_.end()) {
      bluetooth::log::debug("Could not find operation id for device: {}. Autonomus change",
                            device->address);
      HandleAutonomusVolumeChange(device, is_volume_change, is_mute_change);
      return;
    }

    /* Received notification from the device we do expect */
    auto it = find(op->devices_.begin(), op->devices_.end(), device->address);
    op->devices_.erase(it);
    if (!op->devices_.empty()) {
      bluetooth::log::debug("wait for more responses for operation_id: {}", op->operation_id_);
      return;
    }

    if (op->IsGroupOperation()) {
      callbacks_->OnGroupVolumeStateChanged(op->group_id_, device->volume, device->mute,
                                            op->is_autonomous_);
    } else {
      /* op->is_autonomous_ will always be false,
         since we only make it true for group operations */
      callbacks_->OnVolumeStateChanged(device->address, device->volume, device->mute, device->flags,
                                       false);
    }

    ongoing_operations_.erase(op);
    StartQueueOperation();
  }

  void OnVolumeControlFlagsChanged(VolumeControlDevice* device, uint16_t /*len*/, uint8_t* value) {
    device->flags = *value;

    bluetooth::log::info("{}, flags {:#x}", device->address, device->flags);
  }

  void OnExtAudioOutStateChanged(VolumeControlDevice* device, VolumeOffset* offset, uint16_t len,
                                 uint8_t* value) {
    if (len != 3) {
      bluetooth::log::error("{}, id={:#x}, malformed len={:#x}", device->address, offset->id, len);
      return;
    }

    uint8_t* pp = value;
    STREAM_TO_UINT16(offset->offset, pp);
    STREAM_TO_UINT8(offset->change_counter, pp);

    bluetooth::log::verbose("{}, len:{}", device->address, base::HexEncode(value, len));
    bluetooth::log::info("{} id={:#x} offset: {:#x} counter: {:#x}", device->address, offset->id,
                         offset->offset, offset->change_counter);

    if (!device->IsReady()) {
      bluetooth::log::info("Device: {} is not ready yet.", device->address);
      return;
    }

    callbacks_->OnExtAudioOutVolumeOffsetChanged(device->address, offset->id, offset->offset);
  }

  void OnExtAudioOutLocationChanged(VolumeControlDevice* device, VolumeOffset* offset, uint16_t len,
                                    uint8_t* value) {
    if (len != 4) {
      bluetooth::log::error("{}, id={:#x}, malformed len={:#x}", device->address, offset->id, len);
      return;
    }

    uint8_t* pp = value;
    STREAM_TO_UINT32(offset->location, pp);

    bluetooth::log::verbose("{}, data :{}", device->address, base::HexEncode(value, len));
    bluetooth::log::info("{} id={:#x}, location={:#x}", device->address, offset->id,
                         offset->location);

    if (!device->IsReady()) {
      bluetooth::log::info("Device: {} is not ready yet.", device->address);
      return;
    }

    callbacks_->OnExtAudioOutLocationChanged(device->address, offset->id, offset->location);
  }

  void OnExtAudioInputStateChanged(VolumeControlDevice* device, VolumeAudioInput* input,
                                   uint16_t len, uint8_t* value) {
    if (len != 4) {
      bluetooth::log::error("{}, id={}, malformed len={:#x}", device->address, input->id, len);
      return;
    }

    uint8_t* pp = value;
    STREAM_TO_INT8(input->gain_setting, pp);
    uint8_t mute;
    STREAM_TO_UINT8(mute, pp);
    if (!bluetooth::aics::isValidAudioInputMuteValue(mute)) {
      bluetooth::log::error("{} Invalid mute value: {:#x}", device->address, mute);
      return;
    }
    input->mute = bluetooth::aics::parseMuteField(mute);

    uint8_t gain_mode;
    STREAM_TO_UINT8(gain_mode, pp);
    if (!bluetooth::aics::isValidAudioInputGainModeValue(gain_mode)) {
      bluetooth::log::error("{} Invalid GainMode value: {:#x}", device->address, gain_mode);
      return;
    }
    input->gain_mode = bluetooth::aics::parseGainModeField(gain_mode);
    STREAM_TO_UINT8(input->change_counter, pp);

    bluetooth::log::verbose("{}, data:{}", device->address, base::HexEncode(value, len));
    bluetooth::log::info(
            "{} id={:#x}gain_setting {:#x}, mute: {:#x}, mode: {:#x}, change_counter: {}",
            device->address, input->id, input->gain_setting, input->mute, input->gain_mode,
            input->change_counter);

    if (!device->device_ready) {
      return;
    }

    callbacks_->OnExtAudioInStateChanged(device->address, input->id, input->gain_setting,
                                         input->mute, input->gain_mode);
  }

  void OnExtAudioInTypeChanged(VolumeControlDevice* device, VolumeAudioInput* input, uint16_t len,
                               uint8_t* value) {
    if (len != 1) {
      bluetooth::log::error("{}, id={}, malformed len={:#x}", device->address, input->id, len);
      return;
    }

    if (*value >= static_cast<uint8_t>(VolumeInputType::RFU)) {
      bluetooth::log::error("Invalid type {} for {} id={}", device->address, *value, input->id);
      return;
    }

    input->type = static_cast<VolumeInputType>(*value);

    bluetooth::log::info("{}, id={:#x} type={}", device->address, input->id, input->type);

    if (!device->device_ready) {
      return;
    }

    callbacks_->OnExtAudioInTypeChanged(device->address, input->id, input->type);
  }

  void OnExtAudioInputStatusChanged(VolumeControlDevice* device, VolumeAudioInput* input,
                                    uint16_t len, uint8_t* value) {
    if (len != 1) {
      bluetooth::log::error("{}, id={}, malformed len={:#x}", device->address, input->id, len);
      return;
    }

    if (*value >= static_cast<uint8_t>(VolumeInputStatus::RFU)) {
      bluetooth::log::error("Invalid status {:#x} received from {} on id={:#x}", *value,
                            device->address, input->id);
      return;
    }

    input->status = static_cast<VolumeInputStatus>(*value);

    bluetooth::log::info("{}, id={:#x} status {}", device->address, input->id, input->status);

    if (!device->device_ready) {
      return;
    }

    callbacks_->OnExtAudioInStatusChanged(device->address, input->id, input->status);
  }

  void OnExtAudioInDescChanged(VolumeControlDevice* device, VolumeAudioInput* input, uint16_t len,
                               uint8_t* value) {
    std::string description = std::string(value, value + len);
    if (!base::IsStringUTF8(description)) {
      bluetooth::log::error("Received description is no utf8 string for {}, input id={:#x}",
                            device->address, input->id);
    } else {
      input->description = description;
    }

    bluetooth::log::info("{}, id={:#x}, descriptor: {}", device->address, input->id,
                         input->description);

    if (!device->device_ready) {
      return;
    }

    callbacks_->OnExtAudioInDescriptionChanged(device->address, input->id, input->description,
                                               input->description_writable);
  }

  void OnExtAudioInCPWrite(uint16_t connection_id, tGATT_STATUS status, uint16_t handle,
                           uint8_t opcode, uint8_t id) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      bluetooth::log::info("Skipping unknown device disconnect, connection_id={:#x}",
                           connection_id);
      return;
    }

    bluetooth::log::info("{}, Input Control Point write response handle {:#x}, status {:#x}",
                         device->address, handle, status);
    if (status == GATT_SUCCESS) {
      return;
    }

    switch (opcode) {
      case kVolumeInputControlPointOpcodeSetGain:
        callbacks_->OnExtAudioInSetGainSettingFailed(device->address, id);
        break;
      case kVolumeInputControlPointOpcodeMute:
      case kVolumeInputControlPointOpcodeUnmute:
        callbacks_->OnExtAudioInSetMuteFailed(device->address, id);
        break;
      case kVolumeInputControlPointOpcodeSetAutoGainMode:
      case kVolumeInputControlPointOpcodeSetManualGainMode:
        callbacks_->OnExtAudioInSetGainModeFailed(device->address, id);
        break;
      default:
        bluetooth::log::error("{} Not a valid opcode", opcode);
    }
  }

  void OnExtAudioInGainSettingChanged(VolumeControlDevice* device, VolumeAudioInput* input,
                                      uint16_t len, uint8_t* value) {
    if (len != 3) {
      bluetooth::log::error("{}, id={}, malformed len={:#x}", device->address, input->id, len);
      return;
    }

    uint8_t* pp = value;
    STREAM_TO_UINT8(input->gain_settings.unit, pp);
    STREAM_TO_INT8(input->gain_settings.min, pp);
    STREAM_TO_INT8(input->gain_settings.max, pp);

    bluetooth::log::verbose("{}, len:{}", device->address, base::HexEncode(value, len));
    bluetooth::log::info("{}, id={:#x} gain unit {:#x} gain min {:#x} gain max {:#x}",
                         device->address, input->id, input->gain_settings.unit,
                         input->gain_settings.min, input->gain_settings.max);

    if (!device->device_ready) {
      return;
    }

    callbacks_->OnExtAudioInGainSettingPropertiesChanged(
            device->address, input->id, input->gain_settings.unit, input->gain_settings.min,
            input->gain_settings.max);
  }

  void OnExtAudioOutCPWrite(tCONN_ID connection_id, tGATT_STATUS status, uint16_t handle,
                            void* /*data*/) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      bluetooth::log::error("Skipping unknown device disconnect, connection_id={:#x}",
                            connection_id);
      return;
    }

    bluetooth::log::info("Offset Control Point write response handle{:#x} status: {:#x}", handle,
                         static_cast<int>(status));

    /* TODO Design callback API to notify about changes */
  }

  void OnOffsetOutputDescChanged(VolumeControlDevice* device, VolumeOffset* offset, uint16_t len,
                                 uint8_t* value) {
    std::string description = std::string(value, value + len);
    if (!base::IsStringUTF8(description)) {
      bluetooth::log::error(" Received description is no utf8 string for {}, offset id={:#x}",
                            device->address, offset->id);
    } else {
      offset->description = description;
    }

    bluetooth::log::info("{}, {}", device->address, description);

    if (!device->IsReady()) {
      bluetooth::log::info("Device: {} is not ready yet.", device->address);
      return;
    }

    callbacks_->OnExtAudioOutDescriptionChanged(device->address, offset->id, offset->description);
  }

  void OnGattWriteCcc(tCONN_ID connection_id, tGATT_STATUS status, uint16_t handle,
                      uint16_t /*len*/, const uint8_t* /*value*/, void* /*data*/) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      bluetooth::log::error("unknown connection_id={:#x}", connection_id);
      BtaGattQueue::Clean(connection_id);
      return;
    }

    if (status != GATT_SUCCESS) {
      if (status == GATT_DATABASE_OUT_OF_SYNC) {
        bluetooth::log::info("Database out of sync for {}, conn_id: 0x{:04x}", device->address,
                             connection_id);
        ClearDeviceInformationAndStartSearch(device);
      } else {
        bluetooth::log::error("Failed to register for notification: 0x{:04x}, status 0x{:02x}",
                              handle, status);
        device_cleanup_helper(device, true);
      }
      return;
    }

    bluetooth::log::info("Successfully registered on ccc: 0x{:04x}, device: {}", handle,
                         device->address);

    verify_device_ready(device, handle);
  }

  static void OnGattWriteCccStatic(tCONN_ID connection_id, tGATT_STATUS status, uint16_t handle,
                                   uint16_t len, const uint8_t* value, void* data) {
    if (!instance) {
      bluetooth::log::error("connection_id={:#x}, no instance. Handle to write={:#x}",
                            connection_id, handle);
      return;
    }

    instance->OnGattWriteCcc(connection_id, status, handle, len, value, data);
  }

  void Dump(int fd) {
    dprintf(fd, "APP ID: %d\n", gatt_if_);
    volume_control_devices_.DebugDump(fd);
  }

  void Disconnect(const RawAddress& address) override {
    bluetooth::log::info("{}", address);

    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::warn("Device not connected to profile {}", address);
      callbacks_->OnConnectionState(ConnectionState::DISCONNECTED, address);
      return;
    }

    bluetooth::log::info("GAP_EVT_CONN_CLOSED: {}", device->address);
    device->connecting_actively = false;
    device_cleanup_helper(device, true);
  }

  void Remove(const RawAddress& address) override {
    bluetooth::log::info("{}", address);

    /* Removes all registrations for connection. */
    BTA_GATTC_CancelOpen(gatt_if_, address, true);

    Disconnect(address);
    volume_control_devices_.Remove(address);
  }

  void OnGattDisconnected(tCONN_ID connection_id, tGATT_IF /*client_if*/, RawAddress remote_bda,
                          tGATT_DISCONN_REASON /*reason*/) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      bluetooth::log::error("Skipping unknown device disconnect, connection_id={:#x}",
                            connection_id);
      return;
    }

    if (!device->IsConnected()) {
      bluetooth::log::error(
              "Skipping disconnect of the already disconnected device, "
              "connection_id={:#x}",
              connection_id);
      return;
    }

    bluetooth::log::info("{}", remote_bda);

    bool notify = device->IsReady() || device->connecting_actively;
    device_cleanup_helper(device, notify);

    StartOpportunisticConnect(remote_bda);
  }

  void RemoveDeviceFromOperationList(const RawAddress& addr) {
    if (ongoing_operations_.empty()) {
      return;
    }

    for (auto& op : ongoing_operations_) {
      auto it = find(op.devices_.begin(), op.devices_.end(), addr);
      if (it == op.devices_.end()) {
        continue;
      }
      op.devices_.erase(it);
    }

    // Remove operations with no devices
    auto it = ongoing_operations_.begin();
    while (it != ongoing_operations_.end()) {
      if (it->devices_.empty()) {
        it = ongoing_operations_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void RemoveDeviceFromOperationList(const RawAddress& addr, int operation_id) {
    auto op = find_if(
            ongoing_operations_.begin(), ongoing_operations_.end(),
            [operation_id](auto& operation) { return operation.operation_id_ == operation_id; });

    if (op == ongoing_operations_.end()) {
      bluetooth::log::error("Could not find operation id: {}", operation_id);
      return;
    }

    auto it = find(op->devices_.begin(), op->devices_.end(), addr);
    if (it != op->devices_.end()) {
      op->devices_.erase(it);
      if (op->devices_.empty()) {
        ongoing_operations_.erase(op);
        StartQueueOperation();
      }
      return;
    }
  }

  void RemovePendingVolumeControlOperations(const std::vector<RawAddress>& devices, int group_id) {
    bluetooth::log::debug("");
    for (auto op = ongoing_operations_.begin(); op != ongoing_operations_.end();) {
      // We only remove operations that don't affect the mute field.
      if (op->IsStarted() || (op->opcode_ != kControlPointOpcodeSetAbsoluteVolume &&
                              op->opcode_ != kControlPointOpcodeVolumeUp &&
                              op->opcode_ != kControlPointOpcodeVolumeDown)) {
        op++;
        continue;
      }
      if (group_id != bluetooth::groups::kGroupUnknown && op->group_id_ == group_id) {
        bluetooth::log::debug("Removing operation {}", op->operation_id_);
        op = ongoing_operations_.erase(op);
        continue;
      }
      for (auto const& addr : devices) {
        auto it = find(op->devices_.begin(), op->devices_.end(), addr);
        if (it != op->devices_.end()) {
          bluetooth::log::debug("Removing {} from operation", *it);
          op->devices_.erase(it);
        }
      }
      if (op->devices_.empty()) {
        op = ongoing_operations_.erase(op);
        bluetooth::log::debug("Removing operation {}", op->operation_id_);
      } else {
        op++;
      }
    }
  }

  void OnWriteControlResponse(tCONN_ID connection_id, tGATT_STATUS status, uint16_t handle,
                              void* data) {
    VolumeControlDevice* device = volume_control_devices_.FindByConnId(connection_id);
    if (!device) {
      bluetooth::log::error("Skipping unknown device disconnect, connection_id={:#x}",
                            connection_id);
      return;
    }

    bluetooth::log::info("Write response handle: {:#x} status: {:#x}", handle,
                         static_cast<int>(status));

    if (status == GATT_SUCCESS) {
      return;
    }

    /* In case of error, remove device from the tracking operation list */
    RemoveDeviceFromOperationList(device->address, PTR_TO_INT(data));

    if (status == GATT_DATABASE_OUT_OF_SYNC) {
      bluetooth::log::info("Database out of sync for {}", device->address);
      ClearDeviceInformationAndStartSearch(device);
    }
  }

  static void operation_timeout_callback(void* data) {
    if (!instance) {
      bluetooth::log::warn("There is no instance.");
      return;
    }
    instance->OperationMonitorTimeoutFired(PTR_TO_INT(data));
  }

  void OperationMonitorTimeoutFired(int operation_id) {
    auto op = find_if(ongoing_operations_.begin(), ongoing_operations_.end(),
                      [operation_id](auto& it) { return it.operation_id_ == operation_id; });

    if (op == ongoing_operations_.end()) {
      bluetooth::log::error("Could not find operation_id: {}", operation_id);
      return;
    }

    bluetooth::log::warn("Operation {} is taking too long for devices:", operation_id);
    for (const auto& addr : op->devices_) {
      bluetooth::log::warn("{},", addr);
    }
    alarm_set_on_mloop(op->operation_timeout_, kOperationMonitorTimeoutMs,
                       operation_timeout_callback, INT_TO_PTR(operation_id));
  }

  void StartQueueOperation(void) {
    bluetooth::log::info("");
    if (ongoing_operations_.empty()) {
      return;
    }

    auto op = &ongoing_operations_.front();

    bluetooth::log::info("Current operation_id: {}", op->operation_id_);

    if (op->IsStarted()) {
      bluetooth::log::info("Operation {} is started, wait until it is complete", op->operation_id_);
      return;
    }

    op->Start();

    alarm_set_on_mloop(op->operation_timeout_, kOperationMonitorTimeoutMs,
                       operation_timeout_callback, INT_TO_PTR(op->operation_id_));
    devices_control_point_helper(op->devices_, op->opcode_,
                                 op->arguments_.size() == 0 ? nullptr : &(op->arguments_),
                                 op->operation_id_);
  }

  void PrepareVolumeControlOperation(std::vector<RawAddress> devices, int group_id,
                                     bool is_autonomous, uint8_t opcode,
                                     const std::vector<uint8_t>& arguments) {
    bluetooth::log::debug(
            "num of devices: {}, group_id: {}, is_autonomous: {}  opcode: {}, arg "
            "size: {}",
            devices.size(), group_id, is_autonomous, opcode, arguments.size());

    if (std::find_if(
                ongoing_operations_.begin(), ongoing_operations_.end(),
                [opcode, &devices, &arguments](const VolumeOperation& op) {
                  if (op.opcode_ != opcode) {
                    return false;
                  }
                  if (!std::equal(op.arguments_.begin(), op.arguments_.end(), arguments.begin())) {
                    return false;
                  }
                  // Filter out all devices which have the exact operation
                  // already scheduled
                  devices.erase(std::remove_if(devices.begin(), devices.end(),
                                               [&op](auto d) {
                                                 return find(op.devices_.begin(), op.devices_.end(),
                                                             d) != op.devices_.end();
                                               }),
                                devices.end());
                  return devices.empty();
                }) == ongoing_operations_.end()) {
      ongoing_operations_.emplace_back(latest_operation_id_++, group_id, is_autonomous, opcode,
                                       arguments, devices);
    }
  }

  void MuteUnmute(std::variant<RawAddress, int> addr_or_group_id, bool mute) {
    std::vector<uint8_t> arg;

    uint8_t opcode = mute ? kControlPointOpcodeMute : kControlPointOpcodeUnmute;

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      VolumeControlDevice* dev =
              volume_control_devices_.FindByAddress(std::get<RawAddress>(addr_or_group_id));
      if (dev != nullptr) {
        bluetooth::log::debug("Address: {}: isReady: {}", dev->address, dev->IsReady());
        if (dev->IsReady() && (dev->mute != mute)) {
          std::vector<RawAddress> devices = {dev->address};
          PrepareVolumeControlOperation(devices, bluetooth::groups::kGroupUnknown, false, opcode,
                                        arg);
        }
      }
    } else {
      /* Handle group change */
      auto group_id = std::get<int>(addr_or_group_id);
      bluetooth::log::debug("group: {}", group_id);
      auto csis_api = CsisClient::Get();
      if (!csis_api) {
        bluetooth::log::error("Csis is not there");
        return;
      }

      auto devices = csis_api->GetDeviceList(group_id);
      if (devices.empty()) {
        bluetooth::log::error("group id: {} has no devices", group_id);
        return;
      }

      bool muteNotChanged = false;
      bool deviceNotReady = false;

      for (auto it = devices.begin(); it != devices.end();) {
        auto dev = volume_control_devices_.FindByAddress(*it);
        if (!dev) {
          it = devices.erase(it);
          continue;
        }

        if (!dev->IsReady() || (dev->mute == mute)) {
          it = devices.erase(it);
          muteNotChanged = muteNotChanged ? muteNotChanged : (dev->mute == mute);
          deviceNotReady = deviceNotReady ? deviceNotReady : !dev->IsReady();
          continue;
        }
        it++;
      }

      if (devices.empty()) {
        bluetooth::log::debug(
                "No need to update mute for group id: {} . muteNotChanged: {}, "
                "deviceNotReady: {}",
                group_id, muteNotChanged, deviceNotReady);
        return;
      }

      PrepareVolumeControlOperation(devices, group_id, false, opcode, arg);
    }

    StartQueueOperation();
  }

  void Mute(std::variant<RawAddress, int> addr_or_group_id) override {
    bluetooth::log::debug("");
    MuteUnmute(addr_or_group_id, true /* mute */);
  }

  void UnMute(std::variant<RawAddress, int> addr_or_group_id) override {
    bluetooth::log::debug("");
    MuteUnmute(addr_or_group_id, false /* mute */);
  }

  void SetVolume(std::variant<RawAddress, int> addr_or_group_id, uint8_t volume) override {
    std::vector<uint8_t> arg({volume});
    uint8_t opcode = kControlPointOpcodeSetAbsoluteVolume;

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      bluetooth::log::debug("Address: {}:", std::get<RawAddress>(addr_or_group_id));
      VolumeControlDevice* dev =
              volume_control_devices_.FindByAddress(std::get<RawAddress>(addr_or_group_id));
      if (dev != nullptr) {
        bluetooth::log::debug("Address: {}: isReady: {}", dev->address, dev->IsReady());
        if (dev->IsReady() && (dev->volume != volume)) {
          std::vector<RawAddress> devices = {dev->address};
          RemovePendingVolumeControlOperations(devices, bluetooth::groups::kGroupUnknown);
          PrepareVolumeControlOperation(devices, bluetooth::groups::kGroupUnknown, false, opcode,
                                        arg);
        }
      }
    } else {
      /* Handle group change */
      auto group_id = std::get<int>(addr_or_group_id);
      bluetooth::log::debug("group_id: {}, vol: {}", group_id, volume);
      auto csis_api = CsisClient::Get();
      if (!csis_api) {
        bluetooth::log::error("Csis is not there");
        return;
      }

      auto devices = csis_api->GetDeviceList(group_id);
      if (devices.empty()) {
        bluetooth::log::error("group id: {} has no devices", group_id);
        return;
      }

      bool volumeNotChanged = false;
      bool deviceNotReady = false;

      for (auto it = devices.begin(); it != devices.end();) {
        auto dev = volume_control_devices_.FindByAddress(*it);
        if (!dev) {
          it = devices.erase(it);
          continue;
        }

        if (!dev->IsReady() || (dev->volume == volume)) {
          it = devices.erase(it);
          volumeNotChanged = volumeNotChanged ? volumeNotChanged : (dev->volume == volume);
          deviceNotReady = deviceNotReady ? deviceNotReady : !dev->IsReady();
          continue;
        }

        it++;
      }

      if (devices.empty()) {
        bluetooth::log::debug(
                "No need to update volume for group id: {} . volumeNotChanged: {}, "
                "deviceNotReady: {}",
                group_id, volumeNotChanged, deviceNotReady);
        return;
      }

      RemovePendingVolumeControlOperations(devices, group_id);
      PrepareVolumeControlOperation(devices, group_id, false, opcode, arg);
    }

    StartQueueOperation();
  }

  /* Methods to operate on Volume Control Offset Service (VOCS) */
  void GetExtAudioOutVolumeOffset(const RawAddress& address, uint8_t ext_output_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("no such device!");
      return;
    }

    device->GetExtAudioOutVolumeOffset(ext_output_id, chrc_read_callback_static, nullptr);
  }

  void SetExtAudioOutVolumeOffset(const RawAddress& address, uint8_t ext_output_id,
                                  int16_t offset_val) override {
    std::vector<uint8_t> arg(2);
    uint8_t* ptr = arg.data();
    UINT16_TO_STREAM(ptr, offset_val);
    ext_audio_out_control_point_helper(address, ext_output_id, kVolumeOffsetControlPointOpcodeSet,
                                       &arg);
  }

  void GetExtAudioOutLocation(const RawAddress& address, uint8_t ext_output_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("no such device!");
      return;
    }

    device->GetExtAudioOutLocation(ext_output_id, chrc_read_callback_static, nullptr);
  }

  void SetExtAudioOutLocation(const RawAddress& address, uint8_t ext_output_id,
                              uint32_t location) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("no such device!");
      return;
    }

    device->SetExtAudioOutLocation(ext_output_id, location);
  }

  void GetExtAudioOutDescription(const RawAddress& address, uint8_t ext_output_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("no such device!");
      return;
    }

    device->GetExtAudioOutDescription(ext_output_id, chrc_read_callback_static, nullptr);
  }

  void SetExtAudioOutDescription(const RawAddress& address, uint8_t ext_output_id,
                                 std::string descr) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("no such device!");
      return;
    }

    device->SetExtAudioOutDescription(ext_output_id, descr);
  }

  /* Methods to operate on Audio Input Service (AIS) */
  void GetExtAudioInState(const RawAddress& address, uint8_t ext_input_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      return;
    }

    device->GetExtAudioInState(ext_input_id, chrc_read_callback_static, nullptr);
  }

  void GetExtAudioInStatus(const RawAddress& address, uint8_t ext_input_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      return;
    }

    device->GetExtAudioInStatus(ext_input_id, chrc_read_callback_static, nullptr);
  }

  void GetExtAudioInType(const RawAddress& address, uint8_t ext_input_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      return;
    }

    device->GetExtAudioInType(ext_input_id, chrc_read_callback_static, nullptr);
  }

  void GetExtAudioInGainProps(const RawAddress& address, uint8_t ext_input_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      return;
    }

    device->GetExtAudioInGainProps(ext_input_id, chrc_read_callback_static, nullptr);
  }

  void GetExtAudioInDescription(const RawAddress& address, uint8_t ext_input_id) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      return;
    }

    device->GetExtAudioInDescription(ext_input_id, chrc_read_callback_static, nullptr);
  }

  void SetExtAudioInDescription(const RawAddress& address, uint8_t ext_input_id,
                                std::string descr) override {
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      return;
    }

    device->SetExtAudioInDescription(ext_input_id, descr);
  }

  void SetExtAudioInGainSetting(const RawAddress& address, uint8_t ext_input_id,
                                int8_t gain_setting) override {
    std::vector<uint8_t> arg({(uint8_t)gain_setting});
    bluetooth::log::info("{}, input_id={:#x}", address, ext_input_id);

    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      callbacks_->OnExtAudioInSetGainSettingFailed(address, ext_input_id);
      return;
    }

    if (!device->ExtAudioInControlPointOperation(
                ext_input_id, kVolumeInputControlPointOpcodeSetGain, &arg,
                [](uint16_t connection_id, tGATT_STATUS status, uint16_t handle, uint16_t /*len*/,
                   const uint8_t* /*value*/, void* data) {
                  if (instance) {
                    instance->OnExtAudioInCPWrite(connection_id, status, handle,
                                                  kVolumeInputControlPointOpcodeSetGain,
                                                  PTR_TO_INT(data));
                  }
                },
                INT_TO_PTR(ext_input_id))) {
      callbacks_->OnExtAudioInSetGainSettingFailed(address, ext_input_id);
    }
  }

  void SetExtAudioInGainMode(const RawAddress& address, uint8_t ext_input_id,
                             bluetooth::aics::GainMode gain_mode) override {
    bluetooth::log::info("{}, input_id={:#x} gain_mode={:#x}", address, ext_input_id, gain_mode);

    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      callbacks_->OnExtAudioInSetGainModeFailed(address, ext_input_id);
      return;
    }

    if (!device->ExtAudioInControlPointOperation(
                ext_input_id,
                gain_mode == bluetooth::aics::GainMode::AUTOMATIC
                        ? kVolumeInputControlPointOpcodeSetAutoGainMode
                        : kVolumeInputControlPointOpcodeSetManualGainMode,
                nullptr,
                [](uint16_t connection_id, tGATT_STATUS status, uint16_t handle, uint16_t /*len*/,
                   const uint8_t* /*value*/, void* data) {
                  if (instance) {
                    instance->OnExtAudioInCPWrite(connection_id, status, handle,
                                                  kVolumeInputControlPointOpcodeSetAutoGainMode,
                                                  PTR_TO_INT(data));
                  }
                },
                INT_TO_PTR(ext_input_id))) {
      callbacks_->OnExtAudioInSetGainModeFailed(address, ext_input_id);
    }
  }

  void SetExtAudioInMute(const RawAddress& address, uint8_t ext_input_id,
                         bluetooth::aics::Mute mute) override {
    bluetooth::log::info("{}, input_id={:#x}, mute={:#x}", address, ext_input_id, mute);

    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("{}, no such device!", address);
      callbacks_->OnExtAudioInSetMuteFailed(address, ext_input_id);
      return;
    }

    if (!device->ExtAudioInControlPointOperation(
                ext_input_id,
                mute == bluetooth::aics::Mute::MUTED ? kVolumeInputControlPointOpcodeMute
                                                     : kVolumeInputControlPointOpcodeUnmute,
                nullptr,
                [](uint16_t connection_id, tGATT_STATUS status, uint16_t handle, uint16_t /*len*/,
                   const uint8_t* /*value*/, void* data) {
                  if (instance) {
                    instance->OnExtAudioInCPWrite(connection_id, status, handle,
                                                  kVolumeInputControlPointOpcodeMute,
                                                  PTR_TO_INT(data));
                  }
                },
                INT_TO_PTR(ext_input_id))) {
      callbacks_->OnExtAudioInSetMuteFailed(address, ext_input_id);
    }
  }

  void CleanUp() {
    bluetooth::log::info("");
    volume_control_devices_.Disconnect(gatt_if_);
    volume_control_devices_.Clear();
    ongoing_operations_.clear();
    BTA_GATTC_AppDeregister(gatt_if_);
  }

private:
  tGATT_IF gatt_if_;
  bluetooth::vc::VolumeControlCallbacks* callbacks_;
  VolumeControlDevices volume_control_devices_;

  /* Used to track volume control operations */
  std::list<VolumeOperation> ongoing_operations_;
  int latest_operation_id_;

  static constexpr uint64_t kOperationMonitorTimeoutMs = 3000;

  void verify_device_ready(VolumeControlDevice* device, uint16_t handle) {
    bluetooth::log::debug("{}, isReady {}", device->address, device->IsReady());
    if (device->IsReady()) {
      return;
    }

    // VerifyReady sets the device_ready flag if all remaining GATT operations
    // are completed
    if (device->VerifyReady(handle)) {
      bluetooth::log::info("Outstanding reads completed.");

      callbacks_->OnDeviceAvailable(device->address, device->audio_offsets.Size(),
                                    device->audio_inputs.Size());
      callbacks_->OnConnectionState(ConnectionState::CONNECTED, device->address);

      // once profile connected we can notify current states
      callbacks_->OnVolumeStateChanged(device->address, device->volume, device->mute, device->flags,
                                       true);

      device->EnqueueRemainingRequests(gatt_if_, chrc_read_callback_static,
                                       chrc_multi_read_callback_static, OnGattWriteCccStatic);
    }
  }

  void device_cleanup_helper(VolumeControlDevice* device, bool notify) {
    device->Disconnect(gatt_if_);

    RemoveDeviceFromOperationList(device->address);

    if (notify) {
      callbacks_->OnConnectionState(ConnectionState::DISCONNECTED, device->address);
    }
  }

  void devices_control_point_helper(const std::vector<RawAddress>& devices, uint8_t opcode,
                                    const std::vector<uint8_t>* arg, int operation_id = -1) {
    volume_control_devices_.ControlPointOperation(
            devices, opcode, arg,
            [](tCONN_ID connection_id, tGATT_STATUS status, uint16_t handle, uint16_t /*len*/,
               const uint8_t* /*value*/, void* data) {
              if (instance) {
                instance->OnWriteControlResponse(connection_id, status, handle, data);
              }
            },
            INT_TO_PTR(operation_id));
  }

  void ext_audio_out_control_point_helper(const RawAddress& address, uint8_t ext_output_id,
                                          uint8_t opcode, const std::vector<uint8_t>* arg) {
    bluetooth::log::info("{} id={:#x} op={:#x}", address, ext_output_id, opcode);
    VolumeControlDevice* device = volume_control_devices_.FindByAddress(address);
    if (!device) {
      bluetooth::log::error("no such device!");
      return;
    }
    device->ExtAudioOutControlPointOperation(
            ext_output_id, opcode, arg,
            [](tCONN_ID connection_id, tGATT_STATUS status, uint16_t handle, uint16_t /*len*/,
               const uint8_t* /*value*/, void* data) {
              if (instance) {
                instance->OnExtAudioOutCPWrite(connection_id, status, handle, data);
              }
            },
            nullptr);
  }

  void gattc_callback(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
    bluetooth::log::info("event = {}", static_cast<int>(event));

    if (p_data == nullptr) {
      return;
    }

    switch (event) {
      case BTA_GATTC_OPEN_EVT: {
        tBTA_GATTC_OPEN& o = p_data->open;
        OnGattConnected(o.status, o.conn_id, o.client_if, o.remote_bda, o.transport, o.mtu);
      } break;

      case BTA_GATTC_CLOSE_EVT: {
        tBTA_GATTC_CLOSE& c = p_data->close;
        OnGattDisconnected(c.conn_id, c.client_if, c.remote_bda, c.reason);
      } break;

      case BTA_GATTC_SEARCH_CMPL_EVT:
        OnServiceSearchComplete(p_data->search_cmpl.conn_id, p_data->search_cmpl.status);
        break;

      case BTA_GATTC_NOTIF_EVT: {
        tBTA_GATTC_NOTIFY& n = p_data->notify;
        if (!n.is_notify || n.len > GATT_MAX_ATTR_LEN) {
          bluetooth::log::error("rejected BTA_GATTC_NOTIF_EVT. is_notify={}, len={}", n.is_notify,
                                static_cast<int>(n.len));
          break;
        }
        OnNotificationEvent(n.conn_id, n.handle, n.len, n.value);
      } break;

      case BTA_GATTC_ENC_CMPL_CB_EVT: {
        tBTM_STATUS encryption_status;
        if (BTM_IsEncrypted(p_data->enc_cmpl.remote_bda, BT_TRANSPORT_LE)) {
          encryption_status = tBTM_STATUS::BTM_SUCCESS;
        } else {
          encryption_status = tBTM_STATUS::BTM_FAILED_ON_SECURITY;
        }
        OnEncryptionComplete(p_data->enc_cmpl.remote_bda, encryption_status);
      } break;

      case BTA_GATTC_SRVC_CHG_EVT:
        OnServiceChangeEvent(p_data->service_changed.remote_bda);
        break;

      case BTA_GATTC_SRVC_DISC_DONE_EVT:
        OnServiceDiscDoneEvent(p_data->service_discovery_done.remote_bda);
        break;

      default:
        break;
    }
  }

  static void gattc_callback_static(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
    if (instance) {
      instance->gattc_callback(event, p_data);
    }
  }

  static void chrc_read_callback_static(tCONN_ID conn_id, tGATT_STATUS status, uint16_t handle,
                                        uint16_t len, uint8_t* value, void* data) {
    if (instance) {
      instance->OnCharacteristicValueChanged(conn_id, status, handle, len, value, data, false);
    }
  }

  static void chrc_multi_read_callback_static(uint16_t conn_id, tGATT_STATUS status,
                                              tBTA_GATTC_MULTI& handles, uint16_t total_len,
                                              uint8_t* value, void* data) {
    if (!instance) {
      return;
    }

    if (status != GATT_SUCCESS) {
      bluetooth::log::error("conn_id={:#} multi read failed {:#x}", conn_id, status);
      instance->OnCharacteristicValueChanged(conn_id, status, 0, 0, nullptr, nullptr, false);
      return;
    }

    size_t position = 0;
    int index = 0;
    while (position != total_len) {
      uint8_t* ptr = value + position;
      uint16_t len;
      STREAM_TO_UINT16(len, ptr);
      uint16_t hdl = handles.handles[index];

      if (position + len >= total_len) {
        bluetooth::log::warn(
                "Multi read was too long, value truncated conn_id: {:#x} handle: {:#x}, position: "
                "{:#x}, len: {:#x}, total_len: {:#x}, data: {}",
                conn_id, hdl, position, len, total_len, base::HexEncode(value, total_len));
        break;
      }

      instance->OnCharacteristicValueChanged(conn_id, status, hdl, len, ptr,
                                             ((index == (handles.num_attr - 1)) ? data : nullptr),
                                             false);

      position += len + 2; /* skip the length of data */
      index++;
    }

    if (handles.num_attr - 1 != index) {
      bluetooth::log::warn("Attempted to read {} handles, but received just {} values",
                           +handles.num_attr, index + 1);
    }
  }
};
}  // namespace

void VolumeControl::Initialize(bluetooth::vc::VolumeControlCallbacks* callbacks,
                               const base::Closure& initCb) {
  std::scoped_lock<std::mutex> lock(instance_mutex);
  if (instance) {
    bluetooth::log::error("Already initialized!");
    return;
  }

  instance = new VolumeControlImpl(callbacks, initCb);
}

bool VolumeControl::IsVolumeControlRunning() { return instance; }

VolumeControl* VolumeControl::Get(void) {
  bluetooth::log::assert_that(instance != nullptr, "assert failed: instance != nullptr");
  return instance;
}

void VolumeControl::AddFromStorage(const RawAddress& address) {
  if (!instance) {
    bluetooth::log::error("Not initialized yet");
    return;
  }

  instance->AddFromStorage(address);
}

void VolumeControl::CleanUp() {
  std::scoped_lock<std::mutex> lock(instance_mutex);
  if (!instance) {
    bluetooth::log::error("Not initialized!");
    return;
  }

  VolumeControlImpl* ptr = instance;
  instance = nullptr;

  ptr->CleanUp();

  delete ptr;
}

void VolumeControl::DebugDump(int fd) {
  std::scoped_lock<std::mutex> lock(instance_mutex);
  dprintf(fd, "Volume Control Manager:\n");
  if (instance) {
    instance->Dump(fd);
  }
  dprintf(fd, "\n");
}
