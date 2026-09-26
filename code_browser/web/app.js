// The code browser's page.  Everything comes from /api/*; this file only
// renders.  No framework, no build step.
//
// Offsets in the index are *bytes*.  JavaScript strings are UTF-16, so the
// file is fetched as bytes, sliced per span, and each slice decoded on its
// own -- a span never lands in the middle of a multi-byte character because
// the indexer produced it from token boundaries.
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const decoder = new TextDecoder('utf-8');
  const encoder = new TextEncoder();

  const ROLE_NAMES = [
    [1, 'declaration'], [2, 'definition'], [4, 'reference'], [8, 'read'],
    [16, 'write'], [32, 'call'], [64, 'dynamic'], [128, 'address-of'],
    [256, 'implicit'], [512, 'undef'], [1024, 'name-ref'], [2048, 'dependent'],
    [4096, 'pasted'], [8192, 'generates'],
  ];
  const roleNames = (bits) =>
      ROLE_NAMES.filter(([b]) => bits & b).map(([, n]) => n).join(' ');
  const KIND_SHORT = {
    NAMESPACE: 'namespace', NAMESPACE_ALIAS: 'namespace alias', MACRO: 'macro',
    ENUM: 'enum', STRUCT: 'struct', CLASS: 'class', UNION: 'union',
    TYPE_ALIAS: 'alias', FUNCTION: 'function', VARIABLE: 'variable',
    FIELD: 'field', ENUM_CONSTANT: 'enumerator', INSTANCE_METHOD: 'method',
    CLASS_METHOD: 'class method', STATIC_METHOD: 'static method',
    CONSTRUCTOR: 'constructor', DESTRUCTOR: 'destructor',
    CONVERSION_FUNCTION: 'conversion', PARAMETER: 'parameter', USING: 'using',
    TEMPLATE_TYPE_PARM: 'template parameter',
    TEMPLATE_TEMPLATE_PARM: 'template template parameter',
    NON_TYPE_TEMPLATE_PARM: 'non-type template parameter', CONCEPT: 'concept',
    MESSAGE: 'message', ONEOF: 'oneof', SERVICE: 'service', RPC: 'rpc',
    PACKAGE: 'package',
  };
  const kindShort = (k) => KIND_SHORT[k] || (k || 'symbol').toLowerCase();
  // A symbol's language, where saying it helps: C and C++ are the default.
  const LANGUAGE_SHORT = { PROTO: 'proto', OBJC: 'objc' };
  const languageShort = (l) => LANGUAGE_SHORT[l] || '';
  // `field`, or `proto field` for a symbol of another language.
  const kindLabel = (s) =>
      [languageShort(s && s.language), kindShort(s && s.kind)].filter(Boolean).join(' ');
  const ROLE_GENERATES = 8192;

  const state = {
    path: null,
    bytes: null,
    symbols: new Map(),   // id -> SymbolSummary for the open file
    tokens: new Map(),    // symbol id -> [token elements]
    selected: null,       // selected symbol id
    repo: null,
    view: 'code',         // what the middle shows: 'code' or 'text' (results)
    text: null,           // {q, caseSensitive} of the results on the page
    caseSensitive: true,  // the last full-text search's choice
    scroll: {},           // view -> scrollTop while it is hidden
    indexStale: false,    // the open file changed after the index was built
    coverage: null,       // the open file's FileCoverage, when there is one
    covStops: [],         // the first line of each run of missed lines
    stepEl: null,         // the occurrence n/p last went to
  };

  // ---- API -------------------------------------------------------------

  async function getJson(url) {
    const r = await fetch(url);
    if (!r.ok) {
      let message = r.statusText;
      try { message = (await r.json()).message || message; } catch (e) { /* not JSON */ }
      throw new Error(`${r.status} ${message}`);
    }
    return r.json();
  }

  async function getBytes(url) {
    const r = await fetch(url);
    if (!r.ok) {
      let message = r.statusText;
      try { message = (await r.json()).message || message; } catch (e) { /* not JSON */ }
      throw new Error(`${r.status} ${message}`);
    }
    return { bytes: new Uint8Array(await r.arrayBuffer()), headers: r.headers };
  }

  const fileUrl = (path) => `/api/file?path=${encodeURIComponent(path)}`;

  // ---- status ----------------------------------------------------------

  function status(text, isError) {
    const el = $('status');
    el.textContent = text;
    el.classList.toggle('error', !!isError);
  }

  // ---- coverage --------------------------------------------------------

  // Whether coverage is shown, kept per browser (default on).  The server
  // has coverage only when it was started with --coverage.
  const COVERAGE_KEY = 'code_browser.coverage';
  const hasCoverage = () => !!(state.repo && state.repo.coverage);
  function coverageWanted() {
    try { return window.localStorage.getItem(COVERAGE_KEY) !== 'off'; } catch (e) { return true; }
  }
  // Rounded down, so that a file with one line missed never reads 100%.
  function pct(hit, found) {
    if (!found) return '–';
    return (Math.floor((hit || 0) * 1000 / found) / 10).toFixed(1) + '%';
  }
  function formatHits(h) {
    if (h >= 4294967295) return '4.3G+';  // the API caps counts there
    if (h < 1000) return String(h);
    for (const [div, unit] of [[1e9, 'G'], [1e6, 'M'], [1e3, 'k']])
      if (h >= div) return (h / div).toFixed(h < div * 10 ? 1 : 0) + unit;
    return String(h);
  }
  function coverageSummary(t) {
    let s = `${t.lines_hit || 0}/${t.lines_found || 0} lines (${pct(t.lines_hit, t.lines_found)})`;
    if (t.branches_found) s += `, ${t.branches_hit || 0}/${t.branches_found} branches (${pct(t.branches_hit, t.branches_found)})`;
    return s;
  }
  function coverageBadge(t) {
    const el = document.createElement('span');
    const ratio = (t.lines_hit || 0) / t.lines_found;
    el.className = 'cov-pct ' + (ratio < 0.5 ? 'cov-lo' : ratio < 0.8 ? 'cov-mid' : 'cov-hi');
    el.textContent = pct(t.lines_hit, t.lines_found);
    el.title = coverageSummary(t) + (t.files > 1 ? ` in ${t.files} files` : '');
    return el;
  }

  function setCoverageOn(on) {
    document.body.classList.toggle('cov-on', on);
    $('coverage-toggle').setAttribute('aria-pressed', on ? 'true' : 'false');
    try { window.localStorage.setItem(COVERAGE_KEY, on ? 'on' : 'off'); } catch (e) { /* private mode */ }
    updateBanner();
  }

  function initCoverage() {
    const info = state.repo.coverage;
    const t = info.totals || {};
    const button = $('coverage-toggle');
    button.hidden = false;
    button.textContent = 'coverage ' + pct(t.lines_hit, t.lines_found);
    button.title = `${coverageSummary(t)} in ${t.files || 0} files\n` +
        `from ${info.path}` + (info.collected_at ? `, collected ${info.collected_at}` : '') +
        '\nu / U: next / previous uncovered lines';
    button.addEventListener('click', () => setCoverageOn(!document.body.classList.contains('cov-on')));
    setCoverageOn(coverageWanted());
  }

  // Marks every instrumented line of the open file: hit, missed, or partial
  // (ran, but a branch on it never went one of its ways).  Toggling coverage
  // is CSS alone; nothing here runs again.
  function applyCoverage(cov) {
    state.coverage = cov;
    state.covStops = [];
    $('code').classList.toggle('has-cov', !!cov);
    if (!cov) return;
    const lines = cov.lines || [], hits = cov.hits || [];
    const branches = new Map();
    (cov.branch_lines || []).forEach((ln, i) =>
        branches.set(ln, [(cov.branches || [])[i] || 0, (cov.branches_taken || [])[i] || 0]));
    let inMiss = false;
    for (let i = 0; i < lines.length; i++) {
      const ln = lines[i], h = hits[i] || 0, br = branches.get(ln);
      const el = $('L' + ln);
      if (!el) continue;  // past the end: the file changed since
      const cls = h === 0 ? 'cov-miss' : br && br[1] < br[0] ? 'cov-partial' : 'cov-hit';
      el.classList.add(cls);
      el.dataset.hits = formatHits(h);
      el.querySelector('.ln').title = `hit ${h} time${h === 1 ? '' : 's'}` +
          (br ? `; ${br[1]} of ${br[0]} branches taken` : '');
      // Lines with no code in between do not break a run of missed lines.
      if (cls === 'cov-miss' && !inMiss) state.covStops.push(ln);
      inMiss = cls === 'cov-miss';
    }
  }

  // The banner over the code: why its annotations or its coverage may be off.
  function updateBanner() {
    const banner = $('banner');
    const lines = [];
    if (state.indexStale) lines.push('This file changed after the index was built; annotations may be shifted.');
    if (state.coverage && state.coverage.stale && document.body.classList.contains('cov-on'))
      lines.push('This file changed after the coverage was collected; hit counts may be on the wrong lines.');
    banner.textContent = lines.join(' ');
    if (lines.length) banner.dataset.stale = '1';
    else delete banner.dataset.stale;
    if (state.view === 'code' && state.path) banner.hidden = !lines.length;
  }

  // u / U: the next / previous run of missed lines, from the marked line or
  // the middle of the view.
  function stepUncovered(direction) {
    if (!hasCoverage()) { status('no coverage loaded (run cpp_format.sh coverage)'); return; }
    if (!document.body.classList.contains('cov-on')) { status('coverage is off (the coverage button turns it on)'); return; }
    if (!state.coverage) { status(`${state.path || 'this file'}: no coverage data`); return; }
    const stops = state.covStops;
    if (!stops.length) { status(`${state.path}: no uncovered lines`); return; }
    let from = 0;
    const marked = $('code').querySelector('.line.mark');
    const view = $('code').getBoundingClientRect();
    if (marked) {
      const r = marked.getBoundingClientRect();
      if (r.bottom >= view.top && r.top <= view.bottom) from = +marked.id.slice(1);
    }
    if (!from) {
      const hit = document.elementFromPoint(view.left + view.width / 2, (view.top + view.bottom) / 2);
      const line = hit && hit.closest('.line');
      if (line) from = +line.id.slice(1);
    }
    let index;
    if (direction > 0) {
      index = stops.findIndex((ln) => ln > from);
      if (index < 0) index = 0;
    } else {
      index = -1;
      for (let i = stops.length - 1; i >= 0; i--) if (stops[i] < from) { index = i; break; }
      if (index < 0) index = stops.length - 1;
    }
    goToLine(stops[index]);
    status(`${state.path}: uncovered block ${index + 1} of ${stops.length} (line ${stops[index]})`);
  }

  // ---- tree ------------------------------------------------------------

  async function loadDir(prefix, container) {
    let list;
    try {
      list = await getJson(`/api/files?prefix=${encodeURIComponent(prefix)}`);
    } catch (e) {
      container.textContent = e.message;
      return;
    }
    container.textContent = '';
    for (const entry of list.entries || []) {
      if (entry.is_dir) {
        const details = document.createElement('details');
        const summary = document.createElement('summary');
        summary.textContent = entry.name;
        if (entry.coverage && entry.coverage.lines_found) summary.appendChild(coverageBadge(entry.coverage));
        details.appendChild(summary);
        const children = document.createElement('div');
        children.className = 'children';
        details.appendChild(children);
        let loaded = false;
        details.addEventListener('toggle', () => {
          if (details.open && !loaded) {
            loaded = true;
            loadDir(entry.path, children);
          }
        });
        container.appendChild(details);
      } else {
        const a = document.createElement('a');
        a.className = 'file k-' + (entry.kind || 'SOURCE').toLowerCase();
        if (entry.available === false) {
          a.classList.add('unavailable');
          a.title = 'not present in this checkout';
        }
        a.textContent = entry.name;
        if (entry.coverage && entry.coverage.lines_found) a.appendChild(coverageBadge(entry.coverage));
        a.href = '#' + entry.path;
        a.dataset.path = entry.path;
        container.appendChild(a);
      }
    }
  }

  // Opens every directory on the way to `path` so the file is visible.
  async function revealInTree(path) {
    const parts = path.split('/');
    let container = $('tree');
    let prefix = '';
    for (let i = 0; i < parts.length - 1; i++) {
      prefix = prefix ? prefix + '/' + parts[i] : parts[i];
      const details = [...container.querySelectorAll(':scope > details')]
          .find((d) => d.querySelector(':scope > summary').firstChild.textContent === parts[i]);
      if (!details) return;
      if (!details.open) {
        details.open = true;
        // wait for the lazy load
        for (let t = 0; t < 50 && !details.querySelector('.children').children.length; t++)
          await new Promise((r) => setTimeout(r, 20));
      }
      container = details.querySelector('.children');
    }
    for (const a of $('tree').querySelectorAll('a.file'))
      a.classList.toggle('current', a.dataset.path === path);
  }

  // ---- rendering -------------------------------------------------------

  // The spans of the index and the `#include` spellings, as one list sorted
  // by offset.  An include is a span with `include` set and no symbol.
  function mergeSpans(spans, includes) {
    const out = [];
    let i = 0;
    for (const inc of includes) {
      const b = inc.begin || 0;
      while (i < spans.length && (spans[i].begin || 0) < b) out.push(spans[i++]);
      out.push({ begin: b, end: inc.end || 0, include: inc });
    }
    while (i < spans.length) out.push(spans[i++]);
    return out;
  }

  function includeElement(inc, text) {
    const targets = inc.targets || [];
    if (!targets.length) {
      const el = document.createElement('span');
      el.className = 'inc missing';
      el.title = 'no indexed file matches this include';
      el.textContent = text;
      return el;
    }
    const a = document.createElement('a');
    a.className = 'inc';
    a.href = '#' + targets[0].path;
    a.title = targets[0].path + (targets.length > 1 ? ` (+${targets.length - 1} more: click to choose)` : '');
    a.textContent = text;
    if (targets.length > 1) a.include = inc;
    return a;
  }

  function render(bytes, ann, includes) {
    const code = $('code');
    code.textContent = '';
    state.symbols = new Map((ann.symbols || []).map((s) => [s.id, s]));
    state.tokens = new Map();
    const spans = mergeSpans(ann.spans || [], includes || []);
    const lineStarts = [0];
    for (let i = 0; i < bytes.length; i++)
      if (bytes[i] === 0x0a && i + 1 < bytes.length) lineStarts.push(i + 1);
    const fragment = document.createDocumentFragment();
    let si = 0;
    for (let ln = 0; ln < lineStarts.length; ln++) {
      const start = lineStarts[ln];
      let end = ln + 1 < lineStarts.length ? lineStarts[ln + 1] : bytes.length;
      const lineEl = document.createElement('div');
      lineEl.className = 'line';
      lineEl.id = 'L' + (ln + 1);
      const num = document.createElement('a');
      num.className = 'ln';
      num.href = '#' + state.path + ':' + (ln + 1);
      num.textContent = ln + 1;
      lineEl.appendChild(num);
      const codeEl = document.createElement('span');
      codeEl.className = 'text';
      let pos = start;
      let lastTok = null;  // {el, end, syms}
      while (si < spans.length && spans[si].begin < end) {
        const s = spans[si++];
        const b = s.begin || 0, e = s.end || 0, sym = s.symbol || 0;
        if (e <= b || b < start) continue;
        if (s.include) {
          if (b < pos || e > end) continue;  // under a token: leave that alone
          if (b > pos) codeEl.appendChild(document.createTextNode(decoder.decode(bytes.subarray(pos, b))));
          codeEl.appendChild(includeElement(s.include, decoder.decode(bytes.subarray(b, e))));
          pos = e;
          lastTok = null;
          continue;
        }
        if (lastTok && b === lastTok.begin && e === lastTok.end) {
          addSymbol(lastTok, sym, s);  // one token, several symbols
          continue;
        }
        if (lastTok && b < lastTok.end) {
          addSymbol(lastTok, sym, s);  // nested (a pasted argument): fold in
          continue;
        }
        if (b > pos) codeEl.appendChild(document.createTextNode(decoder.decode(bytes.subarray(pos, b))));
        const tok = document.createElement('span');
        tok.textContent = decoder.decode(bytes.subarray(b, Math.min(e, end)));
        tok.dataset.b = b;
        tok.dataset.e = e;
        lastTok = { el: tok, begin: b, end: e, syms: [], roles: 0 };
        addSymbol(lastTok, sym, s);
        codeEl.appendChild(tok);
        pos = Math.min(e, end);
      }
      if (pos < end) codeEl.appendChild(document.createTextNode(decoder.decode(bytes.subarray(pos, end))));
      lineEl.appendChild(codeEl);
      fragment.appendChild(lineEl);
    }
    code.appendChild(fragment);
    for (const u of ann.unresolved || []) {
      // an unresolved dependent token: nothing to link, but say so
      const tok = tokenAt(u.begin);
      if (tok) tok.classList.add('unresolved');
    }

    function addSymbol(tok, sym, span) {
      if (!tok.syms.includes(sym)) tok.syms.push(sym);
      tok.roles |= span.roles || 0;
      const el = tok.el;
      const summary = state.symbols.get(sym);
      el.className = 'tok k-' + ((summary && summary.kind) || 'unknown').toLowerCase();
      if (tok.roles & 2) el.classList.add('r-def');
      else if (tok.roles & 1) el.classList.add('r-decl');
      if (tok.roles & 16) el.classList.add('r-write');
      if (tok.roles & ROLE_GENERATES) el.classList.add('r-generates');
      if (span.macro === 'MACRO_BODY') el.classList.add('macro-body');
      el.dataset.s = tok.syms.join(',');
      if (!state.tokens.has(sym)) state.tokens.set(sym, []);
      if (!state.tokens.get(sym).includes(el)) state.tokens.get(sym).push(el);
    }
  }

  function tokenAt(offset) {
    for (const tok of $('code').querySelectorAll('.tok'))
      if (+tok.dataset.b <= offset && offset < +tok.dataset.e) return tok;
    return null;
  }

  // ---- resizing ----------------------------------------------------------

  // Sizes the reader dragged, kept per browser; the page works without them.
  const SIZES_KEY = 'code_browser.sizes';
  let sizes = {};
  try { sizes = JSON.parse(window.localStorage.getItem(SIZES_KEY) || '{}') || {}; } catch (e) { sizes = {}; }
  function setSize(name, value) {
    if (value === null) {
      delete sizes[name];
      document.documentElement.style.removeProperty('--' + name);
    } else {
      sizes[name] = value;
      document.documentElement.style.setProperty('--' + name, value);
    }
  }
  function saveSizes() {
    try { window.localStorage.setItem(SIZES_KEY, JSON.stringify(sizes)); } catch (e) { /* private mode */ }
  }
  for (const [name, value] of Object.entries(sizes))
    if (typeof value === 'string') document.documentElement.style.setProperty('--' + name, value);
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  // Drags `el` to set the CSS variable `name`.  `begin()` is called when a
  // drag (or a key press) starts and returns what the size becomes for a move
  // of the pointer by `delta` pixels (x for a vertical splitter, y for a
  // horizontal one).  Arrow keys nudge; a double-click forgets.
  function makeSplitter(el, name, begin) {
    const vertical = el.classList.contains('vertical');
    const coordinate = (ev) => (vertical ? ev.clientX : ev.clientY);
    el.addEventListener('pointerdown', (ev) => {
      if (ev.button !== 0) return;
      ev.preventDefault();
      el.setPointerCapture(ev.pointerId);
      el.classList.add('dragging');
      document.body.classList.add('resizing', vertical ? 'vertical' : 'horizontal');
      const start = coordinate(ev);
      const size = begin();
      const move = (e) => setSize(name, size(coordinate(e) - start));
      const done = () => {
        el.removeEventListener('pointermove', move);
        el.removeEventListener('pointerup', done);
        el.removeEventListener('pointercancel', done);
        el.classList.remove('dragging');
        document.body.classList.remove('resizing', 'vertical', 'horizontal');
        saveSizes();
      };
      el.addEventListener('pointermove', move);
      el.addEventListener('pointerup', done);
      el.addEventListener('pointercancel', done);
    });
    el.addEventListener('dblclick', () => { setSize(name, null); saveSizes(); });
    el.addEventListener('keydown', (ev) => {
      const step = ev.shiftKey ? 64 : 16;
      const keys = vertical ? { ArrowLeft: -step, ArrowRight: step } : { ArrowUp: -step, ArrowDown: step };
      if (!(ev.key in keys)) return;
      ev.preventDefault();
      setSize(name, begin()(keys[ev.key]));
      saveSizes();
    });
  }

  function initSplitters() {
    makeSplitter($('tree-splitter'), 'tree-width', () => {
      const width = $('tree').getBoundingClientRect().width;
      const room = $('main').getBoundingClientRect().width - 240;
      return (dx) => clamp(width + dx, 120, room) + 'px';
    });
    makeSplitter($('panel-splitter'), 'panel-height', () => {
      const height = $('panel').getBoundingClientRect().height;
      const room = $('source').getBoundingClientRect().height - 80;
      return (dy) => clamp(height - dy, 80, room) + 'px';
    });
  }

  // The splitter between a symbol's occurrences and its details: a share of
  // the pane's width, so that it survives a resize of the window.
  function columnSplitter(columns) {
    const el = document.createElement('div');
    el.className = 'splitter vertical';
    el.setAttribute('role', 'separator');
    el.setAttribute('aria-orientation', 'vertical');
    el.setAttribute('aria-label', 'resize the columns');
    el.tabIndex = 0;
    el.title = 'drag to resize (double-click resets)';
    makeSplitter(el, 'refs-width', () => {
      const width = columns.firstElementChild.getBoundingClientRect().width;
      const total = columns.clientWidth;  // what a grid track's % is of
      return (dx) => clamp((width + dx) / total * 100, 15, 85).toFixed(2) + '%';
    });
    return el;
  }

  // ---- files -----------------------------------------------------------

  // The middle of the page: the open file, or full-text results.  Both stay
  // rendered; switching back and forth keeps each one's scroll position.
  function showView(view) {
    // Hidden, an element forgets how far it was scrolled: keep it here.
    const scrollers = { code: $('code'), text: $('text-results') };
    if (state.view !== view) {
      const leaving = scrollers[state.view];
      state.scroll[state.view] = leaving.scrollTop;
    }
    const wasHidden = scrollers[view].hidden;
    state.view = view;
    $('code').hidden = view !== 'code';
    $('file-title').hidden = view !== 'code';
    $('text-results').hidden = view !== 'text';
    $('text-title').hidden = view !== 'text';
    if (view === 'text') {
      $('banner').hidden = true;
    } else if (state.path) {
      $('banner').hidden = !$('banner').dataset.stale;
      document.title = state.path.split('/').pop() + ' – code browser';
    }
    if (wasHidden) scrollers[view].scrollTop = state.scroll[view] || 0;
  }

  async function openFile(path, line) {
    if (state.path === path) {
      showView('code');
      goToLine(line);
      return;
    }
    status(`loading ${path}…`);
    let file, ann, includes, cov;
    try {
      [file, ann, includes, cov] = await Promise.all([
        getBytes(fileUrl(path)),
        getJson(`/api/annotations?path=${encodeURIComponent(path)}`).catch((e) => {
          if (String(e.message).startsWith('404')) return { spans: [], symbols: [] };
          throw e;
        }),
        // Links only: a file without them is still a file.
        getJson(`/api/includes?path=${encodeURIComponent(path)}`).catch(() => ({})),
        // Coverage is an overlay: a file the tracefile does not name has none.
        hasCoverage()
          ? getJson(`/api/coverage?path=${encodeURIComponent(path)}`).catch(() => null)
          : null,
      ]);
    } catch (e) {
      status(e.message, true);
      return;
    }
    state.path = path;
    state.bytes = file.bytes;
    $('file-title').textContent = path;
    state.indexStale = !!file.headers.get('X-Newer-Than-Index');
    state.coverage = cov;
    updateBanner();
    state.scroll.code = 0;  // a new file starts at the top (or at `line`)
    state.stepEl = null;
    showView('code');
    render(file.bytes, ann, includes.includes);
    applyCoverage(cov);
    // The panel outlives the file (that is how a reference list is walked):
    // what it selected is highlighted here as well.
    if (state.selected !== null) selectSymbol(state.selected);
    document.title = path.split('/').pop() + ' – code browser';
    let line2 = `${path} — ${(ann.spans || []).length} annotated tokens`;
    if (hasCoverage())
      line2 += cov ? ` · coverage ${coverageSummary(cov.totals || {})}` : ' · no coverage data';
    status(line2);
    revealInTree(path);
    goToLine(line);
  }

  function goToLine(line) {
    for (const el of $('code').querySelectorAll('.line.mark')) el.classList.remove('mark');
    if (!line) return;
    const el = $('L' + line);
    if (!el) return;
    el.classList.add('mark');
    el.scrollIntoView({ block: 'center' });
  }

  // ---- selection and the panel ---------------------------------------

  function selectSymbol(id) {
    if (state.selected !== null)
      for (const el of state.tokens.get(state.selected) || []) el.classList.remove('hl');
    state.selected = id;
    for (const el of state.tokens.get(id) || []) el.classList.add('hl');
  }

  function closePanel() {
    $('panel').hidden = true;
  }

  // Opens the panel below the code with one tab per entry of `tabs`:
  // {label, kind, build(pane), onSelect()}.  A pane is built the first time
  // its tab is shown.  One thing clicked is one tab; a token that names several
  // symbols, several.
  function openPanel(tabs) {
    const ribbon = $('panel-tabs');
    const body = $('panel-body');
    ribbon.textContent = '';
    body.textContent = '';
    const entries = tabs.map((tab) => {
      const button = document.createElement('button');
      button.className = 'tab';
      button.setAttribute('role', 'tab');
      if (tab.kind) {
        const kind = document.createElement('span');
        kind.className = 'kind k-' + tab.kind.toLowerCase();
        kind.textContent = kindLabel(tab);
        button.appendChild(kind);
      }
      button.appendChild(document.createTextNode(tab.label));
      ribbon.appendChild(button);
      const pane = document.createElement('div');
      pane.className = 'pane';
      pane.hidden = true;
      body.appendChild(pane);
      return { tab, button, pane, built: false };
    });
    const show = (entry) => {
      for (const e of entries) {
        e.pane.hidden = e !== entry;
        e.button.classList.toggle('current', e === entry);
        e.button.setAttribute('aria-selected', e === entry ? 'true' : 'false');
      }
      if (!entry.built) {
        entry.built = true;
        entry.tab.build(entry.pane);
      }
      if (entry.tab.onSelect) entry.tab.onSelect();
      body.scrollTop = 0;
    };
    for (const entry of entries) entry.button.addEventListener('click', () => show(entry));
    $('panel').hidden = false;
    if (entries.length) show(entries[0]);
  }

  function location(loc) {
    if (!loc || !loc.path) return null;
    const line = loc.line || 0;
    return { text: line ? `${loc.path}:${line}` : loc.path, href: '#' + loc.path + (line ? ':' + line : '') };
  }

  // Everything about one symbol: what it is, then its details and its
  // references side by side, both loaded at once.
  function symbolPane(summary, pane) {
    const head = document.createElement('div');
    head.className = 'pane-head';
    const name = document.createElement('span');
    name.className = 'qname';
    name.textContent = summary.qualified_name || summary.name;
    head.appendChild(name);
    if (summary.type) {
      const type = document.createElement('span');
      type.className = 'type';
      type.textContent = summary.type;
      head.appendChild(type);
    }
    // Generated code is read where it was written: the .proto field before
    // the accessor's declaration in a header under bazel-out.
    const origin = summary.origin;
    const from = origin && location(origin.definition);
    if (origin) {
      const a = document.createElement('a');
      a.className = 'origin';
      a.href = from ? from.href : '#';
      a.textContent = `generated from ${kindLabel(origin)} ${origin.qualified_name || origin.name}` +
          (from ? ' — ' + from.text : '');
      head.appendChild(a);
    }
    const def = location(summary.definition);
    if (def) {
      const a = document.createElement('a');
      a.href = def.href;
      a.textContent = (origin ? 'generated declaration: ' : 'definition: ') + def.text;
      head.appendChild(a);
    }
    pane.appendChild(head);
    // Where it is used on the left, what it is on the right.
    const columns = document.createElement('div');
    columns.className = 'columns';
    const refs = document.createElement('div');
    const details = document.createElement('div');
    columns.appendChild(refs);
    columns.appendChild(columnSplitter(columns));
    columns.appendChild(details);
    pane.appendChild(columns);
    showDetails(summary.id, details);
    showReferences(summary.id, refs);
  }

  // The candidates of an include that more than one indexed file matches.
  function includePane(inc, pane) {
    const title = document.createElement('div');
    title.className = 'section';
    title.textContent = `${inc.targets.length} indexed files match ${inc.angled ? '<' : '"'}${inc.spelling}${inc.angled ? '>' : '"'}, best first`;
    pane.appendChild(title);
    const list = document.createElement('ul');
    for (const t of inc.targets) {
      const li = document.createElement('li');
      const a = document.createElement('a');
      a.href = '#' + t.path;
      a.textContent = t.path;
      li.appendChild(a);
      const kind = document.createElement('span');
      kind.className = 'roles';
      kind.textContent = ' ' + (t.kind || 'SOURCE').toLowerCase() + (t.available === true ? '' : ', not in this checkout');
      li.appendChild(kind);
      list.appendChild(li);
    }
    pane.appendChild(list);
  }

  async function showReferences(id, body) {
    body.textContent = 'loading…';
    let refs;
    try {
      refs = await getJson(`/api/refs/${id}?limit=200&expand=generated`);
    } catch (e) {
      body.textContent = e.message;
      return;
    }
    body.textContent = '';
    // With generated symbols in the listing, each line says which one it is
    // (`set_size`), and a line of the symbol itself says nothing.
    const generated = new Map((refs.symbols || []).map((s) => [s.id, s]));
    const title = document.createElement('div');
    title.className = 'section';
    title.textContent = `${refs.total || 0} occurrence(s)` +
        (generated.size ? `, with those of the ${generated.size} symbol(s) generated from it` : '') +
        (refs.truncated ? ' (first 200)' : '');
    body.appendChild(title);
    // Uses in test code (files of testonly Bazel targets) come last -- the
    // server orders them so -- under a divider of their own.
    const files = refs.files || [];
    let inTests = false;
    for (const file of files) {
      if (!inTests && file.test) {
        inTests = true;
        const tests = document.createElement('div');
        tests.className = 'tests';
        const n = files.filter((f) => f.test).reduce((k, f) => k + (f.refs || []).length, 0);
        tests.textContent = `in tests (${n}${refs.truncated ? '+' : ''})`;
        body.appendChild(tests);
      }
      const fileEl = document.createElement('div');
      fileEl.className = 'ref-file';
      fileEl.textContent = file.path;
      body.appendChild(fileEl);
      const list = document.createElement('ul');
      for (const ref of file.refs || []) {
        const li = document.createElement('li');
        const loc = ref.location || {};
        const a = document.createElement('a');
        a.href = '#' + file.path + (loc.line ? ':' + loc.line : '');
        a.textContent = loc.line ? String(loc.line) : String(loc.begin || 0);
        li.appendChild(a);
        const via = ref.symbol !== undefined && generated.get(ref.symbol);
        if (via) {
          const name = document.createElement('span');
          name.className = 'via';
          name.textContent = ' ' + via.name;
          name.title = via.qualified_name || via.name;
          li.appendChild(name);
        }
        const roles = document.createElement('span');
        roles.className = 'roles';
        // A call of the setter is a write of the field it was generated from.
        roles.textContent = ' ' + roleNames(ref.roles || 0) +
            (via && via.modifies_origin ? ' — writes it' : '');
        li.appendChild(roles);
        if (ref.line_text) {
          const text = document.createElement('code');
          text.textContent = ref.line_text.trim();
          li.appendChild(text);
        }
        list.appendChild(li);
      }
      body.appendChild(list);
    }
  }

  async function showDetails(id, body) {
    body.textContent = 'loading…';
    let info;
    try {
      info = await getJson(`/api/symbol/${id}`);
    } catch (e) {
      body.textContent = e.message;
      return;
    }
    body.textContent = '';
    const add = (label, locs) => {
      if (!locs || !locs.length) return;
      const title = document.createElement('div');
      title.className = 'section';
      title.textContent = label;
      body.appendChild(title);
      const list = document.createElement('ul');
      for (const l of locs) {
        const loc = location(l);
        if (!loc) continue;
        const li = document.createElement('li');
        const a = document.createElement('a');
        a.href = loc.href;
        a.textContent = loc.text;
        li.appendChild(a);
        list.appendChild(li);
      }
      body.appendChild(list);
    };
    add('definitions', info.definitions);
    add('declarations', info.declarations);
    const symbols = (label, relations, prefix) => {
      if (!relations.length) return;
      const title = document.createElement('div');
      title.className = 'section';
      title.textContent = label;
      body.appendChild(title);
      const list = document.createElement('ul');
      for (const r of relations) {
        const li = document.createElement('li');
        const tag = document.createElement('span');
        tag.className = 'roles';
        tag.textContent = prefix(r);
        li.appendChild(tag);
        const def = location(r.symbol && r.symbol.definition);
        const a = document.createElement('a');
        a.href = def ? def.href : '#';
        a.textContent = (r.symbol && r.symbol.qualified_name) || '?';
        li.appendChild(a);
        list.appendChild(li);
      }
      body.appendChild(list);
    };
    // Provenance first and by name: it is the one relation that crosses
    // languages, and what a reader of generated code is after.
    const all = info.related || [];
    const provenance = (r) => r.kind === 'GENERATED_FROM';
    symbols('generated from', all.filter((r) => provenance(r) && !r.reverse),
            (r) => kindLabel(r.symbol) + ' ');
    symbols('generated from it', all.filter((r) => provenance(r) && r.reverse),
            (r) => kindLabel(r.symbol) + ' ');
    symbols('related', all.filter((r) => !provenance(r)),
            (r) => (r.reverse ? '← ' : '→ ') + (r.kind || '').toLowerCase().replace(/_/g, ' ') + ' ');
    const c = info.counts || {};
    const counts = document.createElement('div');
    counts.className = 'section';
    counts.textContent = `${c.total || 0} occurrences in ${c.files || 0} file(s): ` +
        `${c.definitions || 0} definitions, ${c.declarations || 0} declarations, ${c.references || 0} references` +
        (c.generated ? `; ${c.generated} more of the symbols generated from it` : '');
    body.appendChild(counts);
    if (info.symbol && info.symbol.usr) {
      const usr = document.createElement('div');
      usr.className = 'usr';
      usr.textContent = info.symbol.usr;
      body.appendChild(usr);
    }
  }

  // Keeps what was clicked in view: the panel takes its room from the code.
  function keepVisible(el) {
    const view = $('code').getBoundingClientRect();
    const rect = el.getBoundingClientRect();
    if (rect.bottom > view.bottom || rect.top < view.top) el.scrollIntoView({ block: 'center' });
  }

  function openSymbolPanel(tok) {
    const ids = (tok.dataset.s || '').split(',').filter(Boolean).map(Number);
    if (!ids.length) return;
    const tab = (summary) => ({
      label: summary.name || summary.qualified_name || '?',
      kind: summary.kind || 'unknown',
      language: summary.language,
      build: (pane) => symbolPane(summary, pane),
      onSelect: () => selectSymbol(summary.id),
    });
    // Each symbol the token names, then what those were generated from: the
    // proto field next to `set_size`, a click away.
    const tabs = [];
    const seen = new Set();
    const summaries = ids.map((id) => state.symbols.get(id) || { id, name: '?', kind: 'unknown' });
    for (const summary of summaries.concat(summaries.map((s) => s.origin).filter(Boolean))) {
      if (seen.has(summary.id)) continue;
      seen.add(summary.id);
      tabs.push(tab(summary));
    }
    openPanel(tabs);
    keepVisible(tok);
  }

  // Where ctrl-click goes: the definition -- for generated code, of what it
  // was generated from.
  function definitionOf(tok) {
    for (const id of (tok.dataset.s || '').split(',').filter(Boolean).map(Number)) {
      const summary = state.symbols.get(id);
      const target = summary && location((summary.origin || summary).definition);
      if (target) return target.href;
    }
    return null;
  }

  function openIncludePanel(el) {
    const inc = el.include;
    openPanel([{ label: `#include ${inc.spelling}`, build: (pane) => includePane(inc, pane) }]);
    keepVisible(el);
  }

  // ---- search ----------------------------------------------------------

  // Where a full-text search lives: `#?text=<q>[&case=insensitive]`.  No
  // path starts with `?`.
  function textHash(q, caseSensitive) {
    const params = new URLSearchParams({ text: q });
    if (!caseSensitive) params.set('case', 'insensitive');
    return '#?' + params.toString();
  }

  // The first row of the dropdown: the same words, as text.
  function textSearchRow(q) {
    const a = document.createElement('a');
    a.className = 'hit text-search';
    a.href = textHash(q, state.caseSensitive);
    const kind = document.createElement('span');
    kind.className = 'kind';
    kind.textContent = 'text';
    a.appendChild(kind);
    const name = document.createElement('span');
    name.className = 'qname';
    name.textContent = `Search the text for “${q}”`;
    a.appendChild(name);
    const where = document.createElement('span');
    where.className = 'where';
    where.textContent = 'Enter';
    a.appendChild(where);
    a.addEventListener('click', () => { $('search-results').hidden = true; });
    return a;
  }

  let searchTimer = null;
  let searchSeq = 0;  // a newer search, or Enter, discards an older answer
  async function runSearch() {
    const q = $('search').value.trim();
    const box = $('search-results');
    const seq = ++searchSeq;
    if (!q) {
      box.hidden = true;
      return;
    }
    let result;
    try {
      result = await getJson(`/api/search?q=${encodeURIComponent(q)}&limit=20`);
    } catch (e) {
      if (seq !== searchSeq) return;
      box.textContent = '';
      box.appendChild(textSearchRow(q));
      box.appendChild(document.createTextNode(e.message));
      box.hidden = false;
      return;
    }
    if (seq !== searchSeq) return;
    box.textContent = '';
    box.appendChild(textSearchRow(q));
    const hits = result.hits || [];
    if (!hits.length) {
      const none = document.createElement('div');
      none.className = 'hit';
      none.textContent = 'no symbols';
      box.appendChild(none);
    }
    for (const hit of hits) {
      const s = hit.symbol || {};
      // Generated code is found under its own name and read at its origin.
      const def = location((s.origin || s).definition);
      const a = document.createElement('a');
      a.className = 'hit';
      a.href = def ? def.href : '#';
      const kind = document.createElement('span');
      kind.className = 'kind k-' + (s.kind || 'unknown').toLowerCase();
      kind.textContent = kindLabel(s);
      a.appendChild(kind);
      const name = document.createElement('span');
      name.className = 'qname';
      name.textContent = s.qualified_name || s.name;
      a.appendChild(name);
      if (def) {
        const where = document.createElement('span');
        where.className = 'where';
        where.textContent = def.text;
        a.appendChild(where);
      }
      a.addEventListener('click', () => { box.hidden = true; });
      box.appendChild(a);
    }
    box.hidden = false;
  }

  // ---- full-text results ------------------------------------------------

  const TEXT_PAGE = 200;  // lines per request
  let textSeq = 0;        // a newer search discards an older answer

  // The results of a full-text search, in the middle of the page.  Coming
  // back to the search on the page (Back from a result) shows it as it was.
  async function showTextResults(q, caseSensitive) {
    // The box says what is shown -- unless the reader is typing in it.
    if (document.activeElement !== $('search')) $('search').value = q;
    $('search-results').hidden = true;
    ++searchSeq;
    state.caseSensitive = caseSensitive;
    document.title = `“${q}” – code browser`;
    if (state.text && state.text.q === q && state.text.caseSensitive === caseSensitive) {
      showView('text');
      return;
    }
    state.text = { q, caseSensitive };
    const seq = ++textSeq;
    const title = $('text-title');
    title.textContent = '';
    const summary = document.createElement('span');
    summary.className = 'summary';
    summary.textContent = `searching for “${q}”…`;
    title.appendChild(summary);
    const label = document.createElement('label');
    const box = document.createElement('input');
    box.type = 'checkbox';
    box.checked = caseSensitive;
    box.addEventListener('change', () => {
      state.caseSensitive = box.checked;  // before the hashchange: Enter may come first
      window.location.hash = textHash(q, box.checked);
    });
    label.appendChild(box);
    label.appendChild(document.createTextNode(' match case'));
    title.appendChild(label);
    const body = $('text-results');
    body.textContent = '';
    state.scroll.text = 0;
    showView('text');
    await loadTextPage(q, caseSensitive, 0, { seq, summary, body, last: null });
  }

  async function loadTextPage(q, caseSensitive, offset, page) {
    const started = performance.now();
    let r;
    try {
      r = await getJson(`/api/text?q=${encodeURIComponent(q)}` +
          `&case=${caseSensitive ? 'sensitive' : 'insensitive'}&offset=${offset}&limit=${TEXT_PAGE}`);
    } catch (e) {
      if (page.seq !== textSeq) return;
      state.text = null;  // not cached: try again next time
      page.summary.textContent = `“${q}”`;
      const notice = document.createElement('div');
      notice.className = 'notice';
      notice.textContent = e.message;
      page.body.appendChild(notice);
      status(e.message, true);
      return;
    }
    if (page.seq !== textSeq) return;
    const ms = Math.round(performance.now() - started);
    const matches = +(r.total_matches || 0), lines = +(r.total_lines || 0), files = +(r.files_matched || 0);
    const more = r.truncated ? '+' : '';
    if (offset === 0) {
      page.summary.textContent = matches
        ? `${matches}${more} matches on ${lines}${more} lines in ${files}${more} files for “${q}”`
        : `no matches for “${q}”`;
      page.summary.title = page.summary.textContent;
      status(`full-text search: ${matches}${more} matches (${ms} ms)`);
      if (r.truncated) {
        const notice = document.createElement('div');
        notice.className = 'notice';
        notice.textContent = 'Too many matches: only some of them were looked at. A longer query narrows them down.';
        page.body.appendChild(notice);
      }
    }
    for (const file of r.files || []) {
      if (!page.last || page.last.path !== file.path) {
        const head = document.createElement('a');
        head.className = 'text-file';
        head.href = '#' + file.path;
        head.textContent = file.path;
        if ((file.file_id ?? 0) < 0) {  // proto3 JSON leaves out a 0
          head.classList.add('unindexed');
          head.title = 'not in the symbol index: opens without annotations';
        } else {
          head.classList.add('k-' + (file.kind || 'SOURCE').toLowerCase());
        }
        page.body.appendChild(head);
        page.last = { path: file.path };
      }
      for (const hit of file.lines || []) page.body.appendChild(textLine(file.path, hit));
    }
    if (r.next_offset) {
      const button = document.createElement('button');
      button.className = 'more';
      button.textContent = `more (${lines - r.next_offset} lines)`;
      button.addEventListener('click', () => {
        button.disabled = true;
        loadTextPage(q, caseSensitive, r.next_offset, page).then(() => button.remove());
      });
      page.body.appendChild(button);
    }
  }

  // One matching line: its number and its text, the matches marked; the
  // whole row goes to the line.  Spans are bytes into the UTF-8 text.
  function textLine(path, hit) {
    const a = document.createElement('a');
    a.className = 'text-hit';
    a.href = '#' + path + ':' + hit.line;
    const ln = document.createElement('span');
    ln.className = 'ln';
    ln.textContent = hit.line;
    a.appendChild(ln);
    const text = document.createElement('span');
    text.className = 'text';
    const clip = () => {
      const el = document.createElement('span');
      el.className = 'clip';
      el.textContent = '…';
      return el;
    };
    if (hit.text_offset) text.appendChild(clip());
    const bytes = encoder.encode(hit.text || '');
    let pos = 0;
    for (const s of hit.spans || []) {
      const b = s.begin || 0, e = s.end || 0;
      if (b < pos || e <= b) continue;
      if (b > pos) text.appendChild(document.createTextNode(decoder.decode(bytes.subarray(pos, b))));
      const mark = document.createElement('mark');
      mark.textContent = decoder.decode(bytes.subarray(b, e));
      text.appendChild(mark);
      pos = e;
    }
    if (pos < bytes.length) text.appendChild(document.createTextNode(decoder.decode(bytes.subarray(pos))));
    if (hit.clipped_end) text.appendChild(clip());
    a.appendChild(text);
    return a;
  }

  // ---- navigation ------------------------------------------------------

  function parseHash() {
    const raw = window.location.hash.slice(1);
    if (raw.startsWith('?')) {
      const params = new URLSearchParams(raw.slice(1));
      return { text: params.get('text') || '', caseSensitive: params.get('case') !== 'insensitive' };
    }
    const h = decodeURIComponent(raw);
    if (!h) return null;
    const m = h.match(/^(.*?)(?::(\d+))?$/);
    return { path: m[1], line: m[2] ? +m[2] : 0 };
  }

  function onHash() {
    const target = parseHash();
    if (!target) return;
    if (target.text !== undefined) {
      if (target.text) showTextResults(target.text, target.caseSensitive);
      return;
    }
    if (!target.path) return;
    openFile(target.path, target.line);
  }

  // n / p: the next / previous occurrence of the selected symbol -- after
  // the one n/p last went to while that is still in view, else after the
  // middle of the view.  (Measuring from the middle every time made p find
  // the occurrence it had just centred, and never get past it.)
  function stepHighlighted(direction) {
    if (state.selected === null) return;
    const els = state.tokens.get(state.selected) || [];
    if (!els.length) return;
    const view = $('code').getBoundingClientRect();
    let index = els.indexOf(state.stepEl);
    if (index >= 0) {
      const r = state.stepEl.getBoundingClientRect();
      if (r.bottom < view.top || r.top > view.bottom) index = -1;
    }
    if (index >= 0) {
      index += direction;
    } else {
      const y = (view.top + view.bottom) / 2;
      if (direction > 0) {
        index = els.findIndex((el) => el.getBoundingClientRect().top > y);
        if (index < 0) index = 0;
      } else {
        for (let i = els.length - 1; i >= 0 && index < 0; i--)
          if (els[i].getBoundingClientRect().bottom < y) index = i;
        if (index < 0) index = els.length - 1;
      }
    }
    index = (index + els.length) % els.length;
    if (state.stepEl) state.stepEl.classList.remove('cur');
    state.stepEl = els[index];
    state.stepEl.classList.add('cur');
    els[index].scrollIntoView({ block: 'center' });
    status(`occurrence ${index + 1} of ${els.length}`);
  }

  // ---- init ------------------------------------------------------------

  async function init() {
    try {
      state.repo = await getJson('/api/repo');
      $('repo-name').textContent = state.repo.root.split('/').filter(Boolean).pop() || state.repo.root;
      const head = state.repo.head_commit ? state.repo.head_commit.slice(0, 12) : '';
      $('repo-head').textContent = [state.repo.head_ref ? state.repo.head_ref.replace(/^refs\/heads\//, '') : '', head].filter(Boolean).join(' @ ');
      const st = state.repo.stats || {};
      let line = `${st.files || 0} files, ${st.symbols || 0} symbols, ${st.occurrences || 0} occurrences (indexed ${st.imported_at || '?'})`;
      if (hasCoverage()) {
        initCoverage();
        line += ` · coverage ${coverageSummary(state.repo.coverage.totals || {})}`;
      }
      status(line);
    } catch (e) {
      status(e.message, true);
    }
    initSplitters();
    await loadDir('', $('tree'));
    onHash();
    window.addEventListener('hashchange', onHash);

    $('code').addEventListener('click', (ev) => {
      const inc = ev.target.closest('a.inc');
      if (inc) {
        // One candidate is a plain link; several are chosen from in the panel.
        if (!inc.include || ev.ctrlKey || ev.metaKey || ev.shiftKey) return;
        ev.preventDefault();
        openIncludePanel(inc);
        return;
      }
      const tok = ev.target.closest('.tok');
      if (!tok) return;
      ev.preventDefault();
      const target = (ev.ctrlKey || ev.metaKey) && definitionOf(tok);
      if (target) {
        window.location.hash = target;
        return;
      }
      openSymbolPanel(tok);
    });
    $('panel-close').addEventListener('click', closePanel);
    document.addEventListener('click', (ev) => {
      if (!ev.target.closest('#search-results') && !ev.target.closest('#search'))
        $('search-results').hidden = true;
    });
    $('search').addEventListener('input', () => {
      clearTimeout(searchTimer);
      searchTimer = setTimeout(runSearch, 150);
    });
    $('search').addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') {
        // Enter searches the text; a symbol is a click in the dropdown.
        const q = $('search').value.trim();
        clearTimeout(searchTimer);
        ++searchSeq;
        $('search-results').hidden = true;
        if (!q) return;
        const hash = textHash(q, state.caseSensitive);
        if (window.location.hash === hash) showTextResults(q, state.caseSensitive);
        else window.location.hash = hash;
      } else if (ev.key === 'Escape') {
        $('search-results').hidden = true;
        $('search').blur();
      }
    });
    document.addEventListener('keydown', (ev) => {
      if (ev.target.tagName === 'INPUT') return;
      if (ev.key === '/') {
        ev.preventDefault();
        $('search').focus();
        $('search').select();
      } else if (ev.key === 'Escape') {
        closePanel();
      } else if (ev.ctrlKey || ev.metaKey || ev.altKey) {
        // leave the browser's own shortcuts alone
      } else if (ev.key === 'n') {
        stepHighlighted(+1);
      } else if (ev.key === 'p') {
        stepHighlighted(-1);
      } else if (ev.key === 'u') {
        stepUncovered(+1);
      } else if (ev.key === 'U') {
        stepUncovered(-1);
      }
    });
  }

  init();
})();
