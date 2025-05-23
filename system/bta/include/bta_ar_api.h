/******************************************************************************
 *
 *  Copyright 2004-2012 Broadcom Corporation
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
 *  This is the public interface file for the simulatenous advanced
 *  audio/video streaming (AV) source and sink of BTA, Broadcom's Bluetooth
 *  application layer for mobile phones.
 *
 ******************************************************************************/
#ifndef BTA_AR_API_H
#define BTA_AR_API_H

#include <cstdint>

#include "bta/sys/bta_sys.h"
#include "stack/include/avdt_api.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  Constants and data types
 ****************************************************************************/
/* This event signal to AR user that other profile is connected */
#define BTA_AR_AVDT_CONN_EVT (AVDT_MAX_EVT + 1)

/*******************************************************************************
 *
 * Function         bta_ar_init
 *
 * Description      This function is called from bta_sys_init().
 *                  to initialize the control block
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ar_init(void);

/*******************************************************************************
 *
 * Function         bta_ar_reg_avdt
 *
 * Description      This function is called to register to AVDTP.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ar_reg_avdt(AvdtpRcb* p_reg, tAVDT_CTRL_CBACK* p_cback);

/*******************************************************************************
 *
 * Function         bta_ar_dereg_avdt
 *
 * Description      This function is called to de-register from AVDTP.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ar_dereg_avdt();

/*******************************************************************************
 *
 * Function         bta_ar_reg_avct
 *
 * Description      This function is called to register to AVCTP.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ar_reg_avct();

/*******************************************************************************
 *
 * Function         bta_ar_dereg_avct
 *
 * Description      This function is called to deregister from AVCTP.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ar_dereg_avct();

/******************************************************************************
 *
 * Function         bta_ar_reg_avrc
 *
 * Description      This function is called to register an SDP record for AVRCP.
 *
 * Returns          void
 *
 *****************************************************************************/
void bta_ar_reg_avrc(uint16_t service_uuid, const char* p_service_name, const char* p_provider_name,
                     uint16_t categories, bool browse_supported, uint16_t profile_version);

/******************************************************************************
 *
 * Function         bta_ar_dereg_avrc
 *
 * Description      This function is called to de-register/delete an SDP record
 *                  for AVRCP.
 *
 * Returns          void
 *
 *****************************************************************************/
void bta_ar_dereg_avrc(uint16_t service_uuid);

/******************************************************************************
 *
 * Function         bta_ar_reg_avrc_for_src_sink_coexist
 *
 * Description      This function is called to register an SDP record for AVRCP.
 *                  Add sys_id to distinguish src or sink role and add also save
 *tg_categories
 *
 * Returns          void
 *
 *****************************************************************************/
extern void bta_ar_reg_avrc_for_src_sink_coexist(uint16_t service_uuid, const char* service_name,
                                                 const char* provider_name, uint16_t categories,
                                                 tBTA_SYS_ID sys_id, bool browse_supported,
                                                 uint16_t profile_version);

#endif /* BTA_AR_API_H */
