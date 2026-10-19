(function () {
  'use strict';
  const M = window.CfdModel, NS = 'http://www.w3.org/2000/svg';
  const $ = id => document.getElementById(id);
  const graph = $('architecture-svg'), timeline = $('timeline-svg');
  const state = { session: null, view: 'accelerator', selected: 'cluster', entity: null, time: 112, event: null, playing: false, speed: 1, expanded: new Set() };
  const camera = { scale: 1, x: 0, y: 0, fit: true };
  let drag = null, timer = null, noticeTimer = null, timelineScrubbing = false, graphDragged = false;
  const measureCanvas = document.createElement('canvas'), measure = measureCanvas.getContext('2d');
  const svg = (tag, attrs = {}, text) => {
    const el = document.createElementNS(NS, tag);
    for (const [key, value] of Object.entries(attrs)) el.setAttribute(key, String(value));
    if (text !== undefined) el.textContent = text;
    return el;
  };
  const html = (tag, className, text) => {
    const el = document.createElement(tag);
    if (className) el.className = className;
    if (text !== undefined) el.textContent = text;
    return el;
  };
  const currentView = () => state.session.views.find(v => v.id === state.view);
  const currentNode = () => state.session.nodes.find(n => n.id === state.selected);
  const timeLabel = value => value.toLocaleString() + ' ' + state.session.meta.timeUnit;
  const entityLabel = id => state.session.entities.find(e => e.id === id)?.label || '全局事件';
  const observed = () => state.session.meta.provenance === 'recorded';
  const childrenOf = id => state.session.nodes.filter(node => node.parent === id);
  const visibleInView = (view, id) => view.nodes.some(node => node.id === id) || (view.groups || []).some(group => group.ownerId === id);
  function expandAncestors(id) {
    for (let node = state.session.nodes.find(n => n.id === id); node; node = state.session.nodes.find(n => n.id === node.parent)) if (childrenOf(node.id).length) state.expanded.add(node.id);
  }
  function showNode(id) {
    if (!visibleInView(currentView(), id)) {
      const node = state.session.nodes.find(n => n.id === id);
      const view = state.session.views.find(v => v.ownerId === node.parent && visibleInView(v, id)) || state.session.views.find(v => visibleInView(v, id));
      if (view) { setView(view.id, id); return; }
    }
    selectNode(id);
  }
  function pathContainsNode(path, node) {
    return [...path.modules].some(id => M.isWithin(state.session, id, node.id) || (node.observability === 'structure' && M.isWithin(state.session, node.id, id)));
  }
  function notify(message, error = false) {
    clearTimeout(noticeTimer); $('notice').textContent = message; $('notice').classList.toggle('error', error); $('notice').hidden = false;
    noticeTimer = setTimeout(() => { $('notice').hidden = true; }, 6000);
  }
  function wrapText(text, width, font, maxLines = 2) {
    measure.font = font; const lines = []; let current = '';
    for (const char of String(text)) {
      if (measure.measureText(current + char).width > width && current) { lines.push(current); current = char; }
      else current += char;
    }
    if (current) lines.push(current);
    if (lines.length > maxLines) {
      let last = lines[maxLines - 1];
      while (last && measure.measureText(last + '…').width > width) last = last.slice(0, -1);
      return [...lines.slice(0, maxLines - 1), last + '…'];
    }
    return lines;
  }
  function loadSession(session) {
    M.validateSession(session); stopPlayback();
    state.session = session;
    state.view = session.views.some(v => v.id === session.meta.defaultView) ? session.meta.defaultView : session.views[0].id;
    state.selected = currentView().nodes.some(n => n.id === 'cluster') ? 'cluster' : currentView().nodes[0].id;
    state.entity = null; state.event = null;
    state.expanded = new Set(['dsa', 'contexts']);
    state.time = Number.isSafeInteger(session.meta.defaultTime) ? Math.max(session.meta.start, Math.min(session.meta.end, session.meta.defaultTime)) : session.meta.start;
    $('run-name').textContent = session.meta.name;
    $('config-summary').textContent = session.meta.configSummary || '';
    $('time-domain').textContent = session.meta.clockDomain + ' · ' + session.meta.timeUnit;
    $('provenance').textContent = observed() ? '导入记录' : '示意事件'; $('provenance').classList.toggle('recorded', observed());
    $('session-status').textContent = observed() ? 'IMPORTED SESSION · ' + session.events.length + ' 个事件 · 证据类型随事件展示' : 'DESIGN DEMO · 示例时序，不是测量结果';
    $('sidebar-evidence').textContent = observed() ? '已导入记录；完整性由采集范围决定' : '回放时序为设计示意';
    $('time-slider').min = session.meta.start; $('time-slider').max = session.meta.end;
    $('end-time').textContent = '/ ' + timeLabel(session.meta.end);
    const options = [html('option', '', '全部对象')]; options[0].value = '';
    for (const entity of session.entities) { const option = html('option', '', entity.label); option.value = entity.id; options.push(option); }
    $('entity-select').replaceChildren(...options);
    fitCamera(); render();
  }
  function setView(id, selected) {
    state.view = id; state.event = null;
    const view = currentView();
    state.selected = selected || view.ownerId || (view.nodes.some(n => n.id === 'cluster') ? 'cluster' : view.nodes[0].id);
    expandAncestors(state.selected);
    fitCamera(); render();
  }
  function setTime(time) {
    state.time = Math.max(state.session.meta.start, Math.min(state.session.meta.end, Math.round(time)));
    if (state.event && !(state.event.start <= state.time && state.time < state.event.end)) state.event = null;
    renderGraph(); renderInspector(); renderTimeline(); renderTime();
  }
  function renderTime() {
    $('time-slider').value = state.time;
    $('current-time').textContent = timeLabel(state.time);
    $('inspector-time').textContent = timeLabel(state.time);
    $('prev-cycle').disabled = state.time <= state.session.meta.start;
    $('next-cycle').disabled = state.time >= state.session.meta.end;
    $('next-event').disabled = !state.session.events.some(e => e.start > state.time && (!state.entity || e.entityId === state.entity));
    const count = state.session.events.filter(e => !state.entity || e.entityId === state.entity).length;
    $('event-count').textContent = count + ' 个事件' + (state.entity ? ' / 当前对象' : '');
    $('selection-summary').textContent = (state.entity ? entityLabel(state.entity) + ' · ' : '全部对象 · ') + (observed() ? '无事件不等同于空闲' : '示意片段，省略大量运算');
  }
  function renderNav() {
    const focusedView = document.activeElement.dataset?.viewId, focusedModule = document.activeElement.dataset?.moduleId;
    let rootView = currentView();
    while (rootView.parentView) rootView = state.session.views.find(v => v.id === rootView.parentView);
    $('view-nav').replaceChildren(...state.session.views.filter(view => !view.parentView).map((view, i) => {
      const button = html('button', 'view-button' + (rootView.id === view.id ? ' active' : ''));
      button.dataset.viewId = view.id;
      button.type = 'button'; button.setAttribute('aria-pressed', String(rootView.id === view.id));
      button.append(html('span', 'view-number', String(i + 1).padStart(2, '0')), html('span', '', view.label));
      button.addEventListener('click', () => setView(view.id)); return button;
    }));
    const treeRows = [];
    function appendTree(node, depth) {
      const row = html('div', 'tree-row'); row.style.paddingLeft = depth * 12 + 'px';
      const children = childrenOf(node.id);
      const toggle = html('button', 'tree-toggle', children.length ? (state.expanded.has(node.id) ? '▾' : '▸') : '·');
      toggle.type = 'button'; toggle.disabled = !children.length;
      toggle.setAttribute('aria-label', (state.expanded.has(node.id) ? '收起 ' : '展开 ') + node.label);
      toggle.setAttribute('aria-expanded', String(state.expanded.has(node.id)));
      toggle.addEventListener('click', () => { if (state.expanded.has(node.id)) state.expanded.delete(node.id); else state.expanded.add(node.id); renderNav(); });
      const button = html('button', 'module-button' + (state.selected === node.id ? ' active' : ''));
      button.dataset.moduleId = node.id;
      button.type = 'button'; button.setAttribute('aria-pressed', String(state.selected === node.id));
      button.append(html('span', 'tree-dot'), html('span', '', node.label));
      button.addEventListener('click', () => showNode(node.id));
      button.addEventListener('dblclick', () => { if (node.drillView) setView(node.drillView); });
      row.append(toggle, button); treeRows.push(row);
      if (state.expanded.has(node.id)) children.forEach(child => appendTree(child, depth + 1));
    }
    state.session.nodes.filter(node => !node.parent).forEach(node => appendTree(node, 0));
    $('module-tree').replaceChildren(...treeRows);
    if (focusedView) [...$('view-nav').children].find(el => el.dataset.viewId === focusedView)?.focus({ preventScroll: true });
    if (focusedModule) [...$('module-tree').querySelectorAll('.module-button')].find(el => el.dataset.moduleId === focusedModule)?.focus({ preventScroll: true });
    const ancestors = []; let view = currentView();
    while (view) { ancestors.unshift(view); view = state.session.views.find(v => v.id === view.parentView); }
    $('view-breadcrumb').replaceChildren(...ancestors.map((ancestor, index) => {
      const button = html('button', 'breadcrumb-button', (index ? '› ' : '') + ancestor.label.replace(/ · .*/, ''));
      button.type = 'button'; button.disabled = ancestor.id === state.view;
      button.addEventListener('click', () => setView(ancestor.id)); return button;
    }));
  }
  function selectNode(id) {
    state.selected = id; state.event = null; expandAncestors(id); renderNav();
    // Preserve SVG targets between clicks so double-click/keyboard focus survive.
    for (const element of graph.querySelectorAll('[data-node-id]')) {
      const selected = element.dataset.nodeId === id;
      element.classList.toggle('selected', selected); element.setAttribute('aria-pressed', String(selected));
    }
    renderInspector(); renderTimeline();
  }
  function fitCamera(force = false) {
    const view = currentView(), rect = $('canvas-wrap').getBoundingClientRect();
    const fit = Math.min(rect.width / view.width, rect.height / view.height) * .96;
    camera.scale = force ? fit : Math.max(.7, fit); camera.fit = true;
    const selected = view.nodes.find(n => n.id === state.selected);
    const centerX = rect.width < view.width * .55 && !force && selected ? selected.x + selected.w / 2 : view.width / 2;
    camera.x = centerX - rect.width / camera.scale / 2; camera.y = view.height / 2 - rect.height / camera.scale / 2;
    updateCamera();
  }
  function updateCamera() {
    const rect = $('canvas-wrap').getBoundingClientRect();
    graph.setAttribute('viewBox', [camera.x, camera.y, rect.width / camera.scale, rect.height / camera.scale].join(' '));
    $('zoom-label').textContent = Math.round(camera.scale * 100) + '%';
    $('graph-message').textContent = rect.width < 600 ? '拖动空白处平移 · 适应可查看全图' : '拖动空白处平移 · 点击模块查看 · 双击分组展开';
  }
  function zoom(factor) {
    const rect = $('canvas-wrap').getBoundingClientRect();
    const centerX = camera.x + rect.width / camera.scale / 2, centerY = camera.y + rect.height / camera.scale / 2;
    camera.scale = Math.max(.25, Math.min(2.5, camera.scale * factor)); camera.fit = false;
    camera.x = centerX - rect.width / camera.scale / 2; camera.y = centerY - rect.height / camera.scale / 2; updateCamera();
  }
  function defaultRoute(a, b) {
    const dx = b.x + b.w / 2 - a.x - a.w / 2, dy = b.y + b.h / 2 - a.y - a.h / 2;
    if (Math.abs(dx) > Math.abs(dy)) {
      const x1 = dx > 0 ? a.x + a.w : a.x, x2 = dx > 0 ? b.x : b.x + b.w, y1 = a.y + a.h / 2, y2 = b.y + b.h / 2, mid = (x1 + x2) / 2;
      return [[x1, y1], [mid, y1], [mid, y2], [x2, y2]];
    }
    const y1 = dy > 0 ? a.y + a.h : a.y, y2 = dy > 0 ? b.y : b.y + b.h, x1 = a.x + a.w / 2, x2 = b.x + b.w / 2, mid = (y1 + y2) / 2;
    return [[x1, y1], [x1, mid], [x2, mid], [x2, y2]];
  }
  function renderGraph() {
    const focusedNode = document.activeElement.getAttribute('data-node-id');
    const view = currentView(); $('diagram-title').textContent = view.label; $('breadcrumb').textContent = view.breadcrumb || view.label;
    $('detail-scope-note').textContent = view.ownerId ? '细分单元显示结构；未采集的状态不推断' : '';
    const boxes = new Map(view.nodes.map(n => [n.id, n]));
    const path = M.trackedPath(state.session, state.entity);
    for (const id of path.edges) { const edge = state.session.edges.find(e => e.id === id); path.modules.add(edge.from); path.modules.add(edge.to); }
    const active = state.session.events.filter(e => e.start <= state.time && state.time < e.end && (!state.entity || e.entityId === state.entity));
    const liveEdges = new Set(active.filter(e => e.kind !== 'wait').flatMap(e => e.edgeIds));
    const defs = svg('defs');
    for (const [name, color] of [['base', '#546980'], ['track', '#80aefb'], ['live', '#62d6d1']]) {
      const marker = svg('marker', { id: 'arrow-' + name, viewBox: '0 0 8 8', refX: 7, refY: 4, markerWidth: 7, markerHeight: 7, orient: 'auto', markerUnits: 'userSpaceOnUse' });
      marker.append(svg('path', { d: 'M 0 0 L 8 4 L 0 8 z', fill: color })); defs.append(marker);
    }
    const children = [svg('title', {}, view.label + '，' + state.session.meta.name), svg('desc', {}, state.session.meta.description || '模块层级、数据路径和当前事件状态'), defs];
    for (const group of view.groups || []) {
      const owner = state.session.nodes.find(n => n.id === group.ownerId);
      const events = owner ? M.activeEvents(state.session, owner.id, state.time, state.entity) : [];
      const groupEl = svg('g', { class: 'graph-owner-group' + (owner && state.selected === owner.id ? ' selected' : '') + (events.some(e => e.kind !== 'wait') ? ' active' : '') });
      groupEl.append(svg('rect', { x: group.x, y: group.y, width: group.w, height: group.h, rx: 10, class: 'graph-group-box' }));
      const label = group.label + (events.length ? ' · 父级 ' + events.length + ' 活动事件' : '');
      groupEl.append(svg('text', { x: group.x + 14, y: group.y + 23, class: 'graph-group-label' }, wrapText(label, group.w - 28, '13px system-ui', 1)[0]));
      if (owner) {
        groupEl.setAttribute('data-node-id', owner.id); groupEl.setAttribute('tabindex', '0'); groupEl.setAttribute('role', 'button'); groupEl.setAttribute('aria-label', owner.label + '，选择资源池 / 上下文分组');
        groupEl.append(svg('title', {}, label));
        groupEl.addEventListener('click', () => { if (!graphDragged) selectNode(owner.id); });
        groupEl.addEventListener('dblclick', () => { if (owner.drillView) setView(owner.drillView); });
        groupEl.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selectNode(owner.id); } else if (e.key === 'ArrowRight' && owner.drillView) { e.preventDefault(); setView(owner.drillView); } });
      }
      children.push(groupEl);
    }
    for (const id of view.edges) {
      const edge = state.session.edges.find(e => e.id === id), points = view.routes?.[id] || defaultRoute(boxes.get(edge.from), boxes.get(edge.to));
      const tracked = state.entity && path.edges.has(id), live = liveEdges.has(id), relatedStructure = edge.scope === 'structure' && pathContainsNode(path, state.session.nodes.find(n => n.id === edge.from)) && pathContainsNode(path, state.session.nodes.find(n => n.id === edge.to)), dim = state.entity && !tracked && !relatedStructure;
      const group = svg('g', { 'data-edge-id': id });
      group.append(svg('title', {}, edge.label || id));
      group.append(svg('path', { d: points.map((p, i) => (i ? 'L ' : 'M ') + p.join(' ')).join(' '), class: 'graph-edge ' + edge.kind + (tracked ? ' tracked' : '') + (live ? ' live' : '') + (dim ? ' dim' : ''), 'marker-end': 'url(#arrow-' + (live ? 'live' : tracked ? 'track' : 'base') + ')' }));
      if (!dim) {
        const segment = points.slice(1).map((p, i) => ({ a: points[i], b: p, width: Math.abs(p[0] - points[i][0]) })).filter(s => s.a[1] === s.b[1] && s.width >= 95).sort((a, b) => b.width - a.width)[0];
        const nearGroupTitle = segment && (view.groups || []).some(g => Math.abs(g.y + 23 - (segment.a[1] - 8)) < 17 && (segment.a[0] + segment.b[0]) / 2 > g.x && (segment.a[0] + segment.b[0]) / 2 < g.x + g.w);
        if (segment && !nearGroupTitle) group.append(svg('text', { x: (segment.a[0] + segment.b[0]) / 2, y: segment.a[1] - 8, 'text-anchor': 'middle', class: 'edge-label' }, edge.label || ''));
      }
      if (live) { const p = points[Math.floor(points.length / 2)]; group.append(svg('circle', { cx: p[0], cy: p[1], r: 3, class: 'edge-dot' })); }
      children.push(group);
    }
    for (const box of view.nodes) {
      const node = state.session.nodes.find(n => n.id === box.id), events = M.activeEvents(state.session, node.id, state.time, state.entity);
      const busy = events.some(e => e.kind !== 'wait'), waiting = !busy && events.length > 0;
      const tracked = !state.entity || pathContainsNode(path, node);
      const structureOnly = node.observability === 'structure';
      const group = svg('g', { class: 'graph-node' + (state.selected === node.id ? ' selected' : '') + (busy ? ' active' : '') + (waiting ? ' waiting' : '') + (!tracked ? ' dim' : ''), role: 'button', tabindex: 0, 'aria-label': node.label + '，' + (busy ? '有活动事件' : waiting ? '等待' : '未记录当前事件') + (node.drillView ? '，可展开' : ''), 'aria-pressed': state.selected === node.id, 'data-node-id': node.id });
      group.append(svg('title', {}, node.label + '\n' + node.description));
      group.append(svg('rect', { x: box.x, y: box.y, width: box.w, height: box.h, rx: 8, class: 'node-box' }));
      group.append(svg('circle', { cx: box.x, cy: box.y + box.h / 2, r: 3, class: 'node-port' }), svg('circle', { cx: box.x + box.w, cy: box.y + box.h / 2, r: 3, class: 'node-port' }));
      const lines = wrapText(node.label, box.w - (node.drillView ? 48 : 28), '550 16px system-ui', box.h < 80 ? 1 : 2);
      lines.forEach((line, i) => group.append(svg('text', { x: box.x + 14, y: box.y + 27 + i * 20, class: 'node-name' }, line)));
      if ((box.h >= 95 || (!structureOnly && box.h >= 80)) && lines.length === 1) {
        const subtitle = wrapText(node.subtitle || node.type, box.w - 28, '12px system-ui', 1)[0] || '';
        group.append(svg('text', { x: box.x + 14, y: box.y + 49 + (lines.length - 1) * 18, class: 'node-subtitle' }, subtitle));
      }
      if (events.length || structureOnly) group.append(svg('text', { x: box.x + 14, y: box.y + box.h - 15, class: structureOnly ? 'node-structure-status' : 'node-status' }, structureOnly ? (box.w < 160 ? '结构' : '仅结构 · 状态未采集') : waiting ? '等待 · ' + events.length + ' 事件' : '活动 · ' + events.filter(e => e.kind !== 'wait').length + ' 事件'));
      if (node.drillView) group.append(svg('text', { x: box.x + box.w - 24, y: box.y + 27, class: 'node-open' }, '↗'));
      group.addEventListener('click', () => selectNode(node.id));
      group.addEventListener('dblclick', () => { if (node.drillView) setView(node.drillView); });
      group.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selectNode(node.id); } if (e.key === 'ArrowRight' && node.drillView) { e.preventDefault(); setView(node.drillView); } });
      children.push(group);
    }
    graph.replaceChildren(...children); updateCamera();
    if (focusedNode) [...graph.querySelectorAll('[data-node-id]')].find(el => el.dataset.nodeId === focusedNode)?.focus({ preventScroll: true });
  }
  function renderInspector() {
    const node = currentNode();
    $('inspector-title').textContent = node.label; $('node-type').textContent = node.type;
    $('node-description').textContent = node.description || '';
    $('node-source').textContent = node.source || '未提供源码位置';
    const childNodes = childrenOf(node.id);
    $('node-children-section').hidden = !childNodes.length; $('node-child-count').textContent = childNodes.length + ' 个';
    $('node-children').replaceChildren(...childNodes.map(child => {
      const button = html('button', 'child-unit-button'); button.type = 'button'; button.dataset.childId = child.id;
      button.append(html('span', '', child.label), html('small', '', child.type));
      button.addEventListener('click', () => showNode(child.id)); return button;
    }));
    $('node-evidence').textContent = node.evidence || (observed() ? '当前状态来自导入的事件区间。未记录事件不等同于硬件空闲；聚合、推导或采样事件应保留证据类型。' : '结构参考 gem5 模型；此处时刻、事件与关联对象均为设计示意，不用于性能判断。');
    $('node-params').replaceChildren(...Object.entries(node.params || {}).map(([key, value]) => { const row = html('div'); row.append(html('dt', '', key), html('dd', '', String(value))); return row; }));
    const events = M.activeEvents(state.session, node.id, state.time, state.entity), compute = events.filter(e => e.kind === 'compute'), waiting = events.filter(e => e.kind === 'wait');
    const status = $('node-state'); status.className = 'state-line ' + (events.some(e => e.kind !== 'wait') ? 'busy' : waiting.length ? 'wait' : 'empty');
    status.textContent = !events.length ? (node.observability === 'structure' ? '结构单元 · 状态尚未采集' : '未记录当前时刻的事件') : waiting.length === events.length ? '等待 · ' + waiting[0].label : compute.length ? compute.length + ' 个在途算术操作' : '有 ' + events.filter(e => e.kind !== 'wait').length + ' 个活动事件';
    const lines = [], scope = state.entity ? entityLabel(state.entity) : '全部对象';
    lines.push(html('div', 'activity-line', '观测范围：' + scope));
    if (node.observability === 'structure') {
      let ancestor = state.session.nodes.find(n => n.id === node.parent);
      while (ancestor?.observability === 'structure') ancestor = state.session.nodes.find(n => n.id === ancestor.parent);
      if (ancestor) {
        const parentEvents = M.activeEvents(state.session, ancestor.id, state.time, state.entity);
        lines.push(html('div', 'activity-line', '父级「' + ancestor.label + '」：' + parentEvents.length + ' 个区间事件；未映射到当前单元。'));
      }
    }
    const issued = state.session.events.filter(e => e.start === state.time && e.kind === 'compute' && M.isWithin(state.session, e.moduleId, node.id) && (!state.entity || e.entityId === state.entity));
    if (['cluster', 'mul', 'add', 'div'].includes(node.id)) {
      const grouped = new Map();
      for (const event of issued) grouped.set(event.moduleId, (grouped.get(event.moduleId) || 0) + (event.lanes || 0));
      lines.push(html('div', 'activity-line', grouped.size ? '本时刻发射：' + [...grouped].map(([id, lanes]) => id + ' ' + lanes + ' lanes').join(' · ') : '本时刻无记录的算术发射'));
      lines.push(html('div', 'activity-line', '在途数量包含流水线中的操作，不是发射槽占用率。'));
    }
    const detail = state.event && M.isWithin(state.session, state.event.moduleId, node.id) ? state.event : events.find(e => e.kind !== 'wait') || events[0];
    if (detail) {
      const box = html('div', 'event-detail'); box.append(html('strong', '', detail.label));
      box.append(html('div', '', entityLabel(detail.entityId)), html('div', 'mono', '[' + detail.start + ', ' + detail.end + ') ' + state.session.meta.timeUnit));
      if (detail.bytes !== undefined) box.append(html('div', 'mono', detail.bytes + ' B · 传输组/片段'));
      if (detail.lanes !== undefined) box.append(html('div', 'mono', '发射宽度 ' + detail.lanes + ' lanes'));
      box.append(html('div', '', ({ illustrative: '示意事件', observed: '直接观测', derived: '由记录推导' })[detail.evidence])); lines.push(box);
    }
    $('node-activity').replaceChildren(...lines);
    $('drill-down').hidden = !node.drillView; $('drill-down').onclick = () => setView(node.drillView);
    $('inspector-time').textContent = timeLabel(state.time);
  }
  function selectEvent(event) {
    stopPlayback(); state.entity = event.entityId || null; $('entity-select').value = state.entity || '';
    if (!visibleInView(currentView(), event.moduleId)) {
      const view = state.session.views.find(v => v.nodes.some(n => n.id === event.moduleId));
      if (view) { state.view = view.id; state.selected = event.moduleId; fitCamera(); }
      else { const ancestor = currentView().nodes.find(n => M.isWithin(state.session, event.moduleId, n.id)); if (ancestor) state.selected = ancestor.id; }
    } else state.selected = event.moduleId;
    state.event = event; state.time = event.start; expandAncestors(state.selected); $('event-tooltip').hidden = true; render();
  }
  function renderTimeline() {
    const focusedEvent = document.activeElement.getAttribute('data-event-id');
    const width = Math.max(280, $('timeline-chart').clientWidth), left = width < 500 ? 88 : 116, right = 12, top = 27;
    const chartWidth = width - left - right, span = state.session.meta.end - state.session.meta.start;
    const x = time => left + (time - state.session.meta.start) / span * chartWidth;
    let y = top; const tracks = [];
    for (const track of state.session.tracks) {
      const events = state.session.events.filter(e => track.moduleIds.includes(e.moduleId)).sort((a, b) => a.start - b.start || a.end - b.end);
      const ends = [], placements = [];
      for (const event of events) {
        let row = ends.findIndex(end => end <= event.start); if (row < 0) row = ends.length;
        ends[row] = event.end; placements.push({ event, row });
      }
      const height = Math.max(1, ends.length) * 13 + 10;
      tracks.push({ track, placements, y, height }); y += height + 3;
    }
    const height = y + 5; timeline.setAttribute('viewBox', '0 0 ' + width + ' ' + height); timeline.style.height = height + 'px';
    const children = [svg('title', {}, '事件时间轴 · ' + state.session.meta.clockDomain), svg('desc', {}, '点击区间定位到模块、对象和开始时刻；虚线区间为等待事件。')];
    const rawStep = span / Math.max(2, Math.floor(chartWidth / 95)), magnitude = Math.pow(10, Math.floor(Math.log10(rawStep)));
    const step = [1, 2, 5, 10].map(n => n * magnitude).find(n => n >= rawStep) || magnitude * 10;
    for (let t = Math.ceil(state.session.meta.start / step) * step; t <= state.session.meta.end; t += step) {
      children.push(svg('line', { x1: x(t), x2: x(t), y1: top - 4, y2: height, class: 'timeline-grid' }));
      children.push(svg('text', { x: x(t), y: 14, 'text-anchor': t === state.session.meta.start ? 'start' : t === state.session.meta.end ? 'end' : 'middle', class: 'timeline-axis' }, t.toLocaleString()));
    }
    for (const row of tracks) {
      children.push(svg('text', { x: 0, y: row.y + row.height / 2 + 3, class: 'timeline-label' }, row.track.label));
      children.push(svg('rect', { x: left, y: row.y, width: chartWidth, height: row.height, rx: 2, class: 'timeline-track' }));
    }
    const hit = svg('rect', { x: left, y: top, width: chartWidth, height: height - top, class: 'timeline-hit' });
    const timeAtPointer = e => { const bounds = timeline.getBoundingClientRect(); return state.session.meta.start + ((e.clientX - bounds.left) / bounds.width * width - left) / chartWidth * span; };
    hit.addEventListener('pointerdown', e => { stopPlayback(); timelineScrubbing = true; timeline.setPointerCapture(e.pointerId); setTime(timeAtPointer(e)); });
    timeline.onpointermove = e => { if (timelineScrubbing) setTime(timeAtPointer(e)); };
    timeline.onpointerup = timeline.onpointercancel = () => { timelineScrubbing = false; };
    children.push(hit);
    for (const row of tracks) {
      for (const { event, row: index } of row.placements) {
        const dim = state.entity && event.entityId !== state.entity, selected = state.event?.id === event.id;
        const rect = svg('rect', { x: x(event.start), y: row.y + 5 + index * 13, width: Math.max(1.5, x(event.end) - x(event.start)), height: 10, rx: 2, class: 'timeline-event ' + event.kind + (dim ? ' dim' : '') + (selected ? ' selected' : ''), role: 'button', tabindex: 0, 'aria-label': event.label + '，' + entityLabel(event.entityId) + '，' + event.start + ' 至 ' + event.end + ' ' + state.session.meta.timeUnit, 'data-event-id': event.id });
        rect.append(svg('title', {}, event.label + ' · ' + entityLabel(event.entityId)));
        rect.addEventListener('click', e => { e.stopPropagation(); selectEvent(event); });
        rect.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selectEvent(event); } });
        rect.addEventListener('pointermove', e => {
          if (e.pointerType === 'touch') return;
          const tip = $('event-tooltip'); tip.textContent = event.label + '\n' + entityLabel(event.entityId) + '\n[' + event.start + ', ' + event.end + ') ' + state.session.meta.timeUnit + (event.bytes !== undefined ? ' · ' + event.bytes + ' B' : '') + '\n' + ({ illustrative: '设计示意', observed: '直接观测', derived: '由记录推导' })[event.evidence];
          tip.hidden = false; const bounds = tip.getBoundingClientRect();
          tip.style.left = Math.max(8, Math.min(window.innerWidth - bounds.width - 8, e.clientX + 14)) + 'px'; tip.style.top = Math.max(8, Math.min(window.innerHeight - bounds.height - 8, e.clientY + 14)) + 'px';
        });
        rect.addEventListener('pointerleave', () => { $('event-tooltip').hidden = true; }); children.push(rect);
      }
    }
    children.push(svg('line', { x1: x(state.time), x2: x(state.time), y1: 18, y2: height, class: 'timeline-cursor' }));
    children.push(svg('path', { d: 'M ' + (x(state.time) - 4) + ' 18 L ' + (x(state.time) + 4) + ' 18 L ' + x(state.time) + ' 24 Z', fill: '#62d6d1' }));
    timeline.replaceChildren(...children);
    if (focusedEvent) [...timeline.querySelectorAll('.timeline-event')].find(el => el.dataset.eventId === focusedEvent)?.focus({ preventScroll: true });
  }
  function render() { renderNav(); renderGraph(); renderInspector(); renderTimeline(); renderTime(); }
  function stopPlayback() {
    if (timer) clearInterval(timer); timer = null; state.playing = false;
    $('play').textContent = '播放'; $('play').setAttribute('aria-label', '播放时间回放');
  }
  function play() {
    if (state.playing) { stopPlayback(); return; }
    if (state.time >= state.session.meta.end) setTime(state.session.meta.start);
    state.playing = true; $('play').textContent = '暂停'; $('play').setAttribute('aria-label', '暂停时间回放');
    let time = state.time, last = performance.now();
    timer = setInterval(() => { const now = performance.now(); time += (now - last) / 1000 * 24 * state.speed; last = now; setTime(time); if (time >= state.session.meta.end) stopPlayback(); }, 100);
  }
  function download(data, mime, name) {
    const url = URL.createObjectURL(new Blob([data], { type: mime })), a = html('a'); a.href = url; a.download = name; document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(url), 2000);
  }
  function exportSvg() {
    const clone = graph.cloneNode(true), originalElements = [graph, ...graph.querySelectorAll('*')], clones = [clone, ...clone.querySelectorAll('*')];
    const props = ['fill', 'fill-opacity', 'stroke', 'stroke-width', 'stroke-dasharray', 'stroke-opacity', 'opacity', 'font-family', 'font-size', 'font-weight', 'paint-order'];
    originalElements.forEach((element, index) => {
      const style = getComputedStyle(element);
      for (const prop of props) clones[index].style.setProperty(prop, style.getPropertyValue(prop));
      clones[index].removeAttribute('tabindex'); clones[index].removeAttribute('role');
    });
    const view = currentView(); clone.setAttribute('viewBox', '0 0 ' + view.width + ' ' + view.height); clone.setAttribute('width', view.width); clone.setAttribute('height', view.height);
    const background = svg('rect', { x: 0, y: 0, width: view.width, height: view.height, fill: '#0b1018' }); clone.insertBefore(background, clone.firstChild);
    clone.append(svg('text', { x: 14, y: view.height - 10, fill: '#93a3b7', 'font-size': 11, 'font-family': 'system-ui' }, (observed() ? '导入记录' : '设计示意 · 非测量结果') + ' | ' + state.session.meta.name + ' | ' + timeLabel(state.time)));
    download('<?xml version="1.0" encoding="UTF-8"?>\n' + new XMLSerializer().serializeToString(clone), 'image/svg+xml', 'cfd-dsa-' + state.view + '-' + state.time + '.svg');
    notify('已导出当前层级与时刻的完整 SVG，包含数据来源标记。');
  }
  $('load-demo').addEventListener('click', () => { loadSession(window.CFD_DEMO_SESSION); notify('已恢复设计演示。所有回放事件均为示意。'); });
  $('entity-select').addEventListener('change', e => { state.entity = e.target.value || null; state.event = null; renderGraph(); renderInspector(); renderTimeline(); renderTime(); });
  $('time-slider').addEventListener('input', e => { stopPlayback(); setTime(Number(e.target.value)); });
  $('prev-cycle').addEventListener('click', () => { stopPlayback(); setTime(state.time - 1); });
  $('next-cycle').addEventListener('click', () => { stopPlayback(); setTime(state.time + 1); });
  $('next-event').addEventListener('click', () => { stopPlayback(); const next = state.session.events.filter(e => e.start > state.time && (!state.entity || e.entityId === state.entity)).sort((a, b) => a.start - b.start)[0]; if (next) selectEvent(next); });
  $('speed').addEventListener('change', e => { state.speed = Number(e.target.value); });
  $('play').addEventListener('click', play);
  $('zoom-in').addEventListener('click', () => zoom(1.2)); $('zoom-out').addEventListener('click', () => zoom(1 / 1.2)); $('fit-view').addEventListener('click', () => fitCamera(true));
  $('export-svg').addEventListener('click', exportSvg);
  $('import-session').addEventListener('click', () => $('session-file').click());
  $('session-file').addEventListener('change', async e => {
    const file = e.target.files[0]; if (!file) return;
    try { if (file.size > 20 * 1024 * 1024) throw new Error('原型导入文件上限为 20 MiB'); const session = JSON.parse(await file.text()); loadSession(session); notify('已导入 ' + session.meta.name + ' · ' + session.events.length + ' 个事件'); }
    catch (error) { notify('导入失败：' + error.message, true); }
    finally { e.target.value = ''; }
  });
  graph.addEventListener('pointerdown', e => {
    if (e.target.closest('.graph-node')) return;
    graphDragged = false;
    drag = { x: e.clientX, y: e.clientY, cameraX: camera.x, cameraY: camera.y }; camera.fit = false; graph.setPointerCapture(e.pointerId); graph.classList.add('panning');
  });
  graph.addEventListener('pointermove', e => { if (!drag) return; if (Math.hypot(e.clientX - drag.x, e.clientY - drag.y) > 4) graphDragged = true; camera.x = drag.cameraX - (e.clientX - drag.x) / camera.scale; camera.y = drag.cameraY - (e.clientY - drag.y) / camera.scale; updateCamera(); });
  const stopPan = () => { drag = null; graph.classList.remove('panning'); }; graph.addEventListener('pointerup', stopPan); graph.addEventListener('pointercancel', stopPan);
  graph.addEventListener('wheel', e => { if (e.ctrlKey || e.metaKey) { e.preventDefault(); zoom(e.deltaY < 0 ? 1.07 : 1 / 1.07); } }, { passive: false });
  new ResizeObserver(() => { if (!state.session) return; if (camera.fit) fitCamera(); else updateCamera(); renderTimeline(); }).observe($('canvas-wrap'));
  new ResizeObserver(() => { if (state.session) renderTimeline(); }).observe($('timeline-chart'));
  window.addEventListener('pagehide', stopPlayback);
  loadSession(window.CFD_DEMO_SESSION);
  // Diagnostic snapshots for future adapters; no mutation API.
  window.CFDVisualizer = { getState: () => ({ view: state.view, selected: state.selected, entity: state.entity, time: state.time, playing: state.playing, provenance: state.session.meta.provenance }), getSession: () => structuredClone(state.session) };
})();
