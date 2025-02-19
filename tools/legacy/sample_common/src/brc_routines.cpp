/*############################################################################
  # Copyright (C) 2005 Intel Corporation
  #
  # SPDX-License-Identifier: MIT
  ############################################################################*/

#include "brc_routines.h"

#include <math.h>
#include <algorithm>

#ifndef MFX_VERSION
    #error MFX_VERSION not defined
#endif

#define BRC_SCENE_CHANGE_RATIO1 20.0
#define BRC_SCENE_CHANGE_RATIO2 5.0

static mfxU32 hevcBitRateScale(mfxU32 bitrate) {
    mfxU32 bit_rate_scale = 0;
    while (bit_rate_scale < 16 && (bitrate & ((1 << (6 + bit_rate_scale + 1)) - 1)) == 0)
        bit_rate_scale++;
    return bit_rate_scale;
}
static mfxU32 hevcCbpSizeScale(mfxU32 cpbSize) {
    mfxU32 cpb_size_scale = 2;
    while (cpb_size_scale < 16 && (cpbSize & ((1 << (4 + cpb_size_scale + 1)) - 1)) == 0)
        cpb_size_scale++;
    return cpb_size_scale;
}
const mfxU32 h264_h265_au_cpb_removal_delay_length_minus1 = 23;
const mfxU32 h264_bit_rate_scale                          = 4;
const mfxU32 h264_cpb_size_scale                          = 2;

mfxExtBuffer* Hevc_GetExtBuffer(mfxExtBuffer** extBuf, mfxU32 numExtBuf, mfxU32 id) {
    if (extBuf != 0) {
        for (mfxU16 i = 0; i < numExtBuf; i++) {
            if (extBuf[i] != 0 && extBuf[i]->BufferId == id) // assuming aligned buffers
                return (extBuf[i]);
        }
    }

    return 0;
}

mfxStatus cBRCParams::Init(mfxVideoParam* par, bool bField) {
    printf("Sample BRC is used\n");
    MFX_CHECK_NULL_PTR1(par);
    MFX_CHECK(par->mfx.RateControlMethod == MFX_RATECONTROL_CBR ||
                  par->mfx.RateControlMethod == MFX_RATECONTROL_VBR,
              MFX_ERR_UNDEFINED_BEHAVIOR);
    bFieldMode = bField;
    codecId    = par->mfx.CodecId;

    mfxU32 k  = par->mfx.BRCParamMultiplier == 0 ? 1 : par->mfx.BRCParamMultiplier;
    targetbps = k * par->mfx.TargetKbps * 1000;
    maxbps    = k * par->mfx.MaxKbps * 1000;

    maxbps = (par->mfx.RateControlMethod == MFX_RATECONTROL_CBR)
                 ? targetbps
                 : ((maxbps >= targetbps) ? maxbps : targetbps);

    mfxU32 bit_rate_scale =
        (par->mfx.CodecId == MFX_CODEC_AVC) ? h264_bit_rate_scale : hevcBitRateScale(maxbps);
    mfxU32 cpb_size_scale =
        (par->mfx.CodecId == MFX_CODEC_AVC) ? h264_cpb_size_scale : hevcCbpSizeScale(maxbps);

    rateControlMethod = par->mfx.RateControlMethod;
    maxbps            = ((maxbps >> (6 + bit_rate_scale)) << (6 + bit_rate_scale));

    mfxExtCodingOption* pExtCO = (mfxExtCodingOption*)Hevc_GetExtBuffer(par->ExtParam,
                                                                        par->NumExtParam,
                                                                        MFX_EXTBUFF_CODING_OPTION);

    HRDConformance = MFX_BRC_NO_HRD;
    if (pExtCO) {
        if ((MFX_CODINGOPTION_OFF != pExtCO->NalHrdConformance) &&
            (MFX_CODINGOPTION_OFF != pExtCO->VuiNalHrdParameters))
            HRDConformance = MFX_BRC_HRD_STRONG;
        else if ((MFX_CODINGOPTION_ON == pExtCO->NalHrdConformance) &&
                 (MFX_CODINGOPTION_OFF == pExtCO->VuiNalHrdParameters))
            HRDConformance = MFX_BRC_HRD_WEAK;
    }

    if (HRDConformance != MFX_BRC_NO_HRD) {
        bufferSizeInBytes = ((k * par->mfx.BufferSizeInKB * 1000) >> (cpb_size_scale + 1))
                            << (cpb_size_scale + 1);
        initialDelayInBytes = ((k * par->mfx.InitialDelayInKB * 1000) >> (cpb_size_scale + 1))
                              << (cpb_size_scale + 1);
        bRec   = 1;
        bPanic = (HRDConformance == MFX_BRC_HRD_STRONG) ? 1 : 0;
    }
    MFX_CHECK(par->mfx.FrameInfo.FrameRateExtD != 0 && par->mfx.FrameInfo.FrameRateExtN != 0,
              MFX_ERR_UNDEFINED_BEHAVIOR);

    frameRate = (mfxF64)par->mfx.FrameInfo.FrameRateExtN / (mfxF64)par->mfx.FrameInfo.FrameRateExtD;

    width  = par->mfx.FrameInfo.Width;
    height = par->mfx.FrameInfo.Height;

    chromaFormat = par->mfx.FrameInfo.ChromaFormat == 0 ? MFX_CHROMAFORMAT_YUV420
                                                        : par->mfx.FrameInfo.ChromaFormat;
    bitDepthLuma = par->mfx.FrameInfo.BitDepthLuma == 0 ? 8 : par->mfx.FrameInfo.BitDepthLuma;

    quantOffset = 6 * (bitDepthLuma - 8);

    inputBitsPerFrame    = targetbps / frameRate;
    maxInputBitsPerFrame = maxbps / frameRate;
    gopPicSize           = par->mfx.GopPicSize * (bFieldMode ? 2 : 1);
    gopRefDist           = par->mfx.GopRefDist * (bFieldMode ? 2 : 1);

    mfxExtCodingOption2* pExtCO2 =
        (mfxExtCodingOption2*)Hevc_GetExtBuffer(par->ExtParam,
                                                par->NumExtParam,
                                                MFX_EXTBUFF_CODING_OPTION2);
    bPyr               = (pExtCO2 && pExtCO2->BRefType == MFX_B_REF_PYRAMID);
    maxFrameSizeInBits = pExtCO2 ? pExtCO2->MaxFrameSize * 8 : 0;

    fAbPeriodLong  = 100;
    fAbPeriodShort = 6;
    dqAbPeriod     = 100;
    bAbPeriod      = 100;

    if (maxFrameSizeInBits) {
        bRec   = 1;
        bPanic = 1;
    }

    if (pExtCO2 && pExtCO2->MaxQPI <= 51 && pExtCO2->MaxQPI > pExtCO2->MinQPI &&
        pExtCO2->MinQPI >= 1 && pExtCO2->MaxQPP <= 51 && pExtCO2->MaxQPP > pExtCO2->MinQPP &&
        pExtCO2->MinQPP >= 1 && pExtCO2->MaxQPB <= 51 && pExtCO2->MaxQPB > pExtCO2->MinQPB &&
        pExtCO2->MinQPB >= 1) {
        quantMaxI = pExtCO2->MaxQPI + quantOffset;
        quantMinI = pExtCO2->MinQPI;
        quantMaxP = pExtCO2->MaxQPP + quantOffset;
        quantMinP = pExtCO2->MinQPP;
        quantMaxB = pExtCO2->MaxQPB + quantOffset;
        quantMinB = pExtCO2->MinQPB;
    }
    else {
        quantMaxI = quantMaxP = quantMaxB = 51 + quantOffset;
        quantMinI = quantMinP = quantMinB = 1;
    }

    mfxExtCodingOption3* pExtCO3 =
        (mfxExtCodingOption3*)Hevc_GetExtBuffer(par->ExtParam,
                                                par->NumExtParam,
                                                MFX_EXTBUFF_CODING_OPTION3);
    if (pExtCO3) {
        WinBRCMaxAvgKbps = pExtCO3->WinBRCMaxAvgKbps * par->mfx.BRCParamMultiplier;
        WinBRCSize       = pExtCO3->WinBRCSize;
    }
    mMBBRC = pExtCO3 && (pExtCO3->EnableMBQP == MFX_CODINGOPTION_ON);
    return MFX_ERR_NONE;
}

mfxStatus cBRCParams::GetBRCResetType(mfxVideoParam* par,
                                      bool bNewSequence,
                                      bool& bBRCReset,
                                      bool& bSlidingWindowReset) {
    bBRCReset           = false;
    bSlidingWindowReset = false;

    if (bNewSequence)
        return MFX_ERR_NONE;

    cBRCParams new_par;
    mfxStatus sts = new_par.Init(par);
    MFX_CHECK_STS(sts);

    MFX_CHECK(new_par.rateControlMethod == rateControlMethod, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
    MFX_CHECK(new_par.HRDConformance == HRDConformance, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
    MFX_CHECK(new_par.frameRate == frameRate, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
    MFX_CHECK(new_par.width == width, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
    MFX_CHECK(new_par.height == height, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
    MFX_CHECK(new_par.chromaFormat == chromaFormat, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
    MFX_CHECK(new_par.bitDepthLuma == bitDepthLuma, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);

    if (HRDConformance == MFX_BRC_HRD_STRONG) {
        MFX_CHECK(new_par.bufferSizeInBytes == bufferSizeInBytes, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
        MFX_CHECK(new_par.initialDelayInBytes == initialDelayInBytes,
                  MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
        MFX_CHECK(new_par.targetbps == targetbps, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
        MFX_CHECK(new_par.maxbps == maxbps, MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);
    }
    else if (new_par.targetbps != targetbps || new_par.maxbps != maxbps) {
        bBRCReset = true;
    }

    if (new_par.WinBRCMaxAvgKbps != WinBRCMaxAvgKbps) {
        bBRCReset           = true;
        bSlidingWindowReset = true;
    }

    if (new_par.maxFrameSizeInBits != maxFrameSizeInBits)
        bBRCReset = true;
    if (new_par.gopPicSize != gopPicSize)
        bBRCReset = true;
    if (new_par.gopRefDist != gopRefDist)
        bBRCReset = true;
    if (new_par.bPyr != bPyr)
        bBRCReset = true;
    if (new_par.quantMaxI != quantMaxI)
        bBRCReset = true;
    if (new_par.quantMinI != quantMinI)
        bBRCReset = true;
    if (new_par.quantMaxP != quantMaxP)
        bBRCReset = true;
    if (new_par.quantMinP != quantMinP)
        bBRCReset = true;
    if (new_par.quantMaxB != quantMaxB)
        bBRCReset = true;
    if (new_par.quantMinB != quantMinB)
        bBRCReset = true;

    return MFX_ERR_NONE;
}

enum {
    MFX_BRC_RECODE_NONE  = 0,
    MFX_BRC_RECODE_QP    = 1,
    MFX_BRC_RECODE_PANIC = 2,
};

mfxF64 const QSTEP[88] = {
    0.630,    0.707,    0.794,    0.891,     1.000,     1.122,     1.260,    1.414,    1.587,
    1.782,    2.000,    2.245,    2.520,     2.828,     3.175,     3.564,    4.000,    4.490,
    5.040,    5.657,    6.350,    7.127,     8.000,     8.980,     10.079,   11.314,   12.699,
    14.254,   16.000,   17.959,   20.159,    22.627,    25.398,    28.509,   32.000,   35.919,
    40.317,   45.255,   50.797,   57.018,    64.000,    71.838,    80.635,   90.510,   101.594,
    114.035,  128.000,  143.675,  161.270,   181.019,   203.187,   228.070,  256.000,  287.350,
    322.540,  362.039,  406.375,  456.140,   512.000,   574.701,   645.080,  724.077,  812.749,
    912.280,  1024.000, 1149.401, 1290.159,  1448.155,  1625.499,  1824.561, 2048.000, 2298.802,
    2580.318, 2896.309, 3250.997, 3649.121,  4096.000,  4597.605,  5160.637, 5792.619, 6501.995,
    7298.242, 8192.000, 9195.209, 10321.273, 11585.238, 13003.989, 14596.485
};

mfxI32 QStep2QpFloor(mfxF64 qstep,
                     mfxI32 qpoffset = 0) // QSTEP[qp] <= qstep, return 0<=qp<=51+mQuantOffset
{
    mfxU8 qp = mfxU8(std::upper_bound(QSTEP, QSTEP + 51 + qpoffset, qstep) - QSTEP);
    return qp > 0 ? qp - 1 : 0;
}

mfxI32 Qstep2QP(mfxF64 qstep, mfxI32 qpoffset = 0) // return 0<=qp<=51+mQuantOffset
{
    mfxI32 qp = QStep2QpFloor(qstep, qpoffset);

    // prevent going QSTEP index out of bounds
    if (qp >= (mfxI32)(sizeof(QSTEP) / sizeof(QSTEP[0])) - 1)
        return 0;
    return (qp == 51 + qpoffset || qstep < (QSTEP[qp] + QSTEP[qp + 1]) / 2) ? qp : qp + 1;
}
mfxF64 QP2Qstep(mfxI32 qp, mfxI32 qpoffset = 0) {
    return QSTEP[std::min(51 + qpoffset, qp)];
}

mfxI32 GetNewQP(mfxF64 totalFrameBits,
                mfxF64 targetFrameSizeInBits,
                mfxI32 minQP,
                mfxI32 maxQP,
                mfxI32 qp,
                mfxI32 qp_offset,
                mfxF64 f_pow,
                bool bStrict = false,
                bool bLim    = true) {
    mfxF64 qstep = 0, qstep_new = 0;
    mfxI32 qp_new = qp;

    qstep     = QP2Qstep(qp, qp_offset);
    qstep_new = qstep * pow(totalFrameBits / targetFrameSizeInBits, f_pow);
    qp_new    = Qstep2QP(qstep_new, qp_offset);

    if (totalFrameBits < targetFrameSizeInBits) // overflow
    {
        if (qp <= minQP) {
            return qp; // QP change is impossible
        }
        if (bLim)
            qp_new = std::max(qp_new, (minQP + qp + 1) >> 1);
        if (bStrict)
            qp_new = std::min(qp_new, qp - 1);
    }
    else // underflow
    {
        if (qp >= maxQP) {
            return qp; // QP change is impossible
        }
        if (bLim)
            qp_new = std::min(qp_new, (maxQP + qp + 1) >> 1);
        if (bStrict)
            qp_new = std::max(qp_new, qp + 1);
    }
    return mfx::clamp(qp_new, minQP, maxQP);
}

void UpdateQPParams(mfxI32 qp,
                    mfxU32 type,
                    BRC_Ctx& ctx,
                    mfxU32 /* rec_num */,
                    mfxI32 minQuant,
                    mfxI32 maxQuant,
                    mfxU32 level) {
    ctx.Quant = qp;
    if (type == MFX_FRAMETYPE_I) {
        ctx.QuantI = qp;
        ctx.QuantP = qp + 1;
        ctx.QuantB = qp + 2;
    }
    else if (type == MFX_FRAMETYPE_P) {
        qp -= level;
        ctx.QuantI = qp - 1;
        ctx.QuantP = qp;
        ctx.QuantB = qp + 1;
    }
    else if (type == MFX_FRAMETYPE_B) {
        level = level > 0 ? level - 1 : 0;
        qp -= level;
        ctx.QuantI = qp - 2;
        ctx.QuantP = qp - 1;
        ctx.QuantB = qp;
    }
    ctx.QuantI = mfx::clamp(ctx.QuantI, minQuant, maxQuant);
    ctx.QuantP = mfx::clamp(ctx.QuantP, minQuant, maxQuant);
    ctx.QuantB = mfx::clamp(ctx.QuantB, minQuant, maxQuant);
    //printf("ctx.QuantI %d, ctx.QuantP %d, ctx.QuantB  %d, level %d\n", ctx.QuantI, ctx.QuantP, ctx.QuantB, level);
}

mfxI32 GetRawFrameSize(mfxU32 lumaSize, mfxU16 chromaFormat, mfxU16 bitDepthLuma) {
    mfxI32 frameSize = lumaSize;

    if (chromaFormat == MFX_CHROMAFORMAT_YUV420)
        frameSize += lumaSize / 2;
    else if (chromaFormat == MFX_CHROMAFORMAT_YUV422)
        frameSize += lumaSize;
    else if (chromaFormat == MFX_CHROMAFORMAT_YUV444)
        frameSize += lumaSize * 2;

    frameSize = frameSize * bitDepthLuma / 8;
    return frameSize * 8; //frame size in bits
}
bool isFieldMode(mfxVideoParam* par) {
    return ((par->mfx.CodecId == MFX_CODEC_HEVC) &&
            !(par->mfx.FrameInfo.PicStruct & MFX_PICSTRUCT_PROGRESSIVE));
}

mfxStatus ExtBRC::Init(mfxVideoParam* par) {
    mfxStatus sts = MFX_ERR_NONE;

    MFX_CHECK(!m_bInit, MFX_ERR_UNDEFINED_BEHAVIOR);
    sts = m_par.Init(par, isFieldMode(par));
    MFX_CHECK_STS(sts);

    if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
        if (m_par.codecId == MFX_CODEC_AVC)
            m_hrdSpec.reset(new H264_HRD());
        else
            m_hrdSpec.reset(new HEVC_HRD());
        m_hrdSpec->Init(m_par);
    }
    m_ctx = {};

    m_ctx.fAbLong  = m_par.inputBitsPerFrame;
    m_ctx.fAbShort = m_par.inputBitsPerFrame;
    m_ctx.encOrder = mfxU32(-1);

    mfxI32 rawSize =
        GetRawFrameSize(m_par.width * m_par.height, m_par.chromaFormat, m_par.bitDepthLuma);
    mfxI32 qp = GetNewQP(rawSize,
                         m_par.inputBitsPerFrame,
                         m_par.quantMinI,
                         m_par.quantMaxI,
                         1,
                         m_par.quantOffset,
                         0.5,
                         false,
                         false);

    UpdateQPParams(qp, MFX_FRAMETYPE_I, m_ctx, 0, m_par.quantMinI, m_par.quantMaxI, 0);

    m_ctx.dQuantAb = qp > 0 ? 1. / qp : 1.0; //kw

    if (m_par.WinBRCSize) {
        m_avg.reset(new AVGBitrate(m_par.WinBRCSize,
                                   (mfxU32)(m_par.WinBRCMaxAvgKbps * 1000.0 / m_par.frameRate),
                                   (mfxU32)m_par.inputBitsPerFrame));
        MFX_CHECK_NULL_PTR1(m_avg.get());
    }
    if (m_par.mMBBRC) {
        mfxU32 size   = par->AsyncDepth > 1 ? 2 : 1;
        mfxU16 blSize = 16;
        mfxU32 wInBlk = (par->mfx.FrameInfo.Width + blSize - 1) / blSize;
        mfxU32 hInBlk = (par->mfx.FrameInfo.Height + blSize - 1) / blSize;

        m_MBQPBuff.resize(size * wInBlk * hInBlk);
        m_MBQP.resize(size);
        m_ExtBuff.resize(size);

        for (mfxU32 i = 0; i < size; i++) {
            m_MBQP[i].Header.BufferId = MFX_EXTBUFF_MBQP;
            m_MBQP[i].Header.BufferSz = sizeof(mfxExtMBQP);
            m_MBQP[i].BlockSize       = blSize;
            m_MBQP[i].NumQPAlloc      = wInBlk * hInBlk;
            m_MBQP[i].Mode            = MFX_MBQP_MODE_QP_VALUE;
            m_MBQP[i].QP              = &(m_MBQPBuff[i * wInBlk * hInBlk]);
            m_ExtBuff[i]              = (mfxExtBuffer*)&(m_MBQP[i]);
        }
    }
    m_bInit = true;
    return sts;
}

mfxU16 GetFrameType(mfxU16 m_frameType, mfxU16 level, mfxU16 gopRegDist) {
    if (m_frameType & MFX_FRAMETYPE_IDR)
        return MFX_FRAMETYPE_I;
    else if (m_frameType & MFX_FRAMETYPE_I)
        return MFX_FRAMETYPE_I;
    else if (m_frameType & MFX_FRAMETYPE_P)
        return MFX_FRAMETYPE_P;
    else if ((m_frameType & MFX_FRAMETYPE_REF) && (level == 0 || gopRegDist == 1))
        return MFX_FRAMETYPE_P; //low delay B
    else
        return MFX_FRAMETYPE_B;
}

bool isFrameBeforeIntra(mfxU32 order, mfxU32 intraOrder, mfxU32 gopPicSize, mfxU32 gopRefDist) {
    mfxI32 distance0 = gopPicSize * 3 / 4;
    mfxI32 distance1 = gopPicSize - gopRefDist * 3;
    return mfxI32(order - intraOrder) > std::max(distance0, distance1);
}

mfxStatus SetRecodeParams(mfxU16 brcStatus,
                          mfxI32 qp,
                          mfxI32 qp_new,
                          mfxI32 minQP,
                          mfxI32 maxQP,
                          BRC_Ctx& ctx,
                          mfxBRCFrameStatus* status) {
    ctx.bToRecode = 1;

    if (brcStatus == MFX_BRC_BIG_FRAME || brcStatus == MFX_BRC_PANIC_BIG_FRAME) {
        MFX_CHECK(qp_new >= qp, MFX_ERR_UNDEFINED_BEHAVIOR);
        ctx.Quant    = qp_new;
        ctx.QuantMax = maxQP;
        if (brcStatus == MFX_BRC_BIG_FRAME && qp_new > qp) {
            ctx.QuantMin      = std::max(qp + 1, minQP); //limit QP range for recoding
            status->BRCStatus = MFX_BRC_BIG_FRAME;
        }
        else {
            ctx.QuantMin      = minQP;
            ctx.bPanic        = 1;
            status->BRCStatus = MFX_BRC_PANIC_BIG_FRAME;
        }
    }
    else if (brcStatus == MFX_BRC_SMALL_FRAME || brcStatus == MFX_BRC_PANIC_SMALL_FRAME) {
        MFX_CHECK(qp_new <= qp, MFX_ERR_UNDEFINED_BEHAVIOR);

        ctx.Quant    = qp_new;
        ctx.QuantMin = minQP; //limit QP range for recoding

        if (brcStatus == MFX_BRC_SMALL_FRAME && qp_new < qp) {
            ctx.QuantMax      = std::min(qp - 1, maxQP);
            status->BRCStatus = MFX_BRC_SMALL_FRAME;
        }
        else {
            ctx.QuantMax      = maxQP;
            status->BRCStatus = MFX_BRC_PANIC_SMALL_FRAME;
            ctx.bPanic        = 1;
        }
    }
    //printf("recode %d , qp %d new %d, status %d\n", ctx.encOrder, qp, qp_new, status->BRCStatus);
    return MFX_ERR_NONE;
}
mfxI32
GetNewQPTotal(mfxF64 bo, mfxF64 dQP, mfxI32 minQP, mfxI32 maxQP, mfxI32 qp, bool bPyr, bool bSC) {
    mfxU8 mode = (!bPyr);

    bo               = mfx::clamp(bo, -1.0, 1.0);
    dQP              = mfx::clamp(dQP, 1. / maxQP, 1. / minQP);
    dQP              = dQP + (1. / maxQP - dQP) * bo;
    dQP              = mfx::clamp(dQP, 1. / maxQP, 1. / minQP);
    mfxI32 quant_new = (mfxI32)(1. / dQP + 0.5);

    //printf("   GetNewQPTotal: bo %f, quant %d, quant_new %d, mode %d\n", bo, qp, quant_new, mode);
    if (!bSC) {
        if (mode == 0) // low: qp_diff [-2; 2]
        {
            if (quant_new >= qp + 5)
                quant_new = qp + 2;
            else if (quant_new > qp + 3)
                quant_new = qp + 1;
            else if (quant_new <= qp - 5)
                quant_new = qp - 2;
            else if (quant_new < qp - 2)
                quant_new = qp - 1;
        }
        else // (mode == 1) midle: qp_diff [-3; 3]
        {
            if (quant_new >= qp + 5)
                quant_new = qp + 3;
            else if (quant_new > qp + 3)
                quant_new = qp + 2;
            else if (quant_new <= qp - 5)
                quant_new = qp - 3;
            else if (quant_new < qp - 2)
                quant_new = qp - 2;
        }
    }
    else {
        quant_new = mfx::clamp(quant_new, qp - 5, qp + 5);
    }
    return mfx::clamp(quant_new, minQP, maxQP);
}
// Reduce AB period before intra and increase it after intra (to avoid intra frame affect on the bottom of hrd)
mfxF64 GetAbPeriodCoeff(mfxU32 numInGop, mfxU32 gopPicSize) {
    const mfxU32 maxForCorrection = 30;
    const mfxF64 maxValue         = 1.5;
    const mfxF64 minValue         = 1.0;

    mfxU32 numForCorrection    = std::min(gopPicSize / 2, maxForCorrection);
    mfxF64 k[maxForCorrection] = {};

    if (numInGop >= gopPicSize || gopPicSize < 2)
        return 1.0;

    for (mfxU32 i = 0; i < numForCorrection; i++) {
        k[i] = maxValue - (maxValue - minValue) * i / numForCorrection;
    }
    if (numInGop < gopPicSize / 2) {
        return k[numInGop < numForCorrection ? numInGop : numForCorrection - 1];
    }
    else {
        mfxU32 n = gopPicSize - 1 - numInGop;
        return 1.0 / k[n < numForCorrection ? n : numForCorrection - 1];
    }
}

mfxI32 ExtBRC::GetCurQP(mfxU32 type, mfxI32 layer) {
    mfxI32 qp = 0;
    if (type == MFX_FRAMETYPE_I) {
        qp = m_ctx.QuantI;
        qp = mfx::clamp(qp, m_par.quantMinI, m_par.quantMaxI);
    }
    else if (type == MFX_FRAMETYPE_P) {
        qp = m_ctx.QuantP + layer;
        qp = mfx::clamp(qp, m_par.quantMinP, m_par.quantMaxP);
    }
    else {
        qp = m_ctx.QuantB + (layer > 0 ? layer - 1 : 0);
        qp = mfx::clamp(qp, m_par.quantMinB, m_par.quantMaxB);
    }
    //printf("GetCurQP I %d P %d B %d, min %d max %d type %d \n", m_ctx.QuantI, m_ctx.QuantP, m_ctx.QuantB, m_par.quantMinI, m_par.quantMaxI, type);

    return qp;
}

inline mfxU16 CheckHrdAndUpdateQP(HRDCodecSpec& hrd,
                                  mfxU32 frameSizeInBits,
                                  mfxU32 eo,
                                  bool bIdr,
                                  mfxI32 currQP) {
    if (frameSizeInBits > hrd.GetMaxFrameSizeInBits(eo, bIdr)) {
        hrd.SetUndeflowQuant(currQP);
        return MFX_BRC_BIG_FRAME;
    }
    else if (frameSizeInBits < hrd.GetMinFrameSizeInBits(eo, bIdr)) {
        hrd.SetUndeflowQuant(currQP);
        return MFX_BRC_SMALL_FRAME;
    }
    return MFX_BRC_OK;
}
mfxI32 GetFrameTargetSize(mfxU32 brcSts, mfxI32 minFrameSize, mfxI32 maxFrameSize) {
    if (brcSts != MFX_BRC_BIG_FRAME && brcSts != MFX_BRC_SMALL_FRAME)
        return 0;
    return (brcSts == MFX_BRC_BIG_FRAME) ? maxFrameSize * 3 / 4 : minFrameSize * 5 / 4;
}
mfxStatus ExtBRC::Update(mfxBRCFrameParam* frame_par,
                         mfxBRCFrameCtrl* frame_ctrl,
                         mfxBRCFrameStatus* status) {
    mfxStatus sts = MFX_ERR_NONE;

    MFX_CHECK_NULL_PTR3(frame_par, frame_ctrl, status);
    MFX_CHECK(m_bInit, MFX_ERR_NOT_INITIALIZED);

    mfxU16& brcSts       = status->BRCStatus;
    status->MinFrameSize = 0;

    //printf("ExtBRC::Update:  m_ctx.encOrder %d , frame_par->EncodedOrder %d, frame_par->NumRecode %d, frame_par->CodedFrameSize %d, qp %d\n", m_ctx.encOrder , frame_par->EncodedOrder, frame_par->NumRecode, frame_par->CodedFrameSize, frame_ctrl->QpY);

    mfxI32 bitsEncoded = frame_par->CodedFrameSize * 8;
    mfxU32 picType = GetFrameType(frame_par->FrameType, frame_par->PyramidLayer, m_par.gopRefDist);
    mfxI32 qpY     = frame_ctrl->QpY + m_par.quantOffset;
    mfxI32 layer   = frame_par->PyramidLayer;
    mfxF64 qstep   = QP2Qstep(qpY, m_par.quantOffset);

    mfxF64 fAbLong  = m_ctx.fAbLong + (bitsEncoded - m_ctx.fAbLong) / m_par.fAbPeriodLong;
    mfxF64 fAbShort = m_ctx.fAbShort + (bitsEncoded - m_ctx.fAbShort) / m_par.fAbPeriodShort;
    mfxF64 eRate    = bitsEncoded * sqrt(qstep);
    mfxF64 e2pe     = 0;
    bool bMaxFrameSizeMode =
        m_par.maxFrameSizeInBits != 0 && m_par.rateControlMethod == MFX_RATECONTROL_VBR &&
        m_par.maxFrameSizeInBits < m_par.inputBitsPerFrame * 2 &&
        m_ctx.totalDeviation < (-1) * m_par.inputBitsPerFrame * m_par.frameRate;

    if (picType == MFX_FRAMETYPE_I)
        e2pe = (m_ctx.eRateSH == 0) ? (BRC_SCENE_CHANGE_RATIO2 + 1) : eRate / m_ctx.eRateSH;
    else
        e2pe = (m_ctx.eRate == 0) ? (BRC_SCENE_CHANGE_RATIO2 + 1) : eRate / m_ctx.eRate;

    mfxU32 frameSizeLim = 0xfffffff; // sliding window limitation or external frame size limitation

    bool bSHStart      = false;
    bool bNeedUpdateQP = false;

    brcSts = MFX_BRC_OK;

    if (m_par.bRec && m_ctx.bToRecode &&
        (m_ctx.encOrder != frame_par->EncodedOrder || frame_par->NumRecode == 0)) {
        // Frame must be recoded, but encoder calls BR for another frame
        return MFX_ERR_UNDEFINED_BEHAVIOR;
    }
    if (frame_par->NumRecode == 0 || m_ctx.encOrder != frame_par->EncodedOrder) {
        // Set context for new frame
        if (picType == MFX_FRAMETYPE_I)
            m_ctx.LastIEncOrder = frame_par->EncodedOrder;
        m_ctx.encOrder  = frame_par->EncodedOrder;
        m_ctx.poc       = frame_par->DisplayOrder;
        m_ctx.bToRecode = 0;
        m_ctx.bPanic    = 0;

        if (picType == MFX_FRAMETYPE_I) {
            m_ctx.QuantMin = m_par.quantMinI;
            m_ctx.QuantMax = m_par.quantMaxI;
        }
        else if (picType == MFX_FRAMETYPE_P) {
            m_ctx.QuantMin = m_par.quantMinP;
            m_ctx.QuantMax = m_par.quantMaxP;
        }
        else {
            m_ctx.QuantMin = m_par.quantMinB;
            m_ctx.QuantMax = m_par.quantMaxB;
        }
        m_ctx.Quant = qpY;

        if (m_ctx.SceneChange && (m_ctx.poc > m_ctx.SChPoc + 1 || m_ctx.poc == 0))
            m_ctx.SceneChange &= ~16;

        bNeedUpdateQP = true;

        if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
            m_hrdSpec->ResetQuant();
        }
        //printf("m_ctx.SceneChange %d, m_ctx.poc %d, m_ctx.SChPoc, m_ctx.poc %d \n", m_ctx.SceneChange, m_ctx.poc, m_ctx.SChPoc, m_ctx.poc);
    }
    bool bIntra = picType == MFX_FRAMETYPE_I;
    if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
        brcSts = CheckHrdAndUpdateQP(*m_hrdSpec.get(),
                                     bitsEncoded,
                                     frame_par->EncodedOrder,
                                     bIntra,
                                     qpY);

        MFX_CHECK(brcSts == MFX_BRC_OK || (!m_ctx.bPanic), MFX_ERR_NOT_ENOUGH_BUFFER);
        if (brcSts == MFX_BRC_OK && !m_ctx.bPanic)
            bNeedUpdateQP = true;

        status->MinFrameSize = m_hrdSpec->GetMinFrameSizeInBits(frame_par->EncodedOrder, bIntra);

        //printf("%d: poc %d, size %d QP %d (%d %d), HRD sts %d, maxFrameSize %d, type %d \n",frame_par->EncodedOrder, frame_par->DisplayOrder, bitsEncoded, m_ctx.Quant, m_ctx.QuantMin, m_ctx.QuantMax, brcSts,  m_hrd.GetMaxFrameSize(), frame_par->FrameType);
    }
    if (e2pe > BRC_SCENE_CHANGE_RATIO2) {
        // scene change, resetting BRC statistics
        fAbLong = m_ctx.fAbLong = m_par.inputBitsPerFrame;
        fAbShort = m_ctx.fAbShort = m_par.inputBitsPerFrame;
        fAbLong  = m_ctx.fAbLong + (bitsEncoded - m_ctx.fAbLong) / m_par.fAbPeriodLong;
        fAbShort = m_ctx.fAbShort + (bitsEncoded - m_ctx.fAbShort) / m_par.fAbPeriodShort;
        m_ctx.SceneChange |= 1;
        if (picType != MFX_FRAMETYPE_B) {
            bSHStart = true;
            m_ctx.SceneChange |= 16;
            m_ctx.eRateSH = eRate;
            //if ((frame_par->DisplayOrder - m_ctx.SChPoc) >= std::min((mfxU32)(m_par.frameRate), m_par.gopRefDist))
            { m_ctx.dQuantAb = 1. / m_ctx.Quant; }
            m_ctx.SChPoc = frame_par->DisplayOrder;
            //printf("!!!!!!!!!!!!!!!!!!!!! %d m_ctx.SceneChange %d, order %d\n", frame_par->EncodedOrder, m_ctx.SceneChange, frame_par->DisplayOrder);
        }
    }

    if (m_avg.get()) {
        frameSizeLim = std::min(
            frameSizeLim,
            m_avg->GetMaxFrameSize(m_ctx.bPanic, bSHStart || bIntra, frame_par->NumRecode));
    }
    if (m_par.maxFrameSizeInBits) {
        frameSizeLim = std::min(frameSizeLim, m_par.maxFrameSizeInBits);
    }
    //printf("frameSizeLim %d (%d)\n", frameSizeLim, bitsEncoded);

    if (frame_par->NumRecode < 2)
    // Check other condions for recoding (update qp is it is needed)
    {
        mfxF64 targetFrameSize = std::max<mfxF64>((mfxF64)m_par.inputBitsPerFrame, fAbLong);
        mfxF64 maxFrameSize =
            (m_ctx.encOrder == 0 ? 6.0 : (bSHStart || picType == MFX_FRAMETYPE_I) ? 8.0 : 4.0) *
            targetFrameSize * (m_par.bPyr ? 1.5 : 1.0);
        mfxI32 quantMax = m_ctx.QuantMax;
        mfxI32 quantMin = m_ctx.QuantMin;
        mfxI32 quant    = qpY;

        maxFrameSize = std::min(maxFrameSize, (mfxF64)frameSizeLim);

        if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
            if (bSHStart || bIntra)
                maxFrameSize =
                    std::min(maxFrameSize,
                             3.5 / 9. * m_hrdSpec->GetMaxFrameSizeInBits(m_ctx.encOrder, bIntra) +
                                 5.5 / 9. * targetFrameSize);
            else
                maxFrameSize =
                    std::min(maxFrameSize,
                             2.5 / 9. * m_hrdSpec->GetMaxFrameSizeInBits(m_ctx.encOrder, bIntra) +
                                 6.5 / 9. * targetFrameSize);

            quantMax = std::min(m_hrdSpec->GetMaxQuant(), quantMax);
            quantMin = std::max(m_hrdSpec->GetMinQuant(), quantMin);
        }
        maxFrameSize = std::max(maxFrameSize, targetFrameSize);

        if (bitsEncoded > maxFrameSize && quant < quantMax) {
            mfxI32 quant_new = GetNewQP(bitsEncoded,
                                        (mfxU32)maxFrameSize,
                                        quantMin,
                                        quantMax,
                                        quant,
                                        m_par.quantOffset,
                                        1);
            if (quant_new > quant) {
                bNeedUpdateQP = false;
                //printf("    recode 1-0: %d:  k %5f bitsEncoded %d maxFrameSize %d, targetSize %d, fAbLong %f, inputBitsPerFrame %d, qp %d new %d\n",frame_par->EncodedOrder, bitsEncoded/maxFrameSize, (int)bitsEncoded, (int)maxFrameSize,(int)targetFrameSize, fAbLong, m_par.inputBitsPerFrame, quant, quant_new);
                if (quant_new > GetCurQP(picType, layer)) {
                    UpdateQPParams(bMaxFrameSizeMode ? quant_new - 1 : quant_new,
                                   picType,
                                   m_ctx,
                                   0,
                                   quantMin,
                                   quantMax,
                                   layer);
                    fAbLong = m_ctx.fAbLong = m_par.inputBitsPerFrame;
                    fAbShort = m_ctx.fAbShort = m_par.inputBitsPerFrame;
                    m_ctx.dQuantAb            = 1. / quant_new;
                }

                if (m_par.bRec) {
                    SetRecodeParams(MFX_BRC_BIG_FRAME,
                                    quant,
                                    quant_new,
                                    quantMin,
                                    quantMax,
                                    m_ctx,
                                    status);
                    return sts;
                }
            } //(quant_new > quant)
        } //bitsEncoded >  maxFrameSize

        if (bitsEncoded > maxFrameSize && quant == quantMax && picType != MFX_FRAMETYPE_I &&
            m_par.bPanic && (!m_ctx.bPanic) &&
            isFrameBeforeIntra(m_ctx.encOrder,
                               m_ctx.LastIEncOrder,
                               m_par.gopPicSize,
                               m_par.gopRefDist)) {
            //skip frames before intra
            SetRecodeParams(MFX_BRC_PANIC_BIG_FRAME,
                            quant,
                            quant,
                            quantMin,
                            quantMax,
                            m_ctx,
                            status);
            return sts;
        }
        if (m_par.HRDConformance != MFX_BRC_NO_HRD && frame_par->NumRecode == 0 &&
            (quant < quantMax)) {
            mfxF64 maxFrameSizeHrd =
                m_hrdSpec->GetMaxFrameSizeInBits(frame_par->EncodedOrder, bIntra);
            mfxF64 FAMax = 1. / 9. * maxFrameSizeHrd + 8. / 9. * fAbLong;

            if (fAbShort > FAMax) {
                mfxI32 quant_new =
                    GetNewQP(fAbShort, FAMax, quantMin, quantMax, quant, m_par.quantOffset, 0.5);
                //printf("    recode 2-0: %d:  FAMax %f, fAbShort %f, quant_new %d\n",frame_par->EncodedOrder, FAMax, fAbShort, quant_new);

                if (quant_new > quant) {
                    bNeedUpdateQP = false;
                    if (quant_new > GetCurQP(picType, layer)) {
                        UpdateQPParams(quant_new, picType, m_ctx, 0, quantMin, quantMax, layer);
                        fAbLong = m_ctx.fAbLong = m_par.inputBitsPerFrame;
                        fAbShort = m_ctx.fAbShort = m_par.inputBitsPerFrame;
                        m_ctx.dQuantAb            = 1. / quant_new;
                    }
                    if (m_par.bRec) {
                        SetRecodeParams(MFX_BRC_BIG_FRAME,
                                        quant,
                                        quant_new,
                                        quantMin,
                                        quantMax,
                                        m_ctx,
                                        status);
                        return sts;
                    }
                } //quant_new > quant
            }
        } //m_par.HRDConformance
    }
    if (((m_par.HRDConformance != MFX_BRC_NO_HRD && brcSts != MFX_BRC_OK) ||
         (bitsEncoded > (mfxI32)frameSizeLim)) &&
        m_par.bRec) {
        mfxI32 quant     = m_ctx.Quant;
        mfxI32 quant_new = quant;
        if (bitsEncoded > (mfxI32)frameSizeLim) {
            brcSts    = MFX_BRC_BIG_FRAME;
            quant_new = GetNewQP(bitsEncoded,
                                 frameSizeLim,
                                 m_ctx.QuantMin,
                                 m_ctx.QuantMax,
                                 quant,
                                 m_par.quantOffset,
                                 1,
                                 true);
        }
        else if (brcSts == MFX_BRC_BIG_FRAME || brcSts == MFX_BRC_SMALL_FRAME) {
            mfxF64 targetSize = GetFrameTargetSize(
                brcSts,
                m_hrdSpec->GetMinFrameSizeInBits(frame_par->EncodedOrder, bIntra),
                m_hrdSpec->GetMaxFrameSizeInBits(frame_par->EncodedOrder, bIntra));
            if (targetSize == 0) {
                return MFX_ERR_INVALID_VIDEO_PARAM;
            }

            quant_new = GetNewQP(bitsEncoded,
                                 targetSize,
                                 m_ctx.QuantMin,
                                 m_ctx.QuantMax,
                                 quant,
                                 m_par.quantOffset,
                                 1,
                                 true);
        }
        if (quant_new != quant) {
            if (brcSts == MFX_BRC_SMALL_FRAME) {
                quant_new = std::max(quant_new, quant - 2);
                brcSts    = MFX_BRC_PANIC_SMALL_FRAME;
            }
            // Idea is to check a sign mismatch, 'true' if both are negative or positive
            if ((quant_new - qpY) * (quant_new - GetCurQP(picType, layer)) > 0) {
                UpdateQPParams(quant_new, picType, m_ctx, 0, m_ctx.QuantMin, m_ctx.QuantMax, layer);
            }
            bNeedUpdateQP = false;
        }
        SetRecodeParams(brcSts, quant, quant_new, m_ctx.QuantMin, m_ctx.QuantMax, m_ctx, status);
    }
    else {
        // no recoding are needed. Save context params

        mfxF64 k          = 1. / m_ctx.Quant;
        mfxF64 dqAbPeriod = m_par.dqAbPeriod;
        if (m_ctx.bToRecode)
            dqAbPeriod = (k < m_ctx.dQuantAb) ? 16 : 25;

        if (bNeedUpdateQP) {
            m_ctx.dQuantAb += (k - m_ctx.dQuantAb) / dqAbPeriod;
            m_ctx.dQuantAb = mfx::clamp(m_ctx.dQuantAb, 1. / m_ctx.QuantMax, 1. / m_ctx.QuantMin);

            m_ctx.fAbLong  = fAbLong;
            m_ctx.fAbShort = fAbShort;
        }

        bool oldScene = false;
        if ((m_ctx.SceneChange & 16) && (m_ctx.poc < m_ctx.SChPoc) && (e2pe < .01) &&
            (mfxF64)bitsEncoded < 1.5 * fAbLong)
            oldScene = true;
        //printf("-- m_ctx.eRate %f,  eRate %f, e2pe %f\n", m_ctx.eRate,  eRate, e2pe );

        if (picType != MFX_FRAMETYPE_B) {
            m_ctx.LastNonBFrameSize = bitsEncoded;
            if (picType == MFX_FRAMETYPE_I)
                m_ctx.eRateSH = eRate;
            else
                m_ctx.eRate = eRate;
        }

        if (m_avg.get()) {
            m_avg->UpdateSlidingWindow(bitsEncoded,
                                       m_ctx.encOrder,
                                       m_ctx.bPanic,
                                       bSHStart || picType == MFX_FRAMETYPE_I,
                                       frame_par->NumRecode);
        }

        m_ctx.totalDeviation += ((mfxF64)bitsEncoded - m_par.inputBitsPerFrame);

        //printf("-- %d (%d)) Total deviation %f, old scene %d, bNeedUpdateQP %d, m_ctx.Quant %d, type %d\n", frame_par->EncodedOrder, frame_par->DisplayOrder,m_ctx.totalDeviation, oldScene , bNeedUpdateQP, m_ctx.Quant,picType);

        if (!m_ctx.bPanic && (!oldScene) && bNeedUpdateQP) {
            mfxI32 quant_new = m_ctx.Quant;
            //Update QP

            mfxF64 totDev          = m_ctx.totalDeviation;
            mfxF64 HRDDev          = 0.0;
            mfxF64 maxFrameSizeHrd = 0.0;
            if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
                HRDDev          = m_hrdSpec->GetBufferDeviation(frame_par->EncodedOrder);
                maxFrameSizeHrd = m_hrdSpec->GetMaxFrameSizeInBits(frame_par->EncodedOrder, bIntra);
                //printf("HRDDiv %f\n", HRDDiv);
            }
            mfxF64 dequant_new = m_ctx.dQuantAb * pow(m_par.inputBitsPerFrame / m_ctx.fAbLong, 1.2);
            mfxF64 bAbPreriod  = m_par.bAbPeriod;

            if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
                if (m_par.rateControlMethod == MFX_RATECONTROL_VBR &&
                    m_par.maxbps > m_par.targetbps) {
                    totDev = std::max(totDev, HRDDev);
                }
                else {
                    totDev = HRDDev;
                }
                if (totDev > 0) {
                    bAbPreriod =
                        (mfxF64)(m_par.bPyr ? 4 : 3) * (mfxF64)maxFrameSizeHrd / fAbShort *
                        GetAbPeriodCoeff(m_ctx.encOrder - m_ctx.LastIEncOrder, m_par.gopPicSize);
                    bAbPreriod = mfx::clamp(bAbPreriod, m_par.bAbPeriod / 10, m_par.bAbPeriod);
                }
            }
            quant_new = GetNewQPTotal(totDev / bAbPreriod / (mfxF64)m_par.inputBitsPerFrame,
                                      dequant_new,
                                      m_ctx.QuantMin,
                                      m_ctx.QuantMax,
                                      m_ctx.Quant,
                                      m_par.bPyr && m_par.bRec,
                                      bSHStart && m_ctx.bToRecode == 0);
            //printf("    ===%d quant old %d quant_new %d, bitsEncoded %d m_ctx.QuantMin %d m_ctx.QuantMax %d\n", frame_par->EncodedOrder, m_ctx.Quant, quant_new, bitsEncoded, m_ctx.QuantMin, m_ctx.QuantMax);

            if (bMaxFrameSizeMode) {
                mfxF64 targetMax = ((mfxF64)m_par.maxFrameSizeInBits *
                                    ((bSHStart || picType == MFX_FRAMETYPE_I) ? 0.95 : 0.9));
                mfxF64 targetMin =
                    ((mfxF64)m_par.maxFrameSizeInBits *
                     ((bSHStart || picType == MFX_FRAMETYPE_I) ? 0.9 : 0.8 /*0.75 : 0.5*/));
                mfxI32 QuantNewMin     = GetNewQP(bitsEncoded,
                                              targetMax,
                                              m_ctx.QuantMin,
                                              m_ctx.QuantMax,
                                              m_ctx.Quant,
                                              m_par.quantOffset,
                                              1,
                                              false,
                                              false);
                mfxI32 QuantNewMax     = GetNewQP(bitsEncoded,
                                              targetMin,
                                              m_ctx.QuantMin,
                                              m_ctx.QuantMax,
                                              m_ctx.Quant,
                                              m_par.quantOffset,
                                              1,
                                              false,
                                              false);
                mfxI32 quant_corrected = m_ctx.Quant;

                if (quant_corrected < QuantNewMin - 3)
                    quant_corrected += 2;
                if (quant_corrected < QuantNewMin)
                    quant_corrected++;
                else if (quant_corrected > QuantNewMax + 3)
                    quant_corrected -= 2;
                else if (quant_corrected > QuantNewMax)
                    quant_corrected--;

                //printf("   QuantNewMin %d, QuantNewMax %d, m_ctx.Quant %d, new %d (%d)\n", QuantNewMin, QuantNewMax, m_ctx.Quant, quant_corrected, quant_new);

                quant_new = mfx::clamp(quant_corrected, m_ctx.QuantMin, m_ctx.QuantMax);
            }
            if ((quant_new - m_ctx.Quant) * (quant_new - GetCurQP(picType, layer)) >
                0) // this check is actual for async scheme
            {
                //printf("   Update QP %d: totalDeviation %f, bAbPreriod %f (%f), QP %d (%d %d), qp_new %d (qpY %d), type %d, dequant_new %f (%f) , m_ctx.fAbLong %f, m_par.inputBitsPerFrame %f (%f)\n",frame_par->EncodedOrder,totDev , bAbPreriod, GetAbPeriodCoeff(m_ctx.encOrder - m_ctx.LastIEncOrder, m_par.gopPicSize), m_ctx.Quant, m_ctx.QuantMin, m_ctx.QuantMax,quant_new, qpY, picType, 1.0/dequant_new, 1.0/m_ctx.dQuantAb, m_ctx.fAbLong, m_par.inputBitsPerFrame, m_par.inputBitsPerFrame/m_ctx.fAbLong);
                UpdateQPParams(quant_new, picType, m_ctx, 0, m_ctx.QuantMin, m_ctx.QuantMax, layer);
            }
        }
        m_ctx.bToRecode = 0;
        if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
            m_hrdSpec->Update(bitsEncoded, frame_par->EncodedOrder, bIntra);
        }
    }
    return sts;
}

mfxStatus ExtBRC::GetFrameCtrl(mfxBRCFrameParam* par, mfxBRCFrameCtrl* ctrl) {
    MFX_CHECK_NULL_PTR2(par, ctrl);
    MFX_CHECK(m_bInit, MFX_ERR_NOT_INITIALIZED);

    mfxI32 qp = 0;
    if (par->EncodedOrder == m_ctx.encOrder) {
        qp = m_ctx.Quant;
    }
    else {
        mfxU16 type = GetFrameType(par->FrameType, par->PyramidLayer, m_par.gopRefDist);
        qp          = GetCurQP(type, par->PyramidLayer);
    }
    ctrl->QpY = qp - m_par.quantOffset;
    if (m_par.HRDConformance != MFX_BRC_NO_HRD) {
        ctrl->InitialCpbRemovalDelay  = m_hrdSpec->GetInitCpbRemovalDelay(par->EncodedOrder);
        ctrl->InitialCpbRemovalOffset = m_hrdSpec->GetInitCpbRemovalDelayOffset(par->EncodedOrder);
    }
    if (m_par.mMBBRC) {
        if (ctrl->NumExtParam == 0) {
            //attach MBBRC buffer
            ctrl->NumExtParam = 1;
            ctrl->ExtParam    = &(m_ExtBuff[par->EncodedOrder % m_ExtBuff.size()]);
        }
        mfxExtMBQP* pExtMBQP =
            (mfxExtMBQP*)Hevc_GetExtBuffer(ctrl->ExtParam, ctrl->NumExtParam, MFX_EXTBUFF_MBQP);
        if (pExtMBQP) {
            //fill QP map
            for (size_t i = 0; i < pExtMBQP->NumQPAlloc; i++) {
                pExtMBQP->QP[i] = (mfxU8)(qp + ((qp < 51) ? (i % 2) : 0));
            }
        }
    }
    return MFX_ERR_NONE;
}

mfxStatus ExtBRC::Reset(mfxVideoParam* par) {
    mfxStatus sts = MFX_ERR_NONE;
    MFX_CHECK_NULL_PTR1(par);
    MFX_CHECK(m_bInit, MFX_ERR_NOT_INITIALIZED);

    mfxExtEncoderResetOption* pRO =
        (mfxExtEncoderResetOption*)Hevc_GetExtBuffer(par->ExtParam,
                                                     par->NumExtParam,
                                                     MFX_EXTBUFF_ENCODER_RESET_OPTION);
    if (pRO && pRO->StartNewSequence == MFX_CODINGOPTION_ON) {
        Close();
        sts = Init(par);
    }
    else {
        bool brcReset           = false;
        bool slidingWindowReset = false;

        sts = m_par.GetBRCResetType(par, false, brcReset, slidingWindowReset);
        MFX_CHECK_STS(sts);

        if (brcReset) {
            sts = m_par.Init(par, isFieldMode(par));
            MFX_CHECK_STS(sts);

            m_ctx.Quant = (mfxI32)(
                1. / m_ctx.dQuantAb * pow(m_ctx.fAbLong / m_par.inputBitsPerFrame, 0.32) + 0.5);
            m_ctx.Quant = mfx::clamp(m_ctx.Quant, m_par.quantMinI, m_par.quantMaxI);

            UpdateQPParams(m_ctx.Quant,
                           MFX_FRAMETYPE_I,
                           m_ctx,
                           0,
                           m_par.quantMinI,
                           m_par.quantMaxI,
                           0);

            m_ctx.dQuantAb = 1. / m_ctx.Quant;
            m_ctx.fAbLong  = m_par.inputBitsPerFrame;
            m_ctx.fAbShort = m_par.inputBitsPerFrame;

            if (slidingWindowReset) {
                m_avg.reset(
                    new AVGBitrate(m_par.WinBRCSize,
                                   (mfxU32)(m_par.WinBRCMaxAvgKbps * 1000.0 / m_par.frameRate),
                                   (mfxU32)m_par.inputBitsPerFrame));
                MFX_CHECK_NULL_PTR1(m_avg.get());
            }
        }
    }
    return sts;
}

void HEVC_HRD::Init(cBRCParams& par) {
    m_hrdInput.Init(par);
    m_prevAuCpbRemovalDelayMinus1 = -1;
    m_prevAuCpbRemovalDelayMsb    = 0;
    m_prevAuFinalArrivalTime      = 0;
    m_prevBpAuNominalRemovalTime  = (mfxU32)m_hrdInput.m_initCpbRemovalDelay;
    m_prevBpEncOrder              = 0;
}

void HEVC_HRD::Reset(cBRCParams& par) {
    sHrdInput hrdInput;
    hrdInput.Init(par);
    m_hrdInput.m_bitrate    = hrdInput.m_bitrate;
    m_hrdInput.m_cpbSize90k = hrdInput.m_cpbSize90k;
}

void HEVC_HRD::Update(mfxU32 sizeInbits, mfxU32 eo, bool bSEI) {
    mfxF64 auNominalRemovalTime = 0.0;
    mfxF64 initCpbRemovalDelay  = GetInitCpbRemovalDelay(eo);
    if (eo > 0) {
        mfxU32 auCpbRemovalDelayMinus1 = (eo - m_prevBpEncOrder) - 1;
        // (D-1)
        mfxU32 auCpbRemovalDelayMsb = 0;

        if (!bSEI && (eo - m_prevBpEncOrder) != 1) {
            auCpbRemovalDelayMsb =
                ((mfxI32)auCpbRemovalDelayMinus1 <= m_prevAuCpbRemovalDelayMinus1)
                    ? m_prevAuCpbRemovalDelayMsb + m_hrdInput.m_maxCpbRemovalDelay
                    : m_prevAuCpbRemovalDelayMsb;
        }

        m_prevAuCpbRemovalDelayMsb    = auCpbRemovalDelayMsb;
        m_prevAuCpbRemovalDelayMinus1 = auCpbRemovalDelayMinus1;

        // (D-2)
        mfxU32 auCpbRemovalDelayValMinus1 = auCpbRemovalDelayMsb + auCpbRemovalDelayMinus1;
        // (C-10, C-11)
        auNominalRemovalTime = m_prevBpAuNominalRemovalTime +
                               m_hrdInput.m_clockTick * (auCpbRemovalDelayValMinus1 + 1);
    }
    else // (C-9)
        auNominalRemovalTime = m_hrdInput.m_initCpbRemovalDelay;

    // (C-3)
    mfxF64 initArrivalTime = m_prevAuFinalArrivalTime;

    if (!m_hrdInput.m_cbrFlag) {
        mfxF64 initArrivalEarliestTime = (bSEI)
                                             // (C-7)
                                             ? auNominalRemovalTime - initCpbRemovalDelay
                                             // (C-6)
                                             : auNominalRemovalTime - m_hrdInput.m_cpbSize90k;
        // (C-4)
        initArrivalTime = std::max<mfxF64>(m_prevAuFinalArrivalTime,
                                           initArrivalEarliestTime * m_hrdInput.m_bitrate);
    }
    // (C-8)
    mfxF64 auFinalArrivalTime = initArrivalTime + (mfxF64)sizeInbits * 90000;

    m_prevAuFinalArrivalTime = auFinalArrivalTime;

    if (bSEI) {
        m_prevBpAuNominalRemovalTime = auNominalRemovalTime;
        m_prevBpEncOrder             = eo;
    }
}

mfxU32 HEVC_HRD::GetInitCpbRemovalDelay(mfxU32 eo) const {
    mfxF64 auNominalRemovalTime;

    if (eo > 0) {
        // (D-1)
        mfxU32 auCpbRemovalDelayMsb    = 0;
        mfxU32 auCpbRemovalDelayMinus1 = eo - m_prevBpEncOrder - 1;

        // (D-2)
        mfxU32 auCpbRemovalDelayValMinus1 = auCpbRemovalDelayMsb + auCpbRemovalDelayMinus1;
        // (C-10, C-11)
        auNominalRemovalTime = m_prevBpAuNominalRemovalTime +
                               m_hrdInput.m_clockTick * (auCpbRemovalDelayValMinus1 + 1);

        // (C-17)
        mfxF64 deltaTime90k =
            auNominalRemovalTime - m_prevAuFinalArrivalTime / m_hrdInput.m_bitrate;

        return (m_hrdInput.m_cbrFlag
                    // (C-19)
                    ? (mfxU32)(deltaTime90k)
                    // (C-18)
                    : (mfxU32)std::min(deltaTime90k, m_hrdInput.m_cpbSize90k));
    }

    return (mfxU32)m_hrdInput.m_initCpbRemovalDelay;
}

inline mfxF64 GetTargetDelay(mfxF64 cpbSize90k, mfxF64 initCpbRemovalDelay, bool bVBR) {
    return bVBR ? std::max(std::min(3.0 * cpbSize90k / 4.0, initCpbRemovalDelay), cpbSize90k / 2.0)
                : std::min(cpbSize90k / 2.0, initCpbRemovalDelay);
}
mfxF64 HEVC_HRD::GetBufferDeviation(mfxU32 eo) const {
    mfxU32 delay       = GetInitCpbRemovalDelay(eo);
    mfxF64 targetDelay = GetTargetDelay(m_hrdInput.m_cpbSize90k,
                                        m_hrdInput.m_initCpbRemovalDelay,
                                        !m_hrdInput.m_cbrFlag);
    return (targetDelay - delay) / 90000.0 * m_hrdInput.m_bitrate;
}

mfxF64 HEVC_HRD::GetBufferDeviationFactor(mfxU32 eo) const {
    mfxU32 delay       = GetInitCpbRemovalDelay(eo);
    mfxF64 targetDelay = GetTargetDelay(m_hrdInput.m_cpbSize90k,
                                        m_hrdInput.m_initCpbRemovalDelay,
                                        !m_hrdInput.m_cbrFlag);
    return abs((targetDelay - delay) / targetDelay);
}

mfxU32 HEVC_HRD::GetMaxFrameSizeInBits(mfxU32 eo, bool /*bSEI*/) const {
    return (mfxU32)(GetInitCpbRemovalDelay(eo) / 90000.0 * m_hrdInput.m_bitrate);
}

mfxU32 HEVC_HRD::GetMinFrameSizeInBits(mfxU32 eo, bool /*bSEI*/) const {
    mfxU32 delay = GetInitCpbRemovalDelay(eo);
    if ((!m_hrdInput.m_cbrFlag) ||
        ((delay + m_hrdInput.m_clockTick + 16.0) < m_hrdInput.m_cpbSize90k))
        return 0;
    return (mfxU32)((delay + m_hrdInput.m_clockTick + 16.0 - m_hrdInput.m_cpbSize90k) / 90000.0 *
                        m_hrdInput.m_bitrate +
                    0.99999);
}

H264_HRD::H264_HRD() : m_trn_cur(0), m_taf_prv(0) {}

void H264_HRD::Init(cBRCParams& par) {
    m_hrdInput.Init(par);
    m_hrdInput.m_clockTick *= (1.0 / 90000.0);
    m_taf_prv = 0.0;
    m_trn_cur = m_hrdInput.m_initCpbRemovalDelay / 90000.0;
    m_trn_cur = GetInitCpbRemovalDelay(0) / 90000.0;
}

void H264_HRD::Reset(cBRCParams& par) {
    sHrdInput hrdInput;
    hrdInput.Init(par);
    m_hrdInput.m_bitrate    = hrdInput.m_bitrate;
    m_hrdInput.m_cpbSize90k = hrdInput.m_cpbSize90k;
}

void H264_HRD::Update(mfxU32 sizeInbits, mfxU32 eo, bool bSEI) {
    //const bool interlace = false; //BRC is frame level only
    mfxU32 initDelay = GetInitCpbRemovalDelay(eo);

    double tai_earliest =
        bSEI ? m_trn_cur - (initDelay / 90000.0) : m_trn_cur - (m_hrdInput.m_cpbSize90k / 90000.0);

    double tai_cur = (!m_hrdInput.m_cbrFlag) ? std::max(m_taf_prv, tai_earliest) : m_taf_prv;

    m_taf_prv = tai_cur + (mfxF64)sizeInbits / m_hrdInput.m_bitrate;
    m_trn_cur += m_hrdInput.m_clockTick;
}

mfxU32 H264_HRD::GetInitCpbRemovalDelay(mfxU32 /* eo */) const {
    double delay                  = std::max(0.0, m_trn_cur - m_taf_prv);
    mfxU32 initialCpbRemovalDelay = mfxU32(90000 * delay + 0.5);

    return (mfxU32)(initialCpbRemovalDelay == 0
                        ? 1 // should not be equal to 0
                        : initialCpbRemovalDelay > m_hrdInput.m_cpbSize90k &&
                                  (!m_hrdInput.m_cbrFlag)
                              ? m_hrdInput.m_cpbSize90k // should not exceed hrd buffer
                              : initialCpbRemovalDelay);
}
mfxF64 H264_HRD::GetBufferDeviation(mfxU32 eo) const {
    mfxU32 delay       = GetInitCpbRemovalDelay(eo);
    mfxF64 targetDelay = GetTargetDelay(m_hrdInput.m_cpbSize90k,
                                        m_hrdInput.m_initCpbRemovalDelay,
                                        !m_hrdInput.m_cbrFlag);
    return (targetDelay - delay) / 90000.0 * m_hrdInput.m_bitrate;
}
mfxF64 H264_HRD::GetBufferDeviationFactor(mfxU32 eo) const {
    mfxU32 delay       = GetInitCpbRemovalDelay(eo);
    mfxF64 targetDelay = GetTargetDelay(m_hrdInput.m_cpbSize90k,
                                        m_hrdInput.m_initCpbRemovalDelay,
                                        !m_hrdInput.m_cbrFlag);
    return abs((targetDelay - delay) / targetDelay);
}

mfxU32 H264_HRD::GetInitCpbRemovalDelayOffset(mfxU32 eo) const {
    // init_cpb_removal_delay + init_cpb_removal_delay_offset should be constant
    return mfxU32(m_hrdInput.m_cpbSize90k - GetInitCpbRemovalDelay(eo));
}
mfxU32 H264_HRD::GetMinFrameSizeInBits(mfxU32 eo, bool /*bSEI*/) const {
    mfxU32 delay = GetInitCpbRemovalDelay(eo);
    if ((!m_hrdInput.m_cbrFlag) ||
        ((delay + m_hrdInput.m_clockTick * 90000) < m_hrdInput.m_cpbSize90k))
        return 0;

    return (mfxU32)((delay + m_hrdInput.m_clockTick * 90000.0 - m_hrdInput.m_cpbSize90k) / 90000.0 *
                    m_hrdInput.m_bitrate) +
           16;
}
mfxU32 H264_HRD::GetMaxFrameSizeInBits(mfxU32 eo, bool bSEI) const {
    mfxU32 initDelay = GetInitCpbRemovalDelay(eo);

    double tai_earliest = (bSEI) ? m_trn_cur - (initDelay / 90000.0)
                                 : m_trn_cur - (m_hrdInput.m_cpbSize90k / 90000.0);

    double tai_cur = (!m_hrdInput.m_cbrFlag) ? std::max(m_taf_prv, tai_earliest) : m_taf_prv;

    mfxU32 maxFrameSize = (mfxU32)((m_trn_cur - tai_cur) * m_hrdInput.m_bitrate);

    return maxFrameSize;
}
void sHrdInput::Init(cBRCParams par) {
    m_cbrFlag             = (par.rateControlMethod == MFX_RATECONTROL_CBR);
    m_bitrate             = par.maxbps;
    m_maxCpbRemovalDelay  = 1 << (h264_h265_au_cpb_removal_delay_length_minus1 + 1);
    m_clockTick           = 90000. / par.frameRate;
    m_cpbSize90k          = mfxU32(90000. * par.bufferSizeInBytes * 8.0 / m_bitrate);
    m_initCpbRemovalDelay = 90000. * 8. * par.initialDelayInBytes / m_bitrate;
}
