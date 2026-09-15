'use strict';
// Spark (DGX / Linux peer) probe over ssh: identity, ConnectX ports with MACs,
// speeds, cable EEPROM identity, RDMA devices, GID table, neighbours, GPU and
// the tools available for tests. Read-only.
const { sections, normMac, normIp6, eui64 } = require('./parse');

const PROBE = `
echo '===ID'; hostname; id -u; grep -E '^(PRETTY_NAME|ID)=' /etc/os-release 2>/dev/null; uname -m
echo '===LINKS'; ip -o link show 2>/dev/null
echo '===RDMA'; rdma link 2>/dev/null
echo '===GPU'; nvidia-smi -L 2>/dev/null
echo '===TOOLS'; for t in ethtool ip rdma ibv_devinfo ibv_rc_pingpong ib_write_lat; do printf '%s=%s\\n' "$t" "$(command -v $t 2>/dev/null)"; done
for p in /usr/local/libexec/mcdma/verbs-peer /usr/local/bin/verbs-peer /opt/mcdma/verbs-peer; do [ -x "$p" ] && echo "PEER=$p"; done
echo '===PERSIST'; ls -1 /etc/systemd/system/mcdma-neighbours.service /etc/NetworkManager/dispatcher.d/90-mcdma-neighbours 2>/dev/null; echo '---CONF'; cat /etc/mcdma/neighbours.conf 2>/dev/null
for i in $(rdma link 2>/dev/null | awk '{for(k=1;k<=NF;k++) if($k=="netdev") print $(k+1)}' | sort -u); do
  echo "===PORT $i"
  ethtool $i 2>/dev/null | grep -E 'Speed|Link detected'
  echo '---MODULE'; ethtool -m $i 2>/dev/null | grep -E '^\\s*(Vendor name|Vendor PN|Vendor SN|Identifier|Length \\(Copper|Connector)'
  echo '---ADDR'; ip -6 addr show dev $i 2>/dev/null | grep inet6
  echo '---NEIGH'; ip -6 neigh show dev $i 2>/dev/null
  echo '---STATS'; ethtool -S $i 2>/dev/null | grep -E '^\\s+(rx_packets_phy|tx_packets_phy|rx_bytes_phy|tx_bytes_phy):'
  dev=$(rdma link 2>/dev/null | awk -v n="$i" '{for(k=1;k<=NF;k++) if($k=="netdev" && $(k+1)==n) print $2}' | head -1 | cut -d/ -f1)
  echo "---DEV $dev"
  for g in 0 1 2 3 4 5; do f=/sys/class/infiniband/$dev/ports/1/gids/$g; [ -r "$f" ] && echo "GID $g $(cat $f) $(cat /sys/class/infiniband/$dev/ports/1/gid_attrs/types/$g 2>/dev/null)"; done
done
`;

function parsePort(name, body) {
  const p = { iface: name, speedGbps: null, link: false, cable: null, addrs: [], neighbours: [], stats: {}, rdmaDevice: null, gids: [] };
  const [head, ...rest] = body.split(/^---/m);
  const sp = head.match(/Speed:\s*(\d+)Mb\/s/); if (sp) p.speedGbps = Number(sp[1]) / 1000;
  p.link = /Link detected:\s*yes/.test(head);
  for (const part of rest) {
    const nl = part.indexOf('\n');
    const tag = part.slice(0, nl).trim(), text = part.slice(nl + 1);
    if (tag === 'MODULE') {
      const g = (k) => ((text.match(new RegExp(`${k}\\s*:\\s*(.+)$`, 'm')) || [])[1] || '').trim() || null;
      const vendor = g('Vendor name'), pn = g('Vendor PN'), sn = g('Vendor SN');
      if (vendor || pn || sn) p.cable = { vendor, pn, sn, type: g('Identifier'), length: g('Length \\(Copper or Active cable\\)'), connector: g('Connector') };
    } else if (tag === 'ADDR') {
      for (const m of text.matchAll(/inet6 ([0-9a-f:]+)\/(\d+)/gi)) p.addrs.push(normIp6(m[1]));
    } else if (tag === 'NEIGH') {
      for (const line of text.split('\n')) {
        const m = line.match(/^(\S+)\s+(?:lladdr\s+(\S+)\s+)?(.*)$/);
        if (m && m[1]) p.neighbours.push({ addr: normIp6(m[1]), lladdr: normMac(m[2]), state: (m[3] || '').trim(), permanent: /PERMANENT/.test(m[3] || '') });
      }
    } else if (tag === 'STATS') {
      for (const m of text.matchAll(/(\w+):\s*(\d+)/g)) p.stats[m[1]] = Number(m[2]);
    } else if (tag.startsWith('DEV')) {
      p.rdmaDevice = tag.slice(3).trim() || null;
      for (const m of text.matchAll(/^GID (\d+) (\S+) (.*)$/gm)) {
        const addr = normIp6(m[2]);
        if (addr && addr !== '::') p.gids.push({ index: Number(m[1]), addr, type: m[3].trim() });
      }
    }
  }
  return p;
}

function parse(out) {
  const s = sections(out);
  const id = (s.ID || '').trim().split('\n');
  const os = ((s.ID || '').match(/^PRETTY_NAME="?([^"\n]+)"?/m) || [])[1] || null;
  const macs = {};
  for (const line of (s.LINKS || '').split('\n')) {
    const m = line.match(/^\d+:\s+([^:@]+)[^\n]*link\/ether\s+([0-9a-f:]+)/);
    if (m) macs[m[1]] = normMac(m[2]);
  }
  const rdmaLinks = [];
  for (const m of (s.RDMA || '').matchAll(/^link (\S+)\/(\d+) state (\S+) physical_state (\S+)(?: netdev (\S+))?/gm)) {
    rdmaLinks.push({ device: m[1], port: Number(m[2]), state: m[3], physical: m[4], netdev: m[5] || null });
  }
  const tools = {};
  for (const m of (s.TOOLS || '').matchAll(/^(\w+)=(.*)$/gm)) tools[m[1]] = m[2] || null;
  const peers = [...(s.TOOLS || '').matchAll(/^PEER=(.+)$/gm)].map((m) => m[1]);
  const [persistList, persistConf] = (s.PERSIST || '').split('---CONF');
  const ports = [];
  for (const [k, v] of Object.entries(s)) {
    if (!k.startsWith('PORT ')) continue;
    const p = parsePort(k.slice(5), v);
    p.mac = macs[p.iface] || null;
    p.eui64 = p.mac ? eui64(p.mac) : null;
    const v2 = p.gids.find((g) => /RoCE v2/.test(g.type) && g.addr === p.eui64) || p.gids.find((g) => /RoCE v2/.test(g.type));
    p.gidIndex = v2 ? v2.index : null;
    p.gid = v2 ? v2.addr : null;
    ports.push(p);
  }
  // A CX7 on the Spark shows each physical port twice (two PCI functions). Two
  // interfaces on the same host that read the same cable serial are one port.
  const bySn = {};
  for (const p of ports) if (p.cable && p.cable.sn) (bySn[p.cable.sn] = bySn[p.cable.sn] || []).push(p);
  for (const group of Object.values(bySn)) {
    group.sort((a, b) => (/^enP/.test(a.iface) ? 1 : 0) - (/^enP/.test(b.iface) ? 1 : 0) || a.iface.localeCompare(b.iface));
    group.forEach((p, i) => { p.primary = i === 0; p.views = group.map((x) => x.iface); });
  }
  for (const p of ports) if (p.primary === undefined) { p.primary = true; p.views = [p.iface]; }
  ports.sort((a, b) => a.iface.localeCompare(b.iface));
  return {
    ok: true, reachable: true, hostname: id[0] || null, root: id[1] === '0', os, arch: id[id.length - 1] || null,
    gpus: (s.GPU || '').trim().split('\n').filter(Boolean).map((l) => l.replace(/\s*\(UUID.*$/, '')),
    tools, peerTools: peers, persist: { service: /mcdma-neighbours\.service/.test(persistList || ''), dispatcher: /dispatcher/.test(persistList || ''), conf: (persistConf || '').trim() || null },
    rdmaLinks, ports
  };
}

async function probe(host) {
  const r = await host.sh(PROBE, { timeoutMs: 25000 });
  if (r.code !== 0 && !r.out.includes('===ID')) {
    return { ok: false, reachable: false, error: (r.err || '').trim().split('\n').pop() || `ssh ${host.alias} failed` };
  }
  const info = parse(r.out);
  info.host = host.alias; info.at = Date.now();
  return info;
}

async function testSsh(host) {
  const r = await host.sh('hostname; id -u; uname -s', { timeoutMs: 12000 });
  if (r.code !== 0) return { ok: false, error: (r.err || '').trim().split('\n').pop() || 'connection failed' };
  const [hostname, uid, sys] = r.out.trim().split('\n');
  return { ok: true, hostname, root: uid === '0', system: sys };
}

module.exports = { probe, parse, testSsh, PROBE };
