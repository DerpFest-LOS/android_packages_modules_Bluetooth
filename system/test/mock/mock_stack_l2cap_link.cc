/*
 * Copyright 2020 The Android Open Source Project
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
 */

#include <cstdint>

#include "stack/include/bt_hdr.h"
#include "stack/include/btm_status.h"
#include "stack/include/l2cap_acl_interface.h"
#include "stack/include/l2cap_controller_interface.h"
#include "stack/include/l2cap_hci_link_interface.h"
#include "stack/include/l2cap_security_interface.h"
#include "stack/l2cap/l2c_int.h"
#include "test/common/mock_functions.h"
#include "types/raw_address.h"

bool l2c_link_hci_disc_comp(uint16_t /* handle */, tHCI_REASON /* reason */) {
  inc_func_call_count(__func__);
  return false;
}
tBTM_STATUS l2cu_ConnectAclForSecurity(const RawAddress& /* bd_addr */) {
  inc_func_call_count(__func__);
  return tBTM_STATUS::BTM_SUCCESS;
}
void l2c_OnHciModeChangeSendPendingPackets(RawAddress /* remote */) {
  inc_func_call_count(__func__);
}
void l2c_info_resp_timer_timeout(void* /* data */) { inc_func_call_count(__func__); }
void l2c_link_adjust_allocation(void) { inc_func_call_count(__func__); }
void l2c_link_adjust_chnl_allocation(void) { inc_func_call_count(__func__); }
void l2c_link_check_send_pkts(tL2C_LCB* /* p_lcb */, uint16_t /* local_cid */,
                              BT_HDR* /* p_buf */) {
  inc_func_call_count(__func__);
}
void l2c_link_hci_conn_comp(tHCI_STATUS /* status */, uint16_t /* handle */,
                            const RawAddress& /* p_bda */) {
  inc_func_call_count(__func__);
}
void l2c_link_init(const uint16_t /* acl_buffer_count_classic */) { inc_func_call_count(__func__); }
void l2c_link_role_changed(const RawAddress* /* bd_addr */, tHCI_ROLE /* new_role */,
                           tHCI_STATUS /* hci_status */) {
  inc_func_call_count(__func__);
}
void l2c_link_sec_comp(const RawAddress /* bda */, tBT_TRANSPORT /* transport */,
                       void* /* p_ref_data */, tBTM_STATUS /* status */) {
  inc_func_call_count(__func__);
}

void l2c_link_timeout(tL2C_LCB* /* p_lcb */) { inc_func_call_count(__func__); }
void l2c_packets_completed(uint16_t /* handle */, uint16_t /* num_sent */) {
  inc_func_call_count(__func__);
}
void l2c_pin_code_request(const RawAddress& /* bd_addr */) { inc_func_call_count(__func__); }
void l2cble_update_sec_act(const RawAddress& /* bd_addr */, uint16_t /* sec_act */) {
  inc_func_call_count(__func__);
}
