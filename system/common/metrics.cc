/******************************************************************************
 *
 *  Copyright 2016 Google, Inc.
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

#include "common/metrics.h"

#include <base/base64.h>
#include <bluetooth/log.h>
#include <frameworks/proto_logging/stats/enums/bluetooth/le/enums.pb.h>
#include <include/hardware/bt_av.h>
#include <statslog_bt.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>  // NOLINT
#include <utility>

#include "bluetooth/metrics/bluetooth.pb.h"
#include "common/address_obfuscator.h"
#include "common/leaky_bonded_queue.h"
#include "common/time_util.h"
#include "hci/address.h"
#include "main/shim/metric_id_api.h"
#include "osi/include/osi.h"
#include "types/raw_address.h"

namespace std {
template <>
struct formatter<android::bluetooth::DirectionEnum>
    : enum_formatter<android::bluetooth::DirectionEnum> {};
template <>
struct formatter<android::bluetooth::SocketConnectionstateEnum>
    : enum_formatter<android::bluetooth::SocketConnectionstateEnum> {};
template <>
struct formatter<android::bluetooth::SocketRoleEnum>
    : enum_formatter<android::bluetooth::SocketRoleEnum> {};
template <>
struct formatter<android::bluetooth::AddressTypeEnum>
    : enum_formatter<android::bluetooth::AddressTypeEnum> {};
template <>
struct formatter<android::bluetooth::DeviceInfoSrcEnum>
    : enum_formatter<android::bluetooth::DeviceInfoSrcEnum> {};
}  // namespace std

namespace bluetooth {
namespace common {

using bluetooth::hci::Address;
using bluetooth::metrics::BluetoothMetricsProto::A2DPSession;
using bluetooth::metrics::BluetoothMetricsProto::A2dpSourceCodec;
using bluetooth::metrics::BluetoothMetricsProto::BluetoothLog;
using bluetooth::metrics::BluetoothMetricsProto::BluetoothSession;
using bluetooth::metrics::BluetoothMetricsProto::BluetoothSession_ConnectionTechnologyType;
using bluetooth::metrics::BluetoothMetricsProto::BluetoothSession_DisconnectReasonType;
using bluetooth::metrics::BluetoothMetricsProto::DeviceInfo;
using bluetooth::metrics::BluetoothMetricsProto::DeviceInfo_DeviceType;
using bluetooth::metrics::BluetoothMetricsProto::HeadsetProfileConnectionStats;
using bluetooth::metrics::BluetoothMetricsProto::HeadsetProfileType;
using bluetooth::metrics::BluetoothMetricsProto::HeadsetProfileType_ARRAYSIZE;
using bluetooth::metrics::BluetoothMetricsProto::HeadsetProfileType_IsValid;
using bluetooth::metrics::BluetoothMetricsProto::HeadsetProfileType_MAX;
using bluetooth::metrics::BluetoothMetricsProto::HeadsetProfileType_MIN;
using bluetooth::metrics::BluetoothMetricsProto::PairEvent;
using bluetooth::metrics::BluetoothMetricsProto::ScanEvent;
using bluetooth::metrics::BluetoothMetricsProto::ScanEvent_ScanEventType;
using bluetooth::metrics::BluetoothMetricsProto::ScanEvent_ScanTechnologyType;
using bluetooth::metrics::BluetoothMetricsProto::WakeEvent;
using bluetooth::metrics::BluetoothMetricsProto::WakeEvent_WakeEventType;

static float combine_averages(float avg_a, int64_t ct_a, float avg_b, int64_t ct_b) {
  if (ct_a > 0 && ct_b > 0) {
    return (avg_a * ct_a + avg_b * ct_b) / (ct_a + ct_b);
  } else if (ct_b > 0) {
    return avg_b;
  } else {
    return avg_a;
  }
}

static int32_t combine_averages(int32_t avg_a, int64_t ct_a, int32_t avg_b, int64_t ct_b) {
  if (ct_a > 0 && ct_b > 0) {
    return (avg_a * ct_a + avg_b * ct_b) / (ct_a + ct_b);
  } else if (ct_b > 0) {
    return avg_b;
  } else {
    return avg_a;
  }
}

void A2dpSessionMetrics::Update(const A2dpSessionMetrics& metrics) {
  if (metrics.audio_duration_ms >= 0) {
    audio_duration_ms = std::max(static_cast<int64_t>(0), audio_duration_ms);
    audio_duration_ms += metrics.audio_duration_ms;
  }
  if (metrics.media_timer_min_ms >= 0) {
    if (media_timer_min_ms < 0) {
      media_timer_min_ms = metrics.media_timer_min_ms;
    } else {
      media_timer_min_ms = std::min(media_timer_min_ms, metrics.media_timer_min_ms);
    }
  }
  if (metrics.media_timer_max_ms >= 0) {
    media_timer_max_ms = std::max(media_timer_max_ms, metrics.media_timer_max_ms);
  }
  if (metrics.media_timer_avg_ms >= 0 && metrics.total_scheduling_count >= 0) {
    if (media_timer_avg_ms < 0 || total_scheduling_count < 0) {
      media_timer_avg_ms = metrics.media_timer_avg_ms;
      total_scheduling_count = metrics.total_scheduling_count;
    } else {
      media_timer_avg_ms =
              combine_averages(media_timer_avg_ms, total_scheduling_count,
                               metrics.media_timer_avg_ms, metrics.total_scheduling_count);
      total_scheduling_count += metrics.total_scheduling_count;
    }
  }
  if (metrics.buffer_overruns_max_count >= 0) {
    buffer_overruns_max_count =
            std::max(buffer_overruns_max_count, metrics.buffer_overruns_max_count);
  }
  if (metrics.buffer_overruns_total >= 0) {
    buffer_overruns_total = std::max(static_cast<int32_t>(0), buffer_overruns_total);
    buffer_overruns_total += metrics.buffer_overruns_total;
  }
  if (metrics.buffer_underruns_average >= 0 && metrics.buffer_underruns_count >= 0) {
    if (buffer_underruns_average < 0 || buffer_underruns_count < 0) {
      buffer_underruns_average = metrics.buffer_underruns_average;
      buffer_underruns_count = metrics.buffer_underruns_count;
    } else {
      buffer_underruns_average =
              combine_averages(buffer_underruns_average, buffer_underruns_count,
                               metrics.buffer_underruns_average, metrics.buffer_underruns_count);
      buffer_underruns_count += metrics.buffer_underruns_count;
    }
  }
  if (codec_index < 0) {
    codec_index = metrics.codec_index;
  }
  if (!is_a2dp_offload) {
    is_a2dp_offload = metrics.is_a2dp_offload;
  }
}

bool A2dpSessionMetrics::operator==(const A2dpSessionMetrics& rhs) const {
  return audio_duration_ms == rhs.audio_duration_ms &&
         media_timer_min_ms == rhs.media_timer_min_ms &&
         media_timer_max_ms == rhs.media_timer_max_ms &&
         media_timer_avg_ms == rhs.media_timer_avg_ms &&
         total_scheduling_count == rhs.total_scheduling_count &&
         buffer_overruns_max_count == rhs.buffer_overruns_max_count &&
         buffer_overruns_total == rhs.buffer_overruns_total &&
         buffer_underruns_average == rhs.buffer_underruns_average &&
         buffer_underruns_count == rhs.buffer_underruns_count && codec_index == rhs.codec_index &&
         is_a2dp_offload == rhs.is_a2dp_offload;
}

static DeviceInfo_DeviceType get_device_type(device_type_t type) {
  switch (type) {
    case DEVICE_TYPE_BREDR:
      return DeviceInfo_DeviceType::DeviceInfo_DeviceType_DEVICE_TYPE_BREDR;
    case DEVICE_TYPE_LE:
      return DeviceInfo_DeviceType::DeviceInfo_DeviceType_DEVICE_TYPE_LE;
    case DEVICE_TYPE_DUMO:
      return DeviceInfo_DeviceType::DeviceInfo_DeviceType_DEVICE_TYPE_DUMO;
    case DEVICE_TYPE_UNKNOWN:
    default:
      return DeviceInfo_DeviceType::DeviceInfo_DeviceType_DEVICE_TYPE_UNKNOWN;
  }
}

static BluetoothSession_ConnectionTechnologyType get_connection_tech_type(connection_tech_t type) {
  switch (type) {
    case CONNECTION_TECHNOLOGY_TYPE_LE:
      return BluetoothSession_ConnectionTechnologyType::
              BluetoothSession_ConnectionTechnologyType_CONNECTION_TECHNOLOGY_TYPE_LE;
    case CONNECTION_TECHNOLOGY_TYPE_BREDR:
      return BluetoothSession_ConnectionTechnologyType::
              BluetoothSession_ConnectionTechnologyType_CONNECTION_TECHNOLOGY_TYPE_BREDR;
    case CONNECTION_TECHNOLOGY_TYPE_UNKNOWN:
    default:
      return BluetoothSession_ConnectionTechnologyType::
              BluetoothSession_ConnectionTechnologyType_CONNECTION_TECHNOLOGY_TYPE_UNKNOWN;
  }
}

static ScanEvent_ScanTechnologyType get_scan_tech_type(scan_tech_t type) {
  switch (type) {
    case SCAN_TECH_TYPE_LE:
      return ScanEvent_ScanTechnologyType::ScanEvent_ScanTechnologyType_SCAN_TECH_TYPE_LE;
    case SCAN_TECH_TYPE_BREDR:
      return ScanEvent_ScanTechnologyType::ScanEvent_ScanTechnologyType_SCAN_TECH_TYPE_BREDR;
    case SCAN_TECH_TYPE_BOTH:
      return ScanEvent_ScanTechnologyType::ScanEvent_ScanTechnologyType_SCAN_TECH_TYPE_BOTH;
    case SCAN_TYPE_UNKNOWN:
    default:
      return ScanEvent_ScanTechnologyType::ScanEvent_ScanTechnologyType_SCAN_TYPE_UNKNOWN;
  }
}

static WakeEvent_WakeEventType get_wake_event_type(wake_event_type_t type) {
  switch (type) {
    case WAKE_EVENT_ACQUIRED:
      return WakeEvent_WakeEventType::WakeEvent_WakeEventType_ACQUIRED;
    case WAKE_EVENT_RELEASED:
      return WakeEvent_WakeEventType::WakeEvent_WakeEventType_RELEASED;
    case WAKE_EVENT_UNKNOWN:
    default:
      return WakeEvent_WakeEventType::WakeEvent_WakeEventType_UNKNOWN;
  }
}

static BluetoothSession_DisconnectReasonType get_disconnect_reason_type(disconnect_reason_t type) {
  switch (type) {
    case DISCONNECT_REASON_METRICS_DUMP:
      return BluetoothSession_DisconnectReasonType::
              BluetoothSession_DisconnectReasonType_METRICS_DUMP;
    case DISCONNECT_REASON_NEXT_START_WITHOUT_END_PREVIOUS:
      return BluetoothSession_DisconnectReasonType::
              BluetoothSession_DisconnectReasonType_NEXT_START_WITHOUT_END_PREVIOUS;
    case DISCONNECT_REASON_UNKNOWN:
    default:
      return BluetoothSession_DisconnectReasonType::BluetoothSession_DisconnectReasonType_UNKNOWN;
  }
}

static A2dpSourceCodec get_a2dp_source_codec(int64_t codec_index) {
  switch (codec_index) {
    case BTAV_A2DP_CODEC_INDEX_SOURCE_SBC:
      return A2dpSourceCodec::A2DP_SOURCE_CODEC_SBC;
    case BTAV_A2DP_CODEC_INDEX_SOURCE_AAC:
      return A2dpSourceCodec::A2DP_SOURCE_CODEC_AAC;
    case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX:
      return A2dpSourceCodec::A2DP_SOURCE_CODEC_APTX;
    case BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_HD:
      return A2dpSourceCodec::A2DP_SOURCE_CODEC_APTX_HD;
    case BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC:
      return A2dpSourceCodec::A2DP_SOURCE_CODEC_LDAC;
    default:
      return A2dpSourceCodec::A2DP_SOURCE_CODEC_UNKNOWN;
  }
}

struct BluetoothMetricsLogger::impl {
  impl(size_t max_bluetooth_session, size_t max_pair_event, size_t max_wake_event,
       size_t max_scan_event)
      : bt_session_queue_(new LeakyBondedQueue<BluetoothSession>(max_bluetooth_session)),
        pair_event_queue_(new LeakyBondedQueue<PairEvent>(max_pair_event)),
        wake_event_queue_(new LeakyBondedQueue<WakeEvent>(max_wake_event)),
        scan_event_queue_(new LeakyBondedQueue<ScanEvent>(max_scan_event)) {
    bluetooth_log_ = BluetoothLog::default_instance().New();
    headset_profile_connection_counts_.fill(0);
    bluetooth_session_ = nullptr;
    bluetooth_session_start_time_ms_ = 0;
    a2dp_session_metrics_ = A2dpSessionMetrics();
  }

  /* Bluetooth log lock protected */
  BluetoothLog* bluetooth_log_;
  std::array<int, HeadsetProfileType_ARRAYSIZE> headset_profile_connection_counts_;
  std::recursive_mutex bluetooth_log_lock_;
  /* End Bluetooth log lock protected */
  /* Bluetooth session lock protected */
  BluetoothSession* bluetooth_session_;
  uint64_t bluetooth_session_start_time_ms_;
  A2dpSessionMetrics a2dp_session_metrics_;
  std::recursive_mutex bluetooth_session_lock_;
  /* End bluetooth session lock protected */
  std::unique_ptr<LeakyBondedQueue<BluetoothSession>> bt_session_queue_;
  std::unique_ptr<LeakyBondedQueue<PairEvent>> pair_event_queue_;
  std::unique_ptr<LeakyBondedQueue<WakeEvent>> wake_event_queue_;
  std::unique_ptr<LeakyBondedQueue<ScanEvent>> scan_event_queue_;
};

BluetoothMetricsLogger::BluetoothMetricsLogger()
    : pimpl_(new impl(kMaxNumBluetoothSession, kMaxNumPairEvent, kMaxNumWakeEvent,
                      kMaxNumScanEvent)) {}

void BluetoothMetricsLogger::LogPairEvent(uint32_t disconnect_reason, uint64_t timestamp_ms,
                                          uint32_t device_class, device_type_t device_type) {
  PairEvent* event = new PairEvent();
  DeviceInfo* info = event->mutable_device_paired_with();
  info->set_device_class(device_class);
  info->set_device_type(get_device_type(device_type));
  event->set_disconnect_reason(disconnect_reason);
  event->set_event_time_millis(timestamp_ms);
  pimpl_->pair_event_queue_->Enqueue(event);
  {
    std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_log_lock_);
    pimpl_->bluetooth_log_->set_num_pair_event(pimpl_->bluetooth_log_->num_pair_event() + 1);
  }
}

void BluetoothMetricsLogger::LogWakeEvent(wake_event_type_t type, const std::string& requestor,
                                          const std::string& name, uint64_t timestamp_ms) {
  WakeEvent* event = new WakeEvent();
  event->set_wake_event_type(get_wake_event_type(type));
  event->set_requestor(requestor);
  event->set_name(name);
  event->set_event_time_millis(timestamp_ms);
  pimpl_->wake_event_queue_->Enqueue(event);
  {
    std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_log_lock_);
    pimpl_->bluetooth_log_->set_num_wake_event(pimpl_->bluetooth_log_->num_wake_event() + 1);
  }
}

void BluetoothMetricsLogger::LogScanEvent(bool start, const std::string& initiator,
                                          scan_tech_t type, uint32_t results,
                                          uint64_t timestamp_ms) {
  ScanEvent* event = new ScanEvent();
  if (start) {
    event->set_scan_event_type(ScanEvent::SCAN_EVENT_START);
  } else {
    event->set_scan_event_type(ScanEvent::SCAN_EVENT_STOP);
  }
  event->set_initiator(initiator);
  event->set_scan_technology_type(get_scan_tech_type(type));
  event->set_number_results(results);
  event->set_event_time_millis(timestamp_ms);
  pimpl_->scan_event_queue_->Enqueue(event);
  {
    std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_log_lock_);
    pimpl_->bluetooth_log_->set_num_scan_event(pimpl_->bluetooth_log_->num_scan_event() + 1);
  }
}

void BluetoothMetricsLogger::LogBluetoothSessionStart(connection_tech_t connection_tech_type,
                                                      uint64_t timestamp_ms) {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_session_lock_);
  if (pimpl_->bluetooth_session_ != nullptr) {
    LogBluetoothSessionEnd(DISCONNECT_REASON_NEXT_START_WITHOUT_END_PREVIOUS, 0);
  }
  if (timestamp_ms == 0) {
    timestamp_ms = bluetooth::common::time_get_os_boottime_ms();
  }
  pimpl_->bluetooth_session_start_time_ms_ = timestamp_ms;
  pimpl_->bluetooth_session_ = new BluetoothSession();
  pimpl_->bluetooth_session_->set_connection_technology_type(
          get_connection_tech_type(connection_tech_type));
}

void BluetoothMetricsLogger::LogBluetoothSessionEnd(disconnect_reason_t disconnect_reason,
                                                    uint64_t timestamp_ms) {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_session_lock_);
  if (pimpl_->bluetooth_session_ == nullptr) {
    return;
  }
  if (timestamp_ms == 0) {
    timestamp_ms = bluetooth::common::time_get_os_boottime_ms();
  }
  int64_t session_duration_sec = (timestamp_ms - pimpl_->bluetooth_session_start_time_ms_) / 1000;
  pimpl_->bluetooth_session_->set_session_duration_sec(session_duration_sec);
  pimpl_->bluetooth_session_->set_disconnect_reason_type(
          get_disconnect_reason_type(disconnect_reason));
  pimpl_->bt_session_queue_->Enqueue(pimpl_->bluetooth_session_);
  pimpl_->bluetooth_session_ = nullptr;
  pimpl_->a2dp_session_metrics_ = A2dpSessionMetrics();
  {
    std::lock_guard<std::recursive_mutex> log_lock(pimpl_->bluetooth_log_lock_);
    pimpl_->bluetooth_log_->set_num_bluetooth_session(
            pimpl_->bluetooth_log_->num_bluetooth_session() + 1);
  }
}

void BluetoothMetricsLogger::LogBluetoothSessionDeviceInfo(uint32_t device_class,
                                                           device_type_t device_type) {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_session_lock_);
  if (pimpl_->bluetooth_session_ == nullptr) {
    LogBluetoothSessionStart(CONNECTION_TECHNOLOGY_TYPE_UNKNOWN, 0);
  }
  DeviceInfo* info = pimpl_->bluetooth_session_->mutable_device_connected_to();
  info->set_device_class(device_class);
  info->set_device_type(get_device_type(device_type));
}

void BluetoothMetricsLogger::LogA2dpSession(const A2dpSessionMetrics& a2dp_session_metrics) {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_session_lock_);
  if (pimpl_->bluetooth_session_ == nullptr) {
    // When no bluetooth session exist, create one on system's behalf
    // Set connection type: for A2DP it is always BR/EDR
    LogBluetoothSessionStart(CONNECTION_TECHNOLOGY_TYPE_BREDR, 0);
    LogBluetoothSessionDeviceInfo(BTM_COD_MAJOR_AUDIO, DEVICE_TYPE_BREDR);
  }
  // Accumulate metrics
  pimpl_->a2dp_session_metrics_.Update(a2dp_session_metrics);
  // Get or allocate new A2DP session object
  A2DPSession* a2dp_session = pimpl_->bluetooth_session_->mutable_a2dp_session();
  a2dp_session->set_audio_duration_millis(pimpl_->a2dp_session_metrics_.audio_duration_ms);
  a2dp_session->set_media_timer_min_millis(pimpl_->a2dp_session_metrics_.media_timer_min_ms);
  a2dp_session->set_media_timer_max_millis(pimpl_->a2dp_session_metrics_.media_timer_max_ms);
  a2dp_session->set_media_timer_avg_millis(pimpl_->a2dp_session_metrics_.media_timer_avg_ms);
  a2dp_session->set_buffer_overruns_max_count(
          pimpl_->a2dp_session_metrics_.buffer_overruns_max_count);
  a2dp_session->set_buffer_overruns_total(pimpl_->a2dp_session_metrics_.buffer_overruns_total);
  a2dp_session->set_buffer_underruns_average(
          pimpl_->a2dp_session_metrics_.buffer_underruns_average);
  a2dp_session->set_buffer_underruns_count(pimpl_->a2dp_session_metrics_.buffer_underruns_count);
  a2dp_session->set_source_codec(get_a2dp_source_codec(pimpl_->a2dp_session_metrics_.codec_index));
  a2dp_session->set_is_a2dp_offload(pimpl_->a2dp_session_metrics_.is_a2dp_offload);
}

void BluetoothMetricsLogger::LogHeadsetProfileRfcConnection(tBTA_SERVICE_ID service_id) {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_log_lock_);
  switch (service_id) {
    case BTA_HSP_SERVICE_ID:
      pimpl_->headset_profile_connection_counts_[HeadsetProfileType::HSP]++;
      break;
    case BTA_HFP_SERVICE_ID:
      pimpl_->headset_profile_connection_counts_[HeadsetProfileType::HFP]++;
      break;
    default:
      pimpl_->headset_profile_connection_counts_[HeadsetProfileType::HEADSET_PROFILE_UNKNOWN]++;
      break;
  }
  return;
}

void BluetoothMetricsLogger::WriteString(std::string* serialized) {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_log_lock_);
  log::info("building metrics");
  Build();
  log::info("serializing metrics");
  if (!pimpl_->bluetooth_log_->SerializeToString(serialized)) {
    log::error("error serializing metrics");
  }
  // Always clean up log objects
  pimpl_->bluetooth_log_->Clear();
}

void BluetoothMetricsLogger::WriteBase64String(std::string* serialized) {
  this->WriteString(serialized);
  base::Base64Encode(*serialized, serialized);
}

void BluetoothMetricsLogger::WriteBase64(int fd) {
  std::string protoBase64;
  this->WriteBase64String(&protoBase64);
  ssize_t ret;
  OSI_NO_INTR(ret = write(fd, protoBase64.c_str(), protoBase64.size()));
  if (ret == -1) {
    log::error("error writing to dumpsys fd: {} ({})", strerror(errno), errno);
  }
}

void BluetoothMetricsLogger::CutoffSession() {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_session_lock_);
  if (pimpl_->bluetooth_session_ != nullptr) {
    BluetoothSession* new_bt_session = new BluetoothSession(*pimpl_->bluetooth_session_);
    new_bt_session->clear_a2dp_session();
    new_bt_session->clear_rfcomm_session();
    LogBluetoothSessionEnd(DISCONNECT_REASON_METRICS_DUMP, 0);
    pimpl_->bluetooth_session_ = new_bt_session;
    pimpl_->bluetooth_session_start_time_ms_ = bluetooth::common::time_get_os_boottime_ms();
    pimpl_->a2dp_session_metrics_ = A2dpSessionMetrics();
  }
}

void BluetoothMetricsLogger::Build() {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_log_lock_);
  CutoffSession();
  BluetoothLog* bluetooth_log = pimpl_->bluetooth_log_;
  while (!pimpl_->bt_session_queue_->Empty() &&
         static_cast<size_t>(bluetooth_log->session_size()) <=
                 pimpl_->bt_session_queue_->Capacity()) {
    bluetooth_log->mutable_session()->AddAllocated(pimpl_->bt_session_queue_->Dequeue());
  }
  while (!pimpl_->pair_event_queue_->Empty() &&
         static_cast<size_t>(bluetooth_log->pair_event_size()) <=
                 pimpl_->pair_event_queue_->Capacity()) {
    bluetooth_log->mutable_pair_event()->AddAllocated(pimpl_->pair_event_queue_->Dequeue());
  }
  while (!pimpl_->scan_event_queue_->Empty() &&
         static_cast<size_t>(bluetooth_log->scan_event_size()) <=
                 pimpl_->scan_event_queue_->Capacity()) {
    bluetooth_log->mutable_scan_event()->AddAllocated(pimpl_->scan_event_queue_->Dequeue());
  }
  while (!pimpl_->wake_event_queue_->Empty() &&
         static_cast<size_t>(bluetooth_log->wake_event_size()) <=
                 pimpl_->wake_event_queue_->Capacity()) {
    bluetooth_log->mutable_wake_event()->AddAllocated(pimpl_->wake_event_queue_->Dequeue());
  }
  while (!pimpl_->bt_session_queue_->Empty() &&
         static_cast<size_t>(bluetooth_log->wake_event_size()) <=
                 pimpl_->wake_event_queue_->Capacity()) {
    bluetooth_log->mutable_wake_event()->AddAllocated(pimpl_->wake_event_queue_->Dequeue());
  }
  for (size_t i = 0; i < HeadsetProfileType_ARRAYSIZE; ++i) {
    int num_times_connected = pimpl_->headset_profile_connection_counts_[i];
    if (HeadsetProfileType_IsValid(i) && num_times_connected > 0) {
      HeadsetProfileConnectionStats* headset_profile_connection_stats =
              bluetooth_log->add_headset_profile_connection_stats();
      // Able to static_cast because HeadsetProfileType_IsValid(i) is true
      headset_profile_connection_stats->set_headset_profile_type(
              static_cast<HeadsetProfileType>(i));
      headset_profile_connection_stats->set_num_times_connected(num_times_connected);
    }
  }
  pimpl_->headset_profile_connection_counts_.fill(0);
}

void BluetoothMetricsLogger::ResetSession() {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_session_lock_);
  if (pimpl_->bluetooth_session_ != nullptr) {
    delete pimpl_->bluetooth_session_;
    pimpl_->bluetooth_session_ = nullptr;
  }
  pimpl_->bluetooth_session_start_time_ms_ = 0;
  pimpl_->a2dp_session_metrics_ = A2dpSessionMetrics();
}

void BluetoothMetricsLogger::ResetLog() {
  std::lock_guard<std::recursive_mutex> lock(pimpl_->bluetooth_log_lock_);
  pimpl_->bluetooth_log_->Clear();
}

void BluetoothMetricsLogger::Reset() {
  ResetSession();
  ResetLog();
  pimpl_->bt_session_queue_->Clear();
  pimpl_->pair_event_queue_->Clear();
  pimpl_->wake_event_queue_->Clear();
  pimpl_->scan_event_queue_->Clear();
}

void LogLinkLayerConnectionEvent(const RawAddress* address, uint32_t connection_handle,
                                 android::bluetooth::DirectionEnum direction, uint16_t link_type,
                                 uint32_t hci_cmd, uint16_t hci_event, uint16_t hci_ble_event,
                                 uint16_t cmd_status, uint16_t reason_code) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (address != nullptr) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(*address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(*address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField bytes_field(address != nullptr ? obfuscated_id.c_str() : nullptr,
                         address != nullptr ? obfuscated_id.size() : 0);
  int ret = stats_write(BLUETOOTH_LINK_LAYER_CONNECTION_EVENT, bytes_field, connection_handle,
                        direction, link_type, hci_cmd, hci_event, hci_ble_event, cmd_status,
                        reason_code, metric_id);
  if (ret < 0) {
    log::warn(
            "failed to log status 0x{:x}, reason 0x{:x} from cmd 0x{:x}, event "
            "0x{:x}, ble_event 0x{:x} for {}, handle {}, type 0x{:x}, error {}",
            cmd_status, reason_code, hci_cmd, hci_event, hci_ble_event, *address, connection_handle,
            link_type, ret);
  }
}

void LogHciTimeoutEvent(uint32_t hci_cmd) {
  int ret = stats_write(BLUETOOTH_HCI_TIMEOUT_REPORTED, static_cast<int64_t>(hci_cmd));
  if (ret < 0) {
    log::warn("failed for opcode 0x{:x}, error {}", hci_cmd, ret);
  }
}

void LogRemoteVersionInfo(uint16_t handle, uint8_t status, uint8_t version,
                          uint16_t manufacturer_name, uint16_t subversion) {
  int ret = stats_write(BLUETOOTH_REMOTE_VERSION_INFO_REPORTED, handle, status, version,
                        manufacturer_name, subversion);
  if (ret < 0) {
    log::warn(
            "failed for handle {}, status 0x{:x}, version 0x{:x}, "
            "manufacturer_name 0x{:x}, subversion 0x{:x}, error {}",
            handle, status, version, manufacturer_name, subversion, ret);
  }
}

void LogA2dpAudioUnderrunEvent(const RawAddress& address, uint64_t encoding_interval_millis,
                               int num_missing_pcm_bytes) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField bytes_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                         address.IsEmpty() ? 0 : obfuscated_id.size());
  int64_t encoding_interval_nanos = encoding_interval_millis * 1000000;
  int ret = stats_write(BLUETOOTH_A2DP_AUDIO_UNDERRUN_REPORTED, bytes_field,
                        encoding_interval_nanos, num_missing_pcm_bytes, metric_id);
  if (ret < 0) {
    log::warn(
            "failed for {}, encoding_interval_nanos {}, num_missing_pcm_bytes {}, "
            "error {}",
            address, encoding_interval_nanos, num_missing_pcm_bytes, ret);
  }
}

void LogA2dpAudioOverrunEvent(const RawAddress& address, uint64_t encoding_interval_millis,
                              int num_dropped_buffers, int num_dropped_encoded_frames,
                              int num_dropped_encoded_bytes) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField bytes_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                         address.IsEmpty() ? 0 : obfuscated_id.size());
  int64_t encoding_interval_nanos = encoding_interval_millis * 1000000;
  int ret = stats_write(BLUETOOTH_A2DP_AUDIO_OVERRUN_REPORTED, bytes_field, encoding_interval_nanos,
                        num_dropped_buffers, num_dropped_encoded_frames, num_dropped_encoded_bytes,
                        metric_id);
  if (ret < 0) {
    log::warn(
            "failed to log for {}, encoding_interval_nanos {}, num_dropped_buffers "
            "{}, num_dropped_encoded_frames {}, num_dropped_encoded_bytes {}, "
            "error {}",
            address, encoding_interval_nanos, num_dropped_buffers, num_dropped_encoded_frames,
            num_dropped_encoded_bytes, ret);
  }
}

void LogA2dpPlaybackEvent(const RawAddress& address, int playback_state, int audio_coding_mode) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField bytes_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                         address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_A2DP_PLAYBACK_STATE_CHANGED, bytes_field, playback_state,
                        audio_coding_mode, metric_id);
  if (ret < 0) {
    log::warn(
            "failed to log for {}, playback_state {}, audio_coding_mode {}, error "
            "{}",
            address, playback_state, audio_coding_mode, ret);
  }
}

void LogReadRssiResult(const RawAddress& address, uint16_t handle, uint32_t cmd_status,
                       int8_t rssi) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField bytes_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                         address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_DEVICE_RSSI_REPORTED, bytes_field, handle, cmd_status, rssi,
                        metric_id);
  if (ret < 0) {
    log::warn("failed for {}, handle {}, status 0x{:x}, rssi {} dBm, error {}", address, handle,
              cmd_status, rssi, ret);
  }
}

void LogReadFailedContactCounterResult(const RawAddress& address, uint16_t handle,
                                       uint32_t cmd_status, int32_t failed_contact_counter) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField bytes_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                         address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_DEVICE_FAILED_CONTACT_COUNTER_REPORTED, bytes_field, handle,
                        cmd_status, failed_contact_counter, metric_id);
  if (ret < 0) {
    log::warn(
            "failed for {}, handle {}, status 0x{:x}, failed_contact_counter {} "
            "packets, error {}",
            address, handle, cmd_status, failed_contact_counter, ret);
  }
}

void LogReadTxPowerLevelResult(const RawAddress& address, uint16_t handle, uint32_t cmd_status,
                               int32_t transmit_power_level) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField bytes_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                         address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_DEVICE_TX_POWER_LEVEL_REPORTED, bytes_field, handle, cmd_status,
                        transmit_power_level, metric_id);
  if (ret < 0) {
    log::warn(
            "failed for {}, handle {}, status 0x{:x}, transmit_power_level {} "
            "packets, error {}",
            address, handle, cmd_status, transmit_power_level, ret);
  }
}

void LogSmpPairingEvent(const RawAddress& address, uint8_t smp_cmd,
                        android::bluetooth::DirectionEnum direction, uint8_t smp_fail_reason) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField obfuscated_id_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                                 address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_SMP_PAIRING_EVENT_REPORTED, obfuscated_id_field, smp_cmd,
                        direction, smp_fail_reason, metric_id);
  if (ret < 0) {
    log::warn(
            "failed for {}, smp_cmd 0x{:x}, direction {}, smp_fail_reason 0x{:x}, "
            "error {}",
            address, smp_cmd, direction, smp_fail_reason, ret);
  }
}

void LogClassicPairingEvent(const RawAddress& address, uint16_t handle, uint32_t hci_cmd,
                            uint16_t hci_event, uint16_t cmd_status, uint16_t reason_code,
                            int64_t event_value) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField obfuscated_id_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                                 address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_CLASSIC_PAIRING_EVENT_REPORTED, obfuscated_id_field, handle,
                        hci_cmd, hci_event, cmd_status, reason_code, event_value, metric_id);
  if (ret < 0) {
    log::warn(
            "failed for {}, handle {}, hci_cmd 0x{:x}, hci_event 0x{:x}, "
            "cmd_status 0x{:x}, reason 0x{:x}, event_value {}, error {}",
            address, handle, hci_cmd, hci_event, cmd_status, reason_code, event_value, ret);
  }
}

void LogSdpAttribute(const RawAddress& address, uint16_t protocol_uuid, uint16_t attribute_id,
                     size_t attribute_size, const char* attribute_value) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField obfuscated_id_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                                 address.IsEmpty() ? 0 : obfuscated_id.size());
  BytesField attribute_field(attribute_value, attribute_size);
  int ret = stats_write(BLUETOOTH_SDP_ATTRIBUTE_REPORTED, obfuscated_id_field, protocol_uuid,
                        attribute_id, attribute_field, metric_id);
  if (ret < 0) {
    log::warn("failed for {}, protocol_uuid 0x{:x}, attribute_id 0x{:x}, error {}", address,
              protocol_uuid, attribute_id, ret);
  }
}

void LogSocketConnectionState(const RawAddress& address, int port, int type,
                              android::bluetooth::SocketConnectionstateEnum connection_state,
                              int64_t tx_bytes, int64_t rx_bytes, int uid, int server_port,
                              android::bluetooth::SocketRoleEnum socket_role) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField obfuscated_id_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                                 address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_SOCKET_CONNECTION_STATE_CHANGED, obfuscated_id_field, port, type,
                        connection_state, tx_bytes, rx_bytes, uid, server_port, socket_role,
                        metric_id);
  if (ret < 0) {
    log::warn(
            "failed for {}, port {}, type {}, state {}, tx_bytes {}, rx_bytes {}, "
            "uid {}, server_port {}, socket_role {}, error {}",
            address, port, type, connection_state, tx_bytes, rx_bytes, uid, server_port,
            socket_role, ret);
  }
}

void LogManufacturerInfo(const RawAddress& address,
                         android::bluetooth::AddressTypeEnum address_type,
                         android::bluetooth::DeviceInfoSrcEnum source_type,
                         const std::string& source_name, const std::string& manufacturer,
                         const std::string& model, const std::string& hardware_version,
                         const std::string& software_version) {
  std::string obfuscated_id;
  int metric_id = 0;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
    metric_id = bluetooth::shim::AllocateIdFromMetricIdAllocator(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField obfuscated_id_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                                 address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_DEVICE_INFO_REPORTED, obfuscated_id_field, source_type,
                        source_name.c_str(), manufacturer.c_str(), model.c_str(),
                        hardware_version.c_str(), software_version.c_str(), metric_id, address_type,
                        address.address[5], address.address[4], address.address[3]);
  if (ret < 0) {
    log::warn(
            "failed for {}, source_type {}, source_name {}, manufacturer {}, model "
            "{}, hardware_version {}, software_version {} MAC address type {} MAC "
            "address prefix {} {} {}, error {}",
            address, source_type, source_name, manufacturer, model, hardware_version,
            software_version, address_type, address.address[5], address.address[4],
            address.address[3], ret);
  }
}

void LogBluetoothHalCrashReason(const RawAddress& address, uint32_t error_code,
                                uint32_t vendor_error_code) {
  std::string obfuscated_id;
  if (!address.IsEmpty()) {
    obfuscated_id = AddressObfuscator::GetInstance()->Obfuscate(address);
  }
  // nullptr and size 0 represent missing value for obfuscated_id
  BytesField obfuscated_id_field(address.IsEmpty() ? nullptr : obfuscated_id.c_str(),
                                 address.IsEmpty() ? 0 : obfuscated_id.size());
  int ret = stats_write(BLUETOOTH_HAL_CRASH_REASON_REPORTED, 0, obfuscated_id_field, error_code,
                        vendor_error_code);
  if (ret < 0) {
    log::warn("failed for {}, error_code 0x{:x}, vendor_error_code 0x{:x}, error {}", address,
              error_code, vendor_error_code, ret);
  }
}

void LogLeAudioConnectionSessionReported(
        int32_t group_size, int32_t group_metric_id, int64_t connection_duration_nanos,
        const std::vector<int64_t>& device_connecting_offset_nanos,
        const std::vector<int64_t>& device_connected_offset_nanos,
        const std::vector<int64_t>& device_connection_duration_nanos,
        const std::vector<int32_t>& device_connection_status,
        const std::vector<int32_t>& device_disconnection_status,
        const std::vector<RawAddress>& device_address,
        const std::vector<int64_t>& streaming_offset_nanos,
        const std::vector<int64_t>& streaming_duration_nanos,
        const std::vector<int32_t>& streaming_context_type) {
  std::vector<int32_t> device_metric_id(device_address.size());
  for (uint64_t i = 0; i < device_address.size(); i++) {
    if (!device_address[i].IsEmpty()) {
      device_metric_id[i] = bluetooth::shim::AllocateIdFromMetricIdAllocator(device_address[i]);
    } else {
      device_metric_id[i] = 0;
    }
  }
  int ret = stats_write(LE_AUDIO_CONNECTION_SESSION_REPORTED, group_size, group_metric_id,
                        connection_duration_nanos, device_connecting_offset_nanos,
                        device_connected_offset_nanos, device_connection_duration_nanos,
                        device_connection_status, device_disconnection_status, device_metric_id,
                        streaming_offset_nanos, streaming_duration_nanos, streaming_context_type);
  if (ret < 0) {
    log::warn(
            "failed for group {}device_connecting_offset_nanos[{}], "
            "device_connected_offset_nanos[{}], "
            "device_connection_duration_nanos[{}], device_connection_status[{}], "
            "device_disconnection_status[{}], device_metric_id[{}], "
            "streaming_offset_nanos[{}], streaming_duration_nanos[{}], "
            "streaming_context_type[{}]",
            group_metric_id, device_connecting_offset_nanos.size(),
            device_connected_offset_nanos.size(), device_connection_duration_nanos.size(),
            device_connection_status.size(), device_disconnection_status.size(),
            device_metric_id.size(), streaming_offset_nanos.size(), streaming_duration_nanos.size(),
            streaming_context_type.size());
  }
}

void LogLeAudioBroadcastSessionReported(int64_t duration_nanos) {
  int ret = stats_write(LE_AUDIO_BROADCAST_SESSION_REPORTED, duration_nanos);
  if (ret < 0) {
    log::warn("failed for duration={}", duration_nanos);
  }
}

}  // namespace common

}  // namespace bluetooth
