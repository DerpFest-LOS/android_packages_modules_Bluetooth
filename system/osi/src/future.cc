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

#define LOG_TAG "bt_osi_future"

#include "osi/include/future.h"

#include <bluetooth/log.h>

#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "osi/semaphore.h"

using namespace bluetooth;

struct future_t {
  bool ready_can_be_called;
  semaphore_t* semaphore;  // NULL semaphore means immediate future
  void* result;
};

static void future_free(future_t* future);

future_t* future_new(void) {
  future_t* ret = static_cast<future_t*>(osi_calloc(sizeof(future_t)));

  ret->semaphore = semaphore_new(0);
  if (!ret->semaphore) {
    log::error("unable to allocate memory for the semaphore.");
    goto error;
  }

  ret->ready_can_be_called = true;
  return ret;
error:;
  future_free(ret);
  return NULL;
}

future_t* future_new_immediate(void* value) {
  future_t* ret = static_cast<future_t*>(osi_calloc(sizeof(future_t)));

  ret->result = value;
  ret->ready_can_be_called = false;
  return ret;
}

void future_ready(future_t* future, void* value) {
  log::assert_that(future != NULL, "assert failed: future != NULL");
  log::assert_that(future->ready_can_be_called, "assert failed: future->ready_can_be_called");

  future->ready_can_be_called = false;
  future->result = value;
  semaphore_post(future->semaphore);
}

void* future_await(future_t* future) {
  log::assert_that(future != NULL, "assert failed: future != NULL");

  // If the future is immediate, it will not have a semaphore
  if (future->semaphore) {
    semaphore_wait(future->semaphore);
  }

  void* result = future->result;
  future_free(future);
  return result;
}

static void future_free(future_t* future) {
  if (!future) {
    return;
  }

  semaphore_free(future->semaphore);
  osi_free(future);
}
