'use strict';
// Fabric data collector for `mcdma monitor`, self-testable with `node lib/collect.js [n]`.
// Light tick (every pollMs): interface counters + operstate + one-ping RTTs per link.
// Heavy probe (every heavyEveryTicks): nvidia-smi + local inference-engine HTTP APIs.
// Everything it runs on the remote hosts is read-only.

const { spawn } = require('child_process');
const fs = require('fs');

const NETSTAT = '/usr/sbin/netstat';
const IFCONFIG = '/sbin/ifconfig';
const SSH = '/usr/bin/ssh';

const ENGINE_PORTS = { 8000: 'vLLM', 8080: 'llama.cpp', 11434: 'Ollama', 30000: 'SGLang' };

function run(cmd, args, timeoutMs = 3500) {
  return new Promise((resolve) => {
    let out = '', settled = false;
    const done = (v) => { if (!settled) { settled = true; clearTimeout(timer); resolve(v); } };
    let p;
    try { p = spawn(cmd, args, { stdio: ['ignore', 'pipe', 'ignore'] }); }
    catch { return resolve(null); }
    const timer = setTimeout(() => { done(null); try { p.kill('SIGKILL'); } catch {} }, timeoutMs);
    p.stdout.on('data', (d) => { out += d; });
    p.on('error', () => done(null));
    p.on('close', (code) => done(code === 0 ? out : null));
  });
}

// `netstat -ibn` — first row per interface (the <Link#..> row) carries the totals.
// Rows are parsed right-anchored: the Address column is empty for interfaces
// without a MAC (lo0, bridges), which shifts the left-hand columns.
function parseNetstat(out) {
  const m = {};
  if (!out) return m;
  for (const line of out.split('\n').slice(1)) {
    const f = line.trim().split(/\s+/);
    const n = f.length;
    if (n < 10 || m[f[0]] || f[n - 1] === 'Coll') continue;
    m[f[0]] = { rx: Number(f[n - 5]) || 0, tx: Number(f[n - 2]) || 0 };
  }
  return m;
}

// /proc/net/dev — rx_bytes is field 0, tx_bytes field 8 after the colon.
function parseProcNetDev(out) {
  const m = {};
  if (!out) return m;
  for (const line of out.split('\n')) {
    const c = line.indexOf(':');
    if (c < 0) continue;
    const f = line.slice(c + 1).trim().split(/\s+/);
    if (f.length < 16) continue;
    m[line.slice(0, c).trim()] = { rx: Number(f[0]) || 0, tx: Number(f[8]) || 0 };
  }
  return m;
}

function parseStates(out) {
  const m = {};
  if (!out) return m;
  for (const line of out.split('\n')) {
    const c = line.indexOf(':');
    if (c < 0) continue;
    m[line.slice(0, c).trim()] = line.slice(c + 1).trim();
  }
  return m;
}

function parsePings(out) {
  const m = {};
  if (!out) return m;
  for (const mm of out.matchAll(/^P (\S+) ([0-9.]+)?/gm)) m[mm[1]] = mm[2] ? Number(mm[2]) : null;
  return m;
}

// Split "===NAME" delimited output into {NAME: body} sections.
function sections(out) {
  const parts = (out || '').split(/^===([A-Z]+)\s*$/m);
  const sec = { _: parts[0] || '' };
  for (let i = 1; i < parts.length; i += 2) sec[parts[i]] = parts[i + 1] || '';
  return sec;
}

function lightCmd(host) {
  const pings = host.pings || [];
  const pingPart = pings.length
    ? `echo '===PING'; ` + pings.map((ip) =>
        `echo "P ${ip} $(ping -c 1 -W ${host.os === 'mac' ? 800 : 1} ${ip} 2>/dev/null | grep -o 'time=[0-9.]*' | cut -d= -f2)"`
      ).join('; ')
    : '';
  if (host.os === 'mac') return `netstat -ibn; ${pingPart}`;
  return `cat /proc/net/dev; echo '===ST'; ` +
    `for i in /sys/class/net/*; do echo "$(basename $i):$(cat $i/operstate)"; done; ${pingPart}`;
}

const HEAVY_CMD =
  `echo '===GPU'; nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>/dev/null; ` +
  `echo '===APPS'; nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader 2>/dev/null; ` +
  `for p in $(nvidia-smi --query-compute-apps=pid --format=csv,noheader 2>/dev/null); do echo "CMD $(ps -o args= -p $p 2>/dev/null | head -c 400)"; done; ` +
  `echo '===HTTP'; ` + Object.keys(ENGINE_PORTS).map((p) =>
    `echo "PORT ${p}"; curl -m 1 -s localhost:${p}/$([ ${p} = 11434 ] && echo api/ps || echo v1/models) | head -c 3000; echo`
  ).join('; ') + `; ` +
  `echo '===METRICS'; ` + [8080, 8000, 30000].map((p) =>
    `echo "MPORT ${p}"; curl -m 1 -s localhost:${p}/metrics | grep -Ei 'prompt_tokens|predicted_tokens|generation_tokens' | grep -v '^#' | head -20; echo`
  ).join('; ');

function parseHeavy(out) {
  const sec = sections(out);
  const gpuLine = (sec.GPU || '').trim().split(/\s*,\s*/);
  const gpu = gpuLine.length === 3
    ? { utilPct: Number(gpuLine[0]) || 0, usedMiB: Number(gpuLine[1]) || 0, totalMiB: Number(gpuLine[2]) || 0 }
    : null;
  const apps = [];
  for (const line of (sec.APPS || '').split('\n')) {
    const f = line.split(',').map((s) => s.trim());
    if (f.length >= 2 && /^\d+$/.test(f[0])) apps.push({ pid: f[0], name: f[1], memMiB: Number((f[2] || '').replace(/\D/g, '')) || 0 });
  }
  const cmdlines = (sec.APPS || '').split('\n').filter((l) => l.startsWith('CMD ')).map((l) => l.slice(4));
  const http = {}; // port -> parsed JSON or null
  const httpRaw = {};
  for (const mm of (sec.HTTP || '').matchAll(/^PORT (\d+)\s*$(.*?)^$/gms)) {
    const body = mm[2].trim();
    httpRaw[mm[1]] = body;
    try { http[mm[1]] = body ? JSON.parse(body) : null; } catch { http[mm[1]] = null; }
  }
  const metrics = {}; // port -> {name: value}
  for (const mm of (sec.METRICS || '').matchAll(/^MPORT (\d+)\s*$(.*?)^$/gms)) {
    const m = {};
    for (const line of mm[2].split('\n')) {
      const f = line.trim().split(/\s+/);
      if (f.length === 2 && /^[-\w:]+$/.test(f[0])) m[f[0]] = Number(f[1]) || 0;
    }
    metrics[mm[1]] = m;
  }
  return { gpu, apps, cmdlines, http, httpRaw, metrics };
}

function modelFromCmdline(cmd) {
  if (!cmd) return null;
  const gguf = cmd.match(/[\w./~-]+\.gguf/i);
  if (gguf) return gguf[0].split('/').pop();
  const safetensors = cmd.match(/[\w./~-]+\.safetensors/i);
  if (safetensors) return safetensors[0].split('/').pop();
  const flag = cmd.match(/(?:--model|-m)\s+([\w./~-]+)/);
  if (flag) return flag[1].split('/').pop();
  if (/kokoro/i.test(cmd)) return 'Kokoro';
  return null;
}

function deriveInference(hostId, heavy, prev, dt) {
  if (!heavy) return null;
  const { apps, cmdlines, http, metrics } = heavy;

  let engine = null, model = null;
  for (const [port, name] of Object.entries(ENGINE_PORTS)) {
    const j = http[port];
    if (!j) continue;
    if (port === '11434' && j.models && j.models[0]) { engine = name; model = j.models[0].name; break; }
    if (j.data && j.data[0] && j.data[0].id) { engine = name; model = j.data[0].id; break; }
  }
  const cmd = cmdlines.find(modelFromCmdline);
  const cmdlineModel = modelFromCmdline(cmd);
  let engineKnown = !!engine;
  if (!engine && apps.length) {
    const n = (apps[0].name || '').toLowerCase().split('/').pop();
    engine = n.includes('ollama') ? 'Ollama'
      : n.includes('llama') ? 'llama.cpp'
      : n.includes('vllm') ? 'vLLM'
      : n.includes('sglang') ? 'SGLang'
      : n.includes('python') ? 'Python · CUDA'
      : (apps[0].name || '').split('/').pop();
  }
  if (!engine && !apps.length) return null;
  model = model || cmdlineModel || null;

  // Rates: llama.cpp exports window-rate gauges; vLLM/SGLang export counters we delta ourselves.
  let promptTps = null, decodeTps = null;
  const m8k = metrics[8080] || {};
  if (m8k.prompt_tokens_seconds != null || m8k.predicted_tokens_seconds != null) {
    promptTps = m8k.prompt_tokens_seconds || 0;
    decodeTps = m8k.predicted_tokens_seconds || 0;
  } else {
    for (const p of [8000, 30000]) {
      const m = metrics[p] || {};
      const pr = m['vllm:prompt_tokens_total'] ?? m.prompt_tokens_total;
      const de = m['vllm:generation_tokens_total'] ?? m.generation_tokens_total;
      if (pr == null && de == null) continue;
      const key = `${hostId}:${p}`;
      const pp = prev && prev[key];
      if (pp && dt > 0) {
        promptTps = Math.max(0, (pr - pp.prompt) / dt);
        decodeTps = Math.max(0, (de - pp.decode) / dt);
      }
      prev && (prev[key] = { prompt: pr || 0, decode: de || 0 });
      break;
    }
  }

  return {
    engine, engineKnown, model,
    vramMiB: apps.reduce((a, x) => a + (x.memMiB || 0), 0) || null,
    gpuUtilPct: heavy.gpu ? heavy.gpu.utilPct : null,
    promptTps, decodeTps
  };
}

class FabricCollector {
  constructor(config) {
    this.config = config;
    this.prev = null;
    this.tickN = 0;
    this.inference = {};      // hostId -> derived inference (cached from heavy probe)
    this.heavyBusy = false;
    this.heavyPrev = { t: 0, counters: {} };
    // Per-process mux dir under /tmp: the macOS per-user temp dir is so long
    // that a ControlPath under it exceeds the Unix-domain socket path limit.
    // Wiped at start so a stale socket from a previous run can't poison us.
    this.cmDir = `/tmp/fabric-mon-${process.pid}`;
    fs.rmSync(this.cmDir, { recursive: true, force: true });
    fs.mkdirSync(this.cmDir, { recursive: true });
    this.sshBase = [
      '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4', '-o', 'ServerAliveInterval=15',
      '-o', 'ControlMaster=auto', '-o', `ControlPath=${this.cmDir}/%r@%h-%p`,
      '-o', 'ControlPersist=600'
    ];
  }

  ssh(host, cmd, timeoutMs) {
    // OpenSSH options are first-wins, so the no-mux retry overrides go first.
    return run(SSH, [...this.sshBase, host, cmd], timeoutMs).then((out) =>
      out !== null ? out : run(SSH, ['-o', 'ControlPath=none', '-o', 'ControlMaster=no', ...this.sshBase, host, cmd], timeoutMs));
  }

  async runHeavy() {
    const cfg = this.config;
    const now = Date.now();
    const dtHeavy = this.heavyPrev.t ? (now - this.heavyPrev.t) / 1000 : 0;
    await Promise.all(Object.entries(cfg.hosts).map(async ([id, h]) => {
      if (h.os === 'mac') return;
      const out = await this.ssh(h.ssh, HEAVY_CMD, 12000);
      this.inference[id] = out ? deriveInference(id, parseHeavy(out), this.heavyPrev.counters, dtHeavy) : null;
    }));
    this.heavyPrev.t = now;
  }

  async tick() {
    const cfg = this.config;
    const now = Date.now();
    this.tickN++;
    const hostIds = Object.keys(cfg.hosts || {});
    const [localOut, ...remoteOuts] = await Promise.all([
      run(NETSTAT, ['-ibn']),
      ...hostIds.map((id) => this.ssh(cfg.hosts[id].ssh, lightCmd(cfg.hosts[id])))
    ]);
    const localIf = parseNetstat(localOut);

    const hosts = {};
    hostIds.forEach((id, i) => {
      const out = remoteOuts[i];
      if (!out) { hosts[id] = { up: false, ifaces: {}, states: {}, pings: {} }; return; }
      const sec = sections(out);
      hosts[id] = {
        up: true,
        ifaces: cfg.hosts[id].os === 'mac' ? parseNetstat(sec._) : parseProcNetDev(sec._),
        states: parseStates(sec.ST),
        pings: parsePings(sec.PING)
      };
    });

    if (cfg.heavyEveryTicks && this.tickN % cfg.heavyEveryTicks === 1 && !this.heavyBusy) {
      this.heavyBusy = true;
      this.runHeavy().catch(() => {}).finally(() => { this.heavyBusy = false; });
    }

    // When mcdma runs somewhere other than the Mac with the card, that Mac's
    // counters come over ssh; when running on it, local wins.
    const studioIf = hosts.studio && hosts.studio.up ? hosts.studio.ifaces : null;

    // Active-carrier set for Mac-side candidate ifaces (e.g. the CX5 ports),
    // from local ifconfig — an interface exists in netstat even with no carrier.
    let macActive = null;
    const macLinks = cfg.links.filter((l) => (l.source || {}).type === 'mac-ifaces');
    if (macLinks.length) {
      const names = [...new Set(macLinks.flatMap((l) => l.source.ifaces || []))];
      const out = await run(IFCONFIG, names);
      macActive = new Set();
      if (out) {
        let curIf = null;
        for (const line of out.split('\n')) {
          const m = line.match(/^(\w[\w\d]*):/);
          if (m) curIf = m[1];
          else if (curIf && /status: active/.test(line)) macActive.add(curIf);
        }
      }
    }

    const prev = this.prev;
    const dt = prev ? Math.max(0.2, (now - prev.t) / 1000) : 1;
    const links = {};
    const raw = {};

    for (const l of cfg.links) {
      const src = l.source || {};
      let cur = null, hostUp = true, ifUp = true;

      if (src.type === 'local') {
        cur = localIf[src.iface] || (studioIf ? studioIf[src.iface] : null) || null;
        if (!cur && l.optional) { links[l.id] = { state: 'absent', rx: 0, tx: 0, speedGbps: l.speedGbps, rttMs: null }; raw[l.id] = null; continue; }
      } else if (src.type === 'ssh') {
        let h = hosts[src.host], iface = src.iface;
        if ((!h || !h.up) && src.fallback) { h = hosts[src.fallback.host]; iface = src.fallback.iface; }
        hostUp = !!(h && h.up);
        if (h && h.up) {
          cur = h.ifaces[iface] || null;
          ifUp = h.states[iface] !== 'down';
        }
      } else if (src.type === 'mac-ifaces') {
        let rx = 0, tx = 0, any = false;
        for (const i of src.ifaces || []) {
          const local = localIf[i] || null;
          const c = local || (studioIf ? studioIf[i] : null);
          if (!c) continue;
          const active = local
            ? (macActive ? macActive.has(i) : false)
            : (c.rx + c.tx > 0); // remote fallback: cumulative traffic means it came up
          if (active) { any = true; rx += c.rx; tx += c.tx; }
        }
        if (any) cur = { rx, tx };
      }

      const p = prev && prev.raw[l.id];
      let rx = 0, tx = 0, state;
      if (!hostUp || (src.type !== 'none' && src.type !== 'mac-ifaces' && !cur)) state = 'down';
      else if (src.type === 'mac-ifaces' && !cur) state = 'standby';
      else if (!ifUp) state = 'down';
      else {
        state = 'up';
        if (p && cur) {
          rx = Math.max(0, cur.rx - p.rx) / dt;
          tx = Math.max(0, cur.tx - p.tx) / dt;
        }
      }
      const lat = l.latency || {};
      const rttMs = lat.host && hosts[lat.host] && hosts[lat.host].up
        ? (hosts[lat.host].pings[lat.ip] ?? null) : null;
      if (l.swap) [rx, tx] = [tx, rx]; // counters come from the far end of the link
      links[l.id] = { state, rx, tx, speedGbps: l.speedGbps, rttMs };
      raw[l.id] = cur;
    }

    const nodes = { studio: { up: true } }; // the machine you are looking at (or the ssh'd Mac)
    for (const [id, h] of Object.entries(hosts)) if (!nodes[id]) nodes[id] = { up: h.up };
    for (const n of cfg.nodes) {
      if (nodes[n.id]) continue; // studio and ssh hosts are decided above
      nodes[n.id] = {
        up: cfg.links.some((l) => (l.from === n.id || l.to === n.id) && links[l.id].state === 'up')
      };
    }

    this.prev = { t: now, raw };
    return { t: now, links, nodes, inference: { ...this.inference } };
  }

  dispose() {
    for (const h of Object.values(this.config.hosts || {})) {
      run(SSH, [...this.sshBase, '-O', 'exit', h.ssh], 2000);
    }
    try { fs.rmSync(this.cmDir, { recursive: true, force: true }); } catch {}
  }
}

module.exports = { FabricCollector, parseNetstat, parseProcNetDev, parseStates, parseHeavy, deriveInference };

if (require.main === module) {
  const n = Number(process.argv[2]) || 2;
  const config = require('../config.json');
  const c = new FabricCollector(config);
  (async () => {
    for (let i = 0; i < n; i++) {
      const t = await c.tick();
      console.log(JSON.stringify(t, null, 2));
      if (i < n - 1) await new Promise((r) => setTimeout(r, 1200));
    }
    c.dispose();
  })();
}
