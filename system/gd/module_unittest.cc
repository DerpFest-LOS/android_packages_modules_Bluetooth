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

#include "module.h"

#include <unistd.h>

#include <functional>
#include <sstream>
#include <string>

#include "gtest/gtest.h"
#include "os/handler.h"
#include "os/thread.h"

using ::bluetooth::os::Thread;

namespace bluetooth {
namespace {

class ModuleTest : public ::testing::Test {
protected:
  void SetUp() override {
    thread_ = new Thread("test_thread", Thread::Priority::NORMAL);
    registry_ = new ModuleRegistry();
  }

  void TearDown() override {
    delete registry_;
    delete thread_;
  }

  ModuleRegistry* registry_;
  Thread* thread_;
};

os::Handler* test_module_no_dependency_handler = nullptr;

class TestModuleNoDependency : public Module {
public:
  static const ModuleFactory Factory;

protected:
  void ListDependencies(ModuleList* /* list */) const {}

  void Start() override {
    // A module is not considered started until Start() finishes
    EXPECT_FALSE(GetModuleRegistry()->IsStarted<TestModuleNoDependency>());
    test_module_no_dependency_handler = GetHandler();
  }

  void Stop() override {
    // A module is not considered stopped until after Stop() finishes
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleNoDependency>());
  }

  std::string ToString() const override { return std::string("TestModuleNoDependency"); }
};

const ModuleFactory TestModuleNoDependency::Factory =
        ModuleFactory([]() { return new TestModuleNoDependency(); });

os::Handler* test_module_one_dependency_handler = nullptr;

class TestModuleOneDependency : public Module {
public:
  static const ModuleFactory Factory;

protected:
  void ListDependencies(ModuleList* list) const { list->add<TestModuleNoDependency>(); }

  void Start() override {
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleNoDependency>());

    // A module is not considered started until Start() finishes
    EXPECT_FALSE(GetModuleRegistry()->IsStarted<TestModuleOneDependency>());
    test_module_one_dependency_handler = GetHandler();
  }

  void Stop() override {
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleNoDependency>());

    // A module is not considered stopped until after Stop() finishes
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleOneDependency>());
  }

  std::string ToString() const override { return std::string("TestModuleOneDependency"); }
};

const ModuleFactory TestModuleOneDependency::Factory =
        ModuleFactory([]() { return new TestModuleOneDependency(); });

class TestModuleNoDependencyTwo : public Module {
public:
  static const ModuleFactory Factory;

protected:
  void ListDependencies(ModuleList* /* list */) const {}

  void Start() override {
    // A module is not considered started until Start() finishes
    EXPECT_FALSE(GetModuleRegistry()->IsStarted<TestModuleNoDependencyTwo>());
  }

  void Stop() override {
    // A module is not considered stopped until after Stop() finishes
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleNoDependencyTwo>());
  }

  std::string ToString() const override { return std::string("TestModuleNoDependencyTwo"); }
};

const ModuleFactory TestModuleNoDependencyTwo::Factory =
        ModuleFactory([]() { return new TestModuleNoDependencyTwo(); });

class TestModuleTwoDependencies : public Module {
public:
  static const ModuleFactory Factory;

protected:
  void ListDependencies(ModuleList* list) const {
    list->add<TestModuleOneDependency>();
    list->add<TestModuleNoDependencyTwo>();
  }

  void Start() override {
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleOneDependency>());
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleNoDependencyTwo>());

    // A module is not considered started until Start() finishes
    EXPECT_FALSE(GetModuleRegistry()->IsStarted<TestModuleTwoDependencies>());
  }

  void Stop() override {
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleOneDependency>());
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleNoDependencyTwo>());

    // A module is not considered stopped until after Stop() finishes
    EXPECT_TRUE(GetModuleRegistry()->IsStarted<TestModuleTwoDependencies>());
  }

  std::string ToString() const override { return std::string("TestModuleTwoDependencies"); }
};

const ModuleFactory TestModuleTwoDependencies::Factory =
        ModuleFactory([]() { return new TestModuleTwoDependencies(); });

TEST_F(ModuleTest, no_dependency) {
  ModuleList list;
  list.add<TestModuleNoDependency>();
  registry_->Start(&list, thread_);

  EXPECT_TRUE(registry_->IsStarted<TestModuleNoDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleOneDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependencyTwo>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleTwoDependencies>());

  registry_->StopAll();

  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleOneDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependencyTwo>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleTwoDependencies>());
}

TEST_F(ModuleTest, one_dependency) {
  ModuleList list;
  list.add<TestModuleOneDependency>();
  registry_->Start(&list, thread_);

  EXPECT_TRUE(registry_->IsStarted<TestModuleNoDependency>());
  EXPECT_TRUE(registry_->IsStarted<TestModuleOneDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependencyTwo>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleTwoDependencies>());

  registry_->StopAll();

  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleOneDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependencyTwo>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleTwoDependencies>());
}

TEST_F(ModuleTest, two_dependencies) {
  ModuleList list;
  list.add<TestModuleTwoDependencies>();
  registry_->Start(&list, thread_);

  EXPECT_TRUE(registry_->IsStarted<TestModuleNoDependency>());
  EXPECT_TRUE(registry_->IsStarted<TestModuleOneDependency>());
  EXPECT_TRUE(registry_->IsStarted<TestModuleNoDependencyTwo>());
  EXPECT_TRUE(registry_->IsStarted<TestModuleTwoDependencies>());

  registry_->StopAll();

  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleOneDependency>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleNoDependencyTwo>());
  EXPECT_FALSE(registry_->IsStarted<TestModuleTwoDependencies>());
}

void post_to_module_one_handler() {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  test_module_one_dependency_handler->Post(common::BindOnce([] { FAIL(); }));
}

TEST_F(ModuleTest, shutdown_with_unhandled_callback) {
  ModuleList list;
  list.add<TestModuleOneDependency>();
  registry_->Start(&list, thread_);
  test_module_no_dependency_handler->Post(common::BindOnce(&post_to_module_one_handler));
  registry_->StopAll();
}

}  // namespace
}  // namespace bluetooth
