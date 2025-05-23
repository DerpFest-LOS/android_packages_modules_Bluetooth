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

#include "hal/hci_hal_host.h"

#include <bluetooth/log.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>  // NOLINT
#include <csignal>
#include <cstdint>
#include <mutex>  // NOLINT
#include <queue>
#include <utility>
#include <vector>

#include "hal/hci_hal.h"
#include "hal/link_clocker.h"
#include "hal/snoop_logger.h"
#include "metrics/counter_metrics.h"
#include "os/mgmt.h"
#include "os/reactor.h"
#include "os/thread.h"

extern int GetAdapterIndex();

namespace {
constexpr int INVALID_FD = -1;

constexpr uint8_t kH4Command = 0x01;
constexpr uint8_t kH4Acl = 0x02;
constexpr uint8_t kH4Sco = 0x03;
constexpr uint8_t kH4Event = 0x04;
constexpr uint8_t kH4Iso = 0x05;

constexpr uint8_t kH4HeaderSize = 1;
constexpr uint8_t kHciAclHeaderSize = 4;
constexpr uint8_t kHciScoHeaderSize = 3;
constexpr uint8_t kHciEvtHeaderSize = 2;
constexpr uint8_t kHciIsoHeaderSize = 4;
constexpr int kBufSize =
        1024 + 4 + 1;  // DeviceProperties::acl_data_packet_size_ + ACL header + H4 header

constexpr uint8_t BTPROTO_HCI = 1;
constexpr uint16_t HCI_CHANNEL_USER = 1;
constexpr uint16_t HCI_CHANNEL_CONTROL = 3;
constexpr uint16_t HCI_DEV_NONE = 0xffff;

/* reference from <kernel>/include/net/bluetooth/mgmt.h */
#define MGMT_OP_INDEX_LIST 0x0003
#define MGMT_EV_COMMAND_COMP 0x0001
#define MGMT_EV_SIZE_MAX 1024
#define REPEAT_ON_INTR(fn) \
  do {                     \
  } while ((fn) == -1 && errno == EINTR)

struct sockaddr_hci {
  sa_family_t hci_family;
  uint16_t hci_dev;
  uint16_t hci_channel;
};

struct mgmt_pkt {
  uint16_t opcode;
  uint16_t index;
  uint16_t len;
  uint8_t data[MGMT_EV_SIZE_MAX];
} __attribute__((packed));

struct mgmt_event_read_index {
  uint16_t cc_opcode;
  uint8_t status;
  uint16_t num_intf;
  uint16_t index[0];
} __attribute__((packed));

int waitHciDev(int hci_interface) {
  struct sockaddr_hci addr;
  struct pollfd fds[1];
  struct mgmt_pkt ev;
  int fd;
  int ret;

  fd = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
  if (fd < 0) {
    bluetooth::log::error("Bluetooth socket error: {}", strerror(errno));
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.hci_family = AF_BLUETOOTH;
  addr.hci_dev = HCI_DEV_NONE;
  addr.hci_channel = HCI_CHANNEL_CONTROL;

  ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
  if (ret < 0) {
    bluetooth::log::error("HCI Channel Control: {} {}", errno, strerror(errno));
    close(fd);
    return -1;
  }

  fds[0].fd = fd;
  fds[0].events = POLLIN;

  /* Read Controller Index List Command */
  ev.opcode = MGMT_OP_INDEX_LIST;
  ev.index = HCI_DEV_NONE;
  ev.len = 0;

  ssize_t wrote;
  REPEAT_ON_INTR(wrote = write(fd, &ev, 6));
  if (wrote != 6) {
    bluetooth::log::error("Unable to write mgmt command: {}", strerror(errno));
    close(fd);
    return -1;
  }
  /* validate mentioned hci interface is present and registered with sock system */
  while (1) {
    int n;
    REPEAT_ON_INTR(n = poll(fds, 1, -1));
    if (n == -1) {
      bluetooth::log::error("Poll error: {}", strerror(errno));
      break;
    } else if (n == 0) {
      bluetooth::log::error("Timeout, no HCI device detected");
      break;
    }

    if (fds[0].revents & POLLIN) {
      REPEAT_ON_INTR(n = read(fd, &ev, sizeof(struct mgmt_pkt)));
      if (n < 0) {
        bluetooth::log::error("Error reading control channel: {}", strerror(errno));
        break;
      } else if (n == 0) {  // unlikely to happen, just a safeguard.
        bluetooth::log::error("Error reading control channel: EOF");
        break;
      }

      if (ev.opcode == MGMT_EV_COMMAND_COMP) {
        struct mgmt_event_read_index* cc;
        int i;

        cc = reinterpret_cast<struct mgmt_event_read_index*>(ev.data);

        if (cc->cc_opcode != MGMT_OP_INDEX_LIST) {
          continue;
        }

        // Find the interface in the list of available indices. If unavailable,
        // the result is -1.
        ret = -1;
        if (cc->status == 0) {
          for (i = 0; i < cc->num_intf; i++) {
            if (cc->index[i] == hci_interface) {
              ret = 0;
              break;
            }
          }

          if (ret != 0) {
            // Chipset might be lost. Wait for index added event.
            bluetooth::log::error(
                    "MGMT index list returns {} HCI interfaces, but HCI interface({}) is not found",
                    cc->num_intf, hci_interface);
          }
        } else {
          // Unlikely event (probably developer error or driver shut down).
          bluetooth::log::error("Failed to read index list: status({})", cc->status);
        }

        // Close and return result of Index List.
        close(fd);
        return ret;
      }
    }
  }

  close(fd);
  return -1;
}

// Connect to Linux HCI socket
int ConnectToSocket() {
  int ret = 0;

  int socket_fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
  if (socket_fd < 0) {
    bluetooth::log::error("can't create socket: {}", strerror(errno));
    return INVALID_FD;
  }

  // Determine which hci index we should connect to.
  int hci_interface = GetAdapterIndex();

  if (waitHciDev(hci_interface) != 0) {
    ::close(socket_fd);
    return INVALID_FD;
  }

  struct sockaddr_hci addr;
  memset(&addr, 0, sizeof(addr));
  addr.hci_family = AF_BLUETOOTH;
  addr.hci_dev = hci_interface;
  addr.hci_channel = HCI_CHANNEL_USER;

  ret = bind(socket_fd, (struct sockaddr*)&addr, sizeof(addr));
  if (ret < 0) {
    bluetooth::log::error("HCI Channel Control: {} {}", errno, strerror(errno));
    ::close(socket_fd);
    return INVALID_FD;
  }
  bluetooth::log::info("HCI device ready");
  return socket_fd;
}
}  // namespace

namespace bluetooth {
namespace hal {

class HciHalHost : public HciHal {
public:
  void registerIncomingPacketCallback(HciHalCallbacks* callback) override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    log::info("before");
    {
      std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
      log::assert_that(
              incoming_packet_callback_ == nullptr && callback != nullptr,
              "assert failed: incoming_packet_callback_ == nullptr && callback != nullptr");
      incoming_packet_callback_ = callback;
    }
    log::info("after");
  }

  void unregisterIncomingPacketCallback() override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    log::info("before");
    {
      std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
      incoming_packet_callback_ = nullptr;
    }
    log::info("after");
  }

  void sendHciCommand(HciPacket command) override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (controller_broken_) {
      return;
    }
    log::assert_that(sock_fd_ != INVALID_FD, "assert failed: sock_fd_ != INVALID_FD");
    std::vector<uint8_t> packet = std::move(command);
    btsnoop_logger_->Capture(packet, SnoopLogger::Direction::OUTGOING,
                             SnoopLogger::PacketType::CMD);
    packet.insert(packet.cbegin(), kH4Command);
    write_to_fd(packet);
  }

  void sendAclData(HciPacket data) override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (controller_broken_) {
      return;
    }
    log::assert_that(sock_fd_ != INVALID_FD, "assert failed: sock_fd_ != INVALID_FD");
    std::vector<uint8_t> packet = std::move(data);
    btsnoop_logger_->Capture(packet, SnoopLogger::Direction::OUTGOING,
                             SnoopLogger::PacketType::ACL);
    packet.insert(packet.cbegin(), kH4Acl);
    write_to_fd(packet);
  }

  void sendScoData(HciPacket data) override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (controller_broken_) {
      return;
    }

    log::assert_that(sock_fd_ != INVALID_FD, "assert failed: sock_fd_ != INVALID_FD");
    std::vector<uint8_t> packet = std::move(data);
    btsnoop_logger_->Capture(packet, SnoopLogger::Direction::OUTGOING,
                             SnoopLogger::PacketType::SCO);
    packet.insert(packet.cbegin(), kH4Sco);
    write_to_fd(packet);
  }

  void sendIsoData(HciPacket data) override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (controller_broken_) {
      return;
    }
    log::assert_that(sock_fd_ != INVALID_FD, "assert failed: sock_fd_ != INVALID_FD");
    std::vector<uint8_t> packet = std::move(data);
    btsnoop_logger_->Capture(packet, SnoopLogger::Direction::OUTGOING,
                             SnoopLogger::PacketType::ISO);
    packet.insert(packet.cbegin(), kH4Iso);
    write_to_fd(packet);
  }

  uint16_t getMsftOpcode() override {
    return os::Management::getInstance().getVendorSpecificCode(MGMT_VS_OPCODE_MSFT);
  }

  void markControllerBroken() override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (controller_broken_) {
      log::error("Controller already marked as broken!");
      return;
    }
    controller_broken_ = true;
  }

protected:
  void ListDependencies(ModuleList* list) const {
    list->add<LinkClocker>();
    list->add<metrics::CounterMetrics>();
    list->add<SnoopLogger>();
  }

  void Start() override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    log::assert_that(sock_fd_ == INVALID_FD, "assert failed: sock_fd_ == INVALID_FD");
    sock_fd_ = ConnectToSocket();

    // We don't want to crash when the chipset is broken.
    if (sock_fd_ == INVALID_FD) {
      log::error("Failed to connect to HCI socket. Aborting HAL initialization process.");
      controller_broken_ = true;
      kill(getpid(), SIGTERM);
      return;
    }

    reactable_ = hci_incoming_thread_.GetReactor()->Register(
            sock_fd_, common::Bind(&HciHalHost::incoming_packet_received, common::Unretained(this)),
            common::Bind(&HciHalHost::send_packet_ready, common::Unretained(this)));
    hci_incoming_thread_.GetReactor()->ModifyRegistration(reactable_,
                                                          os::Reactor::REACT_ON_READ_ONLY);
    link_clocker_ = GetDependency<LinkClocker>();
    btsnoop_logger_ = GetDependency<SnoopLogger>();
    log::info("HAL opened successfully");
  }

  void Stop() override {
    std::lock_guard<std::mutex> lock(api_mutex_);
    log::info("HAL is closing");
    if (reactable_ != nullptr) {
      hci_incoming_thread_.GetReactor()->Unregister(reactable_);
      log::info("HAL is stopping, start waiting for last callback");
      // Wait up to 1 second for the last incoming packet callback to finish
      hci_incoming_thread_.GetReactor()->WaitForUnregisteredReactable(
              std::chrono::milliseconds(1000));
      log::info("HAL is stopping, finished waiting for last callback");
      log::assert_that(sock_fd_ != INVALID_FD, "assert failed: sock_fd_ != INVALID_FD");
    }
    reactable_ = nullptr;
    {
      std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
      incoming_packet_callback_ = nullptr;
    }
    auto start = std::chrono::high_resolution_clock::now();
    ::close(sock_fd_);
    auto end = std::chrono::high_resolution_clock::now();
    int64_t duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    log::info("Spent {} milliseconds on closing socket", duration);
    sock_fd_ = INVALID_FD;
    log::info("HAL is closed");
  }

  std::string ToString() const override { return std::string("HciHalHost"); }

private:
  // Held when APIs are called, NOT to be held during callbacks
  std::mutex api_mutex_;
  HciHalCallbacks* incoming_packet_callback_ = nullptr;
  std::mutex incoming_packet_callback_mutex_;
  int sock_fd_ = INVALID_FD;
  bluetooth::os::Thread hci_incoming_thread_ =
          bluetooth::os::Thread("hci_incoming_thread", bluetooth::os::Thread::Priority::NORMAL);
  bluetooth::os::Reactor::Reactable* reactable_ = nullptr;
  std::queue<std::vector<uint8_t>> hci_outgoing_queue_;
  SnoopLogger* btsnoop_logger_ = nullptr;
  LinkClocker* link_clocker_ = nullptr;
  bool controller_broken_ = false;

  void write_to_fd(HciPacket packet) {
    // TODO(chromeos-bt-team@): replace this with new queue when it's ready
    hci_outgoing_queue_.emplace(packet);
    if (hci_outgoing_queue_.size() == 1) {
      hci_incoming_thread_.GetReactor()->ModifyRegistration(reactable_,
                                                            os::Reactor::REACT_ON_READ_WRITE);
    }
  }

  void send_packet_ready() {
    std::lock_guard<std::mutex> lock(api_mutex_);
    if (hci_outgoing_queue_.empty()) {
      return;
    }
    auto packet_to_send = hci_outgoing_queue_.front();
    auto bytes_written =
            write(sock_fd_, reinterpret_cast<void*>(packet_to_send.data()), packet_to_send.size());
    hci_outgoing_queue_.pop();
    if (bytes_written == -1) {
      log::error("Can't write to socket: {}", strerror(errno));
      markControllerBroken();
      kill(getpid(), SIGTERM);
    }
    if (hci_outgoing_queue_.empty()) {
      hci_incoming_thread_.GetReactor()->ModifyRegistration(reactable_,
                                                            os::Reactor::REACT_ON_READ_ONLY);
    }
  }

  void incoming_packet_received() {
    {
      std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
      if (incoming_packet_callback_ == nullptr) {
        log::info("Dropping a packet");
        return;
      }
    }
    uint8_t buf[kBufSize] = {};

    ssize_t received_size;
    RUN_NO_INTR(received_size = read(sock_fd_, buf, kBufSize));

    // we don't want crash when the chipset is broken.
    if (received_size == -1) {
      log::error("Can't receive from socket: {}", strerror(errno));
      markControllerBroken();
      kill(getpid(), SIGTERM);
      return;
    }

    if (received_size == 0) {
      log::warn("Can't read H4 header. EOF received");
      markControllerBroken();
      kill(getpid(), SIGTERM);
      return;
    }

    if (buf[0] == kH4Event) {
      log::assert_that(received_size >= kH4HeaderSize + kHciEvtHeaderSize,
                       "Received bad HCI_EVT packet size: {}", received_size);
      uint8_t hci_evt_parameter_total_length = buf[2];
      ssize_t payload_size = received_size - (kH4HeaderSize + kHciEvtHeaderSize);
      log::assert_that(payload_size == hci_evt_parameter_total_length,
                       "malformed HCI event total parameter size received: {} != {}", payload_size,
                       hci_evt_parameter_total_length);

      HciPacket receivedHciPacket;
      receivedHciPacket.assign(buf + kH4HeaderSize,
                               buf + kH4HeaderSize + kHciEvtHeaderSize + payload_size);
      link_clocker_->OnHciEvent(receivedHciPacket);
      btsnoop_logger_->Capture(receivedHciPacket, SnoopLogger::Direction::INCOMING,
                               SnoopLogger::PacketType::EVT);
      {
        std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
        if (incoming_packet_callback_ == nullptr) {
          log::info("Dropping an event after processing");
          return;
        }
        incoming_packet_callback_->hciEventReceived(receivedHciPacket);
      }
    }

    if (buf[0] == kH4Acl) {
      log::assert_that(received_size >= kH4HeaderSize + kHciAclHeaderSize,
                       "Received bad HCI_ACL packet size: {}", received_size);
      int payload_size = received_size - (kH4HeaderSize + kHciAclHeaderSize);
      uint16_t hci_acl_data_total_length = (buf[4] << 8) + buf[3];
      log::assert_that(payload_size == hci_acl_data_total_length,
                       "malformed ACL length received: {} != {}", payload_size,
                       hci_acl_data_total_length);
      log::assert_that(hci_acl_data_total_length <= kBufSize - kH4HeaderSize - kHciAclHeaderSize,
                       "packet too long");

      HciPacket receivedHciPacket;
      receivedHciPacket.assign(buf + kH4HeaderSize,
                               buf + kH4HeaderSize + kHciAclHeaderSize + payload_size);
      btsnoop_logger_->Capture(receivedHciPacket, SnoopLogger::Direction::INCOMING,
                               SnoopLogger::PacketType::ACL);
      {
        std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
        if (incoming_packet_callback_ == nullptr) {
          log::info("Dropping an ACL packet after processing");
          return;
        }
        incoming_packet_callback_->aclDataReceived(receivedHciPacket);
      }
    }

    if (buf[0] == kH4Sco) {
      log::assert_that(received_size >= kH4HeaderSize + kHciScoHeaderSize,
                       "Received bad HCI_SCO packet size: {}", received_size);
      int payload_size = received_size - (kH4HeaderSize + kHciScoHeaderSize);
      uint8_t hci_sco_data_total_length = buf[3];
      log::assert_that(payload_size == hci_sco_data_total_length,
                       "malformed SCO length received: {} != {}", payload_size,
                       hci_sco_data_total_length);

      HciPacket receivedHciPacket;
      receivedHciPacket.assign(buf + kH4HeaderSize,
                               buf + kH4HeaderSize + kHciScoHeaderSize + payload_size);
      btsnoop_logger_->Capture(receivedHciPacket, SnoopLogger::Direction::INCOMING,
                               SnoopLogger::PacketType::SCO);
      {
        std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
        if (incoming_packet_callback_ == nullptr) {
          log::info("Dropping a SCO packet after processing");
          return;
        }
        incoming_packet_callback_->scoDataReceived(receivedHciPacket);
      }
    }

    if (buf[0] == kH4Iso) {
      log::assert_that(received_size >= kH4HeaderSize + kHciIsoHeaderSize,
                       "Received bad HCI_ISO packet size: {}", received_size);
      int payload_size = received_size - (kH4HeaderSize + kHciIsoHeaderSize);
      uint16_t hci_iso_data_total_length = ((buf[4] & 0x3f) << 8) + buf[3];
      log::assert_that(payload_size == hci_iso_data_total_length,
                       "malformed ISO length received: {} != {}", payload_size,
                       hci_iso_data_total_length);

      HciPacket receivedHciPacket;
      receivedHciPacket.assign(buf + kH4HeaderSize,
                               buf + kH4HeaderSize + kHciIsoHeaderSize + payload_size);
      btsnoop_logger_->Capture(receivedHciPacket, SnoopLogger::Direction::INCOMING,
                               SnoopLogger::PacketType::ISO);
      {
        std::lock_guard<std::mutex> incoming_packet_callback_lock(incoming_packet_callback_mutex_);
        if (incoming_packet_callback_ == nullptr) {
          log::info("Dropping a ISO packet after processing");
          return;
        }
        incoming_packet_callback_->isoDataReceived(receivedHciPacket);
      }
    }
    memset(buf, 0, kBufSize);
  }
};

const ModuleFactory HciHal::Factory = ModuleFactory([]() { return new HciHalHost(); });

}  // namespace hal
}  // namespace bluetooth
