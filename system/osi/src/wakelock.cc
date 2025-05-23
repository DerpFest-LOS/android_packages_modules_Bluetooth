/******************************************************************************
 *
 *  Copyright 2015 Google, Inc.
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

#define LOG_TAG "bt_osi_wakelock"

#include "osi/include/wakelock.h"

#include <bluetooth/log.h>
#include <fcntl.h>
#include <hardware/bluetooth.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <string>

#include "common/metrics.h"
#include "osi/include/osi.h"

using bluetooth::common::BluetoothMetricsLogger;
using namespace bluetooth;

static bt_os_callouts_t* wakelock_os_callouts = NULL;
static bool is_native = true;

static const clockid_t CLOCK_ID = CLOCK_BOOTTIME;
static const char* WAKE_LOCK_ID = "bluetooth_timer";
static const std::string DEFAULT_WAKE_LOCK_PATH = "/sys/power/wake_lock";
static const std::string DEFAULT_WAKE_UNLOCK_PATH = "/sys/power/wake_unlock";
static std::string wake_lock_path;
static std::string wake_unlock_path;
static ssize_t locked_id_len = -1;
static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static int wake_lock_fd = INVALID_FD;
static int wake_unlock_fd = INVALID_FD;

// Wakelock statistics for the "bluetooth_timer"
typedef struct {
  bool is_acquired;
  size_t acquired_count;
  size_t released_count;
  size_t acquired_errors;
  size_t released_errors;
  uint64_t min_acquired_interval_ms;
  uint64_t max_acquired_interval_ms;
  uint64_t last_acquired_interval_ms;
  uint64_t total_acquired_interval_ms;
  uint64_t last_acquired_timestamp_ms;
  uint64_t last_released_timestamp_ms;
  uint64_t last_reset_timestamp_ms;
  int last_acquired_error;
  int last_released_error;
} wakelock_stats_t;

static wakelock_stats_t wakelock_stats;

// This mutex ensures that the functions that update and dump the statistics
// are executed serially.
static std::mutex stats_mutex;

static bt_status_t wakelock_acquire_callout(void);
static bt_status_t wakelock_acquire_native(void);
static bt_status_t wakelock_release_callout(void);
static bt_status_t wakelock_release_native(void);
static void wakelock_initialize(void);
static void wakelock_initialize_native(void);
static void reset_wakelock_stats(void);
static void update_wakelock_acquired_stats(bt_status_t acquired_status);
static void update_wakelock_released_stats(bt_status_t released_status);

void wakelock_set_os_callouts(bt_os_callouts_t* callouts) {
  wakelock_os_callouts = callouts;
  is_native = (wakelock_os_callouts == NULL);
  log::info("set to {}", (is_native) ? "native" : "non-native");
}

bool wakelock_acquire(void) {
  pthread_once(&initialized, wakelock_initialize);

  bt_status_t status = BT_STATUS_FAIL;

  if (is_native) {
    status = wakelock_acquire_native();
  } else {
    status = wakelock_acquire_callout();
  }

  update_wakelock_acquired_stats(status);

  if (status != BT_STATUS_SUCCESS) {
    log::error("unable to acquire wake lock: {}", status);
  }

  return status == BT_STATUS_SUCCESS;
}

static bt_status_t wakelock_acquire_callout(void) {
  return static_cast<bt_status_t>(wakelock_os_callouts->acquire_wake_lock(WAKE_LOCK_ID));
}

static bt_status_t wakelock_acquire_native(void) {
  if (wake_lock_fd == INVALID_FD) {
    log::error("lock not acquired, invalid fd");
    return BT_STATUS_PARM_INVALID;
  }

  if (wake_unlock_fd == INVALID_FD) {
    log::error("not acquiring lock: can't release lock");
    return BT_STATUS_PARM_INVALID;
  }

  long lock_name_len = strlen(WAKE_LOCK_ID);
  locked_id_len = write(wake_lock_fd, WAKE_LOCK_ID, lock_name_len);
  if (locked_id_len == -1) {
    log::error("wake lock not acquired: {}", strerror(errno));
    return BT_STATUS_WAKELOCK_ERROR;
  } else if (locked_id_len < lock_name_len) {
    // TODO (jamuraa): this is weird. maybe we should release and retry.
    log::warn("wake lock truncated to {} chars", locked_id_len);
  }
  return BT_STATUS_SUCCESS;
}

bool wakelock_release(void) {
  pthread_once(&initialized, wakelock_initialize);

  bt_status_t status = BT_STATUS_FAIL;

  if (is_native) {
    status = wakelock_release_native();
  } else {
    status = wakelock_release_callout();
  }

  update_wakelock_released_stats(status);

  return status == BT_STATUS_SUCCESS;
}

static bt_status_t wakelock_release_callout(void) {
  return static_cast<bt_status_t>(wakelock_os_callouts->release_wake_lock(WAKE_LOCK_ID));
}

static bt_status_t wakelock_release_native(void) {
  if (wake_unlock_fd == INVALID_FD) {
    log::error("lock not released, invalid fd");
    return BT_STATUS_PARM_INVALID;
  }

  ssize_t wrote_name_len = write(wake_unlock_fd, WAKE_LOCK_ID, locked_id_len);
  if (wrote_name_len == -1) {
    log::error("can't release wake lock: {}", strerror(errno));
  } else if (wrote_name_len < locked_id_len) {
    log::error("lock release only wrote {}, assuming released", wrote_name_len);
  }
  return BT_STATUS_SUCCESS;
}

static void wakelock_initialize(void) {
  reset_wakelock_stats();

  if (is_native) {
    wakelock_initialize_native();
  }
}

static void wakelock_initialize_native(void) {
  log::info("opening wake locks");

  if (wake_lock_path.empty()) {
    wake_lock_path = DEFAULT_WAKE_LOCK_PATH;
  }

  wake_lock_fd = open(wake_lock_path.c_str(), O_RDWR | O_CLOEXEC);
  if (wake_lock_fd == INVALID_FD) {
    log::error("can't open wake lock {}: {}", wake_lock_path, strerror(errno));
  }

  if (wake_unlock_path.empty()) {
    wake_unlock_path = DEFAULT_WAKE_UNLOCK_PATH;
  }

  wake_unlock_fd = open(wake_unlock_path.c_str(), O_RDWR | O_CLOEXEC);
  if (wake_unlock_fd == INVALID_FD) {
    log::error("can't open wake unlock {}: {}", wake_unlock_path, strerror(errno));
  }
}

void wakelock_cleanup(void) {
  if (wakelock_stats.is_acquired) {
    log::error("releasing wake lock as part of cleanup");
    wakelock_release();
  }
  wake_lock_path.clear();
  wake_unlock_path.clear();
  initialized = PTHREAD_ONCE_INIT;
}

void wakelock_set_paths(const char* lock_path, const char* unlock_path) {
  if (lock_path) {
    wake_lock_path = lock_path;
  }

  if (unlock_path) {
    wake_unlock_path = unlock_path;
  }
}

static uint64_t now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_ID, &ts) == -1) {
    log::error("unable to get current time: {}", strerror(errno));
    return 0;
  }

  return (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

// Reset the Bluetooth wakelock statistics.
// This function is thread-safe.
static void reset_wakelock_stats(void) {
  std::lock_guard<std::mutex> lock(stats_mutex);

  wakelock_stats.is_acquired = false;
  wakelock_stats.acquired_count = 0;
  wakelock_stats.released_count = 0;
  wakelock_stats.acquired_errors = 0;
  wakelock_stats.released_errors = 0;
  wakelock_stats.min_acquired_interval_ms = 0;
  wakelock_stats.max_acquired_interval_ms = 0;
  wakelock_stats.last_acquired_interval_ms = 0;
  wakelock_stats.total_acquired_interval_ms = 0;
  wakelock_stats.last_acquired_timestamp_ms = 0;
  wakelock_stats.last_released_timestamp_ms = 0;
  wakelock_stats.last_reset_timestamp_ms = now_ms();
}

//
// Update the Bluetooth acquire wakelock statistics.
//
// This function should be called every time when the wakelock is acquired.
// |acquired_status| is the status code that was return when the wakelock was
// acquired.
// This function is thread-safe.
//
static void update_wakelock_acquired_stats(bt_status_t acquired_status) {
  const uint64_t just_now_ms = now_ms();

  std::lock_guard<std::mutex> lock(stats_mutex);

  if (acquired_status != BT_STATUS_SUCCESS) {
    wakelock_stats.acquired_errors++;
    wakelock_stats.last_acquired_error = acquired_status;
  }

  if (wakelock_stats.is_acquired) {
    return;
  }

  wakelock_stats.is_acquired = true;
  wakelock_stats.acquired_count++;
  wakelock_stats.last_acquired_timestamp_ms = just_now_ms;

  BluetoothMetricsLogger::GetInstance()->LogWakeEvent(bluetooth::common::WAKE_EVENT_ACQUIRED, "",
                                                      "", just_now_ms);
}

//
// Update the Bluetooth release wakelock statistics.
//
// This function should be called every time when the wakelock is released.
// |released_status| is the status code that was return when the wakelock was
// released.
// This function is thread-safe.
//
static void update_wakelock_released_stats(bt_status_t released_status) {
  const uint64_t just_now_ms = now_ms();

  std::lock_guard<std::mutex> lock(stats_mutex);

  if (released_status != BT_STATUS_SUCCESS) {
    wakelock_stats.released_errors++;
    wakelock_stats.last_released_error = released_status;
  }

  if (!wakelock_stats.is_acquired) {
    return;
  }

  wakelock_stats.is_acquired = false;
  wakelock_stats.released_count++;
  wakelock_stats.last_released_timestamp_ms = just_now_ms;

  // Compute the acquired interval and update the statistics
  uint64_t delta_ms = just_now_ms - wakelock_stats.last_acquired_timestamp_ms;
  if (delta_ms < wakelock_stats.min_acquired_interval_ms || wakelock_stats.released_count == 1) {
    wakelock_stats.min_acquired_interval_ms = delta_ms;
  }
  if (delta_ms > wakelock_stats.max_acquired_interval_ms) {
    wakelock_stats.max_acquired_interval_ms = delta_ms;
  }
  wakelock_stats.last_acquired_interval_ms = delta_ms;
  wakelock_stats.total_acquired_interval_ms += delta_ms;

  BluetoothMetricsLogger::GetInstance()->LogWakeEvent(bluetooth::common::WAKE_EVENT_RELEASED, "",
                                                      "", just_now_ms);
}

void wakelock_debug_dump(int fd) {
  const uint64_t just_now_ms = now_ms();

  std::lock_guard<std::mutex> lock(stats_mutex);

  // Compute the last acquired interval if the wakelock is still acquired
  uint64_t delta_ms = 0;
  uint64_t last_interval_ms = wakelock_stats.last_acquired_interval_ms;
  uint64_t min_interval_ms = wakelock_stats.min_acquired_interval_ms;
  uint64_t max_interval_ms = wakelock_stats.max_acquired_interval_ms;
  uint64_t avg_interval_ms = 0;

  if (wakelock_stats.is_acquired) {
    delta_ms = just_now_ms - wakelock_stats.last_acquired_timestamp_ms;
    if (delta_ms > max_interval_ms) {
      max_interval_ms = delta_ms;
    }
    if (delta_ms < min_interval_ms) {
      min_interval_ms = delta_ms;
    }
    last_interval_ms = delta_ms;
  }
  uint64_t total_interval_ms = wakelock_stats.total_acquired_interval_ms + delta_ms;

  if (wakelock_stats.acquired_count > 0) {
    avg_interval_ms = total_interval_ms / wakelock_stats.acquired_count;
  }

  dprintf(fd, "\nBluetooth Wakelock Statistics:\n");
  dprintf(fd, "  Is acquired                    : %s\n",
          wakelock_stats.is_acquired ? "true" : "false");
  dprintf(fd, "  Acquired/released count        : %zu / %zu\n", wakelock_stats.acquired_count,
          wakelock_stats.released_count);
  dprintf(fd, "  Acquired/released error count  : %zu / %zu\n", wakelock_stats.acquired_errors,
          wakelock_stats.released_errors);
  dprintf(fd, "  Last acquire/release error code: %d / %d\n", wakelock_stats.last_acquired_error,
          wakelock_stats.last_released_error);
  dprintf(fd, "  Last acquired time (ms)        : %llu\n", (unsigned long long)last_interval_ms);
  dprintf(fd, "  Acquired time min/max/avg (ms) : %llu / %llu / %llu\n",
          (unsigned long long)min_interval_ms, (unsigned long long)max_interval_ms,
          (unsigned long long)avg_interval_ms);
  dprintf(fd, "  Total acquired time (ms)       : %llu\n", (unsigned long long)total_interval_ms);
  dprintf(fd, "  Total run time (ms)            : %llu\n",
          (unsigned long long)(just_now_ms - wakelock_stats.last_reset_timestamp_ms));
}
