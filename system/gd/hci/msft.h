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
#pragma once

#include "hci/hci_packets.h"
#include "hci/le_scanning_callback.h"
#include "module.h"

struct MsftAdvMonitor;

namespace bluetooth {
namespace hci {

class MsftExtensionManager : public bluetooth::Module {
public:
  MsftExtensionManager();

  MsftExtensionManager(const MsftExtensionManager&) = delete;
  MsftExtensionManager& operator=(const MsftExtensionManager&) = delete;

  using MsftAdvMonitorAddCallback =
          base::Callback<void(uint8_t /* monitor_handle */, ErrorCode /* status */)>;
  using MsftAdvMonitorRemoveCallback = base::Callback<void(ErrorCode /* status */)>;
  using MsftAdvMonitorEnableCallback = base::Callback<void(ErrorCode /* status */)>;

  virtual bool SupportsMsftExtensions();
  void MsftAdvMonitorAdd(const MsftAdvMonitor& monitor, MsftAdvMonitorAddCallback cb);
  void MsftAdvMonitorRemove(uint8_t monitor_handle, MsftAdvMonitorRemoveCallback cb);
  void MsftAdvMonitorEnable(bool enable, MsftAdvMonitorEnableCallback cb);
  void SetScanningCallback(ScanningCallback* callbacks);

  static const ModuleFactory Factory;

protected:
  void ListDependencies(ModuleList* list) const override;

  void Start() override;

  void Stop() override;

  std::string ToString() const override;

private:
  struct impl;
  std::unique_ptr<impl> pimpl_;
};

}  // namespace hci
}  // namespace bluetooth
