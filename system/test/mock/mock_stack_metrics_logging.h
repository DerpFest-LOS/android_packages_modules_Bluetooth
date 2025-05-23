/*
 * Copyright 2021 The Android Open Source Project
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

/*
 * Generated mock file from original source file
 *   Functions generated:5
 *
 *  mockcify.pl ver 0.2
 */

#include <string>

// Original included files, if any
#include <frameworks/proto_logging/stats/enums/bluetooth/enums.pb.h>
#include <frameworks/proto_logging/stats/enums/bluetooth/hci/enums.pb.h>

#include "hci/address.h"
#include "hci/hci_packets.h"
#include "types/raw_address.h"

// Mocked compile conditionals, if any

namespace test {
namespace mock {
namespace stack_metrics_logging {

// Shared state between mocked functions and tests
// Name: log_classic_pairing_event
// Params: const RawAddress& address, uint16_t handle, uint32_t hci_cmd,
// uint16_t hci_event, uint16_t cmd_status, uint16_t reason_code, int64_t
// event_value Returns: void
struct log_classic_pairing_event {
  std::function<void(const RawAddress& address, uint16_t handle, uint32_t hci_cmd,
                     uint16_t hci_event, uint16_t cmd_status, uint16_t reason_code,
                     int64_t event_value)>
          body{[](const RawAddress& /* address */, uint16_t /* handle */, uint32_t /* hci_cmd */,
                  uint16_t /* hci_event */, uint16_t /* cmd_status */, uint16_t /* reason_code */,
                  int64_t /* event_value */) {}};
  void operator()(const RawAddress& address, uint16_t handle, uint32_t hci_cmd, uint16_t hci_event,
                  uint16_t cmd_status, uint16_t reason_code, int64_t event_value) {
    body(address, handle, hci_cmd, hci_event, cmd_status, reason_code, event_value);
  }
};
extern struct log_classic_pairing_event log_classic_pairing_event;
// Name: log_link_layer_connection_event
// Params:  const RawAddress* address, uint32_t connection_handle,
// android::bluetooth::DirectionEnum direction, uint16_t link_type, uint32_t
// hci_cmd, uint16_t hci_event, uint16_t hci_ble_event, uint16_t cmd_status,
// uint16_t reason_code Returns: void
struct log_link_layer_connection_event {
  std::function<void(const RawAddress* address, uint32_t connection_handle,
                     android::bluetooth::DirectionEnum direction, uint16_t link_type,
                     uint32_t hci_cmd, uint16_t hci_event, uint16_t hci_ble_event,
                     uint16_t cmd_status, uint16_t reason_code)>
          body{[](const RawAddress* /* address */, uint32_t /* connection_handle */,
                  android::bluetooth::DirectionEnum /* direction */, uint16_t /* link_type */,
                  uint32_t /* hci_cmd */, uint16_t /* hci_event */, uint16_t /* hci_ble_event */,
                  uint16_t /* cmd_status */, uint16_t /* reason_code */) {}};
  void operator()(const RawAddress* address, uint32_t connection_handle,
                  android::bluetooth::DirectionEnum direction, uint16_t link_type, uint32_t hci_cmd,
                  uint16_t hci_event, uint16_t hci_ble_event, uint16_t cmd_status,
                  uint16_t reason_code) {
    body(address, connection_handle, direction, link_type, hci_cmd, hci_event, hci_ble_event,
         cmd_status, reason_code);
  }
};
extern struct log_link_layer_connection_event log_link_layer_connection_event;
// Name: log_smp_pairing_event
// Params: const RawAddress& address, uint16_t smp_cmd,
// android::bluetooth::DirectionEnum direction, uint8_t smp_fail_reason Returns:
// void
struct log_smp_pairing_event {
  std::function<void(const RawAddress& address, uint16_t smp_cmd,
                     android::bluetooth::DirectionEnum direction, uint16_t smp_fail_reason)>
          body{[](const RawAddress& /* address */, uint16_t /* smp_cmd */,
                  android::bluetooth::DirectionEnum /* direction */,
                  uint16_t /* smp_fail_reason */) {}};
  void operator()(const RawAddress& address, uint16_t smp_cmd,
                  android::bluetooth::DirectionEnum direction, uint16_t smp_fail_reason) {
    body(address, smp_cmd, direction, smp_fail_reason);
  }
};
extern struct log_smp_pairing_event log_smp_pairing_event;

// Name: log_le_pairing_fail
// Params: const RawAddress& raw_address, uint8_t failure_reason, bool
// is_outgoing Returns:
// void
// Name: log_sdp_attribute
// Params: const RawAddress& address, uint16_t protocol_uuid, uint16_t
// attribute_id, size_t attribute_size, const char* attribute_value Returns:
// void
struct log_le_pairing_fail {
  std::function<void(const RawAddress& raw_address, uint8_t failure_reason, bool is_outgoing)> body{
          [](const RawAddress& /* address */, uint8_t /* failure reason */,
             bool /* is_outgoing */) {}};
  void operator()(const RawAddress& raw_address, uint8_t failure_reason, bool is_outgoing) {
    body(raw_address, failure_reason, is_outgoing);
  }
};
extern struct log_le_pairing_fail log_le_pairing_fail;

struct log_sdp_attribute {
  std::function<void(const RawAddress& address, uint16_t protocol_uuid, uint16_t attribute_id,
                     size_t attribute_size, const char* attribute_value)>
          body{[](const RawAddress& /* address */, uint16_t /* protocol_uuid */,
                  uint16_t /* attribute_id */, size_t /* attribute_size */,
                  const char* /* attribute_value */) {}};
  void operator()(const RawAddress& address, uint16_t protocol_uuid, uint16_t attribute_id,
                  size_t attribute_size, const char* attribute_value) {
    body(address, protocol_uuid, attribute_id, attribute_size, attribute_value);
  }
};
extern struct log_sdp_attribute log_sdp_attribute;
// Name: log_manufacturer_info
// Params: const RawAddress& address, android::bluetooth::DeviceInfoSrcEnum
// source_type, const std::string& source_name, const std::string& manufacturer,
// const std::string& model, const std::string& hardware_version, const
// std::string& software_version Returns: void
struct log_manufacturer_info {
  std::function<void(const RawAddress& address, android::bluetooth::AddressTypeEnum address_type,
                     android::bluetooth::DeviceInfoSrcEnum source_type,
                     const std::string& source_name, const std::string& manufacturer,
                     const std::string& model, const std::string& hardware_version,
                     const std::string& software_version)>
          body2{[](const RawAddress& /* address */,
                   android::bluetooth::AddressTypeEnum /* address_type */,
                   android::bluetooth::DeviceInfoSrcEnum /* source_type */,
                   const std::string& /* source_name */, const std::string& /* manufacturer */,
                   const std::string& /* model */, const std::string& /* hardware_version */,
                   const std::string& /* software_version */) {}};
  void operator()(const RawAddress& address, android::bluetooth::AddressTypeEnum address_type,
                  android::bluetooth::DeviceInfoSrcEnum source_type, const std::string& source_name,
                  const std::string& manufacturer, const std::string& model,
                  const std::string& hardware_version, const std::string& software_version) {
    body2(address, address_type, source_type, source_name, manufacturer, model, hardware_version,
          software_version);
  }
  std::function<void(const RawAddress& address, android::bluetooth::DeviceInfoSrcEnum source_type,
                     const std::string& source_name, const std::string& manufacturer,
                     const std::string& model, const std::string& hardware_version,
                     const std::string& software_version)>
          body{[](const RawAddress& /* address */,
                  android::bluetooth::DeviceInfoSrcEnum /* source_type */,
                  const std::string& /* source_name */, const std::string& /* manufacturer */,
                  const std::string& /* model */, const std::string& /* hardware_version */,
                  const std::string& /* software_version */) {}};
  void operator()(const RawAddress& address, android::bluetooth::DeviceInfoSrcEnum source_type,
                  const std::string& source_name, const std::string& manufacturer,
                  const std::string& model, const std::string& hardware_version,
                  const std::string& software_version) {
    body(address, source_type, source_name, manufacturer, model, hardware_version,
         software_version);
  }
};
extern struct log_manufacturer_info log_manufacturer_info;

// Name: log_counter_metrics
struct log_counter_metrics {
  std::function<void(android::bluetooth::CodePathCounterKeyEnum key, int64_t value)> body{
          [](android::bluetooth::CodePathCounterKeyEnum /* key */, int64_t /* value */) {}};
  void operator()(android::bluetooth::CodePathCounterKeyEnum key, int64_t value) {
    body(key, value);
  }
};
extern struct log_counter_metrics log_counter_metrics;

// Name: log_hfp_audio_packet_loss_stats
struct log_hfp_audio_packet_loss_stats {
  std::function<void(const RawAddress& address, int num_decoded_frames, double packet_loss_ratio,
                     uint16_t codec_type)>
          body{[](const RawAddress& /* address */, int /* num_decoded_frames */,
                  double /* packet_loss_ratio */, uint16_t /* codec_type */) {}};
  void operator()(const RawAddress& address, int num_decoded_frames, double packet_loss_ratio,
                  uint16_t codec_type) {
    body(address, num_decoded_frames, packet_loss_ratio, codec_type);
  }
};
extern struct log_hfp_audio_packet_loss_stats log_hfp_audio_packet_loss_stats;

// Name: log_mmc_transcode_rtt_stats
struct log_mmc_transcode_rtt_stats {
  std::function<void(int maximum_rtt, double mean_rtt, int num_requests, int codec_type)> body{
          [](int /* maximum_rtt */, double /* mean_rtt */, int /* num_requests */,
             int /* codec_type */) {}};
  void operator()(int maximum_rtt, double mean_rtt, int num_requests, int codec_type) {
    body(maximum_rtt, mean_rtt, num_requests, codec_type);
  }
};
extern struct log_mmc_transcode_rtt_stats log_mmc_transcode_rtt_stats;

// Name: log_le_connection_status
struct log_le_connection_status {
  std::function<void(bluetooth::hci::Address address, bool is_connect,
                     bluetooth::hci::ErrorCode reason)>
          body{[](bluetooth::hci::Address /* address */, bool /* is_connect */,
                  bluetooth::hci::ErrorCode /* reason */) {}};
  void operator()(bluetooth::hci::Address address, bool is_connect,
                  bluetooth::hci::ErrorCode reason) {
    body(address, is_connect, reason);
  }
};
extern struct log_le_connection_status log_le_connection_status;

// Name: log_le_device_in_accept_list
struct log_le_device_in_accept_list {
  std::function<void(bluetooth::hci::Address address, bool is_add)> body{
          [](bluetooth::hci::Address /* address */, bool /* is_add */) {}};
  void operator()(bluetooth::hci::Address address, bool is_add) { body(address, is_add); }
};
extern struct log_le_device_in_accept_list log_le_device_in_accept_list;

// Name: log_le_connection_lifecycle
struct log_le_connection_lifecycle {
  std::function<void(bluetooth::hci::Address address, bool is_connect, bool is_direct)> body{
          [](bluetooth::hci::Address /* address */, bool /* is_connect */, bool /* is_direct */) {
          }};
  void operator()(bluetooth::hci::Address address, bool is_connect, bool is_direct) {
    body(address, is_connect, is_direct);
  }
};
extern struct log_le_device_in_accept_list log_le_device_in_accept_list;

}  // namespace stack_metrics_logging
}  // namespace mock
}  // namespace test

// END mockcify generation
