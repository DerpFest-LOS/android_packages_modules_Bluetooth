/*
 * Copyright 2020 HIMSA II K/S - www.himsa.com.
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

#include "le_audio_types.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

#include "le_audio_utils.h"

namespace bluetooth::le_audio {
namespace types {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Pair;
using ::testing::SizeIs;

TEST(LeAudioLtvMapTest, test_serialization) {
  // clang-format off
  const std::vector<uint8_t> ltv_test_vec{
      0x02, 0x01, 0x0a,
      0x03, 0x02, 0xaa, 0xbb,
      0x04, 0x03, 0xde, 0xc0, 0xd0,
  };

  const std::vector<uint8_t> ltv_test_vec2{
      0x04, 0x03, 0xde, 0xc0, 0xde,
      0x05, 0x04, 0xc0, 0xde, 0xc0, 0xde,
  };

  const std::vector<uint8_t> ltv_test_vec_expected{
      0x02, 0x01, 0x0a,
      0x03, 0x02, 0xaa, 0xbb,
      0x04, 0x03, 0xde, 0xc0, 0xde,
      0x05, 0x04, 0xc0, 0xde, 0xc0, 0xde,
  };
  // clang-format on

  // Parse
  bool success;
  LeAudioLtvMap ltv_map = LeAudioLtvMap::Parse(ltv_test_vec.data(), ltv_test_vec.size(), success);
  auto hash_one = ltv_map.GetHash();
  ASSERT_TRUE(success);
  ASSERT_NE(hash_one, 0lu);
  ASSERT_FALSE(ltv_map.IsEmpty());
  ASSERT_EQ((size_t)3, ltv_map.Size());

  ASSERT_TRUE(ltv_map.Find(0x03));
  ASSERT_THAT(*(ltv_map.Find(0x03)), ElementsAre(0xde, 0xc0, 0xd0));

  LeAudioLtvMap ltv_map2 =
          LeAudioLtvMap::Parse(ltv_test_vec2.data(), ltv_test_vec2.size(), success);
  auto hash_two = ltv_map2.GetHash();
  ASSERT_TRUE(success);
  ASSERT_NE(hash_two, 0lu);
  ASSERT_FALSE(ltv_map2.IsEmpty());
  ASSERT_EQ((size_t)2, ltv_map2.Size());
  ASSERT_NE(hash_one, hash_two);

  ltv_map.Append(ltv_map2);
  ASSERT_NE(ltv_map.GetHash(), 0lu);
  ASSERT_NE(ltv_map.GetHash(), hash_one);
  ASSERT_NE(ltv_map.GetHash(), hash_two);
  ASSERT_EQ((size_t)4, ltv_map.Size());

  ASSERT_TRUE(ltv_map.Find(0x01));
  ASSERT_THAT(*(ltv_map.Find(0x01)), ElementsAre(0x0a));
  ASSERT_TRUE(ltv_map.Find(0x02));
  ASSERT_THAT(*(ltv_map.Find(0x02)), ElementsAre(0xaa, 0xbb));
  ASSERT_TRUE(ltv_map.Find(0x03));
  ASSERT_THAT(*(ltv_map.Find(0x03)), ElementsAre(0xde, 0xc0, 0xde));
  ASSERT_TRUE(ltv_map.Find(0x04));
  ASSERT_THAT(*(ltv_map.Find(0x04)), ElementsAre(0xc0, 0xde, 0xc0, 0xde));

  // RawPacket
  std::vector<uint8_t> serialized(ltv_map.RawPacketSize());
  ASSERT_TRUE(ltv_map.RawPacket(serialized.data()));
  ASSERT_THAT(serialized, ElementsAreArray(ltv_test_vec_expected));
  ASSERT_THAT(ltv_map2.RawPacket(), ElementsAreArray(ltv_test_vec2));
}

TEST(LeAudioLtvMapTest, test_serialization_macros) {
  uint64_t value = 0x08090A0B0C0D0E0F;

  auto u16vec = UINT16_TO_VEC_UINT8(value);
  ASSERT_EQ(sizeof(uint16_t), u16vec.size());
  ASSERT_EQ(0x0F, u16vec[0]);
  ASSERT_EQ(0x0E, u16vec[1]);

  auto u32vec = UINT32_TO_VEC_UINT8(value);
  ASSERT_EQ(sizeof(uint32_t), u32vec.size());
  ASSERT_EQ(0x0F, u32vec[0]);
  ASSERT_EQ(0x0E, u32vec[1]);
  ASSERT_EQ(0x0D, u32vec[2]);
  ASSERT_EQ(0x0C, u32vec[3]);
}

TEST(LeAudioLtvMapTest, test_serialization_ltv_len_is_zero) {
  // clang-format off
  const std::vector<uint8_t> ltv_test_vec{
      0x02, 0x01, 0x0a,
      0x03, 0x02, 0xaa, 0xbb,
      0x00, 0x00, 0x00, 0x00, 0x00,       // ltv_len == 0
      0x05, 0x04, 0xc0, 0xde, 0xc0, 0xde,
  };
  // clang-format on

  // Parse
  bool success;
  LeAudioLtvMap ltv_map = LeAudioLtvMap::Parse(ltv_test_vec.data(), ltv_test_vec.size(), success);
  ASSERT_TRUE(success);
  ASSERT_FALSE(ltv_map.IsEmpty());
  ASSERT_EQ((size_t)3, ltv_map.Size());

  ASSERT_TRUE(ltv_map.Find(0x01));
  ASSERT_THAT(*(ltv_map.Find(0x01)), ElementsAre(0x0a));
  ASSERT_TRUE(ltv_map.Find(0x02));
  ASSERT_THAT(*(ltv_map.Find(0x02)), ElementsAre(0xaa, 0xbb));
  ASSERT_TRUE(ltv_map.Find(0x04));
  ASSERT_THAT(*(ltv_map.Find(0x04)), ElementsAre(0xc0, 0xde, 0xc0, 0xde));

  // RawPacket
  std::vector<uint8_t> serialized(ltv_map.RawPacketSize());
  ASSERT_TRUE(ltv_map.RawPacket(serialized.data()));
  ASSERT_THAT(serialized, ElementsAre(0x02, 0x01, 0x0a, 0x03, 0x02, 0xaa, 0xbb, 0x05, 0x04, 0xc0,
                                      0xde, 0xc0, 0xde));
}

TEST(LeAudioLtvMapTest, test_serialization_ltv_len_is_one) {
  // clang-format off
  const std::vector<uint8_t> ltv_test_vec{
    0x02, 0x01, 0x0a,
    0x01, 0x02,
  };
  // clang-format on

  // Parse
  bool success;
  LeAudioLtvMap ltv_map = LeAudioLtvMap::Parse(ltv_test_vec.data(), ltv_test_vec.size(), success);
  ASSERT_TRUE(success);
  ASSERT_FALSE(ltv_map.IsEmpty());
  ASSERT_EQ((size_t)2, ltv_map.Size());

  ASSERT_TRUE(ltv_map.Find(0x01));
  ASSERT_THAT(*(ltv_map.Find(0x01)), ElementsAre(0x0a));
  ASSERT_TRUE(ltv_map.Find(0x02));
  ASSERT_THAT(*(ltv_map.Find(0x02)), SizeIs(0));

  // RawPacket
  std::vector<uint8_t> serialized(ltv_map.RawPacketSize());
  ASSERT_TRUE(ltv_map.RawPacket(serialized.data()));
  ASSERT_THAT(serialized, ElementsAreArray(ltv_test_vec));
}

TEST(LeAudioLtvMapTest, test_serialization_ltv_len_is_invalid) {
  // clang-format off
  const std::vector<uint8_t> ltv_test_vec_1{
      0x02, 0x01, 0x0a,
      0x04, 0x02, 0xaa, 0xbb,  // one byte missing
  };
  const std::vector<uint8_t> ltv_test_vec_2{
      0x02, 0x01, 0x0a,
      0x03, 0x02, 0xaa, 0xbb,
      0x01,
  };
  const std::vector<uint8_t> ltv_test_vec_3{
      0x02, 0x01, 0x0a,
      0x03, 0x02, 0xaa, 0xbb,
      0x02, 0x03,
  };
  // clang-format on

  // Parse
  bool success = true;
  LeAudioLtvMap ltv_map;

  ltv_map = LeAudioLtvMap::Parse(ltv_test_vec_1.data(), ltv_test_vec_1.size(), success);
  ASSERT_FALSE(success);

  ltv_map = LeAudioLtvMap::Parse(ltv_test_vec_2.data(), ltv_test_vec_2.size(), success);
  ASSERT_FALSE(success);

  ltv_map = LeAudioLtvMap::Parse(ltv_test_vec_3.data(), ltv_test_vec_3.size(), success);
  ASSERT_FALSE(success);
}

TEST(LeAudioLtvMapTest, test_configuration_valid) {
  // clang-format off
  const std::vector<uint8_t> config_ltv_vec{
      // SamplingFreq = 48000
      0x02, 0x01, 0x08,
      // FrameDuration = 10000us
      0x02, 0x02, 0x01,
      // AudioChannelAllocation = kLeAudioLocationFrontLeft | kLeAudioLocationFrontRight
      0x05, 0x03, 0x03, 0x00, 0x00, 0x00,
      // OctetsPerCodecFrame = 40
      0x03, 0x04, 40, 0x00,
      // Unknown type entry to ignore
      0x05, 0x06, 0x11, 0x22, 0x33, 0x44,
      // CodecFrameBlocksPerSdu = 1
      0x02, 0x05, 1,
  };
  // clang-format on

  // Parse
  bool success = true;
  LeAudioLtvMap ltv_map =
          LeAudioLtvMap::Parse(config_ltv_vec.data(), config_ltv_vec.size(), success);
  ASSERT_TRUE(success);

  // Verify the codec configuration values
  auto config = ltv_map.GetAsCoreCodecConfig();

  // SamplingFreq = 48000
  ASSERT_TRUE(config.sampling_frequency.has_value());
  ASSERT_EQ(0x08, config.sampling_frequency.value());
  ASSERT_EQ(48000u, config.GetSamplingFrequencyHz());

  // FrameDuration = 10000us
  ASSERT_TRUE(config.frame_duration.has_value());
  ASSERT_EQ(0x01, config.frame_duration.value());
  ASSERT_EQ(10000u, config.GetFrameDurationUs());

  // AudioChannelAllocation = kLeAudioLocationFrontLeft | kLeAudioLocationFrontRight
  ASSERT_TRUE(config.audio_channel_allocation.has_value());
  ASSERT_EQ(0x00000003u, config.audio_channel_allocation.value());

  // OctetsPerCodecFrame = 40
  ASSERT_TRUE(config.octets_per_codec_frame.has_value());
  ASSERT_EQ(0x0028u, config.octets_per_codec_frame.value());

  // CodecFrameBlocksPerSdu = 1
  ASSERT_TRUE(config.codec_frames_blocks_per_sdu.has_value());
  ASSERT_EQ(0x01u, config.codec_frames_blocks_per_sdu.value());
}

TEST(LeAudioLtvMapTest, test_capabilities_valid) {
  // clang-format off
  const std::vector<uint8_t> capabilities_ltv_vec{
      // SupportedSamplingFrequencies = 96000 and 16000
      0x03, 0x01,
          (uint8_t)(codec_spec_caps::kLeAudioSamplingFreq16000Hz) |
              (uint8_t)(codec_spec_caps::kLeAudioSamplingFreq96000Hz),
          (uint8_t)(codec_spec_caps::kLeAudioSamplingFreq16000Hz >> 8) |
              (uint8_t)(codec_spec_caps::kLeAudioSamplingFreq96000Hz >> 8),
      // SupportedFrameDurations = 10ms, 7.5ms, 10ms preferred
      0x02, 0x02, codec_spec_caps::kLeAudioCodecFrameDur7500us |
                      codec_spec_caps::kLeAudioCodecFrameDur10000us |
                      codec_spec_caps::kLeAudioCodecFrameDurPrefer10000us,
      // SupportedAudioChannelCounts = 0b1 | 0b2 (one and two channels)
      0x02, 0x03, 0b01 | 0b10,
      // SupportedOctetsPerCodecFrame = min:40, max:80
      0x05, 0x04, 40, 00, 80, 00,
      // Unknown type entry to ignore
      0x05, 0x06, 0x11, 0x22, 0x33, 0x44,
      // SupportedMaxCodecFramesPerSdu = 2
      0x02, 0x05, 0x02,
  };
  // clang-format on

  // Parse
  bool success = true;
  LeAudioLtvMap ltv_map =
          LeAudioLtvMap::Parse(capabilities_ltv_vec.data(), capabilities_ltv_vec.size(), success);
  ASSERT_TRUE(success);

  // Verify the codec capabilities values
  auto caps = ltv_map.GetAsCoreCodecCapabilities();

  // SupportedSamplingFrequencies = 96000 and 16000
  ASSERT_TRUE(caps.HasSupportedSamplingFrequencies());
  ASSERT_EQ(codec_spec_caps::kLeAudioSamplingFreq16000Hz |
                    codec_spec_caps::kLeAudioSamplingFreq96000Hz,
            caps.supported_sampling_frequencies.value());
  // Check config values against the capabilities
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq8000Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq11025Hz));
  ASSERT_TRUE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq16000Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq22050Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq24000Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq32000Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq44100Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq48000Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq88200Hz));
  ASSERT_TRUE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq96000Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq176400Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq192000Hz));
  ASSERT_FALSE(
          caps.IsSamplingFrequencyConfigSupported(codec_spec_conf::kLeAudioSamplingFreq384000Hz));

  // SupportedFrameDurations = 10ms, 7.5ms, 10ms preferred
  ASSERT_TRUE(caps.HasSupportedFrameDurations());
  ASSERT_EQ(codec_spec_caps::kLeAudioCodecFrameDur7500us |
                    codec_spec_caps::kLeAudioCodecFrameDur10000us |
                    codec_spec_caps::kLeAudioCodecFrameDurPrefer10000us,
            caps.supported_frame_durations.value());
  // Check config values against the capabilities
  ASSERT_TRUE(caps.IsFrameDurationConfigSupported(codec_spec_conf::kLeAudioCodecFrameDur7500us));
  ASSERT_TRUE(caps.IsFrameDurationConfigSupported(codec_spec_conf::kLeAudioCodecFrameDur10000us));

  // SupportedAudioChannelCounts = 0b1 | 0b2 (one and two channels)
  ASSERT_TRUE(caps.HasSupportedAudioChannelCounts());
  ASSERT_EQ(codec_spec_caps::kLeAudioCodecChannelCountSingleChannel |
                    codec_spec_caps::kLeAudioCodecChannelCountTwoChannel,
            caps.supported_audio_channel_counts.value());
  // Check config values against the capabilities
  ASSERT_TRUE(caps.IsAudioChannelCountsSupported(1));
  ASSERT_TRUE(caps.IsAudioChannelCountsSupported(2));
  for (uint8_t i = 3; i < 8; ++i) {
    ASSERT_FALSE(caps.IsAudioChannelCountsSupported(i));
  }

  // SupportedOctetsPerCodecFrame = min:40, max:80
  ASSERT_TRUE(caps.HasSupportedOctetsPerCodecFrame());
  ASSERT_EQ(codec_spec_caps::kLeAudioCodecFrameLen40,
            caps.supported_min_octets_per_codec_frame.value());
  ASSERT_EQ(codec_spec_caps::kLeAudioCodecFrameLen80,
            caps.supported_max_octets_per_codec_frame.value());
  // Check config values against the capabilities
  ASSERT_FALSE(caps.IsOctetsPerCodecFrameConfigSupported(codec_spec_conf::kLeAudioCodecFrameLen30));
  ASSERT_TRUE(caps.IsOctetsPerCodecFrameConfigSupported(codec_spec_conf::kLeAudioCodecFrameLen40));
  // Supported since: 40(min) < 60 < 80(max)
  ASSERT_TRUE(caps.IsOctetsPerCodecFrameConfigSupported(codec_spec_conf::kLeAudioCodecFrameLen60));
  ASSERT_TRUE(caps.IsOctetsPerCodecFrameConfigSupported(codec_spec_conf::kLeAudioCodecFrameLen80));
  ASSERT_FALSE(
          caps.IsOctetsPerCodecFrameConfigSupported(codec_spec_conf::kLeAudioCodecFrameLen120));

  // SupportedMaxCodecFramesPerSdu = 2
  ASSERT_TRUE(caps.HasSupportedMaxCodecFramesPerSdu());
  ASSERT_EQ(2, caps.supported_max_codec_frames_per_sdu.value());
  // Check config values against the capabilities: {1,2} <= 2(max)
  ASSERT_TRUE(caps.IsCodecFramesPerSduSupported(1));
  ASSERT_TRUE(caps.IsCodecFramesPerSduSupported(2));
  ASSERT_FALSE(caps.IsCodecFramesPerSduSupported(3));
}

TEST(LeAudioLtvMapTest, test_metadata_use_guard1) {
  auto default_context = (uint16_t)bluetooth::le_audio::types::LeAudioContextType::VOICEASSISTANTS;
  static const std::vector<uint8_t> default_metadata = {
          bluetooth::le_audio::types::kLeAudioMetadataStreamingAudioContextLen + 1,
          bluetooth::le_audio::types::kLeAudioMetadataTypeStreamingAudioContext,
          (uint8_t)(default_context & 0x00FF), (uint8_t)((default_context & 0xFF00) >> 8)};

  // Parse
  bool success = true;
  LeAudioLtvMap ltv_map =
          LeAudioLtvMap::Parse(default_metadata.data(), default_metadata.size(), success);
  ASSERT_TRUE(success);

  // Verify the codec capabilities values
  auto metadata = ltv_map.GetAsLeAudioMetadata();

  // Should fail when trying to reinterpret the LTV as configuration
  EXPECT_DEATH(ltv_map.GetAsCoreCodecConfig(), "");
}

TEST(LeAudioLtvMapTest, test_metadata_use_guard2) {
  auto default_context = (uint16_t)bluetooth::le_audio::types::LeAudioContextType::VOICEASSISTANTS;
  static const std::vector<uint8_t> default_metadata = {
          bluetooth::le_audio::types::kLeAudioMetadataStreamingAudioContextLen + 1,
          bluetooth::le_audio::types::kLeAudioMetadataTypeStreamingAudioContext,
          (uint8_t)(default_context & 0x00FF), (uint8_t)((default_context & 0xFF00) >> 8)};

  // Parse
  bool success = true;
  LeAudioLtvMap ltv_map =
          LeAudioLtvMap::Parse(default_metadata.data(), default_metadata.size(), success);
  ASSERT_TRUE(success);

  // Verify the codec capabilities values
  auto metadata = ltv_map.GetAsLeAudioMetadata();

  // Should fail when trying to reinterpret the LTV as configuration
  EXPECT_DEATH(ltv_map.GetAsCoreCodecCapabilities(), "");
}

static auto PrepareMetadataLtv() {
  ::bluetooth::le_audio::types::LeAudioLtvMap metadata_ltvs;
  // Prepare the metadata LTVs
  metadata_ltvs
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypePreferredAudioContext,
               (uint16_t)10)
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeStreamingAudioContext, (uint16_t)8)
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeProgramInfo,
               std::string{"ProgramInfo"})
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeLanguage, std::string{"ice"})
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeCcidList,
               std::vector<uint8_t>{1, 2, 3})
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeparentalRating, (uint8_t)0x01)
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeProgramInfoUri,
               std::string{"ProgramInfoUri"})
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeAudioActiveState, false)
          .Add(::bluetooth::le_audio::types::
                       kLeAudioMetadataTypeBroadcastAudioImmediateRenderingFlag,
               true)
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeExtendedMetadata,
               std::vector<uint8_t>{1, 2, 3})
          .Add(::bluetooth::le_audio::types::kLeAudioMetadataTypeVendorSpecific,
               std::vector<uint8_t>{1, 2, 3});
  return metadata_ltvs;
}

TEST(LeAudioLtvMapTest, test_metadata_valid) {
  // Prepare the reference LTV
  auto metadata_ltv = PrepareMetadataLtv();
  auto raw_metadata = metadata_ltv.RawPacket();

  // Check the Parsing
  bool success = true;
  LeAudioLtvMap parsed_ltv_map =
          LeAudioLtvMap::Parse(raw_metadata.data(), raw_metadata.size(), success);
  ASSERT_TRUE(success);

  // Verify the values
  auto metadata = metadata_ltv.GetAsLeAudioMetadata();
  auto parsed_metadata = parsed_ltv_map.GetAsLeAudioMetadata();
  ASSERT_EQ(parsed_metadata.preferred_audio_context.value(),
            metadata.preferred_audio_context.value());
  ASSERT_EQ(parsed_metadata.program_info.value(), metadata.program_info.value());
  ASSERT_TRUE(parsed_metadata.language.has_value());
  ASSERT_TRUE(metadata.language.has_value());
  ASSERT_EQ(parsed_metadata.language.value(), metadata.language.value());
  ASSERT_EQ(parsed_metadata.ccid_list.value(), metadata.ccid_list.value());
  ASSERT_EQ(parsed_metadata.parental_rating.value(), metadata.parental_rating.value());
  ASSERT_EQ(parsed_metadata.program_info_uri.value(), metadata.program_info_uri.value());
  ASSERT_EQ(parsed_metadata.audio_active_state.value(), metadata.audio_active_state.value());
  ASSERT_EQ(parsed_metadata.broadcast_audio_immediate_rendering.value(),
            metadata.broadcast_audio_immediate_rendering.value());
  ASSERT_EQ(parsed_metadata.extended_metadata.value(), metadata.extended_metadata.value());
  ASSERT_EQ(parsed_metadata.vendor_specific.value(), metadata.vendor_specific.value());
}

TEST(LeAudioLtvMapTest, test_adding_types) {
  LeAudioLtvMap ltv_map;
  ltv_map.Add(1, (uint8_t)127);
  ltv_map.Add(2, (uint16_t)32767);
  ltv_map.Add(3, (uint32_t)65535);
  ltv_map.Add(4, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});
  ltv_map.Add(5, std::string("sample text"));
  ltv_map.Add(6, true);

  ASSERT_EQ(6lu, ltv_map.Size());

  uint8_t u8;
  auto pp = ltv_map.At(1).data();
  STREAM_TO_UINT8(u8, pp);
  ASSERT_EQ((uint8_t)127, u8);

  uint16_t u16;
  pp = ltv_map.At(2).data();
  STREAM_TO_UINT16(u16, pp);
  ASSERT_EQ((uint16_t)32767, u16);

  uint32_t u32;
  pp = ltv_map.At(3).data();
  STREAM_TO_UINT32(u32, pp);
  ASSERT_EQ((uint32_t)65535, u32);

  ASSERT_EQ((std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}), ltv_map.At(4));

  ASSERT_EQ(std::string("sample text"), std::string(ltv_map.At(5).begin(), ltv_map.At(5).end()));

  ASSERT_EQ(true, (bool)ltv_map.At(6).data()[0]);
}

TEST(LeAudioLtvMapTest, test_hash_sanity) {
  LeAudioLtvMap ltv_map;
  ASSERT_EQ(ltv_map.GetHash(), 0lu);

  auto hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ltv_map.Add(1, (uint16_t)32767);
  ltv_map.Add(2, (uint32_t)65535);
  ltv_map.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});

  ASSERT_NE(ltv_map.GetHash(), 0lu);
  ASSERT_NE(ltv_map.GetHash(), hash);
  ASSERT_EQ(ltv_map, ltv_map);

  // Compare hashes of equal LTV maps, filled in a different order
  LeAudioLtvMap ltv_map_two;
  ltv_map_two.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});
  ltv_map_two.Add(0, (uint8_t)127);
  ltv_map_two.Add(2, (uint32_t)65535);
  ltv_map_two.Add(1, (uint16_t)32767);
  ASSERT_EQ(ltv_map, ltv_map_two);
}

TEST(LeAudioLtvMapTest, test_value_hash_sanity) {
  LeAudioLtvMap ltv_map;

  ltv_map.Add(1, (uint16_t)32767);
  auto hash = ltv_map.GetHash();

  // Same value but type size is different
  hash = ltv_map.GetHash();
  ltv_map.Add(1, (uint32_t)32767);
  ASSERT_NE(ltv_map.GetHash(), hash);
}

TEST(LeAudioLtvMapTest, test_type_change_same_value) {
  LeAudioLtvMap ltv_map_one;
  ltv_map_one.Add(1, (uint16_t)32767);

  LeAudioLtvMap ltv_map_two;
  // The same value but different type
  ltv_map_two.Add(3, (uint16_t)32767);

  ASSERT_NE(ltv_map_one.GetHash(), ltv_map_two.GetHash());
}

TEST(LeAudioLtvMapTest, test_add_changing_hash) {
  LeAudioLtvMap ltv_map;

  auto hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Add(1, (uint16_t)32767);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Add(2, (uint32_t)65535);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});
  ASSERT_NE(ltv_map.GetHash(), hash);
}

TEST(LeAudioLtvMapTest, test_update_changing_hash) {
  LeAudioLtvMap ltv_map;

  auto hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint16_t)32767);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint32_t)65535);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Add(0, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});
  ASSERT_NE(ltv_map.GetHash(), hash);
}

TEST(LeAudioLtvMapTest, test_update_same_not_changing_hash) {
  LeAudioLtvMap ltv_map;

  auto hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ASSERT_EQ(ltv_map.GetHash(), hash);
}

TEST(LeAudioLtvMapTest, test_remove_changing_hash) {
  LeAudioLtvMap ltv_map;

  auto hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ltv_map.Add(1, (uint16_t)32767);
  ltv_map.Add(2, (uint32_t)65535);
  ltv_map.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});

  hash = ltv_map.GetHash();
  ltv_map.Remove(0);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Remove(1);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Remove(2);
  ASSERT_NE(ltv_map.GetHash(), hash);

  hash = ltv_map.GetHash();
  ltv_map.Remove(3);
  ASSERT_NE(ltv_map.GetHash(), hash);
}

TEST(LeAudioLtvMapTest, test_clear_changing_hash) {
  LeAudioLtvMap ltv_map;

  auto hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ltv_map.Add(1, (uint16_t)32767);
  ltv_map.Add(2, (uint32_t)65535);
  ltv_map.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});

  hash = ltv_map.GetHash();
  ltv_map.Clear();
  ASSERT_NE(ltv_map.GetHash(), hash);

  // 2nd clear should not change it
  hash = ltv_map.GetHash();
  ltv_map.Clear();
  ASSERT_EQ(ltv_map.GetHash(), hash);

  // Check if empty maps have equal hash
  LeAudioLtvMap empty_ltv_map;
  ASSERT_EQ(empty_ltv_map, ltv_map);
}

TEST(LeAudioLtvMapTest, test_remove_all_changing_hash) {
  LeAudioLtvMap ltv_map;

  auto hash = ltv_map.GetHash();
  ltv_map.Add(0, (uint8_t)127);
  ltv_map.Add(1, (uint16_t)32767);
  ltv_map.Add(2, (uint32_t)65535);
  ltv_map.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});

  LeAudioLtvMap ltv_map_1st_half;
  ltv_map_1st_half.Add(1, (uint16_t)32767);
  ltv_map_1st_half.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});

  LeAudioLtvMap ltv_map_2nd_half;
  ltv_map_2nd_half.Add(0, (uint8_t)127);
  ltv_map_2nd_half.Add(2, (uint32_t)65535);

  ASSERT_NE(ltv_map_1st_half, ltv_map_2nd_half);
  ASSERT_NE(ltv_map, ltv_map_2nd_half);

  hash = ltv_map.GetHash();
  ltv_map.RemoveAllTypes(ltv_map_1st_half);
  ASSERT_NE(hash, ltv_map.GetHash());

  hash = ltv_map.GetHash();
  ltv_map.RemoveAllTypes(ltv_map_2nd_half);
  ASSERT_NE(hash, ltv_map.GetHash());

  // Check if empty maps have equal hash
  LeAudioLtvMap empty_ltv_map;
  ASSERT_EQ(empty_ltv_map, ltv_map);
}

TEST(LeAudioLtvMapTest, test_intersection) {
  LeAudioLtvMap ltv_map_one;
  ltv_map_one.Add(1, (uint16_t)32767);
  ltv_map_one.Add(3, std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9});
  ltv_map_one.Add(2, (uint32_t)65535);

  LeAudioLtvMap ltv_map_two;
  ltv_map_two.Add(0, (uint8_t)127);
  // Not the type is the same but value differs
  ltv_map_two.Add(1, (uint16_t)32766);
  ltv_map_two.Add(2, (uint32_t)65535);

  LeAudioLtvMap ltv_map_common;
  ltv_map_common.Add(2, (uint32_t)65535);
  ASSERT_NE(ltv_map_common.GetHash(), 0lu);

  ASSERT_EQ(ltv_map_one.GetIntersection(ltv_map_two).GetHash(), ltv_map_common.GetHash());
  ASSERT_EQ(ltv_map_two.GetIntersection(ltv_map_one), ltv_map_common);
}

constexpr types::LeAudioCodecId kLeAudioCodecIdVendor1 = {
        .coding_format = types::kLeAudioCodingFormatVendorSpecific,
        // Not a particular vendor - just some random numbers
        .vendor_company_id = 0xC0,
        .vendor_codec_id = 0xDE,
};

static const set_configurations::CodecConfigSetting vendor_16_2 = {
        .id = kLeAudioCodecIdVendor1,
        .params = types::LeAudioLtvMap({
                LTV_ENTRY_SAMPLING_FREQUENCY(codec_spec_conf::kLeAudioSamplingFreq16000Hz),
                LTV_ENTRY_FRAME_DURATION(codec_spec_conf::kLeAudioCodecFrameDur10000us),
                LTV_ENTRY_AUDIO_CHANNEL_ALLOCATION(codec_spec_conf::kLeAudioLocationStereo),
                LTV_ENTRY_OCTETS_PER_CODEC_FRAME(40),
        }),
        .vendor_params = {0x01, 0x02, 0x03, 0x04},
        .channel_count_per_iso_stream = 1,
};

TEST(CodecConfigSettingTest, test_vendor_codec_type) {
  auto vendor_codec = vendor_16_2;
  ASSERT_EQ(vendor_16_2, vendor_codec);
}

TEST(CodecSpecTest, test_sampling_frequency_transition) {
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq8000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq8000Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq11025Hz)),
            codec_spec_conf::kLeAudioSamplingFreq11025Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq16000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq16000Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq22050Hz)),
            codec_spec_conf::kLeAudioSamplingFreq22050Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq24000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq24000Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq32000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq32000Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq44100Hz)),
            codec_spec_conf::kLeAudioSamplingFreq44100Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq48000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq48000Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq88200Hz)),
            codec_spec_conf::kLeAudioSamplingFreq88200Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq96000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq96000Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq176400Hz)),
            codec_spec_conf::kLeAudioSamplingFreq176400Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq192000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq192000Hz);
  ASSERT_EQ(codec_spec_conf::SingleSamplingFreqCapability2Config(
                    codec_spec_caps::SamplingFreqConfig2Capability(
                            codec_spec_conf::kLeAudioSamplingFreq384000Hz)),
            codec_spec_conf::kLeAudioSamplingFreq384000Hz);
}

TEST(CodecSpecTest, test_frame_duration_transition) {
  ASSERT_EQ(codec_spec_conf::SingleFrameDurationCapability2Config(
                    codec_spec_caps::FrameDurationConfig2Capability(
                            codec_spec_conf::kLeAudioCodecFrameDur7500us)),
            codec_spec_conf::kLeAudioCodecFrameDur7500us);
  ASSERT_EQ(codec_spec_conf::SingleFrameDurationCapability2Config(
                    codec_spec_caps::FrameDurationConfig2Capability(
                            codec_spec_conf::kLeAudioCodecFrameDur10000us)),
            codec_spec_conf::kLeAudioCodecFrameDur10000us);
}

TEST(CodecSpecTest, test_channel_count_transition) {
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountNone)),
            codec_spec_caps::kLeAudioCodecChannelCountNone);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountSingleChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountSingleChannel);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountTwoChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountTwoChannel);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountThreeChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountThreeChannel);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountFourChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountFourChannel);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountFiveChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountFiveChannel);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountSixChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountSixChannel);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountSevenChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountSevenChannel);
  ASSERT_EQ(codec_spec_caps::ChannelCountConfig2Capability(
                    codec_spec_conf::SingleChannelCountCapability2Config(
                            codec_spec_caps::kLeAudioCodecChannelCountEightChannel)),
            codec_spec_caps::kLeAudioCodecChannelCountEightChannel);
}

TEST(CodecConfigTest, test_lc3_bits_per_sample) {
  set_configurations::CodecConfigSetting lc3_codec_config = {
          .id = {.coding_format = types::kLeAudioCodingFormatLC3},
  };
  ASSERT_EQ(utils::translateToBtLeAudioCodecConfigBitPerSample(lc3_codec_config.GetBitsPerSample()),
            LE_AUDIO_BITS_PER_SAMPLE_INDEX_16);
}

TEST(CodecConfigTest, test_invalid_codec_bits_per_sample) {
  set_configurations::CodecConfigSetting invalid_codec_config = {
          .id = {.coding_format = types::kLeAudioCodingFormatVendorSpecific}};
  ASSERT_EQ(utils::translateToBtLeAudioCodecConfigBitPerSample(
                    invalid_codec_config.GetBitsPerSample()),
            LE_AUDIO_BITS_PER_SAMPLE_INDEX_NONE);
}

}  // namespace types
}  // namespace bluetooth::le_audio
