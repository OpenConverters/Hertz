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
