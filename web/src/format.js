export function fmtHz(f) {
  if (f >= 1e9) return trim(f / 1e9) + ' GHz'
  if (f >= 1e6) return trim(f / 1e6) + ' MHz'
  if (f >= 1e3) return trim(f / 1e3) + ' kHz'
  return trim(f) + ' Hz'
}
function trim(x) {
  const s = x >= 100 ? x.toFixed(0) : x >= 10 ? x.toFixed(1) : x.toFixed(2)
  return s.replace(/\.0+$/, '').replace(/(\.\d*?)0+$/, '$1')
}
export function fmtDb(x, digits = 1) {
  return x === null || x === undefined ? '—' : x.toFixed(digits)
}
export function fmtSi(x, unit) {
  if (x === 0) return '0 ' + unit
  const prefixes = [[1e9, 'G'], [1e6, 'M'], [1e3, 'k'], [1, ''], [1e-3, 'm'], [1e-6, 'µ'], [1e-9, 'n'], [1e-12, 'p']]
  for (const [scale, prefix] of prefixes) {
    if (Math.abs(x) >= scale * 0.9999) return trim(x / scale) + ' ' + prefix + unit
  }
  return x.toExponential(2) + ' ' + unit
}
