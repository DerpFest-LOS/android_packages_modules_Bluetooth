/******************************************************************************
 *
 *  Copyright 2016 The Android Open Source Project
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
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stdarg.h>

#include <string>

#include "crypto_toolbox/crypto_toolbox.h"
#include "hci/include/packet_fragmenter.h"
#include "internal_include/stack_config.h"
#include "stack/btm/btm_int_types.h"
#include "stack/include/acl_api.h"
#include "stack/include/bt_octets.h"
#include "stack/include/btm_ble_api.h"
#include "stack/include/smp_status.h"
#include "stack/smp/p_256_ecc_pp.h"
#include "stack/smp/smp_int.h"
#include "test/mock/mock_stack_acl.h"
#include "types/hci_role.h"
#include "types/raw_address.h"

// TODO(b/369381361) Enfore -Wmissing-prototypes
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

using testing::StrEq;

tBTM_CB btm_cb;

const std::string kSmpOptions("mock smp options");
const std::string kBroadcastAudioConfigOptions("mock broadcast audio config options");
bool get_pts_avrcp_test(void) { return false; }
bool get_pts_secure_only_mode(void) { return false; }
bool get_pts_conn_updates_disabled(void) { return false; }
bool get_pts_crosskey_sdp_disable(void) { return false; }
const std::string* get_pts_smp_options(void) { return &kSmpOptions; }
int get_pts_smp_failure_case(void) { return 123; }
bool get_pts_force_eatt_for_notifications(void) { return false; }
bool get_pts_connect_eatt_unconditionally(void) { return false; }
bool get_pts_connect_eatt_before_encryption(void) { return false; }
bool get_pts_unencrypt_broadcast(void) { return false; }
bool get_pts_eatt_peripheral_collision_support(void) { return false; }
bool get_pts_use_eatt_for_all_services(void) { return false; }
bool get_pts_force_le_audio_multiple_contexts_metadata(void) { return false; }
bool get_pts_l2cap_ecoc_upper_tester(void) { return false; }
int get_pts_l2cap_ecoc_min_key_size(void) { return -1; }
int get_pts_l2cap_ecoc_initial_chan_cnt(void) { return -1; }
bool get_pts_l2cap_ecoc_connect_remaining(void) { return false; }
int get_pts_l2cap_ecoc_send_num_of_sdu(void) { return -1; }
bool get_pts_l2cap_ecoc_reconfigure(void) { return false; }
const std::string* get_pts_broadcast_audio_config_options(void) {
  return &kBroadcastAudioConfigOptions;
}
bool get_pts_le_audio_disable_ases_before_stopping(void) { return false; }
config_t* get_all(void) { return nullptr; }
const packet_fragmenter_t* packet_fragmenter_get_interface() { return nullptr; }

stack_config_t mock_stack_config{
        .get_pts_avrcp_test = get_pts_avrcp_test,
        .get_pts_secure_only_mode = get_pts_secure_only_mode,
        .get_pts_conn_updates_disabled = get_pts_conn_updates_disabled,
        .get_pts_crosskey_sdp_disable = get_pts_crosskey_sdp_disable,
        .get_pts_smp_options = get_pts_smp_options,
        .get_pts_smp_failure_case = get_pts_smp_failure_case,
        .get_pts_force_eatt_for_notifications = get_pts_force_eatt_for_notifications,
        .get_pts_connect_eatt_unconditionally = get_pts_connect_eatt_unconditionally,
        .get_pts_connect_eatt_before_encryption = get_pts_connect_eatt_before_encryption,
        .get_pts_unencrypt_broadcast = get_pts_unencrypt_broadcast,
        .get_pts_eatt_peripheral_collision_support = get_pts_eatt_peripheral_collision_support,
        .get_pts_use_eatt_for_all_services = get_pts_use_eatt_for_all_services,
        .get_pts_force_le_audio_multiple_contexts_metadata =
                get_pts_force_le_audio_multiple_contexts_metadata,
        .get_pts_l2cap_ecoc_upper_tester = get_pts_l2cap_ecoc_upper_tester,
        .get_pts_l2cap_ecoc_min_key_size = get_pts_l2cap_ecoc_min_key_size,
        .get_pts_l2cap_ecoc_initial_chan_cnt = get_pts_l2cap_ecoc_initial_chan_cnt,
        .get_pts_l2cap_ecoc_connect_remaining = get_pts_l2cap_ecoc_connect_remaining,
        .get_pts_l2cap_ecoc_send_num_of_sdu = get_pts_l2cap_ecoc_send_num_of_sdu,
        .get_pts_l2cap_ecoc_reconfigure = get_pts_l2cap_ecoc_reconfigure,
        .get_pts_broadcast_audio_config_options = get_pts_broadcast_audio_config_options,
        .get_pts_le_audio_disable_ases_before_stopping =
                get_pts_le_audio_disable_ases_before_stopping,
        .get_all = get_all,
};
const stack_config_t* stack_config_get_interface(void) { return &mock_stack_config; }

/*
 * This test verifies various key distribution methods in SMP works using the
 * following parameter set:
 *
 * When testing target as Central (Initiator is local, Responder is remote)
 *
 * Initiator's Pairing Request: 0x070710000001(01)
 * Responder's Pairing Response: 0x050008000003(02)
 * Initiator's Bluetooth Address: 0xA1A2A3A4A5A6
 * Initiator's Bluetooth Address Type: 0x01
 * Responder's Bluetooth Address: 0xB1B2B3B4B5B6
 * Responder's Bluetooth Address Type: 0x00
 * Initiator's Random Number: 0x5783D52156AD6F0E6388274EC6702EE0
 * TK Encryption Key: 0x0
 *
 * Correct values:
 *
 * p1: 0x05000800000302070710000001010001
 * p1 XOR r: 0x5283dd2156ae6d096498274ec7712ee1
 * p1 prime: 0x02c7aa2a9857ac866ff91232df0e3c95
 * p2: 0x00000000a1a2a3a4a5a6b1b2b3b4b5b6
 * MConfirm (c1): 0x1e1e3fef878988ead2a74dc5bef13b86
 *
 * NOTE: All these values are presented in mathematical reasonable canonical
 * form that has MSB on the left and LSB on the right. In Bluetooth packets,
 * they are mostly reversed to be Little Endian which have LSB on the left and
 * MSB on the right.
 */

Octet16 smp_gen_p1_4_confirm(tSMP_CB* p_cb, tBLE_ADDR_TYPE remote_bd_addr_type);

Octet16 smp_gen_p2_4_confirm(tSMP_CB* p_cb, const RawAddress& remote_bda);

tSMP_STATUS smp_calculate_confirm(tSMP_CB* p_cb, const Octet16& rand, Octet16* output);

void dump_uint128(const Octet16& a, char* buffer) {
  for (unsigned int i = 0; i < OCTET16_LEN; ++i) {
    snprintf(buffer, 3, "%02x", a[i]);
    buffer += 2;
  }
  *buffer = '\0';
}

void dump_uint128_reverse(const Octet16& a, char* buffer) {
  for (int i = (int)(OCTET16_LEN - 1); i >= 0; --i) {
    snprintf(buffer, 3, "%02x", a[i]);
    buffer += 2;
  }
  *buffer = '\0';
}

void print_uint128(const Octet16& a) {
  for (unsigned int i = 0; i < OCTET16_LEN; ++i) {
    printf("%02x", a[i]);
  }
  printf("\n");
}

Octet16 parse_uint128(const char* input) {
  Octet16 output{0};
  for (unsigned int count = 0; count < OCTET16_LEN; count++) {
    sscanf(input, "%2hhx", &output[count]);
    input += 2;
  }
  return output;
}

class SmpCalculateConfirmTest : public testing::Test {
protected:
  tSMP_CB p_cb_;
  // Set random to 0x5783D52156AD6F0E6388274EC6702EE0
  Octet16 rand_{0x57, 0x83, 0xD5, 0x21, 0x56, 0xAD, 0x6F, 0x0E,
                0x63, 0x88, 0x27, 0x4E, 0xC6, 0x70, 0x2E, 0xE0};

  void SetUp() override {
    p_cb_.tk = {0};
    // Set pairing request packet to 0x070710000001(01)
    p_cb_.local_io_capability = 0x01;
    p_cb_.loc_oob_flag = 0x00;
    p_cb_.loc_auth_req = 0x00;
    p_cb_.loc_enc_size = 0x10;
    p_cb_.local_i_key = 0x07;
    p_cb_.local_r_key = 0x07;
    // Set pairing response packet to 0x050008000003(02)
    p_cb_.peer_io_caps = 0x03;
    p_cb_.peer_oob_flag = 0x00;
    p_cb_.peer_auth_req = 0x00;
    p_cb_.peer_enc_size = 0x08;
    p_cb_.peer_i_key = 0x00;
    p_cb_.peer_r_key = 0x05;
    // Set role to central
    p_cb_.role = HCI_ROLE_CENTRAL;
    std::reverse(rand_.begin(), rand_.end());
  }
  void TearDown() override {}

public:
};

// Test smp_gen_p2_4_confirm function implementation
TEST_F(SmpCalculateConfirmTest, test_smp_gen_p2_4_confirm_as_central) {
  // Set local_bda to 0xA1A2A3A4A5A6
  test::mock::stack_acl::BTM_ReadConnectionAddr.body =
          [](const RawAddress& /*remote_bda*/, RawAddress& local_conn_addr,
             tBLE_ADDR_TYPE* p_addr_type, bool /*ota_address*/) {
            local_conn_addr = RawAddress({0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6});
            *p_addr_type = BLE_ADDR_RANDOM;
          };

  // Set remote bda to 0xB1B2B3B4B5B6
  test::mock::stack_acl::BTM_ReadRemoteConnectionAddr.body =
          [](const RawAddress& /*pseudo_addr*/, RawAddress& conn_addr, tBLE_ADDR_TYPE* p_addr_type,
             bool /*ota_address*/) {
            conn_addr = RawAddress({0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6});
            *p_addr_type = BLE_ADDR_PUBLIC;
            return true;
          };

  RawAddress remote_bda;
  tBLE_ADDR_TYPE remote_bd_addr_type = BLE_ADDR_PUBLIC;
  BTM_ReadRemoteConnectionAddr(p_cb_.pairing_bda, remote_bda, &remote_bd_addr_type, true);
  BTM_ReadConnectionAddr(p_cb_.pairing_bda, p_cb_.local_bda, &p_cb_.addr_type, true);
  Octet16 p2 = smp_gen_p2_4_confirm(&p_cb_, remote_bda);
  // Correct p2 is 0x00000000a1a2a3a4a5a6b1b2b3b4b5b6
  const char expected_p2_str[] = "00000000a1a2a3a4a5a6b1b2b3b4b5b6";
  char p2_str[2 * OCTET16_LEN + 1];
  dump_uint128_reverse(p2, p2_str);
  ASSERT_THAT(p2_str, StrEq(expected_p2_str));

  test::mock::stack_acl::BTM_ReadConnectionAddr = {};
  test::mock::stack_acl::BTM_ReadRemoteConnectionAddr = {};
}

// Test smp_gen_p1_4_confirm and aes_128 function implementation
TEST_F(SmpCalculateConfirmTest, test_aes_128_as_central) {
  // Set local_bda to 0xA1A2A3A4A5A6
  test::mock::stack_acl::BTM_ReadConnectionAddr.body =
          [](const RawAddress& /*remote_bda*/, RawAddress& local_conn_addr,
             tBLE_ADDR_TYPE* p_addr_type, bool /*ota_address*/) {
            local_conn_addr = RawAddress({0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6});
            *p_addr_type = BLE_ADDR_RANDOM;
          };

  // Set remote bda to 0xB1B2B3B4B5B6
  test::mock::stack_acl::BTM_ReadRemoteConnectionAddr.body =
          [](const RawAddress& /*pseudo_addr*/, RawAddress& conn_addr, tBLE_ADDR_TYPE* p_addr_type,
             bool /*ota_address*/) {
            conn_addr = RawAddress({0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6});
            *p_addr_type = BLE_ADDR_PUBLIC;
            return true;
          };

  RawAddress remote_bda;
  tBLE_ADDR_TYPE remote_bd_addr_type = BLE_ADDR_PUBLIC;
  BTM_ReadRemoteConnectionAddr(p_cb_.pairing_bda, remote_bda, &remote_bd_addr_type, true);
  BTM_ReadConnectionAddr(p_cb_.pairing_bda, p_cb_.local_bda, &p_cb_.addr_type, true);
  Octet16 p1 = smp_gen_p1_4_confirm(&p_cb_, remote_bd_addr_type);
  // Correct p1 is 0x05000800000302070710000001010001
  const char expected_p1_str[] = "05000800000302070710000001010001";
  char p1_str[2 * OCTET16_LEN + 1];
  dump_uint128_reverse(p1, p1_str);
  ASSERT_THAT(p1_str, StrEq(expected_p1_str));
  smp_xor_128(&p1, rand_);
  // Correct p1 xor r is 0x5283dd2156ae6d096498274ec7712ee1
  const char expected_p1_xor_r_str[] = "5283dd2156ae6d096498274ec7712ee1";
  char p1_xor_r_str[2 * OCTET16_LEN + 1];
  dump_uint128_reverse(p1, p1_xor_r_str);
  ASSERT_THAT(p1_xor_r_str, StrEq(expected_p1_xor_r_str));
  Octet16 output = crypto_toolbox::aes_128(p_cb_.tk, p1);
  const char expected_p1_prime_str[] = "02c7aa2a9857ac866ff91232df0e3c95";
  char p1_prime_str[2 * OCTET16_LEN + 1];
  dump_uint128_reverse(output, p1_prime_str);
  ASSERT_THAT(p1_prime_str, StrEq(expected_p1_prime_str));

  test::mock::stack_acl::BTM_ReadConnectionAddr = {};
  test::mock::stack_acl::BTM_ReadRemoteConnectionAddr = {};
}

// Test smp_calculate_confirm function implementation
TEST_F(SmpCalculateConfirmTest, test_smp_calculate_confirm_as_central) {
  // Set local_bda to 0xA1A2A3A4A5A6
  test::mock::stack_acl::BTM_ReadConnectionAddr.body =
          [](const RawAddress& /*remote_bda*/, RawAddress& local_conn_addr,
             tBLE_ADDR_TYPE* p_addr_type, bool /*ota_address*/) {
            local_conn_addr = RawAddress({0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6});
            *p_addr_type = BLE_ADDR_RANDOM;
          };

  // Set remote bda to 0xB1B2B3B4B5B6
  test::mock::stack_acl::BTM_ReadRemoteConnectionAddr.body =
          [](const RawAddress& /*pseudo_addr*/, RawAddress& conn_addr, tBLE_ADDR_TYPE* p_addr_type,
             bool /*ota_address*/) {
            conn_addr = RawAddress({0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6});
            *p_addr_type = BLE_ADDR_PUBLIC;
            return true;
          };

  Octet16 output;
  tSMP_STATUS status = smp_calculate_confirm(&p_cb_, rand_, &output);
  EXPECT_EQ(status, SMP_SUCCESS);
  // Correct MConfirm is 0x1e1e3fef878988ead2a74dc5bef13b86
  const char expected_confirm_str[] = "1e1e3fef878988ead2a74dc5bef13b86";
  char confirm_str[2 * OCTET16_LEN + 1];
  dump_uint128_reverse(output, confirm_str);
  ASSERT_THAT(confirm_str, StrEq(expected_confirm_str));

  test::mock::stack_acl::BTM_ReadConnectionAddr = {};
  test::mock::stack_acl::BTM_ReadRemoteConnectionAddr = {};
}

// Test ECC point validation
TEST(SmpEccValidationTest, test_valid_points) {
  Point p;

  // Test data from Bluetooth Core Specification
  // Version 5.0 | Vol 2, Part G | 7.1.2

  // Sample 1
  p.x[7] = 0x20b003d2;
  p.x[6] = 0xf297be2c;
  p.x[5] = 0x5e2c83a7;
  p.x[4] = 0xe9f9a5b9;
  p.x[3] = 0xeff49111;
  p.x[2] = 0xacf4fddb;
  p.x[1] = 0xcc030148;
  p.x[0] = 0x0e359de6;

  p.y[7] = 0xdc809c49;
  p.y[6] = 0x652aeb6d;
  p.y[5] = 0x63329abf;
  p.y[4] = 0x5a52155c;
  p.y[3] = 0x766345c2;
  p.y[2] = 0x8fed3024;
  p.y[1] = 0x741c8ed0;
  p.y[0] = 0x1589d28b;

  EXPECT_TRUE(ECC_ValidatePoint(p));

  // Sample 2
  p.x[7] = 0x2c31a47b;
  p.x[6] = 0x5779809e;
  p.x[5] = 0xf44cb5ea;
  p.x[4] = 0xaf5c3e43;
  p.x[3] = 0xd5f8faad;
  p.x[2] = 0x4a8794cb;
  p.x[1] = 0x987e9b03;
  p.x[0] = 0x745c78dd;

  p.y[7] = 0x91951218;
  p.y[6] = 0x3898dfbe;
  p.y[5] = 0xcd52e240;
  p.y[4] = 0x8e43871f;
  p.y[3] = 0xd0211091;
  p.y[2] = 0x17bd3ed4;
  p.y[1] = 0xeaf84377;
  p.y[0] = 0x43715d4f;

  EXPECT_TRUE(ECC_ValidatePoint(p));
}

TEST(SmpEccValidationTest, test_invalid_points) {
  Point p;
  multiprecision_init(p.x);
  multiprecision_init(p.y);

  EXPECT_FALSE(ECC_ValidatePoint(p));

  // Sample 1
  p.x[7] = 0x20b003d2;
  p.x[6] = 0xf297be2c;
  p.x[5] = 0x5e2c83a7;
  p.x[4] = 0xe9f9a5b9;
  p.x[3] = 0xeff49111;
  p.x[2] = 0xacf4fddb;
  p.x[1] = 0xcc030148;
  p.x[0] = 0x0e359de6;

  EXPECT_FALSE(ECC_ValidatePoint(p));

  p.y[7] = 0xdc809c49;
  p.y[6] = 0x652aeb6d;
  p.y[5] = 0x63329abf;
  p.y[4] = 0x5a52155c;
  p.y[3] = 0x766345c2;
  p.y[2] = 0x8fed3024;
  p.y[1] = 0x741c8ed0;
  p.y[0] = 0x1589d28b;

  p.y[0]--;

  EXPECT_FALSE(ECC_ValidatePoint(p));
}

TEST(SmpStatusText, smp_status_text) {
  std::vector<std::pair<tSMP_STATUS, std::string>> status = {
          std::make_pair(SMP_SUCCESS, "SMP_SUCCESS"),
          std::make_pair(SMP_PASSKEY_ENTRY_FAIL, "SMP_PASSKEY_ENTRY_FAIL"),
          std::make_pair(SMP_OOB_FAIL, "SMP_OOB_FAIL"),
          std::make_pair(SMP_PAIR_AUTH_FAIL, "SMP_PAIR_AUTH_FAIL"),
          std::make_pair(SMP_CONFIRM_VALUE_ERR, "SMP_CONFIRM_VALUE_ERR"),
          std::make_pair(SMP_PAIR_NOT_SUPPORT, "SMP_PAIR_NOT_SUPPORT"),
          std::make_pair(SMP_ENC_KEY_SIZE, "SMP_ENC_KEY_SIZE"),
          std::make_pair(SMP_INVALID_CMD, "SMP_INVALID_CMD"),
          std::make_pair(SMP_PAIR_FAIL_UNKNOWN, "SMP_PAIR_FAIL_UNKNOWN"),
          std::make_pair(SMP_REPEATED_ATTEMPTS, "SMP_REPEATED_ATTEMPTS"),
          std::make_pair(SMP_INVALID_PARAMETERS, "SMP_INVALID_PARAMETERS"),
          std::make_pair(SMP_DHKEY_CHK_FAIL, "SMP_DHKEY_CHK_FAIL"),
          std::make_pair(SMP_NUMERIC_COMPAR_FAIL, "SMP_NUMERIC_COMPAR_FAIL"),
          std::make_pair(SMP_BR_PARING_IN_PROGR, "SMP_BR_PARING_IN_PROGR"),
          std::make_pair(SMP_XTRANS_DERIVE_NOT_ALLOW, "SMP_XTRANS_DERIVE_NOT_ALLOW"),
          std::make_pair(SMP_MAX_FAIL_RSN_PER_SPEC,
                         "SMP_XTRANS_DERIVE_NOT_ALLOW"),  // NOTE: Dup
          std::make_pair(SMP_PAIR_INTERNAL_ERR, "SMP_PAIR_INTERNAL_ERR"),
          std::make_pair(SMP_UNKNOWN_IO_CAP, "SMP_UNKNOWN_IO_CAP"),
          std::make_pair(SMP_BUSY, "SMP_BUSY"),
          std::make_pair(SMP_ENC_FAIL, "SMP_ENC_FAIL"),
          std::make_pair(SMP_STARTED, "SMP_STARTED"),
          std::make_pair(SMP_RSP_TIMEOUT, "SMP_RSP_TIMEOUT"),
          std::make_pair(SMP_FAIL, "SMP_FAIL"),
          std::make_pair(SMP_CONN_TOUT, "SMP_CONN_TOUT"),
  };
  for (const auto& stat : status) {
    ASSERT_STREQ(stat.second.c_str(), smp_status_text(stat.first).c_str());
  }
  auto unknown = base::StringPrintf("UNKNOWN[%hhu]", std::numeric_limits<uint8_t>::max());
  ASSERT_STREQ(
          unknown.c_str(),
          smp_status_text(static_cast<tSMP_STATUS>(std::numeric_limits<uint8_t>::max())).c_str());
}
