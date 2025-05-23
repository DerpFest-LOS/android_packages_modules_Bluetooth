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

#include "os/alarm.h"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cstring>

#include "common/bind.h"
#include "os/linux_generic/linux.h"
#include "os/utils.h"

#ifdef __ANDROID__
#define ALARM_CLOCK CLOCK_BOOTTIME_ALARM
#else
#define ALARM_CLOCK CLOCK_BOOTTIME
#endif

namespace bluetooth {
namespace os {
using common::Closure;
using common::OnceClosure;

Alarm::Alarm(Handler* handler) : Alarm(handler, true) {}

Alarm::Alarm(Handler* handler, bool isWakeAlarm) : handler_(handler) {
  int timerfd_flag =
          com::android::bluetooth::flags::non_wake_alarm_for_rpa_rotation() ? TFD_NONBLOCK : 0;

  fd_ = TIMERFD_CREATE(isWakeAlarm ? ALARM_CLOCK : CLOCK_BOOTTIME, timerfd_flag);

  log::assert_that(fd_ != -1, "cannot create timerfd: {}", strerror(errno));

  token_ = handler_->thread_->GetReactor()->Register(
          fd_, common::Bind(&Alarm::on_fire, common::Unretained(this)), Closure());
}

Alarm::~Alarm() {
  handler_->thread_->GetReactor()->Unregister(token_);

  int close_status;
  RUN_NO_INTR(close_status = TIMERFD_CLOSE(fd_));
  log::assert_that(close_status != -1, "assert failed: close_status != -1");
}

void Alarm::Schedule(OnceClosure task, std::chrono::milliseconds delay) {
  std::lock_guard<std::mutex> lock(mutex_);
  long delay_ms = delay.count();
  itimerspec timer_itimerspec{{/* interval for periodic timer */},
                              {delay_ms / 1000, delay_ms % 1000 * 1000000}};
  int result = TIMERFD_SETTIME(fd_, 0, &timer_itimerspec, nullptr);
  log::assert_that(result == 0, "assert failed: result == 0");

  task_ = std::move(task);
}

void Alarm::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  itimerspec disarm_itimerspec{/* disarm timer */};
  int result = TIMERFD_SETTIME(fd_, 0, &disarm_itimerspec, nullptr);
  log::assert_that(result == 0, "assert failed: result == 0");
}

void Alarm::on_fire() {
  std::unique_lock<std::mutex> lock(mutex_);
  auto task = std::move(task_);
  uint64_t times_invoked;
  auto bytes_read = read(fd_, &times_invoked, sizeof(uint64_t));
  lock.unlock();

  if (com::android::bluetooth::flags::non_wake_alarm_for_rpa_rotation() && bytes_read == -1) {
    log::debug("No data to read.");
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      log::debug("Alarm is already canceled or rescheduled.");
      return;
    }
  }

  log::assert_that(bytes_read == static_cast<ssize_t>(sizeof(uint64_t)),
                   "assert failed: bytes_read == static_cast<ssize_t>(sizeof(uint64_t))");
  log::assert_that(times_invoked == 1u, "Invoked number of times:{} fd:{}", times_invoked, fd_);
  std::move(task).Run();
}

}  // namespace os
}  // namespace bluetooth
