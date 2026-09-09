# Copyright (c) 2010, 2017, 2020, 2024-2025 Arm Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2006-2007 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.defines import buildEnv
from m5.objects.FuncUnit import *
from m5.params import *
from m5.SimObject import SimObject


class IntALU(FUDesc):
    opList = [
        OpDesc(opClass="IntAlu"),
    ]
    count = 6


class IntMultDiv(FUDesc):
    opList = [
        OpDesc(opClass="IntMult", opLat=3),
        OpDesc(opClass="IntDiv", opLat=20, pipelined=False),
    ]

    count = 2


class FP_ALU(FUDesc):
    opList = [
        OpDesc(opClass="FloatAdd", opLat=2),
        OpDesc(opClass="FloatCmp", opLat=2),
        OpDesc(opClass="FloatCvt", opLat=2),
        OpDesc(opClass="Bf16Cvt", opLat=2),
    ]
    count = 4


class FP_MultDiv(FUDesc):
    opList = [
        OpDesc(opClass="FloatMult", opLat=4),
        OpDesc(opClass="FloatMultAcc", opLat=5),
        OpDesc(opClass="FloatMisc", opLat=3),
        OpDesc(opClass="FloatDiv", opLat=12, pipelined=False),
        OpDesc(opClass="FloatSqrt", opLat=24, pipelined=False),
    ]
    count = 2


class SIMD_Unit(FUDesc):
    opList = [
        OpDesc(opClass="SimdAdd"),
        OpDesc(opClass="SimdAddAcc"),
        OpDesc(opClass="SimdAlu"),
        OpDesc(opClass="SimdCmp"),
        OpDesc(opClass="SimdCvt"),
        OpDesc(opClass="SimdMisc"),
        OpDesc(opClass="SimdMult"),
        OpDesc(opClass="SimdMultAcc"),
        OpDesc(opClass="SimdMatMultAcc"),
        OpDesc(opClass="SimdShift"),
        OpDesc(opClass="SimdShiftAcc"),
        OpDesc(opClass="SimdDiv"),
        OpDesc(opClass="SimdSqrt"),
        OpDesc(opClass="SimdFloatAdd"),
        OpDesc(opClass="SimdFloatAlu"),
        OpDesc(opClass="SimdFloatCmp"),
        OpDesc(opClass="SimdFloatCvt"),
        OpDesc(opClass="SimdFloatDiv"),
        OpDesc(opClass="SimdFloatMisc"),
        OpDesc(opClass="SimdFloatMult"),
        OpDesc(opClass="SimdFloatMultAcc"),
        OpDesc(opClass="SimdFloatMatMultAcc"),
        OpDesc(opClass="SimdFloatSqrt"),
        OpDesc(opClass="SimdReduceAdd"),
        OpDesc(opClass="SimdReduceAlu"),
        OpDesc(opClass="SimdReduceCmp"),
        OpDesc(opClass="SimdFloatReduceAdd"),
        OpDesc(opClass="SimdFloatReduceCmp"),
        OpDesc(opClass="SimdExt"),
        OpDesc(opClass="SimdFloatExt"),
        OpDesc(opClass="SimdConfig"),
        OpDesc(opClass="SimdDotProd"),
        OpDesc(opClass="SimdAes"),
        OpDesc(opClass="SimdAesMix"),
        OpDesc(opClass="SimdSha1Hash"),
        OpDesc(opClass="SimdSha1Hash2"),
        OpDesc(opClass="SimdSha256Hash"),
        OpDesc(opClass="SimdSha256Hash2"),
        OpDesc(opClass="SimdShaSigma2"),
        OpDesc(opClass="SimdShaSigma3"),
        OpDesc(opClass="SimdSha3"),
        OpDesc(opClass="SimdSm4e"),
        OpDesc(opClass="SimdCrc"),
        OpDesc(opClass="SimdBf16Add"),
        OpDesc(opClass="SimdBf16Cmp"),
        OpDesc(opClass="SimdBf16Cvt"),
        OpDesc(opClass="SimdBf16DotProd"),
        OpDesc(opClass="SimdBf16MatMultAcc"),
        OpDesc(opClass="SimdBf16Mult"),
        OpDesc(opClass="SimdBf16MultAcc"),
    ]
    count = 4


class Matrix_Unit(FUDesc):
    opList = [
        OpDesc(opClass="Matrix"),
        OpDesc(opClass="MatrixMov"),
        OpDesc(opClass="MatrixOP"),
    ]
    count = 2


# CFD-DSA Matrix Engine: handles DSADotpRow instructions.
# Keep count=1 as default to model a single pipelined dot-product engine.
class CFD_Matrix_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSADotp", opLat=7, pipelined=True),
    ]
    count = 1  # Single pipelined engine


class CFD_DSA_STREAM_LOAD_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSAStreamLd", opLat=1, pipelined=True),
    ]
    count = 1


# CFD-DSA Pack Engine: harvests Path A internal/X accumulator state into Z.
class CFD_DSA_PACK_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSAPack", opLat=1, pipelined=True),
    ]
    count = 1


class CFD_DSA_STORE5_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSAStore5", opLat=1, pipelined=True),
    ]
    count = 1


# CFD-DSA TRSV5 engine.  The first Step2 model computes the full FP64
# 5x5 triangular solve in execute() and uses opLat as a coarse-grain
# O3-visible result latency.
class CFD_DSA_TRSV5_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSATrsv5", opLat=60, pipelined=True),
    ]
    count = 1


class CFD_DSA_TRSM5_MRHS_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSATrsm5Mrhs", opLat=100, pipelined=True),
    ]
    count = 1


class CFD_DSA_TRSM5_INV_LBAR_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSATrsm5InvLbar", opLat=200, pipelined=True),
    ]
    count = 1


class CFD_DSA_TRSM5_COEFF3_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSATrsm5Coeff3", opLat=300, pipelined=True),
    ]
    count = 1


class CFD_DSA_VEC5_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSAVec5", opLat=4, pipelined=True),
    ]
    count = 1


class CFD_DSA_LINEBUF_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSALineBuf", opLat=1, pipelined=True),
    ]
    count = 1


class CFD_DSA_LUSGS_CTRL_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDDSALusgsCtrl", opLat=1, pipelined=False),
    ]
    count = 1


# CFD-SME restricted FMOPA engine. Each instruction models one strict
# outer-product k-step with five effective FP64 FMA updates to ZA[0:4,0].
class CFD_SME_FMOPA_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDSMEFmopaStep", opLat=4, pipelined=True),
    ]
    count = 1


# Path B ZA spatial partial-sum pipeline.  Each instruction advances one
# 5-lane FMA stage and writes the next ZA column.
class CFD_SME_PIPE_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDSMEPipeStep", opLat=1, pipelined=True),
    ]
    count = 1


# Path C selected-column outer-product spatial pipeline.  Each instruction
# conceptually forms one outer-product column and advances the ZA column
# partial-sum pipeline by one stage.
class CFD_SME_ZA_OUTER_Engine(FUDesc):
    opList = [
        OpDesc(opClass="CFDSMEZaOuterStep", opLat=1, pipelined=True),
    ]
    count = 1


class System_Unit(FUDesc):
    opList = [OpDesc(opClass="System")]
    count = 1


class PredALU(FUDesc):
    opList = [OpDesc(opClass="SimdPredAlu")]
    count = 1


class ReadPort(FUDesc):
    opList = [
        OpDesc(opClass="MemRead"),
        OpDesc(opClass="FloatMemRead"),
        OpDesc(opClass="SimdUnitStrideLoad"),
        OpDesc(opClass="SimdUnitStrideMaskLoad"),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad"),
        OpDesc(opClass="SimdStridedLoad"),
        OpDesc(opClass="SimdIndexedLoad"),
        OpDesc(opClass="SimdUnitStrideFaultOnlyFirstLoad"),
        OpDesc(opClass="SimdUnitStrideSegmentedFaultOnlyFirstLoad"),
        OpDesc(opClass="SimdWholeRegisterLoad"),
        OpDesc(opClass="SimdStrideSegmentedLoad"),
        OpDesc(opClass="CFDDSAMatLd", opLat=1, pipelined=True),
    ]
    count = 8


class WritePort(FUDesc):
    opList = [
        OpDesc(opClass="MemWrite"),
        OpDesc(opClass="FloatMemWrite"),
        OpDesc(opClass="SimdUnitStrideStore"),
        OpDesc(opClass="SimdUnitStrideMaskStore"),
        OpDesc(opClass="SimdUnitStrideSegmentedStore"),
        OpDesc(opClass="SimdStridedStore"),
        OpDesc(opClass="SimdIndexedStore"),
        OpDesc(opClass="SimdWholeRegisterStore"),
        OpDesc(opClass="SimdStrideSegmentedStore"),
    ]
    count = 0


class RdWrPort(FUDesc):
    opList = [
        OpDesc(opClass="MemRead"),
        OpDesc(opClass="MemWrite"),
        OpDesc(opClass="FloatMemRead"),
        OpDesc(opClass="FloatMemWrite"),
        OpDesc(opClass="SimdUnitStrideLoad"),
        OpDesc(opClass="SimdUnitStrideStore"),
        OpDesc(opClass="SimdUnitStrideMaskLoad"),
        OpDesc(opClass="SimdUnitStrideMaskStore"),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad"),
        OpDesc(opClass="SimdUnitStrideSegmentedStore"),
        OpDesc(opClass="SimdStridedLoad"),
        OpDesc(opClass="SimdStridedStore"),
        OpDesc(opClass="SimdIndexedLoad"),
        OpDesc(opClass="SimdIndexedStore"),
        OpDesc(opClass="SimdUnitStrideFaultOnlyFirstLoad"),
        OpDesc(opClass="SimdUnitStrideSegmentedFaultOnlyFirstLoad"),
        OpDesc(opClass="SimdWholeRegisterLoad"),
        OpDesc(opClass="SimdWholeRegisterStore"),
        OpDesc(opClass="SimdStrideSegmentedLoad"),
        OpDesc(opClass="SimdStrideSegmentedStore"),
    ]
    count = 4
