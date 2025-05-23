/******************************************************************************
 *
 *  Copyright 2014 Google, Inc.
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

#define LOG_TAG "bt_osi_alarm"

#include "osi/include/alarm.h"

#include <android_bluetooth_sysprop.h>
#include <base/cancelable_callback.h>
#include <bluetooth/log.h>
#include <fcntl.h>
#include <hardware/bluetooth.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include <mutex>

#include "osi/include/allocator.h"
#include "osi/include/fixed_queue.h"
#include "osi/include/list.h"
#include "osi/include/thread.h"
#include "osi/include/wakelock.h"
#include "osi/semaphore.h"
#include "stack/include/main_thread.h"

using base::Bind;
using base::CancelableClosure;
using namespace bluetooth;

// Callback and timer threads should run at RT priority in order to ensure they
// meet audio deadlines.  Use this priority for all audio/timer related thread.
static const int THREAD_RT_PRIORITY = 1;

typedef struct {
  size_t count;
  uint64_t total_ms;
  uint64_t max_ms;
} stat_t;

// Alarm-related information and statistics
typedef struct {
  const char* name;
  size_t scheduled_count;
  size_t canceled_count;
  size_t rescheduled_count;
  size_t total_updates;
  uint64_t last_update_ms;
  stat_t overdue_scheduling;
  stat_t premature_scheduling;
} alarm_stats_t;

/* Wrapper around CancellableClosure that let it be embedded in structs, without
 * need to define copy operator. */
struct CancelableClosureInStruct {
  base::CancelableClosure i;

  CancelableClosureInStruct& operator=(const CancelableClosureInStruct& in) {
    if (!in.i.callback().is_null()) {
      i.Reset(in.i.callback());
    }
    return *this;
  }
};

struct alarm_t {
  // The mutex is held while the callback for this alarm is being executed.
  // It allows us to release the coarse-grained monitor lock while a
  // potentially long-running callback is executing. |alarm_cancel| uses this
  // mutex to provide a guarantee to its caller that the callback will not be
  // in progress when it returns.
  std::shared_ptr<std::recursive_mutex> callback_mutex;
  uint64_t creation_time_ms;
  uint64_t period_ms;
  uint64_t deadline_ms;
  uint64_t prev_deadline_ms;  // Previous deadline - used for accounting of
                              // periodic timers
  bool is_periodic;
  fixed_queue_t* queue;  // The processing queue to add this alarm to
  alarm_callback_t callback;
  void* data;
  alarm_stats_t stats;

  bool for_msg_loop;                  // True, if the alarm should be processed on message loop
  CancelableClosureInStruct closure;  // posted to message loop for processing
};

// If the next wakeup time is less than this threshold, we should acquire
// a wakelock instead of setting a wake alarm so we're not bouncing in
// and out of suspend frequently. This value is externally visible to allow
// unit tests to run faster. It should not be modified by production code.
int64_t TIMER_INTERVAL_FOR_WAKELOCK_IN_MS = 3000;
static const clockid_t CLOCK_ID = CLOCK_BOOTTIME;

// This mutex ensures that the |alarm_set|, |alarm_cancel|, and alarm callback
// functions execute serially and not concurrently. As a result, this mutex
// also protects the |alarms| list.
static std::mutex alarms_mutex;
static list_t* alarms;
static timer_t timer;
static timer_t wakeup_timer;
static bool timer_set;

// All alarm callbacks are dispatched from |dispatcher_thread|
static thread_t* dispatcher_thread;
static bool dispatcher_thread_active;
static semaphore_t* alarm_expired;

// Default alarm callback thread and queue
static thread_t* default_callback_thread;
static fixed_queue_t* default_callback_queue;

static alarm_t* alarm_new_internal(const char* name, bool is_periodic);
static bool lazy_initialize(void);
static uint64_t now_ms(void);
static void alarm_set_internal(alarm_t* alarm, uint64_t period_ms, alarm_callback_t cb, void* data,
                               fixed_queue_t* queue, bool for_msg_loop);
static void alarm_cancel_internal(alarm_t* alarm);
static void remove_pending_alarm(alarm_t* alarm);
static void schedule_next_instance(alarm_t* alarm);
static void reschedule_root_alarm(void);
static void alarm_queue_ready(fixed_queue_t* queue, void* context);
static void timer_callback(void* data);
static void callback_dispatch(void* context);
static bool timer_create_internal(const clockid_t clock_id, timer_t* timer);
static void update_scheduling_stats(alarm_stats_t* stats, uint64_t now_ms, uint64_t deadline_ms);
// Registers |queue| for processing alarm callbacks on |thread|.
// |queue| may not be NULL. |thread| may not be NULL.
static void alarm_register_processing_queue(fixed_queue_t* queue, thread_t* thread);

static void update_stat(stat_t* stat, uint64_t delta_ms) {
  if (stat->max_ms < delta_ms) {
    stat->max_ms = delta_ms;
  }
  stat->total_ms += delta_ms;
  stat->count++;
}

alarm_t* alarm_new(const char* name) { return alarm_new_internal(name, false); }

alarm_t* alarm_new_periodic(const char* name) { return alarm_new_internal(name, true); }

static alarm_t* alarm_new_internal(const char* name, bool is_periodic) {
  // Make sure we have a list we can insert alarms into.
  if (!alarms && !lazy_initialize()) {
    log::fatal("initialization failed");  // if initialization failed, we
                                          // should not continue
    return NULL;
  }

  alarm_t* ret = static_cast<alarm_t*>(osi_calloc(sizeof(alarm_t)));

  std::shared_ptr<std::recursive_mutex> ptr(new std::recursive_mutex());
  ret->callback_mutex = ptr;
  ret->is_periodic = is_periodic;
  ret->stats.name = osi_strdup(name);

  ret->for_msg_loop = false;
  // placement new
  new (&ret->closure) CancelableClosureInStruct();

  // NOTE: The stats were reset by osi_calloc() above

  return ret;
}

void alarm_free(alarm_t* alarm) {
  if (!alarm) {
    return;
  }

  alarm_cancel(alarm);

  osi_free((void*)alarm->stats.name);
  alarm->closure.~CancelableClosureInStruct();
  alarm->callback_mutex.reset();
  osi_free(alarm);
}

uint64_t alarm_get_remaining_ms(const alarm_t* alarm) {
  log::assert_that(alarm != NULL, "assert failed: alarm != NULL");
  uint64_t remaining_ms = 0;
  uint64_t just_now_ms = now_ms();

  std::lock_guard<std::mutex> lock(alarms_mutex);
  if (alarm->deadline_ms > just_now_ms) {
    remaining_ms = alarm->deadline_ms - just_now_ms;
  }

  return remaining_ms;
}

void alarm_set(alarm_t* alarm, uint64_t interval_ms, alarm_callback_t cb, void* data) {
  alarm_set_internal(alarm, interval_ms, cb, data, default_callback_queue, false);
}

void alarm_set_on_mloop(alarm_t* alarm, uint64_t interval_ms, alarm_callback_t cb, void* data) {
  alarm_set_internal(alarm, interval_ms, cb, data, NULL, true);
}

// Runs in exclusion with alarm_cancel and timer_callback.
static void alarm_set_internal(alarm_t* alarm, uint64_t period_ms, alarm_callback_t cb, void* data,
                               fixed_queue_t* queue, bool for_msg_loop) {
  log::assert_that(alarms != NULL, "assert failed: alarms != NULL");
  log::assert_that(alarm != NULL, "assert failed: alarm != NULL");
  log::assert_that(cb != NULL, "assert failed: cb != NULL");

  std::lock_guard<std::mutex> lock(alarms_mutex);

  alarm->creation_time_ms = now_ms();
  alarm->period_ms = period_ms;
  alarm->queue = queue;
  alarm->callback = cb;
  alarm->data = data;
  alarm->for_msg_loop = for_msg_loop;

  schedule_next_instance(alarm);
  alarm->stats.scheduled_count++;
}

void alarm_cancel(alarm_t* alarm) {
  log::assert_that(alarms != NULL, "assert failed: alarms != NULL");
  if (!alarm) {
    return;
  }

  std::shared_ptr<std::recursive_mutex> local_mutex_ref;
  {
    std::lock_guard<std::mutex> lock(alarms_mutex);
    local_mutex_ref = alarm->callback_mutex;
    alarm_cancel_internal(alarm);
  }

  // If the callback for |alarm| is in progress, wait here until it completes.
  std::lock_guard<std::recursive_mutex> lock(*local_mutex_ref);
}

// Internal implementation of canceling an alarm.
// The caller must hold the |alarms_mutex|
static void alarm_cancel_internal(alarm_t* alarm) {
  bool needs_reschedule = (!list_is_empty(alarms) && list_front(alarms) == alarm);

  remove_pending_alarm(alarm);

  alarm->deadline_ms = 0;
  alarm->prev_deadline_ms = 0;
  alarm->callback = NULL;
  alarm->data = NULL;
  alarm->stats.canceled_count++;
  alarm->queue = NULL;

  if (needs_reschedule) {
    reschedule_root_alarm();
  }
}

bool alarm_is_scheduled(const alarm_t* alarm) {
  if ((alarms == NULL) || (alarm == NULL)) {
    return false;
  }
  return alarm->callback != NULL;
}

void alarm_cleanup(void) {
  // If lazy_initialize never ran there is nothing else to do
  if (!alarms) {
    return;
  }

  dispatcher_thread_active = false;
  semaphore_post(alarm_expired);
  thread_free(dispatcher_thread);
  dispatcher_thread = NULL;

  std::lock_guard<std::mutex> lock(alarms_mutex);

  fixed_queue_free(default_callback_queue, NULL);
  default_callback_queue = NULL;
  thread_free(default_callback_thread);
  default_callback_thread = NULL;

  timer_delete(wakeup_timer);
  timer_delete(timer);
  semaphore_free(alarm_expired);
  alarm_expired = NULL;

  list_free(alarms);
  alarms = NULL;
}

static bool lazy_initialize(void) {
  log::assert_that(alarms == NULL, "assert failed: alarms == NULL");

  // timer_t doesn't have an invalid value so we must track whether
  // the |timer| variable is valid ourselves.
  bool timer_initialized = false;
  bool wakeup_timer_initialized = false;

  // some platforms are not wired up to be woken up by the controller.
  // on those platforms, if we go to sleep with a timer armed, it will
  // continue counting during sleep. to prevent unwanted timer fires on
  // those platforms, use CLOCK_MONOTONIC and don't count up during sleep.
  bool wakeup_supported = android::sysprop::bluetooth::hardware::wakeup_supported().value_or(true);
  clockid_t alarm_clockid = wakeup_supported ? CLOCK_BOOTTIME_ALARM : CLOCK_MONOTONIC;

  std::lock_guard<std::mutex> lock(alarms_mutex);

  alarms = list_new(NULL);
  if (!alarms) {
    log::error("unable to allocate alarm list.");
    goto error;
  }

  if (!timer_create_internal(CLOCK_ID, &timer)) {
    goto error;
  }
  timer_initialized = true;

  if (!timer_create_internal(alarm_clockid, &wakeup_timer)) {
    if (!timer_create_internal(CLOCK_BOOTTIME, &wakeup_timer)) {
      goto error;
    }
  }
  wakeup_timer_initialized = true;

  alarm_expired = semaphore_new(0);
  if (!alarm_expired) {
    log::error("unable to create alarm expired semaphore");
    goto error;
  }

  default_callback_thread = thread_new_sized("alarm_default_callbacks", SIZE_MAX);
  if (default_callback_thread == NULL) {
    log::error("unable to create default alarm callbacks thread.");
    goto error;
  }
  thread_set_rt_priority(default_callback_thread, THREAD_RT_PRIORITY);
  default_callback_queue = fixed_queue_new(SIZE_MAX);
  if (default_callback_queue == NULL) {
    log::error("unable to create default alarm callbacks queue.");
    goto error;
  }
  alarm_register_processing_queue(default_callback_queue, default_callback_thread);

  dispatcher_thread_active = true;
  dispatcher_thread = thread_new("alarm_dispatcher");
  if (!dispatcher_thread) {
    log::error("unable to create alarm callback thread.");
    goto error;
  }
  thread_set_rt_priority(dispatcher_thread, THREAD_RT_PRIORITY);
  thread_post(dispatcher_thread, callback_dispatch, NULL);
  return true;

error:
  fixed_queue_free(default_callback_queue, NULL);
  default_callback_queue = NULL;
  thread_free(default_callback_thread);
  default_callback_thread = NULL;

  thread_free(dispatcher_thread);
  dispatcher_thread = NULL;

  dispatcher_thread_active = false;

  semaphore_free(alarm_expired);
  alarm_expired = NULL;

  if (wakeup_timer_initialized) {
    timer_delete(wakeup_timer);
  }

  if (timer_initialized) {
    timer_delete(timer);
  }

  list_free(alarms);
  alarms = NULL;

  return false;
}

static uint64_t now_ms(void) {
  log::assert_that(alarms != NULL, "assert failed: alarms != NULL");

  struct timespec ts;
  if (clock_gettime(CLOCK_ID, &ts) == -1) {
    log::error("unable to get current time: {}", strerror(errno));
    return 0;
  }

  return (ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

// Remove alarm from internal alarm list and the processing queue
// The caller must hold the |alarms_mutex|
static void remove_pending_alarm(alarm_t* alarm) {
  list_remove(alarms, alarm);

  if (alarm->for_msg_loop) {
    alarm->closure.i.Cancel();
  } else {
    while (fixed_queue_try_remove_from_queue(alarm->queue, alarm) != NULL) {
      // Remove all repeated alarm instances from the queue.
      // NOTE: We are defensive here - we shouldn't have repeated alarm
      // instances
    }
  }
}

// Must be called with |alarms_mutex| held
static void schedule_next_instance(alarm_t* alarm) {
  // If the alarm is currently set and it's at the start of the list,
  // we'll need to re-schedule since we've adjusted the earliest deadline.
  bool needs_reschedule = (!list_is_empty(alarms) && list_front(alarms) == alarm);
  if (alarm->callback) {
    remove_pending_alarm(alarm);
  }

  // Calculate the next deadline for this alarm
  uint64_t just_now_ms = now_ms();
  uint64_t ms_into_period = 0;
  if ((alarm->is_periodic) && (alarm->period_ms != 0)) {
    ms_into_period = ((just_now_ms - alarm->creation_time_ms) % alarm->period_ms);
  }
  alarm->deadline_ms = just_now_ms + (alarm->period_ms - ms_into_period);

  // Add it into the timer list sorted by deadline (earliest deadline first).
  if (list_is_empty(alarms) || ((alarm_t*)list_front(alarms))->deadline_ms > alarm->deadline_ms) {
    list_prepend(alarms, alarm);
  } else {
    for (list_node_t* node = list_begin(alarms); node != list_end(alarms); node = list_next(node)) {
      list_node_t* next = list_next(node);
      if (next == list_end(alarms) ||
          ((alarm_t*)list_node(next))->deadline_ms > alarm->deadline_ms) {
        list_insert_after(alarms, node, alarm);
        break;
      }
    }
  }

  // If the new alarm has the earliest deadline, we need to re-evaluate our
  // schedule.
  if (needs_reschedule || (!list_is_empty(alarms) && list_front(alarms) == alarm)) {
    reschedule_root_alarm();
  }
}

// NOTE: must be called with |alarms_mutex| held
static void reschedule_root_alarm(void) {
  log::assert_that(alarms != NULL, "assert failed: alarms != NULL");

  const bool timer_was_set = timer_set;
  alarm_t* next;
  int64_t next_expiration;

  // If used in a zeroed state, disarms the timer.
  struct itimerspec timer_time;
  memset(&timer_time, 0, sizeof(timer_time));

  if (list_is_empty(alarms)) {
    goto done;
  }

  next = static_cast<alarm_t*>(list_front(alarms));
  next_expiration = next->deadline_ms - now_ms();
  if (next_expiration < TIMER_INTERVAL_FOR_WAKELOCK_IN_MS) {
    if (!timer_set) {
      if (!wakelock_acquire()) {
        log::error("unable to acquire wake lock");
      }
    }

    timer_time.it_value.tv_sec = (next->deadline_ms / 1000);
    timer_time.it_value.tv_nsec = (next->deadline_ms % 1000) * 1000000LL;

    // It is entirely unsafe to call timer_settime(2) with a zeroed timerspec
    // for timers with *_ALARM clock IDs. Although the man page states that the
    // timer would be canceled, the current behavior (as of Linux kernel 3.17)
    // is that the callback is issued immediately. The only way to cancel an
    // *_ALARM timer is to delete the timer. But unfortunately, deleting and
    // re-creating a timer is rather expensive; every timer_create(2) spawns a
    // new thread. So we simply set the timer to fire at the largest possible
    // time.
    //
    // If we've reached this code path, we're going to grab a wake lock and
    // wait for the next timer to fire. In that case, there's no reason to
    // have a pending wakeup timer so we simply cancel it.
    struct itimerspec end_of_time;
    memset(&end_of_time, 0, sizeof(end_of_time));
    end_of_time.it_value.tv_sec = (time_t)(1LL << (sizeof(time_t) * 8 - 2));
    timer_settime(wakeup_timer, TIMER_ABSTIME, &end_of_time, NULL);
  } else {
    // WARNING: do not attempt to use relative timers with *_ALARM clock IDs
    // in kernels before 3.17 unless you have the following patch:
    // https://lkml.org/lkml/2014/7/7/576
    struct itimerspec wakeup_time;
    memset(&wakeup_time, 0, sizeof(wakeup_time));

    wakeup_time.it_value.tv_sec = (next->deadline_ms / 1000);
    wakeup_time.it_value.tv_nsec = (next->deadline_ms % 1000) * 1000000LL;
    if (timer_settime(wakeup_timer, TIMER_ABSTIME, &wakeup_time, NULL) == -1) {
      log::error("unable to set wakeup timer: {}", strerror(errno));
    }
  }

done:
  timer_set = timer_time.it_value.tv_sec != 0 || timer_time.it_value.tv_nsec != 0;
  if (timer_was_set && !timer_set) {
    wakelock_release();
  }

  if (timer_settime(timer, TIMER_ABSTIME, &timer_time, NULL) == -1) {
    log::error("unable to set timer: {}", strerror(errno));
  }

  // If next expiration was in the past (e.g. short timer that got context
  // switched) then the timer might have diarmed itself. Detect this case and
  // work around it by manually signalling the |alarm_expired| semaphore.
  //
  // It is possible that the timer was actually super short (a few
  // milliseconds) and the timer expired normally before we called
  // |timer_gettime|. Worst case, |alarm_expired| is signaled twice for that
  // alarm. Nothing bad should happen in that case though since the callback
  // dispatch function checks to make sure the timer at the head of the list
  // actually expired.
  if (timer_set) {
    struct itimerspec time_to_expire;
    timer_gettime(timer, &time_to_expire);
    if (time_to_expire.it_value.tv_sec == 0 && time_to_expire.it_value.tv_nsec == 0) {
      log::info("alarm expiration too close for posix timers, switching to guns");
      semaphore_post(alarm_expired);
    }
  }
}

static void alarm_register_processing_queue(fixed_queue_t* queue, thread_t* thread) {
  log::assert_that(queue != NULL, "assert failed: queue != NULL");
  log::assert_that(thread != NULL, "assert failed: thread != NULL");

  fixed_queue_register_dequeue(queue, thread_get_reactor(thread), alarm_queue_ready, NULL);
}

static void alarm_ready_generic(alarm_t* alarm, std::unique_lock<std::mutex>& lock) {
  if (alarm == NULL) {
    return;  // The alarm was probably canceled
  }

  //
  // If the alarm is not periodic, we've fully serviced it now, and can reset
  // some of its internal state. This is useful to distinguish between expired
  // alarms and active ones.
  //
  if (!alarm->callback) {
    log::fatal("timer callback is NULL! Name={}", alarm->stats.name);
  }
  alarm_callback_t callback = alarm->callback;
  void* data = alarm->data;
  uint64_t deadline_ms = alarm->deadline_ms;
  if (alarm->is_periodic) {
    // The periodic alarm has been rescheduled and alarm->deadline has been
    // updated, hence we need to use the previous deadline.
    deadline_ms = alarm->prev_deadline_ms;
  } else {
    alarm->deadline_ms = 0;
    alarm->callback = NULL;
    alarm->data = NULL;
    alarm->queue = NULL;
  }

  // Increment the reference count of the mutex so it doesn't get freed
  // before the callback gets finished executing.
  std::shared_ptr<std::recursive_mutex> local_mutex_ref = alarm->callback_mutex;
  std::lock_guard<std::recursive_mutex> cb_lock(*local_mutex_ref);
  lock.unlock();

  // Update the statistics
  update_scheduling_stats(&alarm->stats, now_ms(), deadline_ms);

  // NOTE: Do NOT access "alarm" after the callback, as a safety precaution
  // in case the callback itself deleted the alarm.
  callback(data);
}

static void alarm_ready_mloop(alarm_t* alarm) {
  std::unique_lock<std::mutex> lock(alarms_mutex);
  alarm_ready_generic(alarm, lock);
}

static void alarm_queue_ready(fixed_queue_t* queue, void* /* context */) {
  log::assert_that(queue != NULL, "assert failed: queue != NULL");

  std::unique_lock<std::mutex> lock(alarms_mutex);
  alarm_t* alarm = (alarm_t*)fixed_queue_try_dequeue(queue);
  alarm_ready_generic(alarm, lock);
}

// Callback function for wake alarms and our posix timer
static void timer_callback(void* /* ptr */) { semaphore_post(alarm_expired); }

// Function running on |dispatcher_thread| that performs the following:
//   (1) Receives a signal using |alarm_exired| that the alarm has expired
//   (2) Dispatches the alarm callback for processing by the corresponding
// thread for that alarm.
static void callback_dispatch(void* /* context */) {
  while (true) {
    semaphore_wait(alarm_expired);
    if (!dispatcher_thread_active) {
      break;
    }

    std::lock_guard<std::mutex> lock(alarms_mutex);
    alarm_t* alarm;

    // Take into account that the alarm may get cancelled before we get to it.
    // We're done here if there are no alarms or the alarm at the front is in
    // the future. Exit right away since there's nothing left to do.
    if (list_is_empty(alarms) ||
        (alarm = static_cast<alarm_t*>(list_front(alarms)))->deadline_ms > now_ms()) {
      reschedule_root_alarm();
      continue;
    }

    list_remove(alarms, alarm);

    if (alarm->is_periodic) {
      alarm->prev_deadline_ms = alarm->deadline_ms;
      schedule_next_instance(alarm);
      alarm->stats.rescheduled_count++;
    }
    reschedule_root_alarm();

    // Enqueue the alarm for processing
    if (alarm->for_msg_loop) {
      if (!get_main_thread()) {
        log::error("message loop already NULL. Alarm: {}", alarm->stats.name);
        continue;
      }

      alarm->closure.i.Reset(Bind(alarm_ready_mloop, alarm));
      get_main_thread()->DoInThread(FROM_HERE, alarm->closure.i.callback());
    } else {
      fixed_queue_enqueue(alarm->queue, alarm);
    }
  }

  log::info("Callback thread exited");
}

static bool timer_create_internal(const clockid_t clock_id, timer_t* timer) {
  log::assert_that(timer != NULL, "assert failed: timer != NULL");

  struct sigevent sigevent;
  // create timer with RT priority thread
  pthread_attr_t thread_attr;
  pthread_attr_init(&thread_attr);
  pthread_attr_setschedpolicy(&thread_attr, SCHED_FIFO);
  struct sched_param param;
  param.sched_priority = THREAD_RT_PRIORITY;
  pthread_attr_setschedparam(&thread_attr, &param);

  memset(&sigevent, 0, sizeof(sigevent));
  sigevent.sigev_notify = SIGEV_THREAD;
  sigevent.sigev_notify_function = (void (*)(union sigval))timer_callback;
  sigevent.sigev_notify_attributes = &thread_attr;
  if (timer_create(clock_id, &sigevent, timer) == -1) {
    log::error("unable to create timer with clock {}: {}", clock_id, strerror(errno));
    if (clock_id == CLOCK_BOOTTIME_ALARM) {
      log::error(
              "The kernel might not have support for "
              "timer_create(CLOCK_BOOTTIME_ALARM): "
              "https://lwn.net/Articles/429925/");
      log::error(
              "See following patches: "
              "https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/log/"
              "?qt=grep&q=CLOCK_BOOTTIME_ALARM");
    }
    return false;
  }

  return true;
}

static void update_scheduling_stats(alarm_stats_t* stats, uint64_t now_ms, uint64_t deadline_ms) {
  stats->total_updates++;
  stats->last_update_ms = now_ms;

  if (deadline_ms < now_ms) {
    // Overdue scheduling
    uint64_t delta_ms = now_ms - deadline_ms;
    update_stat(&stats->overdue_scheduling, delta_ms);
  } else if (deadline_ms > now_ms) {
    // Premature scheduling
    uint64_t delta_ms = deadline_ms - now_ms;
    update_stat(&stats->premature_scheduling, delta_ms);
  }
}

static void dump_stat(int fd, stat_t* stat, const char* description) {
  uint64_t average_time_ms = 0;
  if (stat->count != 0) {
    average_time_ms = stat->total_ms / stat->count;
  }

  dprintf(fd, "%-51s: %llu / %llu / %llu\n", description, (unsigned long long)stat->total_ms,
          (unsigned long long)stat->max_ms, (unsigned long long)average_time_ms);
}

void alarm_debug_dump(int fd) {
  dprintf(fd, "\nBluetooth Alarms Statistics:\n");

  std::lock_guard<std::mutex> lock(alarms_mutex);

  if (alarms == NULL) {
    dprintf(fd, "  None\n");
    return;
  }

  uint64_t just_now_ms = now_ms();

  dprintf(fd, "  Total Alarms: %zu\n\n", list_length(alarms));

  // Dump info for each alarm
  for (list_node_t* node = list_begin(alarms); node != list_end(alarms); node = list_next(node)) {
    alarm_t* alarm = (alarm_t*)list_node(node);
    alarm_stats_t* stats = &alarm->stats;

    dprintf(fd, "  Alarm : %s (%s)\n", stats->name, (alarm->is_periodic) ? "PERIODIC" : "SINGLE");

    dprintf(fd, "%-51s: %zu / %zu / %zu / %zu\n", "    Action counts (sched/resched/exec/cancel)",
            stats->scheduled_count, stats->rescheduled_count, stats->total_updates,
            stats->canceled_count);

    dprintf(fd, "%-51s: %zu / %zu\n", "    Deviation counts (overdue/premature)",
            stats->overdue_scheduling.count, stats->premature_scheduling.count);

    dprintf(fd, "%-51s: %llu / %llu / %lld\n", "    Time in ms (since creation/interval/remaining)",
            (unsigned long long)(just_now_ms - alarm->creation_time_ms),
            (unsigned long long)alarm->period_ms, (long long)(alarm->deadline_ms - just_now_ms));

    dump_stat(fd, &stats->overdue_scheduling, "    Overdue scheduling time in ms (total/max/avg)");

    dump_stat(fd, &stats->premature_scheduling,
              "    Premature scheduling time in ms (total/max/avg)");

    dprintf(fd, "\n");
  }
}
