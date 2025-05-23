/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <fuzzer/FuzzedDataProvider.h>

#include "audio_hal_interface/a2dp_encoding.h"
#include "include/btif_av.h"
#include "include/btif_av_co.h"
#include "osi/include/properties.h"

using ::bluetooth::audio::a2dp::Status;
using ::bluetooth::audio::a2dp::update_codec_offloading_capabilities;

extern "C" {
struct android_namespace_t* android_get_exported_namespace(const char*) { return nullptr; }
}

constexpr Status kStatus[] = {
        Status::UNKNOWN, Status::SUCCESS, Status::UNSUPPORTED_CODEC_CONFIGURATION,
        Status::FAILURE, Status::PENDING,
};

constexpr int32_t kRandomStringLength = 256;

static void source_init_delayed(void) {}

constexpr btav_a2dp_codec_index_t kCodecIndices[] = {
        BTAV_A2DP_CODEC_INDEX_SOURCE_SBC,  BTAV_A2DP_CODEC_INDEX_SOURCE_AAC,
        BTAV_A2DP_CODEC_INDEX_SOURCE_APTX, BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_HD,
        BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC, BTAV_A2DP_CODEC_INDEX_SINK_SBC,
        BTAV_A2DP_CODEC_INDEX_SINK_AAC,    BTAV_A2DP_CODEC_INDEX_SINK_LDAC};

std::vector<std::vector<btav_a2dp_codec_config_t>> CodecOffloadingPreferenceGenerator() {
  std::vector<std::vector<btav_a2dp_codec_config_t>> offloadingPreferences = {
          std::vector<btav_a2dp_codec_config_t>(0)};
  btav_a2dp_codec_config_t btavCodecConfig = {};
  for (btav_a2dp_codec_index_t i : kCodecIndices) {
    btavCodecConfig.codec_type = i;
    auto duplicated_preferences = offloadingPreferences;
    for (auto iter = duplicated_preferences.begin(); iter != duplicated_preferences.end(); ++iter) {
      iter->push_back(btavCodecConfig);
    }
    offloadingPreferences.insert(offloadingPreferences.end(), duplicated_preferences.begin(),
                                 duplicated_preferences.end());
  }
  return offloadingPreferences;
}

class A2dpEncodingFuzzer {
public:
  ~A2dpEncodingFuzzer() {
    delete (mCodec);
    mCodec = nullptr;
  }
  void process(const uint8_t* data, size_t size);
  static A2dpCodecConfig* mCodec;
};

class TestAudioPort : public bluetooth::audio::a2dp::StreamCallbacks {
  Status StartStream(bool /*low_latency*/) const override { return Status::PENDING; }
  Status SuspendStream() const override { return Status::PENDING; }
  Status SetLatencyMode(bool /*low_latency*/) const override { return Status::SUCCESS; }
};

A2dpCodecConfig* A2dpEncodingFuzzer::mCodec{nullptr};
const TestAudioPort test_audio_port;

void A2dpEncodingFuzzer::process(const uint8_t* data, size_t size) {
  FuzzedDataProvider fdp(data, size);
  if (!mCodec) {
    mCodec = A2dpCodecConfig::createCodec(fdp.PickValueInArray(kCodecIndices));
  }

  bool offload_enabled = fdp.ConsumeBool();
  std::string name = fdp.ConsumeRandomLengthString(kRandomStringLength);
  uint16_t peer_mtu = fdp.ConsumeIntegral<uint16_t>();
  int preferred_encoding_interval_us = fdp.ConsumeIntegral<int>();

  bluetooth::common::MessageLoopThread messageLoopThread(name);
  messageLoopThread.StartUp();
  messageLoopThread.DoInThread(FROM_HERE, base::BindOnce(&source_init_delayed));

  uint16_t delayReport = fdp.ConsumeIntegral<uint16_t>();
  bluetooth::audio::a2dp::set_remote_delay(delayReport);

  if (!bluetooth::audio::a2dp::init(&messageLoopThread, &test_audio_port, offload_enabled)) {
    return;
  }

  if (!bluetooth::audio::a2dp::setup_codec(bta_av_get_a2dp_current_codec(), peer_mtu,
                                           preferred_encoding_interval_us)) {
    return;
  }

  bluetooth::audio::a2dp::start_session();

  Status status = fdp.PickValueInArray(kStatus);
  bluetooth::audio::a2dp::ack_stream_started(status);

  for (auto offloadingPreference : CodecOffloadingPreferenceGenerator()) {
    update_codec_offloading_capabilities(offloadingPreference, false);
  }
  status = fdp.PickValueInArray(kStatus);
  bluetooth::audio::a2dp::ack_stream_suspended(status);
  bluetooth::audio::a2dp::cleanup();
  messageLoopThread.ShutDown();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  A2dpEncodingFuzzer a2dpEncodingFuzzer;
  a2dpEncodingFuzzer.process(data, size);
  return 0;
}
