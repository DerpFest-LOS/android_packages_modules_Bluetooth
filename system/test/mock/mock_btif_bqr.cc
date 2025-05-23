/*
 * Copyright 2021 The Android Open Source Project
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

/*
 * Generated mock file from original source file
 *   Functions generated:51
 */

#include <cstdint>

#include "test/common/mock_functions.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

namespace bluetooth {
namespace bqr {

void DumpLmpLlMessage(uint8_t /* length */, const uint8_t* /* p_event */) {
  inc_func_call_count(__func__);
}

void DumpBtScheduling(uint8_t /* length */, const uint8_t* /* p_event */) {
  inc_func_call_count(__func__);
}

}  // namespace bqr
}  // namespace bluetooth
