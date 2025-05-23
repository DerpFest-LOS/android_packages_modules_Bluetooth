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
 *   Functions generated:6
 */

#include <cstdint>

#include "stack/include/ble_acl_interface.h"
#include "stack/include/hci_error_code.h"
#include "test/common/mock_functions.h"
#include "types/ble_address_with_type.h"
#include "types/hci_role.h"
#include "types/raw_address.h"

void acl_ble_connection_fail(const tBLE_BD_ADDR& /* address_with_type */, uint16_t /* handle */,
                             bool /* enhanced */, tHCI_STATUS /* status */) {
  inc_func_call_count(__func__);
}
void acl_ble_enhanced_connection_complete(const tBLE_BD_ADDR& /* address_with_type */,
                                          uint16_t /* handle */, tHCI_ROLE /* role */,
                                          bool /* match */, uint16_t /* conn_interval */,
                                          uint16_t /* conn_latency */, uint16_t /* conn_timeout */,
                                          const RawAddress& /* local_rpa */,
                                          const RawAddress& /* peer_rpa */,
                                          uint8_t /* peer_addr_type */,
                                          bool /* can_read_discoverable_characteristics */) {
  inc_func_call_count(__func__);
}
void acl_ble_enhanced_connection_complete_from_shim(
        const tBLE_BD_ADDR& /* address_with_type */, uint16_t /* handle */, tHCI_ROLE /* role */,
        uint16_t /* conn_interval */, uint16_t /* conn_latency */, uint16_t /* conn_timeout */,
        const RawAddress& /* local_rpa */, const RawAddress& /* peer_rpa */,
        tBLE_ADDR_TYPE /* peer_addr_type */, bool /* can_read_discoverable_characteristics */) {
  inc_func_call_count(__func__);
}

void gatt_notify_conn_update(const RawAddress& /* remote */, uint16_t /* interval */,
                             uint16_t /* latency */, uint16_t /* timeout */,
                             tHCI_STATUS /* status */);
void acl_ble_update_event_received(tHCI_STATUS /* status */, uint16_t /* handle */,
                                   uint16_t /* interval */, uint16_t /* latency */,
                                   uint16_t /* timeout */) {
  inc_func_call_count(__func__);
}

void acl_ble_update_request_event_received(uint16_t /* handle */, uint16_t /* interval_min */,
                                           uint16_t /* interval_max */, uint16_t /* latency */,
                                           uint16_t /* timeout */) {
  inc_func_call_count(__func__);
}

void acl_ble_data_length_change_event(uint16_t /* handle */, uint16_t /* max_tx_octets */,
                                      uint16_t /* max_tx_time */, uint16_t /* max_rx_octets */,
                                      uint16_t /* max_rx_time */) {
  inc_func_call_count(__func__);
}
