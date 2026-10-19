/* Structural decomposition of the current gem5 model; no invented unit events. */
(function (root) {
  'use strict';
  function expandArchitecture(session) {
    const cc = 'src/arch/arm/cfd_coeff_preprocess_controller.cc';
    const cluster = 'src/arch/arm/cfd_coeff_preprocess_cluster.inc';
    const onchip = 'src/arch/arm/cfd_coeff_preprocess_onchip.inc';
    const tiles = 'src/arch/arm/cfd_coeff_preprocess_tiles.inc';
    const runner = 'projects/cfd_dsa/configs/run_cfd_dsa.py';
    const map = new Map(session.nodes.map(n => [n.id, n]));
    const box = (id, x, y, w = 220, h = 86) => ({ id, x, y, w, h });
    function unit(id, label, parent, type, params, description, source = cc, extra = {}) {
      const node = { id, label, parent, type, params, description, source, subtitle: extra.subtitle || type,
        observability: 'structure', evidence: '组成和参数来自源码结构；此单元尚未采集独立活动状态，父模块事件不会被自动分摊到此单元。', ...extra };
      if (map.has(id)) throw new Error('重复结构单元: ' + id);
      map.set(id, node); session.nodes.push(node); return id;
    }
    function detail(ownerId, label, rows, links = [], extra = {}) {
      const nodes = rows.flatMap((row, r) => row.map((id, c) => {
        const width = (1080 - (row.length - 1) * 24) / row.length;
        return box(id, 40 + c * (width + 24), 95 + r * 135, width, 86);
      }));
      const view = { id: ownerId + '-detail', ownerId, parentView: extra.parentView || 'accelerator', label,
        breadcrumb: label, width: 1160, height: Math.max(650, rows.length * 135 + 70), nodes, edges: [], ...extra };
      const parent = map.get(ownerId); parent.drillView = view.id;
      for (const link of links) {
        const [from, to, text, kind = 'data'] = link, id = view.id + ':' + from + ':' + to;
        session.edges.push({ id, from, to, label: text, kind, scope: 'structure' }); view.edges.push(id);
      }
      session.views.push(view); return view;
    }
    function group(x, y, w, h, label, ownerId) { return { x, y, w, h, label, ownerId }; }
    const physical = '模型资源单元', logic = '逻辑控制', registers = '寄存器状态', region = 'SPM 存储区域', fifo = '有界队列';

    // One shared cluster; individual lane identity is not present in current traces.
    unit('cluster-issue', '发射与 lane 预留', 'cluster', logic, { 规则: '标量 1 lane / 向量 5 lanes', 选择: 'nextIssue ≤ 当前周期' }, 'issueLanes 按可用发射槽预留 lane，向量需一次取得五个槽。它是发射控制逻辑，不是额外计算单元。', cluster);
    unit('cluster-pending', '在途操作完成表', 'cluster', logic, { 字段: 'completeCycle / kind / lanes', 符号: 'pendingOps' }, '根据完成周期发布回调；同一流水单元可以有多个在途操作。表内记录不提供固定 RTL 流水级。', cc);
    unit('cluster-requests', 'LU / RHS / 向量请求', 'cluster', logic, { 入口: 'caller + lanes', 共享: '同一 5 / 5 / 1 簇' }, '不同算法任务请求同一算术资源池；这些入口不表示三套运算硬件。', cluster);
    const muls = [], adds = [];
    for (let i = 0; i < 5; i++) {
      muls.push(unit('mul-lane' + i, 'MUL ' + i, 'mul', physical, { 数据类型: 'FP64', 延迟: '3 cycles', II: '1 cycle', 结构编号: String(i) }, '共享乘法池的一个结构 lane。编号用于展示源码 nextIssue 槽，不代表 trace 已记录具体操作在哪个 lane 上执行。', cluster));
      adds.push(unit('add-lane' + i, 'ADD/SUB ' + i, 'add', physical, { 数据类型: 'FP64', 延迟: '4 cycles', II: '1 cycle', 结构编号: String(i) }, '共享加减池的一个结构 lane，执行加法或减法。当前 trace 仅记录资源池和此次发射 lane 数。', cluster));
    }
    unit('div-unit', 'DIV 0', 'div', physical, { 数据类型: 'FP64', 延迟: '12 cycles', II: '4 cycles' }, '唯一的共享流水除法单元。十二周期完成延迟不等于十二个除法器，也不代表已知的十二级 RTL 结构。', cluster);
    unit('cluster-vector-adds', '待累加向量队列', 'products', fifo, { 符号: 'sharedVectorAdds', 约束: '共享乘积 credit' }, '保存已经计算出的五元素乘积，等待共享加减池消费。', cluster);
    unit('cluster-cold-products', 'Cold 列乘积状态', 'products', registers, { 符号: 'coldProducts', 单列: '5 × FP64' }, 'cold base/Lbar 列乘积提前计算，按原列顺序累加，共用全局乘积 credit。', 'src/arch/arm/cfd_coeff_preprocess_frontier.inc');
    unit('cluster-rhs-products', 'RHS 行乘积缓冲', 'products', registers, { 条目: '60', 保守状态预算: '4096 B', 符号: 'rhsRowProducts' }, '多个 RHS 行组暂存乘积；条目不是额外乘法器。', 'src/arch/arm/cfd_coeff_preprocess_rhs_rows.inc');
    unit('cluster-line-products', 'Line 列乘积 / 累加', 'products', registers, { 每上下文未消费列: '≤ 5', 符号: 'LineMvmState' }, '缓存前向、后向和后台 base 各有有界乘积/累加状态，共用同一物理计算簇。', cc);
    const clusterView = detail('cluster', '共享计算簇 · 单元展开', [], [
      ['cluster-requests', 'cluster-issue', '请求', 'control'],
      ...muls.map(id => ['cluster-issue', id, '发射许可', 'control']),
      ...muls.map(id => [id, 'products', '乘积汇集']),
      ...adds.map(id => ['products', id, '待累加数据']),
      ['cluster-issue', 'div-unit', '除法发射', 'control'],
      ['div-unit', 'cluster-pending', '完成'], ...adds.map(id => [id, 'cluster-pending', '完成'])
    ], {
      height: 740,
      nodes: [box('cluster-requests', 60, 55, 285, 75), box('cluster-issue', 440, 55, 285, 75), ...muls.map((id, i) => box(id, 55 + i * 215, 190, 190, 70)),
        box('products', 440, 295, 285, 75), ...adds.map((id, i) => box(id, 55 + i * 215, 425, 190, 70)), box('div-unit', 55, 600, 250, 75), box('cluster-pending', 760, 600, 315, 75)],
      groups: [group(35, 148, 1090, 128, '共享乘法资源池 · 5 个 FP64 单元', 'mul'), group(35, 382, 1090, 128, '共享加减资源池 · 5 个 FP64 单元', 'add'), group(35, 554, 310, 140, '共享除法资源池 · 1 个单元', 'div')]
    });
    for (const [pool, lanes, name] of [['mul', muls, '乘法'], ['add', adds, '加减'], ['div', ['div-unit'], '除法']]) {
      unit(pool + '-next-issue', '发射间隔状态', pool, registers, { 符号: 'ResourcePool.nextIssue', 槽数: String(lanes.length) }, '每个 lane 的下一次允许发射周期；不是当前在途操作数量。', cc);
      detail(pool, '共享' + name + '池 · 发射单元', [[pool + '-next-issue'], lanes, ['cluster-pending']], lanes.map(id => [pool + '-next-issue', id, 'II / 发射许可', 'control']), { parentView: clusterView.id });
    }
    detail('products', '乘积与累加 · 缓冲状态', [['cluster-cold-products', 'cluster-vector-adds'], ['cluster-rhs-products', 'cluster-line-products'], ['mul', 'add']], [
      ['mul', 'cluster-vector-adds', '向量乘积'], ['cluster-vector-adds', 'add', '累加'], ['mul', 'cluster-cold-products', 'cold 乘积'], ['cluster-cold-products', 'add', '按列消费']
    ], { parentView: clusterView.id });

    // Controller: logical functions and finite state rather than new FUs.
    unit('ctrl-descriptor', 'Descriptor / token', 'controller', logic, { 符号: 'pendingLineLaunches / nextToken' }, '读取父 descriptor，生成 token，校验参数并建立 line 请求。', 'src/arch/arm/cfd_coeff_preprocess_interface.inc');
    unit('ctrl-window', '请求窗口 / credit', 'controller', logic, { 窗口: '8', 符号: 'liveChildren / nextCell' }, '窗口限制活动 cell 请求数量，完成 detach 后可以回收计算请求 credit。', cc);
    unit('ctrl-admission', '上下文准入', 'controller', logic, { LU额度: '2', RHS额度: '2' }, '检查输入 ready、LU/RHS 上下文额度与必要依赖，不等同于发射算术操作。', cc);
    unit('ctrl-frontier', '前沿与依赖判定', 'controller', logic, { 状态: 'stepReady / column masks' }, '检查 LU step、RHS/列和相邻 cell 的 ready 条件。', 'src/arch/arm/cfd_coeff_preprocess_frontier.inc');
    unit('ctrl-arbitration', '任务优先级仲裁', 'controller', logic, { 次序: '反馈 / 前沿 LU / RHS / 后台 LU' }, '决定哪类就绪工作先申请共享发射槽，不添加算术单元。', cc);
    unit('ctrl-generation', 'Generation / 取消', 'controller', logic, { 符号: 'liveGeneration / status' }, '拒绝过期 generation 和失效 token，取消后保留在途数据直到响应安全完成。', cc);
    unit('ctrl-fence', '完成 fence / 状态发布', 'controller', logic, { 条件: '必要数据与记录响应闭合' }, '完成状态可见前，等待必要传输、计算及完成记录响应。', 'src/arch/arm/cfd_coeff_preprocess_lifecycle.inc');
    detail('controller', '控制器 · 准入与调度', [['ctrl-descriptor', 'ctrl-generation'], ['ctrl-window', 'ctrl-admission', 'ctrl-frontier'], ['ctrl-arbitration', 'cluster'], ['ctrl-fence', 'completion']], [
      ['ctrl-descriptor', 'ctrl-window', '建立请求', 'control'], ['ctrl-window', 'ctrl-admission', 'credit', 'control'], ['ctrl-admission', 'ctrl-frontier', '依赖', 'control'], ['ctrl-frontier', 'ctrl-arbitration', '就绪候选', 'control'], ['ctrl-arbitration', 'cluster', '申请发射', 'control'], ['completion', 'ctrl-fence', '必要响应', 'control']
    ]);

    // DMA port state and its actual classic-memory attachment.
    unit('dma-transfer', '传输描述 / 数据快照', 'dma', registers, { 符号: 'InterfaceTransfer', 计数: 'issued / completed' }, '持有地址、读写方向、数据字节及发出/完成计数，直到 timing 响应返回。', 'src/arch/arm/cfd_coeff_preprocess_interface.inc');
    unit('dma-chunk', '分包 / 地址转换', 'dma', logic, { 包宽上限: '64 B', 符号: 'dmaChunkSize / issueDma' }, '按页面边界和配置宽度形成 timing DMA 包。', 'src/arch/arm/cfd_local_spm.cc');
    unit('dma-queue', '待发送 packet 队列', 'dma', fifo, { 符号: 'dmaPackets' }, '排队等待 dma_port 接受请求；与 outstanding credit 分别表达。', 'src/arch/arm/cfd_local_spm.cc');
    unit('dma-retry', '请求重试保持单元', 'dma', registers, { 符号: 'dmaRetryPacket', 事件: 'recvReqRetry' }, 'sendTimingReq 拒绝后保持该包，收到重试通知再发送。', 'src/arch/arm/cfd_local_spm.cc');
    unit('dma-credits', '在途包 credit / sender state', 'dma', logic, { 上限: '32', 符号: 'dmaLivePackets / DmaSenderState' }, '限制在途包总数，并携带 packetId 和完成回调；不推测当前占用。', 'src/arch/arm/cfd_local_spm.cc');
    unit('dma-response', '响应与回调', 'dma', logic, { 符号: 'recvDmaTimingResp', 行为: '完成计数 / 释放 credit' }, '根据 packet sender state 路由完成回调，更新计数并释放在途包 credit。', 'src/arch/arm/cfd_local_spm.cc');
    detail('dma', 'DMA · 请求、重试与响应', [['dma-transfer', 'dma-chunk'], ['dma-queue', 'dma-retry', 'dma-credits'], ['dma-response', 'memory']], [
      ['dma-transfer', 'dma-chunk', '地址 / bytes'], ['dma-chunk', 'dma-queue', 'packet'], ['dma-queue', 'dma-retry', '拒绝后保持', 'control'], ['dma-retry', 'dma-queue', 'retry', 'control'], ['dma-queue', 'memory', 'dma_port'], ['memory', 'dma-response', 'timing 响应'], ['dma-response', 'dma-credits', '释放 credit', 'control']
    ]);

    // Input, LU and RHS contexts. Slots are bounded state, never extra FUs.
    for (let i = 0; i < 2; i++) unit('ingress-slot' + i, '输入槽 ' + i, 'ingress', '上下文槽', { 内容: 'D / L / U / R', 容量口径: '请求状态预算中' }, '基础输入槽保存请求身份与输入状态；具体 cell 与当前有效标志尚未采集。', cc);
    unit('ingress-matrices', 'D / L / U 输入区域', 'ingress', region, { 每矩阵: '25 × FP64 = 200 B', 数据: '3 个 raw 矩阵' }, '输入矩阵是共享 SPM 中的地址区域，不是另配三块独立 SRAM。', onchip);
    unit('ingress-r', 'R 向量输入区域', 'ingress', region, { 数据: '5 × FP64 = 40 B' }, 'R 输入需要片上写入和 ready/读出约束。', onchip);
    unit('ingress-ready', '输入完成 / ready 标志', 'ingress', logic, { 符号: 'inputChunksCompleted / inputRead' }, '区分 DMA 到达、片上存储完成与计算可消费。', cc);
    detail('ingress', '输入暂存 · 槽与数据区域', [['dma', 'ingress-ready'], ['ingress-slot0', 'ingress-slot1'], ['ingress-matrices', 'ingress-r'], ['onchip', 'lu', 'rhs']], [
      ['dma', 'ingress-ready', '响应'], ['ingress-matrices', 'onchip', '片上访问'], ['ingress-r', 'onchip', '向量访问'], ['ingress-ready', 'lu', 'ready', 'control'], ['ingress-ready', 'rhs', 'ready', 'control']
    ]);
    detail('contexts', '计算上下文 · 共享资源边界', [['lu', 'rhs'], ['cluster']], [['lu', 'cluster', '共享算术资源'], ['rhs', 'cluster', '共享算术资源']]);
    const luNodes = [], luGroups = [];
    for (let i = 0; i < 2; i++) {
      const parent = unit('lu-context' + i, 'LU 上下文槽 ' + i, 'lu', '上下文槽', { 槽编号: String(i), 算术资源: '共享计算簇' }, '一个活动 Request 的 LU 状态和数据，不包含专用乘加或除法器。', cc);
      for (const [suffix, label, params, description] of [
        ['matrix', 'D / LU 矩阵寄存器', { 数据: 'D[25] / LU[25]', 类型: 'FP64' }, '保存当前请求的原始 D 和 packed LU。'],
        ['temps', '目标暂存 / 乘积', { 数据: 'luTemps[5] / luProducts[5]' }, '同列目标的临时值与乘积，仍按原 k 顺序更新。'],
        ['phase', '列 / 行 / k 状态机', { 符号: 'LuPhase / luCol / luK' }, '控制 Lower/Upper 运算、Store 和 NextColumn。'],
        ['pivot', 'Pivot 检查 / ready', { 符号: 'forwardStepReady[5]' }, '检查主元后发布 LU step ready，允许 RHS 前沿消费。'],
        ['divide-mask', '除法发射 / 完成掩码', { 符号: 'luDivIssued / luDivCompleted' }, '记录独立目标商的发射和完成，不增加除法器。']
      ]) {
        const id = unit(parent + '-' + suffix, label, parent, suffix === 'matrix' || suffix === 'temps' ? registers : logic, params, description, 'src/arch/arm/cfd_coeff_preprocess_lu_rows.inc');
        const index = luNodes.filter(n => n.id.startsWith(parent)).length;
        luNodes.push(box(id, 55 + i * 565, 130 + index * 92, 480, 74));
      }
      luGroups.push(group(35 + i * 565, 80, 525, 530, 'LU CONTEXT ' + i + ' · 状态 + 寄存器', parent));
    }
    detail('lu', 'LU · 两个上下文槽', [], [], { height: 650, nodes: luNodes, groups: luGroups });
    const rhsNodes = [], rhsGroups = [];
    for (let i = 0; i < 2; i++) {
      const parent = unit('rhs-context' + i, 'RHS 上下文槽 ' + i, 'rhs', '上下文槽', { RHS状态槽: '15', 启用条件: 'rhsEnabledMask' }, '每个请求保存十五组 RhsState；模式和边界掩码决定实际启用哪些 RHS。', cc);
      for (let r = 0; r < 15; r++) {
        const id = unit(parent + '-rhs' + r, 'RHS ' + r, parent, 'RHS 状态槽', { 编号: String(r), 状态: 'phase / k / i / waiting', 值: '5 × FP64', 选择: '由 batch / mask 决定' }, '独立 RHS 状态、向量值和依赖标志，申请同一共享算术簇；状态槽不是一个计算 lane。', cc);
        rhsNodes.push(box(id, 50 + i * 565 + r % 5 * 101, 150 + Math.floor(r / 5) * 110, 92, 88));
      }
      rhsGroups.push(group(35 + i * 565, 95, 525, 385, 'RHS CONTEXT ' + i + ' · 15 个逻辑状态槽', parent));
    }
    unit('rhs-ready-masks', 'RHS 依赖 / done 掩码', 'rhs', logic, { 符号: 'rhsEnabledMask / rhsDoneMask', 依赖: 'LU step / reciprocal' }, '记录任务启用、完成和依赖，不代表全部槽均在运行。', cc);
    detail('rhs', 'RHS · 上下文与状态槽', [], [], { nodes: [...rhsNodes, box('rhs-ready-masks', 60, 545, 480, 86), box('cluster-rhs-products', 620, 545, 480, 86)], groups: rhsGroups, height: 680 });

    // SPM regions are aliases of shared bank storage, not extra port-bearing arrays.
    unit('spm-decode', '字地址 / bank 映射', 'spm', logic, { 映射: '(address / 8) % 4', 字宽: '8 B' }, '根据地址和 grain 选择 bank，处理 stride 和每字访问进度。', onchip);
    unit('spm-cache', '有限系数缓存区域', 'spm', region, { direct路径: 'LU / Lbar / Ubar', 每cell: '600 B', 契约: 'line / cells / version / mode' }, 'direct 路径存 LU，inverse 路径存 DInv；两者是替代模式。当前冷轮可以填充缓存，命中轮按契约复用。', tiles);
    unit('spm-ingress-region', '输入 / 暂存区域', 'spm', region, { 内容: 'raw 输入 / warm R', 端口: '共用 bank R/W 限额' }, '输入存储区域与系数缓存、tile 一起映射到同一 banked SPM。', onchip);
    unit('spm-ready', '访问完成 / ready 发布', 'spm', logic, { 延迟: '末字获准后 1 cycle', 符号: 'readyCycle' }, '不是数据驻留即就绪；最后一个字被端口授予后仍需 ready 延迟。', onchip);
    detail('spm', 'SPM · 区域、仲裁与端口', [['spm-ingress-region', 'spm-cache', 'tile'], ['onchip', 'spm-decode'], ['bank0', 'bank1', 'bank2', 'bank3'], ['spm-ready']], [
      ['spm-ingress-region', 'onchip', '输入访问'], ['spm-cache', 'onchip', '缓存访问'], ['tile', 'onchip', 'tile 访问'], ['onchip', 'spm-decode', '任务地址'], ...Array.from({ length: 4 }, (_, b) => ['spm-decode', 'bank' + b, 'bank 选择', 'control']), ...Array.from({ length: 4 }, (_, b) => ['bank' + b, 'spm-ready', '末字完成', 'control'])
    ]);
    unit('spm-cache-lu', 'LU 系数区域', 'spm-cache', region, { 每cell: '25 × FP64 = 200 B', 模式: 'direct' }, '当前 direct 路径缓存 packed LU；inverse 模式在相同角色使用 DInv，不在当前结构中叠加成额外缓存容量。', tiles);
    unit('spm-cache-lbar', 'Lbar 系数区域', 'spm-cache', region, { 每cell: '25 × FP64 = 200 B' }, '保存有限系数缓存中的 Lbar。', tiles);
    unit('spm-cache-ubar', 'Ubar 系数区域', 'spm-cache', region, { 每cell: '25 × FP64 = 200 B' }, '保存有限系数缓存中的 Ubar，后向计算依契约读取。', tiles);
    unit('spm-cache-contract', 'Valid / version / mode 契约', 'spm-cache', logic, { 符号: 'LineCoeffCache', 条件: 'line / cells / version / directBaseSolve' }, '检查缓存有效性、系数版本、长度和模式。缓存命中仍需有限 SPM 端口读取。', tiles);
    detail('spm-cache', '有限系数缓存 · 数据与契约', [['spm-cache-contract'], ['spm-cache-lu', 'spm-cache-lbar', 'spm-cache-ubar'], ['onchip']], [['spm-cache-contract', 'spm-cache-lu', '匹配', 'control'], ['spm-cache-contract', 'spm-cache-lbar', '匹配', 'control'], ['spm-cache-contract', 'spm-cache-ubar', '匹配', 'control'], ['spm-cache-lu', 'onchip', '读出'], ['spm-cache-lbar', 'onchip', '读出'], ['spm-cache-ubar', 'onchip', '读出']], { parentView: 'spm-detail' });
    unit('onchip-queue', '片上访问任务队列', 'onchip', fifo, { 深度: '64', 符号: 'onchipQueue', 内容: 'owner / address / op / next' }, '有限访问任务描述，队列满则背压；不是六十四个存储端口。', onchip);
    unit('onchip-active', 'DMA 激活 / 取消', 'onchip', logic, { 符号: 'active / eligibleCycle' }, 'DMA 到达之前预留任务，响应后激活；取消尚未激活的任务。', onchip);
    unit('onchip-priority', '优先级 / 稳定排序', 'onchip', logic, { 顺序: 'priority / id', 优先: '前沿读取' }, '同优先级按任务编号稳定排序。', onchip);
    unit('onchip-port-budget', '逐 bank R/W 限额', 'onchip', logic, { 每周期: '4 × (1R + 1W)', 符号: 'reads / writes' }, '每周期分别重置每 bank 的读写限额，不能把 bank 带宽共享成一个无限端口。', onchip);
    detail('onchip', 'SPM 仲裁 · 有限访问任务', [['onchip-queue', 'onchip-active'], ['onchip-priority', 'onchip-port-budget'], ['spm-decode', 'spm-ready']], [['onchip-queue', 'onchip-priority', '活动任务', 'control'], ['onchip-priority', 'onchip-port-budget', '字访问授予', 'control']], { parentView: 'spm-detail' });
    for (let b = 0; b < 4; b++) {
      const parent = 'bank' + b;
      unit(parent + '-read', 'Bank ' + b + ' · R0', parent, '读端口限额', { 每周期: '1 × 8 B', 操作: 'Read' }, '此 bank 的一个读端口授予限额；没有具体端口活动 trace 时不染成运行状态。', onchip);
      unit(parent + '-write', 'Bank ' + b + ' · W0', parent, '写端口限额', { 每周期: '1 × 8 B', 操作: 'Write' }, '此 bank 的一个写端口授予限额，与读端口分别限制。', onchip);
      unit(parent + '-storage', 'Bank ' + b + ' · 存储字', parent, region, { 地址: '(address / 8) % 4 = ' + b, 容量: '由共享区域分配决定' }, '同一 bank 承载输入、缓存与 tile 的映射字；统一 192 KiB 状态预算不能简单除以四当成 SRAM 容量。', onchip);
      detail(parent, 'Bank ' + b + ' · 读写端口', [[parent + '-read', parent + '-write'], [parent + '-storage'], ['spm-ready']], [[parent + '-write', parent + '-storage', '写入字'], [parent + '-storage', parent + '-read', '读出字'], [parent + '-read', 'spm-ready', 'ready', 'control']], { parentView: 'spm-detail' });
    }

    // Two tile buffers and their data/state, bound to the same physical SPM.
    const tileNodes = [], tileGroups = [];
    for (let i = 0; i < 2; i++) {
      const id = unit('tile-buffer' + i, 'Tile ' + (i ? 'B' : 'A'), 'tile', region, { cells: '8', 每cell: 'y 40 B + Ubar 200 B', 符号: i ? 'prefetchTile' : 'tile' }, '一个固定容量 tile 区域，phase 决定前向驻留、回读或保留；缓冲 A/B 没有独立算术单元。', tiles);
      for (const [suffix, label, type, params, desc] of [
        ['y', 'y 向量区域', region, { 容量: '8 × 40 B = 320 B' }, '保存前向结果 y。'],
        ['ubar', 'Ubar 矩阵区域', region, { 容量: '8 × 200 B = 1600 B' }, '保存驻留后向系数 Ubar；命中路径可由有限系数缓存供给。'],
        ['phase', 'Phase / cell 范围', logic, { 状态: 'Idle / Forward / Resident / Read' }, '记录 start/count/retired 和存储编号；phase 状态尚未在演示中逐项采集。'],
        ['dma-count', 'DMA 计数 / 响应引用', registers, { 符号: 'issued[2] / completed[2]' }, '引用保护存储直到必需响应返回，之后才能复用区域。']
      ]) {
        const child = unit(id + '-' + suffix, label, id, type, params, desc, tiles);
        const index = tileNodes.filter(n => n.id.startsWith(id + '-')).length;
        tileNodes.push(box(child, 65 + i * 565, 145 + index * 100, 460, 78));
      }
      tileGroups.push(group(35 + i * 565, 95, 525, 485, 'TILE ' + (i ? 'B · 预取 / 留存' : 'A · 当前缓冲'), id));
    }
    detail('tile', 'Tile A / B · 数据与生命周期', [], [], { parentView: 'spm-detail', nodes: tileNodes, groups: tileGroups });

    unit('feedback-handoff', '前向结果交接', 'feedback', registers, { 数据: '5 × FP64', 符号: 'ForwardDqHandoff' }, '按 line/cell/generation 将前一个 cell 的 dq_star 交给后继，附带 ready/consumed 状态。', cc);
    unit('feedback-forward', '前向列乘积 / 累加', 'feedback', registers, { 符号: 'LineMvmState', 依赖: 'previousY / Lbar' }, '保存前向计算的乘积和累加进度，复用共享 mul/add。', cc);
    unit('feedback-backward', '后向列乘积 / 累加', 'feedback', registers, { 符号: 'LineMvmState', 依赖: 'dq[i+1] / Ubar' }, '单线后向递推状态，仍复用同一计算簇，不画成独立 MVM 硬件。', cc);
    unit('feedback-base', '后台 base 上下文', 'feedback', registers, { 符号: 'LineMvmState', 角色: '后台 base' }, '缓存 inverse 模式下使用的有界 base 状态；当前 direct 模式该路径不被强制激活。', cc, { subtitle: '模式条件启用' });
    unit('feedback-combine', '最终组合 / 发布控制', 'feedback', logic, { 符号: 'ForwardCombineTask / dqStar' }, '等待 base 和 correction ready 后提交组合工作，结果发布至 handoff/tile/output。', cc);
    detail('feedback', '前后向转发 · 递推状态', [['feedback-handoff', 'feedback-base'], ['feedback-forward', 'feedback-backward'], ['cluster', 'feedback-combine'], ['output', 'tile']], [['feedback-handoff', 'feedback-forward', '相邻结果'], ['feedback-forward', 'cluster', '共享计算'], ['feedback-backward', 'cluster', '共享计算'], ['cluster', 'feedback-combine', '完成'], ['feedback-combine', 'output', '向量'], ['tile', 'feedback-backward', 'y / Ubar']]);

    // Explicit FIFO slots, plus producer and response ownership boundaries.
    for (const [parent, prefix, title, source] of [['output', 'output-slot', '输出', tiles], ['completion', 'completion-slot', '完成记录', 'src/arch/arm/cfd_coeff_preprocess_lifecycle.inc']]) {
      const slots = [];
      for (let i = 0; i < 8; i++) slots.push(unit(prefix + i, title + '槽 ' + i, parent, 'FIFO 条目', parent === 'output' ? { 内容: '40 B 向量 / transfer / tile 引用' } : { 内容: 'token / request / generation / transfer', 保守预算: '1024 B' }, '有界队列中的一个结构条目。槽编号只用于展示容量，不映射到未采集的实际请求占用。', source));
      unit(parent + '-producer', title + '入队 / credit', parent, logic, { 深度: '8', 行为: '满则背压' }, '入队条件与相应有限 credit；其预算与计算请求或输入槽分别表达。', source);
      unit(parent + '-retire', title + '响应 / 回收', parent, logic, { 条件: 'completed == bytes.size()' }, '必要响应返回之后，释放数据或记录条目及引用。', source);
      detail(parent, title + '队列 · 八个结构槽', [[parent + '-producer'], slots.slice(0, 4), slots.slice(4), [parent + '-retire', 'dma']], [['dma', parent + '-retire', '响应'], [parent + '-retire', parent + '-producer', '回收 credit', 'control']]);
    }

    // O3 default and actual classic cache/DRAM/SPM hierarchy from this runner.
    map.get('cpu').description = '当前参考配置使用 O3CPU。展开查看前端、重命名、IQ/FU、LSQ、ROB/commit 和 CFD 启动接口；这里未接入 CPU 逐指令 trace。';
    const cpuUnits = [
      ['cpu-fetch', 'Fetch / 分支预测', logic, { 宽度: '8 inst/cycle' }, '前端取指和分支预测，访问 L1I。', 'src/cpu/o3/fetch.hh'],
      ['cpu-decode', 'Decode', logic, { 宽度: '8 inst/cycle' }, '解码 AArch64 与自定义 CFD 指令。', 'src/cpu/o3/decode.hh'],
      ['cpu-rename', 'Rename / 映射与 free list', logic, { 宽度: '8 inst/cycle', int寄存器: '512', vec寄存器: '512', mat寄存器: '256' }, '重命名使用物理寄存器和映射表。', 'src/cpu/o3/rename.hh'],
      ['cpu-iq', 'IQ / 唤醒与选择', logic, { 默认条目: '64', 发射宽度: '8' }, '保留指令依赖，选择就绪指令进入 O3 FU pool；条目数可由运行参数覆盖。', 'src/cpu/o3/inst_queue.hh'],
      ['cpu-fu', 'CPU FU pool / Writeback', physical, { wb宽度: '8', 计数: '由 FUDesc / 运行配置确定' }, '普通 O3 功能单元与事件控制器的共享簇是不同资源边界。', 'src/cpu/o3/fu_pool.hh'],
      ['cpu-lsq', 'LSQ · Load / Store 队列', logic, { LQ: '64', SQ: '64' }, '普通 CPU load/store 经 L1D；DMA 使用独立 DMA cache 接入。', 'src/cpu/o3/lsq.hh'],
      ['cpu-rob', 'ROB / Commit', logic, { 默认ROB: '160', 提交宽度: '8' }, '顺序提交与异常/回滚；ROB 条目数可由实际参数覆盖。', 'src/cpu/o3/commit.hh'],
      ['cpu-launch', 'CFD ISA / Launch / Wait', logic, { 行为: 'descriptor / token' }, '自定义指令提交事件控制器工作，启动后设备自治推进。', 'src/arch/arm/insts/cfd_dsa.cc']
    ];
    for (const [id, label, type, params, desc, source] of cpuUnits) unit(id, label, 'cpu', type, params, desc, source);
    detail('cpu', 'CPU · O3 流水与接口', [['cpu-fetch', 'cpu-decode', 'cpu-rename'], ['cpu-iq', 'cpu-fu', 'cpu-lsq'], ['cpu-rob', 'cpu-launch'], ['controller']], [['cpu-fetch', 'cpu-decode', '指令'], ['cpu-decode', 'cpu-rename', '解码'], ['cpu-rename', 'cpu-iq', '派发'], ['cpu-iq', 'cpu-fu', '发射'], ['cpu-iq', 'cpu-lsq', '访存'], ['cpu-fu', 'cpu-rob', '完成'], ['cpu-launch', 'controller', '启动 / 等待', 'control']], { parentView: 'system' });
    map.get('memory').description = '当前 runner 的内存系统包含 L1I/L1D、DMA cache、toL2Bus、L2、SystemXBar、DRAM SimpleMemory 和独立映射的 SimpleMemory SPM。后者与设备内部 banked SPM 状态预算分别展示。';
    const caches = [['mem-l1i', 'L1 I-Cache', '32 KiB', '8', '32'], ['mem-l1d', 'L1 D-Cache', '32 KiB', '8', '128'], ['mem-dma-cache', 'DMA Cache', '1 KiB', '8', '20'], ['mem-l2', 'L2 Cache', '512 KiB', '16', '128']];
    for (const [id, label, size, assoc, mshrs] of caches) unit(id, label, 'memory', 'Cache', { 容量: size, 相联度: assoc, MSHRs: mshrs }, id === 'mem-dma-cache' ? 'timing DMA 请求先进入此 cache，再接入 classic coherent fabric；不是设备的有限系数缓存。' : '当前 runner 中的 classic cache，具体时序以实际运行配置为准。', runner);
    unit('mem-l2bus', 'toL2Bus · L2XBar', 'memory', '互连', { 宽度: '64 B' }, '汇合 CPU 两个 L1 与 DMA cache 的请求。', runner);
    unit('mem-bus', 'membus · SystemXBar', 'memory', '互连', { 宽度: '64 B', 时钟: '2 GHz' }, '将 L2 侧请求路由到配置的地址范围。', runner);
    unit('mem-dram', 'DRAM · SimpleMemory', 'memory', '内存时序模型', { 默认容量: '16 MB', 延迟: '30 ns', 带宽: '32 GB/s' }, '此配置使用 SimpleMemory，未建模真实 DRAM controller 的 command queues、channel/rank/bank。', runner);
    unit('mem-simple-spm', '映射 SPM · SimpleMemory', 'memory', '独立地址设备', { 地址: '0x70000000', 大小: '256 KiB', 默认延迟: '1 ns', 默认带宽: '64 GB/s' }, 'CPU 访问的 uncached 映射 SPM。它不是 CFD controller 内 192 KiB 统一状态预算，不能把两者容量相加当成同一存储。', runner);
    detail('memory', '内存系统 · Cache 与互连', [['mem-l1i', 'mem-l1d', 'mem-dma-cache'], ['mem-l2bus', 'mem-l2'], ['mem-bus'], ['mem-dram', 'mem-simple-spm']], [['mem-l1i', 'mem-l2bus', '指令访存'], ['mem-l1d', 'mem-l2bus', '数据访存'], ['mem-dma-cache', 'mem-l2bus', 'DMA'], ['mem-l2bus', 'mem-l2', '请求'], ['mem-l2', 'mem-bus', '请求'], ['mem-bus', 'mem-dram', '地址路由'], ['mem-bus', 'mem-simple-spm', 'uncached 地址路由']], { parentView: 'system' });
    for (const [id, label, size, assoc, mshrs] of caches) {
      unit(id + '-tags', 'Tag / valid / dirty', id, registers, { 相联度: assoc, line: '64 B' }, 'cache tag 和有效/脏状态；L1I 为只读 cache。', runner);
      unit(id + '-data', 'Data array', id, 'Cache 数据阵列', { 容量: size }, '该 cache 的数据存储，逐 set/way 内容尚未采集。', runner);
      unit(id + '-mshr', 'MSHR / miss 合并', id, fifo, { 条目: mshrs }, '保留在途 miss 和同一 cache line 的目标请求，不代表内存通道。', runner);
      unit(id + '-control', 'Tag 查询 / 替换控制', id, logic, { 规则: 'Cache 实际配置' }, '处理 cache hit/miss、替换和响应路由；不补出未记录的 hit rate。', runner);
      detail(id, label + ' · 内部单元', [[id + '-control'], [id + '-tags', id + '-data'], [id + '-mshr']], [[id + '-control', id + '-tags', 'tag 查询'], [id + '-control', id + '-data', '数据访问'], [id + '-control', id + '-mshr', 'miss', 'control']], { parentView: 'memory-detail' });
    }
    const resourceOverview = session.views.find(v => v.id === 'resources');
    if (resourceOverview) { resourceOverview.groups[0].ownerId = 'cluster'; resourceOverview.groups[1].ownerId = 'spm'; }
    session.meta.detailVersion = 1;
    return session;
  }
  if (typeof module !== 'undefined' && module.exports) module.exports = expandArchitecture;
  else root.CfdArchitectureDetails = expandArchitecture;
})(typeof window !== 'undefined' ? window : globalThis);
