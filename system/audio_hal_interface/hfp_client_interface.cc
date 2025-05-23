/*
 * Copyright 2023 The Android Open Source Project
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

#include <cstdint>

#define LOG_TAG "BTAudioClientHfpStub"

#include <bluetooth/log.h>

#include "aidl/client_interface_aidl.h"
#include "aidl/hfp_client_interface_aidl.h"
#include "hal_version_manager.h"
#include "hfp_client_interface.h"
#include "osi/include/properties.h"

using ::bluetooth::audio::aidl::hfp::HfpDecodingTransport;
using ::bluetooth::audio::aidl::hfp::HfpEncodingTransport;
using AudioConfiguration = ::aidl::android::hardware::bluetooth::audio::AudioConfiguration;
using ::aidl::android::hardware::bluetooth::audio::ChannelMode;
using ::aidl::android::hardware::bluetooth::audio::CodecId;
using ::aidl::android::hardware::bluetooth::audio::HfpConfiguration;
using ::aidl::android::hardware::bluetooth::audio::PcmConfiguration;

namespace bluetooth {
namespace audio {
namespace hfp {

static aidl::BluetoothAudioSourceClientInterface* get_decode_client_interface() {
  return HfpDecodingTransport::active_hal_interface;
}

static aidl::BluetoothAudioSinkClientInterface* get_encode_client_interface() {
  return HfpEncodingTransport::active_hal_interface;
}

static HfpDecodingTransport* get_decode_transport_instance() {
  return HfpDecodingTransport::instance_;
}

static HfpDecodingTransport* get_encode_transport_instance() {
  return HfpDecodingTransport::instance_;
}

static PcmConfiguration get_default_pcm_configuration() {
  PcmConfiguration pcm_config{
          .sampleRateHz = 8000,
          .channelMode = ChannelMode::MONO,
          .bitsPerSample = 16,
          .dataIntervalUs = 7500,
  };
  return pcm_config;
}

static HfpConfiguration get_default_hfp_configuration() {
  HfpConfiguration hfp_config{
          .codecId = CodecId::Core::CVSD,
          .connectionHandle = 6,
          .nrec = false,
          .controllerCodec = true,
  };
  return hfp_config;
}

static CodecId sco_codec_to_hal_codec(tBTA_AG_UUID_CODEC sco_codec) {
  switch (sco_codec) {
    case tBTA_AG_UUID_CODEC::UUID_CODEC_LC3:
      return CodecId::Core::LC3;
    case tBTA_AG_UUID_CODEC::UUID_CODEC_MSBC:
      return CodecId::Core::MSBC;
    case tBTA_AG_UUID_CODEC::UUID_CODEC_CVSD:
      return CodecId::Core::CVSD;
    default:
      log::warn("Unknown sco_codec {}, defaulting to vendor codec",
                bta_ag_uuid_codec_text(sco_codec));
      return CodecId::Vendor();
  }
}

static AudioConfiguration offload_config_to_hal_audio_config(
        const ::hfp::offload_config& offload_config) {
  HfpConfiguration hfp_config{
          .codecId = sco_codec_to_hal_codec(offload_config.sco_codec),
          .connectionHandle = offload_config.connection_handle,
          .nrec = offload_config.is_nrec,
          .controllerCodec = offload_config.is_controller_codec,
  };
  return AudioConfiguration(hfp_config);
}

static AudioConfiguration pcm_config_to_hal_audio_config(const ::hfp::pcm_config& pcm_config) {
  PcmConfiguration config = get_default_pcm_configuration();
  config.sampleRateHz = pcm_config.sample_rate_hz;
  return AudioConfiguration(config);
}

static bool is_aidl_support_hfp() {
  return HalVersionManager::GetHalTransport() == BluetoothAudioHalTransport::AIDL &&
         HalVersionManager::GetHalVersion() >= BluetoothAudioHalVersion::VERSION_AIDL_V4;
}

// Parent client implementation
HfpClientInterface* HfpClientInterface::interface = nullptr;
HfpClientInterface* HfpClientInterface::Get() {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return nullptr;
  }
  if (HfpClientInterface::interface == nullptr) {
    HfpClientInterface::interface = new HfpClientInterface();
  }
  return HfpClientInterface::interface;
}

// Decode client implementation
void HfpClientInterface::Decode::Cleanup() {
  log::info("decode");
  StopSession();
  if (HfpDecodingTransport::instance_) {
    delete HfpDecodingTransport::software_hal_interface;
    HfpDecodingTransport::software_hal_interface = nullptr;
    delete HfpDecodingTransport::instance_;
    HfpDecodingTransport::instance_ = nullptr;
  }
}

void HfpClientInterface::Decode::StartSession() {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }
  log::info("decode");
  AudioConfiguration audio_config;
  audio_config.set<AudioConfiguration::pcmConfig>(get_default_pcm_configuration());
  if (!get_decode_client_interface()->UpdateAudioConfig(audio_config)) {
    log::error("cannot update audio config to HAL");
    return;
  }
  auto instance = aidl::hfp::HfpEncodingTransport::instance_;
  instance->ResetPendingCmd();
  get_decode_client_interface()->StartSession();
}

void HfpClientInterface::Decode::StopSession() {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }
  log::info("decode");
  get_decode_client_interface()->EndSession();
  if (get_decode_transport_instance()) {
    get_decode_transport_instance()->ResetPendingCmd();
    get_decode_transport_instance()->ResetPresentationPosition();
  }
}

void HfpClientInterface::Decode::UpdateAudioConfigToHal(
        const ::hfp::offload_config& /*offload_config*/) {
  log::warn(
          "'UpdateAudioConfigToHal(offload_config)' should not be called on "
          "HfpClientInterface::Decode");
}

void HfpClientInterface::Decode::UpdateAudioConfigToHal(const ::hfp::pcm_config& pcm_config) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }

  log::info("decode");
  if (!get_decode_client_interface()->UpdateAudioConfig(
              pcm_config_to_hal_audio_config(pcm_config))) {
    log::error("cannot update audio config to HAL");
    return;
  }
}

size_t HfpClientInterface::Decode::Write(const uint8_t* p_buf, uint32_t len) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return 0;
  }
  log::verbose("decode");

  auto instance = aidl::hfp::HfpDecodingTransport::instance_;
  if (instance->IsStreamActive()) {
    return get_decode_client_interface()->WriteAudioData(p_buf, len);
  }

  return len;
}

void HfpClientInterface::Decode::ConfirmStreamingRequest() {
  auto instance = aidl::hfp::HfpDecodingTransport::instance_;
  auto pending_cmd = instance->GetPendingCmd();
  switch (pending_cmd) {
    case aidl::hfp::HFP_CTRL_CMD_NONE:
      log::warn("no pending start stream request");
      FALLTHROUGH_INTENDED;
    case aidl::hfp::HFP_CTRL_CMD_START:
      aidl::hfp::HfpDecodingTransport::software_hal_interface->StreamStarted(
              aidl::BluetoothAudioCtrlAck::SUCCESS_FINISHED);
      instance->ResetPendingCmd();
      return;
    default:
      log::warn("Invalid state, {}", pending_cmd);
  }
}

void HfpClientInterface::Decode::CancelStreamingRequest() {
  auto instance = aidl::hfp::HfpDecodingTransport::instance_;
  auto pending_cmd = instance->GetPendingCmd();
  switch (pending_cmd) {
    case aidl::hfp::HFP_CTRL_CMD_START:
      aidl::hfp::HfpDecodingTransport::software_hal_interface->StreamStarted(
              aidl::BluetoothAudioCtrlAck::FAILURE);
      instance->ResetPendingCmd();
      return;
    case aidl::hfp::HFP_CTRL_CMD_NONE:
      log::warn("no pending start stream request");
      FALLTHROUGH_INTENDED;
    case aidl::hfp::HFP_CTRL_CMD_SUSPEND:
      log::info("suspends");
      aidl::hfp::HfpDecodingTransport::software_hal_interface->StreamSuspended(
              aidl::BluetoothAudioCtrlAck::SUCCESS_FINISHED);
      instance->ResetPendingCmd();
      return;
    default:
      log::warn("Invalid state, {}", pending_cmd);
  }
}

HfpClientInterface::Decode* HfpClientInterface::GetDecode(
        bluetooth::common::MessageLoopThread* /*message_loop*/) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return nullptr;
  }

  if (decode_ == nullptr) {
    decode_ = new Decode();
  } else {
    log::warn("Decode is already acquired");
    return nullptr;
  }

  log::info("decode");

  HfpDecodingTransport::instance_ =
          new HfpDecodingTransport(aidl::SessionType::HFP_SOFTWARE_DECODING_DATAPATH);
  HfpDecodingTransport::software_hal_interface =
          new aidl::BluetoothAudioSourceClientInterface(HfpDecodingTransport::instance_);
  if (!HfpDecodingTransport::software_hal_interface->IsValid()) {
    log::warn("BluetoothAudio HAL for HFP is invalid");
    delete HfpDecodingTransport::software_hal_interface;
    HfpDecodingTransport::software_hal_interface = nullptr;
    delete HfpDecodingTransport::instance_;
    HfpDecodingTransport::instance_ = nullptr;
    return nullptr;
  }

  HfpDecodingTransport::active_hal_interface = HfpDecodingTransport::software_hal_interface;

  return decode_;
}

bool HfpClientInterface::ReleaseDecode(HfpClientInterface::Decode* decode) {
  if (decode != decode_) {
    log::warn("can't release not acquired decode");
    return false;
  }

  log::info("decode");
  if (get_decode_client_interface()) {
    decode->Cleanup();
  }

  delete decode_;
  decode_ = nullptr;

  return true;
}

// Encoding client implementation
void HfpClientInterface::Encode::Cleanup() {
  log::info("encode");
  StopSession();
  if (HfpEncodingTransport::instance_) {
    delete HfpEncodingTransport::software_hal_interface;
    HfpEncodingTransport::software_hal_interface = nullptr;
    delete HfpEncodingTransport::instance_;
    HfpEncodingTransport::instance_ = nullptr;
  }
}

void HfpClientInterface::Encode::StartSession() {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }
  log::info("encode");
  AudioConfiguration audio_config;
  audio_config.set<AudioConfiguration::pcmConfig>(get_default_pcm_configuration());
  if (!get_encode_client_interface()->UpdateAudioConfig(audio_config)) {
    log::error("cannot update audio config to HAL");
    return;
  }
  get_encode_client_interface()->StartSession();
}

void HfpClientInterface::Encode::StopSession() {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }
  log::info("encode");
  get_encode_client_interface()->EndSession();
  if (get_encode_transport_instance()) {
    get_encode_transport_instance()->ResetPendingCmd();
    get_encode_transport_instance()->ResetPresentationPosition();
  }
}

void HfpClientInterface::Encode::UpdateAudioConfigToHal(
        const ::hfp::offload_config& /*offload_config*/) {
  log::warn(
          "'UpdateAudioConfigToHal(offload_config)' should not be called on "
          "HfpClientInterface::Encode");
}

void HfpClientInterface::Encode::UpdateAudioConfigToHal(const ::hfp::pcm_config& pcm_config) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }

  log::info("encode");
  if (!get_encode_client_interface()->UpdateAudioConfig(
              pcm_config_to_hal_audio_config(pcm_config))) {
    log::error("cannot update audio config to HAL");
    return;
  }
}

size_t HfpClientInterface::Encode::Read(uint8_t* p_buf, uint32_t len) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return 0;
  }
  log::verbose("encode");

  auto instance = aidl::hfp::HfpEncodingTransport::instance_;
  if (instance->IsStreamActive()) {
    return get_encode_client_interface()->ReadAudioData(p_buf, len);
  }

  memset(p_buf, 0x00, len);

  return len;
}

void HfpClientInterface::Encode::ConfirmStreamingRequest() {
  auto instance = aidl::hfp::HfpEncodingTransport::instance_;
  auto pending_cmd = instance->GetPendingCmd();
  switch (pending_cmd) {
    case aidl::hfp::HFP_CTRL_CMD_NONE:
      log::warn("no pending start stream request");
      FALLTHROUGH_INTENDED;
    case aidl::hfp::HFP_CTRL_CMD_START:
      aidl::hfp::HfpEncodingTransport::software_hal_interface->StreamStarted(
              aidl::BluetoothAudioCtrlAck::SUCCESS_FINISHED);
      instance->ResetPendingCmd();
      return;
    default:
      log::warn("Invalid state, {}", pending_cmd);
  }
}

void HfpClientInterface::Encode::CancelStreamingRequest() {
  auto instance = aidl::hfp::HfpEncodingTransport::instance_;
  auto pending_cmd = instance->GetPendingCmd();
  switch (pending_cmd) {
    case aidl::hfp::HFP_CTRL_CMD_START:
      aidl::hfp::HfpEncodingTransport::software_hal_interface->StreamStarted(
              aidl::BluetoothAudioCtrlAck::FAILURE);
      instance->ResetPendingCmd();
      return;
    case aidl::hfp::HFP_CTRL_CMD_NONE:
      log::warn("no pending start stream request");
      FALLTHROUGH_INTENDED;
    case aidl::hfp::HFP_CTRL_CMD_SUSPEND:
      log::info("suspends");
      aidl::hfp::HfpEncodingTransport::software_hal_interface->StreamSuspended(
              aidl::BluetoothAudioCtrlAck::SUCCESS_FINISHED);
      instance->ResetPendingCmd();
      return;
    default:
      log::warn("Invalid state, {}", pending_cmd);
  }
}

HfpClientInterface::Encode* HfpClientInterface::GetEncode(
        bluetooth::common::MessageLoopThread* /*message_loop*/) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return nullptr;
  }

  if (encode_ == nullptr) {
    encode_ = new Encode();
  } else {
    log::warn("Encoding is already acquired");
    return nullptr;
  }

  log::info("encode");

  HfpEncodingTransport::instance_ =
          new HfpEncodingTransport(aidl::SessionType::HFP_SOFTWARE_ENCODING_DATAPATH);
  HfpEncodingTransport::software_hal_interface =
          new aidl::BluetoothAudioSinkClientInterface(HfpEncodingTransport::instance_);
  if (!HfpEncodingTransport::software_hal_interface->IsValid()) {
    log::warn("BluetoothAudio HAL for HFP is invalid");
    delete HfpEncodingTransport::software_hal_interface;
    HfpEncodingTransport::software_hal_interface = nullptr;
    delete HfpEncodingTransport::instance_;
    HfpEncodingTransport::instance_ = nullptr;
    return nullptr;
  }

  HfpEncodingTransport::active_hal_interface = HfpEncodingTransport::software_hal_interface;

  return encode_;
}

bool HfpClientInterface::ReleaseEncode(HfpClientInterface::Encode* encode) {
  if (encode != encode_) {
    log::warn("can't release not acquired encode");
    return false;
  }

  if (get_encode_client_interface()) {
    encode->Cleanup();
  }

  delete encode_;
  encode_ = nullptr;

  return true;
}

// Offload client implementation
// Based on HfpEncodingTransport
void HfpClientInterface::Offload::Cleanup() {
  log::info("offload");
  StopSession();
  if (HfpEncodingTransport::instance_) {
    delete HfpEncodingTransport::offloading_hal_interface;
    HfpEncodingTransport::offloading_hal_interface = nullptr;
    delete HfpEncodingTransport::instance_;
    HfpEncodingTransport::instance_ = nullptr;
  }
}

void HfpClientInterface::Offload::StartSession() {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }
  log::info("offload");
  AudioConfiguration audio_config;
  audio_config.set<AudioConfiguration::hfpConfig>(get_default_hfp_configuration());
  if (!get_encode_client_interface()->UpdateAudioConfig(audio_config)) {
    log::error("cannot update audio config to HAL");
    return;
  }
  if (get_encode_client_interface()->StartSession() == 0) {
    log::info("session started");
  } else {
    log::warn("session not started");
  }
}

void HfpClientInterface::Offload::StopSession() {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }
  log::info("offload");
  get_encode_client_interface()->EndSession();
  if (get_encode_transport_instance()) {
    get_encode_transport_instance()->ResetPendingCmd();
    get_encode_transport_instance()->ResetPresentationPosition();
  }
}

void HfpClientInterface::Offload::UpdateAudioConfigToHal(
        const ::hfp::offload_config& offload_config) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return;
  }

  log::info("offload");
  get_encode_client_interface()->UpdateAudioConfig(
          offload_config_to_hal_audio_config(offload_config));
}

void HfpClientInterface::Offload::UpdateAudioConfigToHal(const ::hfp::pcm_config& /*pcm_config*/) {
  log::warn(
          "'UpdateAudioConfigToHal(pcm_config)' should not be called on "
          "HfpClientInterface::Offload");
}

void HfpClientInterface::Offload::ConfirmStreamingRequest() {
  auto instance = aidl::hfp::HfpEncodingTransport::instance_;
  auto pending_cmd = instance->GetPendingCmd();
  switch (pending_cmd) {
    case aidl::hfp::HFP_CTRL_CMD_START:
      aidl::hfp::HfpEncodingTransport::offloading_hal_interface->StreamStarted(
              aidl::BluetoothAudioCtrlAck::SUCCESS_FINISHED);
      instance->ResetPendingCmd();
      return;
    case aidl::hfp::HFP_CTRL_CMD_NONE:
      log::warn("no pending start stream request");
      return;
    default:
      log::warn("Invalid state, {}", pending_cmd);
  }
}

void HfpClientInterface::Offload::CancelStreamingRequest() {
  auto instance = aidl::hfp::HfpEncodingTransport::instance_;
  auto pending_cmd = instance->GetPendingCmd();
  switch (pending_cmd) {
    case aidl::hfp::HFP_CTRL_CMD_START:
      aidl::hfp::HfpEncodingTransport::offloading_hal_interface->StreamStarted(
              aidl::BluetoothAudioCtrlAck::FAILURE);
      instance->ResetPendingCmd();
      return;
    case aidl::hfp::HFP_CTRL_CMD_NONE:
      log::info("no pending start stream request");
      [[fallthrough]];
    case aidl::hfp::HFP_CTRL_CMD_SUSPEND:
      log::info("suspends");
      aidl::hfp::HfpEncodingTransport::offloading_hal_interface->StreamSuspended(
              aidl::BluetoothAudioCtrlAck::SUCCESS_FINISHED);
      instance->ResetPendingCmd();
      return;
    default:
      log::warn("Invalid state, {}", pending_cmd);
  }
}

std::unordered_map<tBTA_AG_UUID_CODEC, ::hfp::sco_config>
HfpClientInterface::Offload::GetHfpScoConfig() {
  return aidl::hfp::HfpTransport::GetHfpScoConfig(aidl::SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH);
}

HfpClientInterface::Offload* HfpClientInterface::GetOffload(
        bluetooth::common::MessageLoopThread* /*message_loop*/) {
  if (!is_aidl_support_hfp()) {
    log::warn("Unsupported HIDL or AIDL version");
    return nullptr;
  }

  if (offload_ == nullptr) {
    offload_ = new Offload();
  } else {
    log::warn("Offload is already acquired");
    return nullptr;
  }

  log::info("offload");

  // Prepare offload hal interface.
  if (bta_ag_get_sco_offload_enabled()) {
    HfpEncodingTransport::instance_ =
            new HfpEncodingTransport(aidl::SessionType::HFP_HARDWARE_OFFLOAD_DATAPATH);
    HfpEncodingTransport::offloading_hal_interface =
            new aidl::BluetoothAudioSinkClientInterface(HfpEncodingTransport::instance_);
    if (!HfpEncodingTransport::offloading_hal_interface->IsValid()) {
      log::fatal("BluetoothAudio HAL for HFP offloading is invalid");
      delete HfpEncodingTransport::offloading_hal_interface;
      HfpEncodingTransport::offloading_hal_interface = nullptr;
      delete HfpEncodingTransport::instance_;
      HfpEncodingTransport::instance_ = static_cast<HfpEncodingTransport*>(
              HfpEncodingTransport::software_hal_interface->GetTransportInstance());
      delete HfpEncodingTransport::software_hal_interface;
      HfpEncodingTransport::software_hal_interface = nullptr;
      delete HfpEncodingTransport::instance_;
      return nullptr;
    }
  }

  HfpEncodingTransport::active_hal_interface = HfpEncodingTransport::offloading_hal_interface;

  return offload_;
}

bool HfpClientInterface::ReleaseOffload(HfpClientInterface::Offload* offload) {
  if (offload != offload_) {
    log::warn("can't release not acquired offload");
    return false;
  }

  if (get_encode_client_interface()) {
    offload->Cleanup();
  }

  delete offload_;
  offload_ = nullptr;

  return true;
}

}  // namespace hfp
}  // namespace audio
}  // namespace bluetooth
