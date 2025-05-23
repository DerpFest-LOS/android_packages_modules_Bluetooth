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

#include <bluetooth/log.h>
#include <grpc++/grpc++.h>

#include <atomic>
#include <chrono>
#include <utility>

#include "blueberry/facade/common.pb.h"
#include "common/blocking_queue.h"

namespace bluetooth {
namespace grpc {

template <typename T>
class GrpcEventQueue {
public:
  /**
   * Create a GrpcEventQueue that can be used to shuffle event from one thread to another
   * @param log_name
   */
  explicit GrpcEventQueue(std::string log_name) : log_name_(std::move(log_name)) {}

  /**
   * Run the event loop and blocks until client cancels the stream request
   * Event queue will be cleared before entering the loop. Hence, only events occurred after gRPC
   * request will be delivered to the user. Hence user is advised to run the loop before generating
   * pending events.
   *
   * @param context client context
   * @param writer output writer
   * @return gRPC status
   */
  ::grpc::Status RunLoop(::grpc::ServerContext* context, ::grpc::ServerWriter<T>* writer) {
    using namespace std::chrono_literals;
    log::info("{}: Entering Loop", log_name_);
    while (!context->IsCancelled()) {
      // Wait for 100 ms so that cancellation can be caught in amortized 50 ms latency
      if (pending_events_.wait_to_take(100ms)) {
        log::info("{}: Got event from queue", log_name_);
        writer->Write(pending_events_.take());
      }
    }
    running_ = false;
    log::info("{}: Exited Loop", log_name_);
    return ::grpc::Status::OK;
  }

  /**
   * Called when there is an incoming event
   * @param event incoming event
   */
  void OnIncomingEvent(T event) {
    if (!running_) {
      log::info("{}: Discarding an event while not running the loop", log_name_);
      return;
    }
    log::info("{}: Got event, enqueuing", log_name_);
    pending_events_.push(std::move(event));
  }

private:
  std::string log_name_;
  std::atomic<bool> running_{true};
  common::BlockingQueue<T> pending_events_;
};

}  // namespace grpc
}  // namespace bluetooth
