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
 *  This is the public interface file for the audio gateway (AG) subsystem
 *  of BTA, Broadcom's Bluetooth application layer for mobile phones.
 *
 ******************************************************************************/
#ifndef BTA_AG_API_H
#define BTA_AG_API_H

#include <bluetooth/log.h>

#include <string>
#include <vector>

#include "bta/include/bta_api.h"
#include "bta_api.h"
#include "internal_include/bt_target.h"
#include "macros.h"
#include "types/raw_address.h"

/*****************************************************************************
 *  Constants and data types
 ****************************************************************************/
/* Number of SCBs (AG service instances that can be registered) */
#define BTA_AG_MAX_NUM_CLIENTS 6

/* AG feature masks */
#define BTA_AG_FEAT_3WAY 0x00000001   /* Three-way calling */
#define BTA_AG_FEAT_ECNR 0x00000002   /* Echo cancellation/noise reduction */
#define BTA_AG_FEAT_VREC 0x00000004   /* Voice recognition */
#define BTA_AG_FEAT_INBAND 0x00000008 /* In-band ring tone */
#define BTA_AG_FEAT_VTAG 0x00000010   /* Attach a phone number to a voice tag */
#define BTA_AG_FEAT_REJECT 0x00000020 /* Ability to reject incoming call */
#define BTA_AG_FEAT_ECS 0x00000040    /* Enhanced Call Status */
#define BTA_AG_FEAT_ECC 0x00000080    /* Enhanced Call Control */
#define BTA_AG_FEAT_EXTERR 0x00000100 /* Extended error codes */
#define BTA_AG_FEAT_CODEC 0x00000200  /* Codec Negotiation */
#define BTA_AG_FEAT_SWB 0x00001000    /* Super Wide Band */

/* AG SDP feature masks */
#define BTA_AG_FEAT_WBS_SUPPORT 0x0020 /* Supports WBS */
#define BTA_AG_FEAT_SWB_SUPPORT 0x0100 /* Supports SWB */

/* Only SDP feature bits 0 to 4 matches BRSF feature bits */
#define HFP_SDP_BRSF_FEATURES_MASK 0x001F

/* Valid feature bit mask for HFP 1.6 (and below) */
#define HFP_1_6_FEAT_MASK 0x000003FF

/* HFP 1.7+ */
#define BTA_AG_FEAT_HF_IND 0x00000400  /* HF Indicators */
#define BTA_AG_FEAT_ESCO_S4 0x00000800 /* eSCO S4 setting supported */

/* Proprietary features: using 31 ~ 16 bits */
#define BTA_AG_FEAT_BTRH 0x00010000    /* CCAP incoming call hold */
#define BTA_AG_FEAT_UNAT 0x00020000    /* Pass unknown AT commands to app */
#define BTA_AG_FEAT_NOSCO 0x00040000   /* No SCO control performed by BTA AG */
#define BTA_AG_FEAT_NO_ESCO 0x00080000 /* Do not allow or use eSCO */
#define BTA_AG_FEAT_VOIP 0x00100000    /* VoIP call */

typedef uint32_t tBTA_AG_FEAT;

/* AG open status */
#define BTA_AG_SUCCESS 0        /* Connection successfully opened */
#define BTA_AG_FAIL_SDP 1       /* Open failed due to SDP */
#define BTA_AG_FAIL_RFCOMM 2    /* Open failed due to RFCOMM */
#define BTA_AG_FAIL_RESOURCES 3 /* out of resources failure */

/* Status to disallow passing AT Events after BTIF */
#define BTA_AG_DISALLOW_AT 5

typedef uint8_t tBTA_AG_STATUS;

/* handle values used with BTA_AgResult */
#define BTA_AG_HANDLE_NONE 0
#define BTA_AG_HANDLE_ALL 0xFFFF
/* It is safe to use the same value as BTA_AG_HANDLE_ALL
 * HANDLE_ALL is used for delivering indication
 * SCO_NO_CHANGE is used for changing sco behavior
 * They donot interfere with each other
 */

/* Number of supported HF indicators, there is one HF indicator so far i.e.
                                                   enhanced driver status. */
/* Number of supported HF indicators,
   1 for Enhanced Safety Status
   2 for Battery Level Status */
#ifndef BTA_AG_NUM_LOCAL_HF_IND
#define BTA_AG_NUM_LOCAL_HF_IND 2
#endif

#define BTA_AG_HANDLE_SCO_NO_CHANGE 0xFFFF

typedef enum : uint8_t {
  /* AG result codes used with BTA_AgResult */
  BTA_AG_SPK_RES = 0,            /* Update speaker volume */
  BTA_AG_MIC_RES = 1,            /* Update microphone volume */
  BTA_AG_INBAND_RING_RES = 2,    /* Update inband ring state */
  BTA_AG_CIND_RES = 3,           /* Send indicator response for AT+CIND */
  BTA_AG_BINP_RES = 4,           /* Send phone number for voice tag for AT+BINP */
  BTA_AG_IND_RES = 5,            /* Update an indicator value */
  BTA_AG_BVRA_RES = 6,           /* Update voice recognition state */
  BTA_AG_CNUM_RES = 7,           /* Send subscriber number response for AT+CNUM */
  BTA_AG_BTRH_RES = 8,           /* Send CCAP incoming call hold */
  BTA_AG_CLCC_RES = 9,           /* Query list of calls */
  BTA_AG_COPS_RES = 10,          /* Read network operator */
  BTA_AG_IN_CALL_RES = 11,       /* Indicate incoming phone call */
  BTA_AG_IN_CALL_CONN_RES = 12,  /* Incoming phone call connected */
  BTA_AG_CALL_WAIT_RES = 13,     /* Call waiting notification */
  BTA_AG_OUT_CALL_ORIG_RES = 14, /* Outgoing phone call origination */

  /* Outgoing phone call alerting remote party */
  BTA_AG_OUT_CALL_ALERT_RES = 15,
  BTA_AG_OUT_CALL_CONN_RES = 16, /* Outgoing phone call connected */

  /* Incoming/outgoing =  3,-way canceled before connected */
  BTA_AG_CALL_CANCEL_RES = 17,
  BTA_AG_END_CALL_RES = 18,      /* End call */
  BTA_AG_IN_CALL_HELD_RES = 19,  /* Incoming call held */
  BTA_AG_UNAT_RES = 20,          /* Response to unknown AT command event */
  BTA_AG_MULTI_CALL_RES = 21,    /* SLC at three way call */
  BTA_AG_BIND_RES = 22,          /* Activate/Deactivate HF indicator */
  BTA_AG_IND_RES_ON_DEMAND = 33, /* Update an indicator value forcible */
  BTA_AG_UNKNOWN = 0xff,         // Should be unset
} tBTA_AG_RES;

inline std::string bta_ag_result_text(const tBTA_AG_RES& result) {
  switch (result) {
    CASE_RETURN_TEXT(BTA_AG_SPK_RES);
    CASE_RETURN_TEXT(BTA_AG_MIC_RES);
    CASE_RETURN_TEXT(BTA_AG_INBAND_RING_RES);
    CASE_RETURN_TEXT(BTA_AG_CIND_RES);
    CASE_RETURN_TEXT(BTA_AG_BINP_RES);
    CASE_RETURN_TEXT(BTA_AG_IND_RES);
    CASE_RETURN_TEXT(BTA_AG_BVRA_RES);
    CASE_RETURN_TEXT(BTA_AG_CNUM_RES);
    CASE_RETURN_TEXT(BTA_AG_BTRH_RES);
    CASE_RETURN_TEXT(BTA_AG_CLCC_RES);
    CASE_RETURN_TEXT(BTA_AG_COPS_RES);
    CASE_RETURN_TEXT(BTA_AG_IN_CALL_RES);
    CASE_RETURN_TEXT(BTA_AG_IN_CALL_CONN_RES);
    CASE_RETURN_TEXT(BTA_AG_CALL_WAIT_RES);
    CASE_RETURN_TEXT(BTA_AG_OUT_CALL_ORIG_RES);
    CASE_RETURN_TEXT(BTA_AG_OUT_CALL_ALERT_RES);
    CASE_RETURN_TEXT(BTA_AG_OUT_CALL_CONN_RES);
    CASE_RETURN_TEXT(BTA_AG_CALL_CANCEL_RES);
    CASE_RETURN_TEXT(BTA_AG_END_CALL_RES);
    CASE_RETURN_TEXT(BTA_AG_IN_CALL_HELD_RES);
    CASE_RETURN_TEXT(BTA_AG_UNAT_RES);
    CASE_RETURN_TEXT(BTA_AG_MULTI_CALL_RES);
    CASE_RETURN_TEXT(BTA_AG_BIND_RES);
    CASE_RETURN_TEXT(BTA_AG_IND_RES_ON_DEMAND);
    CASE_RETURN_TEXT(BTA_AG_UNKNOWN);
    default:
      return std::string("UNKNOWN");
  }
}

/* HFP peer features */
#define BTA_AG_PEER_FEAT_ECNR 0x0001    /* Echo cancellation/noise reduction */
#define BTA_AG_PEER_FEAT_3WAY 0x0002    /* Call waiting and three-way calling */
#define BTA_AG_PEER_FEAT_CLI 0x0004     /* Caller ID presentation capability */
#define BTA_AG_PEER_FEAT_VREC 0x0008    /* Voice recognition activation */
#define BTA_AG_PEER_FEAT_VOL 0x0010     /* Remote volume control */
#define BTA_AG_PEER_FEAT_ECS 0x0020     /* Enhanced Call Status */
#define BTA_AG_PEER_FEAT_ECC 0x0040     /* Enhanced Call Control */
#define BTA_AG_PEER_FEAT_CODEC 0x0080   /* Codec Negotiation */
#define BTA_AG_PEER_FEAT_HF_IND 0x0100  /* HF Indicators */
#define BTA_AG_PEER_FEAT_ESCO_S4 0x0200 /* eSCO S4 setting supported */

/* Proprietary features: using bits after 12 */

/* Pass unknown AT command responses to application */
#define BTA_AG_PEER_FEAT_UNAT 0x1000
#define BTA_AG_PEER_FEAT_VOIP 0x2000 /* VoIP call */

typedef uint16_t tBTA_AG_PEER_FEAT;

/* HFP peer supported codec masks */
typedef uint16_t tBTA_AG_PEER_CODEC;

/* HFP errcode - Set when BTA_AG_OK_ERROR is returned in 'ok_flag' */
#define BTA_AG_ERR_PHONE_FAILURE 0     /* Phone Failure */
#define BTA_AG_ERR_NO_CONN_PHONE 1     /* No connection to phone */
#define BTA_AG_ERR_OP_NOT_ALLOWED 3    /* Operation not allowed */
#define BTA_AG_ERR_OP_NOT_SUPPORTED 4  /* Operation not supported */
#define BTA_AG_ERR_PHSIM_PIN_REQ 5     /* PH-SIM PIN required */
#define BTA_AG_ERR_SIM_NOT_INSERTED 10 /* SIM not inserted */
#define BTA_AG_ERR_SIM_PIN_REQ 11      /* SIM PIN required */
#define BTA_AG_ERR_SIM_PUK_REQ 12      /* SIM PUK required */
#define BTA_AG_ERR_SIM_FAILURE 13      /* SIM failure */
#define BTA_AG_ERR_SIM_BUSY 14         /* SIM busy */
#define BTA_AG_ERR_INCORRECT_PWD 16    /* Incorrect password */
#define BTA_AG_ERR_SIM_PIN2_REQ 17     /* SIM PIN2 required */
#define BTA_AG_ERR_SIM_PUK2_REQ 18     /* SIM PUK2 required */
#define BTA_AG_ERR_MEMORY_FULL 20      /* Memory full */
#define BTA_AG_ERR_INVALID_INDEX 21    /* Invalid index */
#define BTA_AG_ERR_MEMORY_FAILURE 23   /* Memory failure */
#define BTA_AG_ERR_TEXT_TOO_LONG 24    /* Text string too long */
#define BTA_AG_ERR_INV_CHAR_IN_TSTR 25 /* Invalid characters in text string */
#define BTA_AG_ERR_DSTR_TOO_LONG 26    /* Dial string too long */
#define BTA_AG_ERR_INV_CHAR_IN_DSTR 27 /* Invalid characters in dial string */
#define BTA_AG_ERR_NO_NETWORK_SERV 30  /* No network service */
#define BTA_AG_ERR_NETWORK_TIME_OUT 31 /* Network timeout */
/* Network not allowed - emergency service only */
#define BTA_AG_ERR_NO_NET_EMG_ONLY 32
/* AG cannot create simultaneous VoIP and CS calls */
#define BTA_AG_ERR_VOIP_CS_CALLS 33
#define BTA_AG_ERR_NOT_FOR_VOIP 34  /* Not supported on this call type(VoIP) */
#define BTA_AG_ERR_SIP_RESP_CODE 35 /* SIP 3 digit response code */

#if 0                                 /* Not Used in Bluetooth HFP 1.5 Specification */
#define BTA_AG_ERR_PHADAP_LNK_RES 2   /* Phone-adapter link reserved */
#define BTA_AG_ERR_PHFSIM_PIN_REQ 6   /* PH-FSIM PIN required */
#define BTA_AG_ERR_PHFSIM_PUK_REQ 7   /* PH-FSIM PUK required */
#define BTA_AG_ERR_SIM_WRONG 15       /* SIM wrong */
#define BTA_AG_ERR_NOT_FOUND 22       /* Not found */
#define BTA_AG_ERR_NETWORK_TIMEOUT 31 /* Network timeout */
#define BTA_AG_ERR_NET_PIN_REQ 40     /* Network personalization PIN required */
#define BTA_AG_ERR_NET_PUK_REQ 41     /* Network personalization PUK required */
/* Network subset personalization PIN required */
#define BTA_AG_ERR_SUBSET_PIN_REQ 42
/* Network subset personalization PUK required */
#define BTA_AG_ERR_SUBSET_PUK_REQ 43
/* Service provider personalization PIN required */
#define BTA_AG_ERR_SERVPRO_PIN_REQ 44
/* Service provider personalization PUK required */
#define BTA_AG_ERR_SERVPRO_PUK_REQ 45
/* Corporate personalization PIN required */
#define BTA_AG_ERR_CORP_PIN_REQ 46
/* Corporate personalization PUK required */
#define BTA_AG_ERR_CORP_PUK_REQ 47
#define BTA_AG_ERR_UNKNOWN 100          /* Unknown error */

/* GPRS-related errors */
#define BTA_AG_ERR_ILL_MS 103           /* Illegal MS (#3) */
#define BTA_AG_ERR_ILL_ME 106           /* Illegal ME (#6) */
#define BTA_AG_ERR_GPRS_NOT_ALLOWED 107 /* GPRS services not allowed (#7) */
#define BTA_AG_ERR_PLMN_NOT_ALLOWED 111 /* PLMN services not allowed (#11) */
#define BTA_AG_ERR_LOC_NOT_ALLOWED 112  /* Location area not allowed (#12) */
/* Roaming not allowed in this location area (#13) */
#define BTA_AG_ERR_ROAM_NOT_ALLOWED 113
/* Errors related to a failure to Activate a Context */
#define BTA_AG_ERR_OPT_NOT_SUPP 132 /* Service option not supported (#32) */
/* Requested service option not subscribed (#33) */
#define BTA_AG_ERR_OPT_NOT_SUBSCR 133
/* Service option temporarily out of order (#34) */
#define BTA_AG_ERR_OPT_OUT_OF_ORDER 134
#define BTA_AG_ERR_PDP_AUTH_FAILURE 149 /* PDP authentication failure */
/* Other GPRS errors */
#define BTA_AG_ERR_INV_MOBILE_CLASS 150 /* Invalid mobile class */
#define BTA_AG_ERR_UNSPEC_GPRS_ERR 148  /* Unspecified GPRS error */
#endif                                  /* Unused error codes */

/* HFP result data 'ok_flag' */
#define BTA_AG_OK_CONTINUE 0 /* Send out response (more responses coming) */
#define BTA_AG_OK_DONE 1     /* Send out response followed by OK (finished) */
#define BTA_AG_OK_ERROR 2    /* Error response */

/* BTRH values */
#define BTA_AG_BTRH_SET_HOLD 0 /* Put incoming call on hold */
#define BTA_AG_BTRH_SET_ACC 1  /* Accept incoming call on hold */
#define BTA_AG_BTRH_SET_REJ 2  /* Reject incoming call on hold */
#define BTA_AG_BTRH_READ 3     /* Read the current value */
#define BTA_AG_BTRH_NO_RESP 4  /* Not in RH States (reply to read) */

/* clip type constants */
#define BTA_AG_CLIP_TYPE_MIN 128
#define BTA_AG_CLIP_TYPE_MAX 175
#define BTA_AG_CLIP_TYPE_DEFAULT 129
#define BTA_AG_CLIP_TYPE_VOIP 255

/* ASCII character string of arguments to the AT command or result */
#ifndef BTA_AG_AT_MAX_LEN
#define BTA_AG_AT_MAX_LEN 256
#endif

namespace hfp {

// Define hfp offload config structure
struct offload_config {
  tBTA_AG_UUID_CODEC sco_codec;
  int32_t connection_handle;
  bool is_controller_codec;
  bool is_nrec;
};

// Define hfp pcm config structure
struct pcm_config {
  int sample_rate_hz;
  // The below fields are always fixed
  // int channel_mode = ChannelMode::MONO;
  // uint8_t bits_per_sample = 16;
  // int data_interval_us = 7500;
};

struct sco_config {
  int inputDataPath;
  int outputDataPath;
  bool useControllerCodec;
};

}  // namespace hfp
/* data associated with BTA_AG_IND_RES */
typedef struct {
  uint16_t id;
  uint16_t value;
  bool on_demand;
} tBTA_AG_IND;

/* data type for BTA_AgResult() */
struct tBTA_AG_RES_DATA {
  char str[BTA_AG_AT_MAX_LEN + 1];
  tBTA_AG_IND ind;
  uint16_t num;
  uint16_t audio_handle;
  uint16_t errcode; /* Valid only if 'ok_flag' is set to BTA_AG_OK_ERROR */
  uint8_t ok_flag;  /* Indicates if response is finished, and if error occurred */
  bool state;
  static const tBTA_AG_RES_DATA kEmpty;
};

typedef enum : uint8_t {
  /* AG callback events */
  BTA_AG_ENABLE_EVT = 0,      /* AG enabled */
  BTA_AG_REGISTER_EVT = 1,    /* AG registered */
  BTA_AG_OPEN_EVT = 2,        /* AG connection open */
  BTA_AG_CLOSE_EVT = 3,       /* AG connection closed */
  BTA_AG_CONN_EVT = 4,        /* Service level connection opened */
  BTA_AG_AUDIO_OPEN_EVT = 5,  /* Audio connection open */
  BTA_AG_AUDIO_CLOSE_EVT = 6, /* Audio connection closed */
  BTA_AG_SPK_EVT = 7,         /* Speaker volume changed */
  BTA_AG_MIC_EVT = 8,         /* Microphone volume changed */
  BTA_AG_AT_CKPD_EVT = 9,     /* CKPD from the HS */
  BTA_AG_DISABLE_EVT = 30,    /* AG disabled */
  BTA_AG_CODEC_EVT = 31,      /* SCO codec info */
  /* Values below are for HFP only */
  BTA_AG_AT_A_EVT = 10,    /* Answer a call */
  BTA_AG_AT_D_EVT = 11,    /* Place a call using number or memory dial */
  BTA_AG_AT_CHLD_EVT = 12, /* Call hold */
  BTA_AG_AT_CHUP_EVT = 13, /* Hang up a call */
  BTA_AG_AT_CIND_EVT = 14, /* Read indicator settings */
  BTA_AG_AT_VTS_EVT = 15,  /* Transmit DTMF tone */
  BTA_AG_AT_BINP_EVT = 16, /* Retrieve number from voice tag */
  BTA_AG_AT_BLDN_EVT = 17, /* Place call to last dialed number */
  BTA_AG_AT_BVRA_EVT = 18, /* Enable/disable voice recognition */
  BTA_AG_AT_NREC_EVT = 19, /* Disable echo canceling */
  BTA_AG_AT_CNUM_EVT = 20, /* Retrieve subscriber number */
  BTA_AG_AT_BTRH_EVT = 21, /* CCAP-style incoming call hold */
  BTA_AG_AT_CLCC_EVT = 22, /* Query list of current calls */
  BTA_AG_AT_COPS_EVT = 23, /* Query list of current calls */
  BTA_AG_AT_UNAT_EVT = 24, /* Unknown AT command */
  BTA_AG_AT_CBC_EVT = 25,  /* Battery Level report from HF */
  BTA_AG_AT_BAC_EVT = 26,  /* avablable codec */
  BTA_AG_AT_BCS_EVT = 27,  /* Codec select */
  BTA_AG_AT_BIND_EVT = 28, /* HF indicator */
  BTA_AG_AT_BIEV_EVT = 29, /* HF indicator updates from peer */
  BTA_AG_AT_BIA_EVT = 32,  /* AG indicator activation event from peer */
} tBTA_AG_EVT;

/* data associated with most non-AT events */
typedef struct {
  uint16_t handle;
  uint8_t app_id;
  tBTA_AG_STATUS status;
} tBTA_AG_HDR;

/* data associated with BTA_AG_REGISTER_EVT */
typedef struct {
  tBTA_AG_HDR hdr;
  tBTA_AG_STATUS status;
} tBTA_AG_REGISTER;

/* data associated with BTA_AG_OPEN_EVT */
typedef struct {
  tBTA_AG_HDR hdr;
  RawAddress bd_addr;
  tBTA_SERVICE_ID service_id;
  tBTA_AG_STATUS status;
} tBTA_AG_OPEN;

/* data associated with BTA_AG_CLOSE_EVT */
typedef struct {
  tBTA_AG_HDR hdr;
  RawAddress bd_addr;
} tBTA_AG_CLOSE;

/* data associated with BTA_AG_CONN_EVT */
typedef struct {
  tBTA_AG_HDR hdr;
  tBTA_AG_PEER_FEAT peer_feat;
  RawAddress bd_addr;
  tBTA_AG_PEER_CODEC peer_codec;
} tBTA_AG_CONN;

/* data associated with AT command event */
typedef struct {
  tBTA_AG_HDR hdr;
  RawAddress bd_addr;
  char str[BTA_AG_AT_MAX_LEN + 1];
  uint32_t num;
  uint8_t idx;   /* call number used by CLCC and CHLD */
  uint16_t lidx; /* long index, ex, HF indicator */
} tBTA_AG_VAL;

/* union of data associated with AG callback */
typedef union {
  tBTA_AG_HDR hdr;
  tBTA_AG_REGISTER reg;
  tBTA_AG_OPEN open;
  tBTA_AG_CLOSE close;
  tBTA_AG_CONN conn;
  tBTA_AG_VAL val;
} tBTA_AG;

/* AG callback */
typedef void(tBTA_AG_CBACK)(tBTA_AG_EVT event, tBTA_AG* p_data);

/* indicator constants HFP 1.1 and later */
#define BTA_AG_IND_CALL 1      /* position of call indicator */
#define BTA_AG_IND_CALLSETUP 2 /* position of callsetup indicator */
#define BTA_AG_IND_SERVICE 3   /* position of service indicator */

/* indicator constants HFP 1.5 and later */
#define BTA_AG_IND_SIGNAL 4   /* position of signal strength indicator */
#define BTA_AG_IND_ROAM 5     /* position of roaming indicator */
#define BTA_AG_IND_BATTCHG 6  /* position of battery charge indicator */
#define BTA_AG_IND_CALLHELD 7 /* position of callheld indicator */
#define BTA_AG_IND_BEARER 8   /* position of bearer indicator */

/* call indicator values */
#define BTA_AG_CALL_INACTIVE 0 /* Phone call inactive */
#define BTA_AG_CALL_ACTIVE 1   /* Phone call active */

/* callsetup indicator values */
#define BTA_AG_CALLSETUP_NONE 0     /* Not currently in call set up */
#define BTA_AG_CALLSETUP_INCOMING 1 /* Incoming call process ongoing */
#define BTA_AG_CALLSETUP_OUTGOING 2 /* Outgoing call set up is ongoing */
/* Remote party being alerted in an outgoing call */
#define BTA_AG_CALLSETUP_ALERTING 3

/* service indicator values */
#define BTA_AG_SERVICE_NONE 0    /* Neither CS nor VoIP service is available */
#define BTA_AG_SERVICE_CS 1      /* Only CS service is available */
#define BTA_AG_SERVICE_VOIP 2    /* Only VoIP service is available */
#define BTA_AG_SERVICE_CS_VOIP 3 /* Both CS and VoIP services available */

/* callheld indicator values */
#define BTA_AG_CALLHELD_INACTIVE 0 /* No held calls */
#define BTA_AG_CALLHELD_ACTIVE 1   /* Call held and call active */
#define BTA_AG_CALLHELD_NOACTIVE 2 /* Call held and no call active */

/* signal strength indicator values */
#define BTA_AG_ROAMING_INACTIVE 0 /* Phone call inactive */
#define BTA_AG_ROAMING_ACTIVE 1   /* Phone call active */

/* bearer indicator values */
#define BTA_AG_BEARER_WLAN 0      /* WLAN         */
#define BTA_AG_BEARER_BLUETOOTH 1 /* Bluetooth    */
#define BTA_AG_BEARER_WIRED 2     /* Wired        */
#define BTA_AG_BEARER_2G3G 3      /* 2G 3G        */
#define BTA_AG_BEARER_WIMAX 4     /* WIMAX        */
#define BTA_AG_BEARER_RES1 5      /* Reserved     */
#define BTA_AG_BEARER_RES2 6      /* Reserved     */
#define BTA_AG_BEARER_RES3 7      /* Reserved     */

/* type for HF indicator */
typedef struct {
  uint16_t ind_id;
  bool is_supported;
  bool is_enable;
  uint32_t ind_min_val;
  uint32_t ind_max_val;
} tBTA_AG_HF_IND;

/* AG configuration structure */
typedef struct {
  const char* cind_info;
  const char* bind_info;
  uint8_t num_local_hf_ind;
  int32_t conn_tout;
  uint16_t sco_pkt_types;
  const char* chld_val_ecc;
  const char* chld_val;
} tBTA_AG_CFG;

/*****************************************************************************
 *  External Function Declarations
 ****************************************************************************/

/*******************************************************************************
 *
 * Function         BTA_AgEnable
 *
 * Description      Enable the audio gateway service. When the enable
 *                  operation is complete the callback function will be
 *                  called with a BTA_AG_ENABLE_EVT. This function must
 *                  be called before other function in the AG API are
 *                  called.
 *
 * Returns          BTA_SUCCESS if OK, BTA_FAILURE otherwise.
 *
 ******************************************************************************/
tBTA_STATUS BTA_AgEnable(tBTA_AG_CBACK* p_cback);

/*******************************************************************************
 *
 * Function         BTA_AgDisable
 *
 * Description      Disable the audio gateway service
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgDisable();

/*******************************************************************************
 *
 * Function         BTA_AgRegister
 *
 * Description      Register an Audio Gateway service.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgRegister(tBTA_SERVICE_MASK services, tBTA_AG_FEAT features,
                    const std::vector<std::string>& service_names, uint8_t app_id);

/*******************************************************************************
 *
 * Function         BTA_AgDeregister
 *
 * Description      Deregister an audio gateway service.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgDeregister(uint16_t handle);

/*******************************************************************************
 *
 * Function         BTA_AgOpen
 *
 * Description      Opens a connection to a headset or hands-free device.
 *                  When connection is open callback function is called
 *                  with a BTA_AG_OPEN_EVT. Only the data connection is
 *                  opened. The audio connection is not opened.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgOpen(uint16_t handle, const RawAddress& bd_addr);

/*******************************************************************************
 *
 * Function         BTA_AgClose
 *
 * Description      Close the current connection to a headset or a handsfree
 *                  Any current audio connection will also be closed
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgClose(uint16_t handle);

/*******************************************************************************
 *
 * Function         BTA_AgAudioOpen
 *
 * Description      Opens an audio connection to the currently connected
 *                  headset or handsfree. Specify `disabled_codecs` to
 *                  force the stack to avoid using certain codecs.
 *
 *                  Note that CVSD is a mandatory codec and cannot be disabled.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgAudioOpen(uint16_t handle, tBTA_AG_PEER_CODEC disabled_codecs);

/*******************************************************************************
 *
 * Function         BTA_AgAudioClose
 *
 * Description      Close the currently active audio connection to a headset
 *                  or hnadsfree. The data connection remains open
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgAudioClose(uint16_t handle);

/*******************************************************************************
 *
 * Function         BTA_AgResult
 *
 * Description      Send an AT result code to a headset or hands-free device.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgResult(uint16_t handle, tBTA_AG_RES result, const tBTA_AG_RES_DATA& data);

/*******************************************************************************
 *
 * Function         BTA_AgSetCodec
 *
 * Description      Specify the codec type to be used for the subsequent
 *                  audio connection.
 *
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void BTA_AgSetCodec(uint16_t handle, tBTA_AG_PEER_CODEC codec);

void BTA_AgSetScoOffloadEnabled(bool value);

void BTA_AgSetScoAllowed(bool value);

void BTA_AgSetActiveDevice(const RawAddress& active_device_addr);

namespace std {
template <>
struct formatter<tBTA_AG_RES> : enum_formatter<tBTA_AG_RES> {};
}  // namespace std

#endif /* BTA_AG_API_H */
