# CFD-DSA LU-SGS Step3 Macro Controller 技术文档

> 文档版本：v1.0（2026-06-23 Step3 A/B/C functional macro-controller）
> gem5 版本：gem5 25.1.0.0 / ARM SE O3
> 当前状态：`scons -Q build/ARM/gem5.opt -j2` 通过；Step3 A/B/C 正确性、错误路径、decode-exclusive 和既有路径回归通过。
> 后续说明：Step3 继续作为 functional golden/reference 默认保留；Step4-A event-driven controller skeleton 使用独立文件、runner 和显式 env 开关启用，详见 `CFD_DSA_LUSGS_STEP4.md`。

---

## 1. 目标

Step3 的目标是把 LU-SGS 的 CPU 逐 cell 提交改成一个端到端宏控制器接口：

```text
cfd_lusgs_launch xToken, [xDesc]
cfd_lusgs_wait   xStatus, xToken
```

CPU 只提交一个 descriptor，controller 在 gem5 内部完成 forward sweep、backward sweep，以及可选的 `Q += omega * dQ` 更新。Step3 不删除 Path A、Step1、Step2、TRSV5、Path C；现有路径保持可独立运行和回归。

## 2. 冻结边界

本阶段保持以下模块语义不变：

| 模块 | 冻结内容 |
| --- | --- |
| Path A | `lmat5_spm + dotp_row + pack_acc` 编码和执行路径保留 |
| Step1 | 软件 TRSV + Path A MVM 的 LU-SGS baseline 保留 |
| Step2 | `trsv5_lu_spm` 指令接口和 LU-SGS Step2 benchmark 保留 |
| Path C | SME-ZA selected-column outer-product 语义保留 |
| runner 默认值 | Step1/Step2 wrapper 默认参数不被 Step3 选项污染 |

Step3 新增的是一个独立宏控制器路径；Path A/TRSV5 作为黑盒功能单元被 controller 调用，不把原 ISA 路径改写成 controller 专用实现。

## 3. 文件总览

| 文件 | 作用 |
| --- | --- |
| `src/arch/arm/cfd_lusgs_controller.hh/.cc` | Step3 descriptor、状态码、launch/wait、controller functional model 与统计上报 |
| `src/arch/arm/cfd_lusgs_vec5.hh/.cc` | 独立 Vector5 copy/sub/axpy helper |
| `src/arch/arm/cfd_trsv5_math.hh` | TRSV5 指令路径与 controller 共享的 5x5 LU solve 数学 helper |
| `src/arch/arm/insts/cfd_dsa.hh/.cc` | `DSALusgsLaunch` / `DSALusgsWait` 指令类 |
| `src/arch/arm/isa/formats/custom_cfd.isa` | Step3 新指令 decode 与互斥 guard |
| `src/arch/arm/cfd_local_spm.hh/.cc` | `lusgsController*` 与 `pathaArbiter*` 统计项 |
| `src/cpu/op_class.hh`, `src/cpu/FuncUnit.py` | `CFDDSAVec5`、`CFDDSALusgsCtrl` OpClass |
| `src/cpu/o3/FuncUnitConfig.py`, `src/cpu/o3/FUPool.py` | Vec5 和 LU-SGS controller FU 配置 |
| `projects/cfd_dsa/configs/run_cfd_dsa.py` | Step3 参数解析、env、FU override、guest 参数转发 |
| `projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step3.py` | Step3 专用 wrapper |
| `projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step3.c` | Step3 正确性、Q update 和错误路径 benchmark |
| `projects/cfd_dsa/benchmarks/common/test_cfd_dsa_decode_exclusive.c` | pack/TRSV5/Step3 decode 互斥回归 |

## 4. Descriptor ABI

`CfdLusgsDescriptor` 布局如下：

```c
typedef struct {
    uint64_t lu_a_base;
    uint64_t c_base;
    uint64_t bbar_base;
    uint64_t rhs_base;
    uint64_t dqstar_base;
    uint64_t dq_base;
    uint64_t q_base;
    uint32_t n_lines;
    uint32_t n_cells;
    uint32_t line_stride_bytes;
    uint32_t matrix_cell_stride_bytes;
    uint32_t vector_cell_stride_bytes;
    uint32_t flags;
    uint32_t tile_cells;
    double omega;
} CfdLusgsDescriptor;
```

默认 benchmark 使用 contiguous line layout，`matrix_cell_stride_bytes=200`，`vector_cell_stride_bytes=40`。

## 5. Flags

| flag | 值 | 语义 |
| --- | ---: | --- |
| `LUSGS_FLAG_WRITE_DQ` | `1 << 0` | 写出最终 `dQ` |
| `LUSGS_FLAG_UPDATE_Q` | `1 << 1` | 执行 `Q += omega * dQ` |
| `LUSGS_FLAG_CHECK_BOUNDS` | `1 << 2` | 检查 tile working set 是否超过本地 SPM 容量模型 |
| `LUSGS_FLAG_TRACE` | `1 << 3` | 写 controller CSV trace |

## 6. 状态码

| status | 值 | 含义 |
| --- | ---: | --- |
| Complete | 0 | 完成 |
| Busy | 1 | controller 已有未 wait 的 task |
| BadDescriptor | 2 | descriptor 或必需指针非法 |
| BadShape | 3 | `n_lines` 或 `n_cells` 为 0 |
| BadStride | 4 | matrix/vector stride 过小或 tile 为 0 |
| BadAlignment | 5 | 8B 对齐失败 |
| UnsupportedFlags | 6 | flag 中有未支持 bit |
| BadToken | 7 | wait token 不匹配 |
| UnsupportedStage | 8 | stage/shape 不被当前阶段接受 |
| SpmCapacity | 9 | tile working set 超过容量模型 |

## 7. 指令编码

所有 Step3 指令使用 custom CFD 空间：

```text
[31:27]=00000 [26:25]=00
[24:23]=11 [22]=0 [21:20]=00
```

| 指令 | 编码字段 |
| --- | --- |
| `cfd_lusgs_launch xToken, [xDesc]` | `[19:15]=11111 [14:10]=xdesc [9:5]=00001 [4:0]=xtok` |
| `cfd_lusgs_wait xStatus, xToken` | `[19:15]=11110 [14:10]=xtok [9:5]=00010 [4:0]=xstatus` |

互斥检查：

| 路径 | 互斥依据 |
| --- | --- |
| `pack_acc` | tag 为 `00000`，且 `xn == 16` guard |
| `trsv5_lu_spm` | legal base register 限制在 `x21..x24` |
| Step3 launch/wait | tag 为 `11111` / `11110`，且 `rs2` guard 为 `00001` / `00010` |

`build/cfd_dsa/aarch64/test_cfd_dsa_decode_exclusive_arm` 已确认 `pack_acc`、`trsv5_lu_spm`、`cfd_lusgs_launch/wait` 三组编码互斥。

## 8. 指令语义

`cfd_lusgs_launch`：

1. 从 `xdesc` 指向的 guest memory 读取 descriptor。
2. 校验 shape、stride、alignment、flag、stage、capacity。
3. 若 controller 空闲，生成非零 token。
4. 在非投机提交点执行完整 functional controller。
5. 写 token 到 `xtok`。

`cfd_lusgs_wait`：

1. 读取 token。
2. 若 token 匹配当前 task，返回 task status。
3. 清空 busy-until-wait 状态。

Step3 指令标记为 `IsNonSpeculative`、`IsSerializeBefore`、`IsSerializeAfter`，避免错误路径写内存。

## 9. Controller Functional Flow

每条 line 执行：

```text
forward:
  cell 0:       dQ* = TRSV5(LU_A, RHS)
  cell i > 0:   tmp = PathA_MVM(C_i, dQ*_{i-1})
                dQ* = TRSV5(LU_A, RHS_i - tmp)

backward:
  last:         dQ = dQ*
  cell i:       tmp = PathA_MVM(Bbar_i, dQ_{i+1})
                dQ = dQ* - tmp

optional:
  Q_i += omega * dQ_i
```

controller 内部使用 `SETranslatingPortProxy` 读写 guest memory。当前 Path A MVM adapter 使用和 benchmark 一致的逐乘逐加顺序，避免 FMA/rounding 差异。

## 10. Stage A

Stage A 限制：

| 项 | 值 |
| --- | --- |
| `n_lines` | 必须为 1 |
| `contexts` | 1 |
| `Vec5Sub` 统计 | 0，减法视作 controller 内部顺序逻辑 |

通过用例：

```text
1x1, 1x2, 1x3, 1x17, 1x64
```

1x64 关键统计：

| 指标 | 值 |
| --- | ---: |
| `TotalCycles` | 9764 |
| `ForwardCells/BackwardCells` | 64 / 64 |
| `PathARequests` | 126 |
| `Trsv5Requests` | 64 |
| `Vec5Sub/Copy/Axpy` | 0 / 0 / 0 |
| `MaxActiveContexts` | 1 |
| `CyclesPerFullCell` | 152.5625 |
| `PathAUtilization` | 0.554896 |

## 11. Stage B

Stage B 新增：

| 项 | 行为 |
| --- | --- |
| independent Vector5 | `copy/sub/axpy` helper 与 `CFDDSAVec5` FU 配置 |
| internal data residency | `dQ*` / `dQ` 在 controller 内部 vector buffer 暂存 |
| Q update | `LUSGS_FLAG_UPDATE_Q` 触发 `Vec5Axpy` |

通过用例：

```text
1x1, 1x2, 1x3, 1x17, 1x64
Q update: omega = 1.0, 0.5, -0.25
```

1x64 无 Q 更新：

| 指标 | 值 |
| --- | ---: |
| `TotalCycles` | 10272 |
| `PathARequests` | 126 |
| `Trsv5Requests` | 64 |
| `Vec5Sub/Copy/Axpy` | 126 / 1 / 0 |
| `CyclesPerFullCell` | 160.5 |

1x64 + Q 更新：

| 指标 | 值 |
| --- | ---: |
| `TotalCycles` | 10528 |
| `UpdatedQCells` | 64 |
| `Vec5AxpyRequests` | 64 |
| `CyclesPerFullCell` | 164.5 |

## 12. Stage C

Stage C 新增多 line contexts 与 tile scheduling 统计。functional 执行仍保持按 line/cell 的确定性顺序，统计模型记录 context 数、tile loads、prefetch、buffer swap 和资源下界。

通过用例：

```text
2x17 contexts=2
4x17 contexts=4
8x17 contexts=8
8x64 contexts=1/2/4/8
8x64 contexts=8 tile=1/4/8/16
```

## 13. 8x64 对比

所有 8x64 Stage C context sweep 均 bit-exact 通过：

| contexts | TotalCycles | MaxActiveContexts | CyclesPerFullCell | PathAUtil | TRSV5Util | Vec5Util |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 43344 | 1 | 84.65625 | 1.000000 | 0.708749 | 0.093762 |
| 2 | 43344 | 2 | 84.65625 | 1.000000 | 0.708749 | 0.093762 |
| 4 | 43344 | 4 | 84.65625 | 1.000000 | 0.708749 | 0.093762 |
| 8 | 43344 | 8 | 84.65625 | 1.000000 | 0.708749 | 0.093762 |

解释：当前统计模型中 8x64 的资源下界由 Path A adapter 占满，context 增加只提升 `MaxActiveContexts` 和 `ActiveContextCycles`，不降低 `TotalCycles`。

## 14. Tile 对比

8x64, contexts=8：

| tile_cells | TileTotalBytes | TileLoads | TilePrefetches | TotalCycles |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 6720 | 512 | 511 | 43344 |
| 4 | 24960 | 128 | 127 | 43344 |
| 8 | 49280 | 64 | 63 | 43344 |
| 16 | 97920 | 32 | 31 | 43344 |

tile 越大，load/prefetch 次数越少，working-set 字节增加。`tile=16` 仍在 96 KiB 容量模型内；错误测试中 `tile=1000` 正确返回 `SpmCapacity=9`。

## 15. 统计项

新增统计项按用途分组：

| 分组 | 代表字段 |
| --- | --- |
| task | `lusgsControllerLaunches`, `CommittedLaunches`, `CompletedTasks`, `FailedTasks` |
| cells | `ForwardCells`, `BackwardCells`, `UpdatedQCells` |
| requests | `PathARequests`, `Trsv5Requests`, `Vec5SubRequests`, `Vec5CopyRequests`, `Vec5AxpyRequests` |
| wait/stall | `PathAWaitCycles`, `Trsv5WaitCycles`, `Vec5WaitCycles`, `NoReadyContextCycles` |
| context | `ContextAlloc`, `ContextFree`, `MaxActiveContexts`, `ContextSwitches` |
| data | `LuBytes`, `CBytes`, `BbarBytes`, `RhsBytes`, `Dqstar*`, `Dq*`, `Q*`, `TemporaryBytes` |
| tile | `TileTotalBytes`, `TileLoads`, `TilePrefetches`, `PrefetchUseful`, `BufferSwap` |
| Path A arbiter | `pathaArbiterLusgsRequests`, `pathaArbiterOwnershipCycles`, `pathaArbiterBusyStalls` |
| performance | `CyclesPerFullCell`, `CellsPer1000Cycles`, `PathAUtilization`, `Trsv5Utilization`, `Vec5Utilization` |

## 16. Trace

可用参数：

```text
--lusgs-controller-trace-enable
--lusgs-controller-trace-lines=<n>
--lusgs-controller-trace-cells=<n>
--lusgs-controller-trace-file=<path>
```

trace CSV 字段：

```text
cycle,task_id,line,cell,tile,phase,context_id,state,operation,
patha_slot,patha_token,trsv5_token,vec5_token,dependency_ready,
resource_ready,issue,complete,wait_reason,spm_buffer,note
```

当前 trace 是 coarse functional event trace，不是逐周期事件驱动波形。

## 17. 错误路径测试

`projects/cfd_dsa/benchmarks/lusgs/test_cfd_lusgs_patha_step3.c` 支持：

```text
--lusgs-error-case=bad-shape
--lusgs-error-case=bad-stride
--lusgs-error-case=bad-align
--lusgs-error-case=bad-flags
--lusgs-error-case=missing-q
--lusgs-error-case=spm-capacity
--lusgs-error-case=stage-a-multiline
--lusgs-error-case=bad-token
--lusgs-error-case=busy
```

全部返回 `LUSGS_STEP3_ERROR_PASS`。busy 测试确认第二次 launch 返回 token 0，且不破坏第一个 task 的 wait status。

## 18. 回归

已跑回归：

| 类别 | 用例 | 结果 |
| --- | --- | --- |
| Path A | `projects/cfd_dsa/configs/run_cfd_patha.py --bench-iters=64` | PASS |
| TRSV5 | solves 1 / 17 / 512 | PASS |
| Step1 | 1x1 / 1x17 / 8x64 | PASS |
| Step2 | 1x1 / 1x17 / 8x64 | PASS |
| Path C | `projects/cfd_dsa/configs/run_cfd_pathc.py --bench-iters=64` | PASS |
| decode-exclusive | `build/cfd_dsa/aarch64/test_cfd_dsa_decode_exclusive_arm` | PASS |

## 19. 运行命令

典型命令：

```bash
build/ARM/gem5.opt -d results/cfd_dsa/runs/lusgs_step3A_1x64_new \
  projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step3.py \
  --lusgs-step3-stage=A --lusgs-lines=1 --lusgs-cells=64

build/ARM/gem5.opt -d results/cfd_dsa/runs/lusgs_step3B_1x64_q1 \
  projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step3.py \
  --lusgs-step3-stage=B --lusgs-lines=1 --lusgs-cells=64 \
  --lusgs-update-q=1 --lusgs-omega=1.0

build/ARM/gem5.opt -d results/cfd_dsa/runs/lusgs_step3C_8x64_ctx8 \
  projects/cfd_dsa/configs/lusgs/run_cfd_lusgs_step3.py \
  --lusgs-step3-stage=C --lusgs-lines=8 --lusgs-cells=64 \
  --lusgs-contexts=8 --lusgs-tile-cells=1
```

## 20. 当前限制

当前 Step3 是 functional macro-controller + modeled schedule：

1. `launch` 在非投机提交点执行完整 functional task；不是独立 event queue 异步推进。
2. `wait` 当前主要负责 token/status 同步；CPU submit/wait 周期很小，`lusgsControllerTotalCycles` 是 controller 统计模型周期。
3. Path A controller adapter 记录 `PathARequests`，但不会提交真实 `CFDDSADotp/CFDDSAMatLd/CFDDSAPack` guest 指令。
4. Stage C 的 context/tile overlap 是统计模型，不是逐周期仲裁器。
5. trace 是 coarse event trace；若要做硬件级时序，需要下一阶段把 controller 拆成 event-driven state machine。

这些限制不影响 Step3 当前目标：descriptor ABI、decode、functional correctness、统计口径、错误路径和既有路径隔离已经完成。
