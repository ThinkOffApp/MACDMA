'use strict';
const path = require('path');
const fs = require('fs');
const { ARMS } = require('./testrun');

function bandwidthCommand({ root, link, settings, output, options = {} }) {
  const runner = path.join(root, 'benchmarks', 'run_bw.py');
  if (!fs.existsSync(runner)) throw new Error('Bandwidth requires the MCDMA source checkout and Python 3');
  if (!link || !link.status.ready) throw new Error('Select a configured RDMA link');
  if (!output) throw new Error('Bandwidth requires --output with a new evidence directory');
  if (link.mac.kind !== 'ssh') throw new Error('Bandwidth currently requires --studio-host with a key-authenticated SSH alias, including when the Mac is local');
  const arm = ARMS[settings.test.arm];
  if (!arm) throw new Error('Unknown posting mode');
  const tools = settings.tools;
  const args = [runner, '--mac-host', link.mac.host, '--peer-host', link.sparkHost,
    '--mac-bw', tools.macBw || '/usr/local/libexec/mcdma/mcdma-bw',
    '--peer-bw', tools.sparkBw || '/usr/local/libexec/mcdma/mcdma-bw',
    '--mac-checker', tools.macChecker || '/usr/local/libexec/mcdma/cx5-native-check',
    '--mac-provider', tools.provider || '/usr/local/lib/rdma/libmcdma-rdmav34.so',
    '--mac-interface', link.studio.iface, '--peer-interface', link.spark.iface,
    '--mac-device', link.studio.rdmaDevice, '--peer-device', link.spark.rdmaDevice,
    '--peer-gid-index', String(link.spark.gidIndex), '--mtu', String(settings.test.mtu),
    '--mac-cq-map', arm.MCDMA_CQ_MAP, '--mac-user-post', arm.MCDMA_USER_POST,
    '--mac-user-bf', arm.MCDMA_USER_BF, '--finish', 'flag', '--output', path.resolve(output)];
  const defaults = { ops: 'write,read', sizes: '65536,1048576', depths: '1,16', qps: '1', total: '67108864', repeats: '3', warmup: '1', 'verify-bytes': '4194304' };
  for (const [key, value] of Object.entries({ ...defaults, ...options })) args.push('--' + key, String(value));
  return ['python3', args];
}
module.exports = { bandwidthCommand };
