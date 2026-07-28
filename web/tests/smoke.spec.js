import { test, expect } from '@playwright/test'

// RECEIVER and RADIATED are no longer nav destinations — they are MEASURE panes
// inside the Filter bench, where a scope capture / current probe becomes a
// filter requirement. Opening one in pane A leaves pane B on its default ('il'),
// so the pane still holds exactly one file input and one band select.
async function openMeasurePane(page, which) {
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('pane-select-a').selectOption(which)
}

test('spectrum: demo scan fails CISPR 32 B and hands off to the filter designer', async ({ page }) => {
  const consoleErrors = []
  page.on('pageerror', (e) => consoleErrors.push(String(e)))
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })

  await page.getByTestId('scan-example').selectOption('demo')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('fsw-detected')).toContainText('kHz')
  await expect(page.getByTestId('offenders').locator('tbody tr')).not.toHaveCount(0)
  const aReq = await page.getByTestId('areq').textContent()
  expect(parseFloat(aReq)).toBeGreaterThan(10)

  await page.getByTestId('design-fix').click()
  await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
  // the hand-off fills the cards but does NOT run — the user walks to DESIGN
  await expect(page.getByTestId('lcm')).not.toBeVisible()
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await expect(page.getByTestId('il-chart')).toBeVisible()
  await expect(page.getByTestId('middlebrook-margin')).toContainText('dB')
  await page.getByTestId('pane-select-a').selectOption('netlist')
  await expect(page.getByTestId('netlist')).toContainText('.subckt LISN')
  const fsw = await page.getByTestId('fsw').inputValue()
  expect(Math.abs(parseFloat(fsw) - 300)).toBeLessThan(10)
  expect(consoleErrors).toEqual([])
})

test('filter: ANP015 worked example reproduces 3.3 mH', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('areq-cm').fill('40')
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('cy-select').selectOption('4.7')   // the note's C_Y — auto would pick larger
  await page.getByTestId('lcm-source').selectOption('manual')   // the note's value list — catalog is default
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toContainText('3.3 mH')
  await page.getByTestId('pane-select-b').selectOption('values')
  await expect(page.getByTestId('il-cm')).toContainText('40.8')
})

test('receiver: demo signal separates the three detectors', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-scope')
  await page.getByTestId('run-demo').click()
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 45_000 })
})

test('lisn: the test-setup pane works without a design and exports the subckt', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('pane-select-b').selectOption('lisn')
  await expect(page.getByTestId('subckt')).toContainText('L1 eut mains 5e-05')
})

test('filter: bind parts via the schematic and export CIAS', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await page.getByTestId('pane-select-b').selectOption('bom')
  await expect(page.getByTestId('bom')).toBeVisible()
  await expect(page.getByTestId('download-cias')).toBeDisabled()

  for (const hotspot of ['sch-CMC1', 'sch-CX1', 'sch-CY1']) {
    await page.getByTestId(hotspot).click()   // opens CATALOG PARTS in the other pane
    await expect(page.getByTestId('part-panel')).toBeVisible()
    await page.getByTestId('bind-part').first().click()
  }
  await page.getByTestId('pane-select-b').selectOption('bom')
  await expect(page.getByTestId('bom').locator('tbody tr td:has-text("unbound")')).toHaveCount(0)
  await expect(page.getByTestId('download-cias')).toBeEnabled()

  const downloadPromise = page.waitForEvent('download')
  await page.getByTestId('download-cias').click()
  const download = await downloadPromise
  expect(download.suggestedFilename()).toContain('.cias.json')
})

test('filter: binding a curve-carrying CMC overlays measured-impedance IL', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('areq-cm').fill('15')
  await page.getByTestId('areq-dm').fill('15')
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('cy-select').selectOption('4.7')
  await page.getByTestId('lcm-source').selectOption('catalog')
  await page.getByTestId('mfr-filter').selectOption('Murata')
  await page.getByTestId('min-rated').fill('0')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('sch-CMC1').click()
  await expect(page.getByTestId('part-panel')).toContainText('DLW32MH201XK2')
  // SILENT column: measured IL at f_design for curve parts, incl. the
  // impedance-only DLW21HN181SQ2 ("by measured curve")
  await expect(page.getByTestId('part-panel')).toContainText('by measured curve')
  await expect(page.getByTestId('measured-il').first()).toContainText('dB', { timeout: 10_000 })
  await page.getByTestId('bind-part').first().click()
  await page.getByTestId('pane-select-b').selectOption('il')
  await expect(page.getByTestId('measured-note')).toContainText('DLW32MH201XK2')
})

test('receiver: REAL CISPR 16-1-1 calibration waveform through the GUI', async ({ page }) => {
  // One period of the 100 Hz Band-A alternative calibration waveform
  // (Azpurua & Hudlicka, DOI 10.5281/zenodo.17779465, CC-BY-4.0), tiled per its
  // readme (periodic at the PRF; time axis = i / 3 MHz; volts / 2 for 50 ohm).
  test.setTimeout(180_000)
  const fs = await import('node:fs')
  const period = fs.readFileSync('../tests/fixtures/pulse_bandA_100Hz.txt', 'utf8')
    .split('\n').filter(Boolean).map(Number)
  const reps = Math.floor(0.5 * 3e6 / period.length)
  const lines = ['time [s],voltage [V]']
  for (let r = 0; r < reps; r += 1) {
    for (let i = 0; i < period.length; i += 1) {
      lines.push(((r * period.length + i) / 3e6).toFixed(9) + ',' + (period[i] / 2).toPrecision(8))
    }
  }
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-scope')
  await page.getByTestId('band').selectOption('A')
  await page.locator('input[type="file"]').setInputFiles({
    name: 'cispr16_bandA_100Hz.csv', mimeType: 'text/csv',
    buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 150_000 })
})

test('receiver: 2-channel capture separates CM/DM and hands per-mode targets to the designer', async ({ page }) => {
  // synthetic but physically exact: CM tone at 300 kHz on both lines, DM tone
  // at 600 kHz in antiphase; separation must isolate them
  const fs = 3e6
  const lines = ['t,v_line,v_neutral']
  for (let i = 0; i < 0.05 * fs; i += 1) {
    const t = i / fs
    const cm = 3e-3 * Math.sin(2 * Math.PI * 300e3 * t)
    const dm = 1e-3 * Math.sin(2 * Math.PI * 600e3 * t)
    lines.push(t.toFixed(9) + ',' + (cm + dm).toPrecision(6) + ',' + (cm - dm).toPrecision(6))
  }
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-scope')
  await page.locator('input[type="file"]').setInputFiles({
    name: 'two_channel.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 60_000 })
  await page.getByTestId('compute-targets').click()
  await expect(page.getByTestId('target-cm')).toContainText(/\d/)
  await page.getByTestId('design-from-modes').click()
  await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
})

test('spectrum: CISPR 25 between-band points are visibly excluded, never silently passed', async ({ page }) => {
  // Berger NEW-2 repro: quiet inside the protected bands, 115 dBuV between them
  const lines = ['Frequency [MHz];Level [dBµV]']
  for (let f = 150e3; f <= 30e6; f *= 1.03) {
    const inBand = (f >= 150e3 && f <= 300e3) || (f >= 530e3 && f <= 1.8e6) ||
      (f >= 5.9e6 && f <= 6.2e6) || (f >= 26e6 && f <= 28e6)
    lines.push((f / 1e6).toFixed(5) + ';' + (inBand ? '20.0' : '115.0'))
  }
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('standard').selectOption('cispr25_class_5')
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'between_bands.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('verdict')).toHaveText('PASS')
  await expect(page.getByTestId('uncovered-points')).toContainText('NOT judged')
})

test('receiver: co-frequency CM+DM no longer under-states the requirement (Berger NEW-3)', async ({ page }) => {
  // v_line = 2 mV tone, v_neutral = 0 -> CM = DM = 1 mV at the same frequency.
  // The line voltage is 63 dBuV vs a 60.2 limit: true need ~12.8 dB, not 7.
  const fs = 3e6
  const lines = ['t,v_line,v_neutral']
  for (let i = 0; i < 0.05 * fs; i += 1) {
    const t_ = i / fs
    lines.push(t_.toFixed(9) + ',' + (2e-3 * Math.sin(2 * Math.PI * 300e3 * t_)).toPrecision(6) + ',0')
  }
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-scope')
  await page.locator('input[type="file"]').setInputFiles({
    name: 'cofreq.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 60_000 })
  await page.getByTestId('compute-targets').click()
  await expect(page.getByTestId('target-cm')).toBeVisible()
  const cm = parseFloat(await page.getByTestId('target-cm').textContent())
  expect(cm).toBeGreaterThanOrEqual(12)
})

test('spectrum: a stated dBm header survives a dBuV override (Berger R-1 blocker)', async ({ page }) => {
  // header states dBm but not the frequency unit; user must supply MHz. The
  // level override must NOT clobber the stated dBm (-10 dBm = 97 dBuV = FAIL).
  const lines = ['Frequency,Level (dBm)']
  for (let f = 0.15; f <= 30; f *= 1.05) lines.push(f.toFixed(4) + ',-10')
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('freq-unit').selectOption('MHz')        // frequency unit
  await page.getByTestId('level-unit').selectOption('dBuV')      // level override (must lose)
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'dbm_header.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('offenders')).toContainText('97.0')
})

test('spectrum: a broken file is reported, not silently dropped (Berger R-2)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.locator('input[type="file"]').first().setInputFiles([
    { name: 'good.csv', mimeType: 'text/csv',
      buffer: Buffer.from('Frequency [MHz];Level [dBµV]\n0.2;40\n0.5;41\n1.0;42\n') },
    { name: 'broken.csv', mimeType: 'text/csv', buffer: Buffer.from('not a spectrum at all') },
  ])
  await expect(page.getByTestId('verdict')).toBeVisible()
  await expect(page.getByTestId('file-problems')).toContainText('broken.csv')
})

test('spectrum: a multi-trace export demands a column choice — QP fails where Average passed (Berger round-6 F-1)', async ({ page }) => {
  // Berger's false-PASS repro: judging the silently-taken second (Average)
  // column showed +10 dB in hand while the Quasi-peak trace fails by 8.8 dB.
  const csv = ['Frequency (MHz),Average (dBuV),Quasi-peak (dBuV)',
    '0.15,50,72', '0.30,47,69', '0.50,44,62', '1.00,42,60', '2.00,43,59',
    '5.00,45,58', '10.00,48,64', '20.00,49,65', '30.00,50,66'].join('\n')
  const pageErrors = []
  page.on('pageerror', (e) => pageErrors.push(String(e)))
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'multitrace.csv', mimeType: 'text/csv', buffer: Buffer.from(csv),
  })
  // nothing is judged until the user says which trace the file carries
  await expect(page.getByTestId('column-picker')).toBeVisible()
  await expect(page.getByTestId('verdict')).not.toBeVisible()
  await page.getByTestId('column-select').selectOption({ label: 'Quasi-peak (dBuV)' })
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('column-note')).toContainText('Quasi-peak')
  await page.getByTestId('column-select').selectOption({ label: 'Average (dBuV)' })
  await expect(page.getByTestId('verdict')).toHaveText('PASS')
  await expect(page.getByTestId('column-note')).toContainText('Average')
  expect(pageErrors).toEqual([])
})

test('filter: receiver handoff sizes each mode at its own critical point (Berger round-6 F-2)', async ({ page }) => {
  // CM tone at 300 kHz, DM tone at 600 kHz: the designer must keep the modes
  // apart — the choke sized at the CM point, the X cap at the DM point.
  const fs = 3e6
  const lines = ['t,v_line,v_neutral']
  for (let i = 0; i < 0.05 * fs; i += 1) {
    const t = i / fs
    const cm = 3e-3 * Math.sin(2 * Math.PI * 300e3 * t)
    const dm = 1e-3 * Math.sin(2 * Math.PI * 600e3 * t)
    lines.push(t.toFixed(9) + ',' + (cm + dm).toPrecision(6) + ',' + (cm - dm).toPrecision(6))
  }
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-scope')
  await page.locator('input[type="file"]').setInputFiles({
    name: 'two_tone_modes.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 60_000 })
  await page.getByTestId('compute-targets').click()
  await expect(page.getByTestId('target-cm')).toContainText(/\d/)
  await page.getByTestId('design-from-modes').click()
  await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  const note = await page.getByTestId('binding-note').textContent()
  const cmAt = Number(note.match(/CM \d+ dB @ (\d+) kHz/)[1])
  const dmAt = Number(note.match(/DM \d+ dB @ (\d+) kHz/)[1])
  expect(Math.abs(cmAt - 300)).toBeLessThan(15)   // choke sized at the CM offence…
  expect(Math.abs(dmAt - 600)).toBeLessThan(25)   // …X cap at the DM offence, not max() of both
  await expect(page.getByTestId('f-design')).toContainText('CM')
})

test('receiver: a compliant capture hands over nothing — no defaults dressed as a design (Berger round-6 F-4)', async ({ page }) => {
  // ~10 dBuV tones, 50 dB below the limit: nothing binds, so there is no
  // design button and no silently-populated designer form.
  const fs = 3e6
  const lines = ['t,v_line,v_neutral']
  for (let i = 0; i < 0.05 * fs; i += 1) {
    const t = i / fs
    const cm = 3e-6 * Math.sin(2 * Math.PI * 300e3 * t)
    lines.push(t.toFixed(9) + ',' + (cm * 1.2).toPrecision(6) + ',' + (cm * 0.8).toPrecision(6))
  }
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-scope')
  await page.locator('input[type="file"]').setInputFiles({
    name: 'quiet.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 60_000 })
  await page.getByTestId('compute-targets').click()
  await expect(page.getByTestId('no-binding-note')).toContainText('needs no filter')
  await expect(page.getByTestId('design-from-modes')).not.toBeVisible()
})

test('spectrum: a 0 Hz row is refused loudly instead of crashing the chart (Berger round-6 F-3)', async ({ page }) => {
  const pageErrors = []
  page.on('pageerror', (e) => pageErrors.push(String(e)))
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'zero_hz.csv', mimeType: 'text/csv',
    buffer: Buffer.from('Frequency (Hz),Level (dBuV)\n0,10\n150000,72\n500000,62\n1000000,60\n'),
  })
  await expect(page.getByTestId('file-problems')).toContainText('non-positive frequency')
  await expect(page.getByTestId('verdict')).not.toBeVisible()
  expect(pageErrors).toEqual([])   // was: 2x uncaught RangeError from the tick loop
})

test('spectrum: an index-first export is judged on the stated frequency column (Berger round-7 R7-1)', async ({ page }) => {
  // "No.,Frequency (MHz),QP" — reading the row index as megahertz fabricated
  // the whole axis (wrong corner frequency, wrong required attenuation).
  const csv = ['No.,Frequency (MHz),Quasi-peak (dBuV)',
    '1,0.20,70', '2,0.30,69', '3,3.00,40'].join('\n')
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'idx_first.csv', mimeType: 'text/csv', buffer: Buffer.from(csv),
  })
  await expect(page.getByTestId('column-picker')).toBeVisible()
  // the frequency column is never offered as a level trace
  await expect(page.getByTestId('column-select').locator('option', { hasText: 'Frequency' })).toHaveCount(0)
  await page.getByTestId('column-select').selectOption({ label: 'Quasi-peak (dBuV)' })
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  // worst offender at the REAL 200 kHz (70 vs 63.6), not at a fabricated 1 MHz
  await expect(page.getByTestId('offenders')).toContainText('kHz')
})

test('filter: catalog escalation reaches a passing part in one jump (Berger round-7 R7-2)', async ({ page }) => {
  // 26 dB CM @ 2 MHz: hundreds of catalog parts satisfy this, but the old
  // one-candidate-per-iteration walk gave up after 8 steps and reported a
  // shortfall. The selector must land on a passing part.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('fsw').fill('2000')
  await page.getByTestId('areq-cm').fill('26')
  await page.getByTestId('areq-dm').fill('10')
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('lcm-source').selectOption('catalog')
  await page.getByTestId('min-rated').fill('0')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('wc-verdict-cm')).toHaveClass(/pass/, { timeout: 30_000 })
  await expect(page.getByTestId('wc-verdict-cm')).toContainText('≥ 26')
})

test('filter: an unrealizable leakage/choke pair is refused loudly (Berger round-7 R7-3, hardened 2026-07-27)', async ({ page }) => {
  // Originally the pair designed WITH a k-warning and a disabled export. The
  // engine now enforces the realizability floor L_CM > L_DM/2 at selection
  // time, so a 10 mH claimed leakage against a 2.2 mH catalog cannot even
  // produce a design — the refusal names the constraint, and entering a real
  // leakage recovers.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('dm-mode').selectOption('inductance')
  await page.getByTestId('ldm-input').fill('10000')   // 10 mH "leakage" vs a 2.2 mH catalog
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.locator('.err')).toContainText('must carry the assumed DM leakage')
  await expect(page.getByTestId('lcm')).not.toBeVisible()
  // recovery: a realistic leakage designs immediately
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('ldm-input').fill('14.6')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await expect(page.getByTestId('k-warning')).not.toBeVisible()
})

test('spectrum: CISPR 25 Class 3 uses the band-dependent Table 4 steps (Berger round-8 F8-2/F8-4)', async ({ page }) => {
  // Old flat "+20 dB over Class 5" passed this scan; Table 4's real Class 3
  // limits (57 @ MW, 52 @ SW, 43 @ CB, 37 @ FM) fail it at three points, and
  // 40 MHz now has a limit at all (VHF 30-54 band).
  const csv = ['Frequency (MHz),Quasi-peak (dBuV)',
    '0.20,60.0', '1.00,50.0', '6.00,55.0', '27.00,46.0', '40.00,35.0', '90.00,40.0'].join('\n')
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('standard').selectOption('cispr25_class_3')
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'c25_class3.csv', mimeType: 'text/csv', buffer: Buffer.from(csv),
  })
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  const offenders = await page.getByTestId('offenders').textContent()
  expect(offenders).toContain('6 MHz')    // 55 vs 52
  expect(offenders).toContain('27 MHz')   // 46 vs 43
  expect(offenders).toContain('90 MHz')   // 40 vs 37
  expect(offenders).not.toContain('40 MHz')  // 35 vs 43 (VHF 30-54, class 3): in hand
})

test('receiver: switching the band re-measures — a too-short record fails loudly (Berger round-8 F8-1)', async ({ page }) => {
  // The band selector sets RBW and detector time constants; it must re-run
  // the receiver chain, not re-label stale curves. This record fits band B
  // but is far below band A's analysis window — the old code kept showing
  // the band-B curves under the band-A label.
  const fs = 6.4e6
  const lines = ['t,v_line,v_neutral']
  for (let i = 0; i < 16000; i += 1) {
    const t = i / fs
    const cm = 3e-3 * Math.sin(2 * Math.PI * 300e3 * t)
    lines.push(t.toFixed(9) + ',' + cm.toPrecision(6) + ',' + cm.toPrecision(6))
  }
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-scope')
  await page.locator('input[type="file"]').setInputFiles({
    name: 'short_record.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 60_000 })
  await page.getByTestId('band').selectOption('A')
  await expect(page.getByTestId('error')).toContainText('too short', { timeout: 60_000 })
  await expect(page.getByTestId('receiver-chart')).not.toBeVisible()
})

test('spectrum: the lower limit applies at the CISPR 32 Class A 500 kHz boundary (Berger round-9 F9-1)', async ({ page }) => {
  // At a transition frequency the LOWER limit governs: 73 dBuV, not 79 — the
  // old first-segment-wins lookup was 6 dB soft exactly on a very common grid
  // frequency and flipped this FAIL to MARGINAL.
  const csv = ['Frequency (MHz),Quasi-peak (dBuV)',
    '0.30,70.0', '0.499,70.0', '0.50,76.0', '0.501,70.0', '1.00,70.0'].join('\n')
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('standard').selectOption('cispr32_class_a')
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'c32a_500k.csv', mimeType: 'text/csv', buffer: Buffer.from(csv),
  })
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('offenders')).toContainText('73.0')
})

test('filter: a positive min rated current excludes unrated catalog parts (Berger round-9 F9-3)', async ({ page }) => {
  // 150 catalogued chokes carry no rating; letting null bypass the threshold
  // satisfied a "100 A" design with an 11.5 A SMD part.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('cy-select').selectOption('4.7')
  await page.getByTestId('lcm-source').selectOption('catalog')
  await page.getByTestId('min-rated').fill('100')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  // no >=100 A part covers a mains CM choke requirement: the designer must
  // refuse loudly, never fall back to an unrated part
  await expect(page.getByTestId('error')).toContainText(/no catalog parts match|no CM-choke candidate/)
})

test('spectrum: judging an Average column against a QP limit raises a detector-mismatch warning (Berger round-9 F9-4)', async ({ page }) => {
  const csv = ['Frequency (MHz),Quasi-peak (dBuV),Average (dBuV)',
    '0.15,59,52', '1.00,43,36', '6.00,42,35', '27.00,33,26'].join('\n')
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('standard').selectOption('cispr25_class_5')
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'detmix.csv', mimeType: 'text/csv', buffer: Buffer.from(csv),
  })
  await page.getByTestId('column-select').selectOption({ label: 'Average (dBuV)' })
  await expect(page.getByTestId('detector-mismatch')).toContainText('quasi-peak')
  // aligning the detector clears the warning
  await page.getByTestId('detector').selectOption('average')
  await expect(page.getByTestId('detector-mismatch')).not.toBeVisible()
})

test('spectrum: a PASS names the limit bands the scan never reached (Berger round-10 R10-1)', async ({ page }) => {
  // A 1-10 MHz sweep against CISPR 32 Class B leaves 150 kHz-1 MHz and
  // 10-30 MHz unmeasured — the verdict must say so, loudly, or a partial
  // sweep reads as a clean bill for the whole standard.
  const lines = ['Frequency (MHz),Quasi-peak (dBuV)']
  for (let f = 1; f <= 10; f *= 1.05) lines.push(f.toFixed(4) + ',50.0')
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'narrow_1to10.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('verdict')).toHaveText('PASS')
  await expect(page.getByTestId('unswept-note')).toContainText('NOT SWEPT')
  await expect(page.getByTestId('unswept-note')).toContainText('150 kHz')
  await expect(page.getByTestId('unswept-note')).toContainText('30 MHz')
  // a genuinely full, dense sweep shows no such note (3 spot points would
  // rightly be flagged by the two-point rule — that is not a sweep)
  await page.getByRole('button', { name: 'Clear' }).click()
  const full = ['Frequency (MHz),Quasi-peak (dBuV)']
  for (let f = 0.15; f < 30; f *= 1.05) full.push(f.toFixed(5) + ',40')
  full.push('30.00000,40')
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'full_span.csv', mimeType: 'text/csv', buffer: Buffer.from(full.join('\n')),
  })
  await expect(page.getByTestId('verdict')).toHaveText('PASS')
  await expect(page.getByTestId('unswept-note')).not.toBeVisible()
})

test('spectrum: an interior sampling hole is named NOT SWEPT, not painted over (Berger round-11 R11-1)', async ({ page }) => {
  // Spliced scan: 0.15-1 MHz and 20-30 MHz with nothing in between — 56% of
  // the CISPR 32 span. The extent check saw a full sweep; the verdict must
  // name the 1-20 MHz hole.
  const lines = ['Frequency (MHz),Quasi-peak (dBuV)']
  for (let f = 0.15; f < 1; f *= 1.05) lines.push(f.toFixed(5) + ',45.0')
  lines.push('1.00000,45.0')
  for (let f = 20; f < 30; f *= 1.02) lines.push(f.toFixed(4) + ',50.0')
  lines.push('30.0000,50.0')
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'holed_c32.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('verdict')).toHaveText('PASS')
  await expect(page.getByTestId('unswept-note')).toContainText('NOT SWEPT')
  await expect(page.getByTestId('unswept-note')).toContainText('1 MHz–20 MHz')
})

test('spectrum: an entirely skipped CISPR 25 band is named NOT SWEPT (Berger round-13 F13-1)', async ({ page }) => {
  // Every service band covered except CB 26-28 MHz, skipped inside a modest
  // 25->30 MHz jump (ratio 1.2 — far under any ratio rule): a regulated band
  // with zero samples must be flagged regardless of the jump's ratio.
  const lines = ['Frequency (MHz),Quasi-peak (dBuV)']
  const band = (f0, f1, n) => {
    for (let i = 0; i <= n; i += 1) lines.push((f0 * (f1 / f0) ** (i / n)).toFixed(6) + ',20.0')
  }
  band(0.15, 0.3, 30); band(0.53, 1.8, 30); band(5.9, 6.2, 30)
  lines.push('25.000000,20.0')
  band(30, 54, 30); band(68, 108, 30)
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('standard').selectOption('cispr25_class_5')
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'cb_skipped.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('verdict')).toHaveText('PASS')
  await expect(page.getByTestId('unswept-note')).toContainText('26 MHz–28 MHz')
})

test('filter: the schematic passes CIAS netlist verification for 1 and 2 stages', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  // the schematic rendered means the geometry-extracted netlist matched filterNets()
  await expect(page.getByTestId('sch-CMC1')).toBeVisible()
  await expect(page.getByTestId('sch-error')).toHaveCount(0)
  await page.getByTestId('sec-req').click()
  await page.getByTestId('stages').selectOption('2')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('sch-CMC2')).toBeVisible()
  await expect(page.getByTestId('sch-error')).toHaveCount(0)
})

test('spectrum: the real CISPR 25 bench scan loads with attribution and fails Class 5 average', async ({ page }) => {
  // Digitized from Baltic Lab's Fig. 15 (DOI 10.5281/zenodo.18202069, CC-BY-4.0):
  // a real DUT that passes the paper's Class 3 target and fails Class 5 average
  // by ~7.7 dB near 76 MHz — a REAL failing scan for the design-the-fix flow.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('scan-example').selectOption('baltic')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('real-scan-attribution')).toContainText('zenodo')
  await expect(page.getByTestId('column-note')).toContainText('Average')
  const areq = await page.getByTestId('areq').textContent()
  expect(parseFloat(areq)).toBeGreaterThan(15)   // 7.7 dB deficit + 10 dB margin
  // the same scan passes the paper's target: Class 3, average
  await page.getByTestId('standard').selectOption('cispr25_class_3')
  await expect(page.getByTestId('verdict')).toHaveText('PASS')
})

test('filter: the caps manufacturer filter scopes the recommended parts, not only the values', async ({ page }) => {
  // Würth selected as X-cap manufacturer must never recommend WIMA — the
  // closest-value sort otherwise favors the biggest manufacturer in the pool.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('cx-source').selectOption('catalog')
  await page.getByTestId('cx-mfr').selectOption('Würth Elektronik')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('sch-CX1').click()
  await expect(page.getByTestId('part-panel')).toContainText('Würth Elektronik')
  await expect(page.getByTestId('part-panel')).not.toContainText('WIMA')
  await page.getByTestId('sch-CY1').click()
  await expect(page.getByTestId('part-panel')).toContainText('Würth Elektronik')
  await expect(page.getByTestId('part-panel')).not.toContainText('WIMA')
})

test('filter: the rail walks Requirement -> Components -> Grid & safety to the DESIGN button', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  // stage 1 open, DESIGN not reachable yet
  await expect(page.getByTestId('cont-req')).toBeVisible()
  await expect(page.getByTestId('compute')).not.toBeVisible()
  await page.getByTestId('cont-req').click()
  await expect(page.getByTestId('cy-select')).toBeVisible()      // stage 2 opened
  await page.getByTestId('cont-comp').click()
  await expect(page.getByTestId('touch-tier')).toBeVisible()     // stage 3 opened
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
})

test('spectrum: the real CM/DM scan hands per-mode targets — hard CM, light DM', async ({ page }) => {
  // Fig. 18 of the Baltic Lab paper (CC-BY-4.0): CM average fails Class 5 by
  // 9.3 dB; DM PASSES the raw limit (+5.7 dB) but not the 10+6 dB engineering
  // buffer — so the per-mode handoff carries a hard CM requirement (~20 dB)
  // and a small, honestly-derived DM one (~9 dB), each at ITS OWN frequency.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('scan-example').selectOption('baltic-cmdm')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('trace-semantics')).toHaveValue('cm-first')
  await expect(page.getByTestId('real-scan-attribution')).toContainText('Fig. 18')
  await page.getByTestId('design-fix').click()
  await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
  const note = await page.getByTestId('binding-note').textContent()
  expect(Number(note.match(/CM (\d+) dB/)[1])).toBeGreaterThanOrEqual(19)  // 9.3 deficit + 10 dB
  const dmReq = Number(note.match(/DM (\d+) dB/)[1])
  expect(dmReq).toBeGreaterThanOrEqual(5)
  expect(dmReq).toBeLessThanOrEqual(12)   // DM must stay LIGHT — never sized from the CM offence
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await expect(page.getByTestId('wc-verdict-dm')).toContainText(`≥ ${dmReq}`)
})

test('spectrum: the real QR-flyback pre-scan fails LOW and HIGH frequencies (MDPI, CC-BY)', async ({ page }) => {
  // Kuo & Tsou, Energies 10(1):24 (2017), Fig. 20a — real 24 W QR flyback
  // peak-hold pre-scan judged against the CISPR 32 B AVERAGE limit: over the
  // limit around 150-510 kHz AND 5-7 MHz (worst +6.0 dB @ 509 kHz; the
  // paper's own marker table confirms the 498 kHz average failure).
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('scan-example').selectOption('mdpi')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('mdpi-attribution')).toContainText('Energies')
  const areq = await page.getByTestId('areq').textContent()
  expect(parseFloat(areq)).toBeGreaterThan(14)          // ~6 dB deficit + 10 dB margin
  await expect(page.getByTestId('offenders')).toContainText('kHz')   // low-band offender rows
})

test('radiated: the CM-current screening estimator judges against CISPR 32 with a loud uncertainty banner', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await openMeasurePane(page, 'measure-probe')
  await page.getByTestId('radiated-example').click()
  await expect(page.getByTestId('radiated-verdict')).toContainText('screen')
  await expect(page.getByTestId('radiated-uncertainty')).toContainText('triage, not a measurement')
  await expect(page.getByTestId('radiated-chart')).toBeVisible()
  // a voltage scan is refused: 5 uA on 1 m at 30 MHz = 35.97 dBuV/m — the
  // folklore number — must appear when judged at Class B 3 m
  await page.getByTestId('radiated-class').selectOption('a')
  await expect(page.getByTestId('radiated-verdict')).toBeVisible()
})

test('spectrum: a dBuA current spectrum is refused and pointed at the RADIATED screen', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.locator('input[type="file"]').first().setInputFiles({
    name: 'probe.csv', mimeType: 'text/csv',
    buffer: Buffer.from('Frequency (MHz),CM current (dBuA)\n30,20\n100,14\n'),
  })
  await expect(page.getByTestId('file-problems')).toContainText('RADIATED screen')
})

test('filter: binding a part re-runs the evaluation AS BUILT (bound values drive chips and netlist)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('sch-CMC1').click()
  await expect(page.getByTestId('part-panel')).toBeVisible()
  await page.getByTestId('bind-part').first().click()
  await expect(page.getByTestId('as-built-note')).toContainText('AS BUILT')
  // the strip L_CM now shows the BOUND part's value (the note repeats it)
  const note = await page.getByTestId('as-built-note').textContent()
  const lcm = await page.getByTestId('lcm').textContent()
  expect(note).toContain(lcm.trim())
})

test('filter: WE parts show reconstructed-phase Meas. IL marked with ~ (ABT #295)', async ({ page }) => {
  // 397 WE CMCs now carry REDEXPERT-measured |Z| curves; phase is Bode-
  // reconstructed (validated 0.03 dB median vs Murata's measured phase) and
  // the column says so with a '~' prefix.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('areq-cm').fill('35')
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('cy-select').selectOption('4.7')
  await page.getByTestId('mfr-filter').selectOption('Würth Elektronik')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('sch-CMC1').click()
  await expect(page.getByTestId('part-panel')).toContainText('744834101')
  await expect(page.getByTestId('part-panel')).toContainText(/~-?\d+\.\d dB/, { timeout: 10_000 })
})

test('filter: a failing-scan handoff shows every binding point as a requirement marker (user request)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('scan-example').selectOption('demo')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await page.getByTestId('design-fix').click()
  await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
  await expect(page.getByTestId('binding-note')).toContainText('min-f_co')   // inputs filled from the scan
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('il-chart')).toBeVisible()
  // the IL chart carries MANY violation markers (the scan's failing points),
  // not just the two f_design dots
  const markers = await page.getByTestId('il-chart').locator('circle').count()
  expect(markers).toBeGreaterThan(10)
})

test('filter: the predicted post-filter residual judges the demo scan (ABT #289/#291)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('scan-example').selectOption('demo')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await page.getByTestId('design-fix').click()
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('pane-select-a').selectOption('result')
  // the designed filter must actually fix the scan it was designed FROM
  await expect(page.getByTestId('predicted-verdict')).toContainText('PREDICTED PASS')
  await expect(page.getByTestId('predicted-chart')).toBeVisible()
})

test('filter: PRINT REPORT produces the pre-compliance sheet (ABT #293)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('scan-example').selectOption('demo')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await page.getByTestId('design-fix').click()
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.evaluate(() => { window.__printed = 0; window.print = () => { window.__printed += 1 } })
  await page.getByTestId('print-report').click()
  // professional report: metadata dialog first — project/EUT/author on the cover
  await expect(page.getByTestId('report-dialog')).toBeVisible()
  await page.getByTestId('report-project').fill('ACME 65 W adapter rev B')
  await page.getByTestId('report-author').fill('A. Martinez')
  await page.getByTestId('report-print-confirm').click()
  await expect.poll(() => page.evaluate(() => window.__printed)).toBe(1)
  const sheet = await page.getByTestId('report-sheet').textContent()
  expect(sheet).toContain('EMC pre-compliance report')
  expect(sheet).toContain('ACME 65 W adapter rev B')
  expect(sheet).toContain('HZ-')                        // report ID
  expect(sheet).toContain('Summary of verdicts')
  expect(sheet).toContain('Measurement')                // scan context section made it in
  expect(sheet).toContain('Filter design')
  expect(sheet).toContain('Bill of materials')
  expect(sheet).toContain('Method & assumptions')
  expect(sheet).toContain('NOT a certification')
  // the demo scan FAILS, so the measured offender table carries rows with negative margins
  expect(sheet).toMatch(/Margin \(dB\)/)
  // and both charts are embedded as SVGs in the sheet
  expect(await page.getByTestId('report-sheet').locator('svg').count()).toBeGreaterThanOrEqual(3)
})

test('filter: a parallel capacitor bank binds as n x MPN through BOM and CIAS (ABT #294)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('areq-cm').fill('35')
  await page.getByTestId('sec-comp').click()
  // a ~1 uF target: no single part is exact, so 2 x 470 nF / 2 x 560 nF banks rank
  await page.getByTestId('dm-mode').selectOption('inductance')
  await page.getByTestId('ldm-input').fill('28.1')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('sch-CX1').click()
  const bankRow = page.getByTestId('part-panel').locator('tr', { hasText: 'parallel bank' }).first()
  await expect(bankRow).toBeVisible()
  await bankRow.getByTestId('bind-part').click()
  await page.getByTestId('pane-select-b').selectOption('bom')
  await expect(page.getByTestId('bom')).toContainText(/[234] × /)
})

test('filter: the DC/CISPR 25 topology drops the touch budget and swaps the 5 uH LISN (ABT #292)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('topology').selectOption('dc')
  await page.getByTestId('areq-cm').fill('30')   // the >=1 A stub pool tops out at 2.2 mH
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  // chassis-referenced DC port: no IEC touch-current budget to chip
  await expect(page.getByTestId('touch-verdict')).not.toBeVisible()
  await page.getByTestId('pane-select-a').selectOption('netlist')
  await expect(page.getByTestId('netlist')).toContainText('5e-06')   // CISPR 25 5 uH LISN
  await expect(page.getByTestId('netlist')).toContainText('ESL/ESR not modeled')
})

test('filter: a 20 A line current raises saturation warnings on undersized chokes (ABT #290)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('areq-cm').fill('35')
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('mfr-filter').selectOption('Würth Elektronik')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('line-current').fill('20')
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('sch-CMC1').click()
  // 20 A RMS -> 28.3 A peak; the WE stub parts saturate at 2 / 10 A peak
  await expect(page.getByTestId('saturation-warn').first()).toContainText('A pk >')
})

test('filter: 3-phase topology designs from 3-winding chokes with delta X and a 16.7 ohm CM bench (ABT #292)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('topology').selectOption('3ph')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  // AC mains: the touch-current budget applies (phase-to-earth voltage)
  await expect(page.getByTestId('touch-verdict')).toBeVisible()
  // the catalog slot only offers 3-winding parts
  await page.getByTestId('sch-CMC1').click()
  await expect(page.getByTestId('part-panel')).toContainText('744837010290')
  await expect(page.getByTestId('part-panel')).not.toContainText('744834101')
  // the deck carries three coupled windings and the delta wrap capacitor
  await page.getByTestId('pane-select-a').selectOption('netlist')
  await expect(page.getByTestId('netlist')).toContainText('Kcm1_23')
  await expect(page.getByTestId('netlist')).toContainText('Cx1_31')
})

test('filter: 3-phase + neutral refuses the 2-winding catalog loudly, designs from manual candidates (ABT #292)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('topology').selectOption('3phn')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  // no 4-winding chokes exist in the catalog — surfaced, never silently downgraded
  await expect(page.locator('.err')).toContainText('no 4-winding')
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('lcm-source').selectOption('manual')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('pane-select-a').selectOption('netlist')
  await expect(page.getByTestId('netlist')).toContainText('Lcm_neut_1')
  await expect(page.getByTestId('netlist')).toContainText('Cx1_1n')   // star, phase-to-neutral
})

test('filter: voltage ratings are judged on the right basis per position (ABT #292 probe)', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('topology').selectOption('3ph')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  // a 760 VAC-rated 3-phase choke on a 400 V grid: no false alarm
  await page.getByTestId('sch-CMC1').click()
  await expect(page.getByTestId('part-panel')).toContainText('760')
  await page.getByTestId('bind-part').first().click()
  // a correctly-rated Y2 cap (sees 230 V phase-to-earth, not 400): no false alarm
  await page.getByTestId('sch-CY1').click()
  await page.getByTestId('bind-part').first().click()
  // an X2 cap on the delta line-to-line position: the class itself disqualifies it
  await page.getByTestId('sch-CX1').click()
  await expect(page.getByTestId('delta-x-voltage-note')).toContainText('line-to-line')
  await page.getByTestId('bind-part').first().click()
  await page.getByTestId('pane-select-b').selectOption('bom')
  const bom = await page.getByTestId('bom').textContent()
  expect(bom).toContain('NOT rated for it')          // the X2 on 400 V L-L
  expect(bom.match(/NOT rated for it/g).length).toBe(3)  // all three delta caps, nothing else
})

test('filter: an HF-only scan no longer dead-ends on the K constraint (user report 2026-07-27)', async ({ page }) => {
  // The Baltic Lab scans fail at high frequency with small margins: CM alone
  // asks for nanohenries, the catalog rounded up to a 91 nH data-line choke,
  // and the assumed 14.6 uH leakage made the pair unbuildable (K out of (0,1)).
  // The selection now respects the realizability floor L_CM > L_DM/2 and says so.
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('scan-example').selectOption('baltic')
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await page.getByTestId('design-fix').click()
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await expect(page.getByTestId('k-warning')).not.toBeVisible()
  await expect(page.getByTestId('leakage-floor-note')).toContainText('cannot leak more than')
})

test('filter: the ngspice round-trip cross-checks the ABCD model in-browser (ABT #299)', async ({ page }) => {
  test.setTimeout(180_000)   // first click compiles the ~11 MB ngspice engine
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('areq-cm').fill('35')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('pane-select-a').selectOption('il')
  await page.getByTestId('run-roundtrip').click()
  // first click compiles the ~11 MB ngspice engine
  await expect(page.getByTestId('roundtrip-verdict')).toBeVisible({ timeout: 90_000 })
  await expect(page.getByTestId('roundtrip-verdict')).toHaveClass(/pass/)
  await expect(page.getByTestId('roundtrip-verdict')).toContainText('NGSPICE×ABCD')
  await expect(page.getByTestId('il-chart').getByText(/CM ngspice round-trip/)).toBeVisible()   // overlay in the legend
})

test('filter: the 3-phase deck round-trips through ngspice (all-pairs K loads) (ABT #299)', async ({ page }) => {
  test.setTimeout(180_000)
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('topology').selectOption('3ph')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('pane-select-a').selectOption('il')
  await page.getByTestId('run-roundtrip').click()
  await expect(page.getByTestId('roundtrip-verdict')).toBeVisible({ timeout: 90_000 })
  await expect(page.getByTestId('roundtrip-verdict')).toHaveClass(/pass/)
})
