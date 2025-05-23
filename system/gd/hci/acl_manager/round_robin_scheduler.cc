/*
 * Copyright 2020 The Android Open Source Project
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

#include "hci/acl_manager/round_robin_scheduler.h"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <memory>
#include <utility>

#include "hci/acl_manager/acl_fragmenter.h"
namespace bluetooth {
namespace hci {
namespace acl_manager {

RoundRobinScheduler::RoundRobinScheduler(os::Handler* handler, Controller* controller,
                                         common::BidiQueueEnd<AclBuilder, AclView>* hci_queue_end)
    : handler_(handler), controller_(controller), hci_queue_end_(hci_queue_end) {
  max_acl_packet_credits_ = controller_->GetNumAclPacketBuffers();
  acl_packet_credits_ = max_acl_packet_credits_;
  hci_mtu_ = controller_->GetAclPacketLength();
  LeBufferSize le_buffer_size = controller_->GetLeBufferSize();
  le_max_acl_packet_credits_ = le_buffer_size.total_num_le_packets_;
  le_acl_packet_credits_ = le_max_acl_packet_credits_;
  le_hci_mtu_ = le_buffer_size.le_data_packet_length_;
  controller_->RegisterCompletedAclPacketsCallback(
          handler->BindOn(this, &RoundRobinScheduler::incoming_acl_credits));
}

RoundRobinScheduler::~RoundRobinScheduler() {
  unregister_all_connections();
  controller_->UnregisterCompletedAclPacketsCallback();
}

void RoundRobinScheduler::Register(ConnectionType connection_type, uint16_t handle,
                                   std::shared_ptr<acl_manager::AclConnection::Queue> queue) {
  log::assert_that(acl_queue_handlers_.count(handle) == 0,
                   "assert failed: acl_queue_handlers_.count(handle) == 0");
  acl_queue_handler acl_queue_handler = {connection_type, std::move(queue), false, 0};
  acl_queue_handlers_.insert(
          std::pair<uint16_t, RoundRobinScheduler::acl_queue_handler>(handle, acl_queue_handler));
  log::info("registering acl_queue handle={}, acl_credits={}, le_credits={}", handle,
            acl_packet_credits_, le_acl_packet_credits_);
  if (fragments_to_send_.size() == 0) {
    log::info("start round robin");
    start_round_robin();
  }
}

void RoundRobinScheduler::Unregister(uint16_t handle) {
  log::assert_that(acl_queue_handlers_.count(handle) == 1,
                   "assert failed: acl_queue_handlers_.count(handle) == 1");

  if (com::android::bluetooth::flags::drop_acl_fragment_on_disconnect()) {
    // Drop the pending fragments and recalculate number_of_sent_packets_
    drop_packet_fragments(handle);
  }

  auto& acl_queue_handler = acl_queue_handlers_.find(handle)->second;
  log::info("unregistering acl_queue handle={}, sent_packets={}", handle,
            acl_queue_handler.number_of_sent_packets_);

  bool credits_reclaimed_from_zero = acl_queue_handler.number_of_sent_packets_ > 0;

  // Reclaim outstanding packets
  if (acl_queue_handler.connection_type_ == ConnectionType::CLASSIC) {
    credits_reclaimed_from_zero &= (acl_packet_credits_ == 0);
    acl_packet_credits_ += acl_queue_handler.number_of_sent_packets_;
  } else {
    credits_reclaimed_from_zero &= (le_acl_packet_credits_ == 0);
    le_acl_packet_credits_ += acl_queue_handler.number_of_sent_packets_;
  }
  acl_queue_handler.number_of_sent_packets_ = 0;

  if (acl_queue_handler.dequeue_is_registered_) {
    acl_queue_handler.dequeue_is_registered_ = false;
    acl_queue_handler.queue_->GetDownEnd()->UnregisterDequeue();
  }
  acl_queue_handlers_.erase(handle);
  starting_point_ = acl_queue_handlers_.begin();

  // Restart sending packets if we got acl credits
  if (com::android::bluetooth::flags::drop_acl_fragment_on_disconnect() &&
      credits_reclaimed_from_zero) {
    start_round_robin();
  }
}

void RoundRobinScheduler::SetLinkPriority(uint16_t handle, bool high_priority) {
  auto acl_queue_handler = acl_queue_handlers_.find(handle);
  if (acl_queue_handler == acl_queue_handlers_.end()) {
    log::warn("handle {} is invalid", handle);
    return;
  }
  acl_queue_handler->second.high_priority_ = high_priority;
}

uint16_t RoundRobinScheduler::GetCredits() { return acl_packet_credits_; }

uint16_t RoundRobinScheduler::GetLeCredits() { return le_acl_packet_credits_; }

void RoundRobinScheduler::start_round_robin() {
  if (acl_packet_credits_ == 0 && le_acl_packet_credits_ == 0) {
    log::warn("Both buffers are full");
    return;
  }
  if (!fragments_to_send_.empty()) {
    auto connection_type = fragments_to_send_.front().connection_type_;
    bool classic_buffer_full =
            acl_packet_credits_ == 0 && connection_type == ConnectionType::CLASSIC;
    bool le_buffer_full = le_acl_packet_credits_ == 0 && connection_type == ConnectionType::LE;
    if (classic_buffer_full || le_buffer_full) {
      log::warn("Buffer of connection_type {} is full", connection_type);
      return;
    }
    send_next_fragment();
    return;
  }
  if (acl_queue_handlers_.empty()) {
    log::info("No any acl connection");
    return;
  }

  if (acl_queue_handlers_.size() == 1 || starting_point_ == acl_queue_handlers_.end()) {
    starting_point_ = acl_queue_handlers_.begin();
  }
  size_t count = acl_queue_handlers_.size();

  for (auto acl_queue_handler = starting_point_; count > 0; count--) {
    // Prevent registration when credits is zero
    bool classic_buffer_full =
            acl_packet_credits_ == 0 &&
            acl_queue_handler->second.connection_type_ == ConnectionType::CLASSIC;
    bool le_buffer_full = le_acl_packet_credits_ == 0 &&
                          acl_queue_handler->second.connection_type_ == ConnectionType::LE;
    if (!acl_queue_handler->second.dequeue_is_registered_ && !classic_buffer_full &&
        !le_buffer_full) {
      acl_queue_handler->second.dequeue_is_registered_ = true;
      uint16_t acl_handle = acl_queue_handler->first;
      acl_queue_handler->second.queue_->GetDownEnd()->RegisterDequeue(
              handler_, common::Bind(&RoundRobinScheduler::buffer_packet, common::Unretained(this),
                                     acl_handle));
    }
    acl_queue_handler = std::next(acl_queue_handler);
    if (acl_queue_handler == acl_queue_handlers_.end()) {
      acl_queue_handler = acl_queue_handlers_.begin();
    }
  }

  starting_point_ = std::next(starting_point_);
}

void RoundRobinScheduler::buffer_packet(uint16_t acl_handle) {
  BroadcastFlag broadcast_flag = BroadcastFlag::POINT_TO_POINT;
  auto acl_queue_handler = acl_queue_handlers_.find(acl_handle);
  if (acl_queue_handler == acl_queue_handlers_.end()) {
    log::error("Ignore since ACL connection vanished with handle: 0x{:X}", acl_handle);
    return;
  }

  // Wrap packet and enqueue it
  uint16_t handle = acl_queue_handler->first;
  auto packet = acl_queue_handler->second.queue_->GetDownEnd()->TryDequeue();
  log::assert_that(packet != nullptr, "assert failed: packet != nullptr");

  ConnectionType connection_type = acl_queue_handler->second.connection_type_;
  size_t mtu = connection_type == ConnectionType::CLASSIC ? hci_mtu_ : le_hci_mtu_;
  PacketBoundaryFlag packet_boundary_flag =
          (packet->IsFlushable()) ? PacketBoundaryFlag::FIRST_AUTOMATICALLY_FLUSHABLE
                                  : PacketBoundaryFlag::FIRST_NON_AUTOMATICALLY_FLUSHABLE;

  int acl_priority = acl_queue_handler->second.high_priority_ ? 1 : 0;
  if (packet->size() <= mtu) {
    fragments_to_send_.push(packet_fragment{connection_type, handle, acl_priority,
                                            AclBuilder::Create(handle, packet_boundary_flag,
                                                               broadcast_flag, std::move(packet))},
                            acl_priority);
  } else {
    auto fragments = AclFragmenter(mtu, std::move(packet)).GetFragments();
    for (size_t i = 0; i < fragments.size(); i++) {
      fragments_to_send_.push(
              packet_fragment{connection_type, handle, acl_priority,
                              AclBuilder::Create(handle, packet_boundary_flag, broadcast_flag,
                                                 std::move(fragments[i]))},
              acl_priority);
      packet_boundary_flag = PacketBoundaryFlag::CONTINUING_FRAGMENT;
    }
  }
  log::assert_that(fragments_to_send_.size() > 0, "assert failed: fragments_to_send_.size() > 0");
  unregister_all_connections();

  acl_queue_handler->second.number_of_sent_packets_ += fragments_to_send_.size();
  send_next_fragment();
}

// Drops packet fragments associated with the given handle.
void RoundRobinScheduler::drop_packet_fragments(uint16_t acl_handle) {
  if (fragments_to_send_.empty()) {
    return;
  }
  auto acl_queue_handler = acl_queue_handlers_.find(acl_handle);

  decltype(fragments_to_send_) new_fragments_to_send;
  while (!fragments_to_send_.empty()) {
    auto& fragment = fragments_to_send_.front();

    if (fragment.handle_ == acl_handle) {
      // This fragment is not sent to the controller.
      acl_queue_handler->second.number_of_sent_packets_--;
    } else {
      new_fragments_to_send.push(packet_fragment{fragment.connection_type_, fragment.handle_,
                                                 fragment.priority_, std::move(fragment.packet_)},
                                 fragment.priority_);
    }
    fragments_to_send_.pop();
  }

  if (new_fragments_to_send.empty()) {
    if (enqueue_registered_.exchange(false)) {
      hci_queue_end_->UnregisterEnqueue();
    }
  }
  fragments_to_send_.swap(new_fragments_to_send);
}

void RoundRobinScheduler::unregister_all_connections() {
  for (auto acl_queue_handler = acl_queue_handlers_.begin();
       acl_queue_handler != acl_queue_handlers_.end();
       acl_queue_handler = std::next(acl_queue_handler)) {
    if (acl_queue_handler->second.dequeue_is_registered_) {
      acl_queue_handler->second.dequeue_is_registered_ = false;
      acl_queue_handler->second.queue_->GetDownEnd()->UnregisterDequeue();
    }
  }
}

void RoundRobinScheduler::send_next_fragment() {
  if (!enqueue_registered_.exchange(true)) {
    hci_queue_end_->RegisterEnqueue(handler_,
                                    common::Bind(&RoundRobinScheduler::handle_enqueue_next_fragment,
                                                 common::Unretained(this)));
  }
}

// Invoked from some external Queue Reactable context 1
std::unique_ptr<AclBuilder> RoundRobinScheduler::handle_enqueue_next_fragment() {
  ConnectionType connection_type = fragments_to_send_.front().connection_type_;

  if (connection_type == ConnectionType::CLASSIC) {
    log::assert_that(acl_packet_credits_ > 0, "assert failed: acl_packet_credits_ > 0");
    acl_packet_credits_ -= 1;
  } else {
    log::assert_that(le_acl_packet_credits_ > 0, "assert failed: le_acl_packet_credits_ > 0");
    le_acl_packet_credits_ -= 1;
  }

  auto raw_pointer = fragments_to_send_.front().packet_.release();
  fragments_to_send_.pop();
  if (fragments_to_send_.empty()) {
    if (enqueue_registered_.exchange(false)) {
      hci_queue_end_->UnregisterEnqueue();
    }
    handler_->Post(
            common::BindOnce(&RoundRobinScheduler::start_round_robin, common::Unretained(this)));
  } else {
    ConnectionType next_connection_type = fragments_to_send_.front().connection_type_;
    bool classic_buffer_full =
            next_connection_type == ConnectionType::CLASSIC && acl_packet_credits_ == 0;
    bool le_buffer_full = next_connection_type == ConnectionType::LE && le_acl_packet_credits_ == 0;
    if ((classic_buffer_full || le_buffer_full) && enqueue_registered_.exchange(false)) {
      hci_queue_end_->UnregisterEnqueue();
    }
  }
  return std::unique_ptr<AclBuilder>(raw_pointer);
}

void RoundRobinScheduler::incoming_acl_credits(uint16_t handle, uint16_t credits) {
  auto acl_queue_handler = acl_queue_handlers_.find(handle);
  if (acl_queue_handler == acl_queue_handlers_.end()) {
    return;
  }

  if (acl_queue_handler->second.number_of_sent_packets_ >= credits) {
    acl_queue_handler->second.number_of_sent_packets_ -= credits;
  } else {
    log::warn("receive more credits than we sent");
    acl_queue_handler->second.number_of_sent_packets_ = 0;
  }

  bool credit_was_zero = false;
  if (acl_queue_handler->second.connection_type_ == ConnectionType::CLASSIC) {
    if (acl_packet_credits_ == 0) {
      credit_was_zero = true;
    }
    acl_packet_credits_ += credits;
    if (acl_packet_credits_ > max_acl_packet_credits_) {
      acl_packet_credits_ = max_acl_packet_credits_;
      log::warn("acl packet credits overflow due to receive {} credits", credits);
    }
  } else {
    if (le_acl_packet_credits_ == 0) {
      credit_was_zero = true;
    }
    le_acl_packet_credits_ += credits;
    if (le_acl_packet_credits_ > le_max_acl_packet_credits_) {
      le_acl_packet_credits_ = le_max_acl_packet_credits_;
      log::warn("le acl packet credits overflow due to receive {} credits", credits);
    }
  }
  if (credit_was_zero) {
    start_round_robin();
  }
}

}  // namespace acl_manager
}  // namespace hci
}  // namespace bluetooth
