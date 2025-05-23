/******************************************************************************
 *
 *  Copyright 2003-2012 Broadcom Corporation
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
 *  This file contains compile-time configurable constants for the audio
 *  gateway.
 *
 ******************************************************************************/

#include "bta/ag/bta_ag_int.h"
#include "bta/include/bta_ag_api.h"
#include "btm_api_types.h"
#include "device/include/esco_parameters.h"

/* Set the CIND to match HFP 1.5 */
#define BTA_AG_CIND_INFO                                                       \
  "(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"service\",(0-1)),(\"signal\",(0-" \
  "5)),(\"roam\",(0,1)),(\"battchg\",(0-5)),(\"callheld\",(0-2))"

#define BTA_AG_CHLD_VAL_ECC "(0,1,1x,2,2x,3)"

#define BTA_AG_CHLD_VAL "(0,1,2,3)"

#ifndef BTA_AG_CONN_TIMEOUT
#define BTA_AG_CONN_TIMEOUT 5000
#endif

/* S1 packet type setting from HFP 1.5 spec */
#define BTA_AG_SCO_PKT_TYPES /* BTM_SCO_LINK_ALL_PKT_MASK */                         \
  (BTM_SCO_LINK_ONLY_MASK | ESCO_PKT_TYPES_MASK_EV3 | ESCO_PKT_TYPES_MASK_NO_3_EV3 | \
   ESCO_PKT_TYPES_MASK_NO_2_EV5 | ESCO_PKT_TYPES_MASK_NO_3_EV5)

#ifndef BTA_AG_BIND_INFO
#define BTA_AG_BIND_INFO "(1)"
#endif

const tBTA_AG_HF_IND bta_ag_local_hf_ind_cfg[] = {
        /* The first row contains the number of indicators. Need to be updated
           accordingly */
        {BTA_AG_NUM_LOCAL_HF_IND, false, false, 0, 0},

        {1, true, true, 0, 1},  /* Enhanced Driver Status, supported, enabled, range 0 ~ 1 */
        {2, true, true, 0, 100} /* Battery Level Status, supported, enabled, range 0 ~ 100 */
};

const tBTA_AG_CFG bta_ag_cfg = {BTA_AG_CIND_INFO,    BTA_AG_BIND_INFO,     BTA_AG_NUM_LOCAL_HF_IND,
                                BTA_AG_CONN_TIMEOUT, BTA_AG_SCO_PKT_TYPES, BTA_AG_CHLD_VAL_ECC,
                                BTA_AG_CHLD_VAL};

const tBTA_AG_CFG* p_bta_ag_cfg = &bta_ag_cfg;
