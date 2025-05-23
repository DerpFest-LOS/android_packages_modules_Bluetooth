/******************************************************************************
 *
 *  Copyright 2006-2012 Broadcom Corporation
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
 *  This module contains the action functions associated with the channel
 *  control block state machine.
 *
 ******************************************************************************/

#define LOG_TAG "bluetooth-a2dp"

#include <bluetooth/log.h>
#include <string.h>

#include <cstdint>

#include "avdt_api.h"
#include "avdt_defs.h"
#include "avdt_int.h"
#include "avdtc_api.h"
#include "internal_include/bt_target.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/fixed_queue.h"
#include "stack/include/bt_hdr.h"
#include "types/raw_address.h"

using namespace bluetooth;

/*******************************************************************************
 *
 * Function         avdt_ccb_clear_ccb
 *
 * Description      This function clears out certain buffers, queues, and
 *                  other data elements of a ccb.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_ccb_clear_ccb(AvdtpCcb* p_ccb) {
  BT_HDR* p_buf;

  /* clear certain ccb variables */
  p_ccb->cong = false;
  p_ccb->ret_count = 0;

  /* free message being fragmented */
  osi_free_and_reset((void**)&p_ccb->p_curr_msg);

  /* free message being reassembled */
  osi_free_and_reset((void**)&p_ccb->p_rx_msg);

  /* clear out response queue */
  while ((p_buf = (BT_HDR*)fixed_queue_try_dequeue(p_ccb->rsp_q)) != NULL) {
    osi_free(p_buf);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_chan_open
 *
 * Description      This function calls avdt_ad_open_req() to
 *                  initiate a signaling channel connection.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_chan_open(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  avdt_ad_open_req(AVDT_CHAN_SIG, p_ccb, NULL, tAVDT_ROLE::AVDT_INT);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_chan_close
 *
 * Description      This function calls avdt_ad_close_req() to close a
 *                  signaling channel connection.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_chan_close(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  /* close the transport channel used by this CCB */
  avdt_ad_close_req(AVDT_CHAN_SIG, p_ccb, NULL);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_chk_close
 *
 * Description      This function checks for active streams on this CCB.
 *                  If there are none, it starts an idle timer.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_chk_close(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  int i;
  AvdtpScb* p_scb = &(p_ccb->scb[0]);

  /* see if there are any active scbs associated with this ccb */
  for (i = 0; i < AVDT_NUM_SEPS; i++, p_scb++) {
    if ((p_scb->allocated) && (p_scb->p_ccb == p_ccb)) {
      break;
    }
  }

  /* if no active scbs start idle timer */
  if (i == AVDT_NUM_SEPS) {
    alarm_cancel(p_ccb->ret_ccb_timer);
    alarm_cancel(p_ccb->rsp_ccb_timer);
    uint64_t interval_ms = avdtp_cb.rcb.idle_tout * 1000;
    alarm_set_on_mloop(p_ccb->idle_ccb_timer, interval_ms, avdt_ccb_idle_ccb_timer_timeout, p_ccb);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_discover_cmd
 *
 * Description      This function is called when a discover command is
 *                  received from the peer.  It gathers up the stream
 *                  information for all allocated streams and initiates
 *                  sending of a discover response.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_discover_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  tAVDT_SEP_INFO sep_info[AVDT_NUM_SEPS];
  AvdtpScb* p_scb = &(p_ccb->scb[0]);

  log::verbose("p_ccb index={}", avdt_ccb_to_idx(p_ccb));

  p_data->msg.discover_rsp.p_sep_info = sep_info;
  p_data->msg.discover_rsp.num_seps = 0;

  /* for all allocated scbs */
  for (int i = 0; i < AVDT_NUM_SEPS; i++, p_scb++) {
    if (p_scb->allocated) {
      /* copy sep info */
      sep_info[p_data->msg.discover_rsp.num_seps].in_use = p_scb->in_use;
      sep_info[p_data->msg.discover_rsp.num_seps].seid = p_scb->ScbHandle();
      sep_info[p_data->msg.discover_rsp.num_seps].media_type = p_scb->stream_config.media_type;
      sep_info[p_data->msg.discover_rsp.num_seps].tsep = p_scb->stream_config.tsep;

      p_data->msg.discover_rsp.num_seps++;
    }
  }

  /* send response */
  avdt_ccb_event(p_ccb, AVDT_CCB_API_DISCOVER_RSP_EVT, p_data);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_discover_rsp
 *
 * Description      This function is called when a discover response or
 *                  reject is received from the peer.  It calls the application
 *                  callback function with the results.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_discover_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  /* we're done with procedure */
  p_ccb->proc_busy = false;

  /* call app callback with results */
  (*p_ccb->proc_cback)(0, p_ccb->peer_addr, AVDT_DISCOVER_CFM_EVT,
                       (tAVDT_CTRL*)(&p_data->msg.discover_rsp), p_ccb->BtaAvScbIndex());
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_getcap_cmd
 *
 * Description      This function is called when a get capabilities command
 *                  is received from the peer.  It retrieves the stream
 *                  configuration for the requested stream and initiates
 *                  sending of a get capabilities response.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_getcap_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  /* look up scb for seid sent to us */
  AvdtpScb* p_scb = avdt_scb_by_hdl(p_data->msg.single.seid);

  if (p_scb == nullptr) {
    /* not ok, send reject */
    p_data->msg.hdr.err_code = AVDT_ERR_BAD_STATE;
    p_data->msg.hdr.err_param = p_data->msg.single.seid;
    avdt_msg_send_rej(p_ccb, AVDT_SIG_START, &p_data->msg);
    return;
  }

  p_data->msg.svccap.p_cfg = &p_scb->stream_config.cfg;

  avdt_ccb_event(p_ccb, AVDT_CCB_API_GETCAP_RSP_EVT, p_data);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_getcap_rsp
 *
 * Description      This function is called with a get capabilities response
 *                  or reject is received from the peer.  It calls the
 *                  application callback function with the results.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_getcap_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  /* we're done with procedure */
  p_ccb->proc_busy = false;

  /* call app callback with results */
  (*p_ccb->proc_cback)(0, p_ccb->peer_addr, AVDT_GETCAP_CFM_EVT, (tAVDT_CTRL*)(&p_data->msg.svccap),
                       p_ccb->BtaAvScbIndex());
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_start_cmd
 *
 * Description      This function is called when a start command is received
 *                  from the peer.  It verifies that all requested streams
 *                  are in the proper state.  If so, it initiates sending of
 *                  a start response.  Otherwise it sends a start reject.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_start_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  uint8_t err_code = 0;

  /* verify all streams in the right state */
  uint8_t seid = avdt_scb_verify(p_ccb, AVDT_VERIFY_START, p_data->msg.multi.seid_list,
                                 p_data->msg.multi.num_seps, &err_code);
  if (seid == 0 && err_code == 0) {
    /* we're ok, send response */
    avdt_ccb_event(p_ccb, AVDT_CCB_API_START_RSP_EVT, p_data);
  } else {
    /* not ok, send reject */
    p_data->msg.hdr.err_code = err_code;
    p_data->msg.hdr.err_param = seid;
    avdt_msg_send_rej(p_ccb, AVDT_SIG_START, &p_data->msg);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_start_rsp
 *
 * Description      This function is called when a start response or reject
 *                  is received from the peer.  Using the SEIDs stored in the
 *                  current command message, it sends a start response or start
 *                  reject event to each SCB associated with the command.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_start_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  uint8_t event;
  int i;
  uint8_t* p;
  AvdtpScb* p_scb;

  /* determine rsp or rej event */
  event = (p_data->msg.hdr.err_code == 0) ? AVDT_SCB_MSG_START_RSP_EVT : AVDT_SCB_MSG_START_REJ_EVT;

  /* get to where seid's are stashed in current cmd */
  p = (uint8_t*)(p_ccb->p_curr_cmd + 1);

  /* little trick here; length of current command equals number of streams */
  for (i = 0; i < p_ccb->p_curr_cmd->len; i++) {
    p_scb = avdt_scb_by_hdl(p[i]);
    if (p_scb != NULL) {
      avdt_scb_event(p_scb, event, (tAVDT_SCB_EVT*)&p_data->msg);
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_suspend_cmd
 *
 * Description      This function is called when a suspend command is received
 *                  from the peer.  It verifies that all requested streams are
 *                  in the proper state.  If so, it initiates sending of a
 *                  suspend response.  Otherwise it sends a suspend reject.

 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_suspend_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  uint8_t seid;
  uint8_t err_code = 0;

  /* verify all streams in the right state */
  if ((seid = avdt_scb_verify(p_ccb, AVDT_VERIFY_SUSPEND, p_data->msg.multi.seid_list,
                              p_data->msg.multi.num_seps, &err_code)) == 0 &&
      err_code == 0) {
    /* we're ok, send response */
    avdt_ccb_event(p_ccb, AVDT_CCB_API_SUSPEND_RSP_EVT, p_data);
  } else {
    /* not ok, send reject */
    p_data->msg.hdr.err_code = err_code;
    p_data->msg.hdr.err_param = seid;
    avdt_msg_send_rej(p_ccb, AVDT_SIG_SUSPEND, &p_data->msg);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_hdl_suspend_rsp
 *
 * Description      This function is called when a suspend response or reject
 *                  is received from the peer.  Using the SEIDs stored in the
 *                  current command message, it sends a suspend response or
 *                  suspend reject event to each SCB associated with the
 *                  command.
 *
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_hdl_suspend_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  uint8_t event;
  int i;
  uint8_t* p;
  AvdtpScb* p_scb;

  /* determine rsp or rej event */
  event = (p_data->msg.hdr.err_code == 0) ? AVDT_SCB_MSG_SUSPEND_RSP_EVT
                                          : AVDT_SCB_MSG_SUSPEND_REJ_EVT;

  /* get to where seid's are stashed in current cmd */
  p = (uint8_t*)(p_ccb->p_curr_cmd + 1);

  /* little trick here; length of current command equals number of streams */
  for (i = 0; i < p_ccb->p_curr_cmd->len; i++) {
    p_scb = avdt_scb_by_hdl(p[i]);
    if (p_scb != NULL) {
      avdt_scb_event(p_scb, event, (tAVDT_SCB_EVT*)&p_data->msg);
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_discover_cmd
 *
 * Description      This function is called to send a discover command to the
 *                  peer.  It copies variables needed for the procedure from
 *                  the event to the CCB.  It marks the CCB as busy and then
 *                  sends a discover command.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_discover_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  /* store info in ccb struct */
  p_ccb->p_proc_data = p_data->discover.p_sep_info;
  p_ccb->proc_cback = p_data->discover.p_cback;
  p_ccb->proc_param = p_data->discover.num_seps;

  /* we're busy */
  p_ccb->proc_busy = true;

  /* build and queue discover req */
  avdt_msg_send_cmd(p_ccb, NULL, AVDT_SIG_DISCOVER, NULL);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_discover_rsp
 *
 * Description      This function is called to send a discover response to
 *                  the peer.  It takes the stream information passed in the
 *                  event and sends a discover response.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_discover_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  /* send response */
  avdt_msg_send_rsp(p_ccb, AVDT_SIG_DISCOVER, &p_data->msg);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_getcap_cmd
 *
 * Description      This function is called to send a get capabilities command
 *                  to the peer.  It copies variables needed for the procedure
 *                  from the event to the CCB.  It marks the CCB as busy and
 *                  then sends a get capabilities command.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_getcap_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  uint8_t sig_id = AVDT_SIG_GETCAP;

  /* store info in ccb struct */
  p_ccb->p_proc_data = p_data->getcap.p_cfg;
  p_ccb->proc_cback = p_data->getcap.p_cback;

  /* we're busy */
  p_ccb->proc_busy = true;

  /* build and queue discover req */
  if (p_data->msg.hdr.sig_id == AVDT_SIG_GET_ALLCAP) {
    sig_id = AVDT_SIG_GET_ALLCAP;
  }

  avdt_msg_send_cmd(p_ccb, NULL, sig_id, (tAVDT_MSG*)&p_data->getcap.single);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_getcap_rsp
 *
 * Description      This function is called to send a get capabilities response
 *                  to the peer.  It takes the stream information passed in the
 *                  event and sends a get capabilities response.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_getcap_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  uint8_t sig_id = AVDT_SIG_GETCAP;

  if (p_data->msg.hdr.sig_id == AVDT_SIG_GET_ALLCAP) {
    sig_id = AVDT_SIG_GET_ALLCAP;
  }

  /* send response */
  avdt_msg_send_rsp(p_ccb, sig_id, &p_data->msg);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_start_cmd
 *
 * Description      This function is called to send a start command to the
 *                  peer.  It verifies that all requested streams are in the
 *                  proper state.  If so, it sends a start command.  Otherwise
 *                  send ourselves back a start reject.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_start_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  int i;
  AvdtpScb* p_scb;
  tAVDT_MSG avdt_msg;
  uint8_t seid_list[AVDT_NUM_SEPS];

  log::verbose("");

  /* make copy of our seid list */
  memcpy(seid_list, p_data->msg.multi.seid_list, p_data->msg.multi.num_seps);

  /* verify all streams in the right state */
  avdt_msg.hdr.err_param = avdt_scb_verify(p_ccb, AVDT_VERIFY_OPEN, p_data->msg.multi.seid_list,
                                           p_data->msg.multi.num_seps, &avdt_msg.hdr.err_code);
  if (avdt_msg.hdr.err_param == 0) {
    log::verbose("AVDT_SIG_START");

    /* set peer seid list in messsage */
    avdt_scb_peer_seid_list(&p_data->msg.multi);

    /* send command */
    avdt_msg_send_cmd(p_ccb, seid_list, AVDT_SIG_START, &p_data->msg);
  } else {
    /* failed; send ourselves a reject for each stream */
    for (i = 0; i < p_data->msg.multi.num_seps; i++) {
      p_scb = avdt_scb_by_hdl(seid_list[i]);
      if (p_scb != NULL) {
        log::verbose("AVDT_SCB_MSG_START_REJ_EVT: i={}", i);
        tAVDT_SCB_EVT avdt_scb_evt;
        avdt_scb_evt.msg.hdr = avdt_msg.hdr;
        avdt_scb_event(p_scb, AVDT_SCB_MSG_START_REJ_EVT, &avdt_scb_evt);
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_start_rsp
 *
 * Description      This function is called to send a start response to the
 *                  peer.  It takes the stream information passed in the event
 *                  and sends a start response.  Then it sends a start event
 *                  to the SCB for each stream.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_start_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  AvdtpScb* p_scb;
  int i;

  /* send response message */
  avdt_msg_send_rsp(p_ccb, AVDT_SIG_START, &p_data->msg);

  /* send start event to each scb */
  for (i = 0; i < p_data->msg.multi.num_seps; i++) {
    p_scb = avdt_scb_by_hdl(p_data->msg.multi.seid_list[i]);
    if (p_scb != NULL) {
      avdt_scb_event(p_scb, AVDT_SCB_MSG_START_CMD_EVT, NULL);
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_suspend_cmd
 *
 * Description      This function is called to send a suspend command to the
 *                  peer.  It verifies that all requested streams are in the
 *                  proper state.  If so, it sends a suspend command.
 *                  Otherwise it calls the callback function for each requested
 *                  stream and sends a suspend confirmation with failure.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_suspend_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  int i;
  AvdtpScb* p_scb;
  tAVDT_MSG avdt_msg;
  uint8_t seid_list[AVDT_NUM_SEPS];

  /* make copy of our seid list */
  memcpy(seid_list, p_data->msg.multi.seid_list, p_data->msg.multi.num_seps);

  /* verify all streams in the right state */
  avdt_msg.hdr.err_param =
          avdt_scb_verify(p_ccb, AVDT_VERIFY_STREAMING, p_data->msg.multi.seid_list,
                          p_data->msg.multi.num_seps, &avdt_msg.hdr.err_code);
  if (avdt_msg.hdr.err_param == 0) {
    /* set peer seid list in messsage */
    avdt_scb_peer_seid_list(&p_data->msg.multi);

    /* send command */
    avdt_msg_send_cmd(p_ccb, seid_list, AVDT_SIG_SUSPEND, &p_data->msg);
  } else {
    /* failed; send ourselves a reject for each stream */
    for (i = 0; i < p_data->msg.multi.num_seps; i++) {
      p_scb = avdt_scb_by_hdl(seid_list[i]);
      if (p_scb != NULL) {
        tAVDT_SCB_EVT avdt_scb_evt;
        avdt_scb_evt.msg.hdr = avdt_msg.hdr;
        avdt_scb_event(p_scb, AVDT_SCB_MSG_SUSPEND_REJ_EVT, &avdt_scb_evt);
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_suspend_rsp
 *
 * Description      This function is called to send a suspend response to the
 *                  peer.  It takes the stream information passed in the event
 *                  and sends a suspend response.  Then it sends a suspend event
 *                  to the SCB for each stream.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_suspend_rsp(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  AvdtpScb* p_scb;
  int i;

  /* send response message */
  avdt_msg_send_rsp(p_ccb, AVDT_SIG_SUSPEND, &p_data->msg);

  /* send start event to each scb */
  for (i = 0; i < p_data->msg.multi.num_seps; i++) {
    p_scb = avdt_scb_by_hdl(p_data->msg.multi.seid_list[i]);
    if (p_scb != NULL) {
      avdt_scb_event(p_scb, AVDT_SCB_MSG_SUSPEND_CMD_EVT, NULL);
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_clear_cmds
 *
 * Description      This function is called when the signaling channel is
 *                  closed to clean up any pending commands.  For each pending
 *                  command in the command queue, it frees the command and
 *                  calls the application callback function indicating failure.
 *                  Certain CCB variables are also initialized.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_clear_cmds(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  int i;
  AvdtpScb* p_scb = &(p_ccb->scb[0]);
  uint8_t err_code = AVDT_ERR_CONNECT;

  /* clear the ccb */
  avdt_ccb_clear_ccb(p_ccb);

  /* clear out command queue; this is a little tricky here; we need
  ** to handle the case where there is a command on deck in p_curr_cmd,
  ** plus we need to clear out the queue
  */
  do {
    /* we know p_curr_cmd = NULL after this */
    tAVDT_CCB_EVT avdt_ccb_evt;
    avdt_ccb_evt.err_code = err_code;
    avdt_ccb_cmd_fail(p_ccb, &avdt_ccb_evt);

    /* set up next message */
    p_ccb->p_curr_cmd = (BT_HDR*)fixed_queue_try_dequeue(p_ccb->cmd_q);
  } while (p_ccb->p_curr_cmd != NULL);

  /* send a CC_CLOSE_EVT any active scbs associated with this ccb */
  for (i = 0; i < AVDT_NUM_SEPS; i++, p_scb++) {
    if ((p_scb->allocated) && (p_scb->p_ccb == p_ccb)) {
      avdt_scb_event(p_scb, AVDT_SCB_CC_CLOSE_EVT, NULL);
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_cmd_fail
 *
 * Description      This function is called when there is a response timeout.
 *                  The currently pending command is freed and we fake a
 *                  reject message back to ourselves.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_cmd_fail(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  tAVDT_MSG msg;
  uint8_t evt;
  AvdtpScb* p_scb;

  if (p_ccb->p_curr_cmd != NULL) {
    /* set up data */
    msg.hdr.err_code = p_data->err_code;
    msg.hdr.err_param = 0;
    msg.hdr.ccb_idx = avdt_ccb_to_idx(p_ccb);

    /* pretend that we received a rej message */
    evt = avdt_msg_rej_2_evt[p_ccb->p_curr_cmd->event - 1];

    if (evt & AVDT_CCB_MKR) {
      tAVDT_CCB_EVT avdt_ccb_evt;
      avdt_ccb_evt.msg = msg;
      avdt_ccb_event(p_ccb, (uint8_t)(evt & ~AVDT_CCB_MKR), &avdt_ccb_evt);
    } else {
      /* we get the scb out of the current cmd */
      p_scb = avdt_scb_by_hdl(*((uint8_t*)(p_ccb->p_curr_cmd + 1)));
      if (p_scb != NULL) {
        tAVDT_SCB_EVT avdt_scb_evt;
        avdt_scb_evt.msg = msg;
        avdt_scb_event(p_scb, evt, &avdt_scb_evt);
      }
    }

    osi_free_and_reset((void**)&p_ccb->p_curr_cmd);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_free_cmd
 *
 * Description      This function is called when a response is received for a
 *                  currently pending command.  The command is freed.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_free_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  osi_free_and_reset((void**)&p_ccb->p_curr_cmd);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_cong_state
 *
 * Description      This function is called to set the congestion state for
 *                  the CCB.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_cong_state(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) { p_ccb->cong = p_data->llcong; }

/*******************************************************************************
 *
 * Function         avdt_ccb_ret_cmd
 *
 * Description      This function is called to retransmit the currently
 *                  pending command.  The retransmission count is incremented.
 *                  If the count reaches the maximum number of retransmissions,
 *                  the event is treated as a response timeout.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_ret_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  p_ccb->ret_count++;
  if (p_ccb->ret_count == AVDT_RET_MAX) {
    /* command failed */
    p_ccb->ret_count = 0;
    tAVDT_CCB_EVT avdt_ccb_evt;
    avdt_ccb_evt.err_code = AVDT_ERR_TIMEOUT;
    avdt_ccb_cmd_fail(p_ccb, &avdt_ccb_evt);

    /* go to next queued command */
    avdt_ccb_snd_cmd(p_ccb, p_data);
  } else {
    /* if command pending and we're not congested and not sending a fragment */
    if ((!p_ccb->cong) && (p_ccb->p_curr_msg == NULL) && (p_ccb->p_curr_cmd != NULL)) {
      /* make copy of message in p_curr_cmd and send it */
      BT_HDR* p_msg = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);
      memcpy(p_msg, p_ccb->p_curr_cmd,
             (sizeof(BT_HDR) + p_ccb->p_curr_cmd->offset + p_ccb->p_curr_cmd->len));
      avdt_msg_send(p_ccb, p_msg);
    }

    /* restart ret timer */
    alarm_cancel(p_ccb->idle_ccb_timer);
    alarm_cancel(p_ccb->rsp_ccb_timer);
    uint64_t interval_ms = avdtp_cb.rcb.ret_tout * 1000;
    alarm_set_on_mloop(p_ccb->ret_ccb_timer, interval_ms, avdt_ccb_ret_ccb_timer_timeout, p_ccb);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_cmd
 *
 * Description      This function is called the send the next command,
 *                  if any, in the command queue.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_cmd(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  BT_HDR* p_msg;

  /* do we have commands to send?  send next command;  make sure we're clear;
  ** not congested, not sending fragment, not waiting for response
  */
  if ((!p_ccb->cong) && (p_ccb->p_curr_msg == NULL) && (p_ccb->p_curr_cmd == NULL)) {
    p_msg = (BT_HDR*)fixed_queue_try_dequeue(p_ccb->cmd_q);
    if (p_msg != NULL) {
      /* make a copy of buffer in p_curr_cmd */
      p_ccb->p_curr_cmd = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);
      memcpy(p_ccb->p_curr_cmd, p_msg, (sizeof(BT_HDR) + p_msg->offset + p_msg->len));
      avdt_msg_send(p_ccb, p_msg);
    }
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_snd_msg
 *
 * Description
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_snd_msg(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  BT_HDR* p_msg;

  /* if not congested */
  if (!p_ccb->cong) {
    /* are we sending a fragmented message? continue sending fragment */
    if (p_ccb->p_curr_msg != NULL) {
      avdt_msg_send(p_ccb, NULL);
    } else if (!fixed_queue_is_empty(p_ccb->rsp_q)) {
      /* do we have responses to send?  send them */
      while ((p_msg = (BT_HDR*)fixed_queue_try_dequeue(p_ccb->rsp_q)) != NULL) {
        if (avdt_msg_send(p_ccb, p_msg)) {
          /* break out if congested */
          break;
        }
      }
    }

    /* do we have commands to send?  send next command */
    avdt_ccb_snd_cmd(p_ccb, NULL);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_set_reconn
 *
 * Description      This function is called to enable a reconnect attempt when
 *                  a channel transitions from closing to idle state.  It sets
 *                  the reconn variable to true.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_set_reconn(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) { p_ccb->reconn = true; }

/*******************************************************************************
 *
 * Function         avdt_ccb_clr_reconn
 *
 * Description      This function is called to clear the reconn variable.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_clr_reconn(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) { p_ccb->reconn = false; }

/*******************************************************************************
 *
 * Function         avdt_ccb_chk_reconn
 *
 * Description      This function is called to check if a reconnect attempt
 *                  is enabled.  If enabled, it sends an AVDT_CCB_UL_OPEN_EVT
 *                  to the CCB.  If disabled, the CCB is deallocated.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_chk_reconn(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  if (p_ccb->reconn) {
    p_ccb->reconn = false;

    /* clear out ccb */
    avdt_ccb_clear_ccb(p_ccb);

    /* clear out current command, if any */
    uint8_t err_code = AVDT_ERR_CONNECT;
    tAVDT_CCB_EVT avdt_ccb_evt;
    avdt_ccb_evt.err_code = err_code;
    avdt_ccb_cmd_fail(p_ccb, &avdt_ccb_evt);

    /* reopen the signaling channel */
    avdt_ccb_event(p_ccb, AVDT_CCB_UL_OPEN_EVT, NULL);
  } else {
    avdt_ccb_ll_closed(p_ccb, NULL);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_chk_timer
 *
 * Description      This function stops the CCB timer if the idle timer is
 *                  running.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_chk_timer(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  alarm_cancel(p_ccb->idle_ccb_timer);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_set_conn
 *
 * Description      Set CCB variables associated with AVDT_ConnectReq().
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_set_conn(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  /* save callback */
  p_ccb->p_conn_cback = p_data->connect.p_cback;
}

/*******************************************************************************
 *
 * Function         avdt_ccb_set_disconn
 *
 * Description      Set CCB variables associated with AVDT_DisconnectReq().
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_set_disconn(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  /*
  log::verbose("avdt_ccb_set_disconn:conn:x{:x}, api:x{:x}",
  p_ccb->p_conn_cback, p_data->disconnect.p_cback);
      */
  /* save callback */
  if (p_data->disconnect.p_cback) {
    p_ccb->p_conn_cback = p_data->disconnect.p_cback;
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_do_disconn
 *
 * Description      Do action associated with AVDT_DisconnectReq().
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_do_disconn(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  /* clear any pending commands */
  avdt_ccb_clear_cmds(p_ccb, NULL);

  /* close channel */
  avdt_ccb_chan_close(p_ccb, NULL);
}

/*******************************************************************************
 *
 * Function         avdt_ccb_ll_closed
 *
 * Description      Clear commands from and deallocate CCB.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_ll_closed(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* /* p_data */) {
  tAVDT_CTRL_CBACK* p_cback;
  tAVDT_CTRL avdt_ctrl;

  log::verbose("peer {}", p_ccb->peer_addr);

  /* clear any pending commands */
  avdt_ccb_clear_cmds(p_ccb, NULL);

  /* save callback pointer, bd addr */
  p_cback = p_ccb->p_conn_cback;
  if (!p_cback) {
    p_cback = avdtp_cb.p_conn_cback;
  }
  RawAddress bd_addr = p_ccb->peer_addr;
  uint8_t bta_av_scb_index = p_ccb->BtaAvScbIndex();

  /* dealloc ccb */
  avdt_ccb_dealloc(p_ccb, NULL);

  /* call callback */
  if (p_cback) {
    avdt_ctrl.hdr.err_code = 0;
    (*p_cback)(0, bd_addr, AVDT_DISCONNECT_IND_EVT, &avdt_ctrl, bta_av_scb_index);
  }
}

/*******************************************************************************
 *
 * Function         avdt_ccb_ll_opened
 *
 * Description      Call callback on open.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
void avdt_ccb_ll_opened(AvdtpCcb* p_ccb, tAVDT_CCB_EVT* p_data) {
  tAVDT_CTRL avdt_ctrl;

  log::verbose("peer {} BtaAvScbIndex={} p_ccb={}", p_ccb->peer_addr, p_ccb->BtaAvScbIndex(),
               std::format_ptr(p_ccb));
  p_ccb->ll_opened = true;

  if (!p_ccb->p_conn_cback) {
    p_ccb->p_conn_cback = avdtp_cb.p_conn_cback;
  }

  /* call callback */
  if (p_ccb->p_conn_cback) {
    avdt_ctrl.hdr.err_code = 0;
    avdt_ctrl.hdr.err_param = p_data->msg.hdr.err_param;
    (*p_ccb->p_conn_cback)(0, p_ccb->peer_addr, AVDT_CONNECT_IND_EVT, &avdt_ctrl,
                           p_ccb->BtaAvScbIndex());
  }
}
