# LU-SGS Step2-core、line-context 与 hardware line-buffer 优化报告

> 2026-07-10 update: Step2 now also provides the optional
> `step2-pretransform` and `step2-pretransform-context` paths. They preserve all
> implementations described here and move repeated diagonal solves into TRSM5
> multi-RHS coefficient preprocessing. Coefficient semantics, storage, Z-register
> intermediate lifetime, performance, and break-even results are documented in
> `CFD_DSA_LUSGS_PRETRANSFORM.md`.

## 1. 修改目标

本轮不重写 LU-SGS，不增加 macro controller，也不修改 TRSV5 的 coarse
模型。优化对象只限于 Step2 中反复出现的单 cell 核心算子：

```text
base - matrix * vector
```

原 Step2 的 Path A MVM、TRSV5、reference、Step1 和 Step2 baseline 均保留。
Step2-core 的第一步目标是消除 MVM 结果经过普通内存 tmp slot 往返，以及随后由
CPU 执行的 5-lane subtract。当前版本进一步增加了
`step2-core-forwarded`，把 forward 中 `patha_pack_sub5` 产生的 RHS 直接转发给
TRSV5，避免 `rhs_prime[5]` 栈上写回和 `SPM_TRSV_RHS_BASE` staging。
在此基础上，上一阶段继续增加 `step2-core-forwarded-context`，只优化同一
line 内相邻 cell 的 vector 来源：forward 使用上一 cell 刚生成的
`ForwardLineContext.prev_dqstar[5]`，backward 使用右侧刚生成的
`BackwardLineContext.next_dq[5]`。heap 结果数组仍然写回，用于最终结果、
correctness 和后续阶段对照。

当前阶段进一步增加 `step2-core-forwarded-linebuf`。它把上述软件
`ForwardLineContext` / `BackwardLineContext` 收敛到 gem5 侧
`CfdLocalSpm` 内的显式 hardware line buffer，并新增
`linebuf_rd_fwd5` / `linebuf_wr_fwd5` / `linebuf_rd_bwd5` /
`linebuf_wr_bwd5` 四条窄指令。普通 forward/backward cell 的 Path A vector
不再写入 `SPM_PING_VEC_BASE`，也不再通过 `lmat5_spm vec -> z5` 读回；而是由
`linebuf_rd_*5` 按 `(line_id, cell_id)` 直接把 5-lane 相邻解向量写入 `z5`。
Path A matrix、TRSV LU matrix、最终 heap 结果数组仍保留，方便和 Step2
baseline、Step2-core、RHS-forwarded 与软件 context 版本逐项对照。

更准确地说，本阶段优化的是 LU-SGS forward/backward sweep 中的两类
dependency-bound 更新：

```text
forward : rhs_prime[i] = rhs[i]     - C[i]     * dq_star[i - 1]
backward: dq[i]        = dq_star[i] - B_bar[i] * dq[i + 1]
```

其中 `C[i] * dq_star[i - 1]` 和 `B_bar[i] * dq[i + 1]` 仍由既有 Path A
5x5 MVM 执行；本阶段只把原来在 `pack_acc` 之后发生的软件减法前移到
Path A 的 pack 阶段，形成 `MVM -> fused-sub -> result`。它不是新的
LU-SGS macro controller，也没有把整条 sweep 包成一个不可观察的大指令。

## 2. 修改内容

- `src/arch/arm/isa/formats/custom_cfd.isa`：增加 `patha_pack_sub5`、
  `trsv5_lu_spm_zrhs` 和 line-buffer read/write 解码。
- `src/arch/arm/insts/cfd_dsa.hh/.cc`：增加 `DSAPackSubAcc` 和
  `DSATrsv5LuZRHS`，并增加 `DSALineBufRead5` / `DSALineBufWrite5`。
  pack-sub 复用 Path A internal result buffer 和既有 `CFDDSAPack` FU；
  ZRHS-TRSV 复用 `CFDDSATrsv5Op`；line-buffer 指令使用新的
  `CFDDSALineBufOp`。
- `src/arch/arm/cfd_local_spm.hh/.cc`：增加 `pathAFusedSubOps` 以及
  `trsv5RhsForwarded`、`trsv5RhsSpmStageElided` 等 RHS forwarding 计数；
  增加 `LineBufferEntry` 表、`writeLineBuffer()` / `readLineBuffer()` 以及
  line-buffer hit/miss/tag-conflict/stall 统计。
- `src/cpu/FuncUnit.py`、`src/cpu/op_class.hh`、
  `src/cpu/o3/FuncUnitConfig.py`、`src/cpu/o3/FUPool.py`：增加
  `CFDDSALineBuf` OpClass/FU，默认 1-cycle pipelined。
- `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c`：保留原三种
  模式并增加 `step2-core`、`step2-core-forwarded`，增加
  `step2-core-forwarded-context` 和 `step2-core-forwarded-linebuf`，增加
  fused/tmp/sub/forwarding/context/linebuf/cycle/correctness 统计。
- `projects/cfd_dsa/configs/run_cfd_dsa.py` 和 Step2 runner：支持
  `--lusgs-mode=step2-core`、`--lusgs-mode=step2-core-forwarded` 和
  `--lusgs-mode=step2-core-forwarded-context`、
  `--lusgs-mode=step2-core-forwarded-linebuf`；`all` 同时运行 Step2
  baseline、Step2-core、Step2-core-forwarded、Step2-core-forwarded-context
  和 Step2-core-forwarded-linebuf。
- `projects/cfd_dsa/benchmarks/common/test_cfd_dsa_decode_exclusive.c`：增加
  pack-sub、zrhs-TRSV 编码和算术语义回归。

pack-sub 与 pack_acc 共享 `CFDDSAPack`，因此两者竞争同一 pack 资源；ZRHS-TRSV
共享 `CFDDSATrsv5`。line buffer 是独立 1-cycle `CFDDSALineBuf` 资源，用于把
per-line 相邻解向量送入或写回 `CfdLocalSpm` 的 line-buffer 表。原 pack_acc
编码、行为和 standalone Path A 路径不变。

核心技术改动可以概括为三层：

1. ISA 层增加一个只替代 `pack_acc` 的末端变体 `patha_pack_sub5`。
2. gem5 执行层复用 Path A internal result buffer，把 5-lane MVM 结果与
   `base[0:4]` 做逐 lane 减法后写入 Z 寄存器。
3. benchmark 层增加 `step2-core` 和 `step2-core-forwarded` 模式，在
   forward/backward 的 MVM 更新点选择 fused 路径；`step2` baseline 仍保留
   tmp slot 路径作为对照。
4. benchmark 层增加 line context，只替换相邻 cell 的 Path A vector 来源；
   Path A SPM staging、`lmat5_spm vec -> z5`、`dotp_row x5`、
   `patha_pack_sub5` 和 `trsv5_lu_spm_zrhs` 的指令序列均保持不变。
5. gem5 侧增加 hardware line buffer，替换软件 context 的内存读写，并让
   Path A vector 由 `linebuf_rd_*5` 直接写入 `z5`，删除普通 cell 的 vector
   SPM staging 和 vector `lmat5_spm`。

## 3. 新增执行路径

Step2 baseline：

```text
Path A dotp x5 -> internal result -> pack_acc -> Z
  -> st1d tmp_slot -> volatile load -> CPU lane subtract
  -> rhs_prime/dq
```

Step2-core：

```text
Path A dotp x5 -> internal result
  -> patha_pack_sub5(base) -> Z
  -> st1d rhs_prime/dq
```

Step2-core-forwarded 的 forward 热路径：

```text
Path A dotp x5 -> internal result
  -> patha_pack_sub5(base=rhs[i]) -> z11
  -> trsv5_lu_spm_zrhs(lu_a[i], z11)
  -> st1d dq_star[i]
```

Step2-core-forwarded-context 的 forward 热路径：

```text
TRSV5 result dq_star[i-1]
  -> ForwardLineContext.prev_dqstar[5]
  -> stage Path A vector 到 SPM_PING_VEC_BASE
  -> Path A dotp x5
  -> patha_pack_sub5(base=rhs[i]) -> z11
  -> trsv5_lu_spm_zrhs(lu_a[i], z11)
  -> st1d dq_star_step2_core_forwarded_context[line,i]
  -> ForwardLineContext.prev_dqstar[5]
```

Step2-core-forwarded-context 的 backward 热路径：

```text
dq[i+1]
  -> BackwardLineContext.next_dq[5]
  -> stage Path A vector 到 SPM_PING_VEC_BASE
  -> Path A dotp x5
  -> patha_pack_sub5(base=dq_star[i]) -> z11
  -> st1d dq_step2_core_forwarded_context[line,i]
  -> BackwardLineContext.next_dq[5]
```

Step2-core-forwarded-linebuf 的 forward 热路径：

```text
TRSV5 result dq_star[i-1]
  -> linebuf_wr_fwd5(line, i-1, z11)
  -> linebuf_rd_fwd5(line, i-1) -> z5
  -> Path A dotp x5
  -> patha_pack_sub5(base=rhs[i]) -> z11
  -> trsv5_lu_spm_zrhs(lu_a[i], z11)
  -> st1d dq_star_step2_core_forwarded_linebuf[line,i]
  -> linebuf_wr_fwd5(line, i, z11)
```

Step2-core-forwarded-linebuf 的 backward 热路径：

```text
dq[i+1]
  -> linebuf_wr_bwd5(line, i+1, z11)
  -> linebuf_rd_bwd5(line, i+1) -> z5
  -> Path A dotp x5
  -> patha_pack_sub5(base=dq_star[i]) -> z11
  -> st1d dq_step2_core_forwarded_linebuf[line,i]
  -> linebuf_wr_bwd5(line, i, z11)
```

前向的 base 为 `rhs[i]`，pack-sub 结果写入 `rhs_prime`，随后仍由原
`trsv5_lu_spm` 求解。forwarded 模式中，pack-sub 结果不再写入
`rhs_prime[5]`，而是作为 Z 寄存器 RHS 直接进入 `trsv5_lu_spm_zrhs`。后向的
base 为 `dq_star[i]`，结果仍直接写入 `dq[i]`，暂不改变。

这里的 `base` 是被减数，Path A MVM 结果是减数：

```text
pack_sub lane j:
  z_out[j] = base[j] - internal_mvm_result[j]
```

因此 Step2-core 没有改变 LU-SGS 的数学顺序，只改变了 `matrix * vector`
结果和 `base - tmp` 合并的位置。

### 3.1 单次 fused update 的流水线

一次 `patha_mvm_sub5_fused(matrix, vector, base, result)` 在软件和模拟硬件之间
按如下阶段流动：

```text
S0 选择算子输入
   forward : matrix=C[i],     vector=dq_star[i-1], base=rhs[i]
   backward: matrix=B_bar[i], vector=dq[i+1],      base=dq_star[i]

S1 stage Path A 输入到 local SPM
   matrix[5x5] -> SPM_PING_MAT_BASE
   vector[5]   -> SPM_PING_VEC_BASE
   full_barrier()

S2 lmat5_spm 取数到 SVE Z 寄存器
   z0..z4 <- 5 行 matrix
   z5     <- vector

S3 dotp_row x5 形成 5 个 row dot-product
   lane r = dot(matrix_row_r, vector)
   结果写入 Path A internal result buffer，或在 fallback 模式下通过 slot-specific
   token register 维持 O3 依赖。

S4 patha_pack_sub5 消费 Path A MVM 结果
   x12 给出 base 地址
   functional read 读取 base[0:4]
   z11[j] = base[j] - mvm[j]

S5 st1d 写回最终 5-lane update
   普通 core forward : z11 -> rhs_prime，紧接着作为 TRSV RHS
   forwarded forward : z11 -> trsv5_lu_spm_zrhs RHS，TRSV 结果再 st1d 到 dq_star[i]
   backward          : z11 -> dq[i]
   dsb sy; isb
```

S1 和 S5 的 barrier 是当前 benchmark 为了保持原型路径稳定而保留的同步点。
普通 Step2-core 主要减少 tmp slot 的普通内存往返和 CPU lane subtract 指令；
`step2-core-forwarded` 在 forward 热路径上继续删除 `rhs_prime[5]` 的栈上
store/load 以及 TRSV RHS SPM staging。它仍然没有实现跨 cell 的
`dq_star/dq` line context forwarding。

### 3.2 数据如何保存和流转

benchmark 中的主要数组仍由 `Problem` 持有：

```text
lu_a             : 每个 cell 的 LU/TRSV 矩阵
c_mat            : forward coupling matrix C[i]
b_bar            : backward coupling matrix B_bar[i]
rhs              : 原始 RHS
dq_star_step2    : Step2 baseline forward 结果
dq_step2         : Step2 baseline backward 结果
dq_star_step2_core: Step2-core forward 结果
dq_step2_core    : Step2-core backward 结果
dq_star_step2_core_forwarded: Step2-core-forwarded forward 结果
dq_step2_core_forwarded: Step2-core-forwarded backward 结果
dq_star_step2_core_forwarded_context: context 模式 forward 结果
dq_step2_core_forwarded_context: context 模式 backward 结果
dq_star_step2_core_forwarded_linebuf: hardware line-buffer 模式 forward 结果
dq_step2_core_forwarded_linebuf: hardware line-buffer 模式 backward 结果
tmp_slots        : 仅 baseline 使用
```

Path A 的输入 staging 使用固定 SPM 地址：

```text
SPM_BASE          = 0x70000000
SPM_STRIDE_BYTES  = 40
SPM_PING_MAT_BASE = SPM_BASE
SPM_PING_VEC_BASE = SPM_BASE + 5 * 40
```

矩阵按 5 行写入 `SPM_PING_MAT_BASE`，每行 40B，即 5 个 double；向量写入
`SPM_PING_VEC_BASE`。这与 `lmat5_spm(row, x21, zd)` 的访问方式一致：
`x21` 指向矩阵区，`x23` 指向向量区。

baseline 的中间结果保存路径是：

```text
Path A MVM -> z11 -> st1d tmp_slots[line, cell]
tmp_slots  -> volatile load -> tmp[5]
CPU for lane loop: result[lane] = base[lane] - tmp[lane]
```

Step2-core 的保存路径是：

```text
Path A MVM internal result buffer
base[5] -> patha_pack_sub5 -> z11
z11 -> rhs_prime 或 dq[i]
```

普通 Step2-core forward 中的 `rhs_prime` 是栈上 5-lane 临时数组，马上作为
`trsv5_step_counted()` 的 RHS。TRSV 完成后，结果再复制到 `dq_star_step2_core`。
`step2-core-forwarded` forward 则不写 `rhs_prime[5]`，而是由
`trsv5_lu_spm_zrhs` 直接消费 `z11`，最终写入
`dq_star_step2_core_forwarded[line, cell]`。backward 中 fused 结果仍直接写到
`dq_step2_core` 或 `dq_step2_core_forwarded`，不再经过 `tmp_slots` 和软件 lane
loop。

### 3.3 LU-SGS 算法到 Step2-core 的映射

单条 line 内的 forward 依赖保持严格从左到右：

```text
dq_star[0] = TRSV(lu_a[0], rhs[0])

for i = 1 .. cells - 1:
    mvm        = C[i] * dq_star[i - 1]       // Path A lmat + dotp
    rhs_prime  = rhs[i] - mvm                // patha_pack_sub5
    dq_star[i] = TRSV(lu_a[i], rhs_prime)    // trsv5_lu_spm
```

单条 line 内的 backward 依赖保持严格从右到左：

```text
dq[cells - 1] = dq_star[cells - 1]

for i = cells - 2 .. 0:
    mvm   = B_bar[i] * dq[i + 1]             // Path A lmat + dotp
    dq[i] = dq_star[i] - mvm                 // patha_pack_sub5
```

`interleave` 只改变不同 line 的外层遍历顺序，用来暴露 line 间独立性；它不改变
同一 line 内 `i-1 -> i` 和 `i+1 -> i` 的真实依赖。也就是说 Step2-core 仍是
dependency-aware 的 cell-by-cell LU-SGS，而不是把整条 line 批量重排。

### 3.4 所有中间结果保存位置

这一节按“长期数组、SPM staging、寄存器/内部 buffer、栈上临时数组、最终数组”
五类说明所有中间数据的保存位置。核心结论是：Step2-core 只取消了
`MVM result -> tmp_slots -> tmp[5] -> CPU subtract` 这一段普通内存往返；其他
LU-SGS 依赖结果仍保存在原来的 `dq_star` 和 `dq` 数组中。

#### 3.4.1 长期问题数据和结果数组

benchmark 的长期数据全部在 `Problem` 结构体的 heap 数组里，向量按
`((line * cells) + cell) * 5` 编址，矩阵按
`((line * cells) + cell) * 25` 编址：

| 数据 | 保存位置 | 生命周期 | 用途 |
|---|---|---|---|
| `lu_a` | heap 数组 `p->lu_a` | 初始化后只读 | 每个 cell 的 TRSV/LU 矩阵 |
| `c_mat` | heap 数组 `p->c_mat` | 初始化后只读 | forward 的 `C[i]` |
| `b_bar` | heap 数组 `p->b_bar` | 初始化后只读 | backward 的 `B_bar[i]` |
| `rhs` | heap 数组 `p->rhs` | 初始化后只读 | forward 的原始右端项 |
| `dq_star_step2` | heap 数组 | Step2 baseline 写入 | baseline forward/TRSV 后的 `dq_star` |
| `dq_step2` | heap 数组 | Step2 baseline 写入 | baseline backward 后的最终 `dq` |
| `dq_star_step2_core` | heap 数组 | Step2-core 写入 | fused forward/TRSV 后的 `dq_star` |
| `dq_step2_core` | heap 数组 | Step2-core 写入 | fused backward 后的最终 `dq` |
| `dq_star_step2_core_forwarded` | heap 数组 | Step2-core-forwarded 写入 | RHS-forwarded forward/TRSV 后的 `dq_star` |
| `dq_step2_core_forwarded` | heap 数组 | Step2-core-forwarded 写入 | RHS-forwarded backward 后的最终 `dq` |
| `dq_star_step2_core_forwarded_context` | heap 数组 | Step2-core-forwarded-context 写入 | line-context forward/TRSV 后的 `dq_star`，仍用于最终结果和 correctness |
| `dq_step2_core_forwarded_context` | heap 数组 | Step2-core-forwarded-context 写入 | line-context backward 后的最终 `dq`，仍用于最终结果和 correctness |
| `dq_star_step2_core_forwarded_linebuf` | heap 数组 | Step2-core-forwarded-linebuf 写入 | hardware line-buffer forward/TRSV 后的 `dq_star`，用于最终结果、trace 和 correctness |
| `dq_step2_core_forwarded_linebuf` | heap 数组 | Step2-core-forwarded-linebuf 写入 | hardware line-buffer backward 后的最终 `dq`，用于最终结果、trace 和 correctness |
| `tmp_slots` | heap 数组，每条 line 2 个 5-lane slot | 仅 Step2 baseline 热区复用 | 暂存 Path A MVM 的普通内存结果 |

context 模式额外维护两个每 line 状态结构：

```c
typedef struct {
    double prev_dqstar[5];
    int valid;
    int line_id;
    int cell_id;
} ForwardLineContext;

typedef struct {
    double next_dq[5];
    int valid;
    int line_id;
    int cell_id;
} BackwardLineContext;
```

它们的生命周期只覆盖一次 sweep；每个 sweep 开始时 reset。`prev_dqstar[5]`
保存当前 line 最近完成的 `dq_star[cell]`，`next_dq[5]` 保存当前 line 最近完成的
右侧 `dq[cell+1]`。这两个 context 是相邻 cell 的短生命周期旁路状态，不替代
heap 结果数组；每个 cell 的 `dq_star` 和 `dq` 仍写入各自的 heap result array。

line-buffer 模式不再分配或访问上述软件 context。相同的短生命周期状态改由
`CfdLocalSpm::lineBufferEntries` 保存：

```text
LineBufferEntry {
    bool fwdValid;
    bool bwdValid;
    uint64_t lineId;
    uint64_t fwdCellId;
    uint64_t bwdCellId;
    double prevDqStar[5];
    double nextDq[5];
}
```

条目数由 `GEM5_CFD_LUSGS_LINEBUF_ENTRIES` 或
`--lusgs-linebuf-entries` 控制，默认 16。索引为
`line_id % entries`，tag 为 `lineId`。forward 和 backward 共享同一 line tag，
但各自有独立 valid/cell/value 字段：

```text
forward write : linebuf_wr_fwd5(line, cell, zsrc)
  -> entry.prevDqStar[0:4] = zsrc[0:4]
  -> entry.fwdCellId = cell
  -> entry.fwdValid = true

backward write: linebuf_wr_bwd5(line, cell, zsrc)
  -> entry.nextDq[0:4] = zsrc[0:4]
  -> entry.bwdCellId = cell
  -> entry.bwdValid = true

forward read  : linebuf_rd_fwd5(z5, line, cell)
  -> if tag/cell valid, z5[0:4] = entry.prevDqStar[0:4]

backward read : linebuf_rd_bwd5(z5, line, cell)
  -> if tag/cell valid, z5[0:4] = entry.nextDq[0:4]
```

读 miss 或 invalid 时 gem5 会把目的 Z 寄存器清零并记录 miss/invalid/tag-conflict
统计；正确的 Step2 line-buffer 路径要求普通 cell 的读全部命中。边界 cell
不读 line buffer：forward 边界通过 TRSV5 生成 `dq_star[0]` 后写入 forward
line buffer；backward 边界把 `dq_star[cells-1]` 复制为 `dq[cells-1]` 后写入
backward line buffer。

`tmp_slots` 的索引为：

```text
tmp_slot_at(line, cell) =
  &p->tmp_slots[((line * 2) + (cell & 1)) * 5]
```

因此 baseline 并不是为每个 cell 保存一个永久 MVM 结果，而是每条 line 使用两个
ping/pong 风格的 5-double slot。slot 在当前 cell 的软件减法完成后即可被后续
cell 覆盖。

#### 3.4.2 SPM 中保存的 staged 输入

Path A MVM 和 TRSV5 的输入都先由软件写入 local SPM；这些位置保存的是即将执行
的当前 cell 输入，不保存整条 line 的历史结果：

| 数据 | SPM 地址 | 写入者 | 读取者 | 覆盖时机 |
|---|---|---|---|---|
| Path A matrix 行 0..4 | `SPM_PING_MAT_BASE + row * 40B` | `stage_patha_inputs_to_spm()` | `lmat5_spm` -> `z0..z4` | 下一次 Path A MVM |
| Path A vector | `SPM_PING_VEC_BASE` | `stage_patha_inputs_to_spm()` | `lmat5_spm` -> `z5` | 下一次 Path A MVM |
| TRSV LU matrix | `SPM_TRSV_MAT_BASE` | `stage_trsv5_inputs_to_spm()` | `trsv5_lu_spm` | 下一次 TRSV |
| TRSV RHS | `SPM_TRSV_RHS_BASE` | `stage_trsv5_inputs_to_spm()` | `trsv5_lu_spm` | 下一次 TRSV |

Path A 的 SPM 数据流是：

```text
c_mat/b_bar heap -> SPM_PING_MAT_BASE -> z0..z4
dq_star/dq heap  -> SPM_PING_VEC_BASE -> z5
```

context 模式只改变第二行的 source：

```text
forward  ordinary cell: ForwardLineContext.prev_dqstar -> SPM_PING_VEC_BASE -> z5
backward ordinary cell: BackwardLineContext.next_dq    -> SPM_PING_VEC_BASE -> z5
```

也就是说，本阶段仍然把 vector stage 到 `SPM_PING_VEC_BASE`，仍然执行
`lmat5_spm vec -> z5`。context 不绕过 SPM，也不改变 Path A 指令序列；它只消除
“从 `dq_star_step2_core_forwarded[...]` 或 `dq_step2_core_forwarded[...]`
heap 结果数组读取相邻 vector”的来源路径。

line-buffer 模式进一步改变第二行的硬件入口：

```text
forward  ordinary cell:
  C[i] heap -> SPM_PING_MAT_BASE -> z0..z4
  linebuf.prevDqStar(line,i-1) -> linebuf_rd_fwd5 -> z5

backward ordinary cell:
  B_bar[i] heap -> SPM_PING_MAT_BASE -> z0..z4
  linebuf.nextDq(line,i+1) -> linebuf_rd_bwd5 -> z5
```

因此普通 cell 不再向 `SPM_PING_VEC_BASE` 写 vector，也不再执行
`lmat5_spm vec -> z5`。`SPM_PING_VEC_BASE` 仍保留给 baseline/core/forwarded/context
模式以及后续对照；在 line-buffer 模式的 Path A hot path 中，只有 matrix 5 行
进入 Path A SPM。

TRSV5 的 SPM 数据流是：

```text
lu_a heap       -> SPM_TRSV_MAT_BASE
rhs/rhs_prime  -> SPM_TRSV_RHS_BASE
```

这些 SPM staging buffer 是短生命周期的输入窗口；它们不是 LU-SGS 的全局状态。

#### 3.4.3 Path A MVM 结果保存在哪里

Path A 的 5 个 `dotp_row` 产生 5 个 lane 的 MVM 结果。每个 lane 的保存路径有两层：

```text
dotp_row lane r
  -> PathAResultSlot.lane[r]       // 数值结果，内部 buffer
  -> token register                // O3 依赖/兼容 token
```

内部 buffer 的实际数据结构是 gem5 侧的：

```text
pathAResultQueues[slot_id] : deque<PathAResultSlot>

PathAResultSlot:
  valid
  ready[5]
  seq[5]
  lane[5]
```

`lane[5]` 才是真正的 FP64 MVM 数值结果；`ready[5]` 表示每个 row dot-product
是否已经完成。`token register` 主要让 O3 能建立 dotp 到 pack 的依赖；在
internal-buffer 完整可用时，`pack_acc` 和 `patha_pack_sub5` 都从
`PathAResultSlot.lane[5]` 取数，而不是把 token register 当作主要数据通路。

Step2 baseline 对这个 MVM 结果的消费方式是：

```text
PathAResultSlot.lane[5]
  -> pack_acc
  -> z11
  -> st1d tmp_slots[line, cell & 1]
```

Step2-core 对这个 MVM 结果的消费方式是：

```text
PathAResultSlot.lane[5]
  -> patha_pack_sub5(base)
  -> z11 = base[5] - lane[5]
  -> st1d final update destination
```

两条路径都会在 pack 阶段消费 queue front slot；slot 消费后通过
`pathAPopReadSlot(slot_id)` 从 `pathAResultQueues[slot_id]` 弹出。因此 MVM 结果
只是在 Path A internal result buffer 中短暂存在，不会长期留在硬件 buffer 里。

#### 3.4.4 中间向量减法结果保存在哪里

中间向量减法结果是 `base[5] - mvm[5]`。baseline 和 Step2-core 的保存位置不同：

| 路径 | forward 减法结果 | backward 减法结果 |
|---|---|---|
| Step2 baseline | CPU lane loop 写入栈上 `double rhs_prime[5]` | CPU lane loop 直接写入 heap 上的 `dq_step2[line, cell]` |
| Step2-core | `patha_pack_sub5` 先写 `z11`，随后 `st1d` 到栈上 `double rhs_prime[5]` | `patha_pack_sub5` 先写 `z11`，随后 `st1d` 直接写入 heap 上的 `dq_step2_core[line, cell]` |
| Step2-core-forwarded | `patha_pack_sub5` 写 `z11`，`trsv5_lu_spm_zrhs` 直接读 `z11`，TRSV 结果 `st1d` 到 `dq_star_step2_core_forwarded[line, cell]` | 暂同 Step2-core，直接写入 heap 上的 `dq_step2_core_forwarded[line, cell]` |
| Step2-core-forwarded-context | forward 普通 cell 同 forwarded；TRSV 结果同时写 heap `dq_star_step2_core_forwarded_context[line, cell]` 和 `ForwardLineContext.prev_dqstar[5]` | backward 普通 cell 的 `patha_pack_sub5` 结果同时写 heap `dq_step2_core_forwarded_context[line, cell]` 和 `BackwardLineContext.next_dq[5]` |
| Step2-core-forwarded-linebuf | `patha_pack_sub5` 写 `z11` 后直接进入 `trsv5_lu_spm_zrhs`；TRSV 结果 `z11` 同时写 heap `dq_star_step2_core_forwarded_linebuf[line, cell]` 和 hardware forward line buffer | `patha_pack_sub5` 写 `z11` 后直接写 heap `dq_step2_core_forwarded_linebuf[line, cell]` 和 hardware backward line buffer |

forward 和 backward 的差别来自算法本身：

```text
forward:
  rhs_prime = rhs[i] - C[i] * dq_star[i-1]
  dq_star[i] = TRSV(lu_a[i], rhs_prime)

backward:
  dq[i] = dq_star[i] - B_bar[i] * dq[i+1]
```

所以 forward 的减法结果只是 TRSV 的 RHS，它还不是最终 `dq_star`。代码中
`rhs_prime[5]` 是栈上临时数组：

```text
Step2-core forward:
  patha_pack_sub5 -> z11
  st1d z11 -> rhs_prime[5]          // 栈上临时 RHS
  stage_trsv5_inputs_to_spm(lu_a, rhs_prime)
  trsv5_lu_spm -> result[5]         // trsv5_hardware_counted 内部栈数组
  copy back -> rhs_prime[5]
  copy_vec -> dq_star_step2_core[line, cell]
```

backward 不再需要 TRSV，因此减法结果就是最终解向量：

```text
Step2-core backward:
  patha_pack_sub5 -> z11
  st1d z11 -> dq_step2_core[line, cell]
```

这就是为什么 Step2-core 的 forward 仍然会把 fused result 写一次普通内存
`rhs_prime[5]`。`step2-core-forwarded` 已经消除了这一步：

```text
Step2-core-forwarded forward:
  patha_pack_sub5 -> z11
  trsv5_lu_spm_zrhs(lu_a, z11) -> z11
  st1d z11 -> dq_star_step2_core_forwarded[line, cell]
```

此时 TRSV5 仍需要从 `SPM_TRSV_MAT_BASE` 读取 LU matrix，但 RHS 不再经过
`SPM_TRSV_RHS_BASE`。

#### 3.4.5 forward sweep 的完整数据生命周期

forward 的边界 cell `cell=0` 没有 Path A MVM：

```text
rhs[line,0] heap
  -> value[5] 栈数组
  -> TRSV5 SPM RHS
  -> trsv result[5] 栈数组
  -> value[5]
  -> dq_star_step2_core[line,0] heap
```

forward 的普通 cell `cell>=1` 在 Step2-core 中是：

```text
c_mat[line,cell] heap
  -> Path A SPM matrix
  -> z0..z4

dq_star_step2_core[line,cell-1] heap
  -> Path A SPM vector
  -> z5

dotp_row x5
  -> PathAResultSlot.lane[5]

rhs[line,cell] heap
  -> patha_pack_sub5 functional read base_data[5]

PathAResultSlot.lane[5] + base_data[5]
  -> z11 = rhs - C * dq_star_prev
  -> rhs_prime[5] 栈数组

rhs_prime[5]
  -> TRSV5 SPM RHS
  -> trsv result[5] 栈数组
  -> rhs_prime[5]
  -> dq_star_step2_core[line,cell] heap
```

对应的 Step2 baseline 只有中间 MVM/sub 段不同：

```text
dotp_row x5
  -> PathAResultSlot.lane[5]
  -> pack_acc -> z11
  -> tmp_slots[line, cell & 1]
  -> tmp[5] 栈数组

rhs[line,cell] heap + tmp[5]
  -> CPU lane subtract
  -> rhs_prime[5] 栈数组
```

Step2-core-forwarded 的普通 forward cell 则是：

```text
c_mat[line,cell] heap
  -> Path A SPM matrix
  -> z0..z4

dq_star_step2_core_forwarded[line,cell-1] heap
  -> Path A SPM vector
  -> z5

dotp_row x5
  -> PathAResultSlot.lane[5]

rhs[line,cell] heap
  -> patha_pack_sub5 functional read base_data[5]

PathAResultSlot.lane[5] + base_data[5]
  -> z11 = rhs - C * dq_star_prev

z11
  -> trsv5_lu_spm_zrhs
  -> z11 = TRSV(lu_a, z11)
  -> dq_star_step2_core_forwarded[line,cell] heap
```

Step2-core-forwarded-linebuf 的普通 forward cell 删除 vector SPM staging：

```text
c_mat[line,cell] heap
  -> Path A SPM matrix
  -> z0..z4

line buffer entry(line, cell-1).prevDqStar
  -> linebuf_rd_fwd5
  -> z5

dotp_row x5
  -> PathAResultSlot.lane[5]

rhs[line,cell] heap
  -> patha_pack_sub5 functional read base_data[5]

PathAResultSlot.lane[5] + base_data[5]
  -> z11 = rhs - C * dq_star_prev

z11
  -> trsv5_lu_spm_zrhs
  -> z11 = TRSV(lu_a, z11)
  -> dq_star_step2_core_forwarded_linebuf[line,cell] heap
  -> linebuf_wr_fwd5(line, cell, z11)
```

Step2-core-forwarded-context 的普通 forward cell 在此基础上替换 vector source，
并保留双写结果：

```text
ForwardLineContext.prev_dqstar[5]
  -> Path A SPM vector
  -> z5

dotp_row/patha_pack_sub5/trsv5_lu_spm_zrhs
  -> z11 = dq_star[line,cell]

z11
  -> dq_star_step2_core_forwarded_context[line,cell] heap
  -> ForwardLineContext.prev_dqstar[5]
```

边界 `cell=0` 没有 MVM，仍是：

```text
rhs[line,0] heap
  -> TRSV5
  -> dq_star_step2_core_forwarded_context[line,0] heap
  -> ForwardLineContext.prev_dqstar[5]
```

line-buffer 模式的 forward 边界为：

```text
rhs[line,0] heap
  -> TRSV5
  -> z11 / value[5]
  -> dq_star_step2_core_forwarded_linebuf[line,0] heap
  -> linebuf_wr_fwd5(line, 0, z11)
```

因此 `cell=i+1` 使用的 vector 只来自已经完成并写入 context 的 `cell=i`
结果；没有改变 forward 的从左到右顺序。

#### 3.4.6 backward sweep 的完整数据生命周期

backward 的边界 cell `cell=cells-1` 没有 Path A MVM，也没有减法：

```text
dq_star_step2_core[line,cells-1] heap
  -> dq_step2_core[line,cells-1] heap
```

forwarded 模式下对应为：

```text
dq_star_step2_core_forwarded[line,cells-1] heap
  -> dq_step2_core_forwarded[line,cells-1] heap
```

context 模式下还会初始化 backward context：

```text
dq_star_step2_core_forwarded_context[line,cells-1] heap
  -> dq_step2_core_forwarded_context[line,cells-1] heap
  -> BackwardLineContext.next_dq[5]
```

line-buffer 模式下会初始化 backward line buffer：

```text
dq_star_step2_core_forwarded_linebuf[line,cells-1] heap
  -> dq_step2_core_forwarded_linebuf[line,cells-1] heap
  -> linebuf_wr_bwd5(line, cells-1, z11)
```

backward 的普通 cell 在 Step2-core 中是：

```text
b_bar[line,cell] heap
  -> Path A SPM matrix
  -> z0..z4

dq_step2_core[line,cell+1] heap
  -> Path A SPM vector
  -> z5

dotp_row x5
  -> PathAResultSlot.lane[5]

dq_star_step2_core[line,cell] heap
  -> patha_pack_sub5 functional read base_data[5]

PathAResultSlot.lane[5] + base_data[5]
  -> z11 = dq_star - B_bar * dq_next
  -> dq_step2_core[line,cell] heap
```

Step2-core-forwarded-context 的普通 backward cell 使用 context vector：

```text
BackwardLineContext.next_dq[5]
  -> Path A SPM vector
  -> z5

b_bar[line,cell] heap
  -> Path A SPM matrix
  -> z0..z4

dq_star_step2_core_forwarded_context[line,cell] heap
  -> patha_pack_sub5 functional read base_data[5]

PathAResultSlot.lane[5] + base_data[5]
  -> z11 = dq_star - B_bar * dq_next
  -> dq_step2_core_forwarded_context[line,cell] heap
  -> BackwardLineContext.next_dq[5]
```

Step2-core-forwarded-linebuf 的普通 backward cell 删除 vector SPM staging：

```text
line buffer entry(line, cell+1).nextDq
  -> linebuf_rd_bwd5
  -> z5

b_bar[line,cell] heap
  -> Path A SPM matrix
  -> z0..z4

dq_star_step2_core_forwarded_linebuf[line,cell] heap
  -> patha_pack_sub5 functional read base_data[5]

PathAResultSlot.lane[5] + base_data[5]
  -> z11 = dq_star - B_bar * dq_next
  -> dq_step2_core_forwarded_linebuf[line,cell] heap
  -> linebuf_wr_bwd5(line, cell, z11)
```

因此 `cell=i` 使用的 vector 只来自已经完成并写入 context 的 `cell=i+1`
结果；没有改变 backward 的从右到左顺序。注意 backward 的
`base = dq_star[i]` 仍从 heap 结果数组 functional read，本阶段没有优化
pack-sub base read。

对应的 Step2 baseline 仍然通过 tmp slot：

```text
dotp_row x5
  -> PathAResultSlot.lane[5]
  -> pack_acc -> z11
  -> tmp_slots[line, cell & 1]
  -> tmp[5] 栈数组

dq_star_step2[line,cell] heap + tmp[5]
  -> CPU lane subtract
  -> dq_step2[line,cell] heap
```

因此从保存位置看，Step2-core 的优化点非常窄：

```text
删除:
  MVM result 在 tmp_slots 中的一次 40B store
  tmp_slots 到 tmp[5] 的一次 40B volatile load
  CPU lane subtract 的 5 次标量更新

保留:
  baseline/core/context 的 Path A 输入 SPM staging
  line-buffer 模式的 Path A matrix SPM staging
  PathAResultSlot 内部 MVM buffer
  普通 Step2-core 的 forward rhs_prime[5] 栈上 TRSV RHS
  forwarded 模式的 z11 -> TRSV RHS 直接通路
  dq_star_step2_core / dq_step2_core 以及 forwarded/context/linebuf heap 结果数组
```

### 3.5 line context forwarding 的流水线

`step2-core-forwarded-context` 的流水线可以看作在
`step2-core-forwarded` 外侧增加一个 per-line vector latch。这个 latch 在 C
benchmark 中以 `ForwardLineContext` / `BackwardLineContext` 表示；它不是新的
gem5 macro controller，也不是多 line scheduler。

forward 边界 cell 的阶段如下：

```text
F0 read rhs[line,0] heap
F1 stage rhs 到 TRSV RHS SPM，stage lu_a 到 TRSV matrix SPM
F2 trsv5_lu_spm 求 dq_star[0]
F3 st1d/copy dq_star[0] -> dq_star_step2_core_forwarded_context[line,0]
F4 copy dq_star[0] -> ForwardLineContext.prev_dqstar
F5 valid=1, line_id=line, cell_id=0
```

forward 普通 cell 的阶段如下：

```text
F0 vector source = ForwardLineContext.prev_dqstar
F1 stage C[i] 到 SPM_PING_MAT_BASE
F2 stage prev_dqstar 到 SPM_PING_VEC_BASE
F3 lmat5_spm matrix rows -> z0..z4，lmat5_spm vector -> z5
F4 dotp_row x5 -> PathAResultSlot.lane[5]
F5 patha_pack_sub5(base=rhs[i]) -> z11 = rhs[i] - C[i] * prev_dqstar
F6 trsv5_lu_spm_zrhs(lu_a[i], z11) -> z11 = dq_star[i]
F7 st1d z11 -> dq_star_step2_core_forwarded_context[line,i]
F8 st1d/copy z11 -> ForwardLineContext.prev_dqstar
F9 cell_id=i
```

backward 边界 cell 的阶段如下：

```text
B0 read dq_star_step2_core_forwarded_context[line,cells-1]
B1 copy -> dq_step2_core_forwarded_context[line,cells-1]
B2 copy -> BackwardLineContext.next_dq
B3 valid=1, line_id=line, cell_id=cells-1
```

backward 普通 cell 的阶段如下：

```text
B0 vector source = BackwardLineContext.next_dq
B1 stage B_bar[i] 到 SPM_PING_MAT_BASE
B2 stage next_dq 到 SPM_PING_VEC_BASE
B3 lmat5_spm matrix rows -> z0..z4，lmat5_spm vector -> z5
B4 dotp_row x5 -> PathAResultSlot.lane[5]
B5 patha_pack_sub5(base=dq_star[i]) -> z11 = dq_star[i] - B_bar[i] * next_dq
B6 st1d z11 -> dq_step2_core_forwarded_context[line,i]
B7 st1d/copy z11 -> BackwardLineContext.next_dq
B8 cell_id=i
```

这里的 “heap vector load elided” 指的是 F0/B0 不再从
`dq_star_step2_core_forwarded_context[line,i-1]` 或
`dq_step2_core_forwarded_context[line,i+1]` 取相邻 vector。每个普通 cell
仍会把 context vector 写入 `SPM_PING_VEC_BASE`，因为本阶段明确保留
`lmat5_spm vec -> z5`；每个 cell 的最终 `dq_star/dq` 也仍写回 heap 结果数组。
因此 context 模式减少的是相邻 cell 的 heap result round trip，不是取消 SPM
staging 或最终写回。

### 3.6 hardware line-buffer forwarding 的流水线

`step2-core-forwarded-linebuf` 保留 `step2-core-forwarded-context` 的数学顺序，
但把 context latch 从 C benchmark 中移到 gem5 `CfdLocalSpm`。普通 cell 的
关键差异是：vector 不再经过 `SPM_PING_VEC_BASE`，`z5` 由 line-buffer read
指令直接产生。

forward 边界 cell：

```text
F0 read rhs[line,0] heap
F1 stage rhs 到 TRSV RHS SPM，stage lu_a 到 TRSV matrix SPM
F2 trsv5_lu_spm 求 dq_star[0]
F3 st1d/copy dq_star[0] -> dq_star_step2_core_forwarded_linebuf[line,0]
F4 linebuf_wr_fwd5(line, 0, z11/value)
F5 entry.prevDqStar = dq_star[0], fwdValid=1, fwdCellId=0
```

forward 普通 cell：

```text
F0 stage C[i] 到 SPM_PING_MAT_BASE
F1 stage lu_a[i] 到 SPM_TRSV_MAT_BASE
F2 linebuf_rd_fwd5(z5, line, i-1)
F3 lmat5_spm matrix rows -> z0..z4
F4 dotp_row x5 -> PathAResultSlot.lane[5]
F5 patha_pack_sub5(base=rhs[i]) -> z11 = rhs[i] - C[i] * prev_dqstar
F6 trsv5_lu_spm_zrhs(lu_a[i], z11) -> z11 = dq_star[i]
F7 st1d z11 -> dq_star_step2_core_forwarded_linebuf[line,i]
F8 linebuf_wr_fwd5(line, i, z11)
```

backward 边界 cell：

```text
B0 read dq_star_step2_core_forwarded_linebuf[line,cells-1]
B1 copy -> dq_step2_core_forwarded_linebuf[line,cells-1]
B2 linebuf_wr_bwd5(line, cells-1, z11/value)
B3 entry.nextDq = dq[cells-1], bwdValid=1, bwdCellId=cells-1
```

backward 普通 cell：

```text
B0 stage B_bar[i] 到 SPM_PING_MAT_BASE
B1 linebuf_rd_bwd5(z5, line, i+1)
B2 lmat5_spm matrix rows -> z0..z4
B3 dotp_row x5 -> PathAResultSlot.lane[5]
B4 patha_pack_sub5(base=dq_star[i]) -> z11 = dq_star[i] - B_bar[i] * next_dq
B5 st1d z11 -> dq_step2_core_forwarded_linebuf[line,i]
B6 linebuf_wr_bwd5(line, i, z11)
```

该流水线删除了两段原型开销：

```text
删除 1: context/heap vector -> SPM_PING_VEC_BASE 的 40B store
删除 2: SPM_PING_VEC_BASE -> lmat5_spm vec -> z5 的 40B SPM read
```

它没有删除以下路径：

```text
保留 1: C[i] / B_bar[i] 的 Path A matrix SPM staging
保留 2: rhs[i] / dq_star[i] 作为 pack-sub base 的 functional read
保留 3: lu_a[i] 的 TRSV matrix SPM staging
保留 4: 每个 cell 的最终 heap result writeback
```

因此 line-buffer 模式是 “相邻 vector 硬件旁路 + z5 直连”，不是完整 macro
controller，也不是多 line scheduler。

## 4. 新增指令或模式

本轮保持原 `pack_acc` 和原 `trsv5_lu_spm` 都不变，在旁路上增加两个更窄的
指令变体：

```text
patha_pack_sub5      : Path A MVM result + base vector -> fused 5-lane result
trsv5_lu_spm_zrhs    : SPM LU matrix + Z-register RHS -> TRSV result
linebuf_rd_fwd5      : forward line buffer prevDqStar -> Z register
linebuf_wr_fwd5      : Z register -> forward line buffer prevDqStar
linebuf_rd_bwd5      : backward line buffer nextDq -> Z register
linebuf_wr_bwd5      : Z register -> backward line buffer nextDq
```

它们组合成第一阶段真正的数据前递：

```text
Path A internal result buffer
  -> patha_pack_sub5
  -> z11 holds rhs - C * dq_star_prev
  -> trsv5_lu_spm_zrhs consumes z11 as RHS
  -> z11 holds solved dq_star
```

普通 Step2-core 仍然存在，用于和 `step2-core-forwarded` 对照。
`step2-core-forwarded-context` 不增加新 ISA；它复用同一组
`lmat5_spm`、`dotp_row`、`patha_pack_sub5` 和 `trsv5_lu_spm_zrhs`，只在
benchmark 中选择 context vector source，并把结果额外写入 per-line context。
`step2-core-forwarded-linebuf` 增加 line-buffer ISA，用
`linebuf_rd_*5 -> z5` 替代软件 context vector staging 和 vector `lmat5_spm`。

当前可选模式为：

```text
step2                         : tmp slot + 软件减法 baseline
step2-core                    : MVM -> patha_pack_sub5 -> result
step2-core-forwarded          : forward 中 pack_sub RHS 直接进入 TRSV5
step2-core-forwarded-context  : forwarded + dq_star/dq line context vector source
step2-core-forwarded-linebuf  : hardware line buffer + z5 direct vector input
all                           : 依次运行以上五种模式
```

### 4.1 `patha_pack_sub5` 编码和语义

`patha_pack_sub5` 编码使用 pack/TRSV 邻近空间中的独立 tag：

```text
[24:23]=11 [22:20]=000 [19:15]=00001
[14:10]=xbase [9:5]=10000 [4:0]=zd
```

语义为：

```text
Z[zd][0:4] = mem5[X[xbase]] - PathAInternalResult[0:4]
Z[zd][5:7] = 0
```

数值 MVM 结果来自既有 Path A internal result buffer；编码中的 token base 仍作为
fallback 依赖源。当前 Step2-core 使用 `z11/x16`，对应 `x16..x20` 依赖 token。
指令消费 result-buffer slot，并把 fused 结果写入 Z 寄存器。原 `pack_acc`
保持 `Z = MVM result`，不受影响。

benchmark 侧编码函数为：

```c
enc_pack_sub_acc(zd, base, xn)
```

当前 inline asm 的寄存器约定：

```text
x21 : Path A matrix SPM base
x23 : Path A vector SPM base
x12 : base vector memory address
x11 : output vector memory address
z0..z4 : matrix rows
z5     : vector
z11    : fused update result
p0.d/vl5 : 只启用 5 个 double lane
x16..x20 : 当前 z11 路径的 Path A dependency/fallback token
```

context 模式下，`vector` 参数可能来自：

```text
VECTOR_FROM_HEAP
VECTOR_FROM_FORWARD_CONTEXT
VECTOR_FROM_BACKWARD_CONTEXT
```

但无论 source 是哪一种，`stage_patha_inputs_to_spm_with_source()` 都会把
`vector[0:4]` 写入 `SPM_PING_VEC_BASE`。`VectorSource` 当前主要用于区分
benchmark 数据流和统计口径，不改变 Path A 指令序列。

decode 路径位于 `custom_cfd.isa` 的 compute instruction 分支。当
`tag == 0b00001` 且 `field == 0b10000` 时构造：

```text
new DSAPackSubAcc(machInst, zd, base, field)
```

这样可以保证原 `pack_acc` 编码仍走旧路径，TRSV5 launch/wait 和其他 Step3/Step4
编码也不被占用。

### 4.2 `patha_pack_sub5` gem5 执行语义

`DSAPackSubAcc` 复用 `CFDDSAPackOp`。构造函数把 `base` 作为第一个源寄存器，
把 5 个 token 源寄存器作为后续源操作数，并把 `zd` 作为唯一目的 Z 寄存器。
这使 O3 能看到 pack-sub 必须等待前面的 Path A dotp 结果完成。

执行函数的关键步骤是：

```text
base_ea = X[base]
base_data[0:4] = functional_read_40B(base_ea)

if Path A internal buffer 可用且 slot 完整:
    acc[0:4] = result_buffer.front().lane[0:4]
    Z[zd][j] = base_data[j] - acc[j]
    pop result buffer slot
else:
    acc[0:4] = reinterpret_double(X[token_regs])
    Z[zd][j] = base_data[j] - acc[j]

recordPathAFusedSub()
```

internal buffer 路径使用 `pathASlotForPack(zd)` 找到和目的 Z 寄存器对应的
result queue。只有 queue front slot `valid` 且 5 个 lane 都 ready 时，才消费
buffer 中的 MVM 结果；消费成功后记录 `pathAResultBufferPacks`，并释放 slot。
如果 slot 不完整，则记录 buffer stall，并使用 token fallback 语义保持功能路径
可执行。

`recordPathAFusedSub()` 对应 gem5 统计项 `pathAFusedSubOps`。benchmark 侧同时
记录 `fused_forward_updates`、`fused_backward_updates`、`fused_sub_ops` 和
`hardware_vector_sub`，用于证明 fused 路径替代了原软件 vector subtract。

### 4.3 `trsv5_lu_spm_zrhs` 编码和语义

`trsv5_lu_spm_zrhs` 是 TRSV5 的 RHS-forwarded 变体。编码为：

```text
[24:23]=11 [22]=0 [21:20]=00
[19:15]=xlu [14:10]=zrhs [9:5]=10001 [4:0]=zd
```

当前 decoder 在 compute subop `00` 中优先匹配：

```text
field == 0b10001 && lu_base in x21..x24
```

满足条件时构造：

```text
new DSATrsv5LuZRHS(machInst, zd, lu_base, rhs_base)
```

指令源操作数是 `X[lu_base]` 和 `Z[zrhs]`，目的操作数是 `Z[zd]`。执行语义为：

```text
lu_ea = X[lu_base]
lu[5x5] = functional_read_200B(lu_ea)
rhs[0:4] = Z[zrhs][0:4]
sol[0:4] = cfdTrsv5Solve(lu, rhs)
Z[zd][0:4] = sol[0:4]
Z[zd][5:7] = 0
```

普通 `trsv5_lu_spm` 仍然从 `SPM_TRSV_RHS_BASE` 读取 RHS；新指令只删除
forward 热路径上 `rhs_prime[5]` 到 TRSV RHS SPM 的那一段 staging。TRSV LU
matrix 仍由 `stage_patha_and_trsv_lu_to_spm()` 写入 `SPM_TRSV_MAT_BASE`。

### 4.4 line-buffer 指令编码和语义

line-buffer 指令使用同一个 compute subop `00`，由 `[9:5]` 的 field 区分方向和
读写：

```text
[24:23]=11 [22]=0 [21:20]=00
[19:15]=xline [14:10]=xcell [9:5]=field [4:0]=z

field=10010 : linebuf_rd_fwd5  z[zd],   xline, xcell
field=10011 : linebuf_wr_fwd5  z[zsrc], xline, xcell
field=10100 : linebuf_rd_bwd5  z[zd],   xline, xcell
field=10101 : linebuf_wr_bwd5  z[zsrc], xline, xcell
```

执行语义为：

```text
read:
  line_id = X[xline]
  cell_id = X[xcell]
  value   = CfdLocalSpm::readLineBuffer(direction, line_id, cell_id)
  Z[zd][0:4] = value[0:4]
  Z[zd][5:7] = 0

write:
  line_id = X[xline]
  cell_id = X[xcell]
  value   = Z[zsrc][0:4]
  CfdLocalSpm::writeLineBuffer(direction, line_id, cell_id, value)
```

`readLineBuffer()` 会验证 `line_id` tag、方向 valid bit 和 `cell_id`。全部匹配
时记录 hit；否则记录 miss/invalid，tag 不匹配时额外记录
`linebufTagConflicts`。当前 Step2-linebuf hot path 不依赖 miss fallback，正确
运行时 miss/invalid/conflict 都应为 0。

`DSALineBufRead5` 有两个整数源和一个 Z 目的寄存器；`DSALineBufWrite5` 有两个
整数源、一个 Z 源寄存器且无目的寄存器。二者使用 `CFDDSALineBufOp`，O3 配置
为 1-cycle pipelined FU。

### 4.5 benchmark inline asm

普通 Step2-core 的 fused MVM-sub 路径是：

```text
stage_patha_inputs_to_spm(matrix, vector)
full_barrier()

mov x21, SPM_PING_MAT_BASE
mov x23, SPM_PING_VEC_BASE
mov x12, base
mov x11, out
ptrue p0.d, vl5

lmat5_spm row0 -> z0
lmat5_spm row1 -> z1
lmat5_spm row2 -> z2
lmat5_spm row3 -> z3
lmat5_spm row4 -> z4
lmat5_spm vec  -> z5

dotp_row row0
dotp_row row1
dotp_row row2
dotp_row row3
dotp_row row4

patha_pack_sub5 z11, [x12], x16
st1d {z11.d}, p0, [x11]
dsb sy
isb
```

forwarded 热路径由 `patha_mvm_sub5_trsv5_forwarded()` 实现：

```text
stage_patha_and_trsv_lu_to_spm(matrix, vector, lu)
full_barrier()

mov x21, SPM_PING_MAT_BASE
mov x23, SPM_PING_VEC_BASE
mov x22, SPM_TRSV_MAT_BASE
mov x12, rhs_base
mov x11, dq_star_out
ptrue p0.d, vl5

lmat5_spm row0 -> z0
lmat5_spm row1 -> z1
lmat5_spm row2 -> z2
lmat5_spm row3 -> z3
lmat5_spm row4 -> z4
lmat5_spm vec  -> z5

dotp_row row0
dotp_row row1
dotp_row row2
dotp_row row3
dotp_row row4

patha_pack_sub5 z11, [x12], x16
trsv5_lu_spm_zrhs z11, [x22], z11
st1d {z11.d}, p0, [x11]
dsb sy
isb
```

context forward 热路径使用同一序列，只是 `x23` 对应的 staged vector 来自
`ForwardLineContext.prev_dqstar`。TRSV5 求得 `dq_star[i]` 后，AArch64 fast path
在同一个 asm block 中执行两次 `st1d {z11.d}`：

```text
st1d {z11.d}, p0, [x11]   // heap result array
st1d {z11.d}, p0, [x10]   // ForwardLineContext.prev_dqstar
```

context backward 热路径同理，`x23` 的 staged vector 来自
`BackwardLineContext.next_dq`，`patha_pack_sub5` 的 `z11` 同时写入 heap
`dq_step2_core_forwarded_context` 和 `BackwardLineContext.next_dq`。这样可以避免
先写 heap、再由 C 代码从 heap 读回 context 的额外往返。

line-buffer forward 热路径由 `patha_mvm_sub5_trsv5_forwarded_linebuf_fwd()` 实现。
它只 stage matrix 和 TRSV LU，不 stage Path A vector：

```text
stage_patha_matrix_and_trsv_lu_to_spm(matrix, lu)
full_barrier()

mov x21, SPM_PING_MAT_BASE
mov x22, SPM_TRSV_MAT_BASE
mov x12, rhs_base
mov x11, dq_star_out
mov x9,  line_id
mov x10, read_cell_id
ptrue p0.d, vl5

linebuf_rd_fwd5 z5, x9, x10
lmat5_spm row0 -> z0
lmat5_spm row1 -> z1
lmat5_spm row2 -> z2
lmat5_spm row3 -> z3
lmat5_spm row4 -> z4

dotp_row row0
dotp_row row1
dotp_row row2
dotp_row row3
dotp_row row4

patha_pack_sub5 z11, [x12], x16
trsv5_lu_spm_zrhs z11, [x22], z11
st1d {z11.d}, p0, [x11]
mov x10, write_cell_id
linebuf_wr_fwd5 z11, x9, x10
dsb sy
isb
```

line-buffer backward 热路径由 `patha_mvm_sub5_linebuf_bwd()` 实现：

```text
stage_patha_matrix_to_spm(matrix)
full_barrier()

mov x21, SPM_PING_MAT_BASE
mov x12, dq_star_base
mov x11, dq_out
mov x9,  line_id
mov x10, read_cell_id
ptrue p0.d, vl5

linebuf_rd_bwd5 z5, x9, x10
lmat5_spm row0 -> z0
lmat5_spm row1 -> z1
lmat5_spm row2 -> z2
lmat5_spm row3 -> z3
lmat5_spm row4 -> z4

dotp_row row0..row4
patha_pack_sub5 z11, [x12], x16
st1d {z11.d}, p0, [x11]
mov x10, write_cell_id
linebuf_wr_bwd5 z11, x9, x10
dsb sy
isb
```

和 baseline 相比：

```text
baseline  : pack_acc -> st1d tmp -> load tmp -> CPU subtract -> stage TRSV RHS
core      : pack_sub -> st1d rhs_prime/dq
forwarded : pack_sub -> z11 -> trsv5_lu_spm_zrhs -> st1d dq_star
context   : context vector -> same forwarded/core asm -> st1d heap + st1d context
linebuf   : linebuf_rd -> z5 -> same Path A/pack/TRSV -> st1d heap + linebuf_wr
```

当前 base 使用 `SETranslatingPortProxy` functional read。它表示 base 已在核心侧
可用的 Step2-core 上界路径，不建模普通内存 load 延迟，也不提供相邻未提交 store
的 forwarding。正式 benchmark 的只读 `rhs` 和已经提交的 `dq_star/dq` 数组在
热区前或前序 cell 后可见，因此功能正确；后续 realistic 模型应把 base 放入
local SPM/input buffer 或建立 timing read。

### 4.6 新增统计口径

benchmark 侧新增或复用的关键统计：

```text
fusedForwardUpdate / fusedBackwardUpdate
fusedSubOps / hardwareVectorSub
tmpResultStores / tmpResultLoads
trsv5RhsForwarded
trsv5RhsSpmStageElided
step2CoreForwardRhsStackStoreElided
trsv5RhsForwardConsumed
trsv5RhsForwardInvalid
trsv5RhsForwardStallCycles
forwardContextHits / forwardContextMisses
forwardDqstarHeapVectorLoadElided
forwardContextVectorStages / forwardContextUpdates
forwardContextInvalid / forwardContextStallCycles
backwardContextHits / backwardContextMisses
backwardDqHeapVectorLoadElided
backwardContextVectorStages / backwardContextUpdates
backwardContextInvalid / backwardContextStallCycles
lineContextForwardingEnabled
lineContextTotalHits / lineContextTotalMisses
linebufForwardReads / linebufForwardWrites
linebufBackwardReads / linebufBackwardWrites
linebufForwardHits / linebufForwardMisses
linebufBackwardHits / linebufBackwardMisses
linebufSpmVectorStagesElided
linebufLmatVecLoadsElided
linebufContextMemoryStoresElided
linebufContextMemoryLoadsElided
linebufTagConflicts / linebufInvalidReads / linebufStallCycles
linebufForwardingEnabled
```

gem5 local SPM 侧新增执行观测统计：

```text
system.cpu.local_spm.pathAFusedSubOps
system.cpu.local_spm.trsv5RhsForwarded
system.cpu.local_spm.trsv5RhsSpmStageElided
system.cpu.local_spm.trsv5RhsForwardConsumed
system.cpu.local_spm.trsv5RhsForwardInvalid
system.cpu.local_spm.trsv5RhsForwardStallCycles
system.cpu.local_spm.linebufForwardReads
system.cpu.local_spm.linebufForwardWrites
system.cpu.local_spm.linebufBackwardReads
system.cpu.local_spm.linebufBackwardWrites
system.cpu.local_spm.linebufForwardHits
system.cpu.local_spm.linebufForwardMisses
system.cpu.local_spm.linebufBackwardHits
system.cpu.local_spm.linebufBackwardMisses
system.cpu.local_spm.linebufTagConflicts
system.cpu.local_spm.linebufInvalidReads
system.cpu.local_spm.linebufStallCycles
```

`recordTrsv5RhsForwardedExecute()` 仍记录 TRSV solve、LU matrix read 和 output
write，但不会增加 `trsv5InputVectorBytes`，因为 RHS 来自 Z 寄存器而不是 SPM。
forward 边界 `cell=0` 没有 Path A MVM，仍使用原 `trsv5_lu_spm`，所以 forwarded
模式下 `trsv5InputVectorBytes` 不会归零，只剩每条 line 每次 sweep 的边界 RHS。

context 统计的理论值为：

```text
ordinary_cells_per_sweep = lines * (cells - 1)
all_cells_per_sweep      = lines * cells

forwardContextHits                  = sweeps * ordinary_cells_per_sweep
forwardContextMisses                = 0
forwardDqstarHeapVectorLoadElided   = forwardContextHits
forwardContextVectorStages          = forwardContextHits
forwardContextUpdates               = sweeps * all_cells_per_sweep

backwardContextHits                 = sweeps * ordinary_cells_per_sweep
backwardContextMisses               = 0
backwardDqHeapVectorLoadElided      = backwardContextHits
backwardContextVectorStages         = backwardContextHits
backwardContextUpdates              = sweeps * all_cells_per_sweep

lineContextTotalHits                = forwardContextHits + backwardContextHits
lineContextTotalMisses              = 0
```

边界 cell 不计 miss：forward 的 `cell=0` 是初始化 `ForwardLineContext`，
backward 的 `cell=cells-1` 是初始化 `BackwardLineContext`。

注意：benchmark 打印的 `lusgs.linebuf*` 是按 LU-SGS cell 流水线维护的
architectural/model 计数；`system.cpu.local_spm.linebuf*` 是 O3 execute 侧观察到的
instruction execute 次数，可能包含 speculation 或 re-execute，因此可能大于
benchmark 计数。功能闭合和算法期望以 benchmark 计数为准；gem5 侧 stats 用来确认
line-buffer FU/SimObject 路径确实被执行，并观察 miss/invalid/tag-conflict。

benchmark line-buffer 统计的理论值为：

```text
ordinary_cells = sweeps * lines * (cells - 1)
all_cells      = sweeps * lines * cells

linebufForwardReads              = ordinary_cells
linebufForwardWrites             = all_cells
linebufBackwardReads             = ordinary_cells
linebufBackwardWrites            = all_cells
linebufForwardHits               = ordinary_cells
linebufBackwardHits              = ordinary_cells
linebufForwardMisses             = 0
linebufBackwardMisses            = 0
linebufInvalidReads              = 0
linebufTagConflicts              = 0
linebufStallCycles               = 0

linebufSpmVectorStagesElided     = 2 * ordinary_cells
linebufLmatVecLoadsElided        = 2 * ordinary_cells
linebufContextMemoryStoresElided = 2 * all_cells
linebufContextMemoryLoadsElided  = 2 * ordinary_cells
```

`linebufForwardWrites` / `linebufBackwardWrites` 包含边界 cell，因为边界 cell
分别初始化 `prevDqStar` 和 `nextDq`。`linebufForwardReads` /
`linebufBackwardReads` 只覆盖普通 cell。`linebufSpmVectorStagesElided` 和
`linebufLmatVecLoadsElided` 以 forward + backward 两个方向合计，因此是
`2 * ordinary_cells`。

## 5. 正确性与回归结果

### 5.1 功能正确性

`results/cfd_dsa/runs/20260707T025605Z_step2-core-forwarded-correctness` 使用
`lines=1, cells=17, sweeps=1, mode=all, check=1`：

| 对比 | max abs | max rel | mismatch |
|---|---:|---:|---:|
| Step2 vs reference | 5.551115e-17 | 2.778490e-16 | 0 |
| Step2-core vs reference | 5.551115e-17 | 2.778490e-16 | 0 |
| Step2-core vs Step2 | 0 | 0 | 0 |
| Step2-core-forwarded vs reference | 5.551115e-17 | 2.778490e-16 | 0 |
| Step2-core-forwarded vs Step2-core | 0 | 0 | 0 |

该 run 的结构计数：

```text
expected forward/backward MVM = 16 / 16
expected hardware TRSV5       = 17
step2_core_forwarded fused    = 16 forward + 16 backward
step2_core_forwarded tmp      = 0 stores / 0 loads
step2_core_forwarded sw sub   = 0
trsv5RhsForwarded             = 16
trsv5RhsSpmStageElided        = 16
trsv5RhsForwardConsumed       = 16
trsv5RhsForwardInvalid        = 0
```

输出包含：

```text
LUSGS_STEP2_CORE_PASS
LUSGS_STEP2_CORE_FORWARDED_PASS
LUSGS_STEP2_PASS
```

额外回归：

```text
results/cfd_dsa/runs/20260707T025653Z_step2-core-forwarded-cell1
results/cfd_dsa/runs/20260707T025654Z_step2-core-forwarded-8x64-check
```

均通过。

line-context 版本额外回归：

```text
results/cfd_dsa/runs/20260708T041554Z_step2-context-correctness-1x17-final-fastctx
results/cfd_dsa/runs/20260708T035911Z_step2-context-cell1-fastctx
results/cfd_dsa/runs/20260708T040628Z_step2-context-8x64-check-fastctx
```

`step2-core-forwarded-context` 的关键 correctness 结果：

```text
step2_core_forwarded_context_vs_reference.mismatch_count = 0
step2_core_forwarded_context_vs_step2_core_forwarded.mismatch_count = 0
step2_core_forwarded_context_vs_step2_core.mismatch_count = 0
LUSGS_STEP2_CORE_FORWARDED_CONTEXT_PASS
LUSGS_STEP2_PASS
```

`8x64` 全模式回归验证了 `--lusgs-mode=all` 同时覆盖 Step2 baseline、
Step2-core、Step2-core-forwarded 和 Step2-core-forwarded-context。

hardware line-buffer 版本额外回归：

```text
m5out/lusgs-step2-linebuf-correctness-1x17
m5out/lusgs-step2-linebuf-cell1
m5out/lusgs-step2-linebuf-8x64-check
```

`1x17` correctness run 的关键结果：

```text
step2_core_forwarded_linebuf_vs_reference.mismatch_count = 0
step2_core_forwarded_linebuf_vs_step2_core_forwarded_context.mismatch_count = 0
step2_core_forwarded_linebuf_vs_step2_core_forwarded.mismatch_count = 0
step2_core_forwarded_linebuf_vs_step2_core.mismatch_count = 0
LUSGS_STEP2_CORE_FORWARDED_LINEBUF_PASS
LUSGS_STEP2_PASS
```

该 run 的 line-buffer 结构计数：

```text
linebufForwardReads / Writes   = 16 / 17
linebufBackwardReads / Writes  = 16 / 17
linebufForwardHits / Misses    = 16 / 0
linebufBackwardHits / Misses   = 16 / 0
linebufInvalidReads            = 0
linebufTagConflicts            = 0
linebufSpmVectorStagesElided   = 32
linebufLmatVecLoadsElided      = 32
linebufContextMemoryStoresElided = 34
linebufContextMemoryLoadsElided  = 32
```

`cell=1` 边界退化 run 验证没有普通 cell 时不会产生 line-buffer read 或 miss：

```text
linebufForwardReads / Writes   = 0 / 1
linebufBackwardReads / Writes  = 0 / 1
linebufForwardHits / Misses    = 0 / 0
linebufBackwardHits / Misses   = 0 / 0
linebufInvalidReads            = 0
linebufTagConflicts            = 0
```

`8x64` 全模式回归验证了 `--lusgs-mode=all` 同时覆盖 Step2 baseline、
Step2-core、Step2-core-forwarded、Step2-core-forwarded-context 和
Step2-core-forwarded-linebuf；line-buffer 计数为：

```text
linebufForwardReads / Writes   = 504 / 512
linebufBackwardReads / Writes  = 504 / 512
linebufForwardHits / Misses    = 504 / 0
linebufBackwardHits / Misses   = 504 / 0
linebufInvalidReads            = 0
linebufTagConflicts            = 0
linebufSpmVectorStagesElided   = 1008
linebufLmatVecLoadsElided      = 1008
linebufContextMemoryStoresElided = 1024
linebufContextMemoryLoadsElided  = 1008
```

### 5.2 decode-exclusive

`results/cfd_dsa/runs/20260707T025625Z_step2-forwarded-decode` 覆盖新编码：

```text
decode.trsv5 = PASS
decode.trsv5_lu_spm_zrhs = PASS
DECODE_EXCLUSIVE_PASS
```

### 5.3 构建验证

验证过的构建命令：

```text
aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -march=armv8-a+sve \
  -o /tmp/test_cfd_lusgs_patha_step2_arm \
  projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step2.c

aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -march=armv8-a+sve \
  -o /tmp/test_cfd_dsa_decode_exclusive_arm \
  projects/cfd_dsa/benchmarks/common/test_cfd_dsa_decode_exclusive.c

scons -Q build/ARM/gem5.opt -j1
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 \
  lusgs-step2 decode-exclusive
python3 -m py_compile \
  projects/cfd_dsa/configs/run_cfd_dsa.py \
  projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step2.py
git diff --check
```

`scons -j2` 的最后链接阶段曾被系统 `SIGKILL` 终止，随后 `scons -j1` 完整通过；
`build/ARM/gem5.opt` 已验证为可执行 ELF。

## 6. 性能结果

### 6.1 Correctness 规模

`lines=1, cells=17, sweeps=1, mode=all`：

| 指标 | Step2 | Step2-core | Step2-core-forwarded |
|---|---:|---:|---:|
| total cycles | 10,154 | 9,388 | 7,760 |
| cycles/full cell | 597.294 | 552.235 | 456.471 |
| cycles/MVM | 317.313 | 293.375 | 242.500 |
| MVM / TRSV | 32 / 17 | 32 / 17 | 32 / 17 |
| tmp stores / loads | 32 / 32 | 0 / 0 | 0 / 0 |
| software vector sub | 32 | 0 | 0 |
| fused sub | 0 | 32 | 32 |
| TRSV RHS forwarded | 0 | 0 | 16 |
| TRSV RHS SPM stage elided | 0 | 0 | 16 |

速度变化：

```text
Step2-core vs Step2               = 1.081594x
Step2-core-forwarded vs Step2     = 1.308505x
Step2-core-forwarded vs Step2-core= 1.209794x
```

小规模 run 的 forwarded 收益偏大，因为每个 cell 的固定 staging、barrier 和调用开销
占比更高；稳态结论以 8x256x50 为主。

### 6.2 Steady-state 规模

最终稳态对照配置统一为
`lines=8, cells=256, interleave=4, sweeps=50`。主要证据目录：

```text
results/cfd_dsa/runs/20260708T034401Z_step2-context-steady-step2-8x256-i4-s50-minctxstores
results/cfd_dsa/runs/20260708T034401Z_step2-context-steady-core-8x256-i4-s50-minctxstores
results/cfd_dsa/runs/20260708T035148Z_step2-context-steady-forwarded-8x256-i4-s50-minctxstores
results/cfd_dsa/runs/20260708T035924Z_step2-context-steady-forwarded-context-8x256-i4-s50-fastctx
```

| mode | total cycles | cycles/full cell | cycles/MVM | cycles/TRSV | MVM count | TRSV count | tmp stores/loads | software vector sub | fused sub | TRSV RHS forwarded | TRSV RHS invalid | fwd ctx hits/misses | bwd ctx hits/misses | fwd heap vec elided | bwd heap vec elided | line ctx hits/misses | speedup vs Step2 | speedup vs Step2-core | speedup vs forwarded |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| step2 | 88,600,962 | 865.244 | 434.318 | 865.244 | 204,000 | 102,400 | 204,000 / 204,000 | 204,000 | 0 | 0 | 0 | 0 / 0 | 0 / 0 | 0 | 0 | 0 / 0 | 1.000000x | 0.977006x | 0.903029x |
| step2-core | 86,563,640 | 845.348 | 424.332 | 845.348 | 204,000 | 102,400 | 0 / 0 | 0 | 204,000 | 0 | 0 | 0 / 0 | 0 / 0 | 0 | 0 | 0 / 0 | 1.023536x | 1.000000x | 0.924282x |
| step2-core-forwarded | 80,009,202 | 781.340 | 392.202 | 781.340 | 204,000 | 102,400 | 0 / 0 | 0 | 204,000 | 102,000 | 0 | 0 / 0 | 0 / 0 | 0 | 0 | 0 / 0 | 1.107385x | 1.081921x | 1.000000x |
| step2-core-forwarded-context | 80,072,866 | 781.962 | 392.514 | 781.962 | 204,000 | 102,400 | 0 / 0 | 0 | 204,000 | 102,000 | 0 | 102,000 / 0 | 102,000 / 0 | 102,000 | 102,000 | 204,000 / 0 | 1.106504x | 1.081061x | 0.999205x |

`102,000 = lines * sweeps * (cells - 1)`，只覆盖普通 cell；forward 边界
`cell=0` 和 backward 边界 `cell=cells-1` 只初始化 context，不计 miss。
context 模式相对 `step2-core-forwarded` 的周期为 `0.999205x`，属于基本持平：
它已经证明相邻 heap vector load 被消除，但软件 context 版本仍保留
`SPM_PING_VEC_BASE` staging、heap 结果写回、context vector 写入和 barrier。

hardware line-buffer 版本继续删除普通 cell 的 `SPM_PING_VEC_BASE` vector staging
和 `lmat5_spm vec -> z5`，稳态结果见下方新增表。进一步拉开性能还需要把 Path A
matrix/TRSV LU staging、barrier、pack-sub base read 和 line 间调度继续推进到
SPM ping-pong、多 line scheduler 与 TRSV5 internal pipeline。

### 6.3 Hardware line-buffer 稳态补充

line-buffer 稳态用例单独运行目标模式：

```text
m5out/lusgs-step2-linebuf-steady-linebuf-8x256-i4-s50
mode = step2-core-forwarded-linebuf
lines = 8, cells = 256, interleave = 4, sweeps = 50
```

该 run 用于性能和结构计数，不作为新的 reference correctness 证据；严格
correctness 已由上面的 `mode=all` 小规模回归覆盖。

| mode | total cycles | cycles/full cell | cycles/MVM | cycles/TRSV | MVM count | TRSV count | fused sub | TRSV RHS forwarded | linebuf fwd reads/writes | linebuf bwd reads/writes | linebuf hits/misses | vector SPM stages elided | lmat vec loads elided | context stores/loads elided | speedup vs Step2 | speedup vs Step2-core | speedup vs forwarded | speedup vs context |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| step2-core-forwarded-linebuf | 77,347,484 | 755.347 | 379.154 | 755.347 | 204,000 | 102,400 | 204,000 | 102,000 | 102,000 / 102,400 | 102,000 / 102,400 | 204,000 / 0 | 204,000 | 204,000 | 204,800 / 204,000 | 1.145492x | 1.119153x | 1.034412x | 1.035236x |

关键闭合项：

```text
linebufForwardReads / Writes     = 102000 / 102400
linebufBackwardReads / Writes    = 102000 / 102400
linebufForwardHits / Misses      = 102000 / 0
linebufBackwardHits / Misses     = 102000 / 0
linebufInvalidReads              = 0
linebufTagConflicts              = 0
linebufStallCycles               = 0
linebufSpmVectorStagesElided     = 204000
linebufLmatVecLoadsElided        = 204000
linebufContextMemoryStoresElided = 204800
linebufContextMemoryLoadsElided  = 204000
```

与 `step2-core-forwarded-context` 相比，line-buffer 模式从 `80,072,866` cycles
降到 `77,347,484` cycles，收益约 `1.035236x`。这说明删除 vector SPM staging
和 vector `lmat5_spm` 已经带来可测收益；剩余主要开销仍来自 matrix/LU staging、
barrier、TRSV coarse latency、pack-sub base read 和软件循环控制。

## 7. 仍未解决的问题

- `step2-core-forwarded-linebuf` 已把软件 `ForwardLineContext` /
  `BackwardLineContext` 收敛为 `CfdLocalSpm` 侧硬件 line buffer，并让普通 cell
  的 Path A vector 直接进入 `z5`。
- 当前 line buffer 仍是 functional forwarding resource；没有建立多端口 SRAM、
  banking、DMA 或跨 line arbitration 模型。
- backward 的 `base = dq_star[i]` 仍从 heap 结果数组 functional read，本阶段没有
  优化 pack-sub base read。
- forward 边界 cell 仍走原 `trsv5_lu_spm`，因为它没有前置 MVM/sub RHS。
- TRSV5 仍在 execute 中一次完成，`trsv5_lat` 只是 O3-visible coarse latency。
- Path A matrix 和 TRSV LU matrix 仍由软件 stage 到 SPM，并保留 `dsb/isb`。
  line-buffer 模式只删除 Path A vector staging。
- pack-sub 的 base functional read 是核心上界，不是 timing SPM/DRAM 访问模型。
- `interleave` 仍是软件循环顺序，没有硬件级多 line scheduler。
- SPM ping-pong、多 outstanding line scheduling 和 TRSV5 internal pipeline 尚未实现。

## 8. 下一步建议

1. 在保持单 line 前后依赖语义的前提下，引入 SPM ping-pong 和更细粒度 barrier
   token，把 Path A staging、Path A compute 和 TRSV LU staging 分离。
2. 在已有 `interleave` 软件顺序基础上，设计多 line scheduler，把 independent
   lines 的 Path A/TRSV slots 变成可调度队列。
3. 将 TRSV5 从 execute-time functional solve 推进到 internal pipeline，至少区分
   RHS accept、forward solve steps、result writeback 和 backpressure。
4. 将 `patha_pack_sub5` 的 base functional read 收敛为 local input buffer 或
   timing SPM read，避免长期依赖 core-ideal 上界模型。

## 9. 构建与运行

```bash
scons -Q build/ARM/gem5.opt -j1
python3 projects/cfd_dsa/tools/build_benchmarks.py --opt=-O2 \
  lusgs-step2 decode-exclusive

python3 projects/cfd_dsa/tools/run_experiment.py \
  --target lusgs-step2 --case step2-context-correctness-1x17 -- \
  --lusgs-mode=all --lusgs-lines=1 --lusgs-cells=17 \
  --lusgs-interleave=1 --lusgs-check=1

python3 projects/cfd_dsa/tools/run_experiment.py \
  --target lusgs-step2 --case step2-context-steady-forwarded-context-8x256-i4-s50 -- \
  --lusgs-mode=step2-core-forwarded-context --lusgs-lines=8 --lusgs-cells=256 \
  --lusgs-interleave=4 --bench-iters=50 --lusgs-check=0 \
  --max-ticks=200000000000

build/ARM/gem5.opt \
  --outdir=m5out/lusgs-step2-linebuf-steady-linebuf-8x256-i4-s50 \
  projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step2.py \
  --lusgs-mode step2-core-forwarded-linebuf \
  --lusgs-lines 8 --lusgs-cells 256 --lusgs-interleave 4 \
  --bench-iters 50
```

本报告的主要证据目录：

```text
results/cfd_dsa/runs/20260707T025605Z_step2-core-forwarded-correctness
results/cfd_dsa/runs/20260707T025625Z_step2-forwarded-decode
results/cfd_dsa/runs/20260707T025653Z_step2-core-forwarded-cell1
results/cfd_dsa/runs/20260707T025654Z_step2-core-forwarded-8x64-check
results/cfd_dsa/runs/20260707T031147Z_step2-forwarded-steady-baseline-8x256-i4-s50
results/cfd_dsa/runs/20260707T031147Z_step2-forwarded-steady-core-8x256-i4-s50
results/cfd_dsa/runs/20260707T031147Z_step2-forwarded-steady-forwarded-8x256-i4-s50
results/cfd_dsa/runs/20260708T041554Z_step2-context-correctness-1x17-final-fastctx
results/cfd_dsa/runs/20260708T035911Z_step2-context-cell1-fastctx
results/cfd_dsa/runs/20260708T040628Z_step2-context-8x64-check-fastctx
results/cfd_dsa/runs/20260708T034401Z_step2-context-steady-step2-8x256-i4-s50-minctxstores
results/cfd_dsa/runs/20260708T034401Z_step2-context-steady-core-8x256-i4-s50-minctxstores
results/cfd_dsa/runs/20260708T035148Z_step2-context-steady-forwarded-8x256-i4-s50-minctxstores
results/cfd_dsa/runs/20260708T035924Z_step2-context-steady-forwarded-context-8x256-i4-s50-fastctx
m5out/lusgs-step2-linebuf-correctness-1x17
m5out/lusgs-step2-linebuf-cell1
m5out/lusgs-step2-linebuf-8x64-check
m5out/lusgs-step2-linebuf-steady-linebuf-8x256-i4-s50
```
