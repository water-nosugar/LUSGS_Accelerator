/* Pure session validation and queries. Shared by the browser and Node QA. */
(function (root) {
  'use strict';
  const kinds = new Set(['compute', 'transfer', 'wait', 'control']);
  const nodeIndexes = new WeakMap();
  function validateSession(session) {
    const fail = message => { throw new Error(message); };
    if (!session || session.schemaVersion !== 1) fail('会话必须使用 schemaVersion: 1');
    const meta = session.meta;
    if (!meta || typeof meta.name !== 'string' || !meta.name.trim()) fail('缺少会话名称');
    if (!['demo', 'recorded'].includes(meta.provenance)) fail('provenance 必须为 demo 或 recorded');
    if (!['cycle', 'tick'].includes(meta.timeUnit) || typeof meta.clockDomain !== 'string' || !meta.clockDomain.trim()) fail('缺少明确的时间单位或时钟域');
    if (!Number.isSafeInteger(meta.start) || !Number.isSafeInteger(meta.end) || meta.start < 0 || meta.end <= meta.start) fail('时间范围必须是递增的非负安全整数');
    if (!Array.isArray(session.nodes) || !session.nodes.length || session.nodes.length > 500) fail('模块数须为 1–500');
    if (!Array.isArray(session.edges) || session.edges.length > 2000 || !Array.isArray(session.events) || session.events.length > 10000) fail('原型最多支持 2000 条连线、10000 个事件');
    if (!Array.isArray(session.entities) || session.entities.length > 1000 || !Array.isArray(session.views) || !session.views.length || session.views.length > 50) fail('对象或视图格式无效（最多 50 个视图）');
    const unique = (items, name) => {
      const seen = new Set();
      for (const item of items) {
        if (!item || typeof item.id !== 'string' || !item.id || seen.has(item.id)) fail(name + ' ID 缺失或重复');
        seen.add(item.id);
      }
      return seen;
    };
    const nodes = unique(session.nodes, '模块'), edges = unique(session.edges, '连线');
    const entities = unique(session.entities, '对象'), views = unique(session.views, '视图');
    unique(session.events, '事件');
    const nodeMap = new Map(session.nodes.map(node => [node.id, node]));
    for (const node of session.nodes) {
      if (typeof node.label !== 'string' || !node.label || typeof node.type !== 'string') fail('模块名称或类型无效');
      if (node.parent && !nodes.has(node.parent)) fail('模块父级不存在: ' + node.id);
      if (node.drillView && !views.has(node.drillView)) fail('展开视图不存在: ' + node.id);
      if (node.observability !== undefined && !['structure', 'event'].includes(node.observability)) fail('模块观测类型无效: ' + node.id);
      if (node.params && (typeof node.params !== 'object' || Array.isArray(node.params))) fail('模块参数须为键值对象');
      if (node.parent) {
        const chain = new Set([node.id]); let ancestor = node.parent;
        while (ancestor) {
          if (chain.has(ancestor)) fail('模块层级存在循环: ' + node.id);
          chain.add(ancestor); ancestor = nodeMap.get(ancestor).parent;
        }
      }
    }
    for (const edge of session.edges) {
      if (!nodes.has(edge.from) || !nodes.has(edge.to) || !['data', 'control'].includes(edge.kind)) fail('连线端点或类型无效: ' + edge.id);
    }
    for (const entity of session.entities) if (typeof entity.label !== 'string') fail('对象缺少标签');
    for (const view of session.views) {
      if (typeof view.label !== 'string' || !Array.isArray(view.nodes) || !view.nodes.length || !Array.isArray(view.edges)) fail('视图格式无效: ' + view.id);
      const local = unique(view.nodes, '视图模块');
      if (view.ownerId && !nodes.has(view.ownerId)) fail('视图所属模块不存在: ' + view.id);
      if (view.parentView && !views.has(view.parentView)) fail('上级视图不存在: ' + view.id);
      const visited = new Set([view.id]); let parentView = view.parentView;
      while (parentView) {
        if (visited.has(parentView)) fail('视图层级存在循环: ' + view.id);
        visited.add(parentView); parentView = session.views.find(v => v.id === parentView).parentView;
      }
      if (!Number.isFinite(view.width) || !Number.isFinite(view.height) || view.width <= 0 || view.height <= 0) fail('视图尺寸无效');
      for (const n of view.nodes) {
        if (!nodes.has(n.id) || !['x', 'y', 'w', 'h'].every(k => Number.isFinite(n[k])) || n.w < 80 || n.h < 40 || n.x < 0 || n.y < 0 || n.x + n.w > view.width || n.y + n.h > view.height) fail('视图模块位置无效: ' + n.id);
      }
      for (const id of view.edges) {
        const edge = session.edges.find(e => e.id === id);
        if (!edge || !local.has(edge.from) || !local.has(edge.to)) fail('视图连线引用无效: ' + id);
      }
      if (view.groups && (!Array.isArray(view.groups) || view.groups.some(g => !['x', 'y', 'w', 'h'].every(k => Number.isFinite(g[k])) || typeof g.label !== 'string'))) fail('视图分组无效');
      if (view.groups?.some(g => g.ownerId && !nodes.has(g.ownerId))) fail('视图分组所属模块不存在');
      if (view.routes) {
        if (typeof view.routes !== 'object' || Array.isArray(view.routes)) fail('连线路由格式无效');
        for (const [id, points] of Object.entries(view.routes)) {
          if (!view.edges.includes(id) || !Array.isArray(points) || points.length < 2 || points.length > 30 || points.some(p => !Array.isArray(p) || p.length !== 2 || !p.every(Number.isFinite) || p[0] < 0 || p[1] < 0 || p[0] > view.width || p[1] > view.height)) fail('连线路由无效: ' + id);
        }
      }
    }
    for (const event of session.events) {
      if (!nodes.has(event.moduleId) || !kinds.has(event.kind) || typeof event.label !== 'string') fail('事件模块或类型无效: ' + event.id);
      if (nodeMap.get(event.moduleId).observability === 'structure') fail('仅结构单元不可直接绑定活动事件: ' + event.moduleId);
      if (!Number.isSafeInteger(event.start) || !Number.isSafeInteger(event.end) || event.start < meta.start || event.end <= event.start || event.end > meta.end) fail('事件时间范围无效: ' + event.id);
      if (event.entityId !== null && !entities.has(event.entityId)) fail('事件对象不存在: ' + event.id);
      if (!Array.isArray(event.edgeIds) || event.edgeIds.some(id => !edges.has(id))) fail('事件路径引用无效: ' + event.id);
      if (event.lanes !== undefined && (!Number.isSafeInteger(event.lanes) || event.lanes <= 0)) fail('事件 lane 数无效');
      if (event.bytes !== undefined && (!Number.isSafeInteger(event.bytes) || event.bytes < 0)) fail('传输字节数无效');
      if (!['illustrative', 'observed', 'derived'].includes(event.evidence)) fail('事件必须声明证据类型');
      if (meta.provenance === 'recorded' && event.evidence === 'illustrative') fail('实测会话中不可混入示意事件');
    }
    if (!Array.isArray(session.tracks) || !session.tracks.length || session.tracks.length > 30) fail('缺少资源时间轴轨道');
    unique(session.tracks, '轨道');
    for (const track of session.tracks) {
      if (typeof track.label !== 'string' || !Array.isArray(track.moduleIds) || !track.moduleIds.length || track.moduleIds.some(id => !nodes.has(id))) fail('时间轴轨道引用无效');
    }
    const covered = new Set(session.tracks.flatMap(t => t.moduleIds));
    if (session.events.some(event => !covered.has(event.moduleId))) fail('存在未分配到时间轴轨道的事件');
    nodeIndexes.set(session, nodeMap);
    return session;
  }
  function isWithin(session, moduleId, ancestorId) {
    let map = nodeIndexes.get(session);
    if (!map) { map = new Map(session.nodes.map(n => [n.id, n])); nodeIndexes.set(session, map); }
    for (let current = moduleId; current; current = map.get(current)?.parent) if (current === ancestorId) return true;
    return false;
  }
  function activeEvents(session, moduleId, time, entityId = null) {
    return session.events.filter(e => e.start <= time && time < e.end && isWithin(session, e.moduleId, moduleId) && (!entityId || e.entityId === entityId));
  }
  function trackedPath(session, entityId) {
    const events = session.events.filter(e => !entityId || e.entityId === entityId);
    return { modules: new Set(events.map(e => e.moduleId)), edges: new Set(events.flatMap(e => e.edgeIds)) };
  }
  const api = { validateSession, activeEvents, isWithin, trackedPath };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.CfdModel = api;
})(typeof window !== 'undefined' ? window : globalThis);
