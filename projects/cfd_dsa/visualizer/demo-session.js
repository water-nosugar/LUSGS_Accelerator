/* Structure reflects the shared-cluster model. Every replay event is illustrative. */
(function (root) {
  'use strict';
  const arm = 'src/arch/arm/';
  const nodes = [];
  const node = (id, label, type, parent, subtitle, description, params, source, extra = {}) => {
    nodes.push({ id, label, type, parent, subtitle, description, params, source, ...extra });
  };
  node('cpu', 'CPU / Guest', '系统', null, '提交 descriptor · 等待完成', 'CPU 执行提交与等待指令，控制器在启动后自治推进。此视图概括接口边界，CPU 内部流水线尚未展开。', { 展示粒度: '系统接口' }, 'projects/cfd_dsa/configs/run_cfd_dsa.py');
  node('memory', '内存系统', '系统', null, 'gem5 timing memory', '承载原始矩阵、向量、描述符和完成记录。DMA 请求经过实际运行配置中的内存层级；原型将其折叠为一个模块。', { 展示粒度: '折叠层级' }, 'projects/cfd_dsa/configs/run_cfd_dsa.py');
  node('dsa', 'CFD DSA', '加速器', null, '单线 · 前向 / 后向', '有界上下文、共享算术资源、SPM bank/端口和独立完成队列构成的有限资源时序模型。', { 路径: 'cold direct', 研究范围: '1 line / 1 sweep' }, arm + 'cfd_coeff_preprocess_controller.cc', { drillView: 'accelerator' });
  node('controller', '控制与前沿调度', '控制器', 'dsa', '准入 · 依赖 · commit / detach', '为就绪反馈、前沿 LU、RHS 和后台 LU 分配资源。算法任务与实际共享的计算硬件分别表达。', { 请求窗口: '8', LU上下文: '2', RHS上下文: '2' }, arm + 'cfd_coeff_preprocess_frontier.inc');
  node('dma', 'DMA 接口', '传输', 'dsa', '输入 / tile / 向量 / 完成记录', '通过 timing 读写访问内存。接口中的在途请求、传输区间和完成响应可以分别观测。示例事件表示传输组，并非逐 packet 实测。', { DMA包宽: '64 B', outstanding上限: '32' }, arm + 'cfd_local_spm.cc');
  node('ingress', '输入暂存', '存储区域', 'dsa', 'raw D / L / U / R', 'DMA 响应后仍须经过片上写入与操作数读出，数据才可被计算消费。输入槽与输出快照持有不同生命周期。', { 基础输入槽: '2', 输入格式: 'FP64 · 5×5 / 5' }, arm + 'cfd_coeff_preprocess_tiles.inc');
  node('contexts', '计算上下文', '逻辑分组', 'dsa', 'LU / RHS', '有限上下文保存请求状态；这些上下文共用同一组物理算术资源。', { LU: '2', RHS: '2' }, arm + 'cfd_coeff_preprocess_controller.hh');
  node('lu', 'LU 上下文', '上下文', 'contexts', 'factor(D) · 标量任务', '推进 LU 行组并发布可供 RHS 使用的前沿。LU 任务使用共享乘法、加减和除法资源。', { 上下文数: '2', 工作方式: '前沿优先' }, arm + 'cfd_coeff_preprocess_lu_rows.inc');
  node('rhs', 'RHS 上下文', '上下文', 'contexts', '系数求解 / base solve', '推进 RHS 行任务，与 LU 共同竞争计算簇。上下文数不是独立 TRSM 单元数。', { 上下文数: '2', 本设计配置: 'direct' }, arm + 'cfd_coeff_preprocess_rhs_rows.inc');
  node('cluster', '共享计算簇', '共享硬件', 'dsa', '5 mul + 5 add/sub + 1 div', 'LU、RHS 和列向量任务共享同一组算术硬件。标量任务占用一个发射 lane，列向量任务原子申请五个 lane。', { 乘法发射宽度: '5 lanes', 加减发射宽度: '5 lanes', 除法发射宽度: '1 lane', 延迟: '3 / 4 / 12 cycles', 除法II: '4 cycles' }, arm + 'cfd_coeff_preprocess_cluster.inc', { drillView: 'resources', evidence: '现有 cluster trace 记录 pool / lanes / caller / complete_cycle，未记录具体 lane ID 或 request ID。真实导入时不可据此猜测其所属 cell。' });
  node('mul', '乘法资源池', '资源池', 'cluster', '5 lanes · latency 3', '发射宽度与在途操作数是不同概念。流水线内可以同时存在多个尚未完成的操作。', { 发射宽度: '5 lanes', 延迟: '3 cycles', II: '1 cycle' }, arm + 'cfd_coeff_preprocess_cluster.inc', { evidence: '资源池级观测，不呈现没有 trace 依据的具体 lane 身份。' });
  node('add', '加减资源池', '资源池', 'cluster', '5 lanes · latency 4', '为标量 LU/RHS 和列向量累加提供共享的加减发射槽。反馈链延迟需要通过依赖事件说明。', { 发射宽度: '5 lanes', 延迟: '4 cycles', II: '1 cycle' }, arm + 'cfd_coeff_preprocess_cluster.inc');
  node('div', '共享除法器', '资源池', 'cluster', '1 lane · latency 12 · II 4', '单个流水除法器可每四周期发射一次，完成延迟为十二周期；因此一个发射 lane 可以有多个在途操作。', { 发射宽度: '1 lane', 延迟: '12 cycles', II: '4 cycles' }, arm + 'cfd_coeff_preprocess_cluster.inc');
  node('products', '乘积 / 累加寄存器', '寄存器/FIFO', 'cluster', '有限 credit · 非 SPM', '活跃乘积、累加器和待累加向量是寄存器与有界 FIFO 状态，不按每次运算计入 SPM 访问。', { 共享乘积深度: '8', line乘积深度: '5' }, arm + 'cfd_coeff_preprocess_cluster.inc');
  node('spm', 'Banked SPM', '片上存储', 'dsa', '输入 · 系数缓存 · 驻留 tile', '输入暂存、系数缓存与驻留 tile 共享有限 bank/端口的片上存储。图中区域不表示额外独立读写端口。', { 容量预算: '192 KiB', banks: '4', 每bank端口: '1R / 1W', bank粒度: '8 B', ready延迟: '1 cycle' }, arm + 'cfd_coeff_preprocess_onchip.inc', { drillView: 'resources', evidence: '192 KiB 是统一硬件状态预算上限，含寄存器/FIFO，不能当成 SRAM 利用率或综合面积。' });
  node('onchip', '片上访问仲裁', '逻辑分组', 'spm', '有限任务队列 · 前沿优先', '按任务优先级和编号稳定仲裁每字访问。数据已驻留仍需经过端口授予和 ready 延迟。', { 任务队列深度: '64', 最低准入等待: '1 cycle', 完成ready延迟: '1 cycle' }, arm + 'cfd_coeff_preprocess_onchip.inc');
  for (let bank = 0; bank < 4; bank++) node('bank' + bank, 'Bank ' + bank, 'SPM bank', 'spm', '1R / 1W · grain 8 B', '地址按 (address / 8) % 4 映射至此 bank。每周期分别约束读写端口；平均利用率不能解释局部冲突。', { bank编号: String(bank), 读端口: '1', 写端口: '1', 字宽: '8 B' }, arm + 'cfd_coeff_preprocess_onchip.inc');
  node('tile', '驻留 Tile A / B', '存储区域', 'spm', 'y / Ubar · 双缓冲', '有限 tile 缓冲存放前向结果及后向所需系数。进入尾 tile 时可留存前一块，DMA 引用保护其生命周期。', { tile长度: '8 cells', 缓冲数: '2', 每cell数据: '240 B' }, arm + 'cfd_coeff_preprocess_tiles.inc');
  node('feedback', '前向 / 后向转发', '寄存器/控制', 'dsa', 'y[i−1] / dq[i+1]', '把相邻 cell 的递推结果转发至列向量计算。这里是逻辑数据与依赖边界，运算仍经过共享计算簇。', { 数据宽度: '5 × FP64', 依赖: '单线顺序递推' }, arm + 'cfd_coeff_preprocess_frontier.inc');
  node('output', '输出快照队列', '有界FIFO', 'dsa', '结果写回 · 持有 DMA 引用', '计算完成后将结果快照放入有限队列，输入槽可以先释放。输出响应回来后才释放对应写回引用。', { warm写队列深度: '8', 向量宽度: '40 B' }, arm + 'cfd_coeff_preprocess_tiles.inc');
  node('completion', '完成记录队列', '有界FIFO', 'dsa', 'commit → detach → retire', '计算请求 detach 后，完成记录独立写回；最终完成状态仍须等待必要 DMA 响应。', { 队列深度: '8', 每项预算: '1024 B' }, arm + 'cfd_coeff_preprocess_lifecycle.inc');
  const edges = [];
  const edge = (id, from, to, kind, label) => edges.push({ id, from, to, kind, label });
  edge('cpu-dsa', 'cpu', 'dsa', 'control', 'launch / wait');
  edge('memory-dsa', 'memory', 'dsa', 'data', 'timing ReadReq / WriteReq');
  edge('cpu-controller', 'cpu', 'controller', 'control', 'descriptor / token');
  edge('controller-dma', 'controller', 'dma', 'control', '提交传输');
  edge('memory-dma', 'memory', 'dma', 'data', 'timing 请求 / 响应');
  edge('dma-ingress', 'dma', 'ingress', 'data', 'D / L / U / R');
  edge('ingress-lu', 'ingress', 'lu', 'data', 'D');
  edge('ingress-rhs', 'ingress', 'rhs', 'data', 'L / U / R');
  edge('lu-cluster', 'lu', 'cluster', 'data', '标量运算');
  edge('rhs-cluster', 'rhs', 'cluster', 'data', 'RHS 运算');
  edge('cluster-spm', 'cluster', 'spm', 'data', '系数存入');
  edge('spm-cluster', 'spm', 'cluster', 'data', '操作数读出');
  edge('cluster-feedback', 'cluster', 'feedback', 'data', '递推结果');
  edge('feedback-cluster', 'feedback', 'cluster', 'control', '相邻点依赖');
  edge('feedback-output', 'feedback', 'output', 'data', 'y / dq');
  edge('spm-tile', 'spm', 'tile', 'data', '驻留区域');
  edge('tile-feedback', 'tile', 'feedback', 'data', '后向读取');
  edge('output-dma', 'output', 'dma', 'data', '结果写回');
  edge('output-completion', 'output', 'completion', 'control', 'detach');
  edge('completion-dma', 'completion', 'dma', 'data', '完成记录');
  edge('mul-products', 'mul', 'products', 'data', '乘积');
  edge('products-add', 'products', 'add', 'data', '累加');
  for (let bank = 0; bank < 4; bank++) edge('onchip-bank' + bank, 'onchip', 'bank' + bank, 'data', '字访问');
  const box = (id, x, y, w = 220, h = 90) => ({ id, x, y, w, h });
  const views = [
    { id: 'system', label: '系统概览', breadcrumb: 'SYSTEM / INTERFACE', width: 1160, height: 650,
      nodes: [box('cpu', 95, 115, 270, 125), box('memory', 95, 380, 270, 125), box('dsa', 660, 240, 350, 165)],
      edges: ['cpu-dsa', 'memory-dsa'], groups: [{ x: 55, y: 55, w: 355, h: 515, label: 'HOST / MEMORY' }, { x: 595, y: 120, w: 470, h: 410, label: 'ACCELERATOR' }] },
    { id: 'accelerator', label: '加速器内部', breadcrumb: 'DSA / COMPUTE & STORAGE', width: 1160, height: 660,
      nodes: [box('cpu', 35, 55, 230, 80), box('controller', 400, 55, 360, 80), box('memory', 895, 55, 230, 80), box('dma', 895, 155, 230, 70), box('ingress', 35, 270, 215, 100), box('lu', 330, 215, 200, 85), box('rhs', 330, 345, 200, 85), box('cluster', 610, 270, 235, 145), box('feedback', 655, 455, 190, 75), box('spm', 325, 525, 290, 95), box('tile', 655, 555, 200, 65), box('output', 895, 290, 230, 100), box('completion', 895, 500, 230, 100)],
      edges: ['cpu-controller', 'controller-dma', 'memory-dma', 'dma-ingress', 'ingress-lu', 'ingress-rhs', 'lu-cluster', 'rhs-cluster', 'cluster-spm', 'spm-cluster', 'cluster-feedback', 'feedback-cluster', 'feedback-output', 'spm-tile', 'tile-feedback', 'output-dma', 'output-completion', 'completion-dma'],
      routes: {
        'cpu-controller': [[265, 95], [400, 95]], 'controller-dma': [[760, 95], [875, 95], [875, 175], [895, 175]],
        'memory-dma': [[1010, 135], [1010, 155]], 'dma-ingress': [[895, 203], [142, 203], [142, 270]],
        'ingress-lu': [[250, 295], [285, 295], [285, 257], [330, 257]], 'ingress-rhs': [[250, 340], [280, 340], [280, 387], [330, 387]],
        'lu-cluster': [[530, 257], [575, 257], [575, 307], [610, 307]], 'rhs-cluster': [[530, 387], [575, 387], [575, 370], [610, 370]],
        'cluster-spm': [[610, 400], [592, 400], [592, 525]], 'spm-cluster': [[615, 572], [635, 572], [635, 415]],
        'cluster-feedback': [[748, 415], [748, 455]], 'feedback-cluster': [[845, 484], [864, 484], [864, 390], [845, 390]],
        'feedback-output': [[845, 508], [880, 508], [880, 340], [895, 340]], 'spm-tile': [[615, 598], [655, 598]], 'tile-feedback': [[755, 555], [755, 530]],
        'output-dma': [[1125, 330], [1140, 330], [1140, 190], [1125, 190]], 'output-completion': [[1010, 390], [1010, 500]], 'completion-dma': [[1125, 550], [1152, 550], [1152, 210], [1125, 210]]
      }, groups: [{ x: 310, y: 170, w: 550, h: 275, label: 'CONTEXTS → SHARED HARDWARE' }] },
    { id: 'resources', label: '计算与存储资源', breadcrumb: 'DSA / PHYSICAL RESOURCE POOLS', width: 1160, height: 650,
      nodes: [box('mul', 85, 115, 275, 105), box('add', 440, 115, 275, 105), box('div', 795, 115, 275, 105), box('products', 440, 255, 275, 75), box('onchip', 70, 470, 220, 95), ...Array.from({ length: 4 }, (_, b) => box('bank' + b, 350 + b * 195, 470, 170, 95))],
      edges: ['mul-products', 'products-add', 'onchip-bank0', 'onchip-bank1', 'onchip-bank2', 'onchip-bank3'],
      routes: { 'mul-products': [[222, 220], [222, 292], [440, 292]], 'products-add': [[577, 255], [577, 220]], ...Object.fromEntries(Array.from({ length: 4 }, (_, b) => ['onchip-bank' + b, [[290, 490 + b * 15], [315 + b * 10, 490 + b * 15], [315 + b * 10, 420 + b * 10], [435 + b * 195, 420 + b * 10], [435 + b * 195, 470]]])) },
      groups: [{ x: 40, y: 55, w: 1080, h: 295, label: 'SHARED CLUSTER · LU / RHS / VECTOR WORK' }, { x: 40, y: 385, w: 1080, h: 220, label: 'BANKED SPM · REGIONS SHARE THESE PORTS' }] }
  ];
  const entities = Array.from({ length: 4 }, (_, cell) => ({ id: 'line0:g0:cell' + cell, label: 'line 0 · cell ' + cell, cell, generation: 0 }));
  const events = [];
  const event = (moduleId, start, end, kind, label, entityId, edgeIds, attrs = {}) => events.push({ id: 'demo-' + events.length, moduleId, start, end, kind, label, entityId, edgeIds, evidence: 'illustrative', ...attrs });
  event('controller', 0, 8, 'control', '单线启动 / 准入', null, ['cpu-dsa', 'cpu-controller', 'controller-dma']);
  for (let i = 0; i < 4; i++) {
    const entity = entities[i].id, read = 8 + i * 16, base = 42 + i * 54;
    event('dma', read, read + 18, 'transfer', 'D/L/U/R 输入传输组', entity, ['memory-dsa', 'memory-dma', 'dma-ingress'], { bytes: 640 });
    event('bank' + i, read + 18, read + 22, 'transfer', '片上写入片段', entity, ['dma-ingress', 'onchip-bank' + i], { bytes: 32 });
    event('ingress', read + 22, base, 'wait', '等待 LU 上下文准入', entity, ['ingress-lu'], { reason: '等待计算前沿 / 有限上下文' });
    event('lu', base, base + 35, 'control', 'LU 上下文推进（片段）', entity, ['ingress-lu', 'lu-cluster']);
    event('mul', base, base + 3, 'compute', 'LU 标量乘法', entity, ['lu-cluster'], { lanes: 1 });
    event('add', base + 3, base + 7, 'compute', 'LU 标量减法', entity, ['lu-cluster'], { lanes: 1 });
    event('div', base + 7, base + 19, 'compute', 'LU 标量除法', entity, ['lu-cluster'], { lanes: 1 });
    event('mul', base + 20, base + 23, 'compute', 'LU 下一目标乘法', entity, ['lu-cluster'], { lanes: 1 });
    event('div', base + 23, base + 35, 'compute', 'LU 前沿除法', entity, ['lu-cluster'], { lanes: 1 });
    event('rhs', base + 35, base + 63, 'control', 'RHS / base solve（片段）', entity, ['ingress-rhs', 'rhs-cluster']);
    event('mul', base + 35, base + 38, 'compute', 'RHS 列向量乘法', entity, ['rhs-cluster'], { lanes: 5 });
    event('add', base + 38, base + 42, 'compute', 'RHS 列向量累加', entity, ['rhs-cluster'], { lanes: 5 });
    event('mul', base + 42, base + 45, 'compute', 'RHS 第二列乘法', entity, ['rhs-cluster'], { lanes: 5 });
    event('add', base + 45, base + 49, 'compute', 'RHS 第二列累加', entity, ['rhs-cluster'], { lanes: 5 });
    event('div', base + 49, base + 61, 'compute', 'RHS 标量除法', entity, ['rhs-cluster'], { lanes: 1 });
    event('feedback', base + 63, base + 72, 'control', '前向结果转发', entity, ['cluster-feedback', 'feedback-output']);
    event('output', base + 72, base + 88, 'transfer', 'y 输出快照持有', entity, ['feedback-output', 'output-dma'], { bytes: 40 });
    event('dma', base + 72, base + 88, 'transfer', 'y 向量写回', entity, ['output-dma', 'memory-dma', 'memory-dsa'], { bytes: 40 });
    event('completion', base + 82, base + 98, 'transfer', '完成记录异步排出', entity, ['output-completion', 'completion-dma'], { bytes: 320 });
  }
  event('tile', 284, 306, 'transfer', '后向 tile 回读（示意）', entities[3].id, ['memory-dma', 'spm-tile', 'tile-feedback'], { bytes: 240 });
  event('dma', 284, 306, 'transfer', 'tile 读回片段', entities[3].id, ['memory-dma', 'memory-dsa', 'spm-tile'], { bytes: 240 });
  for (let j = 0; j < 4; j++) {
    const entity = entities[3 - j].id, base = 310 + j * 27;
    event('feedback', base, base + 27, 'control', '后向 cell 递推', entity, ['tile-feedback', 'feedback-cluster', 'cluster-feedback']);
    for (let k = 0; k < 5; k++) {
      event('mul', base + k, base + k + 3, 'compute', '后向列 ' + k + ' 乘法', entity, ['spm-cluster', 'feedback-cluster'], { lanes: 5 });
      event('add', base + 3 + k * 4, base + 7 + k * 4, 'compute', '后向列 ' + k + ' 累加', entity, ['cluster-feedback'], { lanes: 5 });
    }
    event('add', base + 23, base + 27, 'compute', '后向最终向量减法', entity, ['cluster-feedback'], { lanes: 5 });
    event('output', base + 27, base + 39, 'transfer', 'dq 最终向量写回', entity, ['feedback-output', 'output-dma'], { bytes: 40 });
  }
  event('dma', 418, 448, 'transfer', '最终向量批量写回', null, ['output-dma', 'memory-dma', 'memory-dsa'], { bytes: 160 });
  event('controller', 448, 462, 'control', '等待必要响应 / 完成 fence', null, ['cpu-dsa', 'cpu-controller', 'completion-dma']);
  const session = {
    schemaVersion: 1,
    meta: { name: '单线 · cold direct · 流程片段', provenance: 'demo', timeUnit: 'cycle', clockDomain: '控制器时钟域', start: 0, end: 480, defaultTime: 112, defaultView: 'accelerator', configSummary: 'shared 5/5/1 · 4 banks · tile 8 × 2', configSource: 'projects/cfd_dsa/suites/single-line-round2-recommended.json / cold-direct-b4-t8-q64', description: '结构参数参考当前推荐配置；全部事件为人工编排的交互示意，省略大量运算，不对应完整算例或测量结果。' },
    nodes, edges, views, entities, events,
    tracks: [
      { id: 'control', label: '控制 / 上下文', moduleIds: ['controller', 'lu', 'rhs'] },
      { id: 'dma', label: 'DMA', moduleIds: ['dma'] },
      { id: 'spm', label: '片上存储', moduleIds: ['bank0', 'bank1', 'bank2', 'bank3', 'tile'] },
      { id: 'input', label: '输入等待', moduleIds: ['ingress'] },
      { id: 'mul', label: '乘法资源池', moduleIds: ['mul'] },
      { id: 'add', label: '加减资源池', moduleIds: ['add'] },
      { id: 'div', label: '除法资源池', moduleIds: ['div'] },
      { id: 'output', label: '反馈 / 输出', moduleIds: ['feedback', 'output'] },
      { id: 'completion', label: '完成记录', moduleIds: ['completion'] }
    ]
  };
  if (typeof module !== 'undefined' && module.exports) module.exports = require('./detail-architecture.js')(session);
  else root.CFD_DEMO_SESSION = root.CfdArchitectureDetails(session);
})(typeof window !== 'undefined' ? window : globalThis);
