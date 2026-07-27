import { test, expect } from '@playwright/test'

test('radiated screen emits a CM-attenuation target and picks a cable ferrite', async ({ page }) => {
  const errors = []
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()) })
  await page.goto('/')
  await page.getByTestId('mode-radiated').click()
  await page.getByTestId('radiated-example').click()
  // the synthetic comb fails the screen
  await expect(page.getByTestId('radiated-verdict')).toContainText('FAIL', { timeout: 20000 })
  // the mitigation panel: a governing CM-current target + a concrete ferrite pick
  const mit = page.getByTestId('mitigation')
  await expect(mit).toBeVisible()
  await expect(mit).toContainText('dB at')                     // governing requirement text
  await expect(page.getByTestId('mitigation-part')).not.toBeEmpty()
  await expect(page.getByTestId('mitigation-meets')).toBeVisible()
  console.log('PART:', await page.getByTestId('mitigation-part').textContent(),
             '| MEETS:', await page.getByTestId('mitigation-meets').textContent())
  expect(errors, errors.join('\n')).toHaveLength(0)
})
