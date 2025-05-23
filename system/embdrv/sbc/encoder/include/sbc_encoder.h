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
 *  This file contains constants and structures used by Encoder.
 *
 ******************************************************************************/

#ifndef SBC_ENCODER_H
#define SBC_ENCODER_H

#define ENCODER_VERSION "0025"

#include <stdbool.h>
#include <stdint.h>

/*DEFINES*/
#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE (!FALSE)
#endif

#define SBC_MAX_NUM_OF_SUBBANDS 8
#define SBC_MAX_NUM_OF_CHANNELS 2
#define SBC_MAX_NUM_OF_BLOCKS 16

#define SBC_LOUDNESS 0
#define SBC_SNR 1

#define SUB_BANDS_8 8
#define SUB_BANDS_4 4

#define SBC_sf16000 0
#define SBC_sf32000 1
#define SBC_sf44100 2
#define SBC_sf48000 3

#define SBC_MONO 0
#define SBC_DUAL 1
#define SBC_STEREO 2
#define SBC_JOINT_STEREO 3

#define SBC_BLOCK_0 4
#define SBC_BLOCK_1 8
#define SBC_BLOCK_2 12
#define SBC_BLOCK_3 16

#define SBC_NULL 0

#define SBC_FORMAT_GENERAL 0
#define SBC_FORMAT_MSBC 1

#ifndef SBC_MAX_NUM_FRAME
#define SBC_MAX_NUM_FRAME 1
#endif

#ifndef SBC_DSP_OPT
#define SBC_DSP_OPT FALSE
#endif

/* Set SBC_USE_ARM_PRAGMA to TRUE to use "#pragma arm section zidata" */
#ifndef SBC_USE_ARM_PRAGMA
#define SBC_USE_ARM_PRAGMA FALSE
#endif

/* Set SBC_ARM_ASM_OPT to TRUE in case the target is an ARM */
/* this will replace all the 32 and 64 bit mult by in line assembly code */
#ifndef SBC_ARM_ASM_OPT
#define SBC_ARM_ASM_OPT FALSE
#endif

/* green hill compiler option -> Used to distinguish the syntax for inline
 * assembly code
 */
#ifndef SBC_GHS_COMPILER
#define SBC_GHS_COMPILER FALSE
#endif

/* ARM compiler option -> Used to distinguish the syntax for inline assembly
 * code */
#ifndef SBC_ARM_COMPILER
#define SBC_ARM_COMPILER TRUE
#endif

/* Set SBC_IPAQ_OPT to TRUE in case the target is an ARM */
/* 32 and 64 bit mult will be performed using int64_t ( usualy __int64 ) cast
 * that usualy give optimal performance if supported
 */
#ifndef SBC_IPAQ_OPT
#define SBC_IPAQ_OPT TRUE
#endif

/* Debug only: set SBC_IS_64_MULT_IN_WINDOW_ACCU to TRUE to use 64 bit
 * multiplication in the windowing
 */
/* -> not recomended, more MIPS for the same restitution.  */
#ifndef SBC_IS_64_MULT_IN_WINDOW_ACCU
#define SBC_IS_64_MULT_IN_WINDOW_ACCU FALSE
#endif /*SBC_IS_64_MULT_IN_WINDOW_ACCU */

/* Set SBC_IS_64_MULT_IN_IDCT to TRUE to use 64 bits multiplication in the DCT
 * of Matrixing
 */
/* -> more MIPS required for a better audio quality. comparasion with the SIG
 * utilities shows a division by 10 of the RMS
 */
/* CAUTION: It only apply in the if SBC_FAST_DCT is set to TRUE */
#ifndef SBC_IS_64_MULT_IN_IDCT
#define SBC_IS_64_MULT_IN_IDCT FALSE
#endif /*SBC_IS_64_MULT_IN_IDCT */

/* set SBC_IS_64_MULT_IN_QUANTIZER to TRUE to use 64 bits multiplication in the
 * quantizer
 */
/* setting this flag to FALSE adds a whistling noise at 5.5 and 11 KHz usualy
 * not perceptible by human's hears. */
#ifndef SBC_IS_64_MULT_IN_QUANTIZER
#define SBC_IS_64_MULT_IN_QUANTIZER TRUE
#endif /*SBC_IS_64_MULT_IN_IDCT */

/* Debug only: set this flag to FALSE to disable fast DCT algorithm */
#ifndef SBC_FAST_DCT
#define SBC_FAST_DCT TRUE
#endif /*SBC_FAST_DCT */

/* In case we do not use joint stereo mode the flag save some RAM and ROM in
 * case it is set to FALSE */
#ifndef SBC_JOINT_STE_INCLUDED
#define SBC_JOINT_STE_INCLUDED TRUE
#endif

#define MINIMUM_ENC_VX_BUFFER_SIZE (8 * 10 * 2)
#ifndef ENC_VX_BUFFER_SIZE
#define ENC_VX_BUFFER_SIZE (MINIMUM_ENC_VX_BUFFER_SIZE + 64)
/*#define ENC_VX_BUFFER_SIZE MINIMUM_ENC_VX_BUFFER_SIZE + 1024*/
#endif

/*constants used for index calculation*/
#define SBC_BLK (SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS)

#define SBC_MAX_PCM_BUFFER_SIZE \
  (SBC_MAX_NUM_FRAME * SBC_MAX_NUM_OF_BLOCKS * SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS)

typedef struct SBC_ENC_PARAMS_TAG {
  int16_t s16SamplingFreq;  /* 16k, 32k, 44.1k or 48k*/
  int16_t s16ChannelMode;   /* mono, dual, streo or joint streo*/
  int16_t s16NumOfSubBands; /* 4 or 8 */
  int16_t s16NumOfChannels;
  int16_t s16NumOfBlocks;      /* 4, 8, 12 or 16*/
  int16_t s16AllocationMethod; /* loudness or SNR*/
  int16_t s16BitPool;          /* 16*numOfSb for mono & dual;
                                 32*numOfSb for stereo & joint stereo */
  uint16_t u16BitRate;
#if (SBC_JOINT_STE_INCLUDED == TRUE)
  int16_t as16Join[SBC_MAX_NUM_OF_SUBBANDS]; /*1 if JS, 0 otherwise*/
#endif

  int16_t s16MaxBitNeed;
  int16_t as16ScaleFactor[SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS];

  int16_t s16ScartchMemForBitAlloc[16];

  int32_t s32SbBuffer[SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS * SBC_MAX_NUM_OF_BLOCKS];

  int16_t as16Bits[SBC_MAX_NUM_OF_CHANNELS * SBC_MAX_NUM_OF_SUBBANDS];

  uint16_t FrameHeader;
  uint8_t Format; /* Default to be SBC_FORMAT_GENERAL for SBC if not assigned.
                    Assigning to SBC_FORMAT_MSBC for mSBC */
} SBC_ENC_PARAMS;

#ifdef __cplusplus
extern "C" {
#endif

/* Encode the frame using SBC. The output is written into |output|. Return
 * number of bytes written. */
uint32_t SBC_Encode(SBC_ENC_PARAMS* strEncParams, int16_t* input, uint8_t* output);
void SBC_Encoder_Init(SBC_ENC_PARAMS* strEncParams);

#ifdef __cplusplus
}
#endif

#endif /* SBC_ENCODER_H */
