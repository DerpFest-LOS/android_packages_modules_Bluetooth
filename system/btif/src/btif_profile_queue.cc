/******************************************************************************
 *
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

/*******************************************************************************
 *
 *  Filename:      btif_profile_queue.c
 *
 *  Description:   Bluetooth remote device connection queuing implementation.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_queue"

#include "btif_profile_queue.h"

#include <base/functional/bind.h>
#include <base/functional/callback.h>
#include <bluetooth/log.h>
#include <string.h>

#include <cstdint>
#include <list>
#include <string>

#include "btif/include/stack_manager_t.h"
#include "btif_common.h"
#include "hardware/bluetooth.h"
#include "types/raw_address.h"

using namespace bluetooth;

/*******************************************************************************
 *  Local type definitions
 ******************************************************************************/

// Class to store connect info.
class ConnectNode {
public:
  ConnectNode(const RawAddress& address, uint16_t uuid, btif_connect_cb_t connect_cb)
      : address_(address), uuid_(uuid), busy_(false), connect_cb_(connect_cb) {}

  std::string ToString() const {
    return std::format("address={} UUID={:04X} busy={}", address_, uuid_, busy_);
  }

  const RawAddress& address() const { return address_; }
  uint16_t uuid() const { return uuid_; }

  /**
   * Initiate the connection.
   *
   * @return BT_STATUS_SUCCESS on success, othewise the corresponding error
   * code. Note: if a previous connect request hasn't been completed, the
   * return value is BT_STATUS_SUCCESS.
   */
  bt_status_t connect() {
    if (busy_) {
      return BT_STATUS_SUCCESS;
    }
    busy_ = true;
    return connect_cb_(&address_, uuid_);
  }

private:
  RawAddress address_;
  uint16_t uuid_;
  bool busy_;
  btif_connect_cb_t connect_cb_;
};

/*******************************************************************************
 *  Static variables
 ******************************************************************************/

static std::list<ConnectNode> connect_queue;

static const size_t MAX_REASONABLE_REQUESTS = 20;

/*******************************************************************************
 *  Queue helper functions
 ******************************************************************************/

static void queue_int_add(uint16_t uuid, const RawAddress& bda, btif_connect_cb_t connect_cb) {
  // Sanity check to make sure we're not leaking connection requests
  log::assert_that(connect_queue.size() < MAX_REASONABLE_REQUESTS,
                   "assert failed: connect_queue.size() < MAX_REASONABLE_REQUESTS");

  ConnectNode param(bda, uuid, connect_cb);
  for (const auto& node : connect_queue) {
    if (node.uuid() == param.uuid() && node.address() == param.address()) {
      log::error("Dropping duplicate profile connection request:{}", param.ToString());
      return;
    }
  }

  log::info("Queueing profile connection request:{}", param.ToString());
  connect_queue.push_back(param);

  btif_queue_connect_next();
}

static void queue_int_advance() {
  if (connect_queue.empty()) {
    return;
  }

  const ConnectNode& head = connect_queue.front();
  log::info("removing connection request: {}", head.ToString());
  connect_queue.pop_front();

  btif_queue_connect_next();
}

static void queue_int_cleanup(uint16_t uuid) {
  log::info("UUID={:04X}", uuid);

  for (auto it = connect_queue.begin(); it != connect_queue.end();) {
    auto it_prev = it++;
    const ConnectNode& node = *it_prev;
    if (node.uuid() == uuid) {
      log::info("removing connection request: {}", node.ToString());
      connect_queue.erase(it_prev);
    }
  }
}

static void queue_int_release() { connect_queue.clear(); }

/*******************************************************************************
 *
 * Function         btif_queue_connect
 *
 * Description      Add a new connection to the queue and trigger the next
 *                  scheduled connection.
 *
 * Returns          BT_STATUS_SUCCESS if successful
 *
 ******************************************************************************/
bt_status_t btif_queue_connect(uint16_t uuid, const RawAddress* bda, btif_connect_cb_t connect_cb) {
  return do_in_jni_thread(base::BindOnce(&queue_int_add, uuid, *bda, connect_cb));
}

/*******************************************************************************
 *
 * Function         btif_queue_cleanup
 *
 * Description      Clean up existing connection requests for a UUID
 *
 * Returns          void, always succeed
 *
 ******************************************************************************/
void btif_queue_cleanup(uint16_t uuid) {
  do_in_jni_thread(base::BindOnce(&queue_int_cleanup, uuid));
}

/*******************************************************************************
 *
 * Function         btif_queue_advance
 *
 * Description      Clear the queue's busy status and advance to the next
 *                  scheduled connection.
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_queue_advance() { do_in_jni_thread(base::BindOnce(&queue_int_advance)); }

bt_status_t btif_queue_connect_next(void) {
  // The call must be on the JNI thread, otherwise the access to connect_queue
  // is not thread-safe.
  log::assert_that(is_on_jni_thread(), "assert failed: is_on_jni_thread()");

  if (connect_queue.empty()) {
    return BT_STATUS_FAIL;
  }
  if (!stack_manager_get_interface()->get_stack_is_running()) {
    return BT_STATUS_UNEXPECTED_STATE;
  }

  ConnectNode& head = connect_queue.front();

  log::info("Executing profile connection request:{}", head.ToString());
  bt_status_t b_status = head.connect();
  if (b_status != BT_STATUS_SUCCESS) {
    log::info("connect {} failed, advance to next scheduled connection.", head.ToString());
    btif_queue_advance();
  }
  return b_status;
}

/*******************************************************************************
 *
 * Function         btif_queue_release
 *
 * Description      Free up all the queue nodes and set the queue head to NULL
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_queue_release() {
  log::info("");
  if (do_in_jni_thread(base::BindOnce(&queue_int_release)) != BT_STATUS_SUCCESS) {
    log::fatal("Failed to schedule on JNI thread");
  }
}
