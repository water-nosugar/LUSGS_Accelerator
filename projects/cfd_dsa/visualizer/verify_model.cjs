/* Verify evidence boundaries and event semantics without a browser. */
'use strict';
const assert = require('node:assert/strict');
const model = require('./model.js');
const demo = require('./demo-session.js');
const clone = () => JSON.parse(JSON.stringify(demo));
let checks = 0;
function check(label, action) { action(); checks++; console.log('PASS ' + label); }
check('demo schema and provenance', () => {
  model.validateSession(demo);
  assert(demo.events.every(e => e.evidence === 'illustrative'));
  assert.equal(demo.meta.provenance, 'demo');
});
check('completion endpoint uses half-open intervals', () => {
  const event = demo.events.find(e => e.moduleId === 'div');
  assert(model.activeEvents(demo, 'div', event.start).some(e => e.id === event.id));
  assert(!model.activeEvents(demo, 'div', event.end).some(e => e.id === event.id));
});
check('shared hardware queries include child resource pools', () => {
  assert(model.activeEvents(demo, 'cluster', 112).some(e => e.moduleId === 'div'));
  assert.equal(model.activeEvents(demo, 'cluster', 112, 'line0:g0:cell0').length, 0);
  assert.equal(model.activeEvents(demo, 'cluster', 112, 'line0:g0:cell1').length, 1);
});
check('cell tracking retains its data path', () => {
  const path = model.trackedPath(demo, 'line0:g0:cell0');
  assert(path.edges.has('lu-cluster'));
  assert(path.modules.has('div'));
});
check('recorded sessions reject illustrative events', () => {
  const session = clone(); session.meta.provenance = 'recorded';
  assert.throws(() => model.validateSession(session), /示意事件/);
});
check('unknown request ownership remains null', () => {
  const session = clone(); session.meta.provenance = 'recorded';
  for (const event of session.events) { event.evidence = 'observed'; event.entityId = null; }
  model.validateSession(session);
  assert.equal(model.activeEvents(session, 'cluster', 112, session.entities[0].id).length, 0);
});
check('invalid references are rejected before rendering', () => {
  const session = clone(); session.events[0].moduleId = 'missing';
  assert.throws(() => model.validateSession(session), /事件模块/);
});
check('cycles in hardware hierarchy are rejected', () => {
  const session = clone(); session.nodes.find(n => n.id === 'dsa').parent = 'cluster';
  assert.throws(() => model.validateSession(session), /循环/);
});
check('dangling view routes are rejected', () => {
  const session = clone(); session.views[1].routes['missing-edge'] = [[0, 0], [10, 10]];
  assert.throws(() => model.validateSession(session), /路由无效/);
});
check('unsafe time integers are rejected', () => {
  const session = clone(); session.meta.end = Number.MAX_SAFE_INTEGER + 1;
  assert.throws(() => model.validateSession(session), /安全整数/);
});
check('every accelerator block has an internal view', () => {
  for (const id of ['cpu', 'memory', 'controller', 'dma', 'ingress', 'lu', 'rhs', 'cluster', 'spm', 'tile', 'feedback', 'output', 'completion']) {
    const node = demo.nodes.find(n => n.id === id);
    const view = demo.views.find(v => v.id === node.drillView);
    assert(view && view.ownerId === id, id + ' lacks an owned internal view');
  }
});
check('the shared cluster contains exactly 5 mul, 5 add and 1 div units', () => {
  assert.equal(demo.nodes.filter(n => n.parent === 'mul' && n.id.startsWith('mul-lane')).length, 5);
  assert.equal(demo.nodes.filter(n => n.parent === 'add' && n.id.startsWith('add-lane')).length, 5);
  assert.equal(demo.nodes.filter(n => n.parent === 'div' && n.id === 'div-unit').length, 1);
});
check('pool events never become invented lane events', () => {
  assert(model.activeEvents(demo, 'mul', 77).length > 0);
  for (let lane = 0; lane < 5; lane++) assert.equal(model.activeEvents(demo, 'mul-lane' + lane, 77).length, 0);
  const session = clone(); session.events[0].moduleId = 'mul-lane0';
  assert.throws(() => model.validateSession(session), /仅结构单元/);
});
check('each SPM bank has one read and one write unit', () => {
  for (let bank = 0; bank < 4; bank++) {
    assert(demo.nodes.some(n => n.id === 'bank' + bank + '-read' && n.parent === 'bank' + bank));
    assert(demo.nodes.some(n => n.id === 'bank' + bank + '-write' && n.parent === 'bank' + bank));
  }
});
check('LU/RHS contexts and output/completion slots are finite', () => {
  for (const parent of ['lu', 'rhs']) assert.equal(demo.nodes.filter(n => n.parent === parent && n.id.startsWith(parent + '-context')).length, 2);
  for (let ctx = 0; ctx < 2; ctx++) assert.equal(demo.nodes.filter(n => n.parent === 'rhs-context' + ctx).length, 15);
  for (const parent of ['output', 'completion']) assert.equal(demo.nodes.filter(n => n.parent === parent && n.id.startsWith(parent + '-slot')).length, 8);
});
check('navigation view cycles are rejected', () => {
  const session = clone(); session.views.find(v => v.id === 'cluster-detail').parentView = 'mul-detail';
  assert.throws(() => model.validateSession(session), /视图层级存在循环/);
});
console.log('MODEL_CHECK_PASS: ' + checks + ' checks');
