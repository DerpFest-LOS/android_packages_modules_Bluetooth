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

#include "main/shim/entry.h"

#include "hal/snoop_logger.h"
#include "hci/acl_manager.h"
#include "hci/controller.h"
#include "hci/controller_interface.h"
#include "hci/distance_measurement_manager.h"
#include "hci/hci_layer.h"
#include "hci/le_advertising_manager.h"
#include "hci/le_scanning_manager.h"
#include "hci/msft.h"
#include "hci/remote_name_request.h"
#include "lpp/lpp_offload_manager.h"
#include "main/shim/stack.h"
#include "metrics/counter_metrics.h"
#include "os/handler.h"
#include "shim/dumpsys.h"
#include "storage/storage_module.h"

namespace bluetooth {
namespace shim {

os::Handler* GetGdShimHandler() { return Stack::GetInstance()->GetHandler(); }

hci::LeAdvertisingManager* GetAdvertising() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::LeAdvertisingManager>();
}

hci::ControllerInterface* GetController() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::Controller>();
}

Dumpsys* GetDumpsys() { return Stack::GetInstance()->GetStackManager()->GetInstance<Dumpsys>(); }

hci::HciInterface* GetHciLayer() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::HciLayer>();
}

hci::RemoteNameRequestModule* GetRemoteNameRequest() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::RemoteNameRequestModule>();
}

hci::LeScanningManager* GetScanning() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::LeScanningManager>();
}

hci::DistanceMeasurementManager* GetDistanceMeasurementManager() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::DistanceMeasurementManager>();
}

hal::SnoopLogger* GetSnoopLogger() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hal::SnoopLogger>();
}

lpp::LppOffloadInterface* GetLppOffloadManager() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<lpp::LppOffloadManager>();
}

storage::StorageModule* GetStorage() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<storage::StorageModule>();
}

hci::AclManager* GetAclManager() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::AclManager>();
}

metrics::CounterMetrics* GetCounterMetrics() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<metrics::CounterMetrics>();
}

hci::MsftExtensionManager* GetMsftExtensionManager() {
  return Stack::GetInstance()->GetStackManager()->GetInstance<hci::MsftExtensionManager>();
}

}  // namespace shim
}  // namespace bluetooth
