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
 *   Functions generated:9
 */

#include "stack/include/bt_hdr.h"
#include "stack/l2cap/l2c_int.h"
#include "test/common/mock_functions.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

tL2CAP_DW_RESULT l2c_data_write(uint16_t /* cid */, BT_HDR* /* p_data */, uint16_t /* flags */) {
  inc_func_call_count(__func__);
  return tL2CAP_DW_RESULT::FAILED;
}
void l2c_ccb_timer_timeout(void* /* data */) { inc_func_call_count(__func__); }
void l2c_fcrb_ack_timer_timeout(void* /* data */) { inc_func_call_count(__func__); }
void l2c_free(void) { inc_func_call_count(__func__); }
void l2c_init(void) { inc_func_call_count(__func__); }
void l2c_lcb_timer_timeout(void* /* data */) { inc_func_call_count(__func__); }
void l2c_rcv_acl_data(BT_HDR* /* p_msg */) { inc_func_call_count(__func__); }
void l2c_receive_hold_timer_timeout(void* /* data */) { inc_func_call_count(__func__); }
