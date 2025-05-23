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

#include "storage/legacy_config_file.h"

#include <bluetooth/log.h>

#include <cerrno>
#include <fstream>
#include <sstream>

#include "common/strings.h"
#include "os/files.h"
#include "storage/device.h"

namespace bluetooth {
namespace storage {

LegacyConfigFile::LegacyConfigFile(std::string path) : path_(std::move(path)) {
  log::assert_that(!path_.empty(), "assert failed: !path_.empty()");
}

std::optional<ConfigCache> LegacyConfigFile::Read(size_t temp_devices_capacity) {
  log::assert_that(!path_.empty(), "assert failed: !path_.empty()");
  std::ifstream config_file(path_);
  if (!config_file || !config_file.is_open()) {
    log::error("unable to open file '{}', error: {}", path_, strerror(errno));
    return std::nullopt;
  }
  [[maybe_unused]] int line_num = 0;
  ConfigCache cache(temp_devices_capacity, Device::kLinkKeyProperties);
  std::string line;
  std::string section(ConfigCache::kDefaultSectionName);
  while (std::getline(config_file, line)) {
    ++line_num;
    line = common::StringTrim(std::move(line));
    if (line.empty()) {
      continue;
    }

    if (line.front() == '\0' || line.front() == '#') {
      continue;
    }
    if (line.front() == '[') {
      if (line.back() != ']') {
        log::warn("unterminated section name on line {}", line_num);
        return std::nullopt;
      }
      // Read 'test' from '[text]', hence -2
      section = line.substr(1, line.size() - 2);
    } else {
      auto tokens = common::StringSplit(line, "=", 2);
      if (tokens.size() != 2) {
        log::warn("no key/value separator found on line {}", line_num);
        return std::nullopt;
      }
      tokens[0] = common::StringTrim(std::move(tokens[0]));
      tokens[1] = common::StringTrim(std::move(tokens[1]));
      cache.SetProperty(section, tokens[0], std::move(tokens[1]));
    }
  }
  return cache;
}

bool LegacyConfigFile::Write(const ConfigCache& cache) {
  return os::WriteToFile(path_, cache.SerializeToLegacyFormat());
}

bool LegacyConfigFile::Delete() {
  if (!os::FileExists(path_)) {
    log::warn("Config file at \"{}\" does not exist", path_);
    return false;
  }
  return os::RemoveFile(path_);
}

}  // namespace storage
}  // namespace bluetooth
