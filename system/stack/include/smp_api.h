/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
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

/******************************************************************************
 *
 *  This file contains the SMP API function external definitions.
 *
 ******************************************************************************/
#ifndef SMP_API_H
#define SMP_API_H

#include <cstdint>

#include "smp_api_types.h"
#include "types/ble_address_with_type.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  External Function Declarations
 ****************************************************************************/
/* API of SMP */

/*******************************************************************************
 *
 * Function         SMP_Init
 *
 * Description      This function initializes the SMP unit.
 *
 * Returns          void
 *
 ******************************************************************************/
void SMP_Init(uint8_t init_security_mode);

/*******************************************************************************
 *
 * Function         SMP_Register
 *
 * Description      This function register for the SMP service callback.
 *
 * Returns          void
 *
 ******************************************************************************/
bool SMP_Register(tSMP_CALLBACK* p_cback);

/*******************************************************************************
 *
 * Function         SMP_Pair
 *
 * Description      This function is called to start a SMP pairing.
 *
 * Returns          SMP_STARTED if bond started, else otherwise exception.
 *
 ******************************************************************************/
tSMP_STATUS SMP_Pair(const RawAddress& bd_addr);
tSMP_STATUS SMP_Pair(const RawAddress& bd_addr, tBLE_ADDR_TYPE addr_type);

/*******************************************************************************
 *
 * Function         SMP_BR_PairWith
 *
 * Description      This function is called to start a SMP pairing over BR/EDR.
 *
 * Returns          SMP_STARTED if pairing started, otherwise the reason for the
 *                  failure.
 *
 ******************************************************************************/
tSMP_STATUS SMP_BR_PairWith(const RawAddress& bd_addr);

/*******************************************************************************
 *
 * Function         SMP_PairCancel
 *
 * Description      This function is called to cancel a SMP pairing.
 *
 * Returns          true - pairing cancelled
 *
 ******************************************************************************/
bool SMP_PairCancel(const RawAddress& bd_addr);

/*******************************************************************************
 *
 * Function         SMP_SecurityGrant
 *
 * Description      This function is called to grant security process.
 *
 * Parameters       bd_addr - peer device bd address.
 *                  res     - result of the operation SMP_SUCCESS if success.
 *                            Otherwise, SMP_REPEATED_ATTEMPTS is too many
 *                            attempts.
 *
 * Returns          None
 *
 ******************************************************************************/
void SMP_SecurityGrant(const RawAddress& bd_addr, tSMP_STATUS res);

/*******************************************************************************
 *
 * Function         SMP_PasskeyReply
 *
 * Description      This function is called after Security Manager submitted
 *                  Passkey request to the application.
 *
 * Parameters:      bd_addr  - Address of the device for which PIN was requested
 *                  res      - result of the operation SMP_SUCCESS if success
 *                  passkey  - numeric value in the range of
 *                             BTM_MIN_PASSKEY_VAL(0) -
 *                             BTM_MAX_PASSKEY_VAL(999999(0xF423F)).
 *
 ******************************************************************************/
void SMP_PasskeyReply(const RawAddress& bd_addr, uint8_t res, uint32_t passkey);

/*******************************************************************************
 *
 * Function         SMP_ConfirmReply
 *
 * Description      This function is called after Security Manager submitted
 *                  numeric comparison request to the application.
 *
 * Parameters:      bd_addr      - Address of the device with which numeric
 *                                 comparison was requested
 *                  res          - comparison result SMP_SUCCESS if success
 *
 ******************************************************************************/
void SMP_ConfirmReply(const RawAddress& bd_addr, uint8_t res);

/*******************************************************************************
 *
 * Function         SMP_OobDataReply
 *
 * Description      This function is called to provide the OOB data for
 *                  SMP in response to SMP_OOB_REQ_EVT
 *
 * Parameters:      bd_addr     - Address of the peer device
 *                  res         - result of the operation SMP_SUCCESS if success
 *                  p_data      - SM Randomizer  C.
 *
 ******************************************************************************/
void SMP_OobDataReply(const RawAddress& bd_addr, tSMP_STATUS res, uint8_t len, uint8_t* p_data);

/*******************************************************************************
 *
 * Function         SMP_SecureConnectionOobDataReply
 *
 * Description      This function is called to provide the SC OOB data for
 *                  SMP in response to SMP_SC_OOB_REQ_EVT
 *
 * Parameters:      p_data      - pointer to the data
 *
 ******************************************************************************/
void SMP_SecureConnectionOobDataReply(uint8_t* p_data);

/*******************************************************************************
 *
 * Function         SMP_CrLocScOobData
 *
 * Description      This function is called to generate a public key to be
 *                  passed to a remote device via an Out of Band transport
 *
 * Returns          true if the request is successfully sent and executed by the
 *                  state machine, false otherwise
 *
 ******************************************************************************/
bool SMP_CrLocScOobData();

/*******************************************************************************
 *
 * Function         SMP_ClearLocScOobData
 *
 * Description      This function is called to clear out the OOB stored locally.
 *
 ******************************************************************************/
void SMP_ClearLocScOobData();

/*******************************************************************************
 *
 * Function         SMP_SirkConfirmDeviceReply
 *
 * Description      This function is called after Security Manager submitted
 *                  verification of device with CSIP.
 *
 * Parameters:      bd_addr      - Address of the device with which verification
 *                                 was requested
 *                  res          - comparison result SMP_SUCCESS if success
 *
 ******************************************************************************/
void SMP_SirkConfirmDeviceReply(const RawAddress& bd_addr, uint8_t res);

// Called when LTK request is received from controller.
bool smp_proc_ltk_request(const RawAddress& bda);

// Called when link is encrypted and notified to peripheral device.
// Proceed to send LTK, DIV and ER to central if bonding the devices.
void smp_link_encrypted(const RawAddress& bda, uint8_t encr_enable);

void smp_cancel_start_encryption_attempt();

#endif /* SMP_API_H */
