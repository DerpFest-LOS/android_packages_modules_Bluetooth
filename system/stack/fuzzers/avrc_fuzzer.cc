/*
 * Copyright 2022 The Android Open Source Project
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

#include <base/functional/bind.h>
#include <bluetooth/log.h>
#include <fuzzer/FuzzedDataProvider.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "osi/include/allocator.h"
#include "stack/include/avct_api.h"
#include "stack/include/avrc_api.h"
#include "stack/include/bt_psm_types.h"
#include "test/fake/fake_osi.h"
#include "test/mock/mock_btif_config.h"
#include "test/mock/mock_stack_acl.h"
#include "test/mock/mock_stack_btm_dev.h"
#include "test/mock/mock_stack_l2cap_api.h"
#include "types/bluetooth/uuid.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wunused-parameter"

using bluetooth::Uuid;
using namespace bluetooth;

// Verify the passed data is readable
static void ConsumeData(const uint8_t* data, size_t size) {
  volatile uint8_t checksum = 0;
  for (size_t i = 0; i < size; i++) {
    checksum ^= data[i];
  }
}

namespace {

constexpr uint16_t kDummyCid = 0x1234;
constexpr uint8_t kDummyId = 0x77;
constexpr uint8_t kDummyRemoteAddr[] = {0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};

// Set up default callback structure
static tL2CAP_APPL_INFO avct_appl, avct_br_appl;

class FakeBtStack {
public:
  FakeBtStack() {
    test::mock::stack_l2cap_api::L2CA_DataWrite.body = [](uint16_t cid, BT_HDR* hdr) {
      log::assert_that(cid == kDummyCid, "assert failed: cid == kDummyCid");
      ConsumeData((const uint8_t*)hdr, hdr->offset + hdr->len);
      osi_free(hdr);
      return tL2CAP_DW_RESULT::SUCCESS;
    };
    test::mock::stack_l2cap_api::L2CA_DisconnectReq.body = [](uint16_t cid) {
      log::assert_that(cid == kDummyCid, "assert failed: cid == kDummyCid");
      return true;
    };
    test::mock::stack_l2cap_api::L2CA_ConnectReqWithSecurity.body =
            [](uint16_t psm, const RawAddress& p_bd_addr, uint16_t sec_level) {
              log::assert_that(p_bd_addr == kDummyRemoteAddr,
                               "assert failed: p_bd_addr == kDummyRemoteAddr");
              return kDummyCid;
            };
    test::mock::stack_l2cap_api::L2CA_RegisterWithSecurity.body =
            [](uint16_t psm, const tL2CAP_APPL_INFO& p_cb_info, bool enable_snoop,
               tL2CAP_ERTM_INFO* p_ertm_info, uint16_t my_mtu, uint16_t required_remote_mtu,
               uint16_t sec_level) {
              log::assert_that(psm == BT_PSM_AVCTP || psm == BT_PSM_AVCTP_BROWSE,
                               "assert failed: psm == BT_PSM_AVCTP || psm == BT_PSM_AVCTP_BROWSE");
              if (psm == BT_PSM_AVDTP) {
                avct_appl = p_cb_info;
              } else if (psm == BT_PSM_AVCTP_BROWSE) {
                avct_br_appl = p_cb_info;
              }
              return psm;
            };
    test::mock::stack_l2cap_api::L2CA_Deregister.body = [](uint16_t psm) {};
  }

  ~FakeBtStack() {
    test::mock::stack_l2cap_api::L2CA_DataWrite = {};
    test::mock::stack_l2cap_api::L2CA_ConnectReqWithSecurity = {};
    test::mock::stack_l2cap_api::L2CA_DisconnectReq = {};
    test::mock::stack_l2cap_api::L2CA_RegisterWithSecurity = {};
    test::mock::stack_l2cap_api::L2CA_Deregister = {};
  }
};

class Fakes {
public:
  test::fake::FakeOsi fake_osi;
  FakeBtStack fake_stack;
};

}  // namespace

#ifdef __ANDROID__
namespace android {
namespace sysprop {
namespace bluetooth {
namespace Avrcp {
std::optional<bool> absolute_volume() { return true; }
}  // namespace Avrcp

namespace Bta {
std::optional<std::int32_t> disable_delay() { return 200; }
}  // namespace Bta

namespace Pan {
std::optional<bool> nap() { return false; }
}  // namespace Pan
}  // namespace bluetooth
}  // namespace sysprop
}  // namespace android
#endif

static void ctrl_cb(uint8_t handle, uint8_t event, uint16_t result, const RawAddress* peer_addr) {}

static void msg_cb(uint8_t handle, uint8_t label, uint8_t opcode, tAVRC_MSG* p_msg) {
  uint8_t scratch_buf[512];
  tAVRC_STS status;

  if (p_msg->hdr.ctype == AVCT_CMD) {
    tAVRC_COMMAND cmd = {0};
    memset(scratch_buf, 0, sizeof(scratch_buf));
    status = AVRC_ParsCommand(p_msg, &cmd, scratch_buf, sizeof(scratch_buf));
    if (status == AVRC_STS_NO_ERROR) {
      BT_HDR* p_pkt = (BT_HDR*)nullptr;
      status = AVRC_BldCommand(&cmd, &p_pkt);
      if (status == AVRC_STS_NO_ERROR && p_pkt) {
        osi_free(p_pkt);
      }
    }
  } else if (p_msg->hdr.ctype == AVCT_RSP) {
    tAVRC_RESPONSE rsp = {0};
    memset(scratch_buf, 0, sizeof(scratch_buf));
    status = AVRC_ParsResponse(p_msg, &rsp, scratch_buf, sizeof(scratch_buf));
    if (status == AVRC_STS_NO_ERROR) {
      BT_HDR* p_pkt = (BT_HDR*)nullptr;
      status = AVRC_BldResponse(handle, &rsp, &p_pkt);
      if (status == AVRC_STS_NO_ERROR && p_pkt) {
        osi_free(p_pkt);
      }
    }

    uint16_t buf_len = sizeof(scratch_buf);
    memset(scratch_buf, 0, sizeof(scratch_buf));
    status = AVRC_Ctrl_ParsResponse(p_msg, &rsp, scratch_buf, &buf_len);
    if (status == AVRC_STS_NO_ERROR) {
      BT_HDR* p_pkt = (BT_HDR*)nullptr;
      status = AVRC_BldResponse(handle, &rsp, &p_pkt);
      if (status == AVRC_STS_NO_ERROR && p_pkt) {
        osi_free(p_pkt);
      }
    }
  }
}

static void Fuzz(const uint8_t* data, size_t size) {
  FuzzedDataProvider fdp(data, size);
  bool is_initiator = fdp.ConsumeBool();
  bool is_controller = fdp.ConsumeBool();
  bool is_br = fdp.ConsumeBool();

  AVCT_Register();
  AVRC_Init();

  tL2CAP_APPL_INFO* appl_info = is_br ? &avct_br_appl : &avct_appl;

  tAVRC_CONN_CB ccb = {
          .ctrl_cback = base::Bind(ctrl_cb),
          .msg_cback = base::Bind(msg_cb),
          .conn = (is_initiator ? AVCT_ROLE_INITIATOR : AVCT_ROLE_ACCEPTOR),
          .control = (uint8_t)(is_controller ? AVCT_CONTROL : AVCT_TARGET),
  };

  appl_info->pL2CA_ConnectInd_Cb(kDummyRemoteAddr, kDummyCid, 0, kDummyId);

  uint8_t handle;
  if (AVCT_SUCCESS != AVRC_Open(&handle, &ccb, kDummyRemoteAddr)) {
    return;
  }

  tL2CAP_CFG_INFO cfg;
  appl_info->pL2CA_ConfigCfm_Cb(kDummyCid, is_initiator, &cfg);

  // Feeding input packets
  constexpr uint16_t kMaxPacketSize = 1024;
  while (fdp.remaining_bytes() > 0) {
    auto size = fdp.ConsumeIntegralInRange<uint16_t>(0, kMaxPacketSize);
    auto bytes = fdp.ConsumeBytes<uint8_t>(size);
    BT_HDR* hdr = (BT_HDR*)osi_calloc(sizeof(BT_HDR) + bytes.size());
    hdr->len = bytes.size();
    std::copy(bytes.cbegin(), bytes.cend(), hdr->data);
    appl_info->pL2CA_DataInd_Cb(kDummyCid, hdr);
  }

  AVRC_Close(handle);

  // Simulating disconnecting event
  appl_info->pL2CA_DisconnectInd_Cb(kDummyCid, false);

  AVCT_Deregister();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
  auto fakes = std::make_unique<Fakes>();
  Fuzz(Data, Size);
  return 0;
}
