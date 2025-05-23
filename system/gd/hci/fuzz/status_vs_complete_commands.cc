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

#include "hci/fuzz/status_vs_complete_commands.h"

#include <map>

namespace bluetooth {
namespace hci {
namespace fuzz {

using ::bluetooth::hci::OpCode;

constexpr OpCode StatusOpCodes[] = {
        OpCode::RESET,
};

static std::map<OpCode, bool> commands_that_use_status;

static void maybe_populate_list() {
  if (!commands_that_use_status.empty()) {
    return;
  }

  for (OpCode code : StatusOpCodes) {
    commands_that_use_status[code] = true;
  }
}

bool uses_command_status(OpCode code) {
  maybe_populate_list();
  return commands_that_use_status.find(code) != commands_that_use_status.end();
}

bool uses_command_status_or_complete(OpCode code) {
  bool is_vendor_specific = (static_cast<int>(code) >> 10) == 0x3f;
  return is_vendor_specific;
}

}  // namespace fuzz
}  // namespace hci
}  // namespace bluetooth
