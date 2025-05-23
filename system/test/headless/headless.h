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

#pragma once

#include <bluetooth/log.h>
#include <unistd.h>

#include <unordered_map>

#include "include/hardware/bluetooth.h"
#include "test/headless/bt_stack_info.h"
#include "test/headless/get_options.h"
#include "test/headless/log.h"

extern bt_interface_t bluetoothInterface;

namespace bluetooth {
namespace test {
namespace headless {

template <typename T>
using ExecutionUnit = std::function<T()>;

constexpr char kHeadlessInitialSentinel[] =
        " INITIAL HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS";
constexpr char kHeadlessStartSentinel[] =
        " START HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS";
constexpr char kHeadlessStopSentinel[] =
        " STOP HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS";
constexpr char kHeadlessFinalSentinel[] =
        " FINAL HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS HEADLESS";

class HeadlessStack {
protected:
  HeadlessStack() {}
  virtual ~HeadlessStack() = default;

  void SetUp();
  void TearDown();

private:
  std::unique_ptr<BtStackInfo> bt_stack_info_;
};

class HeadlessRun : public HeadlessStack {
protected:
  const bluetooth::test::headless::GetOpt& options_;
  uint64_t loop_{0};

  HeadlessRun(const bluetooth::test::headless::GetOpt& options) : options_(options) {}

  template <typename T>
  T RunOnHeadlessStack(ExecutionUnit<T> func) {
    log::info("{}", kHeadlessInitialSentinel);
    SetUp();
    log::info("{}", kHeadlessStartSentinel);

    T rc;
    for (loop_ = 0; loop_ < options_.loop_; loop_++) {
      LOG_CONSOLE("Loop started: %lu", loop_);
      rc = func();
      if (options_.msec_ != 0) {
        usleep(options_.msec_ * 1000);
      }
      if (rc) {
        break;
      }
      LOG_CONSOLE("Loop completed: %lu", loop_);
    }
    if (rc) {
      log::error("FAIL:{} loop/loops:{}/{}", rc, loop_, options_.loop_);
    } else {
      log::info("PASS:{} loop/loops:{}/{}", rc, loop_, options_.loop_);
    }

    log::info("{}", kHeadlessStopSentinel);
    TearDown();
    log::info("{}", kHeadlessFinalSentinel);
    return rc;
  }
  virtual ~HeadlessRun() = default;
};

template <typename T>
class HeadlessTest : public HeadlessRun {
public:
  virtual T Run() {
    if (options_.non_options_.size() == 0) {
      fprintf(stdout, "Must supply at least one subtest name\n");
      return -1;
    }

    std::string subtest = options_.GetNextSubTest();
    if (test_nodes_.find(subtest) == test_nodes_.end()) {
      fprintf(stdout, "Unknown subtest module:%s\n", subtest.c_str());
      return -1;
    }
    return test_nodes_.at(subtest)->Run();
  }

  virtual ~HeadlessTest() = default;

protected:
  HeadlessTest(const bluetooth::test::headless::GetOpt& options) : HeadlessRun(options) {}

  std::unordered_map<std::string, std::unique_ptr<HeadlessTest<T>>> test_nodes_;
};

}  // namespace headless
}  // namespace test
}  // namespace bluetooth
