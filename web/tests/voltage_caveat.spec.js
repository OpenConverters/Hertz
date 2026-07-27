import { test, expect } from '@playwright/test'

test('safety-cap voltage carries the AC/DC caveat as a hover tooltip', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByTestId('engine-led')).toHaveText(/engine ready/, { timeout: 20000 })
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('topology').selectOption('3ph')
  await page.getByTestId('sec-grid').click()
  await page.getByTestId('compute').click()
  await expect(page.getByTestId('lcm')).toBeVisible()
  await page.getByTestId('sch-CY1').click()            // Y-cap position -> safety-cap candidates
  const caveat = page.getByTestId('voltage-caveat').first()
  await expect(caveat).toBeVisible()
  await expect(caveat).toHaveText('*')
  await expect(caveat).toHaveAttribute('title', /AC mains rating/)
  console.log('TOOLTIP:', (await caveat.getAttribute('title')).slice(0, 60), '...')
})
