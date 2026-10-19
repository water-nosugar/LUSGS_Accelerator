/* Standalone structural drawings. The replay model and its views are untouched. */
(function (root) {
  'use strict';
  const palette = {
    control: ['#34363c', '#a4a8b3'], compute: ['#301741', '#a768c7'],
    storage: ['#293519', '#a4b25c'], state: ['#17333e', '#60a1b7'], cache: ['#421e23', '#c16c75']
  };
  const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' }[c]));
  const clip = (value, length = 40) => String(value).length > length ? String(value).slice(0, length - 1) + '…' : String(value);
  function create(session, ownerId = 'dsa') {
    const nodes = new Map(session.nodes.map(n => [n.id, n]));
    const owner = nodes.get(ownerId);
    if (!owner) throw new Error('当前会话没有此模块');
    const parts = [], boxes = [], references = new Set(), width = 1280;
    let height = 1160;
    const p = (id, key) => String(nodes.get(id)?.params?.[key] ?? '未提供');
    const text = (x, y, value, size = 17, color = '#e5e5ea', anchor = 'middle', weight = 400) => {
      parts.push(`<text x="${x}" y="${y}" font-size="${size}" fill="${color}" text-anchor="${anchor}" font-weight="${weight}">${escape(value)}</text>`);
    };
    function target(id, content) {
      if (!nodes.has(id)) return content;
      references.add(id);
      return `<g data-poster-node="${escape(id)}" role="button" tabindex="0" aria-label="${escape(nodes.get(id).label)}${nodes.get(id).drillView ? '，展开分区' : '，查看参数'}"><title>${escape(nodes.get(id).label)}</title>${content}</g>`;
    }
    function frame(x, y, w, h, label, id, tone = 'control') {
      const color = palette[tone];
      parts.push(`<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="3" fill="#25262b" stroke="#3a3c44"/>`);
      const start = parts.length;
      text(x + 18, y + 30, label, 19, color[1], 'start', 600);
      text(x + w - 17, y + 30, nodes.get(id)?.drillView ? '展开 ↗' : '', 13, '#9398a3', 'end');
      const content = parts.splice(start).join('');
      parts.push(target(id, `<rect x="${x + 6}" y="${y + 4}" width="${w - 12}" height="36" fill="transparent"/>` + content));
    }
    function card(id, x, y, w, h, title, lines = [], tone = 'control') {
      if (!nodes.has(id)) return;
      const [fill, stroke] = palette[tone];
      const start = parts.length;
      parts.push(`<rect class="poster-unit" x="${x}" y="${y}" width="${w}" height="${h}" fill="${fill}" stroke="${stroke}" stroke-width="1.3"/>`);
      const size = w < 145 ? 14 : 17;
      const total = lines.length + 1, step = Math.min(24, (h - 18) / total);
      const top = y + h / 2 - (total - 1) * step / 2 + 5;
      text(x + w / 2, top, clip(title || nodes.get(id).label, Math.floor(w / (size * 0.58))), size, '#f0edf2', 'middle', 500);
      lines.forEach((line, i) => text(x + w / 2, top + (i + 1) * step, clip(line, Math.floor(w / 8)), Math.min(size, 14), '#c4c7d0'));
      parts.push(target(id, parts.splice(start).join('')));
      boxes.push({ id, x, y, w, h });
    }
    function arrow(points, label = '', kind = 'data') {
      const color = kind === 'control' ? '#8f939e' : '#aeb4c1';
      parts.push(`<path d="${points.map((point, i) => (i ? 'L' : 'M') + point.join(' ')).join(' ')}" fill="none" stroke="${color}" stroke-width="1.2" ${kind === 'control' ? 'stroke-dasharray="5 4"' : ''} marker-end="url(#poster-arrow)"/>`);
      if (label) { const a = points[0], b = points[points.length - 1]; text((a[0] + b[0]) / 2 + 9, (a[1] + b[1]) / 2 - 7, label, 12, '#a4a9b5', 'start'); }
    }
    function lanes(poolId, x, y, w, h, prefix) {
      const children = session.nodes.filter(n => n.parent === poolId && n.type === '模型资源单元');
      if (!children.length) { card(poolId, x, y, w, h, nodes.get(poolId)?.label, [p(poolId, '发射宽度')], 'compute'); return; }
      const gap = 8, cell = (w - gap * (children.length - 1)) / children.length;
      children.forEach((n, i) => card(n.id, x + i * (cell + gap), y, cell, h, prefix, ['#' + i], 'compute'));
    }
    function heading(title, subtitle) {
      text(640, 59, title, 30, '#f0edf2', 'middle', 600);
      text(640, 91, subtitle, 14, '#8f95a3');
    }
    function dsa() {
      heading('CFD DSA · 微体系结构', '控制与调度 / 上下文状态 / 共享算术资源 / 数据搬运');
      card('cpu', 460, 118, 360, 59, 'CPU / Guest', ['descriptor · launch · wait'], 'state');
      arrow([[640, 177], [640, 200]], '', 'control');
      frame(50, 200, 1180, 168, '控制与前沿调度', 'controller');
      const controlCards = [
        ['ctrl-descriptor', 'Descriptor / token', '解析与请求身份'], ['ctrl-window', '请求窗口 / credit', p('controller', '请求窗口') + ' entries'],
        ['ctrl-admission', '上下文准入', 'LU ' + p('controller', 'LU上下文') + ' / RHS ' + p('controller', 'RHS上下文')],
        ['ctrl-frontier', '前沿 / 依赖', 'ready · masks'], ['ctrl-arbitration', '优先级仲裁', '共享资源发射许可']
      ];
      controlCards.forEach(([id, title, value], i) => card(id, 70 + i * 234, 248, 216, 72, title, [value]));
      text(640, 347, 'Generation 校验 · 取消管理 · 完成 fence', 14, '#adb1bc');
      for (const x of [217, 640, 1063]) arrow([[x, 368], [x, 399]], '', 'control');

      frame(50, 400, 334, 513, '上下文与递推状态', 'contexts', 'state');
      card('lu', 70, 452, 294, 94, 'LU 上下文 ×' + p('lu', '上下文数'), ['矩阵 / 暂存 / pivot', '列、行、k 状态 · 除法掩码'], 'state');
      card('rhs', 70, 574, 294, 94, 'RHS 上下文 ×' + p('rhs', '上下文数'), ['每上下文 ' + p('rhs-context0', 'RHS状态槽') + ' 个状态槽', '向量值 · phase · ready / done'], 'state');
      card('feedback', 70, 696, 294, 97, '前向 / 后向递推', [p('feedback', '数据宽度'), 'handoff · forward / backward / base'], 'state');
      card('ingress', 70, 821, 294, 68, '输入暂存 ×' + p('ingress', '基础输入槽'), ['D / L / U / R · ready'], 'state');

      frame(414, 400, 452, 513, '共享计算簇 · FP64', 'cluster', 'compute');
      card('cluster-issue', 434, 452, 412, 57, '发射与 lane 预留', ['标量 / 向量请求共用一个资源池']);
      arrow([[640, 509], [640, 530]]);
      lanes('mul', 434, 535, 412, 55, 'MUL');
      text(640, 612, '乘法 ×' + session.nodes.filter(n => n.parent === 'mul' && n.type === '模型资源单元').length + ' · Lat ' + p('mul', '延迟') + ' · II ' + p('mul', 'II'), 13, '#b8a3c7');
      arrow([[640, 622], [640, 639]]);
      card('products', 434, 644, 412, 59, '乘积 / 累加寄存器', ['共享深度 ' + p('products', '共享乘积深度') + ' · line 深度 ' + p('products', 'line乘积深度')], 'state');
      arrow([[640, 703], [640, 725]]);
      lanes('add', 434, 730, 412, 55, 'ADD/SUB');
      text(640, 807, '加减 ×' + session.nodes.filter(n => n.parent === 'add' && n.type === '模型资源单元').length + ' · Lat ' + p('add', '延迟') + ' · II ' + p('add', 'II'), 13, '#b8a3c7');
      card('div', 434, 832, 190, 57, 'DIV ×' + p('div', '发射宽度').split(' ')[0], ['Lat ' + p('div', '延迟').replace(' cycles', '') + ' · II ' + p('div', 'II').replace(' cycles', '') + ' cycles'], 'compute');
      card('cluster-pending', 640, 832, 206, 57, '在途操作完成表', ['kind / lanes / 完成周期']);

      frame(896, 400, 334, 513, '搬运与片上存储', 'spm', 'storage');
      card('dma', 916, 452, 294, 75, 'DMA 接口', [p('dma', 'DMA包宽') + ' / packet', p('dma', 'outstanding上限') + ' outstanding'], 'storage');
      arrow([[1063, 527], [1063, 548]]);
      card('onchip', 916, 553, 294, 60, '片上访问仲裁', [p('onchip', '任务队列深度') + ' entries · priority / id'], 'storage');
      arrow([[1063, 613], [1063, 635]]);
      const banks = session.nodes.filter(n => n.parent === 'spm' && /^bank\d+$/.test(n.id));
      const bankWidth = (294 - (banks.length - 1) * 10) / banks.length;
      banks.forEach((n, i) => card(n.id, 916 + i * (bankWidth + 10), 640, bankWidth, 58, 'B' + i, [bankWidth < 50 ? 'R/W' : p('spm', '每bank端口')], 'storage'));
      text(1063, 719, '每 bank ' + p('spm', '每bank端口') + ' · ' + p('spm', 'bank粒度') + ' grain', 13, '#b2bb91');
      text(1063, 738, 'ready ' + p('spm', 'ready延迟'), 12, '#b2bb91');
      card('spm-cache', 916, 752, 294, 57, '有限系数缓存', ['系数区域 · 有效性 / version / mode'], 'storage');
      card('tile', 916, 832, 294, 57, '驻留 Tile A / B', [p('tile', '缓冲数') + ' buffers × ' + p('tile', 'tile长度')], 'storage');

      arrow([[384, 492], [414, 492]], '', 'control');
      arrow([[866, 672], [896, 672]]);
      text(399, 569, '请求', 11, '#959ba8');
      for (const x of [217, 640, 1063]) arrow([[x, 913], [x, 944]]);
      frame(50, 944, 1180, 142, '结果发布与请求生命周期', 'completion', 'state');
      card('output', 70, 994, 354, 64, '输出快照队列', [p('output', 'warm写队列深度') + ' slots · ' + p('output', '向量宽度') + ' / vector'], 'state');
      card('completion', 463, 994, 354, 64, '完成记录队列', [p('completion', '队列深度') + ' slots · ' + p('completion', '每项预算') + ' 预留 / entry'], 'state');
      card('ctrl-fence', 856, 994, 354, 64, 'Commit / Detach / Retire', ['响应闭合 · credit 回收']);
      arrow([[424, 1039], [438, 1039], [438, 1074], [838, 1074], [838, 1039], [856, 1039]], '', 'control');
      arrow([[817, 1013], [856, 1013]], '', 'control');
      text(640, 1115, '统一设备状态预算 ' + p('spm', '容量预算') + '（含寄存器与队列） · 分区为模型结构，连线为功能关系', 13, '#9096a2');
    }
    function cpu() {
      height = 1020;
      heading('CPU / Guest · O3 微体系结构', '统一 IQ 与 FU pool · 宽度、容量取自当前会话的结构参数');
      card('mem-l1i', 460, 125, 360, 67, 'L1 Instruction Cache', [p('mem-l1i', '容量') + ' · ' + p('mem-l1i', '相联度') + '-way'], 'cache');
      arrow([[640, 192], [640, 226]]);
      frame(60, 226, 1160, 277, '前端 · Fetch / Decode / Rename', 'cpu');
      card('cpu-fetch', 345, 278, 590, 52, 'Fetch & Branch Prediction', [p('cpu-fetch', '宽度')]);
      arrow([[640, 330], [640, 348]]);
      card('cpu-decode', 345, 353, 590, 52, 'Decode', [p('cpu-decode', '宽度')]);
      arrow([[640, 405], [640, 423]]);
      card('cpu-rename', 85, 428, 1110, 54, 'Map and Rename', [p('cpu-rename', '宽度')]);
      card('cpu-rename', 85, 353, 235, 52, '整数物理寄存器', [p('cpu-rename', 'int寄存器') + ' entries'], 'state');
      card('cpu-rename', 960, 353, 235, 52, '向量 / 矩阵寄存器', [p('cpu-rename', 'vec寄存器') + ' / ' + p('cpu-rename', 'mat寄存器') + ' entries'], 'state');
      arrow([[640, 503], [640, 536]]);
      card('cpu-iq', 85, 541, 730, 76, '统一 Instruction Queue / Scheduler', ['默认 ' + p('cpu-iq', '默认条目') + ' entries · Issue ' + p('cpu-iq', '发射宽度') + '-wide'], 'compute');
      card('cpu-lsq', 845, 541, 350, 76, 'Load / Store Queue', ['LQ ' + p('cpu-lsq', 'LQ') + ' / SQ ' + p('cpu-lsq', 'SQ') + ' entries'], 'storage');
      arrow([[450, 617], [450, 654]]); arrow([[1020, 617], [1020, 654]]);
      card('cpu-fu', 85, 659, 730, 104, 'CPU Functional Unit Pool', ['整数 · 标量浮点 · SIMD · Matrix', p('cpu-fu', '计数'), 'Writeback ' + p('cpu-fu', 'wb宽度') + '-wide'], 'compute');
      card('mem-l1d', 845, 659, 350, 104, 'L1 Data Cache', [p('mem-l1d', '容量') + ' · ' + p('mem-l1d', '相联度') + '-way', 'MSHR ' + p('mem-l1d', 'MSHRs')], 'cache');
      arrow([[450, 763], [450, 800]]);
      card('cpu-rob', 85, 805, 730, 76, 'Reorder Buffer / Commit', ['默认 ROB ' + p('cpu-rob', '默认ROB') + ' entries · Commit ' + p('cpu-rob', '提交宽度') + '-wide'], 'state');
      card('cpu-launch', 845, 805, 350, 76, 'DSA 软件启动接口', ['descriptor · launch / wait'], 'state');
      arrow([[1020, 881], [1020, 913]], '', 'control');
      card('controller', 845, 918, 350, 56, 'CFD 控制器', [], 'control');
      text(450, 951, 'O3 的 IQ、ROB 默认条目数仍可被运行配置覆盖', 14, '#9399a6');
    }
    function memory() {
      height = 1120;
      heading('存储系统 · Cache 与互连', 'CPU 指令 / 数据与 DSA DMA 汇合到 classic memory system');
      card('cpu', 160, 125, 410, 59, 'CPU / Guest', [], 'control');
      card('dma', 890, 125, 300, 59, 'CFD DSA · DMA', [], 'control');
      arrow([[365, 184], [365, 207], [235, 207], [235, 238]]);
      arrow([[365, 207], [630, 207], [630, 238]]);
      arrow([[1040, 184], [1040, 238]]);
      for (const [id, x, title] of [['mem-l1i', 85, 'L1 Instruction Cache'], ['mem-l1d', 480, 'L1 Data Cache'], ['mem-dma-cache', 890, 'DMA Cache']]) {
        card(id, x, 243, 300, 87, title, [p(id, '容量') + ' · ' + p(id, '相联度') + '-way', 'MSHR ' + p(id, 'MSHRs')], 'cache');
      }
      for (const x of [235, 630, 1040]) arrow([[x, 330], [x, 378]]);
      card('mem-l2bus', 85, 383, 1105, 64, 'toL2Bus · L2XBar', [p('mem-l2bus', '宽度') + ' width']);
      arrow([[640, 447], [640, 487]]);
      card('mem-l2', 345, 492, 590, 87, 'L2 Cache', [p('mem-l2', '容量') + ' · ' + p('mem-l2', '相联度') + '-way', 'MSHR ' + p('mem-l2', 'MSHRs')], 'cache');
      arrow([[640, 579], [640, 619]]);
      card('mem-bus', 85, 624, 1105, 64, 'membus · SystemXBar', [p('mem-bus', '宽度') + ' width']);
      arrow([[365, 688], [365, 742]]);
      arrow([[1020, 688], [1020, 742]], 'uncached 地址范围');
      card('mem-dram', 85, 747, 550, 96, 'DRAM · SimpleMemory', ['默认 ' + p('mem-dram', '默认容量'), p('mem-dram', '延迟') + ' · ' + p('mem-dram', '带宽')], 'storage');
      card('mem-simple-spm', 675, 747, 515, 96, '映射 SPM · SimpleMemory', [p('mem-simple-spm', '大小') + ' @ ' + p('mem-simple-spm', '地址'), 'uncached · 独立地址设备'], 'storage');
      frame(85, 897, 1105, 145, 'CFD 设备内部 · 单独展开', 'spm', 'state');
      card('spm', 108, 946, 521, 68, 'Banked SPM / 寄存器 / 队列', ['统一状态预算 ' + p('spm', '容量预算')], 'state');
      text(915, 970, '映射 SPM 与设备状态预算', 17, '#bac0cb');
      text(915, 996, '使用不同口径，分别显示', 15, '#9399a6');
      text(640, 1076, 'SimpleMemory 提供容量、延迟和带宽模型；当前配置没有真实 DRAM 控制器内部单元', 13, '#9096a2');
    }
    function detail() {
      const view = session.views.find(v => v.id === owner.drillView && (!v.ownerId || v.ownerId === ownerId));
      if (!view) {
        height = 480; heading(owner.label, '模块结构参数');
        card(ownerId, 160, 160, 960, 180, owner.label, Object.entries(owner.params || {}).slice(0, 5).map(([k, v]) => k + ' · ' + v), 'state');
        return;
      }
      // Existing detail geometry and links are reused in this independent drawing.
      const scale = 1160 / view.width, offset = 60, top = 105;
      height = Math.max(620, view.height * scale + 165);
      heading(owner.label + ' · 单元展开', '点击有内部结构的分区继续展开 · 点击单元查看参数与来源');
      const headers = [];
      for (const g of view.groups || []) {
        const tone = ['mul', 'add', 'div'].includes(g.ownerId) ? 'compute' : 'state';
        frame(g.x * scale + offset, g.y * scale + top, g.w * scale, g.h * scale, g.label, g.ownerId, tone);
        headers.push(parts.pop());
      }
      const layout = new Map(view.nodes.map(b => [b.id, { ...b, x: b.x * scale + offset, y: b.y * scale + top, w: b.w * scale, h: b.h * scale }]));
      for (const id of view.edges || []) {
        const edge = session.edges.find(e => e.id === id);
        if (!edge) continue;
        const a = layout.get(edge.from), b = layout.get(edge.to);
        if (!a || !b) continue;
        if (Math.abs(a.y - b.y) < 1) {
          const right = a.x < b.x;
          arrow([[right ? a.x + a.w : a.x, a.y + a.h / 2], [right ? b.x : b.x + b.w, b.y + b.h / 2]], '', edge.kind);
        } else {
          const down = a.y < b.y, ay = down ? a.y + a.h : a.y, by = down ? b.y : b.y + b.h;
          const ax = a.x + a.w / 2, bx = b.x + b.w / 2, mid = (ay + by) / 2;
          arrow([[ax, ay], [ax, mid], [bx, mid], [bx, by]], '', edge.kind);
        }
      }
      // Keep group headings above routed links; their label band stays readable.
      for (const header of headers) parts.push(header.replace('fill="transparent"', 'fill="#25262b"'));
      for (const b of layout.values()) {
        const n = nodes.get(b.id); if (!n) continue;
        let tone = 'control';
        if (['模型资源单元', '资源池'].includes(n.type) || /^mul|^add|^div/.test(n.id)) tone = 'compute';
        else if (n.type === 'Cache') tone = 'cache';
        else if (/SPM|bank|onchip|tile/.test(n.id)) tone = 'storage';
        else if (/寄存器|槽|队列|上下文/.test(n.type)) tone = 'state';
        const lines = b.w < 145 ? Object.values(n.params || {}).slice(0, 1) : Object.entries(n.params || {}).slice(0, b.h < 85 ? 2 : 3).map(([k, v]) => k + ' · ' + v);
        card(n.id, b.x, b.y, b.w, b.h, n.label, lines, tone);
      }
      text(640, height - 31, '结构单元与当前会话一致 · 箭头表达数据 / 控制关系 · 单元活动状态未采集', 13, '#9096a2');
    }
    // Special compositions only apply when their required structure is present.
    const known = ids => ids.every(id => nodes.has(id));
    const bankCount = session.nodes.filter(n => n.parent === 'spm' && /^bank\d+$/.test(n.id)).length;
    if (ownerId === 'dsa' && bankCount <= 8 && known(['cluster-issue', 'mul-lane4', 'rhs-context0', 'bank3', 'ctrl-fence'])) dsa();
    else if (ownerId === 'cpu' && known(['cpu-rename', 'cpu-fu', 'cpu-rob', 'mem-l1i'])) cpu();
    else if (ownerId === 'memory' && known(['mem-l2bus', 'mem-l2', 'mem-bus', 'mem-simple-spm'])) memory();
    else detail();
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" role="group" aria-label="${escape(owner.label)}结构图" font-family="Arial, 'Noto Sans CJK SC', sans-serif"><defs><marker id="poster-arrow" viewBox="0 0 6 6" refX="5" refY="3" markerWidth="5" markerHeight="5" orient="auto-start-reverse"><path d="M0 0L6 3L0 6Z" fill="#a5abb8"/></marker></defs><rect width="${width}" height="${height}" fill="#141518"/><rect x="15" y="15" width="1250" height="${height - 30}" rx="26" fill="#191a1e" stroke="#383d48" stroke-width="6"/>${parts.join('')}</svg>`;
    return { ownerId, title: owner.label, width, height, svg, boxes, references: [...references] };
  }
  const api = { create };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.CFDPosterLayout = api;
})(typeof window !== 'undefined' ? window : globalThis);
