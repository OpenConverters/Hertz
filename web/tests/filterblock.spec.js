// The filter BLOCK (plan S3/S4, ABT #444/#445): one click iterates layout +
// BOM to the target and hands back a characterized part in five formats.
import { test, expect } from '@playwright/test'

async function designAndOpenBlockPane(page, { adm = '30' } = {}) {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20_000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('areq-cm').fill('40')
  await page.getByTestId('areq-dm').fill(adm)
  // e2e runs without the parts catalog: use the manual value ladders
  await page.getByTestId('sec-comp').click()
  await page.getByTestId('lcm-source').selectOption('manual')
  await page.getByTestId('cx-source').selectOption('manual')
  // switch a pane to LAYOUT & BLOCK
  await page.getByTestId('pane-select-b').selectOption('layout')
}

test('the block iterates to a meeting layout and offers every format', async ({ page }) => {
  const consoleErrors = []
  page.on('pageerror', (e) => consoleErrors.push(String(e)))
  await designAndOpenBlockPane(page)

  await page.getByTestId('build-block').click()
  const verdict = page.getByTestId('block-verdict')
  await expect(verdict).toBeVisible({ timeout: 30000 })
  await expect(verdict).toContainText('MEETS TARGET')

  // the generated board renders from the SAME text the download carries
  await expect(page.getByTestId('block-board-svg')).toBeVisible()
  // the parasitic budget is on screen with its band
  const par = page.getByTestId('block-parasitics')
  await expect(par).toContainText('bypass mutual')
  await expect(par).toContainText('PE spine')
  await expect(page.getByTestId('block-il-chart')).toBeVisible()
  // the block IL chart actually RENDERS (had blanked to 'No data yet' on a tuple/object bug)
  await expect(page.locator('[data-test=block-il-chart] svg').first()).toBeVisible()

  // downloads: board, spice, s2p x2, bom, report — capture one and check
  // the bytes are the real artifact
  const [dl] = await Promise.all([
    page.waitForEvent('download'),
    page.getByTestId('dl-board').click(),
  ])
  expect(dl.suggestedFilename()).toBe('hertz-filter.kicad_pcb')
  const path = await dl.path()
  const fs = await import('node:fs')
  const text = fs.readFileSync(path, 'utf8')
  expect(text).toContain('(kicad_pcb')
  expect(text).toContain('REVIEW BEFORE FABRICATION')

  const [dlSpice] = await Promise.all([
    page.waitForEvent('download'),
    page.getByTestId('dl-spice').click(),
  ])
  const spice = fs.readFileSync(await dlSpice.path(), 'utf8')
  expect(spice).toContain('.subckt HERTZ_FILTER_BLOCK')
  expect(spice).toContain('Kbyp')

  for (const id of ['dl-s2p-dm', 'dl-s2p-cm', 'dl-bom', 'dl-report']) {
    const [d] = await Promise.all([
      page.waitForEvent('download'),
      page.getByTestId(id).click(),
    ])
    expect((await d.path()).length).toBeGreaterThan(0)
  }
  expect(consoleErrors).toEqual([])
})

test('an unreachable target reports honestly, never a silent success',
     async ({ page }) => {
  await designAndOpenBlockPane(page, { adm: '200' })
  await page.getByTestId('build-block').click()
  // 200 dB is beyond the catalog: the engine refuses with the catalog's words
  await expect(page.getByTestId('block-error')).toContainText('no realizable design',
                                                              { timeout: 30000 })
})

test('a Faraday handoff seeds the designer and flows to a generated board',
  async ({ page }) => {
    // The full pre-hardware chain: Faraday's PREDICTED spectra arrive in the
    // URL fragment, get judged against CISPR 32 B like any scan, seed the
    // binding sets — and one click later the filter has its own PCB.
    const payload = {
      v: 1, source: 'faraday', fSwHz: 500e3,
      bands: { dmDb: 10, cmDb: 15 },
      note: 'pre-hardware SEEDING estimate for the e2e',
      spectra: {
        // a failing comb: well above the CISPR 32 B QP line (~56-60 dBuV)
        dm: [[5e5, 95], [1.5e6, 90], [5e6, 82], [15e6, 74]],
        cm: [[5e5, 98], [1.5e6, 92], [5e6, 85], [15e6, 78]],
      },
    }
    const frag = Buffer.from(JSON.stringify(payload)).toString('base64')
    await page.goto('/#handoff=' + frag)
    await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/,
                                                            { timeout: 20_000 })
    // the reader consumed the fragment and jumped to the filter bench
    await expect(page.getByTestId('mode-filter')).toHaveClass(/active/)
    await expect(page).not.toHaveURL(/handoff/)

    // the binding sets seeded real per-mode requirements
    const acm = page.getByTestId('areq-cm')
    await expect(acm).not.toHaveValue('40')     // not the default
    const acmVal = Number(await acm.inputValue())
    expect(acmVal).toBeGreaterThan(20)

    // manual candidate ladders (no parts catalog in e2e), then the block
    await page.getByTestId('sec-comp').click()
    await page.getByTestId('lcm-source').selectOption('manual')
    await page.getByTestId('cx-source').selectOption('manual')
    await page.getByTestId('pane-select-b').selectOption('layout')
    await page.getByTestId('build-block').click()
    await expect(page.getByTestId('block-verdict')).toBeVisible({ timeout: 30000 })
    await expect(page.getByTestId('block-board-svg')).toBeVisible()
    // the preview is coloured BY CONDUCTOR CLASS (L / N / PE) — a net-aware legend
    // and at least two distinct trace colours (was one flat orange = "looks weird")
    await expect(page.getByTestId('board-legend')).toBeVisible()
    const strokes = await page.getByTestId('block-board-svg').locator('line').evaluateAll(
      (ls) => [...new Set(ls.map((l) => l.getAttribute('stroke')).filter((s) => s && s !== '#5a6a62'))])
    expect(strokes.length).toBeGreaterThan(1)
  })

test('a PASSING Faraday prediction says "no filter required" — never an '
     + 'unseeded designer in silence', async ({ page }) => {
    const payload = { v: 1, source: 'faraday', fSwHz: 5e5, bands: {},
      note: 'x', spectra: { dm: [[5e5, 20], [5e6, 15]],
                            cm: [[5e5, 22], [5e6, 18]] } }
    const frag = Buffer.from(JSON.stringify(payload)).toString('base64')
    await page.goto('/#handoff=' + frag)
    await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/,
                                                            { timeout: 20_000 })
    await expect(page.locator('[data-test="handoff-error"]'))
      .toContainText('no line filter is required')
    // and a garbage fragment reports itself instead of half-seeding
    await page.goto('/#handoff=not-base64!!!')
    await page.reload()
    await expect(page.locator('[data-test="handoff-error"]'))
      .toContainText('could not be read')
  })
