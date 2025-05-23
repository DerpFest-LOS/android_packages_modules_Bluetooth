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

#include "aidl/a2dp/client_interface_aidl.h"

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
namespace a2dp {

BluetoothAudioClientInterface::BluetoothAudioClientInterface(IBluetoothTransportInstance* instance)
    : provider_(nullptr),
      provider_factory_(nullptr),
      session_started_(false),
      data_mq_(nullptr),
      transport_(instance),
      latency_modes_({LatencyMode::FREE}) {
  death_recipient_ =
          ::ndk::ScopedAIBinder_DeathRecipient(AIBinder_DeathRecipient_new(binderDiedCallbackAidl));
  FetchAudioProvider();
}

BluetoothAudioClientInterface::~BluetoothAudioClientInterface() {
  if (provider_factory_ != nullptr) {
    AIBinder_unlinkToDeath(provider_factory_->asBinder().get(), death_recipient_.get(), nullptr);
  }
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

std::optional<A2dpConfiguration> BluetoothAudioClientInterface::GetA2dpConfiguration(
        std::vector<A2dpRemoteCapabilities> const& remote_capabilities,
        A2dpConfigurationHint const& hint) const {
  if (!is_aidl_available()) {
    return std::nullopt;
  }

  if (provider_ == nullptr) {
    log::error("can't get a2dp configuration from unknown provider");
    return std::nullopt;
  }

  std::optional<A2dpConfiguration> configuration = std::nullopt;
  auto aidl_retval = provider_->getA2dpConfiguration(remote_capabilities, hint, &configuration);

  if (!aidl_retval.isOk()) {
    log::error("getA2dpConfiguration failure: {}", aidl_retval.getDescription());
    return std::nullopt;
  }

  return configuration;
}

std::optional<A2dpStatus> BluetoothAudioClientInterface::ParseA2dpConfiguration(
        const CodecId& codec_id, const std::vector<uint8_t>& configuration,
        CodecParameters* codec_parameters) const {
  A2dpStatus a2dp_status;

  if (provider_ == nullptr) {
    log::error("can not parse A2DP configuration because of unknown provider");
    return std::nullopt;
  }

  auto aidl_retval = provider_->parseA2dpConfiguration(codec_id, configuration, codec_parameters,
                                                       &a2dp_status);

  if (!aidl_retval.isOk()) {
    log::error("parseA2dpConfiguration failure: {}", aidl_retval.getDescription());
    return std::nullopt;
  }

  return std::make_optional(a2dp_status);
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

void BluetoothAudioClientInterface::StreamStarted(const Status& ack) {
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return;
  }
  if (ack == Status::PENDING) {
    log::info("{} ignored", ack);
    return;
  }

  auto status = StatusToHalStatus(ack);
  auto aidl_retval = provider_->streamStarted(status);

  if (!aidl_retval.isOk()) {
    log::error("BluetoothAudioHal failure: {}", aidl_retval.getDescription());
  }
}

void BluetoothAudioClientInterface::StreamSuspended(const Status& ack) {
  if (provider_ == nullptr) {
    log::error("BluetoothAudioHal nullptr");
    return;
  }
  if (ack == Status::PENDING) {
    log::info("{} ignored", ack);
    return;
  }

  auto status = StatusToHalStatus(ack);
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

size_t BluetoothAudioClientInterface::ReadAudioData(uint8_t* p_buf, uint32_t len) {
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

  transport_->LogBytesRead(total_read);
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

}  // namespace a2dp
}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth
