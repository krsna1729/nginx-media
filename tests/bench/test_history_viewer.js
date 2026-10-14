#!/usr/bin/env node
// Exercise the deployed viewer's data selection and rendered inspector/scorecards.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, 'history/index.html'), 'utf8');
const script = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)].at(-1)[1];
const elements = new Map();
function element() {
  return {style: {}, children: [], innerHTML: '', textContent: '', value: '',
    classList: {add() {}, remove() {}}, setAttribute() {}, scrollIntoView() {},
    appendChild(child) { this.children.push(child); }, querySelector() { return element(); }};
}
const sandbox = {
  document: {getElementById(id) {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  }, createElement: element},
  // Startup stays pending; fixtures are supplied directly, never fetched.
  fetch: () => new Promise(() => {}), setTimeout: callback => callback(),
  capturedSparks: [],
};
vm.createContext(sandbox);
vm.runInContext(script, sandbox);
const evaluate = code => vm.runInContext(code, sandbox);
const plain = value => JSON.parse(JSON.stringify(value));
const host = {cpu_model: 'same CPU', kernel: '7.2', nproc_online: 4,
  cgroup: {cpu_max: 'max 100000', cpu_stat: {usage_usec: 1}}};
const recipe = {seconds: 15, rate: '8M', steps: [1, 16, 32],
  stop_after_failures: 2, min_delivery_ratio: 0.95, interval_floor: 0.8,
  max_low_seconds: 2, srt_workers: 'adaptive', hls_push_workers: 'adaptive',
  srt_hls: 'yes', mixes: ['pure-srt'], harness: 'harness-a'};
function record(attempt, fingerprint = host, settings = recipe, rungs = [[16, 10], [32, 100]]) {
  const mix = {fingerprint, highest_passing: 32, rungs: rungs.map(([destinations, cpu]) =>
    ({destinations, outcome: 'pass', sender_cpu_per_gbps: cpu}))};
  return {tier: 'weekly', run: '42', run_attempt: String(attempt), sha: 'same-sha',
    timestamp: `2026-10-0${attempt}T00:00:00Z`, complete: true,
    configs: {srt: {complete: true, recipe: settings, mixes: {'pure-srt': mix}}}};
}
const old = record(1);
const current = record(5, {...host, cgroup: {cpu_max: 'max 100000', cpu_stat: {usage_usec: 999}}}, recipe, [[32, 20]]);
const foreign = record(2, {...host, kernel: 'different-kernel'}, recipe, [[32, 999]]);
const incompatible = record(3, host, {...recipe, seconds: 30}, [[32, 888]]);
const probe = {...record(4, host, recipe, [[32, 777]]), comparison_group: 'probe-attempt-4'};
const mixed = record(6);
mixed.configs.foreign = foreign.configs.srt;
old.configs.fixed = record(1, host, {...recipe, srt_workers: '1'}, [[32, 40]]).configs.srt;
current.configs.fixed = record(5, host, {...recipe, srt_workers: '1'}, [[32, 30]]).configs.srt;
sandbox.records = [old, foreign, incompatible, probe, current];
sandbox.mixed = mixed;
sandbox.host = host;
sandbox.recipe = recipe;

const all = plain(evaluate(`series(records, (c, mix, entry) => entry.rungs.map(r =>
  [c + '@' + r.destinations, r.sender_cpu_per_gbps]))`));
const matched = all.find(s => s.key === 'srt@32' && s.points.some(p => p.y === 20));
assert.deepEqual(matched.points.map(p => p.y), [100, 20]);
assert.equal(new Set(matched.points.map(p => p.x)).size, 2, 'same SHA attempts must have distinct points');
assert.equal(all.filter(s => s.key === 'srt@32').length, 4, 'runner, recipe and profile must remain separate');
assert.equal(evaluate('recipesComparable(null, null)'), false, 'missing recipes are not comparison evidence');
assert.equal(evaluate('runnerIdentity({status: "ok"})'), '', 'unknown host must not form a comparison series');
assert.equal(evaluate('profileRecords(records, "").length'), 4);
assert.equal(evaluate('profileRecords(records, "probe-attempt-4")[0].configs.srt.mixes["pure-srt"].rungs[0].sender_cpu_per_gbps'), 777);
const projected = plain(evaluate('hardwareRecords([mixed], runnerIdentity(host))'));
assert.deepEqual(Object.keys(projected[0].configs), ['srt'], 'mixed records must filter their contents, not just their container');
assert.equal(evaluate('recordKey(hardwareRecords([mixed], runnerIdentity(host))[0]) === recordKey(mixed)'), true);

evaluate('drawSparkline = (canvas, values) => capturedSparks.push(values); render(records)');
assert.deepEqual(plain(sandbox.capturedSparks), [[100, 20], [40, 30]],
  'fixed-rung sparklines must retain distinct configurations of the same mix');
assert.match(elements.get('scorecard-container').innerHTML, /id="spark-0"/);
assert.match(elements.get('scorecard-container').innerHTML, /id="spark-1"/);
assert.equal(elements.get('diagnostic-workload').innerHTML.includes('fixed/pure-srt'), true);
const currentKey = evaluate('recordKey(records[4])');
sandbox.currentKey = currentKey;
evaluate('inspectCommit(currentKey)');
assert.match(elements.get('inspect-meta').textContent, /Attempt: 5/);
assert.match(elements.get('inspect-table').innerHTML, /20\.0%/);
assert.doesNotMatch(elements.get('inspect-table').innerHTML, /100\.0%/);
const profileControl = elements.get('profiles');
profileControl.value = 'probe-attempt-4';
profileControl.onchange();
assert.match(elements.get('run-matrix').innerHTML, /777\.0/);
assert.doesNotMatch(elements.get('run-matrix').innerHTML, /888\.0/);
assert.match(elements.get('diagnostic-context').textContent, /same-sha/);
console.log('test_history_viewer.js: stable series, fixed-rung sparkline, exact attempts and mixed/profile filtering passed');
