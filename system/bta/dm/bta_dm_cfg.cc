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
 *  This file contains compile-time configurable constants for the device
 *  manager.
 *
 ******************************************************************************/

#include <cstdint>

#include "bta/dm/bta_dm_int.h"
#include "bta/include/bta_api.h"
#include "bta/include/bta_hh_api.h"
#include "bta/include/bta_jv_api.h"
#include "bta/sys/bta_sys.h"
#include "internal_include/bt_target.h"
#include "osi/include/properties.h"

/* page timeout in 625uS */
#ifndef BTA_DM_PAGE_TIMEOUT
#define BTA_DM_PAGE_TIMEOUT 8192
#endif

/* TRUE to avoid scatternet when av is streaming (be the central) */
#ifndef BTA_DM_AVOID_SCATTER_A2DP
#define BTA_DM_AVOID_SCATTER_A2DP TRUE
#endif

const tBTA_DM_CFG bta_dm_cfg = {
        /* page timeout in 625uS */
        BTA_DM_PAGE_TIMEOUT,
        /* true to avoid scatternet when av is streaming (be the central) */
        BTA_DM_AVOID_SCATTER_A2DP};

#ifndef BTA_DM_SCATTERNET
/* By default, allow partial scatternet */
#define BTA_DM_SCATTERNET BTA_DM_PARTIAL_SCATTERNET
#endif

#ifndef BTA_HH_ROLE
/* By default, do not specify HH role (backward compatibility) */
#define BTA_HH_ROLE BTA_ANY_ROLE
#endif

#ifndef BTA_PANU_ROLE
/* By default, AV role (backward BTA_CENTRAL_ROLE_PREF) */
#define BTA_PANU_ROLE BTA_PERIPHERAL_ROLE_ONLY
#endif
#define BTA_DM_NUM_RM_ENTRY 6

/* appids for PAN used by insight sample application
   these have to be same as defined in btui_int.h */
#define BTUI_PAN_ID_PANU 0
#define BTUI_PAN_ID_NAP 1
#define BTUI_PAN_ID_GN 2

/* First element is always for SYS:
   app_id = # of entries table, cfg is
   device scatternet support */
const tBTA_DM_RM bta_dm_rm_cfg[] = {{BTA_ID_SYS, BTA_DM_NUM_RM_ENTRY, BTA_DM_SCATTERNET},
                                    {BTA_ID_PAN, BTUI_PAN_ID_NAP, BTA_ANY_ROLE},
                                    {BTA_ID_PAN, BTUI_PAN_ID_GN, BTA_ANY_ROLE},
                                    {BTA_ID_PAN, BTA_APP_ID_PAN_MULTI, BTA_CENTRAL_ROLE_ONLY},
                                    {BTA_ID_PAN, BTUI_PAN_ID_PANU, BTA_PANU_ROLE},
                                    {BTA_ID_HH, BTA_ALL_APP_ID, BTA_HH_ROLE},
                                    {BTA_ID_AV, BTA_ALL_APP_ID, BTA_CENTRAL_ROLE_PREF}};

const tBTA_DM_CFG* p_bta_dm_cfg = &bta_dm_cfg;

const tBTA_DM_RM* p_bta_dm_rm_cfg = &bta_dm_rm_cfg[0];

#define BTA_DM_NUM_PM_ENTRY 25 /* number of entries in bta_dm_pm_cfg except the first */
#define BTA_DM_NUM_PM_SPEC 16  /* number of entries in bta_dm_pm_spec */

tBTA_DM_PM_TYPE_QUALIFIER tBTA_DM_PM_CFG bta_dm_pm_cfg[BTA_DM_NUM_PM_ENTRY + 1] = {
        {BTA_ID_SYS, BTA_DM_NUM_PM_ENTRY, 0}, /* reserved: specifies length of this table. */
        {BTA_ID_AG, BTA_ALL_APP_ID, 0},       /* ag uses first spec table for app id 0 */
        {BTA_ID_CT, 1, 1},                    /* ct (BTA_ID_CT,APP ID=1) spec table */
        {BTA_ID_CG, BTA_ALL_APP_ID, 1},       /* cg reuse ct spec table */
        {BTA_ID_DG, BTA_ALL_APP_ID, 2},       /* dg spec table */
        {BTA_ID_AV, BTA_ALL_APP_ID, 4},       /* av spec table */
        {BTA_ID_FTC, BTA_ALL_APP_ID, 7},      /* ftc spec table */
        {BTA_ID_FTS, BTA_ALL_APP_ID, 8},      /* fts spec table */
        {BTA_ID_HD, BTA_ALL_APP_ID, 3},       /* hd spec table */
        {BTA_ID_HH, BTA_HH_APP_ID_JOY, 5},    /* app BTA_HH_APP_ID_JOY,
                                                 similar to hh spec table */
        {BTA_ID_HH, BTA_HH_APP_ID_GPAD, 5},   /* app BTA_HH_APP_ID_GPAD,
                                                 similar to hh spec table */
        {BTA_ID_HH, BTA_ALL_APP_ID, 6},       /* hh spec table */
        {BTA_ID_PBC, BTA_ALL_APP_ID, 2},      /* reuse dg spec table */
        {BTA_ID_PBS, BTA_ALL_APP_ID, 8},      /* reuse fts spec table */
        {BTA_ID_OPC, BTA_ALL_APP_ID, 7},      /* reuse ftc spec table */
        {BTA_ID_OPS, BTA_ALL_APP_ID, 8},      /* reuse fts spec table */
        {BTA_ID_MSE, BTA_ALL_APP_ID, 8},      /* reuse fts spec table */
        {BTA_ID_JV, BTA_JV_PM_ID_1, 7},       /* app BTA_JV_PM_ID_1, reuse ftc spec table */
        {BTA_ID_JV, BTA_ALL_APP_ID, 8},       /* reuse fts spec table */
        {BTA_ID_HL, BTA_ALL_APP_ID, 9},       /* reuse fts spec table */
        {BTA_ID_PAN, BTUI_PAN_ID_PANU, 10},   /* PANU spec table */
        {BTA_ID_PAN, BTUI_PAN_ID_NAP, 11},    /* NAP spec table */
        {BTA_ID_HS, BTA_ALL_APP_ID, 12},      /* HS spec table */
        {BTA_ID_GATTC, BTA_ALL_APP_ID, 14},   /* gattc spec table */
        {BTA_ID_GATTS, BTA_ALL_APP_ID, 15}    /* gatts spec table */
};

tBTA_DM_PM_TYPE_QUALIFIER tBTA_DM_PM_SPEC* get_bta_dm_pm_spec() {
  static uint16_t hs_sniff_delay =
          uint16_t(osi_property_get_int32("bluetooth.bta_hs_sniff_delay_ms.config", 7000));
  static uint16_t fts_ops_idle_to_sniff_delay_ms =
          uint16_t(osi_property_get_int32("bluetooth.bta_fts_ops_idle_to_sniff_delay_ms.config",
                                          BTA_FTS_OPS_IDLE_TO_SNIFF_DELAY_MS));
  static uint16_t ftc_idle_to_sniff_delay_ms = uint16_t(osi_property_get_int32(
          "bluetooth.bta_ftc_idle_to_sniff_delay_ms.config", BTA_FTC_IDLE_TO_SNIFF_DELAY_MS));

  static tBTA_DM_PM_TYPE_QUALIFIER tBTA_DM_PM_SPEC bta_dm_pm_spec[BTA_DM_NUM_PM_SPEC] = {
          /* AG : 0 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR2),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 7000},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open sniff  */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_SNIFF_SCO_OPEN_IDX, 7000},
                    {BTA_DM_PM_NO_ACTION, 0}}, /* sco open, active */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 7000},
                    {BTA_DM_PM_NO_ACTION, 0}}, /* sco close sniff  */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 7000}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},            /* busy */
                   {{BTA_DM_PM_RETRY, 7000}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* CT, CG : 1 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR2),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_PARK, 5000}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn open  park */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 5000},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* sco open sniff */
                   {{BTA_DM_PM_PARK, 5000}, {BTA_DM_PM_NO_ACTION, 0}},   /* sco close  park */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* busy */
                   {{BTA_DM_PM_RETRY, 5000}, {BTA_DM_PM_NO_ACTION, 0}}   /* mode change retry */
           }},

          /* DG, PBC : 2 */
          {(BTA_DM_PM_ACTIVE), /* no power saving mode allowed */
           (BTA_DM_PM_SSR2),   /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF, 5000}, {BTA_DM_PM_NO_ACTION, 0}},  /* conn open active */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF, 1000}, {BTA_DM_PM_NO_ACTION, 0}},  /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}  /* mode change retry */
           }},

          /* HD : 3 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR3),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF_HD_ACTIVE_IDX, 5000},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open sniff */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close */
                   {{BTA_DM_PM_SNIFF_HD_IDLE_IDX, 5000}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_SNIFF_HD_ACTIVE_IDX, 0}, {BTA_DM_PM_NO_ACTION, 0}},  /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* AV : 4 */
          {(BTA_DM_PM_SNIFF), /* allow sniff */
           (BTA_DM_PM_SSR2),  /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 7000},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open  sniff */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 7000}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},            /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* HH for joysticks and gamepad : 5 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR1),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF6, BTA_DM_PM_HH_OPEN_DELAY},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open  sniff */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0},
                    {BTA_DM_PM_NO_ACTION, 0}}, /* sco close, used for HH suspend */
                   {{BTA_DM_PM_SNIFF6, BTA_DM_PM_HH_IDLE_DELAY},
                    {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_SNIFF6, BTA_DM_PM_HH_ACTIVE_DELAY},
                    {BTA_DM_PM_NO_ACTION, 0}},                          /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* HH : 6 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR1),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF_HH_OPEN_IDX, BTA_DM_PM_HH_OPEN_DELAY},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open  sniff */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0},
                    {BTA_DM_PM_NO_ACTION, 0}}, /* sco close, used for HH suspend */
                   {{BTA_DM_PM_SNIFF_HH_IDLE_IDX, BTA_DM_PM_HH_IDLE_DELAY},
                    {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_SNIFF_HH_ACTIVE_IDX, BTA_DM_PM_HH_ACTIVE_DELAY},
                    {BTA_DM_PM_NO_ACTION, 0}},                          /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* FTC, OPC, JV : 7 */
          {(BTA_DM_PM_SNIFF), /* allow sniff */
           (BTA_DM_PM_SSR2),  /* the SSR entry */
           {
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* conn open  active */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, ftc_idle_to_sniff_delay_ms},
                    {BTA_DM_PM_NO_ACTION, 0}},                          /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* FTS, PBS, OPS, MSE, BTA_JV_PM_ID_1 : 8 */
          {(BTA_DM_PM_SNIFF), /* allow sniff */
           (BTA_DM_PM_SSR2),  /* the SSR entry */
           {
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* conn open  active */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, fts_ops_idle_to_sniff_delay_ms},
                    {BTA_DM_PM_NO_ACTION, 0}},                          /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* HL : 9 */
          {(BTA_DM_PM_SNIFF), /* allow sniff */
           (BTA_DM_PM_SSR2),  /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 5000},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open sniff  */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open, active */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close sniff  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}  /* mode change retry */
           }},

          /* PANU : 10 */
          {(BTA_DM_PM_SNIFF), /* allow sniff */
           (BTA_DM_PM_SSR2),  /* the SSR entry */
           {
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* conn open  active */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 5000}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},            /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* NAP : 11 */
          {(BTA_DM_PM_SNIFF), /* allow sniff */
           (BTA_DM_PM_SSR2),  /* the SSR entry */
           {
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* conn open  active */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 5000}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},            /* busy */

                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* HS : 12 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR2),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF, hs_sniff_delay},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open sniff  */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_SNIFF3, 7000}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open, active */
                   {{BTA_DM_PM_SNIFF, 7000}, {BTA_DM_PM_NO_ACTION, 0}},  /* sco close sniff  */
                   {{BTA_DM_PM_SNIFF, hs_sniff_delay}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},             /* busy */
                   {{BTA_DM_PM_RETRY, 7000}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* AVK : 13 */
          {(BTA_DM_PM_SNIFF), /* allow sniff */
           (BTA_DM_PM_SSR2),  /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF, 3000}, {BTA_DM_PM_NO_ACTION, 0}},  /* conn open  sniff */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF4, 3000}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* busy */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}  /* mode change retry */
           }},

          /* GATTC : 14 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR2),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 10000},
                    {BTA_DM_PM_NO_ACTION, 0}},                           /* conn open  active */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}},   /* conn close  */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},    /* app open */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_ACTION, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close   */
                   {{BTA_DM_PM_SNIFF_A2DP_IDX, 10000}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_ACTIVE, 0}, {BTA_DM_PM_NO_ACTION, 0}},             /* busy */
                   {{BTA_DM_PM_RETRY, 5000}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }},

          /* GATTS : 15 */
          {(BTA_DM_PM_SNIFF | BTA_DM_PM_PARK), /* allow park & sniff */
           (BTA_DM_PM_SSR2),                   /* the SSR entry */
           {
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* conn open  active */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* conn close  */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app open */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* app close */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco open  */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* sco close */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* idle */
                   {{BTA_DM_PM_NO_PREF, 0}, {BTA_DM_PM_NO_ACTION, 0}}, /* busy */
                   {{BTA_DM_PM_RETRY, 5000}, {BTA_DM_PM_NO_ACTION, 0}} /* mode change retry */
           }}

#ifdef BTE_SIM_APP /* For Insight builds only */
          /* Entries at the end of the pm_spec table are user-defined (runtime
             configurable),
             for power consumption experiments.
             Insight finds the first user-defined entry by looking for the first
             BTA_DM_PM_NO_PREF.
             The number of user_defined specs is defined by
             BTA_SWRAP_UD_PM_SPEC_COUNT */
          ,
          {BTA_DM_PM_NO_PREF}, /* pm_spec USER_DEFINED_0 */
          {BTA_DM_PM_NO_PREF}  /* pm_spec USER_DEFINED_1 */
#endif                         /* BTE_SIM_APP */
  };
  return bta_dm_pm_spec;
}

/* Please refer to the SNIFF table definitions in bta_api.h.
 *
 * Adding to or Modifying the Table
 * Additional sniff parameter entries can be added for BTA_DM_PM_SNIFF6 -
 * BTA_DM_PM_SNIFF7.
 * Overrides of additional table entries can be specified in bdroid_buildcfg.h.
 * If additional
 * sniff parameter entries are added or an override of an existing entry is
 * specified in
 * bdroid_buildcfg.h then the BTA_DM_PM_*_IDX defines in bta_api.h will need to
 * be match the new
 * ordering.
 *
 * Table Ordering
 * Sniff Table entries must be ordered from highest latency (biggest interval)
 * to lowest latency.
 * If there is a conflict among the connected services the setting with the
 * lowest latency will
 * be selected.
 */
tBTA_DM_PM_TYPE_QUALIFIER tBTM_PM_PWR_MD bta_dm_pm_md[] = {
        /*
         * More sniff parameter entries can be added for
         * BTA_DM_PM_SNIFF3 - BTA_DM_PM_SNIFF7, if needed. When entries are added or
         * removed, BTA_DM_PM_PARK_IDX needs to be updated to reflect the actual
         * index
         * BTA_DM_PM_PARK_IDX is defined in bta_api.h and can be override by the
         * bdroid_buildcfg.h settings.
         * The SNIFF table entries must be in the order from highest latency
         * (biggest
         * interval) to lowest latency. If there's a conflict among the connected
         * services, the setting with lowest latency wins.
         */
        /* sniff modes: max interval, min interval, attempt, timeout */
        {BTA_DM_PM_SNIFF_MAX, BTA_DM_PM_SNIFF_MIN, BTA_DM_PM_SNIFF_ATTEMPT, BTA_DM_PM_SNIFF_TIMEOUT,
         BTM_PM_MD_SNIFF}, /* for BTA_DM_PM_SNIFF - A2DP */
        {BTA_DM_PM_SNIFF1_MAX, BTA_DM_PM_SNIFF1_MIN, BTA_DM_PM_SNIFF1_ATTEMPT,
         BTA_DM_PM_SNIFF1_TIMEOUT, BTM_PM_MD_SNIFF}, /* for BTA_DM_PM_SNIFF1 */
        {BTA_DM_PM_SNIFF2_MAX, BTA_DM_PM_SNIFF2_MIN, BTA_DM_PM_SNIFF2_ATTEMPT,
         BTA_DM_PM_SNIFF2_TIMEOUT, BTM_PM_MD_SNIFF}, /* for BTA_DM_PM_SNIFF2- HD idle */
        {BTA_DM_PM_SNIFF3_MAX, BTA_DM_PM_SNIFF3_MIN, BTA_DM_PM_SNIFF3_ATTEMPT,
         BTA_DM_PM_SNIFF3_TIMEOUT, BTM_PM_MD_SNIFF}, /* for BTA_DM_PM_SNIFF3- SCO open */
        {BTA_DM_PM_SNIFF4_MAX, BTA_DM_PM_SNIFF4_MIN, BTA_DM_PM_SNIFF4_ATTEMPT,
         BTA_DM_PM_SNIFF4_TIMEOUT, BTM_PM_MD_SNIFF}, /* for BTA_DM_PM_SNIFF4- HD active */
        {BTA_DM_PM_SNIFF5_MAX, BTA_DM_PM_SNIFF5_MIN, BTA_DM_PM_SNIFF5_ATTEMPT,
         BTA_DM_PM_SNIFF5_TIMEOUT, BTM_PM_MD_SNIFF}, /* for BTA_DM_PM_SNIFF5- HD active */
        {BTA_DM_PM_SNIFF6_MAX, BTA_DM_PM_SNIFF6_MIN, BTA_DM_PM_SNIFF6_ATTEMPT,
         BTA_DM_PM_SNIFF6_TIMEOUT, BTM_PM_MD_SNIFF}, /* for BTA_DM_PM_SNIFF6- HD active */
        {BTA_DM_PM_PARK_MAX, BTA_DM_PM_PARK_MIN, BTA_DM_PM_PARK_ATTEMPT, BTA_DM_PM_PARK_TIMEOUT,
         BTM_PM_MD_PARK}

#ifdef BTE_SIM_APP /* For Insight builds only */
        /* Entries at the end of the bta_dm_pm_md table are user-defined (runtime
           configurable),
           for power consumption experiments.
           Insight finds the first user-defined entry by looking for the first
           'max=0'.
           The number of user_defined specs is defined by BTA_SWRAP_UD_PM_DM_COUNT
           */
        ,
        {0}, /* CONN_OPEN/SCO_CLOSE power mode settings for pm_spec USER_DEFINED_0
              */
        {0}, /* SCO_OPEN power mode settings for pm_spec USER_DEFINED_0 */

        {0}, /* CONN_OPEN/SCO_CLOSE power mode settings for pm_spec USER_DEFINED_1
              */
        {0}  /* SCO_OPEN power mode settings for pm_spec USER_DEFINED_1 */
#endif       /* BTE_SIM_APP */
};

/* 0=max_lat -> no SSR */
/* the smaller of the SSR max latency wins.
 * the entries in this table must be from highest latency (biggest interval) to
 * lowest latency */
tBTA_DM_SSR_SPEC bta_dm_ssr_spec[] = {
        /*max_lat, min_rmt_to, min_loc_to*/
        {0, 0, 0, "no_ssr"}, /* BTA_DM_PM_SSR0 - do not use SSR */
        /* BTA_DM_PM_SSR1 - HH, can NOT share entry with any other profile, setting
           default max latency and min remote timeout as 0, and always read
           individual device preference from HH module */
        {0, 0, 2, "hid_host"},
        {1200, 2, 2, "sniff_capable"},  /* BTA_DM_PM_SSR2 - others (only if sniff is allowed) */
        {360, 160, 1600, "hid_device"}, /* BTA_DM_PM_SSR3 - HD */
        {1200, 65534, 65534, "a2dp"}    /* BTA_DM_PM_SSR4 - A2DP streaming */
};

tBTA_DM_SSR_SPEC* p_bta_dm_ssr_spec = &bta_dm_ssr_spec[0];

const tBTA_DM_PM_CFG* p_bta_dm_pm_cfg = &bta_dm_pm_cfg[0];
const tBTM_PM_PWR_MD* p_bta_dm_pm_md = &bta_dm_pm_md[0];

/* The performance impact of EIR packet size
 *
 * 1 to 17 bytes,    DM1 is used and most robust.
 * 18 to 121 bytes,  DM3 is used but impacts inquiry scan time with large number
 *                    of devices.(almost double with 150 users)
 * 122 to 224 bytes, DM5 is used but cause quite big performance loss even with
 *                    small number of users. so it is not recommended.
 * 225 to 240 bytes, DH5 is used without FEC but it not recommended.
 *                    (same reason of DM5)
 */

/* Extended Inquiry Response */
const tBTA_DM_EIR_CONF bta_dm_eir_cfg = {
        50, /* minimum length of local name when it is shortened */
            /* if length of local name is longer than this and EIR has not enough */
            /* room for all UUID list then local name is shortened to this length */
        {
                /* mask of UUID list in EIR */
                0xFFFFFFFF, /* LSB is the first UUID of the first 32 UUIDs in
                               BTM_EIR_UUID_LKUP_TBL */
                0xFFFFFFFF  /* LSB is the first UUID of the next 32 UUIDs in
                               BTM_EIR_UUID_LKUP_TBL */
                            /* BTM_EIR_UUID_LKUP_TBL can be overrided */
        },
        NULL, /* Inquiry TX power         */
        0,    /* length of flags in bytes */
        NULL, /* flags for EIR */
        0,    /* length of manufacturer specific in bytes */
        NULL, /* manufacturer specific */
        0,    /* length of additional data in bytes */
        NULL  /* additional data */
};
const tBTA_DM_EIR_CONF* p_bta_dm_eir_cfg = &bta_dm_eir_cfg;
