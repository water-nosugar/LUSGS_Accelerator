# CFD-DSA 第一种方法：dotp-row + pack_acc Path A 技术文档

> 文档版本：v13.3（2026-07-05 CFD-DSA 项目与结果目录规范化）
> gem5 版本：gem5-stable / ARM SE O3
> 目标架构：ARMv8-A AArch64 + SVE
> 当前状态：Path A 保留 baseline dotp-row 路径，并新增 Path A-only streaming DSA：`patha_stream_ld5 -> patha_dotp_stream -> patha_store5_result`。该模式不改变公共 `lmat5_spm`，不影响第二种方法 Path C。LU-SGS Step4-C 已在 controller 内完成 Path A、TRSV5、Vector5 三类 event resource graph；Path A 主线语义未改变。项目 runner、benchmark、文档、binary 和 result 已分层管理，规范见 `RESULT_MANAGEMENT.md`。

---

## 目录

1. [概述](#1-概述)
2. [Path A 文件总览](#2-path-a-文件总览)
3. [Path A 指令集定义与编码](#3-path-a-指令集定义与编码)
4. [执行语义](#4-执行语义)
5. [X16-X20 与 internal result buffer](#5-x16-x20-与-internal-result-buffer)
6. [Path A streaming DSA](#6-path-a-streaming-dsa)
7. [SPM compact40 / padded64](#7-spm-compact40--padded64)
8. [result store full64 / pred40 / store5](#8-result-store-full64--pred40--store5)
9. [projects/cfd_dsa/configs/run_cfd_patha.py 参数说明](#9-run_cfd_pathapy-参数说明)
10. [projects/cfd_dsa/benchmarks/patha/test_cfd_patha_extreme_perf.c benchmark](#10-test_cfd_patha_extreme_perfc-benchmark)
11. [性能结果与统计口径](#11-性能结果与统计口径)
12. [已知问题与后续优化](#12-已知问题与后续优化)
13. [附录](#13-附录)

---

## 1. 概述

本文档只记录 CFD-DSA 第一种方法 Path A：

```text
lmat5_spm + dotp_row + pack_acc
```

Path A 是固定 5x5 FP64 matrix-vector multiply 的 dotp-row 性能路径。它的目标是降低指令数和显式内存访问开销，同时让 5 行 dot product 能在 O3 后端中尽量并行 issue。本轮硬件友好主优化目标是：

```text
compact40 direct + pred40
```

也就是 SPM input 精确 40B physical read，result output 精确 40B predicated store。当前实现还提供 Path A-only streaming DSA 模型，用于解释更接近硬件实现的流水结构：

```text
PathA stream load engine      patha_stream_ld5, 40B -> PathA input buffer
single dotp engine            patha_dotp_stream, dotp_count=1
internal result buffer        5 lane result queue
async 40B store engine        patha_store5_result + one final drain
benchmark unroll              true 8 matvec / loop
```

该模式优化的是物理带宽、payload utilization、producer/consumer overlap 和硬件解释清晰度。`dotp_count=1` 仍是默认单 dotp engine 流片方向；`dotp_count=2` 只保留为 balanced optional config。`dotp_lat=7` 是保守结果路径，`dotp_lat=5` 表示优化后的 5-lane row-dot pipeline，用于 streaming DSA 主性能点。full64 store、padded64 physical read、pred40_stride64 仍保留为对照。

每个 matvec 的 steady-state 指令结构：

```text
6 x lmat5_spm
5 x dotp_row
1 x pack_acc
1 x result store
```

其中 6 条 `lmat5_spm` 包含 5 行矩阵和 1 个向量：

```text
5 x 40B matrix rows + 1 x 40B vector = 240B logical SPM read / matvec
```

第二种方法 SME-ZA Path C 的完整说明见 `CFD_DSA_SME.md`。LU-SGS 分阶段说明见 `CFD_DSA_LUSGS_STEP1.md`、`CFD_DSA_LUSGS_STEP2.md`、`CFD_DSA_LUSGS_STEP3.md`、`CFD_DSA_LUSGS_STEP4.md`。

---

## 2. Path A 文件总览

### 2.1 推荐入口

| 角色 | 文件 | 说明 |
| --- | --- | --- |
| Path A run script | `projects/cfd_dsa/configs/run_cfd_patha.py` | Path A 专用运行入口和报告 focus |
| Path A benchmark C | `projects/cfd_dsa/benchmarks/patha/test_cfd_patha_extreme_perf.c` | 第一种方法 benchmark |
| Path A binary | `build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm` | 推荐输出 binary |
| Path A sensitivity sweep | `projects/cfd_dsa/tools/run_patha_dotp_sensitivity.py` | 可选 dotp latency/count sweep，不作为主文档展开 |
| 兼容入口 | `projects/cfd_dsa/configs/run_cfd_dsa.py` | deprecated / compatibility wrapper |
| 历史 benchmark | `projects/cfd_dsa/benchmarks/patha/test_cfd_extreme_perf.c` | 旧文件名，仅作为兼容保留 |

### 2.2 ISA / execute 相关文件

| 文件 | Path A 相关内容 |
| --- | --- |
| `src/arch/arm/isa/formats/custom_cfd.isa` | `lmat5_spm`、`dotp_row`、`pack_acc` 解码 |
| `src/arch/arm/insts/cfd_dsa.hh` | Path A 指令类声明 |
| `src/arch/arm/insts/cfd_dsa.cc` | Path A 指令执行语义 |
| `src/cpu/op_class.hh` | `CFDDSAMatLd`、`CFDDSADotp`、`CFDDSAPack` |
| `src/cpu/FuncUnit.py` | OpClass 暴露到 Python |
| `src/cpu/o3/FuncUnitConfig.py` | Path A FU 配置 |
| `src/cpu/o3/FUPool.py` | O3 FU pool 注册 |

---

## 3. Path A 指令集定义与编码

Path A 使用 ARM 保留编码空间：

```text
[31:27] = 00000
[26:25] = 00
```

### 3.1 lmat5_spm

语义：

```text
lmat5_spm zd, [xn, #row * 40]
```

字段：

```text
zd       = vector destination register
xn       = runtime base address register
row      = 0..5 style immediate row selector
payload  = 5 x FP64 = 40B
```

地址必须来自运行时 `X[xn]`：

```text
EA = X[xn] + row * 40
```

不得硬编码基址寄存器编号。

### 3.2 dotp_row

语义：

```text
dotp_row zrow, zvec, #lane
```

执行一行 5 元 FP64 dot product：

```text
sum = zrow[0] * zvec[0]
    + zrow[1] * zvec[1]
    + zrow[2] * zvec[2]
    + zrow[3] * zvec[3]
    + zrow[4] * zvec[4]
```

结果不直接写回 Z 寄存器，而是按 lane 写入跨域整数寄存器 `X16-X20`。

### 3.3 pack_acc

语义：

```text
pack_acc zd
```

默认 `internal-buffer` 模式下，`pack_acc` 从 Path A 内部 result buffer 收割 5 个 FP64 lane，打包到目标 Z 寄存器：

```text
Z[zd][0:4] = result_buffer[slot].lane[0:4]
Z[zd][5:7] = 0
```

`xregs` 兼容模式下，`pack_acc` 仍从 `X16-X20` 读取 bit pattern：

```text
Z[zd][0:4] = bitcast_fp64(X16-X20)
Z[zd][5:7] = 0
```

两种模式都保留 X16-X20 作为 O3 依赖 token，使旧回归和新内部 buffer 模型共存。

`pack_acc` 当前使用独立 OpClass / FU：

```text
OpClass = CFDDSAPack
FU      = CFD_DSA_PACK_Engine
opLat   = 1
count   = 1 默认，可用 --patha-pack-count sweep
```

---

## 4. 执行语义

Path A 计算标准 5x5 MVM：

```text
y = A x
```

row-major 矩阵布局下：

```text
y[i] = sum_k A[i][k] * x[k], i = 0..4
```

单个 matvec 的指令数据流：

```text
lmat5_spm z0, [matrix_base, row=0]
lmat5_spm z1, [matrix_base, row=1]
lmat5_spm z2, [matrix_base, row=2]
lmat5_spm z3, [matrix_base, row=3]
lmat5_spm z4, [matrix_base, row=4]
lmat5_spm z5, [vector_base, row=0]

dotp_row z0, z5, #0 -> X16
dotp_row z1, z5, #1 -> X17
dotp_row z2, z5, #2 -> X18
dotp_row z3, z5, #3 -> X19
dotp_row z4, z5, #4 -> X20

pack_acc z11
store z11
```

Ping/Pong 双缓冲使用：

| 角色 | Ping | Pong |
| --- | --- | --- |
| matrix rows | `Z0-Z4` | `Z6-Z10` |
| vector | `Z5` | `Z12` |
| result | `Z11` | `Z13` |
| matrix base | `X21` | `X22` |
| vector base | `X23` | `X24` |

---

## 5. X16-X20 与 internal result buffer

Path A 早期版本使用单个隐藏 accumulator 时，5 条 dot product 容易形成写后读或重命名不可见的依赖。当前实现保留 `xregs` 兼容路径，但默认切换到更接近硬件 DSA 的 internal result buffer，并让该 buffer 在 true unroll8 kernel 中真实形成 producer/consumer overlap。

### 5.1 xregs 兼容模式

`--patha-acc-mode=xregs` 使用固定整数寄存器保存每一行结果：

| 输出 lane | 保存寄存器 |
| --- | --- |
| y[0] | `X16` |
| y[1] | `X17` |
| y[2] | `X18` |
| y[3] | `X19` |
| y[4] | `X20` |

好处：

- 5 条 `dotp_row` 的目标寄存器不同，O3 可独立调度。
- `pack_acc` 显式读取 `X16-X20`，依赖关系清楚。
- 结果写入 Z 之前不需要隐藏微架构状态。

代价：

- `dotp_row` 是 FP64 计算但写整数寄存器，属于跨域建模。
- `pack_acc` 虽已有独立 FU，但数据仍绕到整数寄存器，硬件解释不如内部结果 buffer 直接。

### 5.2 internal-buffer 默认模式

`--patha-acc-mode=internal-buffer` 是当前默认模式：

```text
dotp_row lane i:
    compute row dot product
    write result_buffer_fifo[slot].back().lane[i]
    write X16+i as O3 dependency token

pack_acc:
    read result_buffer_fifo[slot].front().lane[0:4]
    pack into Z11/Z13
    pop result buffer FIFO head
```

默认 slot 配置：

```text
--patha-result-buffer-depth=2
slot 0 = Ping
slot 1 = Pong
depth 是每个 slot 的 FIFO 深度，而不是全局 live-entry 上限
```

true unroll8 overlap kernel 中，dotp producer 可以在前一个 matvec 结果尚未 pack/store 前继续写入另一个 FIFO entry，`pack_acc` 从 FIFO head 消费最老完整结果。这样 internal buffer 不再只是 xregs fallback 的替代表达，而是真正建模小型 DSA result queue。

当前默认 `depth=2` 的观测结果：

```text
live max = 2
full stalls = 0
overlap events > 0
pack-while-live events > 0
```

`depth=1` 会退化到顺序 unroll8 fallback，不能形成 live overlap；`depth=4` 对当前单 dotp engine steady workload 无额外收益。internal-buffer stats 由 `system.cpu.local_spm` 统计组输出：

```text
pathAResultBufferAllocs
pathAResultBufferFrees
pathAResultBufferWrites
pathAResultBufferPacks
pathAResultBufferFullStalls
pathAResultBufferLiveMax
pathAResultBufferLiveEnd
pathAResultBufferOverlapCycles
pathAResultBufferPackWhileDotpCycles
```

注意：当前 internal-buffer 是 Path A steady benchmark 的硬件模型，不把中间结果暴露为架构状态。X16-X20 在该模式下主要承担 O3 依赖 token 和兼容 fallback 角色。

---

## 6. Path A streaming DSA

Path A 专用 streaming DSA 模型不替换公共 `lmat5_spm`，也不改变 baseline `dotp_row + pack_acc` 语义；只有 `projects/cfd_dsa/configs/run_cfd_patha.py --patha-streaming=1 --patha-kernel=stream-dsa` 时才启用。

### 6.1 Path A-only 指令

| 指令 | 作用 | 是否影响 Path C |
| --- | --- | --- |
| `patha_stream_ld5 slot, field, [xbase]` | 从 SPM 读取 40B，写入 PathA input buffer | 否 |
| `patha_dotp_stream slot, lane` | 从 PathA input buffer 读取 row/vector，执行 1 行 dot product，写 internal result buffer | 否 |
| `patha_store5_result slot, [xOut]` | 从 internal result buffer 精确写回 5 个 FP64 = 40B | 否 |
| `patha_store_drain xOut` | benchmark 末尾等待 async store 可见 | 否 |

这些指令均以 `patha_` 命名，属于第一种方法的硬件建模指令。公共 `lmat5_spm` 仍保持 40B payload -> Z register 的 architectural 行为，因此第二种方法 Path C 继续使用原有 `lmat5_spm` 路径。

### 6.2 input buffer 与 result buffer

streaming DSA 的 steady dataflow：

```text
SPM stream load engine
    -> PathA input buffer[slot].row[0:4] / vec
    -> single dotp engine, 1 row per cycle when supplied
    -> PathA internal result buffer
    -> async store5_result, 40B
```

input buffer 使用 slot 级 reuse token 和 field 级 ready token 分离：

```text
stream load:
    read slot reuse token
    write field ready token

dotp stream:
    read row field token + vector field token
    write result lane token

store5_result:
    read 5 result lane tokens
    write result memory and slot reuse token
```

这一拆分避免 6 个 input field 被一个 token 串行化。当前 true unroll8
stream-dsa kernel 采用 field-ready 启动：

```text
slot vector + row0 ready -> lane0 dotp 可进入 IQ
row1 ready               -> lane1 dotp
...
row4 ready               -> lane4 dotp
```

slot reuse token 只保护下一组输入不覆盖旧 slot，不再作为当前 matvec 的
whole-slot compute barrier。实际运行中 O3 仍可能把后续 row load 抢先完成，
因此报告同时区分：

```text
early-ready opps:
    row/vector token 已满足且整槽尚未 ready 的启动机会

before all fields:
    dotp execute 时整槽仍未 ready 的严格观测
```

50K stream-dsa 主配置观测到 `early-ready opps = 200,001`，
`dual-load cycles = 149,997`，说明 field token 与双 40B stream load 端口
已真实参与调度；`before all fields = 16` 表明 O3 经常能在 dotp execute
前提前完成后续 row load。

本轮新增更细的 early-start 口径，用来区分“依赖已经满足”和“真正早于整槽
ready 执行”：

```text
dep-ready before all:
    row field token + vector field token 已经在 all-fields 之前 ready

execute before all:
    patha_dotp_stream 真正进入 execute 时 all-fields 仍未 ready

complete before all:
    dotp 按当前 latency 估计完成时 all-fields 仍未 ready
```

50K stream-dsa 主配置结果：

```text
early-ready opps        = 200,001
dep-ready before all    = 150,010
execute before all      = 16
complete before all     = 0
early issued/missed     = 150,010 / 149,994
miss reason             = issue scheduling / single dotp issue pressure
```

短 trace 中可直接看到 lane0 的早启动，以及后续 lane 因 O3 issue / 单 dotp
engine 调度延后到 all-fields 之后：

```text
56476 stream_ld field5 vector_ready
56476 stream_ld field0 row0 ready
56481 dotp_stream lane0 execute_before_all
56561 stream_ld field4 all_fields_ready
56563 dotp_stream lane1 execute_after_all
```

因此当前剩余问题不是 field-ready token 没生效，而是大多数 ready dotp 在
IQ / issue / 单 dotp engine 调度中等待，等到执行时后续 field load 已经完成。

### 6.3 async store5

`patha_store5_result` 是 Path A 专用 40B 精确写回模型：

```text
payload = result_buffer[slot].lane[0:4]
writeBlob(result_ptr, payload, 40B)
result_ptr += 40
```

默认性能实验使用：

```bash
--patha-store-mode=store5
--patha-store-stride=40
--patha-async-store=0
```

`store5_result` 不作为普通 SVE `MemWrite` 进入主循环关键路径；若启用
`--patha-async-store=1`，benchmark 末尾用一条 `patha_store_drain` 保证最终
C correctness 检查前所有写回可见。

当前推荐性能点统一使用同步口径：

```text
--patha-store-mode=store5
--patha-store-stride=40
--patha-async-store=0
```

该口径表示 store5 是 Path A DSA 内部精确 40B 写回指令，但不把未完成写回
跨越 benchmark 统计边界。`--patha-async-store=1` 只作为异步 store queue
探索项，报告时必须单独标注，不与同步 store5 默认结果混合。

### 6.4 硬件解释

stream-dsa 模型不是完整 MVM fused instruction。每个 matvec 仍然显式执行：

```text
6 x patha_stream_ld5
5 x patha_dotp_stream
1 x patha_store5_result
```

它的硬件含义是：SPM load、row-dot engine、result queue 和 async store 是 DSA 内部流水级，而不是让 O3 显式 Z register load / pack / SVE store 阶段全部参与关键路径。

---

## 7. SPM compact40 / padded64

### 7.1 逻辑布局

Path A SPM logical layout 始终使用 40B stride：

```text
MATRIX_BASE + 0 * 40B: A[0,0..4]
MATRIX_BASE + 1 * 40B: A[1,0..4]
MATRIX_BASE + 2 * 40B: A[2,0..4]
MATRIX_BASE + 3 * 40B: A[3,0..4]
MATRIX_BASE + 4 * 40B: A[4,0..4]
VEC_BASE:              x[0..4]
```

每个 `lmat5_spm` 的 architectural payload 固定为：

```text
5 x FP64 = 40B
```

### 7.2 physical read width

Path A 支持用 `--patha-spm-read-width` 报告物理读宽：

```bash
--patha-spm-read-width=40
--patha-spm-read-width=64
```

两种口径：

| 模型 | 物理读宽 | 含义 | payload utilization |
| --- | ---: | --- | ---: |
| compact40 | 40B | 320-bit dedicated local SPM read port | 100% |
| padded64 | 64B | 512-bit SRAM row / cacheline-like read width | 62.5% |

padded64 只是物理读宽统计，不改变 40B stride，也不改变 `lmat5_spm` 写入 Z 的 5-lane 语义。

---

## 8. result store full64 / pred40 / store5

Path A result 只有 5 个 FP64 lane，即 40B。benchmark 支持三种写回模式：

```bash
--patha-store-mode=full64
--patha-store-mode=pred40
--patha-store-mode=store5
```

| 模式 | 指令形式 | 写回字节数 |
| --- | --- | ---: |
| full64 | `str zXX, [addr]` | 64B |
| pred40 | `ptrue p0.d, vl5` + `st1d {zXX.d}, p0, [addr]` | 40B |
| store5 | `patha_store5_result slot, [xOut]` | 40B |

`pred40` 只改变结果写回宽度，不改变 `lmat5_spm -> dotp_row -> X16-X20 -> pack_acc -> Z` 的计算路径。
`store5` 只用于 Path A streaming DSA，它绕过 Z result 和 SVE predicate store，从 internal result buffer 直接写 40B result memory。

### 8.1 result stride

benchmark 采用真实 result stream layout：

| 模式 | 写回宽度 | result stride | layout |
| --- | ---: | ---: | --- |
| full64 | 64B | 64B | full vector result slot |
| pred40 | 40B | 40B | compact 5-lane result slot |
| pred40_stride64 | 40B | 64B | 40B exact store with 64B-spaced result slot |
| store5 | 40B | 40B | Path A DSA internal result slot |

`ptrue p0.d, vl5` 在 pred40 kernel prologue 中初始化一次，不在主循环中重复生成。主循环只执行 `st1d {zXX.d}, p0, [addr]` 和 result pointer advance。

新增 `--patha-store-stride=40|64` 用于拆分两类开销：

- `pred40_stride40`：40B 精确 store + 40B compact result layout。
- `pred40_stride64`：40B 精确 store + 64B padded result layout。

若 `pred40_stride64` 明显慢于 `pred40_stride40`，说明主要开销来自 64B-spaced result layout / cache-line / store queue 行为，而不是 SVE predicated store 指令形态本身。若 `store5` 明显降低 `MemWrite issued`，说明 Path A 专用 DSA 写回能把 result store 从 O3/SVE store 关键路径移走。

---

## 9. projects/cfd_dsa/configs/run_cfd_patha.py 参数说明

baseline / pred40 运行入口：

```bash
./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_patha.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm \
    --cpu=o3 \
    --bench-iters=50000 \
    --patha-spm-mode=direct \
    --patha-spm-read-width=40 \
    --patha-store-mode=pred40 \
    --patha-store-stride=40 \
    --patha-acc-mode=internal-buffer \
    --patha-result-buffer-depth=2 \
    --patha-dotp-lat=7 \
    --patha-dotp-count=1 \
    --patha-pack-count=1 \
    --patha-unroll=8
```

streaming DSA 运行入口：

```bash
./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_patha.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm \
    --cpu=o3 \
    --bench-iters=50000 \
    --patha-streaming=1 \
    --patha-kernel=stream-dsa \
    --patha-acc-mode=internal-buffer \
    --patha-result-buffer-depth=2 \
    --patha-input-buffer-depth=2 \
    --patha-stream-read-ports=2 \
    --patha-store-mode=store5 \
    --patha-store-stride=40 \
    --patha-async-store=0 \
    --patha-dotp-lat=5 \
    --patha-dotp-count=1
```

Path A 专用参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--patha-spm-mode` | `direct` | Path A Direct-SPM upper-bound |
| `--patha-spm-read-width` | `40` | 物理读宽，必须 >= 40 |
| `--patha-store-mode` | `pred40` | `full64`、`pred40` 或 Path A streaming 专用 `store5` |
| `--patha-store-stride` | store mode 推导 | `pred40` 支持 40 或 64，`full64` 固定 64，`store5` 推荐 40 |
| `--patha-acc-mode` | `internal-buffer` | `internal-buffer` 默认；`xregs` 兼容回归 |
| `--patha-result-buffer-depth` | `2` | 内部结果 buffer 深度，支持 1/2/4 sweep |
| `--patha-dotp-lat` | `7` | `dotp_row` O3-visible latency；5/6/7 用于 sensitivity sweep |
| `--patha-dotp-count` | `1` | 默认单 dotp engine；2 是 balanced optional config；4 仅作探索对照 |
| `--patha-pack-count` | `1` | 默认单独立 pack FU；2 仅作 sweep |
| `--patha-unroll` | `8` | 默认 true unroll8 kernel；internal-buffer depth>=2 时启用 overlap 调度 |
| `--patha-matld-readports` | default FU config | 可选 lmat readport sweep |
| `--patha-streaming` | `0` | 启用 Path A-only streaming DSA |
| `--patha-kernel` | `baseline` | `baseline`、`stream-scheduled`、`stream-dsa` |
| `--patha-input-buffer-depth` | `2` | Path A streaming input buffer 深度 |
| `--patha-stream-read-ports` | `1` | Path A streaming load FU 数；2 用于隐藏 240B/matvec input bandwidth |
| `--patha-store-queue-depth` | `4` | Path A async store queue 深度 |
| `--patha-async-store` | `0` | `store5` 的 CPU-local async writeback 模式 |
| `--cfd-trace-enable` | off | 输出短 CFD micro-trace，用于检查 field-ready / dotp execute 顺序 |
| `--cfd-trace-matvecs` | `4` | trace 预算，按 matvec 估算事件数 |
| `--cfd-trace-file` | `results/cfd_dsa/runs/cfd_micro_trace.csv` | trace CSV 输出路径 |

兼容入口：

```bash
./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_dsa.py ...
```

`projects/cfd_dsa/configs/run_cfd_dsa.py` 仅作为 deprecated / compatibility wrapper 保留。新实验优先使用 `projects/cfd_dsa/configs/run_cfd_patha.py`。

---

## 10. projects/cfd_dsa/benchmarks/patha/test_cfd_patha_extreme_perf.c benchmark

编译：

```bash
aarch64-linux-gnu-gcc -static -O2 -march=armv8-a+sve \
    -o build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm projects/cfd_dsa/benchmarks/patha/test_cfd_patha_extreme_perf.c
```

benchmark 要求：

- Ping matrix != Pong matrix。
- Ping vector != Pong vector。
- reference C 使用 row-major 标准 MVM。
- SPM 中 matrix row 和 vector 都以 40B stride 存放。
- correctness pair 和 timed benchmark 都必须 PASS。
- `pred40` 路径必须只改变 store，不改变计算序列。
- baseline 仍使用 `pred40_stride40` 的 true unroll8 overlap kernel；streaming DSA 使用 `patha_stream_ld5/patha_dotp_stream/patha_store5_result`，不调用 Path C 路径。
- true unroll8 一次 loop 处理 8 个 matvec；`internal-buffer depth>=2` 使用 overlap kernel，`depth=1` 或 `xregs` 使用安全顺序 fallback。

旧文件名 `projects/cfd_dsa/benchmarks/patha/test_cfd_extreme_perf.c` 仅作为历史兼容保留，不再作为推荐命令。

---

## 11. 性能结果与统计口径

### 11.1 统计公式

```text
pathA.numSpmReads        = committed_CFDDSAMatLd
pathA.logicalBytesRead   = committed_CFDDSAMatLd * 40
pathA.physicalBytesRead  = committed_CFDDSAMatLd * patha_spm_read_width
pathA.payloadUtilization = logicalBytesRead / physicalBytesRead
pathA.localSpmCoverage   = logicalBytesRead / implied_logical_spm_bytes
pathA.resultStoreBytes   = inferred_matvecs * store_width
pathA.resultLayoutBytes  = inferred_matvecs * store_stride
pathA.storeWidth         = store_width
pathA.storeStride        = store_stride
pathA.storeInstructions  = inferred_matvecs
pathA.packAccCount       = inferred_matvecs
pathA.internalBufWrites  = committed_CFDDSADotp
pathA.internalBufPacks   = committed_CFDDSAPack
pathA.streamBytesRead    = committed_CFDDSAStreamLd * 40   # stream-dsa only
pathA.store5Count        = committed_CFDDSAStore5          # stream-dsa only
```

推导：

```text
inferred_matvecs = committed_CFDDSADotp / 5
implied_logical_spm_bytes = committed_CFDDSAMatLd * 40
effective_MACs = committed_CFDDSADotp * 5
effective_FLOPs = effective_MACs * 2
```

stream-dsa 模式下，公共 `lmat5_spm` 不在 timed kernel 中使用；SPM logical read 改由 `committed_CFDDSAStreamLd * 40` 推导。该口径只属于 Path A streaming DSA，不影响 baseline 和其他方法。

### 11.2 当前推荐结果

benchmark 在 timed kernel 前后使用 m5 reset/dump，因此下表只覆盖 steady section，不混入 reference C、printf、初始化和 verify。

| 当前模式 | 用途 | store | cycles/matvec | IPC | FLOPs/cycle | MemWrite / matvec | correctness |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| baseline O3 explicit | 兼容和对照路径 | pred40 | 5.02 | 2.8385 | 9.957 | 1.00 | PASS |
| streaming DSA | 推荐硬件解释路径 | store5 | 5.52 | 2.2196 | 9.058 | 0.00 | PASS |

结论：

```text
baseline 路径用于保留 lmat5_spm + dotp_row + pack_acc 的显式 ISA 行为。
streaming DSA 路径用于解释硬件内部 input/result/store pipeline。
两条路径都保持 dotp_count=1，不依赖增加 dotp engine 数量。
```

### 11.3 必要对照

只保留会影响硬件解释的关键对照：

| 对照项 | 观察点 | 结论 |
| --- | --- | --- |
| `pred40_stride40` vs `full64` | result store bytes | 40B 精确写回减少 result 带宽 |
| `pred40_stride40` vs `pred40_stride64` | result layout | 64B-spaced layout 会放大 store / cache-line 开销 |
| `pred40` vs `store5` | store 执行路径 | Path A 专用 DSA store 可移除 SVE store 的关键路径影响 |
| `dotp_count=1` vs `dotp_count=2` | 面积 / 吞吐权衡 | `dotp_count=1` 是默认；`dotp_count=2` 只作为 balanced optional config |

详细 sweep 输出保留在脚本生成文件中，不再在本文档逐项展开：

```text
projects/cfd_dsa/tools/run_patha_dotp_sensitivity.py
results/cfd_dsa/runs/patha_sweep/patha_dotp_sensitivity_summary.csv
results/cfd_dsa/runs/patha_sweep/patha_dotp_sensitivity_summary.md
```

### 11.4 推荐配置

```text
Path A baseline compatibility:
    patha_store_mode = pred40
    patha_store_stride = 40
    patha_acc_mode = internal-buffer
    patha_result_buffer_depth = 2
    patha_dotp_count = 1

Path A streaming DSA main:
    patha_streaming = 1
    patha_kernel = stream-dsa
    patha_store_mode = store5
    patha_async_store = 0
    patha_stream_read_ports = 2
    patha_dotp_count = 1
    patha_dotp_lat = 5
```

`dotp_count=4`、`dotp_lat<=3`、大规模版本历史表都只属于探索记录，不再作为主文档结论。

### 11.5 early-start 与 trace 统计

Path A streaming DSA 的关键统计：

```text
pathA.earlyReadyOpportunities
pathA.dotpDependencyReadyBeforeAllFields
pathA.dotpExecuteStartedBeforeAllFields
pathA.dotpCompletedBeforeAllFields
pathA.earlyReadyIssued
pathA.earlyReadyMissed
pathA.earlyReadyIssueEfficiency
pathA.earlyMissedDueToIssueWidth
```

50K 主配置实测：

```text
cycles/matvec        = 5.52
IPC                  = 2.2196
early-ready opps     = 200,001
dep-ready before all = 150,010
execute before all   = 16
complete before all  = 0
dual-load cycles     = 149,997
0-issue ratio        = 0.13%
dotp FU busy         = 150,009
stream_ld FU busy    = 99,995
```

trace 命令：

```bash
./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_patha.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm \
    --cpu=o3 \
    --bench-iters=1000 \
    --patha-streaming=1 \
    --patha-kernel=stream-dsa \
    --patha-store-mode=store5 \
    --patha-store-stride=40 \
    --patha-dotp-count=1 \
    --patha-dotp-lat=5 \
    --patha-stream-read-ports=2 \
    --cfd-trace-enable \
    --cfd-trace-matvecs=4 \
    --cfd-trace-file=results/cfd_dsa/runs/patha_micro_trace.csv
```

trace 文件字段：

```text
cycle,seq,path,slot,inst,field,lane,k,ready0,ready1,all_ready,note
```

该 trace 是轻量级 micro-trace，不替代完整 O3PipeView；它用于证明 Path A
field token 的 ready / execute 相对顺序。

### 11.6 验证命令

默认主配置：

```bash
./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_patha.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm \
    --cpu=o3 \
    --bench-iters=50000 \
    --patha-acc-mode=internal-buffer \
    --patha-result-buffer-depth=2 \
    --patha-dotp-lat=7 \
    --patha-dotp-count=1 \
    --patha-pack-count=1 \
    --patha-unroll=8 \
    --patha-spm-mode=direct \
    --patha-spm-read-width=40 \
    --patha-store-mode=pred40 \
    --patha-store-stride=40
```

streaming DSA 主配置：

```bash
./build/ARM/gem5.opt projects/cfd_dsa/configs/run_cfd_patha.py \
    --binary=./build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm \
    --cpu=o3 \
    --bench-iters=50000 \
    --patha-streaming=1 \
    --patha-kernel=stream-dsa \
    --patha-acc-mode=internal-buffer \
    --patha-result-buffer-depth=2 \
    --patha-input-buffer-depth=2 \
    --patha-stream-read-ports=2 \
    --patha-store-mode=store5 \
    --patha-store-stride=40 \
    --patha-async-store=0 \
    --patha-dotp-lat=5 \
    --patha-dotp-count=1
```

其他回归和 sweep 使用第 9 章参数表按需组合，不在主文档展开。

---

## 12. 已知问题与后续优化

1. `CFDDSAPack` 已有独立 OpClass/FU，但 `pack_count=2` 无性能收益；baseline 当前瓶颈仍主要来自单 dotp engine。
2. `internal-buffer` 是 Path A steady benchmark 的 DSA 内部状态模型；X16-X20 仍保留为 O3 依赖 token 和 `xregs` fallback。
3. `dotp_row` 写 `X16-X20` 是跨域建模，适合性能路径，但不是通用 ISA 风格。
4. Direct-SPM 模式代表紧耦合上限，普通内存系统路径不能直接与该结果混为一谈。
5. `pred40_stride64` 证明 result layout 可能比 store 指令形态更关键；不要把所有开销都归因于 `st1d`。
6. full64 / pred40_stride64 当前保留为对照 kernel，不是默认优化路径。
7. stream-dsa 使用 Path A 专用 input/result/store buffer；它不改变公共 `lmat5_spm`，不应与其他路径混写。
8. `dotp_count=2` 是 balanced optional config，不是默认流片点；`dotp_count=4` 只保留为历史探索上限。
9. stream-dsa `dotp_lat=5`、`dotp_count=1`、同步 `store5`、2 个 stream read
   ports 的当前 50K 结果为 5.52 cycles/matvec、IPC 2.2196；
   `early-ready opps = 200,001`，`dep-ready before all = 150,010`，
   `execute before all = 16`，`dual-load cycles = 149,997`。
10. Path A 内部 result/store buffer 的正常路径 correctness 已覆盖；wrong-path
    squash stress 仍需独立小测试，不能用当前 steady benchmark 代替。
11. 后续如果要做 Path A 流片级 SPM backpressure，应单独增加 Path A 专用模型，不应混入其他方法的报告字段。
12. Path C 回归已确认 PASS；Path A streaming 指令没有改变第二种方法的公共执行路径。

---

## 13. 附录

### 13.1 推荐构建顺序

```bash
scons build/ARM/gem5.opt -j2

aarch64-linux-gnu-gcc -static -O2 -march=armv8-a+sve \
    -o build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm projects/cfd_dsa/benchmarks/patha/test_cfd_patha_extreme_perf.c
```

### 13.2 Path A correctness 输出

成功输出应包含：

```text
pair_final_should_be_pong: PASS
benchmark_final_should_be_pong: PASS
Overall: PASS
```

### 13.3 文件命名约定

```text
projects/cfd_dsa/configs/run_cfd_patha.py                 Path A 推荐入口
projects/cfd_dsa/benchmarks/patha/test_cfd_patha_extreme_perf.c    Path A 推荐 benchmark source
build/cfd_dsa/aarch64/test_cfd_patha_extreme_arm       Path A 推荐 binary
projects/cfd_dsa/configs/run_cfd_dsa.py                   deprecated compatibility wrapper
```

### 13.4 LU-SGS Step2 扩展索引

LU-SGS 三阶段方案的 Step2 已作为独立扩展落地，不改变本文档记录的 Path A
底层 MVM 语义。新增 TRSV5 指令/FU/stats/test 见 `CFD_DSA_TRSV5.md`；
端到端 LU-SGS Step2 集成、8x64 结果和 Step1/Path A 回归见
`CFD_DSA_LUSGS_STEP2.md`。Step2 baseline 之上的 core 路径保留原
`pack_acc`，并增加共享 pack FU/internal result buffer 的
`patha_pack_sub5`；当前进一步提供 `step2-core-forwarded`，用
`trsv5_lu_spm_zrhs` 将 forward 热路径的 `pack_sub` 结果作为 Z-register RHS
直接送入 TRSV5，并提供 `step2-core-forwarded-context`，用
`ForwardLineContext.prev_dqstar` / `BackwardLineContext.next_dq` 替代同一
line 相邻 cell 的 heap vector source。当前又增加
`step2-core-forwarded-linebuf`，把软件 `ForwardLineContext` /
`BackwardLineContext` 收敛为 `CfdLocalSpm` 侧 hardware line buffer，并通过
`linebuf_rd_*5 -> z5` 让 Path A vector 直接进入 dotp 输入，删除普通 cell 的
Path A vector SPM staging 和 vector `lmat5_spm`。该 line-buffer 版本仍保留 heap
结果写回、Path A matrix/TRSV LU staging，不是 macro controller、DMA 或多 line
hardware scheduler。实现、正确性、稳态结果、RHS/context/linebuf-forwarding
统计与当前 functional-base-read 限制见 `CFD_DSA_LUSGS_STEP2_CORE.md`。

Step2 现在还保留上述全部路径，并行新增 `step2-pretransform` 与
`step2-pretransform-context`。新方案使用 `trsm5_mrhs_spm` 生成
`D_inv=solve(D,I)` 和 `L_bar=solve(D,C)`；现有 `b_bar` 已是 `D^-1 U`，因此
`U_bar` 直接复用它。运行热循环只执行 Path A MVM 和 `vec5_sub_z`，中间值保留
在 Z 寄存器，不写 heap tmp。8x256、50 sweep 实测热循环相对 forwarded 路径
加速约 1.101x，包含预处理的回本点约 88 至 90 sweep。数学审查、SPM/heap
存储位置、正确性、延迟敏感性和 fallback 见
`CFD_DSA_LUSGS_PRETRANSFORM.md`。

在此基础上又旁路新增 `step2-pretransform-optprep[-context]`。cell-local
`trsm5_inv_lbar_spm` 每 cell 只读取一次 LU，内部生成单位矩阵，并用原 TRSV5
helper 的相同 FP64 顺序生成 `D_inv/L_bar`；旧 MRHS 与全部热循环保持不变。
8x256 fast-validation 预处理从 13,171,910 降至 1,971,204 cycles，保守回本点为
18 sweeps。当前 coarse serializing 模型的实测 overlap 仍为 0，限制和完整分解见
`CFD_DSA_LUSGS_PRETRANSFORM.md` 第 15 至 22 节。

### 13.5 LU-SGS Step4-C 扩展索引

Step4-C 在 Step4-B Path A 子资源图之上新增 staged TRSV5 和 Vector5 queue/lane engine。1x64 实测为 8934 actual event cycles，TRSV/divide/FMA=`64/320/1280`，Vector COPY/SUB/lane-op=`2/126/640`，mismatch=0；1x50000 端到端压力测试同样 mismatch=0。该扩展不修改 guest Path A 指令序列，也不表示 multi-bank SPM、多 context、DMA 或 RTL 已完成；完整设计、参数敏感性和回归见 `CFD_DSA_LUSGS_STEP4.md`。

### 13.6 LU-SGS raw D/L/U/R 完整成本路径

旧 Step2/pretransform 模式继续以 `LU(D)、L、U_bar、R` 作为 prepared input 做
回归；新增 raw 模式以真正的 `D、L、U、R` 开始，禁止 `u_bar=b_bar` alias。两条
raw 方案共同执行软件 5x5 Crout LU 和硬件 `trsm5_mrhs_spm(LU,U)`；预变换仅额外
承担 Dinv/Lbar。新增单-cell `trsm5_coeff3_spm` 可融合 I/L/U 共 15 个 RHS，旧
TRSV5、MRHS、dual 和所有 legacy 模式均未删除。

8x256、interleave=4 的公平联测得到公共预处理 1832394 cycles，TRSV/PRE dual
热循环 1589115/1460570 cycles/sweep，dual 公平 extra 1454686 cycles，回本点
12 sweeps。coeff3 的融合阶段比 dual+MRHS 少约 32724 cycles，但当前仍是
functional coarse model。数据结构、SPM slot、三输出 ring、fallback、更新频率、
五张性能表和正确性证据见 `CFD_DSA_LUSGS_PRETRANSFORM.md` 第 23 至 29 节。

### 13.7 细粒度 event 系数预处理

raw 路径现新增旁路 `CfdCoeffPreprocessController`，由
`--coeff-preprocess-model=event --lu5-model=event` 启用；默认仍为
`coarse/software`，因此旧 Step2、TRSV5、MRHS、dual、coeff3 和 decode 行为不变。
event 控制指令以 160B descriptor launch request、以 token poll completion，均为
non-speculative 且没有全局 serialize-before/after。

控制器每周期独立推进 input staging、Crout LU5、统一 5/15-RHS TRSM5 和 output
drain。LU 与 RHS 状态机不使用一次性 functional helper 计时；divider、mul、sub、
pending table、input slot、SPM read/write port、bank、drain outstanding/queue/output
ring 都有明确资源状态与 stall 计数。Dinv/Lbar/Ubar 具有分批 ready event，可在后续
batch 计算时独立 drain；最终 heap 写回仍完整执行。

8x64 full-validation 的两条 event raw 路径 mismatch/fallback/nonfinite/failure 均为
0，LU/Ubar residual 为 `1.776e-15/2.082e-17`。8x256、16 sweeps、performance
validation 下，TRSV/PRE event preprocess 为 930172/1488166 cycles，热循环每 sweep
为 1598952.63/1444903.75 cycles，PRE fair extra=557994，公平回本点更新为 4 sweeps。
状态、数据保存位置、trace、overlap/stall 分解及当前建模边界见
`CFD_DSA_LUSGS_PRETRANSFORM.md` 第 30 节。

固定 8x256 的 26 项 one-factor sensitivity campaign 全部通过。RHS lanes 是最敏感
资源，mul/sub lanes、pending depth 和 input slots 在默认点已饱和；组合测试推荐
`RHS15/div1/mul1` 作为面积/性能膝点（PRE preprocess=1127000、II=549.68），
`RHS15/div2/mul1` 作为性能优先点（1021358、II=498.13）。

### 13.8 RHS15、LU early-start 与共享 CfdLocalSpm

event 系数预处理现已完成后续实现：默认为 `RHS15/div1/mul1`、
`round-robin-ready` 和 LU step-ready early-start；15 仅表示 15 个可调度 RHS context，
计算资源仍是共享 divider/mul/sub。input/drain 可选经过唯一 `CfdLocalSpm` 异步
request/response，并保留 internal 模型对照；支持 legacy、matrix-separated、
row-striped 三种 layout 和逐 bank/matrix 统计。

packet legacy 的正式 8x256、16-sweep 结果为 TRSV/PRE preprocess
888936/1000020 cycles，runtime/sweep 1607415.50/1444217.88，PRE fair extra=111084，
break-even=1，总加速 1.1037x。div2 将 PRE preprocess 降到 935614，但 dependency
不变且瓶颈转向 mul/sub，故只作为性能优先点。动态 auto 使用当前配置校准轮计算阈值，
不写死 sweep 数；另新增 cancel ISA 和 software-LU/event-TRSM 混合模式。

这里的 packet 是 `CfdLocalSpm` SimObject 内共享 event API，还不是 memory hierarchy
RequestPort 的 `mem::Packet`；compute read 仍来自 request-local buffer，heap store 仍在
SPM response 后由 proxy 提交。完整依赖图、packet 生命周期、压力测试和限制见
`CFD_DSA_LUSGS_PRETRANSFORM.md` 第 31 节。

---

*本文档主线记录 CFD-DSA 第一种方法 Path A；LU-SGS 各阶段作为独立扩展由附属报告记录。*
