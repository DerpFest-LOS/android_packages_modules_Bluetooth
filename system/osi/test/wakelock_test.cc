/******************************************************************************
 *
 *  Copyright 2016 Google, Inc.
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
 *
 ******************************************************************************/

#include "osi/include/wakelock.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

static bool is_wake_lock_acquired = false;

static int acquire_wake_lock_cb(const char* /*lock_name*/) {
  is_wake_lock_acquired = true;
  return BT_STATUS_SUCCESS;
}

static int release_wake_lock_cb(const char* /*lock_name*/) {
  is_wake_lock_acquired = false;
  return BT_STATUS_SUCCESS;
}

static bt_os_callouts_t bt_wakelock_callouts = {sizeof(bt_os_callouts_t), acquire_wake_lock_cb,
                                                release_wake_lock_cb};

class WakelockTest : public ::testing::Test {
protected:
  void SetUp() override {
// TODO (jamuraa): maybe use base::CreateNewTempDirectory instead?
#ifdef __ANDROID__
    tmp_dir_ = "/data/local/tmp/btwlXXXXXX";
#else   // !__ANDROID__
    tmp_dir_ = "/tmp/btwlXXXXXX";
#endif  // __ANDROID__

    char* buffer = const_cast<char*>(tmp_dir_.c_str());
    char* dtemp = mkdtemp(buffer);
    ASSERT_NE(dtemp, nullptr) << "Can't make wake lock test directory";

    lock_path_ = tmp_dir_ + "/wake_lock";
    unlock_path_ = tmp_dir_ + "/wake_unlock";

    lock_path_fd = creat(lock_path_.c_str(), S_IRWXU);
    unlock_path_fd = creat(unlock_path_.c_str(), S_IRWXU);
  }

  int lock_path_fd{-1};
  int unlock_path_fd{-1};

  void TearDown() override {
    is_wake_lock_acquired = false;
    wakelock_cleanup();
    wakelock_set_os_callouts(NULL);

    // Clean up the temp wake lock directory
    unlink(lock_path_.c_str());
    unlink(unlock_path_.c_str());
    rmdir(tmp_dir_.c_str());

    close(lock_path_fd);
    close(unlock_path_fd);
  }

  //
  // Test whether the file-based wakelock is acquired.
  //
  bool IsFileWakeLockAcquired() {
    bool acquired = false;

    int lock_fd = open(lock_path_.c_str(), O_RDONLY);
    EXPECT_GE(lock_fd, 0);

    int unlock_fd = open(unlock_path_.c_str(), O_RDONLY);
    EXPECT_GE(unlock_fd, 0);

    struct stat lock_stat, unlock_stat;
    fstat(lock_fd, &lock_stat);
    fstat(unlock_fd, &unlock_stat);

    EXPECT_GE(lock_stat.st_size, unlock_stat.st_size);

    void* lock_file = mmap(nullptr, lock_stat.st_size, PROT_READ, MAP_PRIVATE, lock_fd, 0);

    void* unlock_file = mmap(nullptr, unlock_stat.st_size, PROT_READ, MAP_PRIVATE, unlock_fd, 0);

    if (memcmp(lock_file, unlock_file, unlock_stat.st_size) == 0) {
      acquired = lock_stat.st_size > unlock_stat.st_size;
    } else {
      // these files should always either be with a lock that has more,
      // or equal.
      ADD_FAILURE();
    }

    munmap(lock_file, lock_stat.st_size);
    munmap(unlock_file, unlock_stat.st_size);
    close(lock_fd);
    close(unlock_fd);

    return acquired;
  }

  std::string tmp_dir_;
  std::string lock_path_;
  std::string unlock_path_;
};

TEST_F(WakelockTest, test_set_os_callouts) {
  wakelock_set_os_callouts(&bt_wakelock_callouts);

  // Initially, the wakelock is not acquired
  ASSERT_FALSE(is_wake_lock_acquired);

  for (size_t i = 0; i < 1000; i++) {
    wakelock_acquire();
    ASSERT_TRUE(is_wake_lock_acquired);
    wakelock_release();
    ASSERT_FALSE(is_wake_lock_acquired);
  }
}

TEST_F(WakelockTest, test_set_paths) {
  wakelock_set_os_callouts(NULL);  // Make sure we use native wakelocks
  wakelock_set_paths(lock_path_.c_str(), unlock_path_.c_str());

  // Initially, the wakelock is not acquired
  ASSERT_FALSE(IsFileWakeLockAcquired());

  for (size_t i = 0; i < 1000; i++) {
    wakelock_acquire();
    ASSERT_TRUE(IsFileWakeLockAcquired());
    wakelock_release();
    ASSERT_FALSE(IsFileWakeLockAcquired());
  }
}
