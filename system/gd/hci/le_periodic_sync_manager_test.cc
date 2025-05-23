/*
 * Copyright 2022 The Android Open Source Project
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

#include "hci/le_periodic_sync_manager.h"

#include <com_android_bluetooth_flags.h>
#include <flag_macros.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <log/log.h>

#include "hci/le_scanning_callback.h"
#include "hci/le_scanning_interface.h"
#include "hci/le_scanning_manager_mock.h"
#include "os/handler.h"

#define TEST_BT com::android::bluetooth::flags

using namespace std::chrono_literals;

using testing::_;

namespace bluetooth {
namespace hci {
namespace {

PacketView<kLittleEndian> GetPacketView(std::unique_ptr<packet::BasePacketBuilder> packet) {
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  BitInserter i(*bytes);
  bytes->reserve(packet->size());
  packet->Serialize(i);
  return packet::PacketView<packet::kLittleEndian>(bytes);
}

class TestLeScanningInterface : public LeScanningInterface {
public:
  void EnqueueCommand(
          std::unique_ptr<LeScanningCommandBuilder> command,
          common::ContextualOnceCallback<void(CommandCompleteView)> on_complete) override {
    std::lock_guard<std::mutex> lock(mutex_);
    command_queue_.push(std::move(command));
    command_complete_callbacks.push_back(std::move(on_complete));
    if (command_promise_ != nullptr) {
      std::promise<void>* prom = command_promise_.release();
      prom->set_value();
      delete prom;
    }
  }

  void EnqueueCommand(std::unique_ptr<LeScanningCommandBuilder> command,
                      common::ContextualOnceCallback<void(CommandStatusView)> on_status) override {
    command_queue_.push(std::move(command));
    command_status_callbacks.push_back(std::move(on_status));
    if (command_promise_ != nullptr) {
      std::promise<void>* prom = command_promise_.release();
      prom->set_value();
      delete prom;
    }
  }

  void EnqueueCommand(std::unique_ptr<LeScanningCommandBuilder> /* command */,
                      common::ContextualOnceCallback<void(
                              CommandStatusOrCompleteView)> /* on_status_or_complete */) override {
    FAIL();
  }

  void SetCommandFuture() {
    ASSERT_EQ(command_promise_, nullptr) << "Promises, Promises, ... Only one at a time.";
    command_promise_ = std::make_unique<std::promise<void>>();
    command_future_ = std::make_unique<std::future<void>>(command_promise_->get_future());
  }

  CommandView GetLastCommand() {
    if (command_queue_.empty()) {
      return CommandView::Create(
              PacketView<kLittleEndian>(std::make_shared<std::vector<uint8_t>>()));
    }
    auto last = std::move(command_queue_.front());
    command_queue_.pop();
    return CommandView::Create(GetPacketView(std::move(last)));
  }

  CommandView GetCommand(OpCode op_code) {
    if (!command_queue_.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (command_future_ != nullptr) {
        command_future_.reset();
        command_promise_.reset();
      }
    } else if (command_future_ != nullptr) {
      auto result = command_future_->wait_for(std::chrono::milliseconds(1000));
      EXPECT_NE(std::future_status::timeout, result);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    log::assert_that(!command_queue_.empty(), "Expecting command {} but command queue was empty",
                     OpCodeText(op_code));
    CommandView command_packet_view = GetLastCommand();
    EXPECT_TRUE(command_packet_view.IsValid());
    EXPECT_EQ(command_packet_view.GetOpCode(), op_code);
    return command_packet_view;
  }

  void CommandCompleteCallback(std::unique_ptr<EventBuilder> event_builder) {
    auto event = EventView::Create(GetPacketView(std::move(event_builder)));
    CommandCompleteView complete_view = CommandCompleteView::Create(event);
    ASSERT_TRUE(complete_view.IsValid());
    ASSERT_NE((uint16_t)command_complete_callbacks.size(), 0);
    std::move(command_complete_callbacks.front())(complete_view);
    command_complete_callbacks.pop_front();
  }

  void CommandStatusCallback(std::unique_ptr<EventBuilder> event_builder) {
    auto event = EventView::Create(GetPacketView(std::move(event_builder)));
    CommandStatusView status_view = CommandStatusView::Create(event);
    ASSERT_TRUE(status_view.IsValid());
    ASSERT_NE((uint16_t)command_status_callbacks.size(), 0);
    std::move(command_status_callbacks.front())(status_view);
    command_status_callbacks.pop_front();
  }

private:
  std::list<common::ContextualOnceCallback<void(CommandCompleteView)>> command_complete_callbacks;
  std::list<common::ContextualOnceCallback<void(CommandStatusView)>> command_status_callbacks;
  std::queue<std::unique_ptr<CommandBuilder>> command_queue_;
  std::unique_ptr<std::promise<void>> command_promise_;
  std::unique_ptr<std::future<void>> command_future_;
  mutable std::mutex mutex_;
};

class PeriodicSyncManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    __android_log_set_minimum_priority(ANDROID_LOG_VERBOSE);
    thread_ = new os::Thread("thread", os::Thread::Priority::NORMAL);
    handler_ = new os::Handler(thread_);
    test_le_scanning_interface_ = new TestLeScanningInterface();
    periodic_sync_manager_ = new PeriodicSyncManager(&mock_callbacks_);
    periodic_sync_manager_->Init(test_le_scanning_interface_, handler_);
  }

  void TearDown() override {
    delete periodic_sync_manager_;
    periodic_sync_manager_ = nullptr;
    delete test_le_scanning_interface_;
    test_le_scanning_interface_ = nullptr;
    handler_->Clear();
    delete handler_;
    handler_ = nullptr;
    delete thread_;
    thread_ = nullptr;
  }

  void sync_handler() {
    log::assert_that(thread_ != nullptr, "assert failed: thread_ != nullptr");
    log::assert_that(thread_->GetReactor()->WaitForIdle(2s),
                     "assert failed: thread_->GetReactor()->WaitForIdle(2s)");
  }

  class MockCallbacks : public bluetooth::hci::ScanningCallback {
  public:
    MOCK_METHOD(void, OnScannerRegistered,
                (const bluetooth::hci::Uuid app_uuid, ScannerId scanner_id, ScanningStatus status),
                (override));
    MOCK_METHOD(void, OnSetScannerParameterComplete, (ScannerId scanner_id, ScanningStatus status),
                (override));
    MOCK_METHOD(void, OnScanResult,
                (uint16_t event_type, uint8_t address_type, Address address, uint8_t primary_phy,
                 uint8_t secondary_phy, uint8_t advertising_sid, int8_t tx_power, int8_t rssi,
                 uint16_t periodic_advertising_interval, std::vector<uint8_t> advertising_data),
                (override));
    MOCK_METHOD(void, OnTrackAdvFoundLost,
                (bluetooth::hci::AdvertisingFilterOnFoundOnLostInfo on_found_on_lost_info),
                (override));
    MOCK_METHOD(void, OnBatchScanReports,
                (int client_if, int status, int report_format, int num_records,
                 std::vector<uint8_t> data),
                (override));
    MOCK_METHOD(void, OnBatchScanThresholdCrossed, (int client_if), (override));
    MOCK_METHOD(void, OnTimeout, (), (override));
    MOCK_METHOD(void, OnFilterEnable, (Enable enable, uint8_t status), (override));
    MOCK_METHOD(void, OnFilterParamSetup,
                (uint8_t available_spaces, ApcfAction action, uint8_t status), (override));
    MOCK_METHOD(void, OnFilterConfigCallback,
                (ApcfFilterType filter_type, uint8_t available_spaces, ApcfAction action,
                 uint8_t status),
                (override));
    MOCK_METHOD(void, OnPeriodicSyncStarted,
                (int, uint8_t, uint16_t, uint8_t, AddressWithType, uint8_t, uint16_t));
    MOCK_METHOD(void, OnPeriodicSyncReport,
                (uint16_t, int8_t, int8_t, uint8_t, std::vector<uint8_t>));
    MOCK_METHOD(void, OnPeriodicSyncLost, (uint16_t));
    MOCK_METHOD(void, OnPeriodicSyncTransferred, (int, uint8_t, Address));
    MOCK_METHOD(void, OnBigInfoReport, (uint16_t, bool));
  } mock_callbacks_;

  os::Thread* thread_;
  os::Handler* handler_;
  TestLeScanningInterface* test_le_scanning_interface_;
  PeriodicSyncManager* periodic_sync_manager_ = nullptr;
};

TEST_F(PeriodicSyncManagerTest, startup_teardown) {}

TEST_F(PeriodicSyncManagerTest, start_sync_test) {
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  int request_id = 0x01;
  uint8_t advertiser_sid = 0x02;
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  uint16_t sync_handle = 0x03;
  PeriodicSyncStates request{
          .request_id = request_id,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  uint16_t skip = 0x04;
  uint16_t sync_timeout = 0x0A;
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, skip, sync_timeout);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto packet_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(packet_view.IsValid());
  ASSERT_EQ(advertiser_sid, packet_view.GetAdvertisingSid());
  ASSERT_EQ(AdvertisingAddressType::PUBLIC_DEVICE_OR_IDENTITY_ADDRESS,
            packet_view.GetAdvertiserAddressType());
  ASSERT_EQ(address, packet_view.GetAdvertiserAddress());
  ASSERT_EQ(skip, packet_view.GetSkip());
  ASSERT_EQ(sync_timeout, packet_view.GetSyncTimeout());
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, handle_advertising_sync_established_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  PeriodicSyncStates request{
          .request_id = 0x01,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncStarted);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest,
       handle_advertising_sync_established_with_public_identity_address_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  PeriodicSyncStates request{
          .request_id = 0x01,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncStarted);

  // Get LePeriodicAdvertisingSyncEstablished with AddressType::PUBLIC_IDENTITY_ADDRESS
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid, AddressType::PUBLIC_IDENTITY_ADDRESS,
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, stop_sync_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  PeriodicSyncStates request{
          .request_id = 0x01,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncStarted);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  // StopSync
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StopSync(sync_handle);
  packet = test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC);
  auto packet_view =
          LePeriodicAdvertisingTerminateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(packet_view.IsValid());
  ASSERT_EQ(sync_handle, packet_view.GetSyncHandle());
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, cancel_create_sync_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  PeriodicSyncStates request{
          .request_id = 0x01,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  // Cancel crate sync
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->CancelCreateSync(advertiser_sid, address_with_type.GetAddress());
  packet = test_le_scanning_interface_->GetCommand(
          OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL);
  auto packet_view =
          LePeriodicAdvertisingCreateSyncCancelView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(packet_view.IsValid());
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, transfer_sync_test) {
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  uint16_t service_data = 0x10;
  uint16_t sync_handle = 0x11;
  uint16_t connection_handle = 0x12;
  int pa_source = 0x01;
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->TransferSync(address, service_data, sync_handle, pa_source,
                                       connection_handle);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_SYNC_TRANSFER);
  auto packet_view =
          LePeriodicAdvertisingSyncTransferView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(packet_view.IsValid());
  ASSERT_EQ(connection_handle, packet_view.GetConnectionHandle());
  ASSERT_EQ(service_data, packet_view.GetServiceData());
  ASSERT_EQ(sync_handle, packet_view.GetSyncHandle());

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncTransferred);

  // Get command complete
  test_le_scanning_interface_->CommandCompleteCallback(
          LePeriodicAdvertisingSyncTransferCompleteBuilder::Create(0x00, ErrorCode::SUCCESS,
                                                                   connection_handle));

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, sync_set_info_test) {
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  uint16_t service_data = 0x10;
  uint16_t advertising_handle = 0x11;
  uint16_t connection_handle = 0x12;
  int pa_source = 0x01;
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->SyncSetInfo(address, service_data, advertising_handle, pa_source,
                                      connection_handle);
  auto packet = test_le_scanning_interface_->GetCommand(
          OpCode::LE_PERIODIC_ADVERTISING_SET_INFO_TRANSFER);
  auto packet_view =
          LePeriodicAdvertisingSetInfoTransferView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(packet_view.IsValid());
  ASSERT_EQ(connection_handle, packet_view.GetConnectionHandle());
  ASSERT_EQ(service_data, packet_view.GetServiceData());
  ASSERT_EQ(advertising_handle, packet_view.GetAdvertisingHandle());

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncTransferred);

  // Get command complete
  test_le_scanning_interface_->CommandCompleteCallback(
          LePeriodicAdvertisingSetInfoTransferCompleteBuilder::Create(0x00, ErrorCode::SUCCESS,
                                                                      connection_handle));

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, sync_tx_parameters_test) {
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  uint8_t mode = 0x00;
  uint16_t skip = 0x11;
  uint16_t timeout = 0x12;
  int reg_id = 0x01;
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->SyncTxParameters(address, mode, skip, timeout, reg_id);
  auto packet = test_le_scanning_interface_->GetCommand(
          OpCode::LE_SET_DEFAULT_PERIODIC_ADVERTISING_SYNC_TRANSFER_PARAMETERS);
  auto packet_view = LeSetDefaultPeriodicAdvertisingSyncTransferParametersView::Create(
          LeScanningCommandView::Create(packet));

  ASSERT_TRUE(packet_view.IsValid());
  ASSERT_EQ(mode, (uint8_t)packet_view.GetMode());
  ASSERT_EQ(skip, packet_view.GetSkip());
  ASSERT_EQ(timeout, packet_view.GetSyncTimeout());

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, handle_sync_lost_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  PeriodicSyncStates request{
          .request_id = 0x01,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncStarted);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncLost);

  // Get LePeriodicAdvertisingSyncLost
  auto builder2 = LePeriodicAdvertisingSyncLostBuilder::Create(sync_handle);

  auto event_view2 = LePeriodicAdvertisingSyncLostView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder2)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncLost(event_view2);

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, handle_advertising_sync_established_after_error_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);

  // First request which will finish with error
  int request_id_1 = 0x01;
  PeriodicSyncStates request{
          .request_id = request_id_1,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(
          mock_callbacks_,
          OnPeriodicSyncStarted(request_id_1,
                                static_cast<uint8_t>(ErrorCode::CONNECTION_FAILED_ESTABLISHMENT), _,
                                _, _, _, _))
          .Times(1);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::CONNECTION_FAILED_ESTABLISHMENT, sync_handle, advertiser_sid,
          address_with_type.GetAddressType(), address_with_type.GetAddress(),
          SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  // Second request with the same data but different id
  int request_id_2 = 0x02;
  request.request_id = request_id_2;
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  packet = test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  temp_view = LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_,
              OnPeriodicSyncStarted(request_id_2, static_cast<uint8_t>(ErrorCode::SUCCESS), _, _, _,
                                    _, _))
          .Times(1);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder2 = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder2)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest,
       handle_advertising_sync_established_after_create_command_error_test) {
  uint16_t sync_handle = 0x12;
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);

  // First request which will finish with error
  int request_id_1 = 0x01;
  uint8_t advertiser_sid_1 = 0x02;
  PeriodicSyncStates request{
          .request_id = request_id_1,
          .advertiser_sid = advertiser_sid_1,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  EXPECT_CALL(mock_callbacks_,
              OnPeriodicSyncStarted(request_id_1,
                                    static_cast<uint8_t>(ErrorCode::MEMORY_CAPACITY_EXCEEDED), _,
                                    advertiser_sid_1, _, _, _))
          .Times(1);

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::MEMORY_CAPACITY_EXCEEDED,
                                                               0x00));

  // Second request
  int request_id_2 = 0x02;
  uint8_t advertiser_sid_2 = 0x03;
  request.request_id = request_id_2;
  request.advertiser_sid = advertiser_sid_2;
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  packet = test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  temp_view = LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_,
              OnPeriodicSyncStarted(request_id_2, static_cast<uint8_t>(ErrorCode::SUCCESS), _,
                                    advertiser_sid_2, _, _, _))
          .Times(1);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid_2, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest,
       handle_advertising_sync_established_after_cancel_command_error_test) {
  uint16_t sync_handle = 0x12;
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);

  // First request which will finish with timeout error
  uint8_t advertiser_sid_1 = 0x02;
  int request_id_1 = 0x01;
  PeriodicSyncStates request{
          .request_id = request_id_1,
          .advertiser_sid = advertiser_sid_1,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(
          mock_callbacks_,
          OnPeriodicSyncStarted(request_id_1, static_cast<uint8_t>(ErrorCode::ADVERTISING_TIMEOUT),
                                _, advertiser_sid_1, _, _, _))
          .Times(1);

  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->OnStartSyncTimeout();
  packet = test_le_scanning_interface_->GetCommand(
          OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL);
  auto temp_view2 =
          LePeriodicAdvertisingCreateSyncCancelView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view2.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandCompleteCallback(
          LePeriodicAdvertisingCreateSyncCancelCompleteBuilder::Create(
                  0x00, ErrorCode::COMMAND_DISALLOWED));

  // Second request
  int request_id_2 = 0x02;
  uint8_t advertiser_sid_2 = 0x03;
  request.request_id = request_id_2;
  request.advertiser_sid = advertiser_sid_2;
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  packet = test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  temp_view = LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_,
              OnPeriodicSyncStarted(request_id_2, static_cast<uint8_t>(ErrorCode::SUCCESS), _,
                                    advertiser_sid_2, _, _, _))
          .Times(1);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder2 = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid_2, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder2)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, onStartSyncTimeout_callWithoutPendingRequestsAndPeriodicSyncs) {
  periodic_sync_manager_->OnStartSyncTimeout();
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, onStartSyncTimeout_callWithoutPeriodicSyncs) {
  uint16_t sync_handle = 0x12;
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);

  uint8_t advertiser_sid_1 = 0x02;
  int request_id_1 = 0x01;
  PeriodicSyncStates request{
          .request_id = request_id_1,
          .advertiser_sid = advertiser_sid_1,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);

  // First timeout to erase periodic_syncs_
  periodic_sync_manager_->OnStartSyncTimeout();
  // Second to actual check
  periodic_sync_manager_->OnStartSyncTimeout();
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest,
       handlePeriodicAdvertisingCreateSyncStatus_callWithoutPeriodicSyncs) {
  uint16_t sync_handle = 0x12;
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);

  int request_id_1 = 0x01;
  uint8_t advertiser_sid_1 = 0x02;
  PeriodicSyncStates request{
          .request_id = request_id_1,
          .advertiser_sid = advertiser_sid_1,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);

  // Timeout to erase periodic_syncs_
  periodic_sync_manager_->OnStartSyncTimeout();

  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::MEMORY_CAPACITY_EXCEEDED,
                                                               0x00));
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, syncEstablished_pendingCheckToCorrectTheOrder) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);

  // start scan
  int request_id_1 = 0x01;
  PeriodicSyncStates request{
          .request_id = request_id_1,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);

  EXPECT_CALL(
          mock_callbacks_,
          OnPeriodicSyncStarted(request_id_1, static_cast<uint8_t>(ErrorCode::ADVERTISING_TIMEOUT),
                                _, _, _, _, _))
          .Times(1);

  // First timeout
  periodic_sync_manager_->OnStartSyncTimeout();

  // Second request with the same data but different id
  int request_id_2 = 0x02;
  request.request_id = request_id_2;
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);

  // Get LePeriodicAdvertisingSyncEstablished for the first request
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::OPERATION_CANCELLED_BY_HOST, sync_handle, advertiser_sid,
          address_with_type.GetAddressType(), address_with_type.GetAddress(),
          SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  EXPECT_CALL(
          mock_callbacks_,
          OnPeriodicSyncStarted(request_id_2, static_cast<uint8_t>(ErrorCode::ADVERTISING_TIMEOUT),
                                _, _, _, _, _))
          .Times(1);

  // Second timeout
  periodic_sync_manager_->OnStartSyncTimeout();

  // Get LePeriodicAdvertisingSyncEstablished for the second request
  auto builder2 = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::OPERATION_CANCELLED_BY_HOST, sync_handle, advertiser_sid,
          address_with_type.GetAddressType(), address_with_type.GetAddress(),
          SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder2)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);
  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, handle_periodic_advertising_report_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  PeriodicSyncStates request{
          .request_id = 0x01,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncStarted);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncReport);

  // Get LePeriodicAdvertisingReport
  std::vector<uint8_t> data = {0x01, 0x02, 0x03};
  auto builder2 = LePeriodicAdvertisingReportBuilder::Create(sync_handle, 0x1a, 0x1a,
                                                             CteType::AOA_CONSTANT_TONE_EXTENSION,
                                                             DataStatus::COMPLETE, data);

  auto event_view2 = LePeriodicAdvertisingReportView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder2)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingReport(event_view2);

  sync_handler();
}

TEST_F(PeriodicSyncManagerTest, handle_biginfo_advertising_report_test) {
  uint16_t sync_handle = 0x12;
  uint8_t advertiser_sid = 0x02;
  // start scan
  Address address;
  Address::FromString("00:11:22:33:44:55", address);
  AddressWithType address_with_type = AddressWithType(address, AddressType::PUBLIC_DEVICE_ADDRESS);
  PeriodicSyncStates request{
          .request_id = 0x01,
          .advertiser_sid = advertiser_sid,
          .address_with_type = address_with_type,
          .sync_handle = sync_handle,
          .sync_state = PeriodicSyncState::PERIODIC_SYNC_STATE_IDLE,
  };
  ASSERT_NO_FATAL_FAILURE(test_le_scanning_interface_->SetCommandFuture());
  periodic_sync_manager_->StartSync(request, 0x04, 0x0A);
  auto packet =
          test_le_scanning_interface_->GetCommand(OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto temp_view =
          LePeriodicAdvertisingCreateSyncView::Create(LeScanningCommandView::Create(packet));
  ASSERT_TRUE(temp_view.IsValid());

  // Get command status
  test_le_scanning_interface_->CommandStatusCallback(
          LePeriodicAdvertisingCreateSyncStatusBuilder::Create(ErrorCode::SUCCESS, 0x00));

  EXPECT_CALL(mock_callbacks_, OnPeriodicSyncStarted);

  // Get LePeriodicAdvertisingSyncEstablished
  auto builder = LePeriodicAdvertisingSyncEstablishedBuilder::Create(
          ErrorCode::SUCCESS, sync_handle, advertiser_sid, address_with_type.GetAddressType(),
          address_with_type.GetAddress(), SecondaryPhyType::LE_1M, 0xFF, ClockAccuracy::PPM_250);
  auto event_view = LePeriodicAdvertisingSyncEstablishedView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder)))));
  periodic_sync_manager_->HandleLePeriodicAdvertisingSyncEstablished(event_view);

  EXPECT_CALL(mock_callbacks_, OnBigInfoReport);

  // Get LeBigInfoAdvertisingReport
  auto builder2 = LeBigInfoAdvertisingReportBuilder::Create(
          sync_handle, 2, 9, 24, 3, 1, 2, 100, 10000, 100, static_cast<SecondaryPhyType>(2),
          static_cast<Enable>(0), static_cast<Enable>(1));

  auto event_view2 = LeBigInfoAdvertisingReportView::Create(
          LeMetaEventView::Create(EventView::Create(GetPacketView(std::move(builder2)))));
  periodic_sync_manager_->HandleLeBigInfoAdvertisingReport(event_view2);

  sync_handler();
}

}  // namespace
}  // namespace hci
}  // namespace bluetooth
