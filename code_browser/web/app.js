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
          .find((d) => d.querySelector(':scope > summary').textContent === parts[i]);
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

  // ---- files -----------------------------------------------------------

  async function openFile(path, line) {
    if (state.path === path) {
      goToLine(line);
      return;
    }
    status(`loading ${path}…`);
    let file, ann, includes;
    try {
      [file, ann, includes] = await Promise.all([
        getBytes(fileUrl(path)),
        getJson(`/api/annotations?path=${encodeURIComponent(path)}`).catch((e) => {
          if (String(e.message).startsWith('404')) return { spans: [], symbols: [] };
          throw e;
        }),
        // Links only: a file without them is still a file.
        getJson(`/api/includes?path=${encodeURIComponent(path)}`).catch(() => ({})),
      ]);
    } catch (e) {
      status(e.message, true);
      return;
    }
    state.path = path;
    state.bytes = file.bytes;
    $('file-title').textContent = path;
    const banner = $('banner');
    if (file.headers.get('X-Newer-Than-Index')) {
      banner.textContent = 'This file changed after the index was built; annotations may be shifted.';
      banner.hidden = false;
    } else {
      banner.hidden = true;
    }
    render(file.bytes, ann, includes.includes);
    // The panel outlives the file (that is how a reference list is walked):
    // what it selected is highlighted here as well.
    if (state.selected !== null) selectSymbol(state.selected);
    document.title = path.split('/').pop() + ' – code browser';
    status(`${path} — ${(ann.spans || []).length} annotated tokens`);
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
    const columns = document.createElement('div');
    columns.className = 'columns';
    const details = document.createElement('div');
    const refs = document.createElement('div');
    columns.appendChild(details);
    columns.appendChild(refs);
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
    for (const file of refs.files || []) {
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

  let searchTimer = null;
  async function runSearch() {
    const q = $('search').value.trim();
    const box = $('search-results');
    if (!q) {
      box.hidden = true;
      return;
    }
    let result;
    try {
      result = await getJson(`/api/search?q=${encodeURIComponent(q)}&limit=20`);
    } catch (e) {
      box.textContent = e.message;
      box.hidden = false;
      return;
    }
    box.textContent = '';
    const hits = result.hits || [];
    if (!hits.length) {
      box.textContent = 'no symbols';
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

  // ---- navigation ------------------------------------------------------

  function parseHash() {
    const h = decodeURIComponent(window.location.hash.slice(1));
    if (!h) return null;
    const m = h.match(/^(.*?)(?::(\d+))?$/);
    return { path: m[1], line: m[2] ? +m[2] : 0 };
  }

  function onHash() {
    const target = parseHash();
    if (!target || !target.path) return;
    openFile(target.path, target.line);
  }

  function stepHighlighted(direction) {
    if (state.selected === null) return;
    const els = state.tokens.get(state.selected) || [];
    if (!els.length) return;
    const view = $('code').getBoundingClientRect();
    const y = (view.top + view.bottom) / 2;
    let index = els.findIndex((el) => el.getBoundingClientRect().top > y);
    if (direction < 0) index = (index < 0 ? els.length : index) - 1;
    else if (index < 0) index = 0;
    while (index < 0) index += els.length;
    els[index % els.length].scrollIntoView({ block: 'center' });
  }

  // ---- init ------------------------------------------------------------

  async function init() {
    try {
      state.repo = await getJson('/api/repo');
      $('repo-name').textContent = state.repo.root.split('/').filter(Boolean).pop() || state.repo.root;
      const head = state.repo.head_commit ? state.repo.head_commit.slice(0, 12) : '';
      $('repo-head').textContent = [state.repo.head_ref ? state.repo.head_ref.replace(/^refs\/heads\//, '') : '', head].filter(Boolean).join(' @ ');
      const st = state.repo.stats || {};
      status(`${st.files || 0} files, ${st.symbols || 0} symbols, ${st.occurrences || 0} occurrences (indexed ${st.imported_at || '?'})`);
    } catch (e) {
      status(e.message, true);
    }
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
        const first = $('search-results').querySelector('a.hit');
        if (first) {
          window.location.hash = first.getAttribute('href').slice(1);
          $('search-results').hidden = true;
        }
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
      } else if (ev.key === 'n') {
        stepHighlighted(+1);
      } else if (ev.key === 'p') {
        stepHighlighted(-1);
      }
    });
  }

  init();
})();
