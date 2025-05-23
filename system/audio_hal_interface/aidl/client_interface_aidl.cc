/*
 * Copyright 2022 The Android Open Source Project
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

#define LOG_TAG "BTAudioClientAIDL"

#include "aidl/client_interface_aidl.h"

#include <android/binder_manager.h>
#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "bta/ag/bta_ag_int.h"

const uint8_t kFetchAudioProviderRetryNumber = 3;

namespace bluetooth {
namespace audio {
namespace aidl {

std::ostream& operator<<(std::ostream& os, const BluetoothAudioCtrlAck& ack) {
  switch (ack) {
    case BluetoothAudioCtrlAck::SUCCESS_FINISHED:
      return os << "SUCCESS_FINISHED";
    case BluetoothAudioCtrlAck::PENDING:
      return os << "PENDING";
    case BluetoothAudioCtrlAck::FAILURE_UNSUPPORTED:
      return os << "FAILURE_UNSUPPORTED";
    case BluetoothAudioCtrlAck::FAILURE_BUSY:
      return os << "FAILURE_BUSY";
    case BluetoothAudioCtrlAck::FAILURE_DISCONNECTING:
      return os << "FAILURE_DISCONNECTING";
    case BluetoothAudioCtrlAck::FAILURE:
      return os << "FAILURE";
    default:
      return os << "UNDEFINED " << static_cast<int8_t>(ack);
  }
}

BluetoothAudioClientInterface::BluetoothAudioClientInterface(IBluetoothTransportInstance* instance)
    : provider_(nullptr),
      provider_factory_(nullptr),
      session_started_(false),
      data_mq_(nullptr),
      transport_(instance),
      latency_modes_({LatencyMode::FREE}) {
  death_recipient_ =
          ::ndk::ScopedAIBinder_DeathRecipient(AIBinder_DeathRecipient_new(binderDiedCallbackAidl));
}

bool BluetoothAudioClientInterface::IsValid() const { return provider_ != nullptr; }

bool BluetoothAudioClientInterface::is_aidl_available() {
  return AServiceManager_isDeclared(kDefaultAudioProviderFactoryInterface.c_str());
}

std::vector<AudioCapabilities> BluetoothAudioClientInterface::GetAudioCapabilities() const {
  return capabilities_;
}

std::vector<AudioCapabilities> BluetoothAudioClientInterface::GetAudioCapabilities(
        SessionType session_type) {
  std::vector<AudioCapabilities> capabilities(0);
  if (!is_aidl_available()) {
    return capabilities;
  }
  auto provider_factory = IBluetoothAudioProviderFactory::fromBinder(::ndk::SpAIBinder(
          AServiceManager_waitForService(kDefaultAudioProviderFactoryInterface.c_str())));

  if (provider_factory == nullptr) {
    log::error("can't get capability from unknown factory");
    return capabilities;
  }

  auto aidl_retval = provider_factory->getProviderCapabilities(session_type, &capabilities);
  if (!aidl_retval.isOk()) {
    log::fatal("BluetoothAudioHal::getProviderCapabilities failure: {}",
               aidl_retval.getDescription());
  }
  return capabilities;
}

std::optional<IBluetoothAudioProviderFactory::ProviderInfo>
BluetoothAudioClientInterface::GetProviderInfo(
        SessionType session_type,
        std::shared_ptr<IBluetoothAudioProviderFactory> provider_factory) {
  if (!is_aidl_available()) {
    return std::nullopt;
  }

  if (provider_factory == nullptr) {
    provider_factory = IBluetoothAudioProviderFactory::fromBinder(::ndk::SpAIBinder(
            AServiceManager_waitForService(kDefaultAudioProviderFactoryInterface.c_str())));
  }

  if (provider_factory == nullptr) {
    log::error("can't get provider info from unknown factory");
    return std::nullopt;
  }

  std::optional<IBluetoothAudioProviderFactory::ProviderInfo> provider_info = {};
  auto aidl_retval = provider_factory->getProviderInfo(session_type, &provider_info);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal::getProviderInfo failure: {}", aidl_retval.getDescription());
    return std::nullopt;
  }

  return provider_info;
}

void BluetoothAudioClientInterface::FetchAudioProvider() {
  if (!is_aidl_available()) {
    log::error("aidl is not supported on this platform.");
    return;
  }
  if (provider_ != nullptr) {
    log::warn("refetch");
  }
  // Retry if audioserver restarts in the middle of fetching.
  // When audioserver restarts, IBluetoothAudioProviderFactory service is also
  // re-registered, so we need to re-fetch the service.
  for (int retry_no = 0; retry_no < kFetchAudioProviderRetryNumber; ++retry_no) {
    auto provider_factory = IBluetoothAudioProviderFactory::fromBinder(::ndk::SpAIBinder(
            AServiceManager_waitForService(kDefaultAudioProviderFactoryInterface.c_str())));

    if (provider_factory == nullptr) {
      log::error("can't get capability from unknown factory");
      return;
    }

    capabilities_.clear();
    auto aidl_retval =
            provider_factory->getProviderCapabilities(transport_->GetSessionType(), &capabilities_);
    if (!aidl_retval.isOk()) {
      log::error("BluetoothAudioHal::getProviderCapabilities failure: {}, retry number {}",
                 aidl_retval.getDescription(), retry_no + 1);
      continue;
    }
    if (capabilities_.empty()) {
      log::warn("SessionType={} Not supported by BluetoothAudioHal",
                toString(transport_->GetSessionType()));
      return;
    }
    log::info("BluetoothAudioHal SessionType={} has {} AudioCapabilities",
              toString(transport_->GetSessionType()), capabilities_.size());

    aidl_retval = provider_factory->openProvider(transport_->GetSessionType(), &provider_);
    if (!aidl_retval.isOk() || provider_ == nullptr) {
      log::error("BluetoothAudioHal::openProvider failure: {}, retry number {}",
                 aidl_retval.getDescription(), retry_no + 1);
    } else {
      provider_factory_ = std::move(provider_factory);
      break;
    }
  }
  log::assert_that(provider_factory_ != nullptr, "assert failed: provider_factory_ != nullptr");
  log::assert_that(provider_ != nullptr, "assert failed: provider_ != nullptr");

  binder_status_t binder_status =
          AIBinder_linkToDeath(provider_factory_->asBinder().get(), death_recipient_.get(), this);
  if (binder_status != STATUS_OK) {
    log::error("Failed to linkToDeath {}", static_cast<int>(binder_status));
  }

  log::info("IBluetoothAudioProvidersFactory::openProvider() returned {}{}",
            std::format_ptr(provider_.get()), (provider_->isRemote() ? " (remote)" : " (local)"));
}

BluetoothAudioSinkClientInterface::BluetoothAudioSinkClientInterface(
        IBluetoothSinkTransportInstance* sink)
    : BluetoothAudioClientInterface{sink}, sink_(sink) {
  FetchAudioProvider();
}

BluetoothAudioSinkClientInterface::~BluetoothAudioSinkClientInterface() {
  if (provider_factory_ != nullptr) {
    AIBinder_unlinkToDeath(provider_factory_->asBinder().get(), death_recipient_.get(), nullptr);
  }
}

BluetoothAudioSourceClientInterface::BluetoothAudioSourceClientInterface(
        IBluetoothSourceTransportInstance* source)
    : BluetoothAudioClientInterface{source}, source_(source) {
  FetchAudioProvider();
}

BluetoothAudioSourceClientInterface::~BluetoothAudioSourceClientInterface() {
  if (provider_factory_ != nullptr) {
    AIBinder_unlinkToDeath(provider_factory_->asBinder().get(), death_recipient_.get(), nullptr);
  }
}

void BluetoothAudioClientInterface::binderDiedCallbackAidl(void* ptr) {
  log::warn("restarting connection with new Audio Hal");
  auto client = static_cast<BluetoothAudioClientInterface*>(ptr);
  if (client == nullptr) {
    log::error("null audio HAL died!");
    return;
  }
  client->RenewAudioProviderAndSession();
}

bool BluetoothAudioClientInterface::UpdateAudioConfig(const AudioConfiguration& audio_config) {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  bool is_software_session =
          (transport_->GetSessionType() == SessionType::A2DP_SOFTWARE_ENCODING_DATAPATH ||
           transport_->GetSessionType() == SessionType::HEARING_AID_SOFTWARE_ENCODING_DATAPATH ||
           transport_->GetSessionType() == SessionType::LE_AUDIO_SOFTWARE_ENCODING_DATAPATH ||
           transport_->GetSessionType() == SessionType::LE_AUDIO_SOFTWARE_DECODING_DATAPATH ||
           transport_->GetSessionType() ==
                   SessionType::LE_AUDIO_BROADCAST_SOFTWARE_ENCODING_DATAPATH ||
           (bta_ag_is_sco_managed_by_audio() &&
            (transport_->GetSessionType() == SessionType::HFP_SOFTWARE_ENCODING_DATAPATH ||
             transport_->GetSessionType() == SessionType::HFP_SOFTWARE_DECODING_DATAPATH)));
  bool is_a2dp_offload_session =
          (transport_->GetSessionType() == SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
  bool is_leaudio_unicast_offload_session =
          (transport_->GetSessionType() ==
                   SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
           transport_->GetSessionType() ==
                   SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH);
  bool is_leaudio_broadcast_offload_session =
          (transport_->GetSessionType() ==
           SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH);
  auto audio_config_tag = audio_config.getTag();
  bool is_software_audio_config =
          (is_software_session && audio_config_tag == AudioConfiguration::pcmConfig);
  bool is_a2dp_offload_audio_config =
          (is_a2dp_offload_session && (audio_config_tag == AudioConfiguration::a2dpConfig ||
                                       audio_config_tag == AudioConfiguration::a2dp));
  bool is_leaudio_unicast_offload_audio_config =
          (is_leaudio_unicast_offload_session &&
           audio_config_tag == AudioConfiguration::leAudioConfig);
  bool is_leaudio_broadcast_offload_audio_config =
          (is_leaudio_broadcast_offload_session &&
           audio_config_tag == AudioConfiguration::leAudioBroadcastConfig);
  bool is_hfp_offload_audio_config =
          (bta_ag_is_sco_managed_by_audio() &&
           transport_->GetSessionType() == SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH &&
           audio_config_tag == AudioConfiguration::hfpConfig);
  if (!is_software_audio_config && !is_a2dp_offload_audio_config &&
      !is_leaudio_unicast_offload_audio_config && !is_leaudio_broadcast_offload_audio_config &&
      !is_hfp_offload_audio_config) {
    return false;
  }
  transport_->UpdateAudioConfiguration(audio_config);

  if (provider_ == nullptr) {
    log::info("BluetoothAudioHal nullptr, update it as session started");
    return true;
  }

  if (!session_started_) {
    log::info("BluetoothAudioHal session has not started");
    return true;
  }

  auto aidl_retval = provider_->updateAudioConfiguration(audio_config);
  if (!aidl_retval.isOk()) {
    if (audio_config.getTag() != transport_->GetAudioConfiguration().getTag()) {
      log::warn(
              "BluetoothAudioHal audio config type: {} doesn't "
              "match provider's audio config type: {}",
              ::aidl::android::hardware::bluetooth::audio::toString(audio_config.getTag()),
              ::aidl::android::hardware::bluetooth::audio::toString(
                      transport_->GetAudioConfiguration().getTag()));
    } else {
      log::warn("BluetoothAudioHal is not ready: {} ", aidl_retval.getDescription());
    }
  }
  return true;
}

bool BluetoothAudioClientInterface::SetAllowedLatencyModes(std::vector<LatencyMode> latency_modes) {
  if (provider_ == nullptr) {
    log::info("BluetoothAudioHal nullptr");
    return false;
  }

  if (latency_modes.empty()) {
    latency_modes_.clear();
    latency_modes_.push_back(LatencyMode::FREE);
  } else {
    /* Ensure that FREE is always included and remove duplicates if any */
    std::set<LatencyMode> temp_set(latency_modes.begin(), latency_modes.end());
    temp_set.insert(LatencyMode::FREE);
    latency_modes_.clear();
    latency_modes_.assign(temp_set.begin(), temp_set.end());
  }

  for (auto latency_mode : latency_modes) {
    log::info("Latency mode allowed: {}",
              ::aidl::android::hardware::bluetooth::audio::toString(latency_mode));
  }

  /* Low latency mode is used if modes other than FREE are present */
  bool allowed = (latency_modes_.size() > 1);
  log::info("Latency mode allowed: {}", allowed);
  auto aidl_retval = provider_->setLowLatencyModeAllowed(allowed);
  if (!aidl_retval.isOk()) {
    log::warn(
            "BluetoothAudioHal is not ready: {}. latency_modes_ is saved and it "
            "will be sent to BluetoothAudioHal at StartSession.",
            aidl_retval.getDescription());
  }
  return true;
}

int BluetoothAudioClientInterface::StartSession() {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    session_started_ = false;
    return -EINVAL;
  }
  if (session_started_) {
    log::error("session started already");
    return -EBUSY;
  }

  std::shared_ptr<IBluetoothAudioPort> stack_if =
          ndk::SharedRefBase::make<BluetoothAudioPortImpl>(transport_, provider_);

  std::unique_ptr<DataMQ> data_mq;
  DataMQDesc mq_desc;

  auto aidl_retval = provider_->startSession(stack_if, transport_->GetAudioConfiguration(),
                                             latency_modes_, &mq_desc);
  if (!aidl_retval.isOk()) {
    if (aidl_retval.getExceptionCode() == EX_ILLEGAL_ARGUMENT) {
      log::error("BluetoothAudioHal Error: {}, audioConfig={}", aidl_retval.getDescription(),
                 transport_->GetAudioConfiguration().toString());
    } else {
      log::fatal("BluetoothAudioHal failure: {}", aidl_retval.getDescription());
    }
    return -EPROTO;
  }
  data_mq.reset(new DataMQ(mq_desc));

  if (data_mq && data_mq->isValid()) {
    data_mq_ = std::move(data_mq);
  } else if (transport_->GetSessionType() == SessionType::A2DP_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
             transport_->GetSessionType() ==
                     SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH ||
             transport_->GetSessionType() ==
                     SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
             transport_->GetSessionType() ==
                     SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
             (bta_ag_is_sco_managed_by_audio() &&
              transport_->GetSessionType() == SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH)) {
    transport_->ResetPresentationPosition();
    session_started_ = true;
    return 0;
  }
  if (data_mq_ && data_mq_->isValid()) {
    transport_->ResetPresentationPosition();
    session_started_ = true;
    return 0;
  } else {
    if (!data_mq_) {
      log::error("Failed to obtain audio data path");
    }
    if (data_mq_ && !data_mq_->isValid()) {
      log::error("Audio data path is invalid");
    }
    session_started_ = false;
    return -EIO;
  }
}

void BluetoothAudioClientInterface::StreamStarted(const BluetoothAudioCtrlAck& ack) {
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return;
  }
  if (ack == BluetoothAudioCtrlAck::PENDING) {
    log::info("{} ignored", ack);
    return;
  }

  auto status = BluetoothAudioCtrlAckToHalStatus(ack);
  auto aidl_retval = provider_->streamStarted(status);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", aidl_retval.getDescription());
  }
}

void BluetoothAudioClientInterface::StreamSuspended(const BluetoothAudioCtrlAck& ack) {
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return;
  }
  if (ack == BluetoothAudioCtrlAck::PENDING) {
    log::info("{} ignored", ack);
    return;
  }

  auto status = BluetoothAudioCtrlAckToHalStatus(ack);
  auto aidl_retval = provider_->streamSuspended(status);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", aidl_retval.getDescription());
  }
}

int BluetoothAudioClientInterface::EndSession() {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  if (!session_started_) {
    log::info("session ended already");
    return 0;
  }

  session_started_ = false;
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return -EINVAL;
  }
  data_mq_ = nullptr;

  auto aidl_retval = provider_->endSession();

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", aidl_retval.getDescription());
    return -EPROTO;
  }
  return 0;
}

void BluetoothAudioClientInterface::FlushAudioData() {
  if (transport_->GetSessionType() == SessionType::LE_AUDIO_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
      transport_->GetSessionType() == SessionType::LE_AUDIO_HARDWARE_OFFLOAD_DECODING_DATAPATH ||
      transport_->GetSessionType() ==
              SessionType::LE_AUDIO_BROADCAST_HARDWARE_OFFLOAD_ENCODING_DATAPATH ||
      (bta_ag_is_sco_managed_by_audio() &&
       transport_->GetSessionType() == SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH)) {
    return;
  }

  if (data_mq_ == nullptr || !data_mq_->isValid()) {
    log::warn("data_mq_ invalid");
    return;
  }
  size_t size = data_mq_->availableToRead();
  if (size == 0) {
    return;
  }

  std::vector<MqDataType> buffer(size);

  if (data_mq_->read(buffer.data(), size) != size) {
    log::warn("failed to flush data queue!");
  }
}

size_t BluetoothAudioSinkClientInterface::ReadAudioData(uint8_t* p_buf, uint32_t len) {
  if (!IsValid()) {
    log::error("BluetoothAudioHal is not valid");
    return 0;
  }
  if (p_buf == nullptr || len == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> guard(internal_mutex_);

  size_t total_read = 0;
  int timeout_ms = kDefaultDataReadTimeoutMs;
  do {
    if (data_mq_ == nullptr || !data_mq_->isValid()) {
      break;
    }

    size_t avail_to_read = data_mq_->availableToRead();
    if (avail_to_read) {
      if (avail_to_read > len - total_read) {
        avail_to_read = len - total_read;
      }
      if (data_mq_->read(reinterpret_cast<MqDataType*>(p_buf) + total_read, avail_to_read) == 0) {
        log::warn("len={} total_read={} failed", len, total_read);
        break;
      }
      total_read += avail_to_read;
    } else if (timeout_ms >= kDefaultDataReadPollIntervalMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kDefaultDataReadPollIntervalMs));
      timeout_ms -= kDefaultDataReadPollIntervalMs;
      continue;
    } else {
      log::warn("{}/{} no data {} ms", len - total_read, len,
                kDefaultDataReadTimeoutMs - timeout_ms);
      break;
    }
  } while (total_read < len);

  if (timeout_ms < (kDefaultDataReadTimeoutMs - kDefaultDataReadPollIntervalMs) &&
      timeout_ms >= kDefaultDataReadPollIntervalMs) {
    log::verbose("underflow {} -> {} read {} ms", len, total_read,
                 kDefaultDataReadTimeoutMs - timeout_ms);
  } else {
    log::verbose("{} -> {} read", len, total_read);
  }

  sink_->LogBytesRead(total_read);
  return total_read;
}

void BluetoothAudioClientInterface::RenewAudioProviderAndSession() {
  // NOTE: must be invoked on the same thread where this
  // BluetoothAudioClientInterface is running
  FetchAudioProvider();

  if (session_started_) {
    log::info("Restart the session while audio HAL recovering");
    session_started_ = false;

    StartSession();
  }
}

size_t BluetoothAudioSourceClientInterface::WriteAudioData(const uint8_t* p_buf, uint32_t len) {
  if (!IsValid()) {
    log::error("BluetoothAudioHal is not valid");
    return 0;
  }
  if (p_buf == nullptr || len == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> guard(internal_mutex_);

  size_t total_written = 0;
  int timeout_ms = kDefaultDataWriteTimeoutMs;
  do {
    if (data_mq_ == nullptr || !data_mq_->isValid()) {
      break;
    }

    size_t avail_to_write = data_mq_->availableToWrite();
    if (avail_to_write) {
      if (avail_to_write > len - total_written) {
        avail_to_write = len - total_written;
      }
      if (data_mq_->write((const MqDataType*)p_buf + total_written, avail_to_write) == 0) {
        log::warn("len={} total_written={} failed", len, total_written);
        break;
      }
      total_written += avail_to_write;
    } else if (timeout_ms >= kDefaultDataWritePollIntervalMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kDefaultDataWritePollIntervalMs));
      timeout_ms -= kDefaultDataWritePollIntervalMs;
      continue;
    } else {
      log::warn("{}/{} no data {} ms", len - total_written, len,
                kDefaultDataWriteTimeoutMs - timeout_ms);
      break;
    }
  } while (total_written < len);

  if (timeout_ms < (kDefaultDataWriteTimeoutMs - kDefaultDataWritePollIntervalMs) &&
      timeout_ms >= kDefaultDataWritePollIntervalMs) {
    log::verbose("underflow {} -> {} read {} ms", len, total_written,
                 kDefaultDataWriteTimeoutMs - timeout_ms);
  } else {
    log::verbose("{} -> {} written", len, total_written);
  }

  source_->LogBytesWritten(total_written);
  return total_written;
}

void BluetoothAudioClientInterface::SetCodecPriority(CodecId codec_id, int32_t priority) {
  log::assert_that(provider_ != nullptr, "assert failed: provider_ != nullptr");
  auto aidl_retval = provider_->setCodecPriority(codec_id, priority);
  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal::setCodecPriority failure: {}", aidl_retval.getDescription());
  }
}

std::vector<IBluetoothAudioProvider::LeAudioAseConfigurationSetting>
BluetoothAudioClientInterface::GetLeAudioAseConfiguration(
        std::optional<
                std::vector<std::optional<IBluetoothAudioProvider::LeAudioDeviceCapabilities>>>&
                remoteSinkAudioCapabilities,
        std::optional<
                std::vector<std::optional<IBluetoothAudioProvider::LeAudioDeviceCapabilities>>>&
                remoteSourceAudioCapabilities,
        std::vector<IBluetoothAudioProvider::LeAudioConfigurationRequirement>& requirements) {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  log::assert_that(provider_ != nullptr, "assert failed: provider_ != nullptr");

  std::vector<IBluetoothAudioProvider::LeAudioAseConfigurationSetting> configurations;
  auto aidl_retval = provider_->getLeAudioAseConfiguration(remoteSinkAudioCapabilities,
                                                           remoteSourceAudioCapabilities,
                                                           requirements, &configurations);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal::getLeAudioAseConfiguration failure: {}",
               aidl_retval.getDescription());
  } else {
    log::info(
            "BluetoothAudioHal::getLeAudioAseConfiguration returned {} "
            "configurations.",
            configurations.size());
  }

  return configurations;
}

IBluetoothAudioProvider::LeAudioAseQosConfigurationPair
BluetoothAudioClientInterface::getLeAudioAseQosConfiguration(
        IBluetoothAudioProvider::LeAudioAseQosConfigurationRequirement& qosRequirement) {
  log::assert_that(provider_ != nullptr, "assert failed: provider_ != nullptr");

  IBluetoothAudioProvider::LeAudioAseQosConfigurationPair qos_configuration;
  auto aidl_retval = provider_->getLeAudioAseQosConfiguration(qosRequirement, &qos_configuration);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal::getLeAudioAseQosConfiguration failure: {}",
               aidl_retval.getDescription());
  }
  return qos_configuration;
}

void BluetoothAudioClientInterface::onSinkAseMetadataChanged(
        IBluetoothAudioProvider::AseState state, int32_t cigId, int32_t cisId,
        std::optional<std::vector<std::optional<MetadataLtv>>>& metadata) {
  log::assert_that(provider_ != nullptr, "assert failed: provider_ != nullptr");

  auto aidl_retval = provider_->onSinkAseMetadataChanged(state, cigId, cisId, metadata);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal::onSinkAseMetadataChanged failure: {}",
               aidl_retval.getDescription());
  }
}

void BluetoothAudioClientInterface::onSourceAseMetadataChanged(
        IBluetoothAudioProvider::AseState state, int32_t cigId, int32_t cisId,
        std::optional<std::vector<std::optional<MetadataLtv>>>& metadata) {
  log::assert_that(provider_ != nullptr, "assert failed: provider_ != nullptr");

  auto aidl_retval = provider_->onSourceAseMetadataChanged(state, cigId, cisId, metadata);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal::onSourceAseMetadataChanged failure: {}",
               aidl_retval.getDescription());
  }
}

IBluetoothAudioProvider::LeAudioBroadcastConfigurationSetting
BluetoothAudioClientInterface::getLeAudioBroadcastConfiguration(
        const std::optional<
                std::vector<std::optional<IBluetoothAudioProvider::LeAudioDeviceCapabilities>>>&
                remoteSinkAudioCapabilities,
        const IBluetoothAudioProvider::LeAudioBroadcastConfigurationRequirement& requirement) {
  std::lock_guard<std::mutex> guard(internal_mutex_);
  log::assert_that(provider_ != nullptr, "assert failed: provider_ != nullptr");

  IBluetoothAudioProvider::LeAudioBroadcastConfigurationSetting setting;
  auto aidl_retval = provider_->getLeAudioBroadcastConfiguration(remoteSinkAudioCapabilities,
                                                                 requirement, &setting);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal::getLeAudioBroadcastConfiguration failure: {}",
               aidl_retval.getDescription());
  }

  return setting;
}

}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth
