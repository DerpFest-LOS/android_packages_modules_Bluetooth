/******************************************************************************
 *
 *  Copyright 2009-2016 Broadcom Corporation
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

#include "btif/include/btif_av.h"

#include <base/functional/bind.h>
#include <base/strings/stringprintf.h>
#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>
#include <frameworks/proto_logging/stats/enums/bluetooth/a2dp/enums.pb.h>
#include <frameworks/proto_logging/stats/enums/bluetooth/enums.pb.h>
#include <stdio.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <ios>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "audio_hal_interface/a2dp_encoding.h"
#include "bta/include/bta_api.h"
#include "bta/include/bta_api_data_types.h"
#include "bta/include/bta_av_api.h"
#include "btif/avrcp/avrcp_service.h"
#include "btif/include/btif_a2dp.h"
#include "btif/include/btif_a2dp_sink.h"
#include "btif/include/btif_a2dp_source.h"
#include "btif/include/btif_av_co.h"
#include "btif/include/btif_common.h"
#include "btif/include/btif_metrics_logging.h"
#include "btif/include/btif_profile_queue.h"
#include "btif/include/btif_rc.h"
#include "btif/include/btif_util.h"
#include "btif/include/stack_manager_t.h"
#include "btif_metrics_logging.h"
#include "common/state_machine.h"
#include "device/include/device_iot_conf_defs.h"
#include "device/include/device_iot_config.h"
#include "hardware/bluetooth.h"
#include "hardware/bt_av.h"
#include "include/hardware/bt_rc.h"
#include "os/logging/log_adapter.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/properties.h"
#include "stack/include/a2dp_codec_api.h"
#include "stack/include/avdt_api.h"
#include "stack/include/avrc_api.h"
#include "stack/include/avrc_defs.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/btm_ble_api.h"
#include "stack/include/btm_ble_api_types.h"
#include "stack/include/btm_log_history.h"
#include "stack/include/main_thread.h"
#include "types/raw_address.h"

#ifdef __ANDROID__
#include <android/sysprop/BluetoothProperties.sysprop.h>
#endif

using namespace bluetooth;

/*****************************************************************************
 *  Constants & Macros
 *****************************************************************************/
static const char kBtifAvSourceServiceName[] = "Advanced Audio Source";
static const char kBtifAvSinkServiceName[] = "Advanced Audio Sink";
static constexpr int kDefaultMaxConnectedAudioDevices = 1;
static constexpr tBTA_AV_HNDL kBtaHandleUnknown = 0;

namespace {
constexpr char kBtmLogHistoryTag[] = "A2DP";
}

static bool delay_reporting_enabled() {
  return !osi_property_get_bool("persist.bluetooth.disabledelayreports", false);
}

/*****************************************************************************
 *  Local type definitions
 *****************************************************************************/

typedef struct {
  int sample_rate;
  int channel_count;
  RawAddress peer_address;
} btif_av_sink_config_req_t;

typedef struct {
  bool use_latency_mode;
} btif_av_start_stream_req_t;

typedef struct {
  bool is_low_latency;
} btif_av_set_latency_req_t;

typedef struct {
  std::vector<btav_a2dp_codec_config_t> codec_preferences;
  std::promise<void> reconf_ready_promise;
} btif_av_reconfig_req_t;

/**
 * BTIF AV events
 */
typedef enum {
  /* Reuse BTA_AV_XXX_EVT - No need to redefine them here */
  BTIF_AV_CONNECT_REQ_EVT = BTA_AV_MAX_EVT,
  BTIF_AV_DISCONNECT_REQ_EVT,
  BTIF_AV_START_STREAM_REQ_EVT,
  BTIF_AV_STOP_STREAM_REQ_EVT,
  BTIF_AV_SUSPEND_STREAM_REQ_EVT,
  BTIF_AV_SINK_CONFIG_REQ_EVT,
  BTIF_AV_ACL_DISCONNECTED,
  BTIF_AV_OFFLOAD_START_REQ_EVT,
  BTIF_AV_AVRCP_OPEN_EVT,
  BTIF_AV_AVRCP_CLOSE_EVT,
  BTIF_AV_AVRCP_REMOTE_PLAY_EVT,
  BTIF_AV_SET_LATENCY_REQ_EVT,
  BTIF_AV_RECONFIGURE_REQ_EVT,
} btif_av_sm_event_t;

class BtifAvEvent {
public:
  BtifAvEvent(uint32_t event, const void* p_data, size_t data_length);
  BtifAvEvent(const BtifAvEvent& other);
  BtifAvEvent() = delete;
  ~BtifAvEvent();
  BtifAvEvent& operator=(const BtifAvEvent& other);

  uint32_t Event() const { return event_; }
  void* Data() const { return data_; }
  size_t DataLength() const { return data_length_; }
  std::string ToString() const;
  static std::string EventName(uint32_t event);

private:
  void DeepCopy(uint32_t event, const void* p_data, size_t data_length);
  void DeepFree();

  uint32_t event_;
  void* data_;
  size_t data_length_;
};

class BtifAvPeer;

// Should not need dedicated Suspend state as actual actions are no
// different than Open state. Suspend flags are needed however to prevent
// media task from trying to restart stream during remote Suspend or while
// we are in the process of a local Suspend.
class BtifAvStateMachine : public bluetooth::common::StateMachine {
public:
  enum {
    kStateIdle,     // AVDTP disconnected
    kStateOpening,  // Opening AVDTP connection
    kStateOpened,   // AVDTP is in OPEN state
    kStateStarted,  // A2DP stream started
    kStateClosing,  // Closing AVDTP connection
  };

  class StateIdle : public State {
  public:
    explicit StateIdle(BtifAvStateMachine& sm) : State(sm, kStateIdle), peer_(sm.Peer()) {}
    void OnEnter() override;
    void OnExit() override;
    bool ProcessEvent(uint32_t event, void* p_data) override;

  private:
    BtifAvPeer& peer_;
  };

  class StateOpening : public State {
  public:
    explicit StateOpening(BtifAvStateMachine& sm) : State(sm, kStateOpening), peer_(sm.Peer()) {}
    void OnEnter() override;
    void OnExit() override;
    bool ProcessEvent(uint32_t event, void* p_data) override;

  private:
    BtifAvPeer& peer_;
  };

  class StateOpened : public State {
  public:
    explicit StateOpened(BtifAvStateMachine& sm) : State(sm, kStateOpened), peer_(sm.Peer()) {}
    void OnEnter() override;
    void OnExit() override;
    bool ProcessEvent(uint32_t event, void* p_data) override;

  private:
    BtifAvPeer& peer_;
  };

  class StateStarted : public State {
  public:
    explicit StateStarted(BtifAvStateMachine& sm) : State(sm, kStateStarted), peer_(sm.Peer()) {}
    void OnEnter() override;
    void OnExit() override;
    bool ProcessEvent(uint32_t event, void* p_data) override;

  private:
    BtifAvPeer& peer_;
  };

  class StateClosing : public State {
  public:
    explicit StateClosing(BtifAvStateMachine& sm) : State(sm, kStateClosing), peer_(sm.Peer()) {}
    void OnEnter() override;
    void OnExit() override;
    bool ProcessEvent(uint32_t event, void* p_data) override;

  private:
    BtifAvPeer& peer_;
  };

  explicit BtifAvStateMachine(BtifAvPeer& btif_av_peer) : peer_(btif_av_peer) {
    state_idle_ = new StateIdle(*this);
    state_opening_ = new StateOpening(*this);
    state_opened_ = new StateOpened(*this);
    state_started_ = new StateStarted(*this);
    state_closing_ = new StateClosing(*this);

    AddState(state_idle_);
    AddState(state_opening_);
    AddState(state_opened_);
    AddState(state_started_);
    AddState(state_closing_);
    SetInitialState(state_idle_);
  }

  BtifAvPeer& Peer() { return peer_; }

private:
  BtifAvPeer& peer_;
  StateIdle* state_idle_;
  StateOpening* state_opening_;
  StateOpened* state_opened_;
  StateStarted* state_started_;
  StateClosing* state_closing_;
};

class BtifAvPeer {
public:
  enum {
    kFlagLocalSuspendPending = 0x1,
    kFlagRemoteSuspend = 0x2,
    kFlagPendingStart = 0x4,
    kFlagPendingStop = 0x8,
    kFlagPendingReconfigure = 0x10,
  };
  static constexpr uint64_t kTimeoutAvOpenOnRcMs = 2 * 1000;  // 2s

  BtifAvPeer(const RawAddress& peer_address, uint8_t peer_sep, tBTA_AV_HNDL bta_handle,
             uint8_t peer_id);
  ~BtifAvPeer();

  bt_status_t Init();
  void Cleanup();

  /**
   * Check whether the peer can be deleted.
   *
   * @return true if the pair can be deleted, otherwise false
   */
  bool CanBeDeleted() const;

  /**
   * Check whether the peer is the active one.
   *
   * @return true if this peer is the active one
   */
  bool IsActivePeer() const { return PeerAddress() == ActivePeerAddress(); }

  /**
   * Get the address of the active peer.
   *
   * @return the address of the active peer
   */
  const RawAddress& ActivePeerAddress() const;

  const RawAddress& PeerAddress() const { return peer_address_; }
  bool IsSource() const { return peer_sep_ == AVDT_TSEP_SRC; }
  bool IsSink() const { return peer_sep_ == AVDT_TSEP_SNK; }
  uint8_t PeerSep() const { return peer_sep_; }
  void SetSep(uint8_t sep_type) { peer_sep_ = sep_type; }
  /**
   * Get the local device's Service Class UUID
   *
   * @return the local device's Service Class UUID: UUID_SERVCLASS_AUDIO_SOURCE
   * or UUID_SERVCLASS_AUDIO_SINK
   */
  uint16_t LocalUuidServiceClass() const {
    return IsSink() ? UUID_SERVCLASS_AUDIO_SOURCE : UUID_SERVCLASS_AUDIO_SINK;
  }
  tBTA_AV_HNDL BtaHandle() const { return bta_handle_; }
  void SetBtaHandle(tBTA_AV_HNDL bta_handle) { bta_handle_ = bta_handle; }
  uint8_t PeerId() const { return peer_id_; }

  BtifAvStateMachine& StateMachine() { return state_machine_; }
  const BtifAvStateMachine& StateMachine() const { return state_machine_; }
  alarm_t* AvOpenOnRcTimer() { return av_open_on_rc_timer_; }
  const alarm_t* AvOpenOnRcTimer() const { return av_open_on_rc_timer_; }

  void SetEdr(tBTA_AV_EDR edr) { edr_ = edr; }
  bool IsEdr() const { return edr_ != 0; }
  bool Is3Mbps() const { return (edr_ & BTA_AV_EDR_3MBPS) != 0; }

  bool IsConnected() const;
  bool IsStreaming() const;
  bool IsInSilenceMode() const { return is_silenced_; }

  void SetSilence(bool silence) { is_silenced_ = silence; }

  // AVDTP delay reporting in 1/10 milliseconds
  void SetDelayReport(uint16_t delay) { delay_report_ = delay; }
  uint16_t GetDelayReport() const { return delay_report_; }

  void SetMandatoryCodecPreferred(bool preferred) { mandatory_codec_preferred_ = preferred; }
  bool IsMandatoryCodecPreferred() const { return mandatory_codec_preferred_; }

  /**
   * Check whether any of the flags specified by the bitlags mask is set.
   *
   * @param bitflags_mask the bitflags to check
   * @return true if any of the flags to check is set, otherwise false.
   */
  bool CheckFlags(uint8_t bitflags_mask) const { return (flags_ & bitflags_mask) != 0; }

  /**
   * Set only the flags as specified by the bitflags mask.
   *
   * @param bitflags_mask the bitflags to set
   */
  void SetFlags(uint8_t bitflags_mask) { flags_ |= bitflags_mask; }

  /**
   * Clear only the flags as specified by the bitflags mask.
   *
   * @param bitflags_mask the bitflags to clear
   */
  void ClearFlags(uint8_t bitflags_mask) { flags_ &= ~bitflags_mask; }

  /**
   * Clear all flags.
   */
  void ClearAllFlags() { flags_ = 0; }

  /**
   * Get a string representation of the flags that are set.
   */
  std::string FlagsToString() const;

  bool SelfInitiatedConnection() const { return self_initiated_connection_; }
  void SetSelfInitiatedConnection(bool v) { self_initiated_connection_ = v; }

  bool UseLatencyMode() const { return use_latency_mode_; }
  void SetUseLatencyMode(bool use_latency_mode) { use_latency_mode_ = use_latency_mode; }

  void SetReconfigureStreamData(btif_av_reconfig_req_t&& req) {
    reconfig_req_ = std::make_optional<btif_av_reconfig_req_t>(std::move(req));
  }

  std::optional<btif_av_reconfig_req_t> GetReconfigureStreamData() {
    std::optional<btif_av_reconfig_req_t> data = std::move(reconfig_req_);
    reconfig_req_ = std::nullopt;
    return data;
  }

private:
  const RawAddress peer_address_;
  uint8_t peer_sep_;  // SEP type of peer device
  tBTA_AV_HNDL bta_handle_;
  const uint8_t peer_id_;
  BtifAvStateMachine state_machine_;
  alarm_t* av_open_on_rc_timer_;
  tBTA_AV_EDR edr_;
  uint8_t flags_;
  bool self_initiated_connection_;
  bool is_silenced_;
  uint16_t delay_report_;
  bool mandatory_codec_preferred_ = false;
  bool use_latency_mode_ = false;
  std::optional<btif_av_reconfig_req_t> reconfig_req_;
};

class BtifAvSource {
public:
  // The PeerId is used as AppId for BTA_AvRegister() purpose
  static constexpr uint8_t kPeerIdMin = 0;
  static constexpr uint8_t kPeerIdMax = BTA_AV_NUM_STRS;

  BtifAvSource()
      : callbacks_(nullptr),
        enabled_(false),
        a2dp_offload_enabled_(false),
        max_connected_peers_(kDefaultMaxConnectedAudioDevices) {}
  ~BtifAvSource();

  void Init(btav_source_callbacks_t* callbacks, int max_connected_audio_devices,
            const std::vector<btav_a2dp_codec_config_t>& codec_priorities,
            const std::vector<btav_a2dp_codec_config_t>& offloading_preference,
            std::vector<btav_a2dp_codec_info_t>* supported_codecs,
            std::promise<bt_status_t> complete_promise);
  void Cleanup();

  btav_source_callbacks_t* Callbacks() { return callbacks_; }
  bool Enabled() const { return enabled_; }
  bool A2dpOffloadEnabled() const { return a2dp_offload_enabled_; }
  BtifAvPeer* FindPeer(const RawAddress& peer_address);
  BtifAvPeer* FindPeerByHandle(tBTA_AV_HNDL bta_handle);
  BtifAvPeer* FindPeerByPeerId(uint8_t peer_id);
  BtifAvPeer* FindOrCreatePeer(const RawAddress& peer_address, tBTA_AV_HNDL bta_handle);

  /**
   * Check whether a connection to a peer is allowed.
   * The check considers the maximum number of connected peers.
   *
   * @param peer_address the peer address to connect to
   * @return true if connection is allowed, otherwise false
   */
  bool AllowedToConnect(const RawAddress& peer_address) const;

  /**
   * Delete all peers that have transitioned to Idle state and can be deleted.
   * If a peer was just created/initialized, then it cannot be deleted yet.
   */
  void DeleteIdlePeers();

  /**
   * Get the active peer.
   *
   * @return the active peer
   */
  const RawAddress& ActivePeer() const { return active_peer_; }

  /**
   * Check whether peer is silenced
   *
   * @param peer_address the peer to check
   * @return true on silence mode enabled, otherwise false
   */
  bool IsPeerSilenced(const RawAddress& peer_address) {
    if (peer_address.IsEmpty()) {
      return false;
    }
    BtifAvPeer* peer = FindPeer(peer_address);
    if (peer == nullptr) {
      log::warn("peer is null");
      return false;
    }
    if (!peer->IsConnected()) {
      log::warn("peer is not connected");
      return false;
    }
    return peer->IsInSilenceMode();
  }

  /**
   * Set peer silence mode
   *
   * @param peer_address the peer to set
   * @param silence true on enable silence mode, false on disable
   * @return true on success, otherwise false
   */
  bool SetSilencePeer(const RawAddress& peer_address, const bool silence) {
    if (peer_address.IsEmpty()) {
      return false;
    }
    log::info("peer: {}", peer_address);
    BtifAvPeer* peer = FindPeer(peer_address);
    if (peer == nullptr) {
      log::warn("peer is null");
      return false;
    }
    if (!peer->IsConnected()) {
      log::warn("peer is not connected");
      return false;
    }
    peer->SetSilence(silence);
    return true;
  }

  /**
   * Set the active peer.
   *
   * @param peer_address the active peer address or RawAddress::kEmpty to
   * reset the active peer
   * @return true on success, otherwise false
   */
  bool SetActivePeer(const RawAddress& peer_address, std::promise<void> peer_ready_promise) {
    log::info("peer={} active_peer={}", peer_address, active_peer_);

    if (active_peer_ == peer_address) {
      peer_ready_promise.set_value();
      return true;  // Nothing has changed
    }
    if (peer_address.IsEmpty()) {
      log::info("peer address is empty, shutdown the Audio source");
      if (!bta_av_co_set_active_source_peer(peer_address)) {
        log::warn("unable to set active peer to empty in BtaAvCo");
      }

      btif_a2dp_source_end_session(active_peer_);
      std::promise<void> shutdown_complete_promise;
      std::future<void> shutdown_complete_future = shutdown_complete_promise.get_future();
      btif_a2dp_source_shutdown(std::move(shutdown_complete_promise));
      using namespace std::chrono_literals;
      if (shutdown_complete_future.wait_for(1s) == std::future_status::timeout) {
        log::error("Timed out waiting for A2DP source shutdown to complete.");
      }
      active_peer_ = peer_address;
      peer_ready_promise.set_value();
      return true;
    }

    BtifAvPeer* peer = FindPeer(peer_address);
    if (peer == nullptr || !peer->IsConnected()) {
      log::error("Error setting {} as active Source peer", peer_address);
      peer_ready_promise.set_value();
      return false;
    }

    if (!btif_a2dp_source_restart_session(active_peer_, peer_address,
                                          std::move(peer_ready_promise))) {
      // cannot set promise but need to be handled within restart_session
      return false;
    }
    active_peer_ = peer_address;
    return true;
  }

  /**
   * Update source codec configuration for a peer.
   *
   * @param peer_address the address of the peer to update
   * @param codec_preferences the updated codec preferences
   */
  void UpdateCodecConfig(const RawAddress& peer_address,
                         const std::vector<btav_a2dp_codec_config_t>& codec_preferences,
                         std::promise<void> peer_ready_promise) {
    // Restart the session if the codec for the active peer is updated
    if (!peer_address.IsEmpty() && active_peer_ == peer_address) {
      btif_a2dp_source_end_session(active_peer_);
    }

    btif_a2dp_source_encoder_user_config_update_req(peer_address, codec_preferences,
                                                    std::move(peer_ready_promise));
  }

  /**
   * Get number of saved peers.
   */
  int GetPeersCount() const {
    std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
    return peers_.size();
  }

  /**
   * Dispatch SUSPEND or STOP A2DP stream event on all peer devices.
   * Returns true if succeeded.
   * @param event SUSPEND or STOP event
   */
  void DispatchSuspendStreamEvent(btif_av_sm_event_t event);

  /**
   * Set peer reconfigure stream data.
   *
   * @param peer_address the peer address to reconfigure stream
   * @param codec_preferences codec preferences for stream reconfiguration
   * @param reconf_ready_promise promise fulfilled when the reconfiguration done
   */
  bt_status_t SetPeerReconfigureStreamData(const RawAddress& peer_address,
                                           const std::vector<btav_a2dp_codec_config_t>& codec_preferences,
                                           std::promise<void> reconf_ready_promise) {
    std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);

    BtifAvPeer* peer = FindPeer(peer_address);
    if (peer == nullptr) {
      log::error("Can not find peer: {}", peer_address.ToString());
      return BT_STATUS_NOT_READY;
    }

    btif_av_reconfig_req_t reconf_stream_req = {
            .codec_preferences = codec_preferences,
            .reconf_ready_promise = std::move(reconf_ready_promise),
    };

    peer->SetReconfigureStreamData(std::move(reconf_stream_req));
    return BT_STATUS_SUCCESS;
  }

  void DumpPeersInfo(int fd);
  void RegisterAllBtaHandles();
  void DeregisterAllBtaHandles();
  void BtaHandleRegistered(uint8_t peer_id, tBTA_AV_HNDL bta_handle);
  BtifAvPeer* popPeer(const RawAddress& peer_address);
  void AddPeer(BtifAvPeer* peer);

private:
  void CleanupAllPeers();

  btav_source_callbacks_t* callbacks_;
  bool enabled_;
  bool a2dp_offload_enabled_;
  int max_connected_peers_;
  std::map<RawAddress, BtifAvPeer*> peers_;
  std::set<RawAddress> silenced_peers_;
  RawAddress active_peer_;
  std::map<uint8_t, tBTA_AV_HNDL> peer_id2bta_handle_;
  // protect for BtifAvSource::peers_
  mutable std::recursive_mutex btifavsource_peers_lock_;
};

class BtifAvSink {
public:
  // The PeerId is used as AppId for BTA_AvRegister() purpose
  static constexpr uint8_t kPeerIdMin = 0;
  static constexpr uint8_t kPeerIdMax = BTA_AV_NUM_STRS;

  BtifAvSink()
      : callbacks_(nullptr),
        enabled_(false),
        max_connected_peers_(kDefaultMaxConnectedAudioDevices) {}
  ~BtifAvSink();

  void Init(btav_sink_callbacks_t* callbacks, int max_connected_audio_devices,
            std::promise<bt_status_t> complete_promise);
  void Cleanup();

  btav_sink_callbacks_t* Callbacks() { return callbacks_; }
  bool Enabled() const { return enabled_; }

  BtifAvPeer* FindPeer(const RawAddress& peer_address);
  BtifAvPeer* FindPeerByHandle(tBTA_AV_HNDL bta_handle);
  BtifAvPeer* FindPeerByPeerId(uint8_t peer_id);
  BtifAvPeer* FindOrCreatePeer(const RawAddress& peer_address, tBTA_AV_HNDL bta_handle);

  /**
   * Check whether a connection to a peer is allowed.
   * The check considers the maximum number of connected peers.
   *
   * @param peer_address the peer address to connect to
   * @return true if connection is allowed, otherwise false
   */
  bool AllowedToConnect(const RawAddress& peer_address) const;

  /**
   * Delete all peers that have transitioned to Idle state and can be deleted.
   * If a peer was just created/initialized, then it cannot be deleted yet.
   */
  void DeleteIdlePeers();

  /**
   * Get the active peer.
   *
   * @return the active peer
   */
  const RawAddress& ActivePeer() const { return active_peer_; }

  /**
   * Set the active peer.
   *
   * @param peer_address the active peer address or RawAddress::kEmpty to
   * reset the active peer
   * @return true on success, otherwise false
   */
  bool SetActivePeer(const RawAddress& peer_address, std::promise<void> peer_ready_promise) {
    log::info("peer={} active_peer={}", peer_address, active_peer_);

    if (active_peer_ == peer_address) {
      peer_ready_promise.set_value();
      return true;  // Nothing has changed
    }
    if (peer_address.IsEmpty()) {
      log::verbose("peer address is empty, shutdown the Audio sink");
      if (!bta_av_co_set_active_sink_peer(peer_address)) {
        log::warn("unable to set active peer to empty in BtaAvCo");
      }

      btif_a2dp_sink_end_session(active_peer_);
      btif_a2dp_sink_shutdown();
      active_peer_ = peer_address;
      peer_ready_promise.set_value();
      return true;
    }

    BtifAvPeer* peer = FindPeer(peer_address);
    if (peer == nullptr || !peer->IsConnected()) {
      log::error("Error setting {} as active Sink peer", peer_address);
      peer_ready_promise.set_value();
      return false;
    }

    if (!btif_a2dp_sink_restart_session(active_peer_, peer_address,
                                        std::move(peer_ready_promise))) {
      // cannot set promise but need to be handled within restart_session
      return false;
    }
    log::info("Setting the active peer to peer address {}", peer_address);
    active_peer_ = peer_address;
    return true;
  }

  /**
   * Get number of saved peers.
   */
  int GetPeersCount() const {
    std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
    return peers_.size();
  }

  void DumpPeersInfo(int fd);
  void RegisterAllBtaHandles();
  void DeregisterAllBtaHandles();
  void BtaHandleRegistered(uint8_t peer_id, tBTA_AV_HNDL bta_handle);
  BtifAvPeer* popPeer(const RawAddress& peer_address);
  void AddPeer(BtifAvPeer* peer);

private:
  void CleanupAllPeers();

  btav_sink_callbacks_t* callbacks_;
  bool enabled_;
  int max_connected_peers_;
  std::map<RawAddress, BtifAvPeer*> peers_;
  RawAddress active_peer_;
  std::map<uint8_t, tBTA_AV_HNDL> peer_id2bta_handle_;
  // protect for BtifAvSink::peers_
  mutable std::recursive_mutex btifavsink_peers_lock_;
};

/*****************************************************************************
 *  Static variables
 *****************************************************************************/
static BtifAvSource btif_av_source;
static BtifAvSink btif_av_sink;

/* Helper macro to avoid code duplication in the state machine handlers */
#define CHECK_RC_EVENT(e, d)       \
  case BTA_AV_RC_OPEN_EVT:         \
  case BTA_AV_RC_BROWSE_OPEN_EVT:  \
  case BTA_AV_RC_CLOSE_EVT:        \
  case BTA_AV_RC_BROWSE_CLOSE_EVT: \
  case BTA_AV_REMOTE_CMD_EVT:      \
  case BTA_AV_VENDOR_CMD_EVT:      \
  case BTA_AV_META_MSG_EVT:        \
  case BTA_AV_RC_FEAT_EVT:         \
  case BTA_AV_RC_PSM_EVT:          \
  case BTA_AV_REMOTE_RSP_EVT: {    \
    btif_rc_handler(e, d);         \
  } break;

static void btif_av_source_dispatch_sm_event(const RawAddress& peer_address,
                                             btif_av_sm_event_t event);
static void btif_av_sink_dispatch_sm_event(const RawAddress& peer_address,
                                           btif_av_sm_event_t event);
static void btif_av_handle_event(uint8_t peer_sep, const RawAddress& peer_address,
                                 tBTA_AV_HNDL bta_handle, const BtifAvEvent& btif_av_event);
static void btif_debug_av_peer_dump(int fd, const BtifAvPeer& peer);
static void btif_report_connection_state(const RawAddress& peer_address,
                                         btav_connection_state_t state, const bt_status_t status,
                                         uint8_t error_code, const A2dpType local_a2dp_type);
static void btif_report_audio_state(const RawAddress& peer_address, btav_audio_state_t state,
                                    const A2dpType local_a2dp_type);
static void btif_av_report_sink_audio_config_state(const RawAddress& peer_address, int sample_rate,
                                                   int channel_count);
static void btif_av_query_mandatory_codec_priority(const RawAddress& peer_address);
static void btif_av_source_initiate_av_open_timer_timeout(void* data);
static void btif_av_sink_initiate_av_open_timer_timeout(void* data);
static void bta_av_sink_media_callback(const RawAddress& peer_address, tBTA_AV_EVT event,
                                       tBTA_AV_MEDIA* p_data);

static BtifAvPeer* btif_av_source_find_peer(const RawAddress& peer_address) {
  return btif_av_source.FindPeer(peer_address);
}

static BtifAvPeer* btif_av_sink_find_peer(const RawAddress& peer_address) {
  return btif_av_sink.FindPeer(peer_address);
}

static BtifAvPeer* btif_av_find_peer(const RawAddress& peer_address,
                                     const A2dpType local_a2dp_type) {
  if (btif_av_source.Enabled() && local_a2dp_type == A2dpType::kSource) {
    BtifAvPeer* sourcePeer = btif_av_source_find_peer(peer_address);
    if (sourcePeer != nullptr) {
      return sourcePeer;
    }
  }
  if (btif_av_sink.Enabled() && local_a2dp_type == A2dpType::kSink) {
    BtifAvPeer* sinkPeer = btif_av_sink_find_peer(peer_address);
    if (sinkPeer != nullptr) {
      return sinkPeer;
    }
  }
  if (btif_av_source.Enabled()) {
    BtifAvPeer* sourcePeer = btif_av_source_find_peer(peer_address);
    if (sourcePeer != nullptr) {
      return sourcePeer;
    }
  }
  if (btif_av_sink.Enabled()) {
    BtifAvPeer* sinkPeer = btif_av_sink_find_peer(peer_address);
    if (sinkPeer != nullptr) {
      return sinkPeer;
    }
  }
  log::info("Unable to find the peer {}", peer_address);
  return nullptr;
}

static BtifAvPeer* btif_av_find_active_peer(const A2dpType local_a2dp_type) {
  if (btif_av_source.Enabled() && local_a2dp_type == A2dpType::kSource) {
    return btif_av_source_find_peer(btif_av_source.ActivePeer());
  }
  if (btif_av_sink.Enabled() && local_a2dp_type == A2dpType::kSink) {
    return btif_av_sink_find_peer(btif_av_sink.ActivePeer());
  }
  return nullptr;
}

const RawAddress& btif_av_find_by_handle(tBTA_AV_HNDL bta_handle) {
  BtifAvPeer* peer = nullptr;
  if (btif_av_both_enable()) {
    peer = btif_av_source.FindPeerByHandle(bta_handle);
    if (peer == nullptr) {
      peer = btif_av_sink.FindPeerByHandle(bta_handle);
    }
    if (peer == nullptr) {
      return RawAddress::kEmpty;
    }

    return peer->PeerAddress();
  }
  if (btif_av_source.Enabled()) {
    peer = btif_av_source.FindPeerByHandle(bta_handle);
  }
  if (btif_av_sink.Enabled()) {
    peer = btif_av_sink.FindPeerByHandle(bta_handle);
  }

  if (peer == nullptr) {
    return RawAddress::kEmpty;
  }

  return peer->PeerAddress();
}

/*****************************************************************************
 * Local helper functions
 *****************************************************************************/

static const char* dump_av_sm_event_name(btif_av_sm_event_t event) {
  switch (static_cast<int>(event)) {
    CASE_RETURN_STR(BTA_AV_ENABLE_EVT)
    CASE_RETURN_STR(BTA_AV_REGISTER_EVT)
    CASE_RETURN_STR(BTA_AV_OPEN_EVT)
    CASE_RETURN_STR(BTA_AV_CLOSE_EVT)
    CASE_RETURN_STR(BTA_AV_START_EVT)
    CASE_RETURN_STR(BTA_AV_STOP_EVT)
    CASE_RETURN_STR(BTA_AV_PROTECT_REQ_EVT)
    CASE_RETURN_STR(BTA_AV_PROTECT_RSP_EVT)
    CASE_RETURN_STR(BTA_AV_RC_OPEN_EVT)
    CASE_RETURN_STR(BTA_AV_RC_CLOSE_EVT)
    CASE_RETURN_STR(BTA_AV_RC_BROWSE_OPEN_EVT)
    CASE_RETURN_STR(BTA_AV_RC_BROWSE_CLOSE_EVT)
    CASE_RETURN_STR(BTA_AV_REMOTE_CMD_EVT)
    CASE_RETURN_STR(BTA_AV_REMOTE_RSP_EVT)
    CASE_RETURN_STR(BTA_AV_VENDOR_CMD_EVT)
    CASE_RETURN_STR(BTA_AV_VENDOR_RSP_EVT)
    CASE_RETURN_STR(BTA_AV_RECONFIG_EVT)
    CASE_RETURN_STR(BTA_AV_SUSPEND_EVT)
    CASE_RETURN_STR(BTA_AV_PENDING_EVT)
    CASE_RETURN_STR(BTA_AV_META_MSG_EVT)
    CASE_RETURN_STR(BTA_AV_REJECT_EVT)
    CASE_RETURN_STR(BTA_AV_RC_FEAT_EVT)
    CASE_RETURN_STR(BTA_AV_RC_PSM_EVT)
    CASE_RETURN_STR(BTA_AV_OFFLOAD_START_RSP_EVT)
    CASE_RETURN_STR(BTIF_AV_CONNECT_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_DISCONNECT_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_START_STREAM_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_STOP_STREAM_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_SUSPEND_STREAM_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_SINK_CONFIG_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_ACL_DISCONNECTED)
    CASE_RETURN_STR(BTIF_AV_OFFLOAD_START_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_AVRCP_OPEN_EVT)
    CASE_RETURN_STR(BTIF_AV_AVRCP_CLOSE_EVT)
    CASE_RETURN_STR(BTIF_AV_AVRCP_REMOTE_PLAY_EVT)
    CASE_RETURN_STR(BTIF_AV_SET_LATENCY_REQ_EVT)
    CASE_RETURN_STR(BTIF_AV_RECONFIGURE_REQ_EVT)
    default:
      return "UNKNOWN_EVENT";
  }
}

BtifAvEvent::BtifAvEvent(uint32_t event, const void* p_data, size_t data_length)
    : event_(event), data_(nullptr), data_length_(0) {
  DeepCopy(event, p_data, data_length);
}

BtifAvEvent::BtifAvEvent(const BtifAvEvent& other) : event_(0), data_(nullptr), data_length_(0) {
  *this = other;
}

BtifAvEvent& BtifAvEvent::operator=(const BtifAvEvent& other) {
  DeepFree();
  DeepCopy(other.Event(), other.Data(), other.DataLength());
  return *this;
}

BtifAvEvent::~BtifAvEvent() { DeepFree(); }

std::string BtifAvEvent::ToString() const { return BtifAvEvent::EventName(event_); }

std::string BtifAvEvent::EventName(uint32_t event) {
  std::string name = dump_av_sm_event_name(static_cast<btif_av_sm_event_t>(event));
  std::stringstream ss_value;
  ss_value << "(0x" << std::hex << event << ")";
  return name + ss_value.str();
}

void BtifAvEvent::DeepCopy(uint32_t event, const void* p_data, size_t data_length) {
  event_ = event;
  data_length_ = data_length;
  if (data_length == 0) {
    data_ = nullptr;
  } else {
    data_ = osi_malloc(data_length_);
    memcpy(data_, p_data, data_length);
  }

  switch (event) {
    case BTA_AV_META_MSG_EVT: {
      log::assert_that(data_length >= sizeof(tBTA_AV),
                       "assert failed: data_length >= sizeof(tBTA_AV)");
      const tBTA_AV* av_src = (const tBTA_AV*)p_data;
      tBTA_AV* av_dest = reinterpret_cast<tBTA_AV*>(data_);
      if (av_src->meta_msg.p_data && av_src->meta_msg.len) {
        av_dest->meta_msg.p_data = reinterpret_cast<uint8_t*>(osi_calloc(av_src->meta_msg.len));
        memcpy(av_dest->meta_msg.p_data, av_src->meta_msg.p_data, av_src->meta_msg.len);
      }

      if (av_src->meta_msg.p_msg) {
        av_dest->meta_msg.p_msg = reinterpret_cast<tAVRC_MSG*>(osi_calloc(sizeof(tAVRC_MSG)));
        memcpy(av_dest->meta_msg.p_msg, av_src->meta_msg.p_msg, sizeof(tAVRC_MSG));

        tAVRC_MSG* p_msg_src = av_src->meta_msg.p_msg;
        tAVRC_MSG* p_msg_dest = av_dest->meta_msg.p_msg;

        if ((p_msg_src->hdr.opcode == AVRC_OP_VENDOR) &&
            (p_msg_src->vendor.p_vendor_data && p_msg_src->vendor.vendor_len)) {
          p_msg_dest->vendor.p_vendor_data =
                  reinterpret_cast<uint8_t*>(osi_calloc(p_msg_src->vendor.vendor_len));
          memcpy(p_msg_dest->vendor.p_vendor_data, p_msg_src->vendor.p_vendor_data,
                 p_msg_src->vendor.vendor_len);
        }
        if ((p_msg_src->hdr.opcode == AVRC_OP_BROWSE) && p_msg_src->browse.p_browse_data &&
            p_msg_src->browse.browse_len) {
          p_msg_dest->browse.p_browse_data =
                  reinterpret_cast<uint8_t*>(osi_calloc(p_msg_src->browse.browse_len));
          memcpy(p_msg_dest->browse.p_browse_data, p_msg_src->browse.p_browse_data,
                 p_msg_src->browse.browse_len);
        }
      }
    } break;

    default:
      break;
  }
}

void BtifAvEvent::DeepFree() {
  switch (event_) {
    case BTA_AV_META_MSG_EVT: {
      tBTA_AV* av = reinterpret_cast<tBTA_AV*>(data_);
      osi_free(av->meta_msg.p_data);
      av->meta_msg.p_data = nullptr;

      if (av->meta_msg.p_msg) {
        if (av->meta_msg.p_msg->hdr.opcode == AVRC_OP_VENDOR) {
          osi_free(av->meta_msg.p_msg->vendor.p_vendor_data);
        }
        if (av->meta_msg.p_msg->hdr.opcode == AVRC_OP_BROWSE) {
          osi_free(av->meta_msg.p_msg->browse.p_browse_data);
        }
        osi_free(av->meta_msg.p_msg);
        av->meta_msg.p_msg = nullptr;
      }
    } break;

    default:
      break;
  }

  osi_free(data_);
  data_ = nullptr;
  data_length_ = 0;
}

BtifAvPeer::BtifAvPeer(const RawAddress& peer_address, uint8_t peer_sep, tBTA_AV_HNDL bta_handle,
                       uint8_t peer_id)
    : peer_address_(peer_address),
      peer_sep_(peer_sep),
      bta_handle_(bta_handle),
      peer_id_(peer_id),
      state_machine_(*this),
      av_open_on_rc_timer_(nullptr),
      edr_(0),
      flags_(0),
      self_initiated_connection_(false),
      delay_report_(0) {}

BtifAvPeer::~BtifAvPeer() { alarm_free(av_open_on_rc_timer_); }

std::string BtifAvPeer::FlagsToString() const {
  std::string result;

  if (flags_ & BtifAvPeer::kFlagLocalSuspendPending) {
    if (!result.empty()) {
      result += "|";
    }
    result += "LOCAL_SUSPEND_PENDING";
  }
  if (flags_ & BtifAvPeer::kFlagRemoteSuspend) {
    if (!result.empty()) {
      result += "|";
    }
    result += "REMOTE_SUSPEND";
  }
  if (flags_ & BtifAvPeer::kFlagPendingStart) {
    if (!result.empty()) {
      result += "|";
    }
    result += "PENDING_START";
  }
  if (flags_ & BtifAvPeer::kFlagPendingStop) {
    if (!result.empty()) {
      result += "|";
    }
    result += "PENDING_STOP";
  }
  if (flags_ & BtifAvPeer::kFlagPendingReconfigure) {
    if (!result.empty()) {
      result += "|";
    }
    result += "PENDING_RECONFIGURE";
  }
  if (result.empty()) {
    result = "None";
  }

  return base::StringPrintf("0x%x(%s)", flags_, result.c_str());
}

bt_status_t BtifAvPeer::Init() {
  alarm_free(av_open_on_rc_timer_);
  av_open_on_rc_timer_ = alarm_new("btif_av_peer.av_open_on_rc_timer");
  is_silenced_ = false;

  state_machine_.Start();
  return BT_STATUS_SUCCESS;
}

void BtifAvPeer::Cleanup() {
  state_machine_.Quit();
  alarm_free(av_open_on_rc_timer_);
  av_open_on_rc_timer_ = nullptr;
}

bool BtifAvPeer::CanBeDeleted() const {
  return (state_machine_.StateId() == BtifAvStateMachine::kStateIdle) &&
         (state_machine_.PreviousStateId() != BtifAvStateMachine::kStateInvalid);
}

const RawAddress& BtifAvPeer::ActivePeerAddress() const {
  if (IsSource()) {
    return btif_av_sink.ActivePeer();
  }
  if (IsSink()) {
    return btif_av_source.ActivePeer();
  }

  log::fatal("A2DP peer {} is neither Source nor Sink", PeerAddress());
  return RawAddress::kEmpty;
}

bool BtifAvPeer::IsConnected() const {
  int state = state_machine_.StateId();
  return (state == BtifAvStateMachine::kStateOpened) ||
         (state == BtifAvStateMachine::kStateStarted);
}

bool BtifAvPeer::IsStreaming() const {
  int state = state_machine_.StateId();
  return state == BtifAvStateMachine::kStateStarted;
}

BtifAvSource::~BtifAvSource() { CleanupAllPeers(); }

void BtifAvSource::Init(btav_source_callbacks_t* callbacks, int max_connected_audio_devices,
                        const std::vector<btav_a2dp_codec_config_t>& codec_priorities,
                        const std::vector<btav_a2dp_codec_config_t>& offloading_preference,
                        std::vector<btav_a2dp_codec_info_t>* supported_codecs,
                        std::promise<bt_status_t> complete_promise) {
  log::info("max_connected_audio_devices={}", max_connected_audio_devices);
  Cleanup();
  CleanupAllPeers();
  max_connected_peers_ = max_connected_audio_devices;

  /* A2DP OFFLOAD */
  a2dp_offload_enabled_ = GetInterfaceToProfiles()->config->isA2DPOffloadEnabled();
  log::verbose("a2dp_offload.enable = {}", a2dp_offload_enabled_);

  callbacks_ = callbacks;
  if (a2dp_offload_enabled_) {
    tBTM_BLE_VSC_CB vsc_cb = {};
    BTM_BleGetVendorCapabilities(&vsc_cb);
    bool supports_a2dp_hw_offload_v2 =
            vsc_cb.version_supported >= 0x0104 && vsc_cb.a2dp_offload_v2_support;
    bluetooth::audio::a2dp::update_codec_offloading_capabilities(offloading_preference,
                                                                 supports_a2dp_hw_offload_v2);
  }
  bta_av_co_init(codec_priorities, supported_codecs);

  if (!btif_a2dp_source_init()) {
    complete_promise.set_value(BT_STATUS_FAIL);
    return;
  }
  enabled_ = true;
  btif_enable_service(BTA_A2DP_SOURCE_SERVICE_ID);
  complete_promise.set_value(BT_STATUS_SUCCESS);
}

void BtifAvSource::Cleanup() {
  log::info("");
  if (!enabled_) {
    return;
  }
  enabled_ = false;

  btif_queue_cleanup(UUID_SERVCLASS_AUDIO_SOURCE);

  std::promise<void> peer_ready_promise;
  btif_av_source.SetActivePeer(RawAddress::kEmpty, std::move(peer_ready_promise));
  btif_a2dp_source_cleanup();

  btif_disable_service(BTA_A2DP_SOURCE_SERVICE_ID);
  CleanupAllPeers();

  callbacks_ = nullptr;
}

BtifAvPeer* BtifAvSource::FindPeer(const RawAddress& peer_address) {
  auto it = peers_.find(peer_address);
  if (it != peers_.end()) {
    return it->second;
  }
  return nullptr;
}

BtifAvPeer* BtifAvSource::FindPeerByHandle(tBTA_AV_HNDL bta_handle) {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  for (auto it : peers_) {
    BtifAvPeer* peer = it.second;
    if (peer->BtaHandle() == bta_handle) {
      return peer;
    }
  }
  return nullptr;
}

BtifAvPeer* BtifAvSource::FindPeerByPeerId(uint8_t peer_id) {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  for (auto it : peers_) {
    BtifAvPeer* peer = it.second;
    if (peer->PeerId() == peer_id) {
      return peer;
    }
  }
  return nullptr;
}

BtifAvPeer* BtifAvSource::FindOrCreatePeer(const RawAddress& peer_address,
                                           tBTA_AV_HNDL bta_handle) {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  log::verbose("peer={} bta_handle=0x{:x}", peer_address, bta_handle);

  BtifAvPeer* peer = FindPeer(peer_address);
  if (peer != nullptr) {
    return peer;
  }

  // Find next availabie Peer ID to use
  uint8_t peer_id;
  for (peer_id = kPeerIdMin; peer_id < kPeerIdMax; peer_id++) {
    /* because the peer id may be in source cb and we cannot use it */
    if (btif_av_src_sink_coexist_enabled() && btif_av_both_enable()) {
      if (FindPeerByPeerId(peer_id) == nullptr &&
          btif_av_sink.FindPeerByPeerId(peer_id) == nullptr) {
        break;
      }
    } else {
      if (FindPeerByPeerId(peer_id) == nullptr) {
        break;
      }
    }
  }
  if (peer_id == kPeerIdMax) {
    log::error(
            "Cannot create peer for peer={} : cannot allocate unique Peer "
            "ID",
            peer_address);
    return nullptr;
  }

  // Get the BTA Handle (if known)
  if (bta_handle == kBtaHandleUnknown) {
    auto it = peer_id2bta_handle_.find(peer_id);
    if (it == peer_id2bta_handle_.end() || it->second == kBtaHandleUnknown) {
      log::error(
              "Cannot create peer for peer={} : cannot convert Peer ID={} "
              "to unique BTA Handle",
              peer_address, peer_id);
      return nullptr;
    }
    bta_handle = it->second;
  }

  log::info("Create peer: peer={} bta_handle=0x{:x} peer_id={}", peer_address, bta_handle, peer_id);
  peer = new BtifAvPeer(peer_address, AVDT_TSEP_SNK, bta_handle, peer_id);
  peers_.insert(std::make_pair(peer_address, peer));
  peer->Init();
  return peer;
}

bool BtifAvSource::AllowedToConnect(const RawAddress& peer_address) const {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  int connected = 0;

  // Count peers that are in the process of connecting or already connected
  for (auto it : peers_) {
    const BtifAvPeer* peer = it.second;
    switch (peer->StateMachine().StateId()) {
      case BtifAvStateMachine::kStateOpening:
      case BtifAvStateMachine::kStateOpened:
      case BtifAvStateMachine::kStateStarted:
        if (peer->PeerAddress() == peer_address) {
          return true;  // Already connected or accounted for
        }
        connected++;
        break;
      default:
        break;
    }
  }
  const int sink_connected_peers_size = btif_av_sink.GetPeersCount();
  log::info("connected={}, max_connected_peers_={}, sink_connected_peers_size={}", connected,
            max_connected_peers_, btif_av_sink.GetPeersCount());
  return (connected + sink_connected_peers_size) < max_connected_peers_;
}

void BtifAvSource::DumpPeersInfo(int fd) {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  for (auto it : peers_) {
    const BtifAvPeer* peer = it.second;
    if (peer != nullptr) {
      btif_debug_av_peer_dump(fd, *peer);
    }
  }
}

void BtifAvSource::DispatchSuspendStreamEvent(btif_av_sm_event_t event) {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  if (event != BTIF_AV_SUSPEND_STREAM_REQ_EVT && event != BTIF_AV_STOP_STREAM_REQ_EVT) {
    log::error("Invalid event: {} id: {}", dump_av_sm_event_name(event), static_cast<int>(event));
    return;
  }
  bool av_stream_idle = true;
  for (auto it : peers_) {
    const BtifAvPeer* peer = it.second;
    if (peer->StateMachine().StateId() == BtifAvStateMachine::kStateStarted) {
      btif_av_source_dispatch_sm_event(peer->PeerAddress(), event);
      av_stream_idle = false;
    }
  }

  if (av_stream_idle) {
    btif_a2dp_on_stopped(nullptr, A2dpType::kSource);
  }
}

void BtifAvSource::DeleteIdlePeers() {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  for (auto it = peers_.begin(); it != peers_.end();) {
    BtifAvPeer* peer = it->second;
    auto prev_it = it++;
    if (!peer->CanBeDeleted()) {
      continue;
    }
    log::info("peer={} bta_handle=0x{:x}", peer->PeerAddress(), peer->BtaHandle());
    peer->Cleanup();
    peers_.erase(prev_it);
    delete peer;
  }
}

void BtifAvSource::CleanupAllPeers() {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  log::info("");

  while (!peers_.empty()) {
    auto it = peers_.begin();
    BtifAvPeer* peer = it->second;
    peer->Cleanup();
    peers_.erase(it);
    delete peer;
  }
}

void BtifAvSource::RegisterAllBtaHandles() {
  for (int peer_id = kPeerIdMin; peer_id < kPeerIdMax; peer_id++) {
    BTA_AvRegister(BTA_AV_CHNL_AUDIO, kBtifAvSourceServiceName, peer_id, nullptr,
                   UUID_SERVCLASS_AUDIO_SOURCE);
  }
}

void BtifAvSource::DeregisterAllBtaHandles() {
  for (auto it : peer_id2bta_handle_) {
    tBTA_AV_HNDL bta_handle = it.second;
    BTA_AvDeregister(bta_handle);
  }
  peer_id2bta_handle_.clear();
}

void BtifAvSource::BtaHandleRegistered(uint8_t peer_id, tBTA_AV_HNDL bta_handle) {
  peer_id2bta_handle_.insert(std::make_pair(peer_id, bta_handle));

  // Set the BTA Handle for the Peer (if exists)
  BtifAvPeer* peer = FindPeerByPeerId(peer_id);
  if (peer != nullptr && peer->BtaHandle() != bta_handle) {
    if (peer->BtaHandle() == kBtaHandleUnknown) {
      log::verbose("Assign peer: peer={} bta_handle=0x{:x} peer_id={}", peer->PeerAddress(),
                   bta_handle, peer_id);
    } else {
      log::warn("Correct peer: peer={} bta_handle=0x{:x}->0x{:x} peer_id={}", peer->PeerAddress(),
                peer->BtaHandle(), bta_handle, peer_id);
    }
    peer->SetBtaHandle(bta_handle);
  }
}

BtifAvPeer* BtifAvSource::popPeer(const RawAddress& peer_address) {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  auto it = peers_.find(peer_address);
  if (it == peers_.end()) {
    return nullptr;
  }
  BtifAvPeer* peer = it->second;
  peers_.erase(it);
  log::info("peer={}, state={}", peer->PeerAddress(), peer->StateMachine().StateId());
  return peer;
}

void BtifAvSource::AddPeer(BtifAvPeer* peer) {
  std::lock_guard<std::recursive_mutex> lock(btifavsource_peers_lock_);
  log::info("peer={}, state={}", peer->PeerAddress(), peer->StateMachine().StateId());
  peers_.insert(std::make_pair(peer->PeerAddress(), peer));
}
BtifAvSink::~BtifAvSink() { CleanupAllPeers(); }

void BtifAvSink::Init(btav_sink_callbacks_t* callbacks, int max_connected_audio_devices,
                      std::promise<bt_status_t> complete_promise) {
  log::info("(max_connected_audio_devices={})", max_connected_audio_devices);
  Cleanup();
  CleanupAllPeers();
  max_connected_peers_ = max_connected_audio_devices;
  callbacks_ = callbacks;

  /** source will have this configuration, but sink don't have, so don't
   * overwrite it. */
  if (!btif_av_source.Enabled()) {
    std::vector<btav_a2dp_codec_config_t> codec_priorities;  // Default priorities
    std::vector<btav_a2dp_codec_info_t> supported_codecs;
    bta_av_co_init(codec_priorities, &supported_codecs);
  }

  if (!btif_a2dp_sink_init()) {
    complete_promise.set_value(BT_STATUS_FAIL);
    return;
  }
  enabled_ = true;
  btif_enable_service(BTA_A2DP_SINK_SERVICE_ID);
  complete_promise.set_value(BT_STATUS_SUCCESS);
}

void BtifAvSink::Cleanup() {
  log::info("");
  if (!enabled_) {
    return;
  }
  enabled_ = false;

  btif_queue_cleanup(UUID_SERVCLASS_AUDIO_SINK);

  std::promise<void> peer_ready_promise;
  btif_av_sink.SetActivePeer(RawAddress::kEmpty, std::move(peer_ready_promise));
  btif_a2dp_sink_cleanup();

  btif_disable_service(BTA_A2DP_SINK_SERVICE_ID);
  CleanupAllPeers();

  callbacks_ = nullptr;
}

BtifAvPeer* BtifAvSink::FindPeer(const RawAddress& peer_address) {
  auto it = peers_.find(peer_address);
  if (it != peers_.end()) {
    return it->second;
  }
  return nullptr;
}

BtifAvPeer* BtifAvSink::FindPeerByHandle(tBTA_AV_HNDL bta_handle) {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  for (auto it : peers_) {
    BtifAvPeer* peer = it.second;
    if (peer->BtaHandle() == bta_handle) {
      return peer;
    }
  }
  return nullptr;
}

BtifAvPeer* BtifAvSink::FindPeerByPeerId(uint8_t peer_id) {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  for (auto it : peers_) {
    BtifAvPeer* peer = it.second;
    if (peer->PeerId() == peer_id) {
      return peer;
    }
  }
  return nullptr;
}

BtifAvPeer* BtifAvSink::FindOrCreatePeer(const RawAddress& peer_address, tBTA_AV_HNDL bta_handle) {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  log::verbose("peer={} bta_handle=0x{:x}", peer_address, bta_handle);

  BtifAvPeer* peer = FindPeer(peer_address);
  if (peer != nullptr) {
    return peer;
  }

  // Find next availabie Peer ID to use
  uint8_t peer_id;
  for (peer_id = kPeerIdMin; peer_id < kPeerIdMax; peer_id++) {
    /* because the peer id may be in source cb and we cannot use it */
    if (btif_av_both_enable()) {
      if (FindPeerByPeerId(peer_id) == nullptr &&
          btif_av_source.FindPeerByPeerId(peer_id) == nullptr) {
        break;
      }

    } else {
      if (FindPeerByPeerId(peer_id) == nullptr) {
        break;
      }
    }
  }
  if (peer_id == kPeerIdMax) {
    log::error(
            "Cannot create peer for peer={} : cannot allocate unique Peer "
            "ID",
            peer_address);
    return nullptr;
  }

  // Get the BTA Handle (if known)
  if (bta_handle == kBtaHandleUnknown) {
    auto it = peer_id2bta_handle_.find(peer_id);
    if (it == peer_id2bta_handle_.end() || it->second == kBtaHandleUnknown) {
      log::error(
              "Cannot create peer for peer={} : cannot convert Peer ID={} "
              "to unique BTA Handle",
              peer_address, peer_id);
      return nullptr;
    }
    bta_handle = it->second;
  }

  log::info("Create peer: peer={} bta_handle=0x{:x} peer_id={}", peer_address, bta_handle, peer_id);
  peer = new BtifAvPeer(peer_address, AVDT_TSEP_SRC, bta_handle, peer_id);
  peers_.insert(std::make_pair(peer_address, peer));
  peer->Init();
  return peer;
}

void BtifAvSink::DumpPeersInfo(int fd) {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  for (auto it : peers_) {
    const BtifAvPeer* peer = it.second;
    if (peer != nullptr) {
      btif_debug_av_peer_dump(fd, *peer);
    }
  }
}

bool BtifAvSink::AllowedToConnect(const RawAddress& peer_address) const {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  int connected = 0;

  // Count peers that are in the process of connecting or already connected
  for (auto it : peers_) {
    const BtifAvPeer* peer = it.second;
    switch (peer->StateMachine().StateId()) {
      case BtifAvStateMachine::kStateOpening:
      case BtifAvStateMachine::kStateOpened:
      case BtifAvStateMachine::kStateStarted:
        if (peer->PeerAddress() == peer_address) {
          return true;  // Already connected or accounted for
        }
        connected++;
        break;
      case BtifAvStateMachine::kStateClosing:
      case BtifAvStateMachine::kStateIdle:
        if ((btif_a2dp_sink_get_audio_track() != nullptr) &&
            (peer->PeerAddress() != peer_address)) {
          log::info("there is another peer with audio track({}), another={}, peer={}",
                    std::format_ptr(btif_a2dp_sink_get_audio_track()), peer->PeerAddress(),
                    peer_address);
          connected++;
        }
        break;
      default:
        break;
    }
  }
  const int source_connected_peers_size = btif_av_source.GetPeersCount();
  log::info("connected={}, max_connected_peers_={}, source_connected_peers_size={}", connected,
            max_connected_peers_, source_connected_peers_size);
  return (connected + source_connected_peers_size) < max_connected_peers_;
}

void BtifAvSink::DeleteIdlePeers() {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  for (auto it = peers_.begin(); it != peers_.end();) {
    BtifAvPeer* peer = it->second;
    auto prev_it = it++;
    if (!peer->CanBeDeleted()) {
      continue;
    }
    log::info("Deleting idle peer: {} bta_handle=0x{:x}", peer->PeerAddress(), peer->BtaHandle());
    peer->Cleanup();
    peers_.erase(prev_it);
    delete peer;
  }
}

void BtifAvSink::CleanupAllPeers() {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  log::info("");

  while (!peers_.empty()) {
    auto it = peers_.begin();
    BtifAvPeer* peer = it->second;
    peer->Cleanup();
    peers_.erase(it);
    delete peer;
  }
}

void BtifAvSink::RegisterAllBtaHandles() {
  for (int peer_id = kPeerIdMin; peer_id < kPeerIdMax; peer_id++) {
    BTA_AvRegister(BTA_AV_CHNL_AUDIO, kBtifAvSinkServiceName, peer_id, bta_av_sink_media_callback,
                   UUID_SERVCLASS_AUDIO_SINK);
  }
}

void BtifAvSink::DeregisterAllBtaHandles() {
  for (auto it : peer_id2bta_handle_) {
    tBTA_AV_HNDL bta_handle = it.second;
    BTA_AvDeregister(bta_handle);
  }
  peer_id2bta_handle_.clear();
}

void BtifAvSink::BtaHandleRegistered(uint8_t peer_id, tBTA_AV_HNDL bta_handle) {
  peer_id2bta_handle_.insert(std::make_pair(peer_id, bta_handle));

  // Set the BTA Handle for the Peer (if exists)
  BtifAvPeer* peer = FindPeerByPeerId(peer_id);
  if (peer != nullptr && peer->BtaHandle() != bta_handle) {
    if (peer->BtaHandle() == kBtaHandleUnknown) {
      log::verbose("Assign peer: peer={} bta_handle=0x{:x} peer_id={}", peer->PeerAddress(),
                   bta_handle, peer_id);
    } else {
      log::warn("Correct peer: peer={} bta_handle=0x{:x}->0x{:x} peer_id={}", peer->PeerAddress(),
                peer->BtaHandle(), bta_handle, peer_id);
    }
    peer->SetBtaHandle(bta_handle);
  }
}

BtifAvPeer* BtifAvSink::popPeer(const RawAddress& peer_address) {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  auto it = peers_.find(peer_address);
  if (it == peers_.end()) {
    return nullptr;
  }
  BtifAvPeer* peer = it->second;
  peers_.erase(it);
  log::info("peer={}, state={}", peer->PeerAddress(), peer->StateMachine().StateId());
  return peer;
}

void BtifAvSink::AddPeer(BtifAvPeer* peer) {
  std::lock_guard<std::recursive_mutex> lock(btifavsink_peers_lock_);
  log::info("peer={}, state={}", peer->PeerAddress(), peer->StateMachine().StateId());
  peers_.insert(std::make_pair(peer->PeerAddress(), peer));
}

void BtifAvStateMachine::StateIdle::OnEnter() {
  log::info("state=Idle peer={}", peer_.PeerAddress());

  peer_.SetEdr(0);
  peer_.ClearAllFlags();

  // Stop A2DP if this is the active peer
  if (peer_.IsActivePeer() || peer_.ActivePeerAddress().IsEmpty()) {
    btif_a2dp_on_idle(peer_.PeerAddress(), peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
  }

  // Reset the active peer if this was the active peer and
  // the Idle state was reentered
  if (peer_.IsActivePeer() && peer_.CanBeDeleted()) {
    std::promise<void> peer_ready_promise;
    if (peer_.IsSink()) {
      btif_av_source.SetActivePeer(RawAddress::kEmpty, std::move(peer_ready_promise));
    } else if (peer_.IsSource()) {
      btif_av_sink.SetActivePeer(RawAddress::kEmpty, std::move(peer_ready_promise));
    }
  }

  // Delete peers that are re-entering the Idle state
  if (peer_.IsSink()) {
    do_in_main_thread(
            base::BindOnce(&BtifAvSource::DeleteIdlePeers, base::Unretained(&btif_av_source)));
  } else if (peer_.IsSource()) {
    do_in_main_thread(
            base::BindOnce(&BtifAvSink::DeleteIdlePeers, base::Unretained(&btif_av_sink)));
  }
}

void BtifAvStateMachine::StateIdle::OnExit() {
  log::info("state=Idle peer={}", peer_.PeerAddress());
}

bool BtifAvStateMachine::StateIdle::ProcessEvent(uint32_t event, void* p_data) {
  log::info("state=Idle peer={} event={} flags={} active_peer={}", peer_.PeerAddress(),
            BtifAvEvent::EventName(event), peer_.FlagsToString(), peer_.IsActivePeer());

  switch (event) {
    case BTA_AV_ENABLE_EVT:
      break;

    case BTIF_AV_STOP_STREAM_REQ_EVT:
    case BTIF_AV_SUSPEND_STREAM_REQ_EVT:
    case BTIF_AV_ACL_DISCONNECTED:
      // Ignore. Just re-enter Idle so the peer can be deleted
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      break;

    case BTIF_AV_DISCONNECT_REQ_EVT:
      if (peer_.BtaHandle() != kBtaHandleUnknown) {
        BTA_AvClose(peer_.BtaHandle());
        if (peer_.IsSource()) {
          BTA_AvCloseRc(peer_.BtaHandle());
        }
      }
      // Re-enter Idle so the peer can be deleted
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      break;

    case BTIF_AV_CONNECT_REQ_EVT:
    case BTA_AV_PENDING_EVT: {
      bool can_connect = true;
      peer_.SetSelfInitiatedConnection(event == BTIF_AV_CONNECT_REQ_EVT);
      // Check whether connection is allowed
      if (peer_.IsSink()) {
        can_connect = btif_av_source.AllowedToConnect(peer_.PeerAddress());
        if (!can_connect) {
          btif_av_source_disconnect(peer_.PeerAddress());
        }
      } else if (peer_.IsSource()) {
        can_connect = btif_av_sink.AllowedToConnect(peer_.PeerAddress());
        if (!can_connect) {
          btif_av_sink_disconnect(peer_.PeerAddress());
        }
      }
      if (!can_connect) {
        log::error("Cannot connect to peer {}: too many connected peers", peer_.PeerAddress());
        if (peer_.SelfInitiatedConnection()) {
          btif_queue_advance();
        }
        break;
      }
      btif_av_query_mandatory_codec_priority(peer_.PeerAddress());
      BTA_AvOpen(peer_.PeerAddress(), peer_.BtaHandle(), true, peer_.LocalUuidServiceClass());
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateOpening);
      if (event == BTIF_AV_CONNECT_REQ_EVT) {
        DEVICE_IOT_CONFIG_ADDR_SET_INT(
                peer_.PeerAddress(), IOT_CONF_KEY_A2DP_ROLE,
                (peer_.LocalUuidServiceClass() == UUID_SERVCLASS_AUDIO_SOURCE)
                        ? IOT_CONF_VAL_A2DP_ROLE_SINK
                        : IOT_CONF_VAL_A2DP_ROLE_SOURCE);
        DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(peer_.PeerAddress(), IOT_CONF_KEY_A2DP_CONN_COUNT);
      } else if (event == BTA_AV_PENDING_EVT) {
        DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(peer_.PeerAddress(), IOT_CONF_KEY_A2DP_CONN_COUNT);
      }
    } break;
    case BTIF_AV_AVRCP_OPEN_EVT:
    case BTA_AV_RC_OPEN_EVT: {
      // IOP_FIX: Jabra 620 only does AVRCP Open without AV Open whenever it
      // connects. So as per the AV WP, an AVRCP connection cannot exist
      // without an AV connection. Therefore, we initiate an AV connection
      // if an RC_OPEN_EVT is received when we are in AV_CLOSED state.
      // We initiate the AV connection after a small 3s timeout to avoid any
      // collisions from the headsets, as some headsets initiate the AVRCP
      // connection first and then immediately initiate the AV connection
      //
      // TODO: We may need to do this only on an AVRCP Play. FixMe
      log::warn("Peer {} : event={} received without AV", peer_.PeerAddress(),
                BtifAvEvent::EventName(event));

      bool can_connect = true;
      // Check whether connection is allowed
      if (peer_.IsSink()) {
        can_connect = btif_av_source.AllowedToConnect(peer_.PeerAddress());
        if (!can_connect) {
          log::error("Source profile doesn't allow connection to peer:{}", peer_.PeerAddress());
          if (btif_av_src_sink_coexist_enabled()) {
            BTA_AvCloseRc((reinterpret_cast<tBTA_AV*>(p_data))->rc_open.rc_handle);
          } else {
            btif_av_source_disconnect(peer_.PeerAddress());
          }
        }
      } else if (peer_.IsSource()) {
        can_connect = btif_av_sink.AllowedToConnect(peer_.PeerAddress());
        if (!can_connect) {
          log::error("Sink profile doesn't allow connection to peer:{}", peer_.PeerAddress());
          if (btif_av_src_sink_coexist_enabled()) {
            BTA_AvCloseRc((reinterpret_cast<tBTA_AV*>(p_data))->rc_open.rc_handle);
          } else {
            btif_av_sink_disconnect(peer_.PeerAddress());
          }
        }
      }
      if (!can_connect) {
        log::error("Cannot connect to peer {}: too many connected peers", peer_.PeerAddress());
        break;
      }
      /* if peer is source, then start timer for sink connect to src */
      if (btif_av_src_sink_coexist_enabled()) {
        if (peer_.IsSource()) {
          alarm_set_on_mloop(peer_.AvOpenOnRcTimer(), BtifAvPeer::kTimeoutAvOpenOnRcMs,
                             btif_av_sink_initiate_av_open_timer_timeout, &peer_);
        } else {
          alarm_set_on_mloop(peer_.AvOpenOnRcTimer(), BtifAvPeer::kTimeoutAvOpenOnRcMs,
                             btif_av_source_initiate_av_open_timer_timeout, &peer_);
        }
      } else {
        if (btif_av_source.Enabled()) {
          alarm_set_on_mloop(peer_.AvOpenOnRcTimer(), BtifAvPeer::kTimeoutAvOpenOnRcMs,
                             btif_av_source_initiate_av_open_timer_timeout, &peer_);
        } else if (btif_av_sink.Enabled()) {
          alarm_set_on_mloop(peer_.AvOpenOnRcTimer(), BtifAvPeer::kTimeoutAvOpenOnRcMs,
                             btif_av_sink_initiate_av_open_timer_timeout, &peer_);
        }
      }
      if (event == BTA_AV_RC_OPEN_EVT) {
        btif_rc_handler(event, reinterpret_cast<tBTA_AV*>(p_data));
      }
    } break;

    case BTA_AV_RC_BROWSE_OPEN_EVT:
      btif_rc_handler(event, reinterpret_cast<tBTA_AV*>(p_data));
      break;

    // In case Signalling channel is not down and remote started Streaming
    // Procedure, we have to handle Config and Open event in Idle state.
    // We hit these scenarios while running PTS test case for AVRCP Controller.
    case BTIF_AV_SINK_CONFIG_REQ_EVT: {
      const btif_av_sink_config_req_t* p_config_req =
              static_cast<const btif_av_sink_config_req_t*>(p_data);
      btif_av_report_sink_audio_config_state(p_config_req->peer_address, p_config_req->sample_rate,
                                             p_config_req->channel_count);
    } break;

    case BTA_AV_OPEN_EVT: {
      tBTA_AV* p_bta_data = reinterpret_cast<tBTA_AV*>(p_data);
      tBTA_AV_STATUS status = p_bta_data->open.status;
      bool can_connect = true;

      log::info("Peer {} : event={} flags={} status={}({}) edr=0x{:x}", peer_.PeerAddress(),
                BtifAvEvent::EventName(event), peer_.FlagsToString(), status,
                (status == BTA_AV_SUCCESS) ? "SUCCESS" : "FAILED", p_bta_data->open.edr);

      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_CONNECTING,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);

      if (p_bta_data->open.status == BTA_AV_SUCCESS) {
        peer_.SetEdr(p_bta_data->open.edr);
        if (btif_av_src_sink_coexist_enabled()) {
          log::verbose("Peer {} sep={}, open_sep={}", peer_.PeerAddress(), peer_.PeerSep(),
                       p_bta_data->open.sep);
          /* if peer is wrong sep type, move it to BtifAvSxxx */
          if (peer_.PeerSep() != p_bta_data->open.sep) {
            BtifAvPeer* tmp_peer = nullptr;
            if (peer_.PeerSep() == AVDT_TSEP_SNK) {
              tmp_peer = btif_av_source.popPeer(peer_.PeerAddress());

              if (peer_.PeerAddress() != tmp_peer->PeerAddress()) {
                log::error("error, not same peer");
              }

              btif_av_sink.AddPeer(tmp_peer);
            } else {
              tmp_peer = btif_av_sink.popPeer(peer_.PeerAddress());

              if (peer_.PeerAddress() != tmp_peer->PeerAddress()) {
                log::error("error, not same peer");
              }

              btif_av_source.AddPeer(tmp_peer);
            }
            peer_.SetSep(p_bta_data->open.sep);
          }
          if (btif_rc_is_connected_peer(peer_.PeerAddress())) {
            log::verbose("AVRCP connected, update avrc sep");
            BTA_AvSetPeerSep(peer_.PeerAddress(), peer_.PeerSep());
          }
          btif_rc_check_pending_cmd(p_bta_data->open.bd_addr);
        }
        log::assert_that(peer_.PeerSep() == p_bta_data->open.sep,
                         "assert failed: peer_.PeerSep() == p_bta_data->open.sep");

        can_connect = peer_.IsSink() ? btif_av_source.AllowedToConnect(peer_.PeerAddress())
                                     : btif_av_sink.AllowedToConnect(peer_.PeerAddress());

        if (!can_connect) {
          log::error("Cannot connect to peer {}: too many connected peers", peer_.PeerAddress());

          if (peer_.IsSink()) {
            btif_av_source_disconnect(peer_.PeerAddress());
          } else if (peer_.IsSource()) {
            btif_av_sink_disconnect(peer_.PeerAddress());
          }

          btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                       bt_status_t::BT_STATUS_NOMEM, BTA_AV_FAIL_RESOURCES,
                                       peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
          peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
        } else {
          if (peer_.IsSink()) {
            // If queued PLAY command, send it now
            btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr,
                                              (p_bta_data->open.status == BTA_AV_SUCCESS));
          } else if (peer_.IsSource() && (p_bta_data->open.status == BTA_AV_SUCCESS)) {
            // Bring up AVRCP connection as well
            BTA_AvOpenRc(peer_.BtaHandle());
          }
          btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_CONNECTED,
                                       bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                       peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
          peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateOpened);
        }
      } else {
        btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                     bt_status_t::BT_STATUS_FAIL, status,
                                     peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
        peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
        DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(peer_.PeerAddress(), IOT_CONF_KEY_A2DP_CONN_FAIL_COUNT);
      }
      btif_queue_advance();
    } break;

    case BTA_AV_REMOTE_CMD_EVT:
    case BTA_AV_VENDOR_CMD_EVT:
    case BTA_AV_META_MSG_EVT:
    case BTA_AV_RC_FEAT_EVT:
    case BTA_AV_RC_PSM_EVT:
    case BTA_AV_REMOTE_RSP_EVT:
      btif_rc_handler(event, reinterpret_cast<tBTA_AV*>(p_data));
      break;

    case BTIF_AV_AVRCP_CLOSE_EVT:
    case BTA_AV_RC_CLOSE_EVT: {
      log::verbose("Peer {} : event={} : Stopping AV timer", peer_.PeerAddress(),
                   BtifAvEvent::EventName(event));
      alarm_cancel(peer_.AvOpenOnRcTimer());

      if (event == BTA_AV_RC_CLOSE_EVT) {
        btif_rc_handler(event, reinterpret_cast<tBTA_AV*>(p_data));
      }
    } break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      log::error("Peer {} : event={}: stream is not Opened", peer_.PeerAddress(),
                 BtifAvEvent::EventName(event));
      btif_a2dp_on_offload_started(peer_.PeerAddress(), BTA_AV_FAIL);
      break;

    case BTIF_AV_RECONFIGURE_REQ_EVT: {
      // Unlock JNI thread only
      auto req_data = peer_.GetReconfigureStreamData();
      if (req_data) {
        req_data.value().reconf_ready_promise.set_value();
      }
      break;
    }

    default:
      log::warn("Peer {} : Unhandled event={}", peer_.PeerAddress(), BtifAvEvent::EventName(event));
      return false;
  }

  return true;
}

void BtifAvStateMachine::StateOpening::OnEnter() {
  log::info("state=Opening peer={}", peer_.PeerAddress());

  // Inform the application that we are entering connecting state
  if (btif_av_both_enable()) {
    /* if peer connect to us, don't know which role it is */
    if (!peer_.SelfInitiatedConnection()) {
      return;
    }
  }
  btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_CONNECTING,
                               bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                               peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
}

void BtifAvStateMachine::StateOpening::OnExit() {
  log::info("state=Opening peer={}", peer_.PeerAddress());
}

bool BtifAvStateMachine::StateOpening::ProcessEvent(uint32_t event, void* p_data) {
  log::info("state=Opening peer={} event={} flags={} active_peer={}", peer_.PeerAddress(),
            BtifAvEvent::EventName(event), peer_.FlagsToString(), peer_.IsActivePeer());

  switch (event) {
    case BTIF_AV_STOP_STREAM_REQ_EVT:
    case BTIF_AV_SUSPEND_STREAM_REQ_EVT:
      break;  // Ignore

    case BTIF_AV_ACL_DISCONNECTED:
      // ACL Disconnected needs to be handled only in Opening state, because
      // it is in an intermediate state. In other states we can handle
      // incoming/outgoing connect/disconnect requests.
      log::warn("Peer {} : event={}: transitioning to Idle due to ACL Disconnect",
                peer_.PeerAddress(), BtifAvEvent::EventName(event));
      log_counter_metrics_btif(
              android::bluetooth::CodePathCounterKeyEnum::A2DP_CONNECTION_ACL_DISCONNECTED, 1);
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                   bt_status_t::BT_STATUS_FAIL, BTA_AV_FAIL,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      if (peer_.SelfInitiatedConnection()) {
        btif_queue_advance();
      }
      break;
    case BTA_AV_REJECT_EVT:
      log::warn("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());
      log_counter_metrics_btif(
              android::bluetooth::CodePathCounterKeyEnum::A2DP_CONNECTION_REJECT_EVT, 1);
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                   bt_status_t::BT_STATUS_AUTH_REJECTED, BTA_AV_FAIL,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      if (peer_.SelfInitiatedConnection()) {
        btif_queue_advance();
      }
      break;

    case BTA_AV_OPEN_EVT: {
      tBTA_AV* p_bta_data = reinterpret_cast<tBTA_AV*>(p_data);
      int av_state;
      tBTA_AV_STATUS status = p_bta_data->open.status;

      log::info("Peer {} : event={} flags={} status={}({}) edr=0x{:x}", peer_.PeerAddress(),
                BtifAvEvent::EventName(event), peer_.FlagsToString(), status,
                (status == BTA_AV_SUCCESS) ? "SUCCESS" : "FAILED", p_bta_data->open.edr);

      if (p_bta_data->open.status == BTA_AV_SUCCESS) {
        av_state = BtifAvStateMachine::kStateOpened;
        peer_.SetEdr(p_bta_data->open.edr);
        if (btif_av_src_sink_coexist_enabled()) {
          log::verbose("Peer {} sep={}, open_sep={}", peer_.PeerAddress(), peer_.PeerSep(),
                       p_bta_data->open.sep);
          /* if peer is wrong sep type, move it to BtifAvSxxx */
          if (peer_.PeerSep() != p_bta_data->open.sep) {
            BtifAvPeer* tmp_peer = nullptr;
            if (peer_.PeerSep() == AVDT_TSEP_SNK) {
              tmp_peer = btif_av_source.popPeer(peer_.PeerAddress());

              if (peer_.PeerAddress() != tmp_peer->PeerAddress()) {
                log::error("error, not same peer");
              }

              btif_av_sink.AddPeer(tmp_peer);
            } else {
              tmp_peer = btif_av_sink.popPeer(peer_.PeerAddress());

              if (peer_.PeerAddress() != tmp_peer->PeerAddress()) {
                log::error("error, not same peer");
              }

              btif_av_source.AddPeer(tmp_peer);
            }
            peer_.SetSep(p_bta_data->open.sep);
          }
          if (btif_rc_is_connected_peer(peer_.PeerAddress())) {
            log::verbose("AVRCP connected, update avrc sep");
            BTA_AvSetPeerSep(peer_.PeerAddress(), peer_.PeerSep());
          }
          btif_rc_check_pending_cmd(p_bta_data->open.bd_addr);
        }
        log::assert_that(peer_.PeerSep() == p_bta_data->open.sep,
                         "assert failed: peer_.PeerSep() == p_bta_data->open.sep");
        /** normally it can be checked in IDLE PENDING/CONNECT_REQ, in case:
         * 1 speacker connected to DUT and phone connect DUT, because
         * default
         * connect req is as SINK peer. only at here, we can know which
         * role
         *  it is.@{ */
        if (btif_av_src_sink_coexist_enabled()) {
          bool can_connect = true;
          if (peer_.IsSink()) {
            can_connect = btif_av_source.AllowedToConnect(peer_.PeerAddress());
            if (!can_connect) {
              btif_av_source_disconnect(peer_.PeerAddress());
            }
          } else if (peer_.IsSource()) {
            can_connect = btif_av_sink.AllowedToConnect(peer_.PeerAddress());
            if (!can_connect) {
              btif_av_sink_disconnect(peer_.PeerAddress());
            }
          }
        }
        /** @} */

        // Report the connection state to the application
        btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_CONNECTED,
                                     bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                     peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
        log_counter_metrics_btif(
                android::bluetooth::CodePathCounterKeyEnum::A2DP_CONNECTION_SUCCESS, 1);
      } else {
        if (btif_rc_is_connected_peer(peer_.PeerAddress())) {
          // Disconnect the AVRCP connection, in case the A2DP connectiton
          // failed for any reason.
          log::warn("Peer {} : Disconnecting AVRCP", peer_.PeerAddress());
          uint8_t peer_handle = btif_rc_get_connected_peer_handle(peer_.PeerAddress());
          if (peer_handle != BTRC_HANDLE_NONE) {
            BTA_AvCloseRc(peer_handle);
          }
          DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(peer_.PeerAddress(),
                                             IOT_CONF_KEY_A2DP_CONN_FAIL_COUNT);
        }
        av_state = BtifAvStateMachine::kStateIdle;
        // Report the connection state to the application
        btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                     bt_status_t::BT_STATUS_FAIL, status,
                                     peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
        log_counter_metrics_btif(
                android::bluetooth::CodePathCounterKeyEnum::A2DP_CONNECTION_FAILURE, 1);
      }

      // Change state to Open/Idle based on the status
      peer_.StateMachine().TransitionTo(av_state);
      if (peer_.IsSink()) {
        // If queued PLAY command, send it now
        btif_rc_check_handle_pending_play(p_bta_data->open.bd_addr,
                                          (p_bta_data->open.status == BTA_AV_SUCCESS));
      } else if (peer_.IsSource() && (p_bta_data->open.status == BTA_AV_SUCCESS)) {
        // Bring up AVRCP connection as well
        if (btif_av_src_sink_coexist_enabled() &&
            btif_av_sink.AllowedToConnect(peer_.PeerAddress())) {
          BTA_AvOpenRc(peer_.BtaHandle());
        }
      }
      if (peer_.SelfInitiatedConnection()) {
        btif_queue_advance();
      }
    } break;

    case BTIF_AV_SINK_CONFIG_REQ_EVT: {
      const btif_av_sink_config_req_t* p_config_req =
              static_cast<const btif_av_sink_config_req_t*>(p_data);
      /* before this point, we don't know it's role, actually peer is source */
      if (btif_av_both_enable()) {
        btif_av_report_sink_audio_config_state(
                p_config_req->peer_address, p_config_req->sample_rate, p_config_req->channel_count);
        break;
      }
      if (peer_.IsSource()) {
        btif_av_report_sink_audio_config_state(
                p_config_req->peer_address, p_config_req->sample_rate, p_config_req->channel_count);
      }
    } break;

    case BTIF_AV_CONNECT_REQ_EVT: {
      // The device has moved already to Opening, hence don't report the
      // connection state.
      log::warn(
              "Peer {} : event={} : device is already connecting, ignore Connect "
              "request",
              peer_.PeerAddress(), BtifAvEvent::EventName(event));
      log_counter_metrics_btif(android::bluetooth::CodePathCounterKeyEnum::A2DP_ALREADY_CONNECTING,
                               1);
      btif_queue_advance();
    } break;

    case BTA_AV_PENDING_EVT: {
      // The device has moved already to Opening, hence don't report the
      // connection state.
      log::warn(
              "Peer {} : event={} : device is already connecting, ignore incoming "
              "request",
              peer_.PeerAddress(), BtifAvEvent::EventName(event));
      log_counter_metrics_btif(android::bluetooth::CodePathCounterKeyEnum::A2DP_ALREADY_CONNECTING,
                               1);
    } break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      log::error("Peer {} : event={}: stream is not Opened", peer_.PeerAddress(),
                 BtifAvEvent::EventName(event));
      btif_a2dp_on_offload_started(peer_.PeerAddress(), BTA_AV_FAIL);
      log_counter_metrics_btif(
              android::bluetooth::CodePathCounterKeyEnum::A2DP_OFFLOAD_START_REQ_FAILURE, 1);
      break;

    case BTA_AV_CLOSE_EVT:
      btif_a2dp_on_stopped(nullptr, peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                   bt_status_t::BT_STATUS_FAIL, BTA_AV_FAIL,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      log_counter_metrics_btif(android::bluetooth::CodePathCounterKeyEnum::A2DP_CONNECTION_CLOSE,
                               1);
      DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(peer_.PeerAddress(), IOT_CONF_KEY_A2DP_CONN_FAIL_COUNT);
      if (peer_.SelfInitiatedConnection()) {
        btif_queue_advance();
      }
      break;

    case BTIF_AV_DISCONNECT_REQ_EVT:
      BTA_AvClose(peer_.BtaHandle());
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                   bt_status_t::BT_STATUS_FAIL, BTA_AV_FAIL,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      DEVICE_IOT_CONFIG_ADDR_INT_ADD_ONE(peer_.PeerAddress(), IOT_CONF_KEY_A2DP_CONN_FAIL_COUNT);
      log_counter_metrics_btif(
              android::bluetooth::CodePathCounterKeyEnum::A2DP_CONNECTION_DISCONNECTED, 1);
      if (peer_.SelfInitiatedConnection()) {
        btif_queue_advance();
      }
      break;

    case BTIF_AV_RECONFIGURE_REQ_EVT: {
      // Unlock JNI thread only
      auto req_data = peer_.GetReconfigureStreamData();
      if (req_data) {
        req_data.value().reconf_ready_promise.set_value();
      }
      break;
    }

      CHECK_RC_EVENT(event, reinterpret_cast<tBTA_AV*>(p_data));

    default:
      log_counter_metrics_btif(
              android::bluetooth::CodePathCounterKeyEnum::A2DP_CONNECTION_UNKNOWN_EVENT, 1);
      log::warn("Peer {} : Unhandled event={}", peer_.PeerAddress(), BtifAvEvent::EventName(event));
      return false;
  }
  return true;
}

void BtifAvStateMachine::StateOpened::OnEnter() {
  log::info("state=Opened peer={}", peer_.PeerAddress());

  peer_.ClearFlags(BtifAvPeer::kFlagLocalSuspendPending | BtifAvPeer::kFlagPendingStart |
                   BtifAvPeer::kFlagPendingStop);

  // Set the active peer if the first connected device.
  // NOTE: This should be done only if we are A2DP Sink, because the A2DP Sink
  // implementation in Java doesn't support active devices (yet).
  // For A2DP Source, the setting of the Active device is done by the
  // ActiveDeviceManager in Java.
  if (peer_.IsSource() && btif_av_sink.ActivePeer().IsEmpty()) {
    std::promise<void> peer_ready_promise;
    if (!btif_av_sink.SetActivePeer(peer_.PeerAddress(), std::move(peer_ready_promise))) {
      log::error("Error setting {} as active Source peer", peer_.PeerAddress());
    }
  }
}

void BtifAvStateMachine::StateOpened::OnExit() {
  log::info("state=Opened peer={}", peer_.PeerAddress());

  peer_.ClearFlags(BtifAvPeer::kFlagPendingStart);
}

bool BtifAvStateMachine::StateOpened::ProcessEvent(uint32_t event, void* p_data) {
  tBTA_AV* p_av = reinterpret_cast<tBTA_AV*>(p_data);

  log::info("state=Opened peer={} event={} flags={} active_peer={}", peer_.PeerAddress(),
            BtifAvEvent::EventName(event), peer_.FlagsToString(), peer_.IsActivePeer());

  if ((event == BTA_AV_REMOTE_CMD_EVT) && peer_.CheckFlags(BtifAvPeer::kFlagRemoteSuspend) &&
      (p_av->remote_cmd.rc_id == AVRC_ID_PLAY)) {
    log::verbose("Peer {} : Resetting remote suspend flag on RC PLAY", peer_.PeerAddress());
    peer_.ClearFlags(BtifAvPeer::kFlagRemoteSuspend);
  }

  switch (event) {
    case BTIF_AV_STOP_STREAM_REQ_EVT:
    case BTIF_AV_SUSPEND_STREAM_REQ_EVT:
    case BTIF_AV_ACL_DISCONNECTED:
      break;  // Ignore

    // Event sent by the Bluetooth Audio HAL to a source A2DP stack
    // when a stream is ready to play. The stack shall send AVDTP Start to the
    // remote device to start the stream.
    case BTIF_AV_START_STREAM_REQ_EVT: {
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());
      if (p_data) {
        const btif_av_start_stream_req_t* p_start_steam_req =
                static_cast<const btif_av_start_stream_req_t*>(p_data);
        log::info("Stream use_latency_mode={}", p_start_steam_req->use_latency_mode);
        peer_.SetUseLatencyMode(p_start_steam_req->use_latency_mode);
      }

      BTA_AvStart(peer_.BtaHandle(), peer_.UseLatencyMode());
      peer_.SetFlags(BtifAvPeer::kFlagPendingStart);
    } break;

    // Event sent by lower layer to indicate that the AVDTP stream is started.
    // May be initiated by the remote device to start a stream, in this case the
    // event is ignored by source A2DP, and the stack shall immediately suspend
    // the stream.
    case BTA_AV_START_EVT: {
      log::info("Peer {} : event={} status={} suspending={} initiator={} flags={}",
                peer_.PeerAddress(), BtifAvEvent::EventName(event), p_av->start.status,
                p_av->start.suspending, p_av->start.initiator, peer_.FlagsToString());

      if ((p_av->start.status == BTA_SUCCESS) && p_av->start.suspending) {
        return true;
      }

      // If remote tries to start A2DP when DUT is A2DP Source, then Suspend.
      // If A2DP is Sink and call is active, then disconnect the AVDTP
      // channel.
      bool should_suspend = false;
      if (peer_.IsSink()) {
        if (!peer_.CheckFlags(BtifAvPeer::kFlagPendingStart | BtifAvPeer::kFlagRemoteSuspend)) {
          log::warn("Peer {} : trigger Suspend as remote initiated", peer_.PeerAddress());
          should_suspend = true;
        } else if (!peer_.IsActivePeer()) {
          log::warn("Peer {} : trigger Suspend as non-active", peer_.PeerAddress());
          should_suspend = true;
        }

        // Invoke the started handler only when initiator.
        if (!com::android::bluetooth::flags::a2dp_ignore_started_when_responder() ||
            peer_.CheckFlags(BtifAvPeer::kFlagPendingStart)) {
          if (btif_a2dp_on_started(peer_.PeerAddress(), &p_av->start, A2dpType::kSource)) {
            // Only clear pending flag after acknowledgement
            peer_.ClearFlags(BtifAvPeer::kFlagPendingStart);
          }
        }
      }

      // Remain in Open state if status failed
      if (p_av->start.status != BTA_AV_SUCCESS) {
        return false;
      }

      if (peer_.IsSource() && peer_.IsActivePeer()) {
        // Remove flush state, ready for streaming
        btif_a2dp_sink_set_rx_flush(false);
        btif_a2dp_sink_on_start();
      }

      if (should_suspend) {
        btif_av_source_dispatch_sm_event(peer_.PeerAddress(), BTIF_AV_SUSPEND_STREAM_REQ_EVT);
      }

      if (com::android::bluetooth::flags::av_stream_reconfigure_fix() &&
          peer_.CheckFlags(BtifAvPeer::kFlagPendingReconfigure)) {
        log::info(
                "Peer {} : Stream started but reconfiguration pending. "
                "Reconfiguring stream",
                peer_.PeerAddress());
        btif_av_source_dispatch_sm_event(peer_.PeerAddress(), BTIF_AV_RECONFIGURE_REQ_EVT);
      }

      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateStarted);
      break;
    }

    case BTIF_AV_DISCONNECT_REQ_EVT:
      BTA_AvClose(peer_.BtaHandle());
      if (peer_.IsSource()) {
        BTA_AvCloseRc(peer_.BtaHandle());
      }

      // Inform the application that we are disconnecting
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTING,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);

      // Wait in closing state until fully closed
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateClosing);
      break;

    case BTA_AV_CLOSE_EVT:
      // AVDTP link is closed
      // Inform the application that we are disconnecting
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTING,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      // Change state to Idle, send acknowledgement if start is pending
      if (peer_.CheckFlags(BtifAvPeer::kFlagPendingStart)) {
        log::warn("Peer {} : failed pending start request", peer_.PeerAddress());
        tBTA_AV_START av_start = {.chnl = p_av->close.chnl,
                                  .hndl = p_av->close.hndl,
                                  .status = BTA_AV_FAIL_STREAM,
                                  .initiator = true,
                                  .suspending = true};
        btif_a2dp_on_started(peer_.PeerAddress(), &av_start,
                             peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
        // Pending start flag will be cleared when exit current state
      } else if (peer_.IsActivePeer()) {
        btif_a2dp_on_stopped(nullptr, peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      }

      // Inform the application that we are disconnected
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      break;

    case BTA_AV_RECONFIG_EVT:
      if (p_av->reconfig.status != BTA_AV_SUCCESS) {
        log::warn("Peer {} : failed reconfiguration", peer_.PeerAddress());
        if (peer_.CheckFlags(BtifAvPeer::kFlagPendingStart)) {
          log::error("Peer {} : cannot proceed to do AvStart", peer_.PeerAddress());
          peer_.ClearFlags(BtifAvPeer::kFlagPendingStart);
          bluetooth::audio::a2dp::ack_stream_started(bluetooth::audio::a2dp::Status::FAILURE);
        }
        if (peer_.IsSink()) {
          btif_av_source_disconnect(peer_.PeerAddress());
        } else if (peer_.IsSource()) {
          btif_av_sink_disconnect(peer_.PeerAddress());
        }
        break;
      }

      if (peer_.IsActivePeer()) {
        log::info("Peer {} : Reconfig done - calling startSession() to audio HAL",
                  peer_.PeerAddress());
        std::promise<void> peer_ready_promise;
        std::future<void> peer_ready_future = peer_ready_promise.get_future();

        if (com::android::bluetooth::flags::a2dp_clear_pending_start_on_session_restart()) {
          // The stream may not be restarted without an explicit request from the
          // Bluetooth Audio HAL. Any start request that was pending before the
          // reconfiguration is invalidated when the session is ended.
          peer_.ClearFlags(BtifAvPeer::kFlagPendingStart);
        }

        btif_a2dp_source_start_session(peer_.PeerAddress(), std::move(peer_ready_promise));
      }
      if (peer_.CheckFlags(BtifAvPeer::kFlagPendingStart)) {
        log::info("Peer {} : Reconfig done - calling BTA_AvStart(0x{:x})", peer_.PeerAddress(),
                  peer_.BtaHandle());
        BTA_AvStart(peer_.BtaHandle(), peer_.UseLatencyMode());
      }
      break;

    case BTIF_AV_CONNECT_REQ_EVT: {
      log::warn("Peer {} : Ignore {} for same device", peer_.PeerAddress(),
                BtifAvEvent::EventName(event));
      btif_queue_advance();
    } break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      log::error("Peer {} : event={}: stream is not Started", peer_.PeerAddress(),
                 BtifAvEvent::EventName(event));
      btif_a2dp_on_offload_started(peer_.PeerAddress(), BTA_AV_FAIL);
      break;

    case BTIF_AV_AVRCP_REMOTE_PLAY_EVT:
      if (peer_.CheckFlags(BtifAvPeer::kFlagRemoteSuspend)) {
        log::verbose("Peer {} : Resetting remote suspend flag on RC PLAY", peer_.PeerAddress());
        peer_.ClearFlags(BtifAvPeer::kFlagRemoteSuspend);
      }
      break;

      CHECK_RC_EVENT(event, reinterpret_cast<tBTA_AV*>(p_data));

    case BTIF_AV_SET_LATENCY_REQ_EVT: {
      const btif_av_set_latency_req_t* p_set_latency_req =
              static_cast<const btif_av_set_latency_req_t*>(p_data);
      log::info("Peer {} : event={} flags={} is_low_latency={}", peer_.PeerAddress(),
                BtifAvEvent::EventName(event), peer_.FlagsToString(),
                p_set_latency_req->is_low_latency);

      BTA_AvSetLatency(peer_.BtaHandle(), p_set_latency_req->is_low_latency);
    } break;

    case BTIF_AV_RECONFIGURE_REQ_EVT: {
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());
      if (!peer_.IsSink()) {
        log::verbose("Peer {} is not sink", peer_.PeerAddress());
        break;
      }

      if (peer_.CheckFlags(BtifAvPeer::kFlagPendingStart)) {
        // The start stream request was sent but we wait for response.
        // Enable the reconfigure pending flag to schedule reconfiguration
        // after start stream response.
        peer_.SetFlags(BtifAvPeer::kFlagPendingReconfigure);
      } else {
        // Reconfigure
        peer_.ClearFlags(BtifAvPeer::kFlagPendingReconfigure);
        if (btif_av_source.Enabled()) {
          auto req_data = peer_.GetReconfigureStreamData();
          if (req_data) {
            btif_av_source.UpdateCodecConfig(peer_.PeerAddress(),
                                             req_data.value().codec_preferences,
                                             std::move(req_data.value().reconf_ready_promise));
          }
        }
      }
    } break;

    default:
      log::warn("Peer {} : Unhandled event={}", peer_.PeerAddress(), BtifAvEvent::EventName(event));
      return false;
  }
  return true;
}

void BtifAvStateMachine::StateStarted::OnEnter() {
  log::info("state=Started peer={}", peer_.PeerAddress());

  // We are again in started state, clear any remote suspend flags
  peer_.ClearFlags(BtifAvPeer::kFlagRemoteSuspend);

  btif_a2dp_sink_set_rx_flush(false);

  // Report that we have entered the Streaming stage. Usually, this should
  // be followed by focus grant. See set_audio_focus_state()
  btif_report_audio_state(peer_.PeerAddress(), BTAV_AUDIO_STATE_STARTED,
                          peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
}

void BtifAvStateMachine::StateStarted::OnExit() {
  log::info("state=Started peer={}", peer_.PeerAddress());
}

bool BtifAvStateMachine::StateStarted::ProcessEvent(uint32_t event, void* p_data) {
  tBTA_AV* p_av = reinterpret_cast<tBTA_AV*>(p_data);

  log::info("state=Started peer={} event={} flags={} active_peer={}", peer_.PeerAddress(),
            BtifAvEvent::EventName(event), peer_.FlagsToString(), peer_.IsActivePeer());

  switch (event) {
    case BTIF_AV_ACL_DISCONNECTED:
      break;  // Ignore

    case BTIF_AV_START_STREAM_REQ_EVT:
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());
      // We were started remotely, just ACK back the local request
      if (peer_.IsSink()) {
        btif_a2dp_on_started(peer_.PeerAddress(), nullptr,
                             peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      }
      break;

    // FIXME -- use suspend = true always to work around issue with BTA AV
    case BTIF_AV_STOP_STREAM_REQ_EVT:
    case BTIF_AV_SUSPEND_STREAM_REQ_EVT:
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());

      // There is a pending LocalSuspend already, ignore.
      if (peer_.CheckFlags(BtifAvPeer::kFlagLocalSuspendPending)) {
        break;
      }

      // Set pending flag to ensure the BTIF task is not trying to restart
      // the stream while suspend is in progress.
      peer_.SetFlags(BtifAvPeer::kFlagLocalSuspendPending);

      // If we were remotely suspended but suspend locally, local suspend
      // always overrides.
      peer_.ClearFlags(BtifAvPeer::kFlagRemoteSuspend);

      if (peer_.IsSink() &&
          (peer_.IsActivePeer() || !btif_av_stream_started_ready(A2dpType::kSource))) {
        // Immediately stop transmission of frames while suspend is pending
        if (event == BTIF_AV_STOP_STREAM_REQ_EVT) {
          btif_a2dp_on_stopped(nullptr, peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
        } else {
          // ensure tx frames are immediately suspended
          btif_a2dp_source_set_tx_flush(true);
        }
      } else if (peer_.IsSource()) {
        btif_a2dp_on_stopped(nullptr, peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      }
      BTA_AvStop(peer_.BtaHandle(), true);
      break;

    case BTIF_AV_DISCONNECT_REQ_EVT:
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());

      // Request AVDTP to close
      BTA_AvClose(peer_.BtaHandle());
      if (peer_.IsSource()) {
        BTA_AvCloseRc(peer_.BtaHandle());
      }

      // Inform the application that we are disconnecting
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTING,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);

      // Wait in closing state until fully closed
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateClosing);
      break;

    case BTA_AV_SUSPEND_EVT: {
      log::info("Peer {} : event={} status={} initiator={} flags={}", peer_.PeerAddress(),
                BtifAvEvent::EventName(event), p_av->suspend.status, p_av->suspend.initiator,
                peer_.FlagsToString());

      // A2DP suspended, stop A2DP encoder / decoder until resumed
      if (peer_.IsActivePeer() ||
          !btif_av_stream_started_ready(peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource)) {
        btif_a2dp_on_suspended(&p_av->suspend,
                               peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      }

      // If not successful, remain in current state
      if (p_av->suspend.status != BTA_AV_SUCCESS) {
        peer_.ClearFlags(BtifAvPeer::kFlagLocalSuspendPending);

        if (peer_.IsSink() && peer_.IsActivePeer()) {
          // Suspend failed, reset back tx flush state
          btif_a2dp_source_set_tx_flush(false);
        }
        return false;
      }

      btav_audio_state_t state = BTAV_AUDIO_STATE_REMOTE_SUSPEND;
      if (p_av->suspend.initiator != true) {
        // Remote suspend, notify HAL and await audioflinger to
        // suspend/stop stream.
        //
        // Set remote suspend flag to block media task from restarting
        // stream only if we did not already initiate a local suspend.
        if (!peer_.CheckFlags(BtifAvPeer::kFlagLocalSuspendPending)) {
          peer_.SetFlags(BtifAvPeer::kFlagRemoteSuspend);
        }
      } else {
        state = BTAV_AUDIO_STATE_STOPPED;
      }

      btif_report_audio_state(peer_.PeerAddress(), state,
                              peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      // Suspend completed, clear local pending flags while entering Opened
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateOpened);
    } break;

    case BTA_AV_STOP_EVT:
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());

      peer_.SetFlags(BtifAvPeer::kFlagPendingStop);
      peer_.ClearFlags(BtifAvPeer::kFlagLocalSuspendPending);

      // Don't change the encoder and audio provider state by a non-active
      // peer since they are shared between peers
      if (peer_.IsActivePeer() ||
          !btif_av_stream_started_ready(peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource)) {
        btif_a2dp_on_stopped(&p_av->suspend,
                             peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      }

      btif_report_audio_state(peer_.PeerAddress(), BTAV_AUDIO_STATE_STOPPED,
                              peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);

      // If stop was successful, change state to Open
      if (p_av->suspend.status == BTA_AV_SUCCESS) {
        peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateOpened);
      }

      break;

    case BTA_AV_CLOSE_EVT:
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());
      // Inform the application that we are disconnecting
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTING,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);

      peer_.SetFlags(BtifAvPeer::kFlagPendingStop);

      // AVDTP link is closed
      if (peer_.IsActivePeer()) {
        btif_a2dp_on_stopped(nullptr, peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      }

      // Inform the application that we are disconnected
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);

      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      if (peer_.CheckFlags(BtifAvPeer::kFlagLocalSuspendPending | BtifAvPeer::kFlagRemoteSuspend |
                           BtifAvPeer::kFlagPendingStop)) {
        log::warn("Peer {} : event={} flags={}: stream is Suspending", peer_.PeerAddress(),
                  BtifAvEvent::EventName(event), peer_.FlagsToString());
        btif_a2dp_on_offload_started(peer_.PeerAddress(), BTA_AV_FAIL);
        break;
      }
      BTA_AvOffloadStart(peer_.BtaHandle());
      break;

    case BTA_AV_OFFLOAD_START_RSP_EVT:
      btif_a2dp_on_offload_started(peer_.PeerAddress(), p_av->status);
      break;

    case BTIF_AV_SET_LATENCY_REQ_EVT: {
      const btif_av_set_latency_req_t* p_set_latency_req =
              static_cast<const btif_av_set_latency_req_t*>(p_data);
      log::info("Peer {} : event={} flags={} is_low_latency={}", peer_.PeerAddress(),
                BtifAvEvent::EventName(event), peer_.FlagsToString(),
                p_set_latency_req->is_low_latency);

      BTA_AvSetLatency(peer_.BtaHandle(), p_set_latency_req->is_low_latency);
    } break;

      CHECK_RC_EVENT(event, reinterpret_cast<tBTA_AV*>(p_data));

    case BTIF_AV_RECONFIGURE_REQ_EVT: {
      log::info("Peer {} : event={} flags={}", peer_.PeerAddress(), BtifAvEvent::EventName(event),
                peer_.FlagsToString());
      peer_.ClearFlags(BtifAvPeer::kFlagPendingReconfigure);
      if (btif_av_source.Enabled()) {
        auto req_data = peer_.GetReconfigureStreamData();
        if (req_data) {
          btif_av_source.UpdateCodecConfig(peer_.PeerAddress(), req_data.value().codec_preferences,
                                           std::move(req_data.value().reconf_ready_promise));
        }
      }
    } break;

    default:
      log::warn("Peer {} : Unhandled event={}", peer_.PeerAddress(), BtifAvEvent::EventName(event));
      return false;
  }

  return true;
}

void BtifAvStateMachine::StateClosing::OnEnter() {
  log::info("state=Closing peer={}", peer_.PeerAddress());

  if (peer_.IsActivePeer()) {
    if (peer_.IsSink()) {
      // Immediately stop transmission of frames
      btif_a2dp_source_set_tx_flush(true);
      // Wait for Audio Flinger to stop A2DP
    } else if (peer_.IsSource()) {
      btif_a2dp_sink_set_rx_flush(true);
    }
  }
}

void BtifAvStateMachine::StateClosing::OnExit() {
  log::info("state=Closing peer={}", peer_.PeerAddress());
}

bool BtifAvStateMachine::StateClosing::ProcessEvent(uint32_t event, void* p_data) {
  log::info("state=Closing peer={} event={} flags={} active_peer={}", peer_.PeerAddress(),
            BtifAvEvent::EventName(event), peer_.FlagsToString(), peer_.IsActivePeer());

  switch (event) {
    case BTIF_AV_SUSPEND_STREAM_REQ_EVT:
    case BTIF_AV_ACL_DISCONNECTED:
      break;  // Ignore

    case BTA_AV_STOP_EVT:
    case BTIF_AV_STOP_STREAM_REQ_EVT:
      if (peer_.IsActivePeer()) {
        btif_a2dp_on_stopped(nullptr, peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);
      }
      break;

    case BTA_AV_CLOSE_EVT:
      // Inform the application that we are disconnecting
      btif_report_connection_state(peer_.PeerAddress(), BTAV_CONNECTION_STATE_DISCONNECTED,
                                   bt_status_t::BT_STATUS_SUCCESS, BTA_AV_SUCCESS,
                                   peer_.IsSource() ? A2dpType::kSink : A2dpType::kSource);

      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      break;

    // Handle the RC_CLOSE event for the cleanup
    case BTA_AV_RC_CLOSE_EVT:
      btif_rc_handler(event, reinterpret_cast<tBTA_AV*>(p_data));
      break;

    // Handle the RC_BROWSE_CLOSE event for testing
    case BTA_AV_RC_BROWSE_CLOSE_EVT:
      btif_rc_handler(event, reinterpret_cast<tBTA_AV*>(p_data));
      break;

    case BTIF_AV_OFFLOAD_START_REQ_EVT:
      log::error("Peer {} : event={}: stream is not Opened", peer_.PeerAddress(),
                 BtifAvEvent::EventName(event));
      btif_a2dp_on_offload_started(peer_.PeerAddress(), BTA_AV_FAIL);
      break;

    case BTIF_AV_CONNECT_REQ_EVT:
      log::warn("Peer {} : Ignore {} in StateClosing", peer_.PeerAddress(),
                BtifAvEvent::EventName(event));
      btif_queue_advance();
      peer_.StateMachine().TransitionTo(BtifAvStateMachine::kStateIdle);
      break;

    case BTIF_AV_RECONFIGURE_REQ_EVT: {
      // Unlock JNI thread only
      auto req_data = peer_.GetReconfigureStreamData();
      if (req_data) {
        req_data.value().reconf_ready_promise.set_value();
      }
      break;
    }

    default:
      log::warn("Peer {} : Unhandled event={}", peer_.PeerAddress(), BtifAvEvent::EventName(event));
      return false;
  }
  return true;
}

/**
 * Timer to trigger AV Open on the Source if the remote Sink device
 * establishes AVRCP connection without AV connection. The timer is needed to
 * interoperate with headsets that do establish AV after AVRCP connection.
 */
static void btif_av_source_initiate_av_open_timer_timeout(void* data) {
  BtifAvPeer* peer = reinterpret_cast<BtifAvPeer*>(data);
  bool device_connected = false;

  if (com::android::bluetooth::flags::avrcp_connect_a2dp_with_delay() && is_new_avrcp_enabled()) {
    // check if device is connected
    if (bluetooth::avrcp::AvrcpService::Get() != nullptr) {
      device_connected =
              bluetooth::avrcp::AvrcpService::Get()->IsDeviceConnected(peer->PeerAddress());
    }
  } else {
    device_connected = btif_rc_is_connected_peer(peer->PeerAddress());
  }

  log::verbose("Peer {}", peer->PeerAddress());

  // Check if AVRCP is connected to the peer
  if (!device_connected) {
    log::error("AVRCP peer {} is not connected", peer->PeerAddress());
    return;
  }

  // Connect to the AVRCP peer
  if (btif_av_source.Enabled() && btif_av_source.FindPeer(peer->PeerAddress()) == peer) {
    log::verbose("Connecting to AVRCP peer {}", peer->PeerAddress());
    btif_av_source_dispatch_sm_event(peer->PeerAddress(), BTIF_AV_CONNECT_REQ_EVT);
  }
}

/**
 * Timer to trigger AV Open on the Sink if the remote Source device
 * establishes AVRCP connection without AV connection.
 */
static void btif_av_sink_initiate_av_open_timer_timeout(void* data) {
  BtifAvPeer* peer = reinterpret_cast<BtifAvPeer*>(data);

  log::verbose("Peer {}", peer->PeerAddress());

  // Check if AVRCP is connected to the peer
  if (!btif_rc_is_connected_peer(peer->PeerAddress())) {
    log::error("AVRCP peer {} is not connected", peer->PeerAddress());
    return;
  }

  // Connect to the AVRCP peer
  if (btif_av_sink.Enabled() && btif_av_sink.FindPeer(peer->PeerAddress()) == peer) {
    log::verbose("Connecting to AVRCP peer {}", peer->PeerAddress());
    btif_av_sink_dispatch_sm_event(peer->PeerAddress(), BTIF_AV_CONNECT_REQ_EVT);
  }
}

/**
 * Report the A2DP connection state
 *
 * @param peer_address the peer address
 * @param state the connection state
 */
static void btif_report_connection_state(const RawAddress& peer_address,
                                         btav_connection_state_t state, bt_status_t status,
                                         uint8_t error_code, const A2dpType local_a2dp_type) {
  log::info("peer={} state={}", peer_address, state);
  if (btif_av_src_sink_coexist_enabled() && btif_av_both_enable()) {
    BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
    if (peer == nullptr) {
      log::error("peer is null");
      return;
    }

    if (peer->IsSink()) {
      do_in_jni_thread(base::BindOnce(btif_av_source.Callbacks()->connection_state_cb, peer_address,
                                      state, btav_error_t{}));
    } else if (peer->IsSource()) {
      do_in_jni_thread(base::BindOnce(btif_av_sink.Callbacks()->connection_state_cb, peer_address,
                                      state, btav_error_t{}));
    }
    return;
  }

  if (btif_av_source.Enabled()) {
    do_in_jni_thread(base::BindOnce(btif_av_source.Callbacks()->connection_state_cb, peer_address,
                                    state,
                                    btav_error_t{.status = status, .error_code = error_code}));
  } else if (btif_av_sink.Enabled()) {
    do_in_jni_thread(base::BindOnce(btif_av_sink.Callbacks()->connection_state_cb, peer_address,
                                    state,
                                    btav_error_t{.status = status, .error_code = error_code}));
  }
}

/**
 * Report the audio state of the A2DP connection.
 * The state is updated when either the remote ends starts streaming
 * (Started state) or whenever it transitions out of Started state
 * (to Opened or Streaming state).
 *
 * @param peer_address the peer address
 * @param state the audio state
 */
static void btif_report_audio_state(const RawAddress& peer_address, btav_audio_state_t state,
                                    const A2dpType local_a2dp_type) {
  log::info("peer={} state={}", peer_address, state);

  if (btif_av_both_enable()) {
    BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
    if (peer->IsSink()) {
      do_in_jni_thread(
              base::BindOnce(btif_av_source.Callbacks()->audio_state_cb, peer_address, state));
    } else if (peer->IsSource()) {
      do_in_jni_thread(
              base::BindOnce(btif_av_sink.Callbacks()->audio_state_cb, peer_address, state));
    }
    return;
  }
  if (btif_av_source.Enabled()) {
    do_in_jni_thread(
            base::BindOnce(btif_av_source.Callbacks()->audio_state_cb, peer_address, state));
  } else if (btif_av_sink.Enabled()) {
    do_in_jni_thread(base::BindOnce(btif_av_sink.Callbacks()->audio_state_cb, peer_address, state));
  }

  using android::bluetooth::a2dp::AudioCodingModeEnum;
  using android::bluetooth::a2dp::PlaybackStateEnum;
  PlaybackStateEnum playback_state = PlaybackStateEnum::PLAYBACK_STATE_UNKNOWN;
  switch (state) {
    case BTAV_AUDIO_STATE_STARTED:
      playback_state = PlaybackStateEnum::PLAYBACK_STATE_PLAYING;
      break;
    case BTAV_AUDIO_STATE_STOPPED:
      playback_state = PlaybackStateEnum::PLAYBACK_STATE_NOT_PLAYING;
      break;
    default:
      break;
  }
  AudioCodingModeEnum audio_coding_mode = btif_av_is_a2dp_offload_running()
                                                  ? AudioCodingModeEnum::AUDIO_CODING_MODE_HARDWARE
                                                  : AudioCodingModeEnum::AUDIO_CODING_MODE_SOFTWARE;

  log_a2dp_playback_event(peer_address, playback_state, audio_coding_mode);
}

void btif_av_report_source_codec_state(
        const RawAddress& peer_address, const btav_a2dp_codec_config_t& codec_config,
        const std::vector<btav_a2dp_codec_config_t>& codecs_local_capabilities,
        const std::vector<btav_a2dp_codec_config_t>& codecs_selectable_capabilities) {
  log::verbose("peer={}", peer_address);
  if (btif_av_source.Enabled()) {
    do_in_jni_thread(base::BindOnce(btif_av_source.Callbacks()->audio_config_cb, peer_address,
                                    codec_config, codecs_local_capabilities,
                                    codecs_selectable_capabilities));
  }
}

/**
 * Report the audio config state of the A2DP Sink connection.
 *
 * @param peer_address the peer address
 * @param sample_rate the sample rate (in samples per second)
 * @param channel_count the channel count (1 for Mono, 2 for Stereo)
 */
static void btif_av_report_sink_audio_config_state(const RawAddress& peer_address, int sample_rate,
                                                   int channel_count) {
  log::info("peer={} sample_rate={} channel_count={}", peer_address, sample_rate, channel_count);
  if (btif_av_sink.Enabled()) {
    do_in_jni_thread(base::BindOnce(btif_av_sink.Callbacks()->audio_config_cb, peer_address,
                                    sample_rate, channel_count));
  }
}

/**
 * Call out to JNI / JAVA layers to retrieve whether the mandatory codec is
 * more preferred than others.
 *
 * @param peer_address the peer address
 */
static void btif_av_query_mandatory_codec_priority(const RawAddress& peer_address) {
  auto query_priority = [](const RawAddress& peer_address) {
    if (!btif_av_source.Enabled()) {
      log::warn("BTIF AV Source is not enabled");
      return;
    }
    btav_source_callbacks_t* callbacks = btif_av_source.Callbacks();
    bool preferred = callbacks != nullptr && callbacks->mandatory_codec_preferred_cb(peer_address);
    if (preferred) {
      auto apply_priority = [](const RawAddress& peer_address, bool preferred) {
        BtifAvPeer* peer = btif_av_find_peer(peer_address, A2dpType::kSource);
        if (peer == nullptr) {
          log::warn("btif_av_query_mandatory_codec_priority: peer is null");
          return;
        }
        peer->SetMandatoryCodecPreferred(preferred);
      };
      do_in_main_thread(base::BindOnce(apply_priority, peer_address, preferred));
    }
  };
  if (btif_av_source.Enabled()) {
    do_in_jni_thread(base::BindOnce(query_priority, peer_address));
  }
}

static BtifAvPeer* btif_av_handle_both_peer(uint8_t peer_sep, const RawAddress& peer_address,
                                            tBTA_AV_HNDL bta_handle) {
  BtifAvPeer* peer = nullptr;

  if (peer_address != RawAddress::kEmpty) {
    if (btif_av_both_enable()) {
      peer = btif_av_find_peer(peer_address, A2dpType::kUnknown);
      /* if no this peer, default it's sink device */
      if (peer == nullptr) {
        if (peer_sep == AVDT_TSEP_SRC) {
          log::verbose("peer_sep({}), create a new source peer", peer_sep);
          peer = btif_av_sink.FindOrCreatePeer(peer_address, bta_handle);
        } else if (peer_sep == AVDT_TSEP_SNK) {
          log::verbose("peer_sep({}), create a new sink peer", peer_sep);
          peer = btif_av_source.FindOrCreatePeer(peer_address, bta_handle);
        } else {
          if (btif_av_source.GetPeersCount() != 0) {
            log::verbose(
                    "peer_sep invalid, and already has sink peer, so try create a "
                    "new sink peer");
            peer = btif_av_source.FindOrCreatePeer(peer_address, bta_handle);
          } else if (btif_av_sink.GetPeersCount() != 0) {
            log::verbose(
                    "peer_sep invalid, and already has source peer, so try create "
                    "a new source peer");
            peer = btif_av_sink.FindOrCreatePeer(peer_address, bta_handle);
          } else {
            log::verbose(
                    "peer_sep invalid, and no active peer, so try create a new "
                    "sink peer");
            peer = btif_av_source.FindOrCreatePeer(peer_address, bta_handle);
          }
        }
      }
    } else {
      if (peer_sep == AVDT_TSEP_SNK) {
        log::verbose("peer_sep({}), only init src create a new source peer", peer_sep);
        peer = btif_av_source.FindOrCreatePeer(peer_address, bta_handle);
      } else if (peer_sep == AVDT_TSEP_SRC) {
        log::verbose("peer_sep({}), only init sink create a new source peer", peer_sep);
        peer = btif_av_sink.FindOrCreatePeer(peer_address, bta_handle);
      }
    }
    if (peer == NULL && bta_handle != 0) {
      if (peer_sep == AVDT_TSEP_SNK) {
        peer = btif_av_source.FindPeerByHandle(bta_handle);
      } else if (peer_sep == AVDT_TSEP_SRC) {
        peer = btif_av_sink.FindPeerByHandle(bta_handle);
      }
      log::verbose("peer is check 3");
    }
  } else if (bta_handle != 0) {
    if (peer_sep == AVDT_TSEP_INVALID) {
      peer = btif_av_source.FindPeerByHandle(bta_handle);
      /* if no this peer, default it's sink device */
      if (peer == nullptr) {
        peer = btif_av_sink.FindPeerByHandle(bta_handle);
      }
    } else if (peer_sep == AVDT_TSEP_SNK) {
      peer = btif_av_source.FindPeerByHandle(bta_handle);
    } else if (peer_sep == AVDT_TSEP_SRC) {
      peer = btif_av_sink.FindPeerByHandle(bta_handle);
    }
  }
  return peer;
}

/**
 * Process BTIF or BTA AV or BTA AVRCP events. The processing is done on the
 * JNI thread.
 *
 * @param peer_sep the corresponding peer's SEP: AVDT_TSEP_SRC if the peer
 * is A2DP Source, or AVDT_TSEP_SNK if the peer is A2DP Sink.
 * @param peer_address the peer address if known, otherwise RawAddress::kEmpty
 * @param bta_handle the BTA handle for the peer if known, otherwise
 * kBtaHandleUnknown
 * @param btif_av_event the corresponding event
 */
static void btif_av_handle_event(uint8_t peer_sep, const RawAddress& peer_address,
                                 tBTA_AV_HNDL bta_handle, const BtifAvEvent& btif_av_event) {
  log::info("peer={} handle=0x{:x} event={}", peer_address, bta_handle, btif_av_event.ToString());

  BtifAvPeer* peer = nullptr;

  // Find the peer
  if (btif_av_src_sink_coexist_enabled()) {
    peer = btif_av_handle_both_peer(peer_sep, peer_address, bta_handle);
  } else {
    if (peer_address != RawAddress::kEmpty) {
      if (peer_sep == AVDT_TSEP_SNK) {
        peer = btif_av_source.FindOrCreatePeer(peer_address, bta_handle);
      } else if (peer_sep == AVDT_TSEP_SRC) {
        peer = btif_av_sink.FindOrCreatePeer(peer_address, bta_handle);
      }
    } else if (bta_handle != kBtaHandleUnknown) {
      if (peer_sep == AVDT_TSEP_SNK) {
        peer = btif_av_source.FindPeerByHandle(bta_handle);
      } else if (peer_sep == AVDT_TSEP_SRC) {
        peer = btif_av_sink.FindPeerByHandle(bta_handle);
      }
    }
  }
  if (peer == nullptr) {
    log::error(
            "Cannot find or create {} peer for peer={}  "
            "bta_handle=0x{:x} : event dropped: {}",
            peer_stream_endpoint_text(peer_sep), peer_address, bta_handle,
            btif_av_event.ToString());
    return;
  }

  peer->StateMachine().ProcessEvent(btif_av_event.Event(), btif_av_event.Data());
}

/**
 * Process BTA AV or BTA AVRCP events. The processing is done on the JNI
 * thread.
 *
 * @param peer_sep the corresponding peer's SEP: AVDT_TSEP_SRC if the peer
 * is A2DP Source, or AVDT_TSEP_SNK if the peer is A2DP Sink.
 * @param btif_av_event the corresponding event
 */
static void btif_av_handle_bta_av_event(uint8_t peer_sep, const BtifAvEvent& btif_av_event) {
  RawAddress peer_address = RawAddress::kEmpty;
  tBTA_AV_HNDL bta_handle = kBtaHandleUnknown;
  tBTA_AV_EVT event = btif_av_event.Event();
  tBTA_AV* p_data = reinterpret_cast<tBTA_AV*>(btif_av_event.Data());
  std::string msg;

  log::verbose("peer_sep={} event={}", peer_stream_endpoint_text(peer_sep),
               btif_av_event.ToString());

  switch (event) {
    case BTA_AV_ENABLE_EVT: {
      const tBTA_AV_ENABLE& enable = p_data->enable;
      log::verbose("Enable features=0x{:x}", enable.features);
      return;  // Nothing to do
    }
    case BTA_AV_REGISTER_EVT: {
      const tBTA_AV_REGISTER& reg = p_data->reg;
      bta_handle = reg.hndl;
      uint8_t peer_id = reg.app_id;  // The PeerId is used as AppId
      log::verbose("Register bta_handle=0x{:x} app_id={}", bta_handle, reg.app_id);
      if (btif_av_src_sink_coexist_enabled()) {
        if (peer_sep == AVDT_TSEP_INVALID) {
          if (reg.peer_sep == AVDT_TSEP_SNK) {
            peer_sep = AVDT_TSEP_SNK;
          } else {
            peer_sep = AVDT_TSEP_SRC;
          }
        }
      }
      if (peer_sep == AVDT_TSEP_SNK) {
        btif_av_source.BtaHandleRegistered(peer_id, bta_handle);
      } else if (peer_sep == AVDT_TSEP_SRC) {
        btif_av_sink.BtaHandleRegistered(peer_id, bta_handle);
      }
      return;  // Nothing else to do
    }
    case BTA_AV_OPEN_EVT: {
      const tBTA_AV_OPEN& open = p_data->open;
      peer_address = open.bd_addr;
      bta_handle = open.hndl;
      msg = "Stream opened";
      break;
    }
    case BTA_AV_CLOSE_EVT: {
      const tBTA_AV_CLOSE& close = p_data->close;
      bta_handle = close.hndl;
      msg = "Stream closed";
      break;
    }
    case BTA_AV_START_EVT: {
      const tBTA_AV_START& start = p_data->start;
      bta_handle = start.hndl;
      msg = "Stream started";
      break;
    }
    case BTA_AV_SUSPEND_EVT:
    case BTA_AV_STOP_EVT: {
      const tBTA_AV_SUSPEND& suspend = p_data->suspend;
      bta_handle = suspend.hndl;
      msg = "Stream stopped";
      break;
    }
    case BTA_AV_PROTECT_REQ_EVT: {
      const tBTA_AV_PROTECT_REQ& protect_req = p_data->protect_req;
      bta_handle = protect_req.hndl;
      break;
    }
    case BTA_AV_PROTECT_RSP_EVT: {
      const tBTA_AV_PROTECT_RSP& protect_rsp = p_data->protect_rsp;
      bta_handle = protect_rsp.hndl;
      break;
    }
    case BTA_AV_RC_OPEN_EVT: {
      const tBTA_AV_RC_OPEN& rc_open = p_data->rc_open;
      peer_address = rc_open.peer_addr;
      break;
    }
    case BTA_AV_RC_CLOSE_EVT: {
      const tBTA_AV_RC_CLOSE& rc_close = p_data->rc_close;
      peer_address = rc_close.peer_addr;
      break;
    }
    case BTA_AV_RC_BROWSE_OPEN_EVT: {
      const tBTA_AV_RC_BROWSE_OPEN& rc_browse_open = p_data->rc_browse_open;
      peer_address = rc_browse_open.peer_addr;
      break;
    }
    case BTA_AV_RC_BROWSE_CLOSE_EVT: {
      const tBTA_AV_RC_BROWSE_CLOSE& rc_browse_close = p_data->rc_browse_close;
      peer_address = rc_browse_close.peer_addr;
      break;
    }
    case BTA_AV_REMOTE_CMD_EVT:
    case BTA_AV_REMOTE_RSP_EVT:
    case BTA_AV_VENDOR_CMD_EVT:
    case BTA_AV_VENDOR_RSP_EVT:
    case BTA_AV_META_MSG_EVT: {
      if (btif_av_src_sink_coexist_enabled()) {
        if (peer_sep == AVDT_TSEP_INVALID) {
          const tBTA_AV_REMOTE_CMD& rc_rmt_cmd = p_data->remote_cmd;
          btif_rc_get_addr_by_handle(rc_rmt_cmd.rc_handle, peer_address);
          if (peer_address == RawAddress::kEmpty) {
            peer_address = btif_av_source.ActivePeer();
            if (peer_address == RawAddress::kEmpty) {
              peer_address = btif_av_sink.ActivePeer();
            }
          }
        } else if (peer_sep == AVDT_TSEP_SNK) {
          peer_address = btif_av_source.ActivePeer();
        } else if (peer_sep == AVDT_TSEP_SRC) {
          peer_address = btif_av_sink.ActivePeer();
        }
        break;
      } else {
        [[fallthrough]];
      }
    }
    case BTA_AV_OFFLOAD_START_RSP_EVT: {
      // TODO: Might be wrong - this code will be removed once those
      // events are received from the AVRCP module.
      if (peer_sep == AVDT_TSEP_SNK) {
        peer_address = btif_av_source.ActivePeer();
        msg = "Stream sink offloaded";
      } else if (peer_sep == AVDT_TSEP_SRC) {
        peer_address = btif_av_sink.ActivePeer();
        msg = "Stream source offloaded";
      }
      break;
    }
    case BTA_AV_RECONFIG_EVT: {
      const tBTA_AV_RECONFIG& reconfig = p_data->reconfig;
      bta_handle = reconfig.hndl;
      break;
    }
    case BTA_AV_PENDING_EVT: {
      const tBTA_AV_PEND& pend = p_data->pend;
      peer_address = pend.bd_addr;
      break;
    }
    case BTA_AV_REJECT_EVT: {
      const tBTA_AV_REJECT& reject = p_data->reject;
      peer_address = reject.bd_addr;
      bta_handle = reject.hndl;
      break;
    }
    case BTA_AV_RC_FEAT_EVT: {
      const tBTA_AV_RC_FEAT& rc_feat = p_data->rc_feat;
      peer_address = rc_feat.peer_addr;
      break;
    }
    case BTA_AV_RC_PSM_EVT: {
      const tBTA_AV_RC_PSM& rc_psm = p_data->rc_cover_art_psm;
      peer_address = rc_psm.peer_addr;
      break;
    }
  }

  if (!msg.empty()) {
    BTM_LogHistory(kBtmLogHistoryTag, peer_address, msg, btif_av_event.ToString());
  }
  btif_av_handle_event(peer_sep, peer_address, bta_handle, btif_av_event);
}

bool btif_av_both_enable(void) { return btif_av_sink.Enabled() && btif_av_source.Enabled(); }

static bool is_a2dp_source_property_enabled(void) {
#ifdef __ANDROID__
  return android::sysprop::BluetoothProperties::isProfileA2dpSourceEnabled().value_or(false);
#else
  return false;
#endif
}

static bool is_a2dp_sink_property_enabled(void) {
#ifdef __ANDROID__
  return android::sysprop::BluetoothProperties::isProfileA2dpSinkEnabled().value_or(false);
#else
  return false;
#endif
}
bool btif_av_src_sink_coexist_enabled(void) {
  return is_a2dp_sink_property_enabled() && is_a2dp_source_property_enabled();
}

static void bta_av_source_callback(tBTA_AV_EVT event, tBTA_AV* p_data) {
  BtifAvEvent btif_av_event(event, p_data, sizeof(tBTA_AV));
  log::verbose("event={}", btif_av_event.ToString());

  do_in_main_thread(base::BindOnce(&btif_av_handle_bta_av_event, AVDT_TSEP_SNK /* peer_sep */,
                                   btif_av_event));
}

static void bta_av_sink_callback(tBTA_AV_EVT event, tBTA_AV* p_data) {
  BtifAvEvent btif_av_event(event, p_data, sizeof(tBTA_AV));
  do_in_main_thread(base::BindOnce(&btif_av_handle_bta_av_event, AVDT_TSEP_SRC /* peer_sep */,
                                   btif_av_event));
}

static void bta_av_event_callback(tBTA_AV_EVT event, tBTA_AV* p_data) {
  if (btif_av_both_enable()) {
    BtifAvEvent btif_av_event(event, p_data, sizeof(tBTA_AV));
    do_in_main_thread(base::BindOnce(&btif_av_handle_bta_av_event, AVDT_TSEP_INVALID /* peer_sep */,
                                     btif_av_event));
    return;
  }

  if (btif_av_is_sink_enabled()) {
    return bta_av_sink_callback(event, p_data);
  }

  return bta_av_source_callback(event, p_data);
}

// TODO: All processing should be done on the JNI thread
static void bta_av_sink_media_callback(const RawAddress& peer_address, tBTA_AV_EVT event,
                                       tBTA_AV_MEDIA* p_data) {
  log::verbose("event={} peer {}", event, peer_address);

  switch (event) {
    case BTA_AV_SINK_MEDIA_DATA_EVT: {
      BtifAvPeer* peer = btif_av_sink_find_peer(peer_address);
      if (peer != nullptr && peer->IsActivePeer()) {
        int state = peer->StateMachine().StateId();
        if ((state == BtifAvStateMachine::kStateStarted) ||
            (state == BtifAvStateMachine::kStateOpened)) {
          uint8_t queue_len = btif_a2dp_sink_enqueue_buf(reinterpret_cast<BT_HDR*>(p_data));
          log::verbose("Packets in Sink queue {}", queue_len);
        }
      }
      break;
    }
    case BTA_AV_SINK_MEDIA_CFG_EVT: {
      btif_av_sink_config_req_t config_req;

      log::verbose("address={}", p_data->avk_config.bd_addr);

      // Update the codec info of the A2DP Sink decoder
      btif_a2dp_sink_update_decoder(p_data->avk_config.bd_addr,
                                    reinterpret_cast<uint8_t*>(p_data->avk_config.codec_info));

      config_req.sample_rate = A2DP_GetTrackSampleRate(p_data->avk_config.codec_info);
      if (config_req.sample_rate == -1) {
        log::error("Cannot get the track frequency");
        break;
      }
      config_req.channel_count = A2DP_GetTrackChannelCount(p_data->avk_config.codec_info);
      if (config_req.channel_count == -1) {
        log::error("Cannot get the channel count");
        break;
      }
      config_req.peer_address = p_data->avk_config.bd_addr;
      BtifAvEvent btif_av_event(BTIF_AV_SINK_CONFIG_REQ_EVT, &config_req, sizeof(config_req));
      do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                       AVDT_TSEP_SRC,  // peer_sep
                                       config_req.peer_address, kBtaHandleUnknown, btif_av_event));
      break;
    }
    default:
      break;
  }
}

// Initializes the AV interface for source mode
bt_status_t btif_av_source_init(btav_source_callbacks_t* callbacks, int max_connected_audio_devices,
                                const std::vector<btav_a2dp_codec_config_t>& codec_priorities,
                                const std::vector<btav_a2dp_codec_config_t>& offloading_preference,
                                std::vector<btav_a2dp_codec_info_t>* supported_codecs) {
  log::info("");
  std::promise<bt_status_t> init_complete_promise;
  std::future<bt_status_t> init_complete_promise_future = init_complete_promise.get_future();
  const auto& status = do_in_main_thread(
          base::BindOnce(&BtifAvSource::Init, base::Unretained(&btif_av_source), callbacks,
                         max_connected_audio_devices, codec_priorities, offloading_preference,
                         supported_codecs, std::move(init_complete_promise)));
  if (status == BT_STATUS_SUCCESS) {
    init_complete_promise_future.wait();
    return init_complete_promise_future.get();
  } else {
    log::warn("Failed to init source profile");
    return status;
  }
}

// Initializes the AV interface for sink mode
bt_status_t btif_av_sink_init(btav_sink_callbacks_t* callbacks, int max_connected_audio_devices) {
  log::info("");
  std::promise<bt_status_t> init_complete_promise;
  std::future<bt_status_t> init_complete_promise_future = init_complete_promise.get_future();
  const auto status = do_in_main_thread(
          base::BindOnce(&BtifAvSink::Init, base::Unretained(&btif_av_sink), callbacks,
                         max_connected_audio_devices, std::move(init_complete_promise)));
  if (status == BT_STATUS_SUCCESS) {
    init_complete_promise_future.wait();
    return init_complete_promise_future.get();
  } else {
    log::warn("Failed to init sink");
    return status;
  }
}

// Updates the final focus state reported by components calling this module
void btif_av_sink_set_audio_focus_state(int state) {
  log::info("state={}", state);
  btif_a2dp_sink_set_focus_state_req((btif_a2dp_sink_focus_state_t)state);
}

// Updates the track gain (used for ducking).
void btif_av_sink_set_audio_track_gain(float gain) {
  log::info("gain={:f}", gain);
  btif_a2dp_sink_set_audio_track_gain(gain);
}

// Establishes the AV signalling channel with the remote headset
static bt_status_t connect_int(RawAddress* peer_address, uint16_t uuid) {
  log::info("peer={} uuid=0x{:x}", *peer_address, uuid);

  if (btif_av_both_enable()) {
    const RawAddress tmp = *peer_address;
    if (uuid == UUID_SERVCLASS_AUDIO_SOURCE) {
      btif_av_source_dispatch_sm_event(tmp, BTIF_AV_CONNECT_REQ_EVT);
    } else if (uuid == UUID_SERVCLASS_AUDIO_SINK) {
      btif_av_sink_dispatch_sm_event(tmp, BTIF_AV_CONNECT_REQ_EVT);
    }
    return BT_STATUS_SUCCESS;
  }

  auto connection_task = [](RawAddress* peer_address, uint16_t uuid) {
    BtifAvPeer* peer = nullptr;
    if (uuid == UUID_SERVCLASS_AUDIO_SOURCE) {
      peer = btif_av_source.FindOrCreatePeer(*peer_address, kBtaHandleUnknown);
    } else if (uuid == UUID_SERVCLASS_AUDIO_SINK) {
      peer = btif_av_sink.FindOrCreatePeer(*peer_address, kBtaHandleUnknown);
    }
    if (peer == nullptr) {
      btif_queue_advance();
      return;
    }
    peer->StateMachine().ProcessEvent(BTIF_AV_CONNECT_REQ_EVT, nullptr);
  };
  bt_status_t status = do_in_main_thread(base::BindOnce(connection_task, peer_address, uuid));
  if (status != BT_STATUS_SUCCESS) {
    log::error("can't post connection task to main_thread");
  }
  return status;
}

static void set_source_silence_peer_int(const RawAddress& peer_address, bool silence) {
  log::info("peer={} silence={}", peer_address, silence);

  if (!btif_av_source.SetSilencePeer(peer_address, silence)) {
    log::error("Error setting silence state to {}", peer_address);
  }
}

// Set the active peer
static void set_active_peer_int(uint8_t peer_sep, const RawAddress& peer_address,
                                std::promise<void> peer_ready_promise) {
  log::info("peer_sep={} peer={}", peer_sep == AVDT_TSEP_SRC ? "Source" : "Sink", peer_address);

  BtifAvPeer* peer = nullptr;
  if (peer_sep == AVDT_TSEP_SNK) {
    if (!btif_av_src_sink_coexist_enabled() ||
        (btif_av_src_sink_coexist_enabled() && btif_av_both_enable() &&
         (btif_av_sink.FindPeer(peer_address) == nullptr))) {
      if (!btif_av_source.SetActivePeer(peer_address, std::move(peer_ready_promise))) {
        log::error("Error setting {} as active Sink peer", peer_address);
      }
    }
    return;
  }
  if (peer_sep == AVDT_TSEP_SRC) {
    if (!btif_av_src_sink_coexist_enabled() ||
        (btif_av_src_sink_coexist_enabled() && btif_av_both_enable() &&
         (btif_av_source.FindPeer(peer_address) == nullptr))) {
      if (!btif_av_sink.SetActivePeer(peer_address, std::move(peer_ready_promise))) {
        log::error("Error setting {} as active Source peer", peer_address);
      }
    }
    return;
  }
  // If reached here, we could not set the active peer
  log::error("Cannot set active {} peer to {}: peer not {}",
             (peer_sep == AVDT_TSEP_SRC) ? "Source" : "Sink", peer_address,
             (peer == nullptr) ? "found" : "connected");
  peer_ready_promise.set_value();
}

bt_status_t btif_av_source_connect(const RawAddress& peer_address) {
  log::info("peer={}", peer_address);

  if (!btif_av_source.Enabled()) {
    log::warn("BTIF AV Source is not enabled");
    return BT_STATUS_NOT_READY;
  }

  RawAddress peer_address_copy(peer_address);
  return btif_queue_connect(UUID_SERVCLASS_AUDIO_SOURCE, &peer_address_copy, connect_int);
}

bt_status_t btif_av_sink_connect(const RawAddress& peer_address) {
  log::info("peer={}", peer_address);

  if (!btif_av_sink.Enabled()) {
    log::warn("BTIF AV Sink is not enabled");
    return BT_STATUS_NOT_READY;
  }

  RawAddress peer_address_copy(peer_address);
  return btif_queue_connect(UUID_SERVCLASS_AUDIO_SINK, &peer_address_copy, connect_int);
}

bt_status_t btif_av_source_disconnect(const RawAddress& peer_address) {
  log::info("peer={}", peer_address);

  if (!btif_av_source.Enabled()) {
    log::warn("BTIF AV Source is not enabled");
    return BT_STATUS_NOT_READY;
  }

  BtifAvEvent btif_av_event(BTIF_AV_DISCONNECT_REQ_EVT, &peer_address, sizeof(peer_address));
  return do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                          AVDT_TSEP_SNK,  // peer_sep
                                          peer_address, kBtaHandleUnknown, btif_av_event));
}

bt_status_t btif_av_sink_disconnect(const RawAddress& peer_address) {
  log::info("peer={}", peer_address);

  if (!btif_av_sink.Enabled()) {
    log::warn("BTIF AV Sink is not enabled");
    return BT_STATUS_NOT_READY;
  }

  BtifAvEvent btif_av_event(BTIF_AV_DISCONNECT_REQ_EVT, &peer_address, sizeof(peer_address));
  return do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                          AVDT_TSEP_SRC,  // peer_sep
                                          peer_address, kBtaHandleUnknown, btif_av_event));
}

bt_status_t btif_av_sink_set_active_device(const RawAddress& peer_address) {
  log::info("peer={}", peer_address);

  if (!btif_av_sink.Enabled()) {
    log::warn("BTIF AV Source is not enabled");
    return BT_STATUS_NOT_READY;
  }

  std::promise<void> peer_ready_promise;
  std::future<void> peer_ready_future = peer_ready_promise.get_future();
  bt_status_t status =
          do_in_main_thread(base::BindOnce(&set_active_peer_int,
                                           AVDT_TSEP_SRC,  // peer_sep
                                           peer_address, std::move(peer_ready_promise)));
  if (status == BT_STATUS_SUCCESS) {
    peer_ready_future.wait();
  } else {
    log::warn("BTIF AV Sink fails to change peer");
  }
  return status;
}

bt_status_t btif_av_source_set_silence_device(const RawAddress& peer_address, bool silence) {
  log::info("peer={} silence={}", peer_address, silence);

  if (!btif_av_source.Enabled()) {
    log::warn("BTIF AV Source is not enabled");
    return BT_STATUS_NOT_READY;
  }

  return do_in_main_thread(base::BindOnce(&set_source_silence_peer_int, peer_address, silence));
}

bt_status_t btif_av_source_set_active_device(const RawAddress& peer_address) {
  log::info("peer={}", peer_address);

  if (!btif_av_source.Enabled()) {
    log::warn("BTIF AV Source is not enabled");
    return BT_STATUS_NOT_READY;
  }

  std::promise<void> peer_ready_promise;
  std::future<void> peer_ready_future = peer_ready_promise.get_future();
  bt_status_t status =
          do_in_main_thread(base::BindOnce(&set_active_peer_int,
                                           AVDT_TSEP_SNK,  // peer_sep
                                           peer_address, std::move(peer_ready_promise)));
  if (status == BT_STATUS_SUCCESS) {
    peer_ready_future.wait();
  } else {
    log::warn("BTIF AV Source fails to change peer");
  }
  return status;
}

bt_status_t btif_av_source_set_codec_config_preference(
        const RawAddress& peer_address, std::vector<btav_a2dp_codec_config_t> codec_preferences) {
  log::info("peer={} codec_preferences=[{}]", peer_address, codec_preferences.size());

  if (!btif_av_source.Enabled()) {
    log::warn("BTIF AV Source is not enabled");
    return BT_STATUS_NOT_READY;
  }

  if (peer_address.IsEmpty()) {
    log::warn("BTIF AV Source needs peer to config");
    return BT_STATUS_PARM_INVALID;
  }

  std::promise<void> peer_ready_promise;
  std::future<void> peer_ready_future = peer_ready_promise.get_future();
  bt_status_t status = BT_STATUS_FAIL;

  if (com::android::bluetooth::flags::av_stream_reconfigure_fix()) {
    status = btif_av_source.SetPeerReconfigureStreamData(peer_address, codec_preferences,
                                                         std::move(peer_ready_promise));
    if (status != BT_STATUS_SUCCESS) {
      log::error("SetPeerReconfigureStreamData failed, status: {}", status);
      return status;
    }

    BtifAvEvent btif_av_event(BTIF_AV_RECONFIGURE_REQ_EVT, nullptr, 0);
    status = do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                              AVDT_TSEP_SNK,  // peer_sep
                                              peer_address, kBtaHandleUnknown, btif_av_event));
  } else {
    status = do_in_main_thread(base::BindOnce(&BtifAvSource::UpdateCodecConfig,
                                              base::Unretained(&btif_av_source), peer_address,
                                              codec_preferences, std::move(peer_ready_promise)));
  }

  if (status != BT_STATUS_SUCCESS) {
    log::error("do_in_main_thread failed, status: {}", status);
    return status;
  }

  if (peer_ready_future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    log::error("BTIF AV Source fails to config codec");
    return BT_STATUS_FAIL;
  }

  return status;
}

void btif_av_source_cleanup(void) {
  log::info("");
  do_in_main_thread(base::BindOnce(&BtifAvSource::Cleanup, base::Unretained(&btif_av_source)));
}

void btif_av_sink_cleanup(void) {
  log::info("");
  do_in_main_thread(base::BindOnce(&BtifAvSink::Cleanup, base::Unretained(&btif_av_sink)));
}

RawAddress btif_av_source_active_peer(void) { return btif_av_source.ActivePeer(); }
RawAddress btif_av_sink_active_peer(void) { return btif_av_sink.ActivePeer(); }

bool btif_av_is_sink_enabled(void) { return btif_av_sink.Enabled(); }

bool btif_av_is_source_enabled(void) { return btif_av_source.Enabled(); }

void btif_av_stream_start(const A2dpType /*local_a2dp_type*/) {
  log::info("");

  btif_av_source_dispatch_sm_event(btif_av_source_active_peer(), BTIF_AV_START_STREAM_REQ_EVT);
}

void btif_av_stream_start_with_latency(bool use_latency_mode) {
  log::info("peer={} use_latency_mode={}", btif_av_source_active_peer(), use_latency_mode);

  btif_av_start_stream_req_t start_stream_req;
  start_stream_req.use_latency_mode = use_latency_mode;
  BtifAvEvent btif_av_event(BTIF_AV_START_STREAM_REQ_EVT, &start_stream_req,
                            sizeof(start_stream_req));

  do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                   AVDT_TSEP_SNK,  // peer_sep
                                   btif_av_source_active_peer(), kBtaHandleUnknown, btif_av_event));
}

void btif_av_stream_stop(const RawAddress& peer_address) {
  log::info("peer={}", peer_address);

  if (!peer_address.IsEmpty()) {
    btif_av_source_dispatch_sm_event(peer_address, BTIF_AV_STOP_STREAM_REQ_EVT);
    return;
  }

  // The active peer might have changed and we might be in the process
  // of reconfiguring the stream. We need to stop the appropriate peer(s).
  btif_av_source.DispatchSuspendStreamEvent(BTIF_AV_STOP_STREAM_REQ_EVT);
}

void btif_av_stream_suspend(void) {
  log::info("");

  // The active peer might have changed and we might be in the process
  // of reconfiguring the stream. We need to suspend the appropriate peer(s).
  btif_av_source.DispatchSuspendStreamEvent(BTIF_AV_SUSPEND_STREAM_REQ_EVT);
}

void btif_av_stream_start_offload(void) {
  log::info("");

  btif_av_source_dispatch_sm_event(btif_av_source_active_peer(), BTIF_AV_OFFLOAD_START_REQ_EVT);
}

bool btif_av_stream_ready(const A2dpType local_a2dp_type) {
  // Make sure the main adapter is enabled
  if (btif_is_enabled() == 0) {
    log::verbose("Main adapter is not enabled");
    return false;
  }

  BtifAvPeer* peer = btif_av_find_active_peer(local_a2dp_type);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  int state = peer->StateMachine().StateId();
  log::info("active_peer={} state={} flags={}", peer->PeerAddress(), state, peer->FlagsToString());

  // check if we are remotely suspended or stop is pending
  if (peer->CheckFlags(BtifAvPeer::kFlagRemoteSuspend | BtifAvPeer::kFlagPendingStop)) {
    return false;
  }

  return state == BtifAvStateMachine::kStateOpened;
}

bool btif_av_stream_started_ready(const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_active_peer(local_a2dp_type);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  int state = peer->StateMachine().StateId();
  bool ready = false;
  if (peer->CheckFlags(BtifAvPeer::kFlagLocalSuspendPending | BtifAvPeer::kFlagRemoteSuspend |
                       BtifAvPeer::kFlagPendingStop)) {
    // Disallow media task to start if we have pending actions
    ready = false;
  } else {
    ready = (state == BtifAvStateMachine::kStateStarted);
  }

  log::info("active_peer={} state={} flags={} ready={}", peer->PeerAddress(), state,
            peer->FlagsToString(), ready);

  return ready;
}

static void btif_av_source_dispatch_sm_event(const RawAddress& peer_address,
                                             btif_av_sm_event_t event) {
  BtifAvEvent btif_av_event(event, nullptr, 0);
  log::verbose("peer={} event={}", peer_address, btif_av_event.ToString());

  do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                   AVDT_TSEP_SNK,  // peer_sep
                                   peer_address, kBtaHandleUnknown, btif_av_event));
}

static void btif_av_sink_dispatch_sm_event(const RawAddress& peer_address,
                                           btif_av_sm_event_t event) {
  BtifAvEvent btif_av_event(event, nullptr, 0);
  log::verbose("peer={} event={}", peer_address, btif_av_event.ToString());

  do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                   AVDT_TSEP_SRC,  // peer_sep
                                   peer_address, kBtaHandleUnknown, btif_av_event));
}

bt_status_t btif_av_source_execute_service(bool enable) {
  log::info("enable={}", enable);

  if (enable) {
    // Added BTA_AV_FEAT_NO_SCO_SSPD - this ensures that the BTA does not
    // auto-suspend av streaming on AG events(SCO or Call). The suspend shall
    // be initiated by the app/audioflinger layers.
    // Support for browsing for SDP record should work only if we enable
    // BROWSE while registering.
    tBTA_AV_FEAT features =
            BTA_AV_FEAT_RCTG | BTA_AV_FEAT_METADATA | BTA_AV_FEAT_VENDOR | BTA_AV_FEAT_NO_SCO_SSPD;

    if (delay_reporting_enabled()) {
      features |= BTA_AV_FEAT_DELAY_RPT;
    }

    if (avrcp_absolute_volume_is_enabled()) {
      features |= BTA_AV_FEAT_RCCT | BTA_AV_FEAT_ADV_CTRL | BTA_AV_FEAT_BROWSE;
    }

    if (btif_av_src_sink_coexist_enabled()) {
      features |= BTA_AV_FEAT_SRC;
      BTA_AvEnable(features, bta_av_event_callback);
    } else {
      BTA_AvEnable(features, bta_av_source_callback);
    }
    btif_av_source.RegisterAllBtaHandles();
    return BT_STATUS_SUCCESS;
  }

  // Disable the service
  btif_av_source.DeregisterAllBtaHandles();
  BTA_AvDisable();
  return BT_STATUS_SUCCESS;
}

bt_status_t btif_av_sink_execute_service(bool enable) {
  log::info("enable={}", enable);

  if (enable) {
    // Added BTA_AV_FEAT_NO_SCO_SSPD - this ensures that the BTA does not
    // auto-suspend AV streaming on AG events (SCO or Call). The suspend shall
    // be initiated by the app/audioflinger layers.
    tBTA_AV_FEAT features = BTA_AV_FEAT_NO_SCO_SSPD | BTA_AV_FEAT_RCCT | BTA_AV_FEAT_METADATA |
                            BTA_AV_FEAT_VENDOR | BTA_AV_FEAT_ADV_CTRL | BTA_AV_FEAT_RCTG |
                            BTA_AV_FEAT_BROWSE | BTA_AV_FEAT_COVER_ARTWORK;

    if (delay_reporting_enabled()) {
      features |= BTA_AV_FEAT_DELAY_RPT;
    }

    if (btif_av_src_sink_coexist_enabled()) {
      BTA_AvEnable(features, bta_av_event_callback);
    } else {
      BTA_AvEnable(features, bta_av_sink_callback);
    }
    btif_av_sink.RegisterAllBtaHandles();
    return BT_STATUS_SUCCESS;
  }

  // Disable the service
  btif_av_sink.DeregisterAllBtaHandles();
  BTA_AvDisable();
  return BT_STATUS_SUCCESS;
}

bool btif_av_is_connected(const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_active_peer(local_a2dp_type);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  bool connected = peer->IsConnected();
  log::verbose("active_peer={} connected={}", peer->PeerAddress(), connected);
  return peer->IsConnected();
}

uint8_t btif_av_get_peer_sep(const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_active_peer(local_a2dp_type);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return AVDT_TSEP_INVALID;
  }

  uint8_t peer_sep = peer->PeerSep();
  log::verbose("active_peer={} sep={}", peer->PeerAddress(),
               peer_sep == AVDT_TSEP_SRC ? "Source" : "Sink");
  return peer_sep;
}

void btif_av_clear_remote_suspend_flag(const A2dpType local_a2dp_type) {
  auto clear_remote_suspend_flag = [](const A2dpType local_a2dp_type) {
    BtifAvPeer* peer = btif_av_find_active_peer(local_a2dp_type);
    if (peer == nullptr) {
      log::warn("No active peer found");
      return;
    }
    log::verbose("active_peer={} flags={}", peer->PeerAddress(), peer->FlagsToString());
    peer->ClearFlags(BtifAvPeer::kFlagRemoteSuspend);
  };
  // switch to main thread to prevent a race condition of accessing peers
  do_in_main_thread(base::BindOnce(clear_remote_suspend_flag, local_a2dp_type));
}

bool btif_av_is_peer_edr(const RawAddress& peer_address, const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
  if (peer == nullptr) {
    log::warn("peer={} not found", peer_address);
    return false;
  }
  if (!peer->IsConnected()) {
    log::warn("peer={} not connected", peer_address);
    return false;
  }

  bool is_edr = peer->IsEdr();
  log::verbose("peer={} is_edr={}", peer_address, is_edr);
  return is_edr;
}

bool btif_av_peer_supports_3mbps(const RawAddress& peer_address, const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
  if (peer == nullptr) {
    log::warn("peer={} not found", peer_address);
    return false;
  }

  bool is_3mbps = peer->Is3Mbps();
  bool is_connected = peer->IsConnected();
  log::verbose("peer={} connected={}, edr_3mbps={}", peer_address, is_connected, is_3mbps);
  return is_connected && is_3mbps;
}

bool btif_av_peer_prefers_mandatory_codec(const RawAddress& peer_address,
                                          const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
  if (peer == nullptr) {
    log::warn("peer={} not found", peer_address);
    return false;
  }
  return peer->IsMandatoryCodecPreferred();
}

void btif_av_acl_disconnected(const RawAddress& peer_address, const A2dpType local_a2dp_type) {
  log::info("peer={}", peer_address);

  // Inform the application that ACL is disconnected and move to idle state
  if (btif_av_both_enable()) {
    BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
    if (peer != nullptr) {
      if (peer->IsSource()) {
        btif_av_sink_dispatch_sm_event(peer_address, BTIF_AV_ACL_DISCONNECTED);
      } else {
        btif_av_source_dispatch_sm_event(peer_address, BTIF_AV_ACL_DISCONNECTED);
      }
    }
    return;
  }

  if (btif_av_source.Enabled()) {
    btif_av_source_dispatch_sm_event(peer_address, BTIF_AV_ACL_DISCONNECTED);
  } else if (btif_av_sink.Enabled()) {
    btif_av_sink_dispatch_sm_event(peer_address, BTIF_AV_ACL_DISCONNECTED);
  }
}

static void btif_debug_av_peer_dump(int fd, const BtifAvPeer& peer) {
  std::string state_str;
  int state = peer.StateMachine().StateId();
  switch (state) {
    case BtifAvStateMachine::kStateIdle:
      state_str = "Idle";
      break;
    case BtifAvStateMachine::kStateOpening:
      state_str = "Opening";
      break;
    case BtifAvStateMachine::kStateOpened:
      state_str = "Opened";
      break;
    case BtifAvStateMachine::kStateStarted:
      state_str = "Started";
      break;
    case BtifAvStateMachine::kStateClosing:
      state_str = "Closing";
      break;
    default:
      state_str = "Unknown(" + std::to_string(state) + ")";
      break;
  }

  dprintf(fd, "  Peer: %s\n", ADDRESS_TO_LOGGABLE_CSTR(peer.PeerAddress()));
  dprintf(fd, "    Connected: %s\n", peer.IsConnected() ? "true" : "false");
  dprintf(fd, "    Streaming: %s\n", peer.IsStreaming() ? "true" : "false");
  dprintf(fd, "    SEP: %d(%s)\n", peer.PeerSep(), (peer.IsSource()) ? "Source" : "Sink");
  dprintf(fd, "    State Machine: %s\n", state_str.c_str());
  dprintf(fd, "    Flags: %s\n", peer.FlagsToString().c_str());
  dprintf(fd, "    OpenOnRcTimer: %s\n",
          alarm_is_scheduled(peer.AvOpenOnRcTimer()) ? "Scheduled" : "Not scheduled");
  dprintf(fd, "    BTA Handle: 0x%x\n", peer.BtaHandle());
  dprintf(fd, "    Peer ID: %d\n", peer.PeerId());
  dprintf(fd, "    EDR: %s\n", peer.IsEdr() ? "true" : "false");
  dprintf(fd, "    Support 3Mbps: %s\n", peer.Is3Mbps() ? "true" : "false");
  dprintf(fd, "    Self Initiated Connection: %s\n",
          peer.SelfInitiatedConnection() ? "true" : "false");
  dprintf(fd, "    Delay Reporting: %u (in 1/10 milliseconds) \n", peer.GetDelayReport());
  dprintf(fd, "    Codec Preferred: %s\n",
          peer.IsMandatoryCodecPreferred() ? "Mandatory" : "Optional");
}

static void btif_debug_av_source_dump(int fd) {
  bool enabled = btif_av_source.Enabled();

  dprintf(fd, "\nA2DP Source State: %s\n", (enabled) ? "Enabled" : "Disabled");
  if (!enabled) {
    return;
  }
  dprintf(fd, "  Active peer: %s\n", ADDRESS_TO_LOGGABLE_CSTR(btif_av_source.ActivePeer()));
  dprintf(fd, "  Peers:\n");
  btif_av_source.DumpPeersInfo(fd);
}

static void btif_debug_av_sink_dump(int fd) {
  bool enabled = btif_av_sink.Enabled();

  dprintf(fd, "\nA2DP Sink State: %s\n", (enabled) ? "Enabled" : "Disabled");
  if (!enabled) {
    return;
  }
  dprintf(fd, "  Active peer: %s\n", ADDRESS_TO_LOGGABLE_CSTR(btif_av_sink.ActivePeer()));
  dprintf(fd, "  Peers:\n");
  btif_av_sink.DumpPeersInfo(fd);
}

void btif_debug_av_dump(int fd) {
  btif_debug_av_source_dump(fd);
  btif_debug_av_sink_dump(fd);
}

void btif_av_set_audio_delay(const RawAddress& peer_address, uint16_t delay,
                             const A2dpType local_a2dp_type) {
  log::info("peer={} delay={}", peer_address, delay);

  BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
  if (peer != nullptr && peer->IsSink()) {
    peer->SetDelayReport(delay);
    if (peer->IsActivePeer()) {
      bluetooth::audio::a2dp::set_remote_delay(peer->GetDelayReport());
    }
  }
}

uint16_t btif_av_get_audio_delay(const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_active_peer(local_a2dp_type);
  if (peer != nullptr && peer->IsSink()) {
    return peer->GetDelayReport();
  }
  return 0;
}

bool btif_av_is_a2dp_offload_enabled() { return btif_av_source.A2dpOffloadEnabled(); }

bool btif_av_is_a2dp_offload_running() {
  if (!btif_av_is_a2dp_offload_enabled()) {
    return false;
  }
  if (!bluetooth::audio::a2dp::is_hal_enabled()) {
    return false;
  }
  return bluetooth::audio::a2dp::is_hal_offloading();
}

bool btif_av_is_peer_silenced(const RawAddress& peer_address) {
  return btif_av_source.IsPeerSilenced(peer_address);
}

void btif_av_set_dynamic_audio_buffer_size(uint8_t dynamic_audio_buffer_size) {
  btif_a2dp_source_set_dynamic_audio_buffer_size(dynamic_audio_buffer_size);
}

void btif_av_set_low_latency(bool is_low_latency) {
  log::info("active_peer={} is_low_latency={}", btif_av_source_active_peer(), is_low_latency);

  btif_av_set_latency_req_t set_latency_req;
  set_latency_req.is_low_latency = is_low_latency;
  BtifAvEvent btif_av_event(BTIF_AV_SET_LATENCY_REQ_EVT, &set_latency_req, sizeof(set_latency_req));

  do_in_main_thread(base::BindOnce(&btif_av_handle_event,
                                   AVDT_TSEP_SNK,  // peer_sep
                                   btif_av_source_active_peer(), kBtaHandleUnknown, btif_av_event));
}

bool btif_av_is_connected_addr(const RawAddress& peer_address, const A2dpType local_a2dp_type) {
  BtifAvPeer* peer = btif_av_find_peer(peer_address, local_a2dp_type);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  bool connected = peer->IsConnected();
  log::verbose("active_peer={} connected={}", peer->PeerAddress(), connected);
  return connected;
}

bool btif_av_peer_is_connected_sink(const RawAddress& peer_address) {
  BtifAvPeer* peer = btif_av_source_find_peer(peer_address);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  bool connected = peer->IsConnected();
  log::verbose("active_peer={} connected={}", peer->PeerAddress(), connected);
  return connected;
}

bool btif_av_peer_is_connected_source(const RawAddress& peer_address) {
  BtifAvPeer* peer = btif_av_sink_find_peer(peer_address);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  bool connected = peer->IsConnected();
  log::verbose("active_peer={} connected={}", peer->PeerAddress(), connected);
  return connected;
}

bool btif_av_peer_is_sink(const RawAddress& peer_address) {
  BtifAvPeer* peer = btif_av_source_find_peer(peer_address);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  return true;
}

bool btif_av_peer_is_source(const RawAddress& peer_address) {
  BtifAvPeer* peer = btif_av_sink_find_peer(peer_address);
  if (peer == nullptr) {
    log::warn("No active peer found");
    return false;
  }

  return true;
}

void btif_av_connect_sink_delayed(uint8_t handle, const RawAddress& peer_address) {
  log::info("peer={} handle=0x{:x}", peer_address, handle);

  if (btif_av_source.Enabled()) {
    btif_av_source_dispatch_sm_event(peer_address, BTIF_AV_AVRCP_OPEN_EVT);
  }
}
