/*
 *  Copyright 2022 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <future>

#include "module.h"
#include "os/handler.h"
#include "os/system_properties.h"
#include "os/thread.h"
#include "shim/dumpsys.h"
#include "stack_manager.h"

using namespace bluetooth;
using namespace testing;

class MainShimDumpsysTest : public testing::Test {
public:
protected:
  void SetUp() override {
    ModuleList modules;
    modules.add<shim::Dumpsys>();

    os::Thread* thread = new os::Thread("thread", os::Thread::Priority::NORMAL);
    stack_manager_.StartUp(&modules, thread);
  }
  void TearDown() override { stack_manager_.ShutDown(); }
  StackManager stack_manager_;

  os::Thread* thread_{nullptr};
  os::Handler* handler_{nullptr};
};

TEST_F(MainShimDumpsysTest, dumpsys) {
  std::promise<void> promise;
  auto future = promise.get_future();
  stack_manager_.GetInstance<shim::Dumpsys>()->Dump(STDOUT_FILENO, nullptr, std::move(promise));
  future.get();
}
