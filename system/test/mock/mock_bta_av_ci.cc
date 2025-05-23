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
 *   Functions generated:2
 */

#include "bta/include/bta_av_api.h"
#include "test/common/mock_functions.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

void bta_av_ci_setconfig(tBTA_AV_HNDL /* bta_av_handle */, uint8_t /* err_code */,
                         uint8_t /* category */, bool /* recfg_needed */,
                         uint8_t /* avdt_handle */) {
  inc_func_call_count(__func__);
}
void bta_av_ci_src_data_ready(tBTA_AV_CHNL /* chnl */) { inc_func_call_count(__func__); }
