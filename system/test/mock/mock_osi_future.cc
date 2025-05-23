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
 *   Functions generated:4
 *
 *  mockcify.pl ver 0.3.0
 */
// Mock include file to share data between tests and mock
#include "test/mock/mock_osi_future.h"

#include "test/common/mock_functions.h"

// Mocked internal structures, if any

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

namespace test {
namespace mock {
namespace osi_future {

// Function state capture and return values, if needed
struct future_await future_await;
struct future_new future_new;
struct future_new_named future_new_named;
struct future_new_immediate future_new_immediate;
struct future_ready future_ready;

}  // namespace osi_future
}  // namespace mock
}  // namespace test

// Mocked functions, if any
void* future_await(future_t* future) {
  inc_func_call_count(__func__);
  return test::mock::osi_future::future_await(future);
}
future_t* future_new(void) {
  inc_func_call_count(__func__);
  return test::mock::osi_future::future_new();
}
future_t* future_new_named(const char* name) {
  inc_func_call_count(__func__);
  return test::mock::osi_future::future_new_named(name);
}
future_t* future_new_immediate(void* value) {
  inc_func_call_count(__func__);
  return test::mock::osi_future::future_new_immediate(value);
}
void future_ready(future_t* future, void* value) {
  inc_func_call_count(__func__);
  test::mock::osi_future::future_ready(future, value);
}
// Mocked functions complete
// END mockcify generation
