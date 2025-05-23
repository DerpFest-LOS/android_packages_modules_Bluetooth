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

#include "os/parameter_provider.h"

#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <string>

namespace bluetooth {
namespace os {

namespace {
std::mutex parameter_mutex;
std::string config_file_path;
std::string snoop_log_file_path;
std::string snooz_log_file_path;
std::string sysprops_file_path;
}  // namespace

// Write to $PWD/bt_stack.conf if $PWD can be found, otherwise, write to $HOME/bt_stack.conf
std::string ParameterProvider::ConfigFilePath() {
  {
    std::lock_guard<std::mutex> lock(parameter_mutex);
    if (!config_file_path.empty()) {
      return config_file_path;
    }
  }
  return "/var/lib/bluetooth/bt_config.conf";
}

void ParameterProvider::OverrideConfigFilePath(const std::string& path) {
  std::lock_guard<std::mutex> lock(parameter_mutex);
  config_file_path = path;
}

std::string ParameterProvider::SnoopLogFilePath() {
  {
    std::lock_guard<std::mutex> lock(parameter_mutex);
    if (!snoop_log_file_path.empty()) {
      return snoop_log_file_path;
    }
  }

  return "/var/log/bluetooth/btsnoop_hci.log";
}

void ParameterProvider::OverrideSnoopLogFilePath(const std::string& path) {
  std::lock_guard<std::mutex> lock(parameter_mutex);
  snoop_log_file_path = path;
}

std::string ParameterProvider::SnoozLogFilePath() {
  {
    std::lock_guard<std::mutex> lock(parameter_mutex);
    if (!snooz_log_file_path.empty()) {
      return snooz_log_file_path;
    }
  }
  return "/var/log/bluetooth/btsnooz_hci.log";
}

std::string ParameterProvider::SyspropsFilePath() {
  {
    std::lock_guard<std::mutex> lock(parameter_mutex);
    if (!sysprops_file_path.empty()) {
      return sysprops_file_path;
    }
  }
  return "/var/lib/bluetooth/sysprops.conf";
}

void ParameterProvider::OverrideSyspropsFilePath(const std::string& path) {
  std::lock_guard<std::mutex> lock(parameter_mutex);
  sysprops_file_path = path;
}

bluetooth_keystore::BluetoothKeystoreInterface* ParameterProvider::GetBtKeystoreInterface() {
  return nullptr;
}

void ParameterProvider::SetBtKeystoreInterface(
        bluetooth_keystore::BluetoothKeystoreInterface* bt_keystore) {}

bool ParameterProvider::IsCommonCriteriaMode() { return false; }

void ParameterProvider::SetCommonCriteriaMode(bool enable) {}

int ParameterProvider::GetCommonCriteriaConfigCompareResult() { return 0b11; }

void ParameterProvider::SetCommonCriteriaConfigCompareResult(int result) {}

}  // namespace os
}  // namespace bluetooth
