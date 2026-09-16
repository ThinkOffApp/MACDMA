'use strict';
// Compare numeric release versions; unknown formats never authorize an install.
function compareVersions(a, b) {
  const parse = (v) => typeof v === 'string' && /^\d+\.\d+\.\d+$/.test(v) ? v.split('.').map(Number) : null;
  const aa = parse(a), bb = parse(b);
  if (!aa || !bb || [...aa, ...bb].some((n) => !Number.isSafeInteger(n))) return null;
  for (let i = 0; i < 3; i++) if (aa[i] !== bb[i]) return aa[i] > bb[i] ? 1 : -1;
  return 0;
}
function installDecision(installed, candidate) {
  if (compareVersions(candidate, candidate) !== 0) return { allowed: false, reason: 'Invalid package version' };
  if (!installed) return { allowed: true, reason: 'Fresh installation' };
  const order = compareVersions(candidate, installed);
  return order === null ? { allowed: false, reason: 'Cannot compare installed and package versions' }
    : order < 0 ? { allowed: false, reason: `Refusing downgrade from ${installed} to ${candidate}` }
    : { allowed: true, reason: order ? 'Upgrade' : 'Same version' };
}
module.exports = { compareVersions, installDecision };
