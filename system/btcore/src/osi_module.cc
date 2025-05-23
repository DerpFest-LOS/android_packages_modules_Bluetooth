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

#define LOG_TAG "bt_osi_module"

#include "btcore/include/osi_module.h"

#include "btcore/include/module.h"
#include "osi/include/alarm.h"
#include "osi/include/future.h"
#include "osi/include/osi.h"
#include "osi/include/wakelock.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

future_t* osi_init(void) { return future_new_immediate(FUTURE_SUCCESS); }

future_t* osi_clean_up(void) {
  alarm_cleanup();
  wakelock_cleanup();
  return future_new_immediate(FUTURE_SUCCESS);
}

EXPORT_SYMBOL extern const module_t osi_module = {.name = OSI_MODULE,
                                                  .init = osi_init,
                                                  .start_up = NULL,
                                                  .shut_down = NULL,
                                                  .clean_up = osi_clean_up,
                                                  .dependencies = {NULL}};
