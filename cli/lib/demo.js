'use strict';
// Synthetic probe results for demo mode (screenshots, machines without the
// hardware). Shapes mirror macinfo.probe() and sparks.probe().
const { eui64 } = require('./parse');

function studio(stage = 'ready') {
  const ports = [['mcrdma0', '3:0:0', '02:aa:bb:00:00:0a'], ['mcrdma1', '3:0:1', '02:aa:bb:00:00:0b']];
  const installed = stage !== 'fresh';
  const loaded = stage === 'ready' || stage === 'unconfigured';
  const registry = loaded ? ports.map(([iface, pci]) => ({ iface, pci, portActive: true, gidLive: stage === 'ready', quarantined: false, startError: null, userQueues: true, userBlueFlame: true, blueFlame: true,
    ethernetMtu: 9000, frameMtu: 9022, transport: 'hardware RoCE v2; polled RC; static IPv6 neighbours', build: '26A428', pciePath: `${pci} mps=128 mrrs=512 ro=1 aspm=0 | 2:0:0(3:3) mps=128 mrrs=512 ro=1 aspm=0`, knobs: { maxReadRequest: 0, relaxedOrdering: false, ackEveryPacket: false }, raw: {} })) : [];
  const ifaces = {};
  if (loaded) for (const [iface, , mac] of ports) ifaces[iface] = { name: iface, flags: ['UP', 'BROADCAST', 'RUNNING', 'MULTICAST'], mtu: 9000, mac, inet6: stage === 'ready' ? [{ addr: eui64(mac), prefix: 64 }] : [], inet: [], status: null };
  const ndp = stage === 'ready' ? [
    { addr: eui64('02:cc:dd:00:00:21'), iface: 'mcrdma0', lladdr: '02:cc:dd:00:00:21', expire: 'permanent', state: 'R', permanent: true },
    { addr: eui64('02:cc:dd:00:00:11'), iface: 'mcrdma1', lladdr: '02:cc:dd:00:00:11', expire: 'permanent', state: 'R', permanent: true }] : [];
  const info = {
    ok: true, reachable: true, at: Date.now(), hostKind: 'local', hostLabel: 'this Mac', demo: true,
    os: { name: 'macOS', version: '27.0', build: '26A428', major: 27 }, chip: { brand: 'Apple M3 Ultra', memoryGiB: 256, arch: 'arm64', hostname: 'Mac-Studio' },
    sip: { raw: 'System Integrity Protection status: disabled.', state: 'disabled' }, kextConsent: 'enabled', appleSilicon: true,
    pci: { devices: [], cards: [] }, thunderbolt: [{ name: 'PCIe expansion box', vendor: null, mode: 'usb_four_v2', uid: '0x0000000000000001', deviceId: null, speed: '80 Gb/s', firmware: null, bus: 'thunderboltusb4_bus_3', receptacle: '4' }], tunnels: [],
    registry, loaded: loaded ? { loaded: true, version: '0.1.18', uuid: '00000000-0000-0000-0000-000000000018' } : { loaded: false, version: null, uuid: null },
    kext: installed ? { installed: true, version: '0.1.18', bundleId: 'org.mcdma.cx5.native', requiresMacOSMajor: 27, signature: 'ad hoc', sha256: '0000000000000000000000000000000000000000000000000000000000000001', personality: {}, match: '0x101915b3' } : { installed: false },
    provider: installed ? { present: true, sha256: '0000000000000000000000000000000000000000000000000000000000000002', conf: 'driver /usr/local/lib/rdma/libmcdma', confPresent: true, signature: 'ad hoc' } : { present: false, sha256: null, conf: null, confPresent: false, signature: null },
    rdma: loaded ? ports.map(([iface]) => ({ name: `rdma_${iface}`, transport: 'InfiniBand (0)', vendorId: '0x15b3', partId: 4121, ports: [{ port: 1, state: 'PORT_ACTIVE', active: true, linkLayer: 'Ethernet', activeMtu: 4096, maxMtu: 4096 }] })) : [],
    ifaces, ndp, tools: { installed: installed ? ['native-verbs-peer', 'fabric-keepalive', 'mcdma-set'] : [], launchDaemon: false, neighboursConf: null }, pending: [], installState: stage === 'approve' ? { version: '0.1.18', loadExit: 27, loadLog: 'Kext rejected due to system policy: must be approved in System Settings' } : null
  };
  for (const [iface, pci] of ports) info.pci.devices.push({ name: 'ConnectX-5 Ex', vendorId: '0x15b3', deviceId: '0x1019', subsystemId: '0x0008', revision: '0x0000', slot: `Thunderbolt@${pci.replace(/:/g, ',')}`, pci, card: '3:0', tunnelled: true, linkWidth: 'x4', linkSpeed: '16.0 GT/s', linkUp: true, driverAttached: loaded, supported: true });
  const enclosure = { ...info.thunderbolt[0], matched: true };
  for (const d of info.pci.devices) d.enclosure = enclosure;
  info.pci.cards = [{ id: '3:0', name: 'ConnectX-5 Ex', deviceId: '0x1019', functions: info.pci.devices, tunnelled: true, linkWidth: 'x4', linkSpeed: '16.0 GT/s', supported: true, enclosure }];
  info.ports = info.pci.devices.map((d) => {
    const reg = registry.find((r) => r.pci === d.pci) || null; const iface = reg ? ifaces[reg.iface] : null;
    return { id: reg ? reg.iface : `pci-${d.pci}`, pci: d.pci, card: d.card, cardName: d.name, deviceId: d.deviceId, enclosure, supported: true, driverAttached: !!reg, iface: reg ? reg.iface : null, mac: iface ? iface.mac : null,
      linkLocal: iface && iface.inet6[0] ? iface.inet6[0].addr : null, mtu: 9000, up: !!iface, portActive: !!reg, gidLive: reg ? reg.gidLive : false, quarantined: false, startError: null,
      rdmaDevice: reg ? `rdma_${reg.iface}` : null, rdmaActive: !!reg, rdmaMtu: 4096, neighbours: ndp.filter((n) => reg && n.iface === reg.iface), registry: reg };
  });
  info.mcdmaDevices = info.rdma;
  return info;
}

function spark(id, host, macBase, configured = true) {
  const mk = (iface, suffix, speed, sn, pn) => {
    const mac = `${macBase}:${suffix}`;
    return { iface, speedGbps: speed, link: true, cable: { vendor: pn.startsWith('MCP') ? 'Mellanox' : 'Amphenol', pn, sn, type: '0x11 (QSFP28)', length: '1m', connector: 'No separable connector' },
      addrs: [eui64(mac)], neighbours: [], stats: {}, rdmaDevice: `roce${iface.replace(/^en/, '').replace(/np\d$/, '')}`, mac, eui64: eui64(mac), gids: [{ index: 0, addr: eui64(mac), type: 'IB/RoCE v1' }, { index: 1, addr: eui64(mac), type: 'RoCE v2' }], gidIndex: 1, gid: eui64(mac), primary: true, views: [iface] };
  };
  const p0 = mk('enp1s0f0np0', id === 'spark1' ? '00:11' : '00:21', 100, id === 'spark1' ? 'DEMO00000001' : 'DEMO00000002', 'MCP1600-C001E30N');
  const p1 = mk('enp1s0f1np1', id === 'spark1' ? '00:12' : '00:22', 200, 'DEMO00000003', 'NJAAKK-AU06');
  const macMac = id === 'spark1' ? '02:aa:bb:00:00:0b' : '02:aa:bb:00:00:0a';
  if (configured) p0.neighbours.push({ addr: eui64(macMac), lladdr: macMac, state: 'PERMANENT', permanent: true });
  return { ok: true, reachable: true, id, host, hostname: id, root: true, os: 'Ubuntu 24.04.4 LTS', arch: 'aarch64', gpus: ['GPU 0: NVIDIA GB10'],
    tools: { ethtool: '/usr/sbin/ethtool', ip: '/usr/sbin/ip', rdma: '/usr/bin/rdma', ibv_devinfo: '/usr/bin/ibv_devinfo', ibv_rc_pingpong: '/usr/bin/ibv_rc_pingpong' },
    peerTools: ['/usr/local/libexec/mcdma/verbs-peer'], persist: { service: false, dispatcher: false, conf: null }, rdmaLinks: [], ports: [p0, p1], at: Date.now(), demo: true };
}

function sparks(stage) {
  return [spark('spark1', 'spark1', '02:cc:dd:00', stage === 'ready'), spark('spark2', 'spark2', '02:cc:dd:00', stage === 'ready')];
}

function demoTick(cfg, phase) {
  const t = { t: Date.now(), links: {}, nodes: {}, inference: {} };
  for (const n of cfg.nodes) t.nodes[n.id] = { up: true };
  let i = 0;
  for (const l of cfg.links) {
    const p = phase + i++ * 1.7;
    const cap = (l.speedGbps || 10) * 1e9 / 8;
    const busy = Math.max(0, Math.sin(p) * 0.6 + Math.sin(p * 0.23) * 0.4);
    t.links[l.id] = { state: 'up', rx: cap * 0.35 * busy, tx: cap * 0.2 * Math.max(0, Math.sin(p + 2)), speedGbps: l.speedGbps, rttMs: null };
  }
  for (const n of cfg.nodes) if (n.kind !== 'studio') t.inference[n.id] = { engine: 'vLLM', engineKnown: true, model: 'Qwen3-32B-AWQ', vramMiB: 24000, gpuUtilPct: 40 + Math.round(40 * Math.abs(Math.sin(phase))), promptTps: 3000 * Math.abs(Math.sin(phase * 0.7)), decodeTps: 30 + 40 * Math.abs(Math.sin(phase * 0.5)) };
  return t;
}

module.exports = { studio, sparks, demoTick };
