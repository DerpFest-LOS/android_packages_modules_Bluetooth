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

#pragma once

#include <vector>

#include "a2dp_encoding.h"
#include "common/message_loop_thread.h"
#include "hardware/bt_av.h"

namespace bluetooth {
namespace audio {
namespace hidl {
namespace a2dp {

bool update_codec_offloading_capabilities(
        const std::vector<btav_a2dp_codec_config_t>& framework_preference);

// Check if new bluetooth_audio is enabled
bool is_hal_2_0_enabled();

// Check if new bluetooth_audio is running with offloading encoders
bool is_hal_2_0_offloading();

// Initialize BluetoothAudio HAL: openProvider
bool init(bluetooth::common::MessageLoopThread* message_loop,
          bluetooth::audio::a2dp::StreamCallbacks const* stream_callbacks, bool offload_enabled);

// Clean up BluetoothAudio HAL
void cleanup();

// Set up the codec into BluetoothAudio HAL
bool setup_codec(A2dpCodecConfig* a2dp_config, uint16_t peer_mtu,
                 int preferred_encoding_interval_us);

// Send command to the BluetoothAudio HAL: StartSession, EndSession,
// StreamStarted, StreamSuspended
void start_session();
void end_session();
void ack_stream_started(::bluetooth::audio::a2dp::Status status);
void ack_stream_suspended(::bluetooth::audio::a2dp::Status status);

// Read from the FMQ of BluetoothAudio HAL
size_t read(uint8_t* p_buf, uint32_t len);

// Update A2DP delay report to BluetoothAudio HAL
void set_remote_delay(uint16_t delay_report);

}  // namespace a2dp
}  // namespace hidl
}  // namespace audio
}  // namespace bluetooth
