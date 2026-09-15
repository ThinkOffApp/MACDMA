'use strict';
// Small parsing helpers shared by the probes.

// Split "===NAME" (optionally "===NAME extra") delimited output into sections.
function sections(out) {
  const sec = { _: '' };
  let cur = '_';
  for (const line of (out || '').split('\n')) {
    const m = line.match(/^===([A-Z]+)(?:\s+(.*))?$/);
    if (m) { cur = m[2] ? `${m[1]} ${m[2].trim()}` : m[1]; sec[cur] = ''; continue; }
    sec[cur] += line + '\n';
  }
  return sec;
}

function jsonSafe(text, fallback = null) {
  try { return JSON.parse(text); } catch { return fallback; }
}

// MAC addresses: canonical lowercase, zero-padded ("2:a:b:..." from ndp -> "02:0a:0b:...").
function normMac(s) {
  if (!s) return null;
  const parts = String(s).trim().toLowerCase().split(':');
  if (parts.length !== 6 || parts.some((p) => !/^[0-9a-f]{1,2}$/.test(p))) return null;
  return parts.map((p) => p.padStart(2, '0')).join(':');
}

// Modified EUI-64 link-local address from a MAC (what mlx5 and the MCDMA driver use as the default GID).
function eui64(mac) {
  const m = normMac(mac);
  if (!m) return null;
  const b = m.split(':').map((x) => parseInt(x, 16));
  b[0] ^= 0x02;
  const words = [(b[0] << 8) | b[1], (b[2] << 8) | 0xff, 0xfe00 | b[3], (b[4] << 8) | b[5]];
  return 'fe80::' + words.map((w) => w.toString(16)).join(':');
}

// Expand an IPv6 address to its canonical compressed form for comparisons.
function normIp6(s) {
  if (!s) return null;
  let a = String(s).trim().toLowerCase().replace(/%.*$/, '').replace(/^\[|\]$/g, '');
  if (!/^[0-9a-f:.]+$/.test(a)) return null;
  const halves = a.split('::');
  if (halves.length > 2) return null;
  const head = halves[0] ? halves[0].split(':') : [];
  const tail = halves.length === 2 && halves[1] ? halves[1].split(':') : [];
  const fill = halves.length === 2 ? 8 - head.length - tail.length : 0;
  if (fill < 0 || (halves.length === 1 && head.length !== 8)) return null;
  const words = [...head, ...Array(fill).fill('0'), ...tail].map((w) => parseInt(w || '0', 16));
  if (words.some((w) => isNaN(w) || w > 0xffff)) return null;
  // compress the longest zero run (RFC 5952)
  let best = -1, bestLen = 0, i = 0;
  while (i < 8) {
    if (words[i] !== 0) { i++; continue; }
    let j = i; while (j < 8 && words[j] === 0) j++;
    if (j - i > bestLen && j - i >= 2) { best = i; bestLen = j - i; }
    i = j;
  }
  const hex = words.map((w) => w.toString(16));
  if (best < 0) return hex.join(':');
  const left = hex.slice(0, best).join(':'), right = hex.slice(best + bestLen).join(':');
  return `${left}::${right}`;
}

function macFromEui64(ip) {
  const full = normIp6(ip);
  if (!full || !full.startsWith('fe80::')) return null;
  const words = full.replace('fe80::', '').split(':');
  if (words.length !== 4) return null;
  const w = words.map((x) => parseInt(x, 16));
  if (((w[1] & 0xff) !== 0xff) || ((w[2] >> 8) !== 0xfe)) return null;
  const b = [((w[0] >> 8) ^ 0x02), w[0] & 0xff, w[1] >> 8, w[2] & 0xff, w[3] >> 8, w[3] & 0xff];
  return b.map((x) => x.toString(16).padStart(2, '0')).join(':');
}

function sha256File(hostShResult) { return (hostShResult || '').trim().split(/\s+/)[0] || null; }

module.exports = { sections, jsonSafe, normMac, eui64, normIp6, macFromEui64, sha256File };
