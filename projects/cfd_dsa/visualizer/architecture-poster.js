(function () {
  'use strict';
  const launch = document.getElementById('open-architecture-poster');
  if (!launch || !window.CFDPosterLayout || !window.CFDVisualizer) return;
  const dialog = document.createElement('dialog');
  dialog.className = 'architecture-poster';
  dialog.setAttribute('aria-labelledby', 'poster-title');
  dialog.innerHTML = `
    <header class="poster-header"><div><div class="poster-kicker">MICROARCHITECTURE / BLOCK DIAGRAM</div><h2 id="poster-title">架构总览</h2></div>
      <div class="poster-header-actions"><button type="button" data-action="export">导出此图 SVG</button><button type="button" data-action="close" aria-label="关闭架构总览">关闭 ×</button></div></header>
    <nav class="poster-nav" aria-label="独立架构图分区"><button type="button" data-owner="dsa">CFD DSA 总图</button><button type="button" data-owner="cpu">CPU 流水</button><button type="button" data-owner="memory">存储系统</button><button type="button" data-action="fit">适应宽度</button><button type="button" data-action="actual">100%</button><span class="poster-guide">点击分区展开 · 点击单元查看参数 · Esc 关闭</span></nav>
    <div class="poster-content"><section class="poster-board" aria-label="架构图画布"><nav class="poster-breadcrumb" aria-label="架构图路径"></nav><div class="poster-scroll"><div class="poster-drawing"></div></div></section><aside class="poster-info" aria-label="架构图模块说明"></aside></div>
    <footer class="poster-footer"><div class="poster-key"><span><i style="background:#301741;border-color:#a768c7"></i>计算资源</span><span><i style="background:#293519;border-color:#a4b25c"></i>存储 / 搬运</span><span><i style="background:#17333e;border-color:#60a1b7"></i>状态 / 队列</span><span><i style="background:#421e23;border-color:#c16c75"></i>Cache</span></div><span>结构示意 · 功能关系 · 无逐单元活动回放</span></footer>`;
  document.body.append(dialog);
  const drawing = dialog.querySelector('.poster-drawing');
  const info = dialog.querySelector('.poster-info');
  const breadcrumb = dialog.querySelector('.poster-breadcrumb');
  let session, map, history = [], poster, fit = true;
  function element(tag, text, className) {
    const e = document.createElement(tag); if (text !== undefined) e.textContent = text;
    if (className) e.className = className; return e;
  }
  function inspect(id) {
    const n = map.get(id); if (!n) return;
    for (const box of drawing.querySelectorAll('[data-poster-node]')) box.classList.toggle('poster-selected', box.dataset.posterNode === id);
    info.replaceChildren(element('div', n.type, 'poster-kicker'), element('h3', n.label), element('p', n.description));
    const dl = element('dl');
    for (const [key, value] of Object.entries(n.params || {})) dl.append(element('dt', key), element('dd', value));
    info.append(element('div', '结构参数', 'poster-info-label'), dl);
    const children = session.nodes.filter(child => child.parent === id);
    if (children.length) {
      const links = element('div', undefined, 'poster-children');
      for (const child of children) {
        const button = element('button', child.label + (child.drillView ? ' ↗' : ''));
        button.type = 'button'; button.dataset.inspect = child.id; links.append(button);
      }
      info.append(element('div', '内部单元 · ' + children.length, 'poster-info-label'), links);
    }
    if (n.drillView && poster.ownerId !== id) {
      const button = element('button', '展开此分区 ↗'); button.type = 'button'; button.dataset.owner = id;
      info.append(element('div', '', 'poster-info-label'), button);
    }
    info.append(element('div', '来源', 'poster-info-label'), element('code', n.source || '当前导入会话'), element('p', n.evidence || '显示当前会话的结构定义；连线表达模块间的功能关系。'));
    info.scrollTop = 0;
  }
  function sizing() { const svg = drawing.querySelector('svg'); if (svg) svg.style.width = fit ? '100%' : poster.width + 'px'; }
  function render(id, reset = false) {
    if (!map.has(id)) return;
    const n = map.get(id);
    if (!reset && poster && !n.drillView && !['dsa', 'cpu', 'memory'].includes(id)) { inspect(id); return; }
    if (reset) history = [id]; else if (history[history.length - 1] !== id) history.push(id);
    poster = window.CFDPosterLayout.create(session, id);
    drawing.innerHTML = poster.svg; sizing();
    dialog.querySelector('.poster-scroll').scrollTo(0, 0);
    breadcrumb.replaceChildren();
    history.forEach((key, i) => {
      if (i) breadcrumb.append(element('span', '›'));
      const button = element('button', map.get(key).label); button.type = 'button'; button.dataset.history = String(i);
      if (i === history.length - 1) button.setAttribute('aria-current', 'page');
      breadcrumb.append(button);
    });
    for (const button of dialog.querySelectorAll('.poster-nav [data-owner]')) button.setAttribute('aria-pressed', String(button.dataset.owner === history[0]));
    inspect(id);
  }
  function open() {
    session = window.CFDVisualizer.getSession();
    map = new Map(session.nodes.map(n => [n.id, n]));
    for (const button of dialog.querySelectorAll('.poster-nav [data-owner]')) button.disabled = !map.has(button.dataset.owner);
    const first = map.has('dsa') ? 'dsa' : session.nodes.find(n => n.drillView)?.id || session.nodes[0].id;
    fit = true; history = []; poster = null; drawing.replaceChildren(); render(first, true);
    dialog.showModal();
  }
  function exportSvg() {
    const svg = drawing.querySelector('svg').cloneNode(true);
    svg.removeAttribute('style');
    svg.querySelectorAll('[data-poster-node]').forEach(n => { n.removeAttribute('tabindex'); n.removeAttribute('role'); n.removeAttribute('data-poster-node'); });
    const desc = document.createElementNS('http://www.w3.org/2000/svg', 'desc');
    desc.textContent = '结构示意：' + session.meta.name + '；' + (session.meta.configSource || '当前导入会话') + '。功能关系图，无逐单元活动回放。';
    svg.prepend(desc);
    const blob = new Blob([new XMLSerializer().serializeToString(svg)], { type: 'image/svg+xml;charset=utf-8' });
    const url = URL.createObjectURL(blob), a = document.createElement('a');
    a.href = url; a.download = 'cfd-architecture-' + poster.ownerId.replace(/[^a-zA-Z0-9_-]/g, '_') + '.svg';
    document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  launch.addEventListener('click', open);
  dialog.addEventListener('close', () => launch.focus());
  dialog.addEventListener('click', event => {
    const button = event.target.closest('[data-action], [data-owner], [data-history], [data-inspect], [data-poster-node]');
    if (!button) return;
    if (button.dataset.action === 'close') dialog.close();
    else if (button.dataset.action === 'export') exportSvg();
    else if (button.dataset.action === 'fit') { fit = true; sizing(); }
    else if (button.dataset.action === 'actual') { fit = false; sizing(); }
    else if (button.dataset.history !== undefined) { history = history.slice(0, Number(button.dataset.history) + 1); render(history[history.length - 1]); }
    else if (button.dataset.owner) render(button.dataset.owner, !!button.closest('.poster-nav'));
    else if (button.dataset.inspect) { if (map.get(button.dataset.inspect)?.drillView) render(button.dataset.inspect); else inspect(button.dataset.inspect); }
    else if (button.dataset.posterNode) render(button.dataset.posterNode);
  });
  dialog.addEventListener('keydown', event => {
    const node = event.target.closest('[data-poster-node]');
    if (node && (event.key === 'Enter' || event.key === ' ')) { event.preventDefault(); render(node.dataset.posterNode); }
  });
})();
