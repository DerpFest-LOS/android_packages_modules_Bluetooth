/******************************************************************************
 *
 *  Copyright 2016 The Android Open Source Project
 *  Copyright 2009-2012 Broadcom Corporation
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

#define LOG_TAG "bluetooth-a2dp"
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include "btif_a2dp_source.h"

#include <base/functional/bind.h>
#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>
#include <stdio.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "a2dp_api.h"
#include "a2dp_codec_api.h"
#include "audio_hal_interface/a2dp_encoding.h"
#include "avdt_api.h"
#include "bta_av_api.h"
#include "bta_av_ci.h"
#include "btif_av.h"
#include "btif_av_co.h"
#include "btif_common.h"
#include "btif_hf.h"
#include "btif_metrics_logging.h"
#include "btm_iso_api.h"
#include "common/message_loop_thread.h"
#include "common/metrics.h"
#include "common/repeating_timer.h"
#include "common/time_util.h"
#include "hardware/bt_av.h"
#include "osi/include/allocator.h"
#include "osi/include/fixed_queue.h"
#include "osi/include/wakelock.h"
#include "stack/include/a2dp_sbc_constants.h"
#include "stack/include/acl_api.h"
#include "stack/include/acl_api_types.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/btm_client_interface.h"
#include "stack/include/btm_status.h"
#include "stack/include/main_thread.h"
#include "types/bt_transport.h"
#include "types/raw_address.h"

#ifdef __ANDROID__
#include <cutils/trace.h>
#endif

using bluetooth::audio::a2dp::Status;
using bluetooth::common::A2dpSessionMetrics;
using bluetooth::common::BluetoothMetricsLogger;
using bluetooth::common::RepeatingTimer;
using namespace bluetooth;

/**
 * The typical runlevel of the tx queue size is ~1 buffer
 * but due to link flow control or thread preemption in lower
 * layers we might need to temporarily buffer up data.
 */
#define MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ (MAX_PCM_FRAME_NUM_PER_TICK * 2)

class SchedulingStats {
public:
  SchedulingStats() { Reset(); }
  void Reset() {
    total_updates = 0;
    last_update_us = 0;
    overdue_scheduling_count = 0;
    total_overdue_scheduling_delta_us = 0;
    max_overdue_scheduling_delta_us = 0;
    premature_scheduling_count = 0;
    total_premature_scheduling_delta_us = 0;
    max_premature_scheduling_delta_us = 0;
    exact_scheduling_count = 0;
    total_scheduling_time_us = 0;
  }

  // Counter for total updates
  size_t total_updates;

  // Last update timestamp (in us)
  uint64_t last_update_us;

  // Counter for overdue scheduling
  size_t overdue_scheduling_count;

  // Accumulated overdue scheduling deviations (in us)
  uint64_t total_overdue_scheduling_delta_us;

  // Max. overdue scheduling delta time (in us)
  uint64_t max_overdue_scheduling_delta_us;

  // Counter for premature scheduling
  size_t premature_scheduling_count;

  // Accumulated premature scheduling deviations (in us)
  uint64_t total_premature_scheduling_delta_us;

  // Max. premature scheduling delta time (in us)
  uint64_t max_premature_scheduling_delta_us;

  // Counter for exact scheduling
  size_t exact_scheduling_count;

  // Accumulated and counted scheduling time (in us)
  uint64_t total_scheduling_time_us;
};

class BtifMediaStats {
public:
  BtifMediaStats() { Reset(); }
  void Reset() {
    session_start_us = 0;
    session_end_us = 0;
    tx_queue_enqueue_stats.Reset();
    tx_queue_dequeue_stats.Reset();
    tx_queue_total_frames = 0;
    tx_queue_max_frames_per_packet = 0;
    tx_queue_total_queueing_time_us = 0;
    tx_queue_max_queueing_time_us = 0;
    tx_queue_total_readbuf_calls = 0;
    tx_queue_last_readbuf_us = 0;
    tx_queue_total_flushed_messages = 0;
    tx_queue_last_flushed_us = 0;
    tx_queue_total_dropped_messages = 0;
    tx_queue_max_dropped_messages = 0;
    tx_queue_dropouts = 0;
    tx_queue_last_dropouts_us = 0;
    media_read_total_underflow_bytes = 0;
    media_read_total_underflow_count = 0;
    media_read_last_underflow_us = 0;
    codec_index = -1;
  }

  uint64_t session_start_us;
  uint64_t session_end_us;

  SchedulingStats tx_queue_enqueue_stats;
  SchedulingStats tx_queue_dequeue_stats;

  size_t tx_queue_total_frames;
  size_t tx_queue_max_frames_per_packet;

  uint64_t tx_queue_total_queueing_time_us;
  uint64_t tx_queue_max_queueing_time_us;

  size_t tx_queue_total_readbuf_calls;
  uint64_t tx_queue_last_readbuf_us;

  size_t tx_queue_total_flushed_messages;
  uint64_t tx_queue_last_flushed_us;

  size_t tx_queue_total_dropped_messages;
  size_t tx_queue_max_dropped_messages;
  size_t tx_queue_dropouts;
  uint64_t tx_queue_last_dropouts_us;

  size_t media_read_total_underflow_bytes;
  size_t media_read_total_underflow_count;
  uint64_t media_read_last_underflow_us;

  int codec_index = -1;
};

class BtifA2dpSource {
public:
  enum RunState { kStateOff, kStateStartingUp, kStateRunning, kStateShuttingDown };

  BtifA2dpSource()
      : tx_audio_queue(nullptr),
        tx_flush(false),
        sw_audio_is_encoding(false),
        encoder_interface(nullptr),
        encoder_interval_ms(0),
        state_(kStateOff) {}

  void Reset() {
    fixed_queue_free(tx_audio_queue, nullptr);
    tx_audio_queue = nullptr;
    tx_flush = false;
    media_alarm.CancelAndWait();
    wakelock_release();
    encoder_interface = nullptr;
    encoder_interval_ms = 0;
    stats.Reset();
    accumulated_stats.Reset();
    state_ = kStateOff;
  }

  BtifA2dpSource::RunState State() const { return state_; }
  std::string StateStr() const {
    switch (state_) {
      case kStateOff:
        return "STATE_OFF";
      case kStateStartingUp:
        return "STATE_STARTING_UP";
      case kStateRunning:
        return "STATE_RUNNING";
      case kStateShuttingDown:
        return "STATE_SHUTTING_DOWN";
    }
  }

  void SetState(BtifA2dpSource::RunState state) { state_ = state; }

  fixed_queue_t* tx_audio_queue;
  bool tx_flush; /* Discards any outgoing data when true */
  bool sw_audio_is_encoding;
  RepeatingTimer media_alarm;
  const tA2DP_ENCODER_INTERFACE* encoder_interface;
  uint64_t encoder_interval_ms; /* Local copy of the encoder interval */
  BtifMediaStats stats;
  BtifMediaStats accumulated_stats;

private:
  BtifA2dpSource::RunState state_;
};

/// Source worker thread created to run the CPU heavy encoder calls.
/// Exactly three functions are executed on this thread:
///   - btif_a2dp_source_audio_handle_timer
///   - btif_a2dp_source_read_callback
///   - btif_a2dp_source_enqueue_callback
static bluetooth::common::MessageLoopThread btif_a2dp_source_thread("bt_a2dp_source_worker_thread");

static BtifA2dpSource btif_a2dp_source_cb;
static uint8_t btif_a2dp_source_dynamic_audio_buffer_size = MAX_OUTPUT_A2DP_FRAME_QUEUE_SZ;

static void btif_a2dp_source_init_delayed(void);
static bool btif_a2dp_source_startup(void);
static void btif_a2dp_source_startup_delayed(void);
static void btif_a2dp_source_start_session_delayed(const RawAddress& peer_address,
                                                   std::promise<void> start_session_promise);
static void btif_a2dp_source_end_session_delayed(const RawAddress& peer_address);
static void btif_a2dp_source_shutdown_delayed(std::promise<void>);
static void btif_a2dp_source_audio_tx_start_event(void);
static void btif_a2dp_source_audio_tx_stop_event(void);
static void btif_a2dp_source_audio_tx_flush_event(void);
// Set up the A2DP Source codec, and prepare the encoder.
// The peer address is |peer_addr|.
// This function should be called prior to starting A2DP streaming.
static void btif_a2dp_source_setup_codec(const RawAddress& peer_addr);
static void btif_a2dp_source_cleanup_codec();
static void btif_a2dp_source_cleanup_codec_delayed();
static void btif_a2dp_source_encoder_user_config_update_event(
        const RawAddress& peer_address,
        const std::vector<btav_a2dp_codec_config_t>& codec_user_preferences,
        std::promise<void> peer_ready_promise);
static void btif_a2dp_source_audio_feeding_update_event(
        const btav_a2dp_codec_config_t& codec_audio_config);
static bool btif_a2dp_source_audio_tx_flush_req(void);
static void btif_a2dp_source_audio_handle_timer(void);
static uint32_t btif_a2dp_source_read_callback(uint8_t* p_buf, uint32_t len);
static bool btif_a2dp_source_enqueue_callback(BT_HDR* p_buf, size_t frames_n, uint32_t bytes_read);
static void log_tstamps_us(const char* comment, uint64_t timestamp_us);
static void update_scheduling_stats(SchedulingStats* stats, uint64_t now_us,
                                    uint64_t expected_delta);
// Update the A2DP Source related metrics.
// This function should be called before collecting the metrics.
static void btif_a2dp_source_update_metrics(void);
static void btm_read_rssi_cb(void* data);
static void btm_read_failed_contact_counter_cb(void* data);
static void btm_read_tx_power_cb(void* data);

static void btif_a2dp_source_accumulate_scheduling_stats(SchedulingStats* src,
                                                         SchedulingStats* dst) {
  dst->total_updates += src->total_updates;
  dst->last_update_us = src->last_update_us;
  dst->overdue_scheduling_count += src->overdue_scheduling_count;
  dst->total_overdue_scheduling_delta_us += src->total_overdue_scheduling_delta_us;
  dst->max_overdue_scheduling_delta_us =
          std::max(dst->max_overdue_scheduling_delta_us, src->max_overdue_scheduling_delta_us);
  dst->premature_scheduling_count += src->premature_scheduling_count;
  dst->total_premature_scheduling_delta_us += src->total_premature_scheduling_delta_us;
  dst->max_premature_scheduling_delta_us =
          std::max(dst->max_premature_scheduling_delta_us, src->max_premature_scheduling_delta_us);
  dst->exact_scheduling_count += src->exact_scheduling_count;
  dst->total_scheduling_time_us += src->total_scheduling_time_us;
}

static void btif_a2dp_source_accumulate_stats(BtifMediaStats* src, BtifMediaStats* dst) {
  dst->tx_queue_total_frames += src->tx_queue_total_frames;
  dst->tx_queue_max_frames_per_packet =
          std::max(dst->tx_queue_max_frames_per_packet, src->tx_queue_max_frames_per_packet);
  dst->tx_queue_total_queueing_time_us += src->tx_queue_total_queueing_time_us;
  dst->tx_queue_max_queueing_time_us =
          std::max(dst->tx_queue_max_queueing_time_us, src->tx_queue_max_queueing_time_us);
  dst->tx_queue_total_readbuf_calls += src->tx_queue_total_readbuf_calls;
  dst->tx_queue_last_readbuf_us = src->tx_queue_last_readbuf_us;
  dst->tx_queue_total_flushed_messages += src->tx_queue_total_flushed_messages;
  dst->tx_queue_last_flushed_us = src->tx_queue_last_flushed_us;
  dst->tx_queue_total_dropped_messages += src->tx_queue_total_dropped_messages;
  dst->tx_queue_max_dropped_messages =
          std::max(dst->tx_queue_max_dropped_messages, src->tx_queue_max_dropped_messages);
  dst->tx_queue_dropouts += src->tx_queue_dropouts;
  dst->tx_queue_last_dropouts_us = src->tx_queue_last_dropouts_us;
  dst->media_read_total_underflow_bytes += src->media_read_total_underflow_bytes;
  dst->media_read_total_underflow_count += src->media_read_total_underflow_count;
  dst->media_read_last_underflow_us = src->media_read_last_underflow_us;
  if (dst->codec_index < 0) {
    dst->codec_index = src->codec_index;
  }
  btif_a2dp_source_accumulate_scheduling_stats(&src->tx_queue_enqueue_stats,
                                               &dst->tx_queue_enqueue_stats);
  btif_a2dp_source_accumulate_scheduling_stats(&src->tx_queue_dequeue_stats,
                                               &dst->tx_queue_dequeue_stats);
  src->Reset();
}

/// Select the thread to run a2dp source actions on (a2dp encoder excluded).
static bluetooth::common::MessageLoopThread* local_thread() {
  return com::android::bluetooth::flags::a2dp_source_threading_fix() ? get_main_thread()
                                                                     : &btif_a2dp_source_thread;
}

bool btif_a2dp_source_init(void) {
  log::info("");

  // Start A2DP Source media task
  btif_a2dp_source_thread.StartUp();

  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_init_delayed));
  return true;
}

class A2dpStreamCallbacks : public bluetooth::audio::a2dp::StreamCallbacks {
  Status StartStream(bool low_latency) const override {
    // Check if a phone call is currently active.
    if (!bluetooth::headset::IsCallIdle()) {
      log::error("unable to start stream: call is active");
      return Status::FAILURE;
    }

    // Check if LE Audio is currently active.
    if (com::android::bluetooth::flags::a2dp_check_lea_iso_channel() &&
        hci::IsoManager::GetInstance()->GetNumberOfActiveIso() > 0) {
      log::error("unable to start stream: LEA is active");
      return Status::FAILURE;
    }

    // Check if the stream has already been started.
    if (btif_av_stream_started_ready(A2dpType::kSource)) {
      log::verbose("stream is already started");
      return Status::SUCCESS;
    }

    // Check if the stream is ready to start.
    if (!btif_av_stream_ready(A2dpType::kSource)) {
      log::error("unable to start stream: not ready");
      return Status::FAILURE;
    }

    // Check if codec needs to be switched prior to stream start.
    invoke_switch_codec_cb(low_latency);

    // Post start event. The start request is pending, completion will be
    // notified to bluetooth::audio::a2dp::ack_stream_started.
    btif_av_stream_start_with_latency(low_latency);
    return Status::PENDING;
  }

  Status SuspendStream() const override {
    // Check if the stream is already suspended.
    if (!btif_av_stream_started_ready(A2dpType::kSource)) {
      btif_av_clear_remote_suspend_flag(A2dpType::kSource);
      log::verbose("stream is already suspended");
      return Status::SUCCESS;
    }

    // Post suspend event. The suspend request is pending, completion will
    // be notified to bluetooth::audio::a2dp::ack_stream_suspended.
    btif_av_stream_suspend();
    return Status::PENDING;
  }

  Status StopStream() const override {
    // Check if the stream is already suspended.
    if (!btif_av_stream_started_ready(A2dpType::kSource)) {
      btif_av_clear_remote_suspend_flag(A2dpType::kSource);
      log::verbose("stream is already stopped");
      return Status::SUCCESS;
    }

    // Post stop event. The stop request is pending, but completion is not
    // notified to the HAL.
    btif_av_stream_stop(RawAddress::kEmpty);
    return Status::PENDING;
  }

  Status SetLatencyMode(bool low_latency) const override {
    btif_av_set_low_latency(low_latency);
    return Status::SUCCESS;
  }
};

static const A2dpStreamCallbacks a2dp_stream_callbacks;

static void btif_a2dp_source_init_delayed(void) {
  log::info("");
  // When codec extensibility is enabled in the audio HAL interface,
  // the provider needs to be initialized earlier in order to ensure
  // get_a2dp_configuration and parse_a2dp_configuration can be
  // invoked before the stream is started.
  bluetooth::audio::a2dp::init(local_thread(), &a2dp_stream_callbacks,
                               btif_av_is_a2dp_offload_enabled());
}

static bool btif_a2dp_source_startup(void) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  if (btif_a2dp_source_cb.State() != BtifA2dpSource::kStateOff) {
    log::error("A2DP Source media task already running");
    return false;
  }

  btif_a2dp_source_cb.Reset();
  btif_a2dp_source_cb.SetState(BtifA2dpSource::kStateStartingUp);
  btif_a2dp_source_cb.tx_audio_queue = fixed_queue_new(SIZE_MAX);

  // Schedule the rest of the operations
  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_startup_delayed));

  return true;
}

static void btif_a2dp_source_startup_delayed() {
  log::info("state={}", btif_a2dp_source_cb.StateStr());
  if (!btif_a2dp_source_thread.EnableRealTimeScheduling()) {
#if defined(__ANDROID__)
    log::fatal("unable to enable real time scheduling");
#endif
  }
  if (!bluetooth::audio::a2dp::init(local_thread(), &a2dp_stream_callbacks,
                                    btif_av_is_a2dp_offload_enabled())) {
    log::warn("Failed to setup the bluetooth audio HAL");
  }
  btif_a2dp_source_cb.SetState(BtifA2dpSource::kStateRunning);
}

bool btif_a2dp_source_start_session(const RawAddress& peer_address,
                                    std::promise<void> peer_ready_promise) {
  log::info("peer_address={} state={}", peer_address, btif_a2dp_source_cb.StateStr());

  btif_a2dp_source_audio_tx_flush_req();

  if (local_thread()->DoInThread(
              FROM_HERE, base::BindOnce(&btif_a2dp_source_start_session_delayed, peer_address,
                                        std::move(peer_ready_promise)))) {
    return true;
  } else {
    // cannot set promise but triggers crash
    log::fatal("peer_address={} state={} fails to context switch", peer_address,
               btif_a2dp_source_cb.StateStr());
    return false;
  }
}

static void btif_a2dp_source_start_session_delayed(const RawAddress& peer_address,
                                                   std::promise<void> peer_ready_promise) {
  log::info("peer_address={} state={}", peer_address, btif_a2dp_source_cb.StateStr());

  btif_a2dp_source_setup_codec(peer_address);

  if (btif_a2dp_source_cb.State() != BtifA2dpSource::kStateRunning) {
    log::error("A2DP Source media task is not running");
    peer_ready_promise.set_value();
    return;
  }

  if (bluetooth::audio::a2dp::is_hal_enabled()) {
    bluetooth::audio::a2dp::start_session();
    bluetooth::audio::a2dp::set_remote_delay(btif_av_get_audio_delay(A2dpType::kSource));
    BluetoothMetricsLogger::GetInstance()->LogBluetoothSessionStart(
            bluetooth::common::CONNECTION_TECHNOLOGY_TYPE_BREDR, 0);
  } else {
    BluetoothMetricsLogger::GetInstance()->LogBluetoothSessionStart(
            bluetooth::common::CONNECTION_TECHNOLOGY_TYPE_BREDR, 0);
  }

  peer_ready_promise.set_value();
}

bool btif_a2dp_source_restart_session(const RawAddress& old_peer_address,
                                      const RawAddress& new_peer_address,
                                      std::promise<void> peer_ready_promise) {
  log::info("old_peer_address={} new_peer_address={} state={}", old_peer_address, new_peer_address,
            btif_a2dp_source_cb.StateStr());

  log::assert_that(!new_peer_address.IsEmpty(), "assert failed: !new_peer_address.IsEmpty()");

  // Must stop first the audio streaming
  btif_a2dp_source_stop_audio_req();

  // If the old active peer was valid, end the old session.
  // Otherwise, time to startup the A2DP Source processing.
  if (!old_peer_address.IsEmpty()) {
    btif_a2dp_source_end_session(old_peer_address);
  } else {
    btif_a2dp_source_startup();
  }

  // Start the session.
  btif_a2dp_source_start_session(new_peer_address, std::move(peer_ready_promise));
  // If audio was streaming before, DON'T start audio streaming, but leave the
  // control to the audio HAL.
  return true;
}

bool btif_a2dp_source_end_session(const RawAddress& peer_address) {
  log::info("peer_address={} state={}", peer_address, btif_a2dp_source_cb.StateStr());
  local_thread()->DoInThread(FROM_HERE,
                             base::BindOnce(&btif_a2dp_source_end_session_delayed, peer_address));
  btif_a2dp_source_cleanup_codec();
  return true;
}

static void btif_a2dp_source_end_session_delayed(const RawAddress& peer_address) {
  log::info("peer_address={} state={}", peer_address, btif_a2dp_source_cb.StateStr());
  if ((btif_a2dp_source_cb.State() == BtifA2dpSource::kStateRunning) ||
      (btif_a2dp_source_cb.State() == BtifA2dpSource::kStateShuttingDown)) {
    btif_av_stream_stop(peer_address);
  } else {
    log::error("A2DP Source media task is not running");
  }
  if (bluetooth::audio::a2dp::is_hal_enabled()) {
    bluetooth::audio::a2dp::end_session();
    BluetoothMetricsLogger::GetInstance()->LogBluetoothSessionEnd(
            bluetooth::common::DISCONNECT_REASON_UNKNOWN, 0);
  } else {
    BluetoothMetricsLogger::GetInstance()->LogBluetoothSessionEnd(
            bluetooth::common::DISCONNECT_REASON_UNKNOWN, 0);
  }
}

void btif_a2dp_source_allow_low_latency_audio(bool allowed) {
  log::info("allowed={}", allowed);

  local_thread()->DoInThread(
          FROM_HERE,
          base::BindOnce(bluetooth::audio::a2dp::set_audio_low_latency_mode_allowed, allowed));
}

void btif_a2dp_source_shutdown(std::promise<void> shutdown_complete_promise) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  if ((btif_a2dp_source_cb.State() == BtifA2dpSource::kStateOff) ||
      (btif_a2dp_source_cb.State() == BtifA2dpSource::kStateShuttingDown)) {
    return;
  }

  /* Make sure no channels are restarted while shutting down */
  btif_a2dp_source_cb.SetState(BtifA2dpSource::kStateShuttingDown);

  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_shutdown_delayed,
                                                       std::move(shutdown_complete_promise)));
}

static void btif_a2dp_source_shutdown_delayed(std::promise<void> shutdown_complete_promise) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  // Stop the timer
  btif_a2dp_source_cb.media_alarm.CancelAndWait();
  wakelock_release();

  bluetooth::audio::a2dp::cleanup();

  fixed_queue_free(btif_a2dp_source_cb.tx_audio_queue, nullptr);
  btif_a2dp_source_cb.tx_audio_queue = nullptr;

  btif_a2dp_source_cb.SetState(BtifA2dpSource::kStateOff);

  shutdown_complete_promise.set_value();
}

void btif_a2dp_source_cleanup(void) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  // Make sure the source is shutdown
  std::promise<void> shutdown_complete_promise;
  btif_a2dp_source_shutdown(std::move(shutdown_complete_promise));

  // Exit the thread
  btif_a2dp_source_thread.ShutDown();
}

// This runs on worker thread
bool btif_a2dp_source_is_streaming(void) { return btif_a2dp_source_cb.media_alarm.IsScheduled(); }

/// Return the MTU for the active peer audio connection.
static uint16_t btif_a2dp_get_peer_mtu(A2dpCodecConfig* a2dp_config) {
  uint8_t codec_info[AVDT_CODEC_SIZE];
  a2dp_config->copyOutOtaCodecConfig(codec_info);

  RawAddress peer_addr = btif_av_source_active_peer();
  tA2DP_ENCODER_INIT_PEER_PARAMS peer_params;
  bta_av_co_get_peer_params(peer_addr, &peer_params);
  uint16_t peer_mtu = peer_params.peer_mtu;
  uint16_t effective_mtu = bta_av_co_get_encoder_effective_frame_size(peer_addr);

  if (effective_mtu > 0 && effective_mtu < peer_mtu) {
    peer_mtu = effective_mtu;
  }

  // b/188020925
  // When SBC headsets report middle quality bitpool under a larger MTU, we
  // reduce the packet size to prevent the hardware encoder from putting too
  // many frames in one packet.
  if (a2dp_config->codecIndex() == BTAV_A2DP_CODEC_INDEX_SOURCE_SBC &&
      codec_info[2] /* maxBitpool */ <= A2DP_SBC_BITPOOL_MIDDLE_QUALITY) {
    peer_mtu = MAX_2MBPS_AVDTP_MTU;
  }

  // b/177205770
  // Fix the MTU value not to be greater than an AVDTP packet, so the data
  // encoded by A2DP hardware encoder can be fitted into one AVDTP packet
  // without fragmented
  if (peer_mtu > MAX_3MBPS_AVDTP_MTU) {
    peer_mtu = MAX_3MBPS_AVDTP_MTU;
  }

  return peer_mtu;
}

static void btif_a2dp_source_setup_codec(const RawAddress& peer_address) {
  log::info("peer_address={} state={}", peer_address, btif_a2dp_source_cb.StateStr());

  tA2DP_ENCODER_INIT_PEER_PARAMS peer_params;
  bta_av_co_get_peer_params(peer_address, &peer_params);
  if (!bta_av_co_set_active_source_peer(peer_address)) {
    log::error("Cannot stream audio: cannot set active peer to {}", peer_address);
    return;
  }

  const tA2DP_ENCODER_INTERFACE* encoder_interface = bta_av_co_get_encoder_interface(peer_address);
  if (encoder_interface == nullptr) {
    log::error("Cannot stream audio: no source encoder interface");
    return;
  }

  A2dpCodecConfig* a2dp_codec_config = bta_av_get_a2dp_current_codec();
  if (a2dp_codec_config == nullptr) {
    log::error("Cannot stream audio: current codec is not set");
    return;
  }

  encoder_interface->encoder_init(&peer_params, a2dp_codec_config, btif_a2dp_source_read_callback,
                                  btif_a2dp_source_enqueue_callback);

  // Save a local copy of the encoder_interval_ms
  btif_a2dp_source_cb.encoder_interface = encoder_interface;
  btif_a2dp_source_cb.encoder_interval_ms =
          btif_a2dp_source_cb.encoder_interface->get_encoder_interval_ms();

  if (bluetooth::audio::a2dp::is_hal_enabled()) {
    bluetooth::audio::a2dp::setup_codec(a2dp_codec_config,
                                        btif_a2dp_get_peer_mtu(a2dp_codec_config),
                                        bta_av_co_get_encoder_preferred_interval_us());
  }
}

static void btif_a2dp_source_cleanup_codec() {
  log::info("state={}", btif_a2dp_source_cb.StateStr());
  // Must stop media task first before cleaning up the encoder
  btif_a2dp_source_stop_audio_req();
  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_cleanup_codec_delayed));
}

static void btif_a2dp_source_cleanup_codec_delayed() {
  log::info("state={}", btif_a2dp_source_cb.StateStr());
  if (btif_a2dp_source_cb.encoder_interface != nullptr) {
    btif_a2dp_source_cb.encoder_interface->encoder_cleanup();
    btif_a2dp_source_cb.encoder_interface = nullptr;
  }
}

void btif_a2dp_source_start_audio_req(void) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_audio_tx_start_event));
}

void btif_a2dp_source_stop_audio_req(void) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_audio_tx_stop_event));
}

void btif_a2dp_source_encoder_user_config_update_req(
        const RawAddress& peer_address,
        const std::vector<btav_a2dp_codec_config_t>& codec_user_preferences,
        std::promise<void> peer_ready_promise) {
  log::info("peer_address={} state={} {} codec_preference(s)", peer_address,
            btif_a2dp_source_cb.StateStr(), codec_user_preferences.size());

  if (!local_thread()->DoInThread(
              FROM_HERE,
              base::BindOnce(&btif_a2dp_source_encoder_user_config_update_event, peer_address,
                             codec_user_preferences, std::move(peer_ready_promise)))) {
    // cannot set promise but triggers crash
    log::fatal("peer_address={} state={} fails to context switch", peer_address,
               btif_a2dp_source_cb.StateStr());
  }
}

static void btif_a2dp_source_encoder_user_config_update_event(
        const RawAddress& peer_address,
        const std::vector<btav_a2dp_codec_config_t>& codec_user_preferences,
        std::promise<void> peer_ready_promise) {
  bool restart_output = false;
  bool success = false;
  for (auto codec_user_config : codec_user_preferences) {
    success = bta_av_co_set_codec_user_config(peer_address, codec_user_config, &restart_output);
    if (success) {
      log::info("peer_address={} state={} codec_preference=[{}] restart_output={}", peer_address,
                btif_a2dp_source_cb.StateStr(), codec_user_config.ToString(), restart_output);
      break;
    }
  }
  if (success && restart_output) {
    // Codec reconfiguration is in progress, and it is safe to unlock since
    // remaining tasks like starting audio session and reporting new codec
    // will be handled by BTA_AV_RECONFIG_EVT later.
    peer_ready_promise.set_value();
    return;
  }
  if (!success) {
    log::error("cannot update codec user configuration(s)");
  }
  if (!peer_address.IsEmpty() && peer_address == btif_av_source_active_peer()) {
    // No more actions needed with remote, and if succeed, user had changed the
    // config like the bits per sample only. Let's resume the session now.
    btif_a2dp_source_start_session(peer_address, std::move(peer_ready_promise));
  } else {
    // Unlock for non-active peer
    peer_ready_promise.set_value();
  }
}

void btif_a2dp_source_feeding_update_req(const btav_a2dp_codec_config_t& codec_audio_config) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());
  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_audio_feeding_update_event,
                                                       codec_audio_config));
}

static void btif_a2dp_source_audio_feeding_update_event(
        const btav_a2dp_codec_config_t& codec_audio_config) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());
  if (!bta_av_co_set_codec_audio_config(codec_audio_config)) {
    log::error("cannot update codec audio feeding parameters");
  }
}

void btif_a2dp_source_on_idle(void) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());
  if (btif_a2dp_source_cb.State() == BtifA2dpSource::kStateOff) {
    return;
  }

  /* Make sure media task is stopped */
  btif_a2dp_source_stop_audio_req();
}

void btif_a2dp_source_on_stopped(tBTA_AV_SUSPEND* p_av_suspend) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  btif_a2dp_source_cb.sw_audio_is_encoding = false;

  // allow using this API for other (acknowledgement and stopping media task)
  // than suspend
  if (p_av_suspend != nullptr && p_av_suspend->status != BTA_AV_SUCCESS) {
    log::error("A2DP stop failed: status={}, initiator={}", p_av_suspend->status,
               p_av_suspend->initiator);
    if (p_av_suspend->initiator) {
      bluetooth::audio::a2dp::ack_stream_suspended(Status::FAILURE);
    }
  } else {
    bluetooth::audio::a2dp::ack_stream_suspended(Status::SUCCESS);
  }

  if (btif_a2dp_source_cb.State() == BtifA2dpSource::kStateOff) {
    return;
  }

  // ensure tx frames are immediately suspended
  btif_a2dp_source_cb.tx_flush = true;
  // ensure tx frames are immediately flushed
  btif_a2dp_source_audio_tx_flush_req();

  // request to stop media task
  btif_a2dp_source_stop_audio_req();

  // once software stream is fully stopped we will ack back
}

void btif_a2dp_source_on_suspended(tBTA_AV_SUSPEND* p_av_suspend) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  if (btif_a2dp_source_cb.State() == BtifA2dpSource::kStateOff) {
    return;
  }

  log::assert_that(p_av_suspend != nullptr, "Suspend result could not be nullptr");

  // check for status failures
  if (p_av_suspend->status != BTA_AV_SUCCESS) {
    log::warn("A2DP suspend failed: status={}, initiator={}", p_av_suspend->status,
              p_av_suspend->initiator);
    if (p_av_suspend->initiator) {
      bluetooth::audio::a2dp::ack_stream_suspended(Status::FAILURE);
    }
  } else if (btif_av_is_a2dp_offload_running()) {
    bluetooth::audio::a2dp::ack_stream_suspended(Status::SUCCESS);
  }

  // ensure tx frames are immediately suspended
  btif_a2dp_source_cb.tx_flush = true;

  // stop timer tick
  btif_a2dp_source_stop_audio_req();

  // once software stream is fully stopped we will ack back
}

/* when true media task discards any tx frames */
void btif_a2dp_source_set_tx_flush(bool enable) {
  log::info("enable={} state={}", enable, btif_a2dp_source_cb.StateStr());
  btif_a2dp_source_cb.tx_flush = enable;
}

static void btif_a2dp_source_audio_tx_start_event(void) {
  log::info("streaming {} state={}", btif_a2dp_source_is_streaming(),
            btif_a2dp_source_cb.StateStr());

  btif_a2dp_source_cb.stats.Reset();
  btif_a2dp_source_cb.stats.session_start_us = bluetooth::common::time_get_os_boottime_us();
  btif_a2dp_source_cb.stats.session_end_us = 0;

  A2dpCodecConfig* codec_config = bta_av_get_a2dp_current_codec();
  if (codec_config != nullptr) {
    btif_a2dp_source_cb.stats.codec_index = codec_config->codecIndex();
  }

  if (btif_av_is_a2dp_offload_running()) {
    return;
  }

  log::assert_that(btif_a2dp_source_cb.encoder_interface != nullptr,
                   "assert failed: btif_a2dp_source_cb.encoder_interface != nullptr");

  log::verbose("starting media encoder timer with interval {}ms",
               btif_a2dp_source_cb.encoder_interface->get_encoder_interval_ms());

  wakelock_acquire();
  btif_a2dp_source_cb.encoder_interface->feeding_reset();
  btif_a2dp_source_cb.tx_flush = false;
  btif_a2dp_source_cb.sw_audio_is_encoding = true;
  btif_a2dp_source_cb.media_alarm.SchedulePeriodic(
          btif_a2dp_source_thread.GetWeakPtr(), FROM_HERE,
          base::BindRepeating(&btif_a2dp_source_audio_handle_timer),
          std::chrono::milliseconds(
                  btif_a2dp_source_cb.encoder_interface->get_encoder_interval_ms()));
}

static void btif_a2dp_source_audio_tx_stop_event(void) {
  log::info("streaming {} state={}", btif_a2dp_source_is_streaming(),
            btif_a2dp_source_cb.StateStr());

  btif_a2dp_source_cb.stats.session_end_us = bluetooth::common::time_get_os_boottime_us();

  btif_a2dp_source_update_metrics();
  btif_a2dp_source_accumulate_stats(&btif_a2dp_source_cb.stats,
                                    &btif_a2dp_source_cb.accumulated_stats);

  if (btif_av_is_a2dp_offload_running()) {
    return;
  }

  if (!btif_a2dp_source_is_streaming()) {
    return;
  }

  /* Drain data still left in the queue */
  static constexpr size_t AUDIO_STREAM_OUTPUT_BUFFER_SZ = 28 * 512;
  uint8_t p_buf[AUDIO_STREAM_OUTPUT_BUFFER_SZ * 2];
  bluetooth::audio::a2dp::read(p_buf, sizeof(p_buf));

  /* Stop the timer first */
  btif_a2dp_source_cb.media_alarm.CancelAndWait();
  wakelock_release();

  bluetooth::audio::a2dp::ack_stream_suspended(Status::SUCCESS);

  /* audio engine stopped, reset tx suspended flag */
  btif_a2dp_source_cb.tx_flush = false;

  /* Reset the media feeding state */
  if (btif_a2dp_source_cb.encoder_interface != nullptr) {
    btif_a2dp_source_cb.encoder_interface->feeding_reset();
  }
}

/// Periodic task responsible for encoding the audio stream and forwarding
/// to the remote device. It will read PCM samples from the HAL provided FMQ,
/// encode them into audio frames. Runs on the source worker thread.
///
/// The timer driving the periodic task is cancelled before any state cleanup
/// when the stream is ended.
static void btif_a2dp_source_audio_handle_timer(void) {
  uint64_t timestamp_us = bluetooth::common::time_get_audio_server_tick_us();
  uint64_t stats_timestamp_us = bluetooth::common::time_get_os_boottime_us();

  log_tstamps_us("A2DP Source tx scheduling timer", timestamp_us);

  log::assert_that(btif_a2dp_source_cb.encoder_interface != nullptr,
                   "assert failed: btif_a2dp_source_cb.encoder_interface != nullptr");

  size_t transmit_queue_length = fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);

#ifdef __ANDROID__
  ATRACE_INT("btif TX queue", transmit_queue_length);
#endif

  if (btif_a2dp_source_cb.encoder_interface->set_transmit_queue_length != nullptr) {
    btif_a2dp_source_cb.encoder_interface->set_transmit_queue_length(transmit_queue_length);
  }

  btif_a2dp_source_cb.encoder_interface->send_frames(timestamp_us);

  bta_av_ci_src_data_ready(BTA_AV_CHNL_AUDIO);
  update_scheduling_stats(&btif_a2dp_source_cb.stats.tx_queue_enqueue_stats, stats_timestamp_us,
                          btif_a2dp_source_cb.encoder_interval_ms * 1000);
}

/// Callback invoked by the encoder for reading PCM audio data from the
/// Bluetooth Audio HAL. Runs on the source worker thread.
static uint32_t btif_a2dp_source_read_callback(uint8_t* p_buf, uint32_t len) {
  if (!btif_a2dp_source_cb.sw_audio_is_encoding) {
    return 0;
  }

  uint32_t bytes_read = bluetooth::audio::a2dp::read(p_buf, len);

  if (bytes_read < len) {
    log::warn("UNDERFLOW: ONLY READ {} BYTES OUT OF {}", bytes_read, len);
    btif_a2dp_source_cb.stats.media_read_total_underflow_bytes += (len - bytes_read);
    btif_a2dp_source_cb.stats.media_read_total_underflow_count++;
    btif_a2dp_source_cb.stats.media_read_last_underflow_us =
            bluetooth::common::time_get_os_boottime_us();
    log_a2dp_audio_underrun_event(btif_av_source_active_peer(),
                                  btif_a2dp_source_cb.encoder_interval_ms, len - bytes_read);
  }

  return bytes_read;
}

/// Callback invoked by the encoder for sending encoded audio frames to the
/// remote Bluetooth device. Runs on the source worker thread.
static bool btif_a2dp_source_enqueue_callback(BT_HDR* p_buf, size_t frames_n,
                                              uint32_t /*bytes_read*/) {
  uint64_t now_us = bluetooth::common::time_get_os_boottime_us();

  // Check if the transmission queue has been flushed.
  if (btif_a2dp_source_cb.tx_flush) {
    log::verbose("tx suspended, discarded frame");

    btif_a2dp_source_cb.stats.tx_queue_total_flushed_messages +=
            fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);
    btif_a2dp_source_cb.stats.tx_queue_last_flushed_us = now_us;
    fixed_queue_flush(btif_a2dp_source_cb.tx_audio_queue, osi_free);

    osi_free(p_buf);
    return false;
  }

  // Check for TX queue overflow
  // TODO: Using frames_n here is probably wrong: should be "+ 1" instead.
  if (fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue) + frames_n >
      btif_a2dp_source_dynamic_audio_buffer_size) {
    log::warn("TX queue buffer size now={} adding={} max={}",
              (uint32_t)fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue), (uint32_t)frames_n,
              btif_a2dp_source_dynamic_audio_buffer_size);
    // Keep track of drop-outs
    btif_a2dp_source_cb.stats.tx_queue_dropouts++;
    btif_a2dp_source_cb.stats.tx_queue_last_dropouts_us = now_us;

    // Flush all queued buffers
    size_t drop_n = fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);
    btif_a2dp_source_cb.stats.tx_queue_max_dropped_messages =
            std::max(drop_n, btif_a2dp_source_cb.stats.tx_queue_max_dropped_messages);
    int num_dropped_encoded_bytes = 0;
    int num_dropped_encoded_frames = 0;
    while (fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue)) {
      btif_a2dp_source_cb.stats.tx_queue_total_dropped_messages++;
      void* p_data = fixed_queue_try_dequeue(btif_a2dp_source_cb.tx_audio_queue);
      if (p_data != nullptr) {
        auto p_dropped_buf = static_cast<BT_HDR*>(p_data);
        num_dropped_encoded_bytes += p_dropped_buf->len;
        num_dropped_encoded_frames += p_dropped_buf->layer_specific;
        osi_free(p_data);
      }
    }
    log_a2dp_audio_overrun_event(btif_av_source_active_peer(),
                                 btif_a2dp_source_cb.encoder_interval_ms, drop_n,
                                 num_dropped_encoded_frames, num_dropped_encoded_bytes);

    // Request additional debug info if we had to flush buffers
    RawAddress peer_bda = btif_av_source_active_peer();
    tBTM_STATUS status =
            get_btm_client_interface().link_controller.BTM_ReadRSSI(peer_bda, btm_read_rssi_cb);
    if (status != tBTM_STATUS::BTM_CMD_STARTED) {
      log::warn("Cannot read RSSI: status {}", status);
    }

    status = BTM_ReadFailedContactCounter(peer_bda, btm_read_failed_contact_counter_cb);
    if (status != tBTM_STATUS::BTM_CMD_STARTED) {
      log::warn("Cannot read Failed Contact Counter: status {}", status);
    }

    status = BTM_ReadTxPower(peer_bda, BT_TRANSPORT_BR_EDR, btm_read_tx_power_cb);
    if (status != tBTM_STATUS::BTM_CMD_STARTED) {
      log::warn("Cannot read Tx Power: status {}", status);
    }
  }

  // Update the statistics.
  btif_a2dp_source_cb.stats.tx_queue_total_frames += frames_n;
  btif_a2dp_source_cb.stats.tx_queue_max_frames_per_packet =
          std::max(frames_n, btif_a2dp_source_cb.stats.tx_queue_max_frames_per_packet);

  fixed_queue_enqueue(btif_a2dp_source_cb.tx_audio_queue, p_buf);

  return true;
}

static void btif_a2dp_source_audio_tx_flush_event(void) {
  /* Flush all enqueued audio buffers (encoded) */
  log::info("state={}", btif_a2dp_source_cb.StateStr());
  if (btif_av_is_a2dp_offload_running()) {
    return;
  }

  if (btif_a2dp_source_cb.encoder_interface != nullptr) {
    btif_a2dp_source_cb.encoder_interface->feeding_flush();
  }

  btif_a2dp_source_cb.stats.tx_queue_total_flushed_messages +=
          fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue);
  btif_a2dp_source_cb.stats.tx_queue_last_flushed_us = bluetooth::common::time_get_os_boottime_us();
  fixed_queue_flush(btif_a2dp_source_cb.tx_audio_queue, osi_free);
}

static bool btif_a2dp_source_audio_tx_flush_req(void) {
  log::info("state={}", btif_a2dp_source_cb.StateStr());

  local_thread()->DoInThread(FROM_HERE, base::BindOnce(&btif_a2dp_source_audio_tx_flush_event));
  return true;
}

BT_HDR* btif_a2dp_source_audio_readbuf(void) {
  uint64_t now_us = bluetooth::common::time_get_os_boottime_us();
  BT_HDR* p_buf = (BT_HDR*)fixed_queue_try_dequeue(btif_a2dp_source_cb.tx_audio_queue);

  btif_a2dp_source_cb.stats.tx_queue_total_readbuf_calls++;
  btif_a2dp_source_cb.stats.tx_queue_last_readbuf_us = now_us;
  if (p_buf != nullptr) {
    // Update the statistics
    update_scheduling_stats(&btif_a2dp_source_cb.stats.tx_queue_dequeue_stats, now_us,
                            btif_a2dp_source_cb.encoder_interval_ms * 1000);
  }

  return p_buf;
}

static void log_tstamps_us(const char* comment, uint64_t timestamp_us) {
  static uint64_t prev_us = 0;
  log::verbose("[{}] ts {:08}, diff : {:08}, queue sz {}", comment, timestamp_us,
               timestamp_us - prev_us, fixed_queue_length(btif_a2dp_source_cb.tx_audio_queue));
  prev_us = timestamp_us;
}

static void update_scheduling_stats(SchedulingStats* stats, uint64_t now_us,
                                    uint64_t expected_delta) {
  uint64_t last_us = stats->last_update_us;

  stats->total_updates++;
  stats->last_update_us = now_us;

  if (last_us == 0) {
    return;  // First update: expected delta doesn't apply
  }

  uint64_t deadline_us = last_us + expected_delta;
  if (deadline_us < now_us) {
    // Overdue scheduling
    uint64_t delta_us = now_us - deadline_us;
    // Ignore extreme outliers
    if (delta_us < 10 * expected_delta) {
      stats->max_overdue_scheduling_delta_us =
              std::max(delta_us, stats->max_overdue_scheduling_delta_us);
      stats->total_overdue_scheduling_delta_us += delta_us;
      stats->overdue_scheduling_count++;
      stats->total_scheduling_time_us += now_us - last_us;
    }
  } else if (deadline_us > now_us) {
    // Premature scheduling
    uint64_t delta_us = deadline_us - now_us;
    // Ignore extreme outliers
    if (delta_us < 10 * expected_delta) {
      stats->max_premature_scheduling_delta_us =
              std::max(delta_us, stats->max_premature_scheduling_delta_us);
      stats->total_premature_scheduling_delta_us += delta_us;
      stats->premature_scheduling_count++;
      stats->total_scheduling_time_us += now_us - last_us;
    }
  } else {
    // On-time scheduling
    stats->exact_scheduling_count++;
    stats->total_scheduling_time_us += now_us - last_us;
  }
}

void btif_a2dp_source_debug_dump(int fd) {
  btif_a2dp_source_accumulate_stats(&btif_a2dp_source_cb.stats,
                                    &btif_a2dp_source_cb.accumulated_stats);
  uint64_t now_us = bluetooth::common::time_get_os_boottime_us();
  BtifMediaStats* accumulated_stats = &btif_a2dp_source_cb.accumulated_stats;
  SchedulingStats* enqueue_stats = &accumulated_stats->tx_queue_enqueue_stats;
  SchedulingStats* dequeue_stats = &accumulated_stats->tx_queue_dequeue_stats;
  size_t ave_size;
  uint64_t ave_time_us;

  dprintf(fd, "\nA2DP State:\n");
  dprintf(fd, "  TxQueue:\n");

  dprintf(fd, "  Counts (enqueue/dequeue/readbuf)                        : %zu / %zu / %zu\n",
          enqueue_stats->total_updates, dequeue_stats->total_updates,
          accumulated_stats->tx_queue_total_readbuf_calls);

  dprintf(fd, "  Last update time ago in ms (enqueue/dequeue/readbuf)    : %llu / %llu / %llu\n",
          (enqueue_stats->last_update_us > 0)
                  ? (unsigned long long)(now_us - enqueue_stats->last_update_us) / 1000
                  : 0,
          (dequeue_stats->last_update_us > 0)
                  ? (unsigned long long)(now_us - dequeue_stats->last_update_us) / 1000
                  : 0,
          (accumulated_stats->tx_queue_last_readbuf_us > 0)
                  ? (unsigned long long)(now_us - accumulated_stats->tx_queue_last_readbuf_us) /
                            1000
                  : 0);

  ave_size = 0;
  if (enqueue_stats->total_updates != 0) {
    ave_size = accumulated_stats->tx_queue_total_frames / enqueue_stats->total_updates;
  }
  dprintf(fd, "  Frames per packet (total/max/ave)                       : %zu / %zu / %zu\n",
          accumulated_stats->tx_queue_total_frames,
          accumulated_stats->tx_queue_max_frames_per_packet, ave_size);

  dprintf(fd, "  Counts (flushed/dropped/dropouts)                       : %zu / %zu / %zu\n",
          accumulated_stats->tx_queue_total_flushed_messages,
          accumulated_stats->tx_queue_total_dropped_messages, accumulated_stats->tx_queue_dropouts);

  dprintf(fd, "  Counts (max dropped)                                    : %zu\n",
          accumulated_stats->tx_queue_max_dropped_messages);

  dprintf(fd, "  Last update time ago in ms (flushed/dropped)            : %llu / %llu\n",
          (accumulated_stats->tx_queue_last_flushed_us > 0)
                  ? (unsigned long long)(now_us - accumulated_stats->tx_queue_last_flushed_us) /
                            1000
                  : 0,
          (accumulated_stats->tx_queue_last_dropouts_us > 0)
                  ? (unsigned long long)(now_us - accumulated_stats->tx_queue_last_dropouts_us) /
                            1000
                  : 0);

  dprintf(fd, "  Counts (underflow)                                      : %zu\n",
          accumulated_stats->media_read_total_underflow_count);

  dprintf(fd, "  Bytes (underflow)                                       : %zu\n",
          accumulated_stats->media_read_total_underflow_bytes);

  dprintf(fd, "  Last update time ago in ms (underflow)                  : %llu\n",
          (accumulated_stats->media_read_last_underflow_us > 0)
                  ? (unsigned long long)(now_us - accumulated_stats->media_read_last_underflow_us) /
                            1000
                  : 0);

  //
  // TxQueue enqueue stats
  //
  dprintf(fd, "  Enqueue deviation counts (overdue/premature)            : %zu / %zu\n",
          enqueue_stats->overdue_scheduling_count, enqueue_stats->premature_scheduling_count);

  ave_time_us = 0;
  if (enqueue_stats->overdue_scheduling_count != 0) {
    ave_time_us = enqueue_stats->total_overdue_scheduling_delta_us /
                  enqueue_stats->overdue_scheduling_count;
  }
  dprintf(fd, "  Enqueue overdue scheduling time in ms (total/max/ave)   : %llu / %llu / %llu\n",
          (unsigned long long)enqueue_stats->total_overdue_scheduling_delta_us / 1000,
          (unsigned long long)enqueue_stats->max_overdue_scheduling_delta_us / 1000,
          (unsigned long long)ave_time_us / 1000);

  ave_time_us = 0;
  if (enqueue_stats->premature_scheduling_count != 0) {
    ave_time_us = enqueue_stats->total_premature_scheduling_delta_us /
                  enqueue_stats->premature_scheduling_count;
  }
  dprintf(fd, "  Enqueue premature scheduling time in ms (total/max/ave) : %llu / %llu / %llu\n",
          (unsigned long long)enqueue_stats->total_premature_scheduling_delta_us / 1000,
          (unsigned long long)enqueue_stats->max_premature_scheduling_delta_us / 1000,
          (unsigned long long)ave_time_us / 1000);

  //
  // TxQueue dequeue stats
  //
  dprintf(fd, "  Dequeue deviation counts (overdue/premature)            : %zu / %zu\n",
          dequeue_stats->overdue_scheduling_count, dequeue_stats->premature_scheduling_count);

  ave_time_us = 0;
  if (dequeue_stats->overdue_scheduling_count != 0) {
    ave_time_us = dequeue_stats->total_overdue_scheduling_delta_us /
                  dequeue_stats->overdue_scheduling_count;
  }
  dprintf(fd, "  Dequeue overdue scheduling time in ms (total/max/ave)   : %llu / %llu / %llu\n",
          (unsigned long long)dequeue_stats->total_overdue_scheduling_delta_us / 1000,
          (unsigned long long)dequeue_stats->max_overdue_scheduling_delta_us / 1000,
          (unsigned long long)ave_time_us / 1000);

  ave_time_us = 0;
  if (dequeue_stats->premature_scheduling_count != 0) {
    ave_time_us = dequeue_stats->total_premature_scheduling_delta_us /
                  dequeue_stats->premature_scheduling_count;
  }
  dprintf(fd, "  Dequeue premature scheduling time in ms (total/max/ave) : %llu / %llu / %llu\n",
          (unsigned long long)dequeue_stats->total_premature_scheduling_delta_us / 1000,
          (unsigned long long)dequeue_stats->max_premature_scheduling_delta_us / 1000,
          (unsigned long long)ave_time_us / 1000);
}

static void btif_a2dp_source_update_metrics(void) {
  BtifMediaStats stats = btif_a2dp_source_cb.stats;
  SchedulingStats enqueue_stats = stats.tx_queue_enqueue_stats;
  A2dpSessionMetrics metrics;
  metrics.codec_index = stats.codec_index;
  metrics.is_a2dp_offload = btif_av_is_a2dp_offload_running();

  // session_start_us is 0 when btif_a2dp_source_start_audio_req() is not called
  // mark the metric duration as invalid (-1) in this case
  if (stats.session_start_us != 0) {
    metrics.audio_duration_ms = (stats.session_end_us - stats.session_start_us) / 1000;
  }

  if (enqueue_stats.total_updates > 1) {
    metrics.media_timer_min_ms = btif_a2dp_source_cb.encoder_interval_ms -
                                 (enqueue_stats.max_premature_scheduling_delta_us / 1000);
    metrics.media_timer_max_ms = btif_a2dp_source_cb.encoder_interval_ms +
                                 (enqueue_stats.max_overdue_scheduling_delta_us / 1000);

    metrics.total_scheduling_count = enqueue_stats.overdue_scheduling_count +
                                     enqueue_stats.premature_scheduling_count +
                                     enqueue_stats.exact_scheduling_count;
    if (metrics.total_scheduling_count > 0) {
      metrics.media_timer_avg_ms =
              enqueue_stats.total_scheduling_time_us / (1000 * metrics.total_scheduling_count);
    }

    metrics.buffer_overruns_max_count = stats.tx_queue_max_dropped_messages;
    metrics.buffer_overruns_total = stats.tx_queue_total_dropped_messages;
    metrics.buffer_underruns_count = stats.media_read_total_underflow_count;
    metrics.buffer_underruns_average = 0;
    if (metrics.buffer_underruns_count > 0) {
      metrics.buffer_underruns_average =
              (float)stats.media_read_total_underflow_bytes / metrics.buffer_underruns_count;
    }
  }
  BluetoothMetricsLogger::GetInstance()->LogA2dpSession(metrics);

  if (metrics.audio_duration_ms != -1) {
    log_a2dp_session_metrics_event(btif_av_source_active_peer(), metrics.audio_duration_ms,
                                   metrics.media_timer_min_ms, metrics.media_timer_max_ms,
                                   metrics.media_timer_avg_ms, metrics.total_scheduling_count,
                                   metrics.buffer_overruns_max_count, metrics.buffer_overruns_total,
                                   metrics.buffer_underruns_average, metrics.buffer_underruns_count,
                                   metrics.codec_index, metrics.is_a2dp_offload);
  }
}

void btif_a2dp_source_set_dynamic_audio_buffer_size(uint8_t dynamic_audio_buffer_size) {
  btif_a2dp_source_dynamic_audio_buffer_size = dynamic_audio_buffer_size;
}

static void btm_read_rssi_cb(void* data) {
  if (data == nullptr) {
    log::error("Read RSSI request timed out");
    return;
  }

  tBTM_RSSI_RESULT* result = (tBTM_RSSI_RESULT*)data;
  if (result->status != tBTM_STATUS::BTM_SUCCESS) {
    log::error("unable to read remote RSSI (status {})", result->status);
    return;
  }

  log_read_rssi_result(result->rem_bda, bluetooth::common::kUnknownConnectionHandle,
                       result->hci_status, result->rssi);

  log::warn("device: {}, rssi: {}", result->rem_bda, result->rssi);
}

static void btm_read_failed_contact_counter_cb(void* data) {
  if (data == nullptr) {
    log::error("Read Failed Contact Counter request timed out");
    return;
  }

  tBTM_FAILED_CONTACT_COUNTER_RESULT* result = (tBTM_FAILED_CONTACT_COUNTER_RESULT*)data;
  if (result->status != tBTM_STATUS::BTM_SUCCESS) {
    log::error("unable to read Failed Contact Counter (status {})", result->status);
    return;
  }
  log_read_failed_contact_counter_result(result->rem_bda,
                                         bluetooth::common::kUnknownConnectionHandle,
                                         result->hci_status, result->failed_contact_counter);

  log::warn("device: {}, Failed Contact Counter: {}", result->rem_bda,
            result->failed_contact_counter);
}

static void btm_read_tx_power_cb(void* data) {
  if (data == nullptr) {
    log::error("Read Tx Power request timed out");
    return;
  }

  tBTM_TX_POWER_RESULT* result = (tBTM_TX_POWER_RESULT*)data;
  if (result->status != tBTM_STATUS::BTM_SUCCESS) {
    log::error("unable to read Tx Power (status {})", result->status);
    return;
  }
  log_read_tx_power_level_result(result->rem_bda, bluetooth::common::kUnknownConnectionHandle,
                                 result->hci_status, result->tx_power);

  log::warn("device: {}, Tx Power: {}", result->rem_bda, result->tx_power);
}
