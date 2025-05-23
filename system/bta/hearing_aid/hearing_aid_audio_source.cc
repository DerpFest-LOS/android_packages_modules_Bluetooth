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

#include <base/files/file_util.h>
#include <bluetooth/log.h>
#include <stdio.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <sstream>
#include <vector>

#include "audio_hal_interface/hearing_aid_software_encoding.h"
#include "audio_hearing_aid_hw/include/audio_hearing_aid_hw.h"
#include "bta/include/bta_hearing_aid_api.h"
#include "common/message_loop_thread.h"
#include "common/repeating_timer.h"
#include "common/time_util.h"
#include "hardware/bluetooth.h"
#include "hardware/bt_av.h"
#include "osi/include/wakelock.h"
#include "stack/include/main_thread.h"
#include "udrv/include/uipc.h"

using namespace bluetooth;

namespace std {
template <>
struct formatter<tUIPC_EVENT> : enum_formatter<tUIPC_EVENT> {};
template <>
struct formatter<tHEARING_AID_CTRL_ACK> : enum_formatter<tHEARING_AID_CTRL_ACK> {};
template <>
struct formatter<tHEARING_AID_CTRL_CMD> : enum_formatter<tHEARING_AID_CTRL_CMD> {};
}  // namespace std

namespace {
#define CASE_RETURN_STR(const) \
  case const:                  \
    return #const;

const char* audio_ha_hw_dump_ctrl_event(tHEARING_AID_CTRL_CMD event) {
  switch (event) {
    CASE_RETURN_STR(HEARING_AID_CTRL_CMD_NONE)
    CASE_RETURN_STR(HEARING_AID_CTRL_CMD_CHECK_READY)
    CASE_RETURN_STR(HEARING_AID_CTRL_CMD_START)
    CASE_RETURN_STR(HEARING_AID_CTRL_CMD_STOP)
    CASE_RETURN_STR(HEARING_AID_CTRL_CMD_SUSPEND)
    CASE_RETURN_STR(HEARING_AID_CTRL_GET_INPUT_AUDIO_CONFIG)
    CASE_RETURN_STR(HEARING_AID_CTRL_GET_OUTPUT_AUDIO_CONFIG)
    CASE_RETURN_STR(HEARING_AID_CTRL_SET_OUTPUT_AUDIO_CONFIG)
    CASE_RETURN_STR(HEARING_AID_CTRL_CMD_OFFLOAD_START)
    default:
      break;
  }

  return "UNKNOWN HEARING_AID_CTRL_CMD";
}

int bit_rate = -1;
int sample_rate = -1;
int data_interval_ms = -1;
int num_channels = 2;
bluetooth::common::RepeatingTimer audio_timer;
HearingAidAudioReceiver* localAudioReceiver = nullptr;
std::unique_ptr<tUIPC_STATE> uipc_hearing_aid = nullptr;

struct AudioHalStats {
  size_t media_read_total_underflow_bytes;
  size_t media_read_total_underflow_count;
  uint64_t media_read_last_underflow_us;

  AudioHalStats() { Reset(); }

  void Reset() {
    media_read_total_underflow_bytes = 0;
    media_read_total_underflow_count = 0;
    media_read_last_underflow_us = 0;
  }
};

AudioHalStats stats;

bool hearing_aid_on_resume_req(bool start_media_task);
bool hearing_aid_on_suspend_req();

void send_audio_data() {
  uint32_t bytes_per_tick = (num_channels * sample_rate * data_interval_ms * (bit_rate / 8)) / 1000;

  uint8_t p_buf[bytes_per_tick];

  uint32_t bytes_read;
  if (bluetooth::audio::hearing_aid::is_hal_enabled()) {
    bytes_read = bluetooth::audio::hearing_aid::read(p_buf, bytes_per_tick);
  } else {
    bytes_read = UIPC_Read(*uipc_hearing_aid, UIPC_CH_ID_AV_AUDIO, p_buf, bytes_per_tick);
  }

  log::debug("bytes_read: {}", bytes_read);
  if (bytes_read < bytes_per_tick) {
    stats.media_read_total_underflow_bytes += bytes_per_tick - bytes_read;
    stats.media_read_total_underflow_count++;
    stats.media_read_last_underflow_us = bluetooth::common::time_get_os_boottime_us();
  }

  std::vector<uint8_t> data(p_buf, p_buf + bytes_read);

  if (localAudioReceiver != nullptr) {
    localAudioReceiver->OnAudioDataReady(data);
  }
}

void hearing_aid_send_ack(tHEARING_AID_CTRL_ACK status) {
  uint8_t ack = status;
  log::debug("Hearing Aid audio ctrl ack: {}", status);
  UIPC_Send(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, 0, &ack, sizeof(ack));
}

void start_audio_ticks() {
  if (data_interval_ms != HA_INTERVAL_10_MS && data_interval_ms != HA_INTERVAL_20_MS) {
    log::fatal("Unsupported data interval: {}", data_interval_ms);
  }

  wakelock_acquire();
  audio_timer.SchedulePeriodic(get_main_thread()->GetWeakPtr(), FROM_HERE,
                               base::BindRepeating(&send_audio_data),
                               std::chrono::milliseconds(data_interval_ms));
  log::info("running with data interval: {}", data_interval_ms);
}

void stop_audio_ticks() {
  log::info("stopped");
  audio_timer.CancelAndWait();
  wakelock_release();
}

void hearing_aid_data_cb(tUIPC_CH_ID, tUIPC_EVENT event) {
  log::debug("Hearing Aid audio data event: {}", event);
  switch (event) {
    case UIPC_OPEN_EVT:
      log::info("UIPC_OPEN_EVT");
      /*
       * Read directly from media task from here on (keep callback for
       * connection events.
       */
      UIPC_Ioctl(*uipc_hearing_aid, UIPC_CH_ID_AV_AUDIO, UIPC_REG_REMOVE_ACTIVE_READSET, NULL);
      UIPC_Ioctl(*uipc_hearing_aid, UIPC_CH_ID_AV_AUDIO, UIPC_SET_READ_POLL_TMO,
                 reinterpret_cast<void*>(0));

      do_in_main_thread(base::BindOnce(start_audio_ticks));
      break;
    case UIPC_CLOSE_EVT:
      log::info("UIPC_CLOSE_EVT");
      hearing_aid_send_ack(HEARING_AID_CTRL_ACK_SUCCESS);
      do_in_main_thread(base::BindOnce(stop_audio_ticks));
      break;
    default:
      log::error("Hearing Aid audio data event not recognized: {}", event);
  }
}

void hearing_aid_recv_ctrl_data() {
  tHEARING_AID_CTRL_CMD cmd = HEARING_AID_CTRL_CMD_NONE;
  int n;

  uint8_t read_cmd = 0; /* The read command size is one octet */
  n = UIPC_Read(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, &read_cmd, 1);
  cmd = static_cast<tHEARING_AID_CTRL_CMD>(read_cmd);

  /* detach on ctrl channel means audioflinger process was terminated */
  if (n == 0) {
    log::warn("CTRL CH DETACHED");
    UIPC_Close(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL);
    return;
  }

  log::info("{}", audio_ha_hw_dump_ctrl_event(cmd));
  //  a2dp_cmd_pending = cmd;

  tHEARING_AID_CTRL_ACK ctrl_ack_status;

  switch (cmd) {
    case HEARING_AID_CTRL_CMD_CHECK_READY:
      hearing_aid_send_ack(HEARING_AID_CTRL_ACK_SUCCESS);
      break;

    case HEARING_AID_CTRL_CMD_START:
      ctrl_ack_status = HEARING_AID_CTRL_ACK_SUCCESS;
      // timer is restarted in UIPC_Open
      if (!hearing_aid_on_resume_req(false)) {
        ctrl_ack_status = HEARING_AID_CTRL_ACK_FAILURE;
      } else {
        UIPC_Open(*uipc_hearing_aid, UIPC_CH_ID_AV_AUDIO, hearing_aid_data_cb,
                  HEARING_AID_DATA_PATH);
      }
      hearing_aid_send_ack(ctrl_ack_status);
      break;

    case HEARING_AID_CTRL_CMD_STOP:
      if (!hearing_aid_on_suspend_req()) {
        log::info("HEARING_AID_CTRL_CMD_STOP: hearing_aid_on_suspend_req() errs, but ignored.");
      }
      hearing_aid_send_ack(HEARING_AID_CTRL_ACK_SUCCESS);
      break;

    case HEARING_AID_CTRL_CMD_SUSPEND:
      ctrl_ack_status = HEARING_AID_CTRL_ACK_SUCCESS;
      if (!hearing_aid_on_suspend_req()) {
        ctrl_ack_status = HEARING_AID_CTRL_ACK_FAILURE;
      }
      hearing_aid_send_ack(ctrl_ack_status);
      break;

    case HEARING_AID_CTRL_GET_OUTPUT_AUDIO_CONFIG: {
      btav_a2dp_codec_config_t codec_config;
      btav_a2dp_codec_config_t codec_capability;
      if (sample_rate == 16000) {
        codec_config.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_16000;
        codec_capability.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_16000;
      } else if (sample_rate == 24000) {
        codec_config.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_24000;
        codec_capability.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_24000;
      } else {
        log::fatal("unsupported sample rate: {}", sample_rate);
      }

      codec_config.bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16;
      codec_capability.bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16;

      codec_config.channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO;
      codec_capability.channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO;

      hearing_aid_send_ack(HEARING_AID_CTRL_ACK_SUCCESS);
      // Send the current codec config
      UIPC_Send(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_config.sample_rate),
                sizeof(btav_a2dp_codec_sample_rate_t));
      UIPC_Send(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_config.bits_per_sample),
                sizeof(btav_a2dp_codec_bits_per_sample_t));
      UIPC_Send(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_config.channel_mode),
                sizeof(btav_a2dp_codec_channel_mode_t));
      // Send the current codec capability
      UIPC_Send(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_capability.sample_rate),
                sizeof(btav_a2dp_codec_sample_rate_t));
      UIPC_Send(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_capability.bits_per_sample),
                sizeof(btav_a2dp_codec_bits_per_sample_t));
      UIPC_Send(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, 0,
                reinterpret_cast<const uint8_t*>(&codec_capability.channel_mode),
                sizeof(btav_a2dp_codec_channel_mode_t));
      break;
    }

    case HEARING_AID_CTRL_SET_OUTPUT_AUDIO_CONFIG: {
      // TODO: we only support one config for now!
      btav_a2dp_codec_config_t codec_config;
      codec_config.sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_NONE;
      codec_config.bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_NONE;
      codec_config.channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_NONE;

      hearing_aid_send_ack(HEARING_AID_CTRL_ACK_SUCCESS);
      // Send the current codec config
      if (UIPC_Read(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL,
                    reinterpret_cast<uint8_t*>(&codec_config.sample_rate),
                    sizeof(btav_a2dp_codec_sample_rate_t)) !=
          sizeof(btav_a2dp_codec_sample_rate_t)) {
        log::error("Error reading sample rate from audio HAL");
        break;
      }
      if (UIPC_Read(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL,
                    reinterpret_cast<uint8_t*>(&codec_config.bits_per_sample),
                    sizeof(btav_a2dp_codec_bits_per_sample_t)) !=
          sizeof(btav_a2dp_codec_bits_per_sample_t)) {
        log::error("Error reading bits per sample from audio HAL");

        break;
      }
      if (UIPC_Read(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL,
                    reinterpret_cast<uint8_t*>(&codec_config.channel_mode),
                    sizeof(btav_a2dp_codec_channel_mode_t)) !=
          sizeof(btav_a2dp_codec_channel_mode_t)) {
        log::error("Error reading channel mode from audio HAL");

        break;
      }
      log::info(
              "HEARING_AID_CTRL_SET_OUTPUT_AUDIO_CONFIG: sample_rate={}, "
              "bits_per_sample={},channel_mode={}",
              codec_config.sample_rate, codec_config.bits_per_sample, codec_config.channel_mode);
      break;
    }

    default:
      log::error("UNSUPPORTED CMD: {}", cmd);
      hearing_aid_send_ack(HEARING_AID_CTRL_ACK_FAILURE);
      break;
  }
  log::info("a2dp-ctrl-cmd : {} DONE", audio_ha_hw_dump_ctrl_event(cmd));
}

void hearing_aid_ctrl_cb(tUIPC_CH_ID, tUIPC_EVENT event) {
  log::debug("Hearing Aid audio ctrl event: {}", event);
  switch (event) {
    case UIPC_OPEN_EVT:
      break;
    case UIPC_CLOSE_EVT:
      /* restart ctrl server unless we are shutting down */
      if (HearingAid::IsHearingAidRunning()) {
        UIPC_Open(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, hearing_aid_ctrl_cb,
                  HEARING_AID_CTRL_PATH);
      }
      break;
    case UIPC_RX_DATA_READY_EVT:
      hearing_aid_recv_ctrl_data();
      break;
    default:
      log::error("Hearing Aid audio ctrl unrecognized event: {}", event);
  }
}

bool hearing_aid_on_resume_req(bool start_media_task) {
  if (localAudioReceiver == nullptr) {
    log::error("HEARING_AID_CTRL_CMD_START: audio receiver not started");
    return false;
  }
  bt_status_t status;
  if (start_media_task) {
    status = do_in_main_thread(base::BindOnce(&HearingAidAudioReceiver::OnAudioResume,
                                              base::Unretained(localAudioReceiver),
                                              start_audio_ticks));
  } else {
    auto start_dummy_ticks = []() { log::info("start_audio_ticks: waiting for data path opened"); };
    status = do_in_main_thread(base::BindOnce(&HearingAidAudioReceiver::OnAudioResume,
                                              base::Unretained(localAudioReceiver),
                                              start_dummy_ticks));
  }
  if (status != BT_STATUS_SUCCESS) {
    log::error("HEARING_AID_CTRL_CMD_START: do_in_main_thread err={}", status);
    return false;
  }
  return true;
}

bool hearing_aid_on_suspend_req() {
  if (localAudioReceiver == nullptr) {
    log::error("HEARING_AID_CTRL_CMD_SUSPEND: audio receiver not started");
    return false;
  }
  bt_status_t status =
          do_in_main_thread(base::BindOnce(&HearingAidAudioReceiver::OnAudioSuspend,
                                           base::Unretained(localAudioReceiver), stop_audio_ticks));
  if (status != BT_STATUS_SUCCESS) {
    log::error("HEARING_AID_CTRL_CMD_SUSPEND: do_in_main_thread err={}", status);
    return false;
  }
  return true;
}
}  // namespace

void HearingAidAudioSource::Start(const CodecConfiguration& codecConfiguration,
                                  HearingAidAudioReceiver* audioReceiver,
                                  uint16_t remote_delay_ms) {
  log::info("Hearing Aid Source Open");

  bit_rate = codecConfiguration.bit_rate;
  sample_rate = codecConfiguration.sample_rate;
  data_interval_ms = codecConfiguration.data_interval_ms;

  stats.Reset();

  if (bluetooth::audio::hearing_aid::is_hal_enabled()) {
    bluetooth::audio::hearing_aid::start_session();
    bluetooth::audio::hearing_aid::set_remote_delay(remote_delay_ms);
  }
  localAudioReceiver = audioReceiver;
}

void HearingAidAudioSource::Stop() {
  log::info("Hearing Aid Source Close");

  localAudioReceiver = nullptr;
  if (bluetooth::audio::hearing_aid::is_hal_enabled()) {
    bluetooth::audio::hearing_aid::end_session();
  }

  stop_audio_ticks();
}

void HearingAidAudioSource::Initialize() {
  auto stream_cb = bluetooth::audio::hearing_aid::StreamCallbacks{
          .on_resume_ = hearing_aid_on_resume_req,
          .on_suspend_ = hearing_aid_on_suspend_req,
  };
  if (!bluetooth::audio::hearing_aid::init(stream_cb, get_main_thread())) {
    log::warn("Using legacy HAL");
    uipc_hearing_aid = UIPC_Init();
    UIPC_Open(*uipc_hearing_aid, UIPC_CH_ID_AV_CTRL, hearing_aid_ctrl_cb, HEARING_AID_CTRL_PATH);
  }
}

void HearingAidAudioSource::CleanUp() {
  if (bluetooth::audio::hearing_aid::is_hal_enabled()) {
    bluetooth::audio::hearing_aid::cleanup();
  } else {
    UIPC_Close(*uipc_hearing_aid, UIPC_CH_ID_ALL);
    uipc_hearing_aid = nullptr;
  }
}

void HearingAidAudioSource::DebugDump(int fd) {
  uint64_t now_us = bluetooth::common::time_get_os_boottime_us();
  std::stringstream stream;
  stream << "  Hearing Aid Audio HAL:"
         << "\n    Counts (underflow)                                      : "
         << stats.media_read_total_underflow_count
         << "\n    Bytes (underflow)                                       : "
         << stats.media_read_total_underflow_bytes
         << "\n    Last update time ago in ms (underflow)                  : "
         << (stats.media_read_last_underflow_us > 0
                     ? (now_us - stats.media_read_last_underflow_us) / 1000
                     : 0)
         << std::endl;
  dprintf(fd, "%s", stream.str().c_str());
}
