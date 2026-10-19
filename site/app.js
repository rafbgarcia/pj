// pj benchmark site: loads data/index.json and one results file per machine (pj/suite/run.py output).
'use strict';

const SERIES = [ // fixed order = fixed color slot; color follows the parser, never its rank
  { id: 'pj', label: 'pj (all cores)' },
  { id: 'pj-1t', label: 'pj (1 thread)' },
  { id: 'simdjson', label: 'simdjson DOM' },
  { id: 'simdjson-od', label: 'simdjson On-Demand' },
  { id: 'yyjson', label: 'yyjson' },
];
const color = id => `var(--series-${SERIES.findIndex(s => s.id === id) + 1})`;
const KB = 1024;
const REPO = 'https://github.com/rafbgarcia/pj'; // pj revision in the meta line links here
const MEM_MIN = 256 * KB;

const METRICS = {
  total: { label: 'End-to-end throughput (GB/s)', unit: 'GB/s', higher: true,
    note: 'Parse, visit every value, free. The whole cost of using a document once. Higher is better.',
    get: r => r.bytes / r.total.med, iqr: r => r.total },
  parse: { label: 'Parse throughput (GB/s)', unit: 'GB/s', higher: true, lazy: true,
    note: 'Steady-state parse until the document is ready to query. simdjson On-Demand is lazy, so it is left out here (see end-to-end). Higher is better.',
    get: r => r.bytes / r.parse.med, iqr: r => r.parse },
  cold: { label: 'Cold first parse (GB/s)', unit: 'GB/s', higher: true, lazy: true,
    note: 'The first parse in a fresh process: page faults and first-touch allocation included. Higher is better.',
    get: r => r.bytes / r.cold },
  mem: { label: 'Peak memory (× input size)', unit: '×', higher: false,
    note: 'Peak memory during a cold parse and walk, above the footprint just before it, divided by input size (input buffer excluded). Documents of 256 KB and up. Lower is better.',
    get: r => (r.bytes >= MEM_MIN && r.mem_total > 0 ? r.mem_total / r.bytes : null) },
  cores: { label: 'Cores used (CPU time ÷ wall time)', unit: 'cores', higher: null,
    note: 'CPU time of all threads per end-to-end document, divided by wall time. pj with all cores trades CPU for latency.',
    get: r => r.cpu_total / r.total.med },
  ipb: { label: 'Instructions per byte', unit: 'instr/B', higher: false,
    note: 'End-to-end, single-threaded parsers (Linux: the measuring thread; macOS: the process). Lower is better.',
    get: r => (r.hw_total && r.threads === 1 ? r.hw_total.instructions / r.bytes : null) },
  cpb: { label: 'Cycles per byte', unit: 'cycles/B', higher: false,
    note: 'End-to-end, single-threaded parsers. Lower is better.',
    get: r => (r.hw_total && r.threads === 1 ? r.hw_total.cycles / r.bytes : null) },
  bm: { label: 'Branch misses per KB', unit: 'misses/KB', higher: false,
    note: 'End-to-end, single-threaded parsers, Linux only. Lower is better.',
    get: r => (r.hw_total && r.threads === 1 && r.hw_total.branch_misses != null ? r.hw_total.branch_misses / (r.bytes / KB) : null) },
};
const SUMMARY_SIZES = ['16KB', '4MB', '256MB'];

const $ = id => document.getElementById(id);
const el = (tag, attrs = {}, text) => {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  if (text != null) n.textContent = text;
  return n;
};
const svgEl = (tag, attrs = {}) => {
  const n = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  return n;
};
function fmt(v) {
  if (v == null || !isFinite(v)) return '–';
  const a = Math.abs(v);
  if (a >= 100) return v.toFixed(0);
  if (a >= 10) return v.toFixed(1);
  if (a >= 1) return v.toFixed(2);
  return v.toPrecision(2);
}
function niceMax(v) {
  if (!(v > 0)) return 1;
  const e = Math.pow(10, Math.floor(Math.log10(v))), m = v / e;
  return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 2.5 ? 2.5 : m <= 5 ? 5 : 10) * e;
}

let INDEX = [], CACHE = {}, state = { machine: null, metric: 'total', table: false };

async function load() {
  INDEX = await (await fetch('data/index.json', { cache: 'no-cache' })).json();
  const q = new URLSearchParams(location.search);
  state.machine = q.get('m') && INDEX.some(m => m.id === q.get('m')) ? q.get('m') : INDEX[0].id;
  state.metric = METRICS[q.get('metric')] ? q.get('metric') : 'total';
  const ms = $('machine');
  for (const m of INDEX) ms.append(el('option', { value: m.id }, m.label));
  ms.value = state.machine;
  const mt = $('metric');
  for (const [k, m] of Object.entries(METRICS)) mt.append(el('option', { value: k }, m.label));
  mt.value = state.metric;
  ms.onchange = () => { state.machine = ms.value; render(); };
  mt.onchange = () => { state.metric = mt.value; render(); };
  $('showTable').onchange = e => { state.table = e.target.checked; render(); };
  const lg = $('legend');
  for (const s of SERIES) {
    const sp = el('span', { role: 'listitem' });
    const i = el('i'); i.style.background = color(s.id);
    sp.append(i, document.createTextNode(s.label));
    lg.append(sp);
  }
  render();
}

async function machineData(id) {
  if (!CACHE[id]) CACHE[id] = await (await fetch(`data/${id}.json`, { cache: 'no-cache' })).json();
  return CACHE[id];
}

async function render() {
  const q = new URLSearchParams({ m: state.machine, metric: state.metric });
  history.replaceState(null, '', `?${q}`);
  const D = await machineData(state.machine);
  const M = METRICS[state.metric];
  const mi = D.machine, ev = D.env, v = ev.versions || {};
  const meta = $('meta'); meta.replaceChildren();
  const pj = ev.git ? el('a', { href: `${REPO}/commit/${ev.git}` }, `pj ${ev.git}${ev.dirty ? '+' : ''}`) : document.createTextNode('pj');
  const parts = [mi.cpu, mi.instance_type, mi.cores || (mi.logical_cpus + ' vCPUs'), mi.os, ev.cc,
    `yyjson ${v.yyjson || '?'}`, `simdjson ${v.simdjson || '?'} (${v.simdjson_kernel || '?'})`, [pj, ` (${v.pj_arch || '?'})`],
    ev.date && ev.date.slice(0, 10)].filter(Boolean);
  parts.forEach((x, i) => { if (i) meta.append(' · '); [].concat(x).forEach(y => meta.append(y)); });
  $('metricNote').textContent = M.note;
  $('raw').href = `data/${state.machine}.json`;
  const bad = D.results.filter(r => !r.checksum_ok).length + (D.failures || []).length;
  if (bad) $('metricNote').textContent += ` Warning: ${bad} result(s) failed or disagreed on checksums.`;

  const rows = {}; // family -> size -> parser -> row
  for (const r of D.results) ((rows[r.family] ??= {})[r.size] ??= {})[r.parser] = r;
  const spread = (fam, size, p) => { // relative half-IQR of the samples, for throughput metrics
    const r = rows[fam]?.[size]?.[p];
    if (!r || !M.iqr) return null;
    const s = M.iqr(r);
    return s && s.med ? (s.p75 - s.p25) / 2 / s.med : null;
  };
  state.spread = spread;
  const val = (fam, size, p) => {
    const r = rows[fam]?.[size]?.[p];
    if (!r || (M.lazy && p === 'simdjson-od')) return null;
    const x = M.get(r);
    return x == null || !isFinite(x) ? null : x;
  };
  summary(D, M, val);
  charts(D, M, val);
  $('tableSec').hidden = !state.table;
  if (state.table) fullTable(D, M, val);
}

function summary(D, M, val) {
  const t = $('summary'); t.replaceChildren();
  if (M.higher == null) {
    $('sumSub').textContent = 'Not a better-or-worse metric: pick a throughput, memory or counter metric for this table.';
    return;
  }
  $('sumSub').textContent = `${M.label}. "Best other" is the best of yyjson and simdjson (DOM or On-Demand) at that size; green means pj beats it.`;
  const head = el('tr'); head.append(el('th', {}, 'Family'));
  const sizes = SUMMARY_SIZES.filter(s => D.sizes.includes(s));
  for (const s of sizes) head.append(el('th', {}, `${s} · pj`), el('th', {}, `${s} · pj 1t`), el('th', {}, `${s} · best other`));
  const thead = el('thead'); thead.append(head);
  const tb = el('tbody');
  const label = id => SERIES.find(x => x.id === id).label;
  for (const fam of Object.keys(D.families)) {
    const tr = el('tr'); tr.append(el('td', {}, fam));
    for (const s of sizes) {
      let best = null, bestId = null;
      for (const p of ['yyjson', 'simdjson', 'simdjson-od']) {
        const x = val(fam, s, p);
        if (x != null && (best == null || (M.higher ? x > best : x < best))) { best = x; bestId = p; }
      }
      for (const p of ['pj', 'pj-1t']) {
        const x = val(fam, s, p);
        const win = x != null && best != null && (M.higher ? x >= best : x <= best);
        tr.append(el('td', { class: x == null ? 'dim' : win ? 'good' : '' }, x == null ? '–' : fmt(x)));
      }
      const td = el('td', { class: 'dim' }, best == null ? '–' : fmt(best));
      if (bestId) { td.title = label(bestId); td.append(el('small', {}, ` ${bestId}`)); }
      tr.append(td);
    }
    tb.append(tr);
  }
  t.append(thead, tb);
}

function charts(D, M, val) {
  const box = $('charts'); box.replaceChildren();
  const W = 520, H = 260, L = 44, R = 12, T = 12, B = 30;
  for (const fam of Object.keys(D.families)) {
    const card = el('div', { class: 'card' });
    card.append(el('h3', {}, fam), el('p', {}, D.families[fam]));
    const pts = SERIES.map(s => ({ s, ys: D.sizes.map(z => val(fam, z, s.id)) }));
    const all = pts.flatMap(p => p.ys).filter(x => x != null);
    if (!all.length) { card.append(el('div', { class: 'empty' }, 'No data for this metric on this machine.')); box.append(card); continue; }
    const ymax = niceMax(Math.max(...all) * 1.05);
    const xs = i => L + (W - L - R) * (D.sizes.length === 1 ? 0.5 : i / (D.sizes.length - 1));
    const ys = y => T + (H - T - B) * (1 - y / ymax);
    const svg = svgEl('svg', { viewBox: `0 0 ${W} ${H}`, role: 'img', 'aria-label': `${fam}: ${M.label} by document size` });
    for (let k = 0; k <= 4; k++) {
      const yv = ymax * k / 4, y = ys(yv);
      svg.append(svgEl('line', { class: k ? 'gridline' : 'baseline', x1: L, x2: W - R, y1: y, y2: y }));
      const tx = svgEl('text', { x: L - 6, y: y + 4, 'text-anchor': 'end' }); tx.textContent = fmt(yv); svg.append(tx);
    }
    D.sizes.forEach((z, i) => { const tx = svgEl('text', { x: xs(i), y: H - 10, 'text-anchor': 'middle' }); tx.textContent = z; svg.append(tx); });
    for (const { s, ys: v } of pts) {
      let d = '', pen = false;
      v.forEach((y, i) => { if (y == null) { pen = false; return; } d += `${pen ? 'L' : 'M'}${xs(i).toFixed(1)},${ys(y).toFixed(1)}`; pen = true; });
      if (d) svg.append(svgEl('path', { class: 'series', d, stroke: color(s.id) }));
      v.forEach((y, i) => { if (y != null) svg.append(svgEl('circle', { class: 'dot', cx: xs(i), cy: ys(y), r: 4, fill: color(s.id) })); });
    }
    const cross = svgEl('line', { class: 'cross', y1: T, y2: H - B, visibility: 'hidden' });
    svg.append(cross);
    const hit = svgEl('rect', { x: 0, y: 0, width: W, height: H, fill: 'transparent', tabindex: 0 });
    svg.append(hit);
    const show = (i, cx, cy) => {
      cross.setAttribute('x1', xs(i)); cross.setAttribute('x2', xs(i)); cross.setAttribute('visibility', 'visible');
      tooltip(`${fam} · ${D.sizes[i]}`, pts.map(p => ({ s: p.s, y: p.ys[i], sp: state.spread(fam, D.sizes[i], p.s.id) })), M, cx, cy);
    };
    hit.addEventListener('pointermove', e => {
      const r = svg.getBoundingClientRect(), x = (e.clientX - r.left) * W / r.width;
      let i = Math.round((x - L) / ((W - L - R) / Math.max(1, D.sizes.length - 1)));
      i = Math.max(0, Math.min(D.sizes.length - 1, i));
      show(i, e.clientX, e.clientY);
    });
    let fi = D.sizes.length - 1;
    hit.addEventListener('focus', () => { const r = svg.getBoundingClientRect(); show(fi, r.left + r.width / 2, r.top + 20); });
    hit.addEventListener('keydown', e => {
      if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
      fi = Math.max(0, Math.min(D.sizes.length - 1, fi + (e.key === 'ArrowRight' ? 1 : -1)));
      const r = svg.getBoundingClientRect(); show(fi, r.left + r.width * xs(fi) / W, r.top + 20);
    });
    const hide = () => { cross.setAttribute('visibility', 'hidden'); $('tip').hidden = true; };
    hit.addEventListener('pointerleave', hide); hit.addEventListener('blur', hide);
    card.append(svg);
    box.append(card);
  }
}

function tooltip(title, items, M, cx, cy) {
  const tip = $('tip'); tip.replaceChildren(el('div', { class: 't' }, title));
  const sorted = items.filter(x => x.y != null).sort((a, b) => (M.higher === false ? a.y - b.y : b.y - a.y));
  for (const { s, y, sp } of sorted) {
    const r = el('div', { class: 'r' });
    const name = el('span'); const k = el('i'); k.style.background = color(s.id); name.append(k, document.createTextNode(s.label));
    const b = el('b', {}, `${fmt(y)} ${M.unit}`);
    if (sp != null) b.append(el('em', {}, `±${(sp * 100).toFixed(sp < 0.01 ? 1 : 0)}%`));
    r.append(name, b);
    tip.append(r);
  }
  if (!sorted.length) tip.append(el('div', {}, 'no data'));
  tip.hidden = false;
  const w = tip.offsetWidth, h = tip.offsetHeight;
  tip.style.left = `${Math.min(window.innerWidth - w - 8, cx + 14)}px`;
  tip.style.top = `${Math.max(8, Math.min(window.innerHeight - h - 8, cy - h - 10))}px`;
}

function fullTable(D, M, val) {
  const t = $('full'); t.replaceChildren();
  const head = el('tr'); head.append(el('th', {}, 'Family'), el('th', {}, 'Size'));
  for (const s of SERIES) head.append(el('th', {}, s.label));
  const thead = el('thead'); thead.append(head);
  const tb = el('tbody');
  for (const fam of Object.keys(D.families)) for (const z of D.sizes) {
    const tr = el('tr'); tr.append(el('td', {}, fam), el('td', {}, z));
    for (const s of SERIES) {
      const x = val(fam, z, s.id);
      tr.append(el('td', { class: x == null ? 'dim' : '' }, x == null ? '–' : fmt(x)));
    }
    tb.append(tr);
  }
  t.append(thead, tb);
  $('tableTitle').textContent = `All results · ${M.label}`;
}

load().catch(e => { $('metricNote').textContent = `Failed to load results: ${e.message}`; });
