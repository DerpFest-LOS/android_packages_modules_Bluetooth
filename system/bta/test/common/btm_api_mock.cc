/*
 * Copyright 2021 HIMSA II K/S - www.himsa.com.
 * Represented by EHIMA - www.ehima.com
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

#include "btm_api_mock.h"

#include <bluetooth/log.h>

#include <optional>

#include "bt_octets.h"
#include "stack/include/btm_ble_sec_api.h"
#include "test/mock/mock_stack_btm_interface.h"
#include "types/raw_address.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using namespace bluetooth;

static bluetooth::manager::MockBtmInterface* btm_interface = nullptr;

void bluetooth::manager::SetMockBtmInterface(MockBtmInterface* mock_btm_interface) {
  btm_interface = mock_btm_interface;
  mock_btm_client_interface.peer.BTM_IsPhy2mSupported = [](const RawAddress& remote_bda,
                                                           tBT_TRANSPORT transport) {
    return btm_interface->IsPhy2mSupported(remote_bda, transport);
  };
  mock_btm_client_interface.peer.BTM_GetHCIConnHandle = [](RawAddress const& bd_addr,
                                                           tBT_TRANSPORT transport) -> uint16_t {
    return btm_interface->GetHCIConnHandle(bd_addr, transport);
  };
  mock_btm_client_interface.peer.BTM_GetPeerSCA = [](const RawAddress& remote_bda,
                                                     tBT_TRANSPORT transport) {
    return btm_interface->GetPeerSCA(remote_bda, transport);
  };
  mock_btm_client_interface.peer.BTM_RequestPeerSCA = [](RawAddress const& bd_addr,
                                                         tBT_TRANSPORT transport) {
    btm_interface->RequestPeerSCA(bd_addr, transport);
  };
  mock_btm_client_interface.ble.BTM_BleSetPhy = [](const RawAddress& bd_addr, uint8_t tx_phys,
                                                   uint8_t rx_phys, uint16_t phy_options) {
    btm_interface->BleSetPhy(bd_addr, tx_phys, rx_phys, phy_options);
  };
  mock_btm_client_interface.peer.BTM_IsAclConnectionUp = [](const RawAddress& remote_bda,
                                                            tBT_TRANSPORT transport) {
    return btm_interface->BTM_IsAclConnectionUp(remote_bda, transport);
  };
}

bool BTM_IsLinkKeyKnown(const RawAddress& bd_addr, tBT_TRANSPORT transport) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->IsLinkKeyKnown(bd_addr, transport);
}

bool BTM_IsEncrypted(const RawAddress& bd_addr, tBT_TRANSPORT transport) {
  return btm_interface->BTM_IsEncrypted(bd_addr, transport);
}

tBTM_STATUS BTM_SetEncryption(const RawAddress& bd_addr, tBT_TRANSPORT transport,
                              tBTM_SEC_CALLBACK* p_callback, void* p_ref_data,
                              tBTM_BLE_SEC_ACT sec_act) {
  return btm_interface->SetEncryption(bd_addr, transport, p_callback, p_ref_data, sec_act);
}

bool BTM_SecIsSecurityPending(const RawAddress& bd_addr) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->SecIsSecurityPending(bd_addr);
}

tBTM_SEC_DEV_REC* btm_find_dev(const RawAddress& bd_addr) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->FindDevice(bd_addr);
}

void acl_disconnect_from_handle(uint16_t handle, tHCI_STATUS reason, std::string /*comment*/) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->AclDisconnectFromHandle(handle, reason);
}

tBTM_INQ_INFO* BTM_InqDbFirst(void) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->BTM_InqDbFirst();
}
tBTM_INQ_INFO* BTM_InqDbNext(tBTM_INQ_INFO* p_cur) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->BTM_InqDbNext(p_cur);
}

std::optional<Octet16> BTM_BleGetPeerLTK(const RawAddress address) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->BTM_BleGetPeerLTK(address);
}

std::optional<Octet16> BTM_BleGetPeerIRK(const RawAddress address) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->BTM_BleGetPeerIRK(address);
}

bool BTM_BleIsLinkKeyKnown(const RawAddress address) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->BTM_BleIsLinkKeyKnown(address);
}

std::optional<tBLE_BD_ADDR> BTM_BleGetIdentityAddress(const RawAddress address) {
  log::assert_that(btm_interface != nullptr, "Mock btm interface not set!");
  return btm_interface->BTM_BleGetIdentityAddress(address);
}
