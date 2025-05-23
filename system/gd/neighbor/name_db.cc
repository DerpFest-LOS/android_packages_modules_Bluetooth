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
#define LOG_TAG "bt_gd_neigh"

#include "neighbor/name_db.h"

#include <bluetooth/log.h>

#include <memory>
#include <unordered_map>
#include <utility>

#include "common/bind.h"
#include "hci/hci_packets.h"
#include "hci/remote_name_request.h"
#include "module.h"
#include "os/handler.h"

namespace bluetooth {
namespace neighbor {

namespace {
struct PendingRemoteNameRead {
  ReadRemoteNameDbCallback callback_;
  os::Handler* handler_;
};
}  // namespace

struct NameDbModule::impl {
  void ReadRemoteNameRequest(hci::Address address, ReadRemoteNameDbCallback callback,
                             os::Handler* handler);

  bool IsNameCached(hci::Address address) const;
  RemoteName ReadCachedRemoteName(hci::Address address) const;

  impl(const NameDbModule& module);

  void Start();
  void Stop();

private:
  std::unordered_map<hci::Address, std::list<PendingRemoteNameRead>> address_to_pending_read_map_;
  std::unordered_map<hci::Address, RemoteName> address_to_name_map_;

  void OnRemoteNameResponse(hci::Address address, hci::ErrorCode status, RemoteName name);

  hci::RemoteNameRequestModule* name_module_;

  const NameDbModule& module_;
  os::Handler* handler_;
};

const ModuleFactory neighbor::NameDbModule::Factory =
        ModuleFactory([]() { return new neighbor::NameDbModule(); });

neighbor::NameDbModule::impl::impl(const neighbor::NameDbModule& module) : module_(module) {}

void neighbor::NameDbModule::impl::ReadRemoteNameRequest(hci::Address address,
                                                         ReadRemoteNameDbCallback callback,
                                                         os::Handler* handler) {
  if (address_to_pending_read_map_.find(address) != address_to_pending_read_map_.end()) {
    log::warn("Already have remote read db in progress; adding callback to callback list");
    address_to_pending_read_map_[address].push_back({std::move(callback), handler});
    return;
  }

  std::list<PendingRemoteNameRead> tmp;
  address_to_pending_read_map_[address] = std::move(tmp);
  address_to_pending_read_map_[address].push_back({std::move(callback), handler});

  // TODO(cmanton) Use remote name request defaults for now
  hci::PageScanRepetitionMode page_scan_repetition_mode = hci::PageScanRepetitionMode::R1;
  uint16_t clock_offset = 0;
  hci::ClockOffsetValid clock_offset_valid = hci::ClockOffsetValid::INVALID;
  name_module_->StartRemoteNameRequest(
          address,
          hci::RemoteNameRequestBuilder::Create(address, page_scan_repetition_mode, clock_offset,
                                                clock_offset_valid),
          handler_->BindOnce([](hci::ErrorCode /* status */) {}),
          handler_->BindOnce([&](uint64_t /* features */) {
            log::warn("UNIMPLEMENTED: ignoring host supported features");
          }),
          handler_->BindOnceOn(this, &NameDbModule::impl::OnRemoteNameResponse, address));
}

void neighbor::NameDbModule::impl::OnRemoteNameResponse(hci::Address address, hci::ErrorCode status,
                                                        RemoteName name) {
  log::assert_that(address_to_pending_read_map_.find(address) != address_to_pending_read_map_.end(),
                   "assert failed: address_to_pending_read_map_.find(address) != "
                   "address_to_pending_read_map_.end()");
  if (status == hci::ErrorCode::SUCCESS) {
    address_to_name_map_[address] = name;
  }
  auto& callback_list = address_to_pending_read_map_.at(address);
  for (auto& it : callback_list) {
    it.handler_->Call(std::move(it.callback_), address, status == hci::ErrorCode::SUCCESS);
  }
  address_to_pending_read_map_.erase(address);
}

bool neighbor::NameDbModule::impl::IsNameCached(hci::Address address) const {
  return address_to_name_map_.count(address) == 1;
}

RemoteName neighbor::NameDbModule::impl::ReadCachedRemoteName(hci::Address address) const {
  log::assert_that(IsNameCached(address), "assert failed: IsNameCached(address)");
  return address_to_name_map_.at(address);
}

/**
 * General API here
 */
neighbor::NameDbModule::NameDbModule() : pimpl_(std::make_unique<impl>(*this)) {}

neighbor::NameDbModule::~NameDbModule() { pimpl_.reset(); }

void neighbor::NameDbModule::ReadRemoteNameRequest(hci::Address address,
                                                   ReadRemoteNameDbCallback callback,
                                                   os::Handler* handler) {
  GetHandler()->Post(common::BindOnce(&NameDbModule::impl::ReadRemoteNameRequest,
                                      common::Unretained(pimpl_.get()), address,
                                      std::move(callback), handler));
}

bool neighbor::NameDbModule::IsNameCached(hci::Address address) const {
  return pimpl_->IsNameCached(address);
}

RemoteName neighbor::NameDbModule::ReadCachedRemoteName(hci::Address address) const {
  return pimpl_->ReadCachedRemoteName(address);
}

void neighbor::NameDbModule::impl::Start() {
  name_module_ = module_.GetDependency<hci::RemoteNameRequestModule>();
  handler_ = module_.GetHandler();
}

void neighbor::NameDbModule::impl::Stop() {}

/**
 * Module methods here
 */
void neighbor::NameDbModule::ListDependencies(ModuleList* list) const {
  list->add<hci::RemoteNameRequestModule>();
}

void neighbor::NameDbModule::Start() { pimpl_->Start(); }

void neighbor::NameDbModule::Stop() { pimpl_->Stop(); }

}  // namespace neighbor
}  // namespace bluetooth
