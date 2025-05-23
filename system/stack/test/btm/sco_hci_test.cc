/*
 *
 *  Copyright 2022 The Android Open Source Project
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
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>

#include "btif/include/core_callbacks.h"
#include "btif/include/stack_manager_t.h"
#include "stack/btm/btm_sco.h"
#include "stack/include/hfp_lc3_decoder.h"
#include "stack/include/hfp_lc3_encoder.h"
#include "stack/include/hfp_msbc_decoder.h"
#include "stack/include/hfp_msbc_encoder.h"
#include "stack/test/btm/btm_test_fixtures.h"
#include "test/common/mock_functions.h"

extern bluetooth::core::CoreInterface* GetInterfaceToProfiles();

namespace {

using testing::AllOf;
using testing::Ge;
using testing::Le;
using testing::Test;

const std::vector<uint8_t> msbc_zero_packet{
        0x01, 0x08, 0xad, 0x00, 0x00, 0xc5, 0x00, 0x00, 0x00, 0x00, 0x77, 0x6d, 0xb6, 0xdd, 0xdb,
        0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7,
        0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb,
        0x6d, 0xdd, 0xb6, 0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6c, 0x00};

const std::vector<uint8_t> lc3_zero_packet{
        0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x24, 0xf9, 0x4a, 0x0d, 0x00, 0x00, 0x03};

// Maps irregular packet size to expected decode buffer size.
// See |btm_wbs_supported_pkt_size| and |btm_wbs_msbc_buffer_size|.
const std::map<size_t, size_t> irregular_packet_to_buffer_size{
        {72, 360},
        {24, 120},
};

// The encoded packet size is 60 regardless of the codec.
const int ENCODED_PACKET_SIZE = 60;

struct MsbcCodecInterface : bluetooth::core::CodecInterface {
  MsbcCodecInterface() : bluetooth::core::CodecInterface() {}

  void initialize() override {
    hfp_msbc_decoder_init();
    hfp_msbc_encoder_init();
  }

  void cleanup() override {
    hfp_msbc_decoder_cleanup();
    hfp_msbc_encoder_cleanup();
  }

  uint32_t encodePacket(int16_t* input, uint8_t* output) {
    return hfp_msbc_encode_frames(input, output);
  }

  bool decodePacket(const uint8_t* i_buf, int16_t* o_buf, size_t out_len) {
    return hfp_msbc_decoder_decode_packet(i_buf, o_buf, out_len);
  }
};

struct Lc3CodecInterface : bluetooth::core::CodecInterface {
  Lc3CodecInterface() : bluetooth::core::CodecInterface() {}

  void initialize() override {
    hfp_lc3_decoder_init();
    hfp_lc3_encoder_init();
  }

  void cleanup() override {
    hfp_lc3_decoder_cleanup();
    hfp_lc3_encoder_cleanup();
  }

  uint32_t encodePacket(int16_t* input, uint8_t* output) {
    return hfp_lc3_encode_frames(input, output);
  }

  bool decodePacket(const uint8_t* i_buf, int16_t* o_buf, size_t out_len) {
    return hfp_lc3_decoder_decode_packet(i_buf, o_buf, out_len);
  }
};

class ScoHciTest : public BtmWithMocksTest {
public:
protected:
  void SetUp() override {
    BtmWithMocksTest::SetUp();

    static auto msbc_codec = MsbcCodecInterface{};
    static auto lc3_codec = Lc3CodecInterface{};
    GetInterfaceToProfiles()->msbcCodec = &msbc_codec;
    GetInterfaceToProfiles()->lc3Codec = &lc3_codec;
  }
  void TearDown() override { BtmWithMocksTest::TearDown(); }
};

class ScoHciWithOpenCleanTest : public ScoHciTest {
public:
protected:
  void SetUp() override {
    ScoHciTest::SetUp();
    bluetooth::audio::sco::open();
  }
  void TearDown() override { bluetooth::audio::sco::cleanup(); }
};

class ScoHciWbsTest : public ScoHciTest {};
class ScoHciSwbTest : public ScoHciTest {};

class ScoHciWbsWithInitCleanTest : public ScoHciTest {
public:
protected:
  void SetUp() override {
    ScoHciTest::SetUp();
    bluetooth::audio::sco::wbs::init(60);
  }
  void TearDown() override { bluetooth::audio::sco::wbs::cleanup(); }
};

class ScoHciSwbWithInitCleanTest : public ScoHciTest {
public:
protected:
  void SetUp() override {
    ScoHciTest::SetUp();
    bluetooth::audio::sco::swb::init(60);
  }
  void TearDown() override { bluetooth::audio::sco::swb::cleanup(); }
};

TEST_F(ScoHciWbsTest, WbsInit) {
  ASSERT_EQ(bluetooth::audio::sco::wbs::init(60), size_t(60));
  ASSERT_EQ(bluetooth::audio::sco::wbs::init(72), size_t(72));
  // Fallback to 60 if the packet size is not supported
  ASSERT_EQ(bluetooth::audio::sco::wbs::init(48), size_t(60));
  bluetooth::audio::sco::wbs::cleanup();
}

TEST_F(ScoHciSwbTest, SwbInit) {
  ASSERT_EQ(bluetooth::audio::sco::swb::init(60), size_t(60));
  ASSERT_EQ(bluetooth::audio::sco::swb::init(72), size_t(72));
  // Fallback to 60 if the packet size is not supported
  ASSERT_EQ(bluetooth::audio::sco::swb::init(48), size_t(60));
  bluetooth::audio::sco::swb::cleanup();
}

TEST_F(ScoHciWbsTest, WbsEnqueuePacketWithoutInit) {
  std::vector<uint8_t> payload{60, 0};
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(payload, false), false);
}

TEST_F(ScoHciSwbTest, SwbEnqueuePacketWithoutInit) {
  std::vector<uint8_t> payload{60, 0};
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(payload, false), false);
}

TEST_F(ScoHciWbsWithInitCleanTest, WbsEnqueuePacket) {
  std::vector<uint8_t> payload;
  for (size_t i = 0; i < 60; i++) {
    payload.push_back(0);
  }
  ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(payload, false), true);
  // Return 0 if buffer is full
  ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(payload, false), false);
}

TEST_F(ScoHciSwbWithInitCleanTest, SwbEnqueuePacket) {
  std::vector<uint8_t> payload;
  for (size_t i = 0; i < 60; i++) {
    payload.push_back(0);
  }
  ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(payload, false), true);
  // Return 0 if buffer is full
  ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(payload, false), false);
}

TEST_F(ScoHciWbsTest, WbsDecodeWithoutInit) {
  const uint8_t* decoded = nullptr;
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(0));
  ASSERT_EQ(decoded, nullptr);
}

TEST_F(ScoHciSwbTest, SwbDecodeWithoutInit) {
  const uint8_t* decoded = nullptr;
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(0));
  ASSERT_EQ(decoded, nullptr);
}

TEST_F(ScoHciWbsWithInitCleanTest, WbsDecode) {
  const uint8_t* decoded = nullptr;
  std::vector<uint8_t> payload;
  for (size_t i = 0; i < 60; i++) {
    payload.push_back(0);
  }

  // No data to decode
  ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(0));
  ASSERT_EQ(decoded, nullptr);
  // Fill in invalid packet, all zeros.
  ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(payload, false), true);

  // Return all zero frames when there comes an invalid packet.
  // This is expected even with PLC as there is no history in the PLC buffer.
  ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(BTM_MSBC_CODE_SIZE));
  ASSERT_NE(decoded, nullptr);
  for (size_t i = 0; i < BTM_MSBC_CODE_SIZE; i++) {
    ASSERT_EQ(decoded[i], 0);
  }

  decoded = nullptr;
  ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(msbc_zero_packet, false), true);
  ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(BTM_MSBC_CODE_SIZE));
  ASSERT_NE(decoded, nullptr);
  for (size_t i = 0; i < BTM_MSBC_CODE_SIZE; i++) {
    ASSERT_EQ(decoded[i], 0);
  }

  decoded = nullptr;
  // No remaining data to decode
  ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(0));
  ASSERT_EQ(decoded, nullptr);
}

TEST_F(ScoHciSwbWithInitCleanTest, SwbDecode) {
  const uint8_t* decoded = nullptr;
  std::vector<uint8_t> payload;
  for (size_t i = 0; i < 60; i++) {
    payload.push_back(0);
  }

  // No data to decode
  ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(0));
  ASSERT_EQ(decoded, nullptr);
  // Fill in invalid packet, all zeros.
  ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(payload, false), true);

  // Return all zero frames when there comes an invalid packet.
  // This is expected even with PLC as there is no history in the PLC buffer.
  ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(BTM_LC3_CODE_SIZE));
  ASSERT_NE(decoded, nullptr);
  for (size_t i = 0; i < BTM_LC3_CODE_SIZE; i++) {
    ASSERT_EQ(decoded[i], 0);
  }

  decoded = nullptr;
  ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(lc3_zero_packet, false), true);
  ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(BTM_LC3_CODE_SIZE));
  ASSERT_NE(decoded, nullptr);
  for (size_t i = 0; i < BTM_LC3_CODE_SIZE; i++) {
    ASSERT_EQ(decoded[i], 0);
  }

  decoded = nullptr;
  // No remaining data to decode
  ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(0));
  ASSERT_EQ(decoded, nullptr);
}

TEST_F(ScoHciWbsTest, WbsDecodeWithIrregularOffset) {
  for (auto [pkt_size, buf_size] : irregular_packet_to_buffer_size) {
    ASSERT_EQ(buf_size % pkt_size, 0u);

    bluetooth::audio::sco::wbs::init(pkt_size);

    const uint8_t* decoded = nullptr;

    // No data to decode
    ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(0));
    ASSERT_EQ(decoded, nullptr);

    // Start the payload with an irregular offset that misaligns with the
    // packet size.
    std::vector<uint8_t> payload = std::vector<uint8_t>(1, 0);
    while (payload.size() <= pkt_size) {
      payload.insert(payload.end(), msbc_zero_packet.begin(), msbc_zero_packet.end());
    }
    size_t packet_offset = msbc_zero_packet.size() - (payload.size() - pkt_size);
    payload.resize(pkt_size);

    // Try to decode as many packets as to hit the boundary.
    for (size_t iter = 0, decodable = 0; iter < 2 * buf_size / pkt_size; ++iter) {
      ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(payload, false), true);
      decodable += payload.size() - !iter;  // compensate for the first offset

      while (decodable >= ENCODED_PACKET_SIZE) {
        decoded = nullptr;
        ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(BTM_MSBC_CODE_SIZE));
        ASSERT_NE(decoded, nullptr);
        for (size_t i = 0; i < BTM_MSBC_CODE_SIZE; i++) {
          ASSERT_EQ(decoded[i], 0);
        }
        decodable -= ENCODED_PACKET_SIZE;
      }

      payload = std::vector<uint8_t>(msbc_zero_packet.begin() + packet_offset,
                                     msbc_zero_packet.end());
      while (payload.size() < pkt_size) {
        payload.insert(payload.end(), msbc_zero_packet.begin(), msbc_zero_packet.end());
      }
      packet_offset += msbc_zero_packet.size() - packet_offset;
      packet_offset +=
              msbc_zero_packet.size() - (payload.size() - pkt_size) % msbc_zero_packet.size();
      packet_offset %= msbc_zero_packet.size();
      payload.resize(pkt_size);
    }

    bluetooth::audio::sco::wbs::cleanup();
  }
}

TEST_F(ScoHciSwbTest, SwbDecodeWithIrregularOffset) {
  for (auto [pkt_size, buf_size] : irregular_packet_to_buffer_size) {
    ASSERT_EQ(buf_size % pkt_size, 0u);

    bluetooth::audio::sco::swb::init(pkt_size);

    const uint8_t* decoded = nullptr;

    // No data to decode
    ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(0));
    ASSERT_EQ(decoded, nullptr);

    // Start the payload with an irregular offset that misaligns with the
    // packet size.
    std::vector<uint8_t> payload = std::vector<uint8_t>(1, 0);
    while (payload.size() <= pkt_size) {
      payload.insert(payload.end(), lc3_zero_packet.begin(), lc3_zero_packet.end());
    }
    size_t packet_offset = lc3_zero_packet.size() - (payload.size() - pkt_size);
    payload.resize(pkt_size);

    // Try to decode as many packets as to hit the boundary.
    for (size_t iter = 0, decodable = 0; iter < 2 * buf_size / pkt_size; ++iter) {
      ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(payload, false), true);
      decodable += payload.size() - !iter;  // compensate for the first offset

      while (decodable >= ENCODED_PACKET_SIZE) {
        decoded = nullptr;
        ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(BTM_LC3_CODE_SIZE));
        ASSERT_NE(decoded, nullptr);
        for (size_t i = 0; i < BTM_LC3_CODE_SIZE; i++) {
          ASSERT_EQ(decoded[i], 0);
        }
        decodable -= ENCODED_PACKET_SIZE;
      }

      payload =
              std::vector<uint8_t>(lc3_zero_packet.begin() + packet_offset, lc3_zero_packet.end());
      while (payload.size() < pkt_size) {
        payload.insert(payload.end(), lc3_zero_packet.begin(), lc3_zero_packet.end());
      }
      packet_offset += lc3_zero_packet.size() - packet_offset;
      packet_offset +=
              lc3_zero_packet.size() - (payload.size() - pkt_size) % lc3_zero_packet.size();
      packet_offset %= lc3_zero_packet.size();
      payload.resize(pkt_size);
    }

    bluetooth::audio::sco::swb::cleanup();
  }
}

TEST_F(ScoHciWbsTest, WbsEncodeWithoutInit) {
  int16_t data[120] = {0};
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data)), size_t(0));
}

TEST_F(ScoHciSwbTest, SwbEncodeWithoutInit) {
  int16_t data[BTM_LC3_CODE_SIZE / 2] = {0};
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data)), size_t(0));
}

TEST_F(ScoHciWbsWithInitCleanTest, WbsEncode) {
  int16_t data[120] = {0};

  // Return 0 if data is invalid
  ASSERT_EQ(bluetooth::audio::sco::wbs::encode(nullptr, sizeof(data)), size_t(0));
  // Return 0 if data length is insufficient
  ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data) - 1), size_t(0));
  ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data)), sizeof(data));

  // Return 0 if the packet buffer is full
  ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data)), size_t(0));
}

TEST_F(ScoHciSwbWithInitCleanTest, SwbEncode) {
  int16_t data[BTM_LC3_CODE_SIZE / 2] = {0};

  // Return 0 if data is invalid
  ASSERT_EQ(bluetooth::audio::sco::swb::encode(nullptr, sizeof(data)), size_t(0));
  // Return 0 if data length is insufficient
  ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data) - 1), size_t(0));
  ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data)), sizeof(data));

  // Return 0 if the packet buffer is full
  ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data)), size_t(0));
}

TEST_F(ScoHciWbsTest, WbsDequeuePacketWithoutInit) {
  const uint8_t* encoded = nullptr;
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::wbs::dequeue_packet(&encoded), size_t(0));
  ASSERT_EQ(encoded, nullptr);
}

TEST_F(ScoHciSwbTest, SwbDequeuePacketWithoutInit) {
  const uint8_t* encoded = nullptr;
  // Return 0 if buffer is uninitialized
  ASSERT_EQ(bluetooth::audio::sco::swb::dequeue_packet(&encoded), size_t(0));
  ASSERT_EQ(encoded, nullptr);
}

TEST_F(ScoHciWbsWithInitCleanTest, WbsDequeuePacket) {
  const uint8_t* encoded = nullptr;
  // Return 0 if output pointer is invalid
  ASSERT_EQ(bluetooth::audio::sco::wbs::dequeue_packet(nullptr), size_t(0));
  ASSERT_EQ(encoded, nullptr);

  // Return 0 if there is insufficient data to dequeue
  ASSERT_EQ(bluetooth::audio::sco::wbs::dequeue_packet(&encoded), size_t(0));
  ASSERT_EQ(encoded, nullptr);
}

TEST_F(ScoHciSwbWithInitCleanTest, SwbDequeuePacket) {
  const uint8_t* encoded = nullptr;
  // Return 0 if output pointer is invalid
  ASSERT_EQ(bluetooth::audio::sco::swb::dequeue_packet(nullptr), size_t(0));
  ASSERT_EQ(encoded, nullptr);

  // Return 0 if there is insufficient data to dequeue
  ASSERT_EQ(bluetooth::audio::sco::swb::dequeue_packet(&encoded), size_t(0));
  ASSERT_EQ(encoded, nullptr);
}

TEST_F(ScoHciWbsWithInitCleanTest, WbsEncodeDequeuePackets) {
  uint8_t h2_header_frames_count[] = {0x08, 0x38, 0xc8, 0xf8};
  int16_t data[120] = {0};
  const uint8_t* encoded = nullptr;

  for (size_t i = 0; i < 5; i++) {
    ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::wbs::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);
    for (size_t j = 0; j < 60; j++) {
      ASSERT_EQ(encoded[j], j == 1 ? h2_header_frames_count[i % 4] : msbc_zero_packet[j]);
    }
  }
}

TEST_F(ScoHciSwbWithInitCleanTest, SwbEncodeDequeuePackets) {
  uint8_t h2_header_frames_count[] = {0x08, 0x38, 0xc8, 0xf8};
  int16_t data[BTM_LC3_CODE_SIZE / 2] = {0};
  const uint8_t* encoded = nullptr;

  for (size_t i = 0; i < 5; i++) {
    ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::swb::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);
    for (size_t j = 0; j < 60; j++) {
      ASSERT_EQ(encoded[j], j == 1 ? h2_header_frames_count[i % 4] : lc3_zero_packet[j]);
    }
  }
}

TEST_F(ScoHciWbsWithInitCleanTest, WbsPlc) {
  int16_t triangle[16] = {0, 100,  200,  300,  400,  300,  200,  100,
                          0, -100, -200, -300, -400, -300, -200, -100};
  int16_t data[120];
  int16_t expect_data[120];
  std::vector<uint8_t> encoded_vec;
  for (size_t i = 0; i < 60; i++) {
    encoded_vec.push_back(0);
  }
  const uint8_t* encoded = nullptr;
  const uint8_t* decoded = nullptr;
  size_t lost_pkt_idx = 17;

  // Simulate a run without any packet loss
  for (size_t i = 0, sample_idx = 0; i <= lost_pkt_idx; i++) {
    // Input data is a 1000Hz triangle wave
    for (size_t j = 0; j < 120; j++, sample_idx++) {
      data[j] = triangle[sample_idx % 16];
    }
    // Build the packet
    ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::wbs::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);

    // Simulate the reception of the packet
    std::copy(encoded, encoded + size_t(60), encoded_vec.data());
    ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(encoded_vec, false), true);
    ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(BTM_MSBC_CODE_SIZE));
    ASSERT_NE(decoded, nullptr);
  }
  // Store the decoded data we expect to get
  std::copy((const int16_t*)decoded, (const int16_t*)(decoded + BTM_MSBC_CODE_SIZE), expect_data);
  // Start with the fresh WBS buffer
  bluetooth::audio::sco::wbs::cleanup();
  bluetooth::audio::sco::wbs::init(60);

  // check PLC returns gracefully with invalid parameters
  ASSERT_EQ(bluetooth::audio::sco::wbs::fill_plc_stats(nullptr, nullptr), false);

  int num_decoded_frames;
  double packet_loss_ratio;
  // check PLC returns gracefully when there hasn't been decoded frames
  ASSERT_EQ(bluetooth::audio::sco::wbs::fill_plc_stats(&num_decoded_frames, &packet_loss_ratio),
            false);

  int decode_count = 0;
  for (size_t i = 0, sample_idx = 0; i <= lost_pkt_idx; i++) {
    // Data is a 1000Hz triangle wave
    for (size_t j = 0; j < 120; j++, sample_idx++) {
      data[j] = triangle[sample_idx % 16];
    }
    ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::wbs::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);

    // Substitute to invalid packet to simulate packet loss.
    std::copy(encoded, encoded + size_t(60), encoded_vec.data());
    ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(
                      i != lost_pkt_idx ? encoded_vec : std::vector<uint8_t>(60, 0), false),
              true);
    ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(BTM_MSBC_CODE_SIZE));
    decode_count++;
    ASSERT_NE(decoded, nullptr);
  }

  ASSERT_EQ(bluetooth::audio::sco::wbs::fill_plc_stats(&num_decoded_frames, &packet_loss_ratio),
            true);
  ASSERT_EQ(num_decoded_frames, decode_count);
  ASSERT_EQ(packet_loss_ratio, (double)1 / decode_count);

  int16_t* ptr = (int16_t*)decoded;
  for (size_t i = 0; i < 120; i++) {
    // The frames generated by PLC won't be perfect due to:
    // 1. mSBC decoder is statefull
    // 2. We apply overlap-add to glue the frames when packet loss happens
    ASSERT_THAT(ptr[i] - expect_data[i], AllOf(Ge(-3), Le(3)))
            << "PLC data " << ptr[i] << " deviates from expected " << expect_data[i] << " at index "
            << i;
  }

  size_t corrupted_pkt_idx = lost_pkt_idx;
  // Start with the fresh WBS buffer
  decode_count = 0;
  bluetooth::audio::sco::wbs::cleanup();
  bluetooth::audio::sco::wbs::init(60);
  for (size_t i = 0, sample_idx = 0; i <= corrupted_pkt_idx; i++) {
    // Data is a 1000Hz triangle wave
    for (size_t j = 0; j < 120; j++, sample_idx++) {
      data[j] = triangle[sample_idx % 16];
    }
    ASSERT_EQ(bluetooth::audio::sco::wbs::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::wbs::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);

    // Substitute to report packet corrupted to simulate packet loss.
    std::copy(encoded, encoded + size_t(60), encoded_vec.data());
    ASSERT_EQ(bluetooth::audio::sco::wbs::enqueue_packet(encoded_vec, i == corrupted_pkt_idx),
              true);
    ASSERT_EQ(bluetooth::audio::sco::wbs::decode(&decoded), size_t(BTM_MSBC_CODE_SIZE));
    decode_count++;
    ASSERT_NE(decoded, nullptr);
  }

  ASSERT_EQ(bluetooth::audio::sco::wbs::fill_plc_stats(&num_decoded_frames, &packet_loss_ratio),
            true);
  ASSERT_EQ(num_decoded_frames, decode_count);
  ASSERT_EQ(packet_loss_ratio, (double)1 / decode_count);

  ptr = (int16_t*)decoded;
  for (size_t i = 0; i < 120; i++) {
    // The frames generated by PLC won't be perfect due to:
    // 1. mSBC decoder is statefull
    // 2. We apply overlap-add to glue the frames when packet loss happens
    ASSERT_THAT(ptr[i] - expect_data[i], AllOf(Ge(-3), Le(3)))
            << "PLC data " << ptr[i] << " deviates from expected " << expect_data[i] << " at index "
            << i;
  }
}

// TODO(b/269970706): implement PLC validation with
// github.com/google/liblc3/issues/16 in mind.
TEST_F(ScoHciSwbWithInitCleanTest, SwbPlc) {
  int16_t triangle[16] = {0, 100,  200,  300,  400,  300,  200,  100,
                          0, -100, -200, -300, -400, -300, -200, -100};
  int16_t data[BTM_LC3_CODE_SIZE / 2];
  int16_t expect_data[BTM_LC3_CODE_SIZE / 2];
  std::vector<uint8_t> encoded_vec;
  for (size_t i = 0; i < 60; i++) {
    encoded_vec.push_back(0);
  }
  const uint8_t* encoded = nullptr;
  const uint8_t* decoded = nullptr;
  size_t lost_pkt_idx = 17;

  // Simulate a run without any packet loss
  for (size_t i = 0, sample_idx = 0; i <= lost_pkt_idx; i++) {
    // Input data is a 1000Hz triangle wave
    for (size_t j = 0; j < BTM_LC3_CODE_SIZE / 2; j++, sample_idx++) {
      data[j] = triangle[sample_idx % 16];
    }
    // Build the packet
    ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::swb::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);

    // Simulate the reception of the packet
    std::copy(encoded, encoded + size_t(60), encoded_vec.data());
    ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(encoded_vec, false), true);
    ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(BTM_LC3_CODE_SIZE));
    ASSERT_NE(decoded, nullptr);
  }
  // Store the decoded data we expect to get
  std::copy((const int16_t*)decoded, (const int16_t*)(decoded + BTM_LC3_CODE_SIZE), expect_data);
  // Start with the fresh SWB buffer
  bluetooth::audio::sco::swb::cleanup();
  bluetooth::audio::sco::swb::init(60);

  // check PLC returns gracefully with invalid parameters
  ASSERT_EQ(bluetooth::audio::sco::swb::fill_plc_stats(nullptr, nullptr), false);

  int num_decoded_frames;
  double packet_loss_ratio;
  // check PLC returns gracefully when there hasn't been decoded frames
  ASSERT_EQ(bluetooth::audio::sco::swb::fill_plc_stats(&num_decoded_frames, &packet_loss_ratio),
            false);

  int decode_count = 0;
  for (size_t i = 0, sample_idx = 0; i <= lost_pkt_idx; i++) {
    // Data is a 1000Hz triangle wave
    for (size_t j = 0; j < BTM_LC3_CODE_SIZE / 2; j++, sample_idx++) {
      data[j] = triangle[sample_idx % 16];
    }
    ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::swb::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);

    // Substitute to invalid packet to simulate packet loss.
    std::copy(encoded, encoded + size_t(60), encoded_vec.data());
    ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(
                      i != lost_pkt_idx ? encoded_vec : std::vector<uint8_t>(60, 0), false),
              true);
    ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(BTM_LC3_CODE_SIZE));
    decode_count++;
    ASSERT_NE(decoded, nullptr);
  }

  ASSERT_EQ(bluetooth::audio::sco::swb::fill_plc_stats(&num_decoded_frames, &packet_loss_ratio),
            true);
  ASSERT_EQ(num_decoded_frames, decode_count);
  ASSERT_EQ(packet_loss_ratio, (double)1 / decode_count);

  size_t corrupted_pkt_idx = lost_pkt_idx;
  // Start with the fresh SWB buffer
  decode_count = 0;
  bluetooth::audio::sco::swb::cleanup();
  bluetooth::audio::sco::swb::init(60);
  for (size_t i = 0, sample_idx = 0; i <= corrupted_pkt_idx; i++) {
    // Data is a 1000Hz triangle wave
    for (size_t j = 0; j < BTM_LC3_CODE_SIZE / 2; j++, sample_idx++) {
      data[j] = triangle[sample_idx % 16];
    }
    ASSERT_EQ(bluetooth::audio::sco::swb::encode(data, sizeof(data)), sizeof(data));
    ASSERT_EQ(bluetooth::audio::sco::swb::dequeue_packet(&encoded), size_t(60));
    ASSERT_NE(encoded, nullptr);

    // Substitute to report packet corrupted to simulate packet loss.
    std::copy(encoded, encoded + size_t(60), encoded_vec.data());
    ASSERT_EQ(bluetooth::audio::sco::swb::enqueue_packet(encoded_vec, i == corrupted_pkt_idx),
              true);
    ASSERT_EQ(bluetooth::audio::sco::swb::decode(&decoded), size_t(BTM_LC3_CODE_SIZE));
    decode_count++;
    ASSERT_NE(decoded, nullptr);
  }

  ASSERT_EQ(bluetooth::audio::sco::swb::fill_plc_stats(&num_decoded_frames, &packet_loss_ratio),
            true);
  ASSERT_EQ(num_decoded_frames, decode_count);
  ASSERT_EQ(packet_loss_ratio, (double)1 / decode_count);
}
}  // namespace
