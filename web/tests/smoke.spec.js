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
