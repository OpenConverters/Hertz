import { test, expect } from '@playwright/test'

test('spectrum: demo scan fails CISPR 32 B and hands off to the filter designer', async ({ page }) => {
  const consoleErrors = []
  page.on('pageerror', (e) => consoleErrors.push(String(e)))
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })

  await page.getByTestId('load-demo').click()
  await expect(page.getByTestId('verdict')).toHaveText('FAIL')
  await expect(page.getByTestId('fsw-detected')).toContainText('kHz')
  await expect(page.getByTestId('offenders').locator('tbody tr')).not.toHaveCount(0)
  const aReq = await page.getByTestId('areq').textContent()
  expect(parseFloat(aReq)).toBeGreaterThan(10)

  await page.getByTestId('design-fix').click()
  await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
  await expect(page.getByTestId('lcm')).toBeVisible()
  await expect(page.getByTestId('il-chart')).toBeVisible()
  await expect(page.getByTestId('middlebrook-margin')).toContainText('dB')
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
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toContainText('3.3 mH')
  await expect(page.getByTestId('il-cm')).toContainText('40.8')
})

test('receiver: demo signal separates the three detectors', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-receiver').click()
  await page.getByTestId('run-demo').click()
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 45_000 })
})

test('lisn: subckt export present', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-lisn').click()
  await expect(page.getByTestId('subckt')).toContainText('L1 eut mains 5e-05')
})

test('filter: bind parts via the schematic and export CIAS', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('bom')).toBeVisible()
  await expect(page.getByTestId('download-cias')).toBeDisabled()

  for (const hotspot of ['sch-CMC1', 'sch-CX1', 'sch-CY1']) {
    await page.getByTestId(hotspot).click()
    await expect(page.getByTestId('part-panel')).toBeVisible()
    await page.getByTestId('bind-part').first().click()
  }
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
  await page.getByTestId('lcm-source').selectOption('catalog')
  await page.getByTestId('mfr-filter').selectOption('Murata')
  await page.getByTestId('min-rated').fill('0')
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('bom')).toBeVisible()
  await page.getByTestId('sch-CMC1').click()
  await expect(page.getByTestId('part-panel')).toContainText('DLW32MH201XK2')
  // SILENT column: measured IL at f_design for curve parts, incl. the
  // impedance-only DLW21HN181SQ2 ("by measured curve")
  await expect(page.getByTestId('part-panel')).toContainText('by measured curve')
  await expect(page.getByTestId('measured-il').first()).toContainText('dB', { timeout: 10_000 })
  await page.getByTestId('bind-part').first().click()
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
  await page.getByTestId('mode-receiver').click()
  await page.locator('select').first().selectOption('A')
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
  await page.getByTestId('mode-receiver').click()
  await page.locator('input[type="file"]').setInputFiles({
    name: 'two_channel.csv', mimeType: 'text/csv', buffer: Buffer.from(lines.join('\n')),
  })
  await expect(page.getByTestId('receiver-chart')).toBeVisible({ timeout: 60_000 })
  await page.getByTestId('compute-targets').click()
  await expect(page.getByTestId('target-cm')).toContainText(/\d/)
  await page.getByTestId('design-from-modes').click()
  await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
  await expect(page.getByTestId('bom')).toBeVisible()
})
