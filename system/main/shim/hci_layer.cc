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

#define LOG_TAG "bt_shim_hci"

#include "main/shim/hci_layer.h"

#include <base/functional/bind.h>
#include <bluetooth/log.h>

#include <algorithm>
#include <cstdint>

#include "common/bidi_queue.h"
#include "hci/hci_interface.h"
#include "hci/hci_packets.h"
#include "hci/include/packet_fragmenter.h"
#include "main/shim/entry.h"
#include "osi/include/allocator.h"
#include "packet/raw_builder.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_types.h"
#include "stack/include/btm_iso_api.h"
#include "stack/include/hcimsgs.h"
#include "stack/include/main_thread.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using namespace bluetooth;

/**
 * Callback data wrapped as opaque token bundled with the command
 * transmit request to the Gd layer.
 *
 * Upon completion a token for a corresponding command transmit.
 * request is returned from the Gd layer.
 */
using CommandCallbackData = struct {
  void* context;
};

constexpr size_t kBtHdrSize = sizeof(BT_HDR);
constexpr size_t kCommandLengthSize = sizeof(uint8_t);
constexpr size_t kCommandOpcodeSize = sizeof(uint16_t);

static base::Callback<void(BT_HDR*)> send_data_upwards;
static const packet_fragmenter_t* packet_fragmenter;

namespace {
bool register_event_code(bluetooth::hci::EventCode event_code) {
  switch (event_code) {
    // SCO
    case bluetooth::hci::EventCode::SYNCHRONOUS_CONNECTION_COMPLETE:
    case bluetooth::hci::EventCode::SYNCHRONOUS_CONNECTION_CHANGED:

    // SecurityEvents
    case bluetooth::hci::EventCode::ENCRYPTION_CHANGE:
    case bluetooth::hci::EventCode::PIN_CODE_REQUEST:
    case bluetooth::hci::EventCode::LINK_KEY_REQUEST:
    case bluetooth::hci::EventCode::LINK_KEY_NOTIFICATION:
    case bluetooth::hci::EventCode::ENCRYPTION_KEY_REFRESH_COMPLETE:
    case bluetooth::hci::EventCode::IO_CAPABILITY_REQUEST:
    case bluetooth::hci::EventCode::IO_CAPABILITY_RESPONSE:
    case bluetooth::hci::EventCode::REMOTE_OOB_DATA_REQUEST:
    case bluetooth::hci::EventCode::SIMPLE_PAIRING_COMPLETE:
    case bluetooth::hci::EventCode::USER_PASSKEY_NOTIFICATION:
    case bluetooth::hci::EventCode::USER_CONFIRMATION_REQUEST:
    case bluetooth::hci::EventCode::USER_PASSKEY_REQUEST:
    case bluetooth::hci::EventCode::ENCRYPTION_CHANGE_V2:
      return true;
    default:
      return false;
  }
}

static bool register_subevent_code(bluetooth::hci::SubeventCode subevent_code) {
  switch (subevent_code) {
    case bluetooth::hci::SubeventCode::READ_REMOTE_FEATURES_COMPLETE:
    case bluetooth::hci::SubeventCode::LONG_TERM_KEY_REQUEST:
    case bluetooth::hci::SubeventCode::READ_LOCAL_P256_PUBLIC_KEY_COMPLETE:
    case bluetooth::hci::SubeventCode::GENERATE_DHKEY_COMPLETE:
    case bluetooth::hci::SubeventCode::CHANNEL_SELECTION_ALGORITHM:
    case bluetooth::hci::SubeventCode::CONNECTIONLESS_IQ_REPORT:
    case bluetooth::hci::SubeventCode::CONNECTION_IQ_REPORT:
    case bluetooth::hci::SubeventCode::CTE_REQUEST_FAILED:
    case bluetooth::hci::SubeventCode::CIS_ESTABLISHED:
    case bluetooth::hci::SubeventCode::CIS_REQUEST:
    case bluetooth::hci::SubeventCode::CREATE_BIG_COMPLETE:
    case bluetooth::hci::SubeventCode::TERMINATE_BIG_COMPLETE:
    case bluetooth::hci::SubeventCode::BIG_SYNC_ESTABLISHED:
    case bluetooth::hci::SubeventCode::BIG_SYNC_LOST:
    case bluetooth::hci::SubeventCode::REQUEST_PEER_SCA_COMPLETE:
    case bluetooth::hci::SubeventCode::PATH_LOSS_THRESHOLD:
      return true;
    default:
      return false;
  }
}

}  // namespace

namespace cpp {
bluetooth::common::BidiQueueEnd<bluetooth::hci::IsoBuilder, bluetooth::hci::IsoView>*
        hci_iso_queue_end = nullptr;
static bluetooth::os::EnqueueBuffer<bluetooth::hci::IsoBuilder>* pending_iso_data = nullptr;

static std::unique_ptr<bluetooth::packet::RawBuilder> MakeUniquePacket(const uint8_t* data,
                                                                       size_t len) {
  bluetooth::packet::RawBuilder builder;
  std::vector<uint8_t> bytes(data, data + len);

  auto payload = std::make_unique<bluetooth::packet::RawBuilder>();
  payload->AddOctets(bytes);

  return payload;
}

static BT_HDR* WrapPacketAndCopy(uint16_t event,
                                 bluetooth::hci::PacketView<bluetooth::hci::kLittleEndian>* data) {
  size_t packet_size = data->size() + kBtHdrSize;
  BT_HDR* packet = reinterpret_cast<BT_HDR*>(osi_malloc(packet_size));
  packet->offset = 0;
  packet->len = data->size();
  packet->layer_specific = 0;
  packet->event = event;
  std::copy(data->begin(), data->end(), packet->data);
  return packet;
}

static void event_callback(bluetooth::hci::EventView event_packet_view) {
  if (!send_data_upwards) {
    return;
  }
  send_data_upwards.Run(WrapPacketAndCopy(MSG_HC_TO_STACK_HCI_EVT, &event_packet_view));
}

static void subevent_callback(bluetooth::hci::LeMetaEventView le_meta_event_view) {
  if (!send_data_upwards) {
    return;
  }
  send_data_upwards.Run(WrapPacketAndCopy(MSG_HC_TO_STACK_HCI_EVT, &le_meta_event_view));
}

void OnTransmitPacketCommandComplete(command_complete_cb complete_callback, void* context,
                                     bluetooth::hci::CommandCompleteView view) {
  log::debug("Received cmd complete for {}", bluetooth::hci::OpCodeText(view.GetCommandOpCode()));
  BT_HDR* response = WrapPacketAndCopy(MSG_HC_TO_STACK_HCI_EVT, &view);
  complete_callback(response, context);
}

void OnTransmitPacketStatus(command_status_cb status_callback, void* context,
                            std::unique_ptr<OsiObject> command,
                            bluetooth::hci::CommandStatusView view) {
  log::debug("Received cmd status {} for {}", bluetooth::hci::ErrorCodeText(view.GetStatus()),
             bluetooth::hci::OpCodeText(view.GetCommandOpCode()));
  uint8_t status = static_cast<uint8_t>(view.GetStatus());
  status_callback(status, static_cast<BT_HDR*>(command->Release()), context);
}

static void transmit_command(const BT_HDR* command, command_complete_cb complete_callback,
                             command_status_cb status_callback, void* context) {
  log::assert_that(command != nullptr, "assert failed: command != nullptr");
  const uint8_t* data = command->data + command->offset;
  size_t len = command->len;
  log::assert_that(len >= (kCommandOpcodeSize + kCommandLengthSize),
                   "assert failed: len >= (kCommandOpcodeSize + kCommandLengthSize)");

  // little endian command opcode
  uint16_t command_op_code = (data[1] << 8 | data[0]);
  // Gd stack API requires opcode specification and calculates length, so
  // no need to provide opcode or length here.
  data += (kCommandOpcodeSize + kCommandLengthSize);
  len -= (kCommandOpcodeSize + kCommandLengthSize);

  auto op_code = static_cast<const bluetooth::hci::OpCode>(command_op_code);

  auto payload = MakeUniquePacket(data, len);
  auto packet = bluetooth::hci::CommandBuilder::Create(op_code, std::move(payload));

  log::debug("Sending command {}", bluetooth::hci::OpCodeText(op_code));

  if (bluetooth::hci::Checker::IsCommandStatusOpcode(op_code)) {
    auto command_unique = std::make_unique<OsiObject>(command);
    bluetooth::shim::GetHciLayer()->EnqueueCommand(
            std::move(packet),
            bluetooth::shim::GetGdShimHandler()->BindOnce(OnTransmitPacketStatus, status_callback,
                                                          context, std::move(command_unique)));
  } else {
    bluetooth::shim::GetHciLayer()->EnqueueCommand(
            std::move(packet),
            bluetooth::shim::GetGdShimHandler()->BindOnce(OnTransmitPacketCommandComplete,
                                                          complete_callback, context));
    osi_free(const_cast<void*>(static_cast<const void*>(command)));
  }
}

static void transmit_iso_fragment(const uint8_t* stream, size_t length) {
  uint16_t handle_with_flags;
  STREAM_TO_UINT16(handle_with_flags, stream);
  auto pb_flag = static_cast<bluetooth::hci::IsoPacketBoundaryFlag>(handle_with_flags >> 12 & 0b11);
  auto ts_flag = static_cast<bluetooth::hci::TimeStampFlag>(handle_with_flags >> 14);
  uint16_t handle = HCID_GET_HANDLE(handle_with_flags);
  log::assert_that(handle <= HCI_HANDLE_MAX, "Require handle <= 0x{:X}, but is 0x{:X}",
                   HCI_HANDLE_MAX, handle);
  length -= 2;
  // skip data total length
  stream += 2;
  length -= 2;
  auto payload = MakeUniquePacket(stream, length);
  auto iso_packet =
          bluetooth::hci::IsoBuilder::Create(handle, pb_flag, ts_flag, std::move(payload));

  pending_iso_data->Enqueue(std::move(iso_packet), bluetooth::shim::GetGdShimHandler());
}

static void register_event(bluetooth::hci::EventCode event_code) {
  auto handler = bluetooth::shim::GetGdShimHandler();
  bluetooth::shim::GetHciLayer()->RegisterEventHandler(event_code, handler->Bind(event_callback));
}

static void register_le_event(bluetooth::hci::SubeventCode subevent_code) {
  auto handler = bluetooth::shim::GetGdShimHandler();
  bluetooth::shim::GetHciLayer()->RegisterLeEventHandler(subevent_code,
                                                         handler->Bind(subevent_callback));
}

static void iso_data_callback() {
  if (hci_iso_queue_end == nullptr) {
    return;
  }
  auto packet = hci_iso_queue_end->TryDequeue();
  log::assert_that(packet != nullptr, "assert failed: packet != nullptr");
  if (!packet->IsValid()) {
    log::info("Dropping invalid packet of size {}", packet->size());
    return;
  }
  if (!send_data_upwards) {
    return;
  }
  auto data = WrapPacketAndCopy(MSG_HC_TO_STACK_HCI_ISO, packet.get());
  packet_fragmenter->reassemble_and_dispatch(data);
}

static void register_for_iso() {
  hci_iso_queue_end = bluetooth::shim::GetHciLayer()->GetIsoQueueEnd();
  hci_iso_queue_end->RegisterDequeue(bluetooth::shim::GetGdShimHandler(),
                                     bluetooth::common::Bind(iso_data_callback));
  pending_iso_data =
          new bluetooth::os::EnqueueBuffer<bluetooth::hci::IsoBuilder>(hci_iso_queue_end);
  // Register ISO for disconnect notifications
  bluetooth::shim::GetHciLayer()->RegisterForDisconnects(
          get_main_thread()->Bind([](uint16_t handle, bluetooth::hci::ErrorCode error_code) {
            auto iso = bluetooth::hci::IsoManager::GetInstance();
            if (iso) {
              auto reason = static_cast<uint8_t>(error_code);
              log::info("ISO disconnection from GD, handle: 0x{:02x}, reason: 0x{:02x}", handle,
                        reason);
              iso->HandleDisconnect(handle, reason);
            }
          }));
}

static void on_shutting_down() {
  if (pending_iso_data != nullptr) {
    pending_iso_data->Clear();
    delete pending_iso_data;
    pending_iso_data = nullptr;
  }
  if (hci_iso_queue_end != nullptr) {
    hci_iso_queue_end->UnregisterDequeue();
    hci_iso_queue_end = nullptr;
  }
}

}  // namespace cpp

using bluetooth::common::Bind;
using bluetooth::common::BindOnce;
using bluetooth::common::Unretained;

static void set_data_cb(base::Callback<void(BT_HDR*)> send_data_cb) {
  send_data_upwards = std::move(send_data_cb);
}

static void transmit_command(const BT_HDR* command, command_complete_cb complete_callback,
                             command_status_cb status_callback, void* context) {
  cpp::transmit_command(command, complete_callback, status_callback, context);
}

static void transmit_fragment(BT_HDR* packet, bool send_transmit_finished) {
  uint16_t event = packet->event & MSG_EVT_MASK;

  // HCI command packets are freed on a different thread when the matching
  // event is received. Check packet->event before sending to avoid a race.
  bool free_after_transmit = event != MSG_STACK_TO_HC_HCI_CMD && send_transmit_finished;

  if (event == MSG_STACK_TO_HC_HCI_ISO) {
    const uint8_t* stream = packet->data + packet->offset;
    size_t length = packet->len;
    cpp::transmit_iso_fragment(stream, length);
  }

  if (free_after_transmit) {
    osi_free(packet);
  }
}
static void dispatch_reassembled(BT_HDR* packet) {
  // Only ISO should be handled here
  log::assert_that((packet->event & MSG_EVT_MASK) == MSG_HC_TO_STACK_HCI_ISO,
                   "assert failed: (packet->event & MSG_EVT_MASK) == "
                   "MSG_HC_TO_STACK_HCI_ISO");
  log::assert_that(!send_data_upwards.is_null(), "assert failed: !send_data_upwards.is_null()");
  send_data_upwards.Run(packet);
}

static const packet_fragmenter_callbacks_t packet_fragmenter_callbacks = {transmit_fragment,
                                                                          dispatch_reassembled};

static void transmit_downward(void* raw_data, uint16_t iso_buffer_size) {
  bluetooth::shim::GetGdShimHandler()->Call(packet_fragmenter->fragment_and_dispatch,
                                            static_cast<BT_HDR*>(raw_data), iso_buffer_size);
}

static hci_t interface = {.set_data_cb = set_data_cb,
                          .transmit_command = transmit_command,
                          .transmit_downward = transmit_downward};

const hci_t* bluetooth::shim::hci_layer_get_interface() {
  packet_fragmenter = packet_fragmenter_get_interface();
  packet_fragmenter->init(&packet_fragmenter_callbacks);
  return &interface;
}

void bluetooth::shim::hci_on_reset_complete() {
  log::assert_that(!send_data_upwards.is_null(), "assert failed: !send_data_upwards.is_null()");

  for (uint16_t event_code_raw = 0; event_code_raw < 0x100; event_code_raw++) {
    auto event_code = static_cast<bluetooth::hci::EventCode>(event_code_raw);
    if (!register_event_code(event_code)) {
      continue;
    }
    cpp::register_event(event_code);
  }

  for (uint16_t subevent_code_raw = 0; subevent_code_raw < 0x100; subevent_code_raw++) {
    auto subevent_code = static_cast<bluetooth::hci::SubeventCode>(subevent_code_raw);
    if (!register_subevent_code(subevent_code)) {
      continue;
    }
    cpp::register_le_event(subevent_code);
  }

  cpp::register_for_iso();
}

void bluetooth::shim::hci_on_shutting_down() { cpp::on_shutting_down(); }
