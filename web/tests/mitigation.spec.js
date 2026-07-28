import { test, expect } from '@playwright/test'

// The radiated screen is now the MEASURE · CM PROBE pane of the Filter bench —
// its CM-attenuation target and cable-ferrite pick are inputs to the design, so
// they live beside it rather than on a destination of their own.
async function openProbePane(page) {
  await page.getByTestId('mode-filter').click()
  await page.getByTestId('pane-select-a').selectOption('measure-probe')
}

// Ferrite catalog fixtures (shape of scripts/export_ferrites.py output): a main
// slice + a curves side file keyed by MPN. Curves span 10 MHz–1 GHz so they
// cover the synthetic comb's flagged band (≈30–300 MHz); resistive (im = 0) for
// a predictable pick. Stubbing these exercises the real fetch→build→pick path.
const FERRITES = {
  version: 1, count: 3,
  parts: [
    { mpn: 'FIX-S', manufacturer: 'Fixture', family: 'FIX', kind: 'cableCore', mountingForm: 'snapOn', cableMaxM: 0.01, zAt100MHzOhm: 180, zPeakOhm: 220, fPeakHz: 300e6, ratedCurrentA: null, dcrOhm: null },
    { mpn: 'FIX-M', manufacturer: 'Fixture', family: 'FIX', kind: 'cableCore', mountingForm: 'snapOn', cableMaxM: 0.014, zAt100MHzOhm: 330, zPeakOhm: 400, fPeakHz: 300e6, ratedCurrentA: null, dcrOhm: null },
    { mpn: 'FIX-L', manufacturer: 'Fixture', family: 'FIX', kind: 'cableCore', mountingForm: 'solidRing', cableMaxM: 0.02, zAt100MHzOhm: 560, zPeakOhm: 650, fPeakHz: 300e6, ratedCurrentA: null, dcrOhm: null },
  ],
}
const CURVES = {
  version: 1, count: 3,
  curves: {
    'FIX-S': { f: [10e6, 30e6, 100e6, 300e6, 1e9], re: [30, 90, 180, 220, 200], im: [0, 0, 0, 0, 0], rec: true },
    'FIX-M': { f: [10e6, 30e6, 100e6, 300e6, 1e9], re: [60, 170, 330, 400, 360], im: [0, 0, 0, 0, 0], rec: true },
    'FIX-L': { f: [10e6, 30e6, 100e6, 300e6, 1e9], re: [120, 300, 560, 650, 600], im: [0, 0, 0, 0, 0], rec: true },
  },
}

test('radiated screen emits a CM-attenuation target and picks a catalog ferrite', async ({ page }) => {
  await page.route('**/kelvin/hertz-ferrites.v1.json', (r) => r.fulfill({ json: FERRITES }))
  await page.route('**/kelvin/hertz-ferrites-curves.v1.json', (r) => r.fulfill({ json: CURVES }))
  const errors = []
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()) })
  await page.goto('/')
  await openProbePane(page)
  await page.getByTestId('radiated-example').click()
  // the synthetic comb fails the screen
  await expect(page.getByTestId('radiated-verdict')).toContainText('FAIL', { timeout: 20000 })
  // the mitigation panel: a governing CM-current target + a concrete ferrite pick
  const mit = page.getByTestId('mitigation')
  await expect(mit).toBeVisible()
  await expect(mit).toContainText('dB at')                     // governing requirement text
  const part = await page.getByTestId('mitigation-part').textContent()
  expect(part).toMatch(/FIX-[SML]/)                            // one of the stubbed catalog parts
  await expect(page.getByTestId('mitigation-meets')).toBeVisible()
  await expect(mit).toContainText('cable ferrite cores')       // real clamp-on cores, not beads
  await expect(page.getByTestId('mitigation-meta')).toContainText('mm cable')  // form + cable-fit surfaced
  console.log('PART:', part, '| MEETS:', await page.getByTestId('mitigation-meets').textContent())
  expect(errors, errors.join('\n')).toHaveLength(0)
})

test('an unreachable ferrite catalog is surfaced, not faked', async ({ page }) => {
  await page.route('**/kelvin/hertz-ferrites.v1.json', (r) => r.fulfill({ status: 404, body: '' }))
  await page.route('**/kelvin/hertz-ferrites-curves.v1.json', (r) => r.fulfill({ status: 404, body: '' }))
  await page.goto('/')
  await openProbePane(page)
  await page.getByTestId('radiated-example').click()
  await expect(page.getByTestId('radiated-verdict')).toContainText('FAIL', { timeout: 20000 })
  const mit = page.getByTestId('mitigation')
  await expect(mit).toContainText('dB at')                     // the target is still shown
  await expect(mit).toContainText('ferrite catalog unreachable')  // no invented pick
  await expect(page.getByTestId('mitigation-part')).toHaveCount(0)
})

test('cable-diameter fit excludes cores the cable will not thread through', async ({ page }) => {
  // fixture holes: FIX-S ≤10 mm, FIX-M ≤14 mm, FIX-L ≤20 mm.
  await page.route('**/kelvin/hertz-ferrites.v1.json', (r) => r.fulfill({ json: FERRITES }))
  await page.route('**/kelvin/hertz-ferrites-curves.v1.json', (r) => r.fulfill({ json: CURVES }))
  await page.goto('/')
  await openProbePane(page)
  await page.getByTestId('radiated-example').click()
  await expect(page.getByTestId('radiated-verdict')).toContainText('FAIL', { timeout: 20000 })

  // A 15 mm cable: FIX-S (10) and FIX-M (14) can't fit — only FIX-L (20) remains.
  await page.getByTestId('cable-od').fill('15')
  await page.getByTestId('cable-od').blur()
  await expect(page.getByTestId('mitigation-part')).toContainText('FIX-L')
  await expect(page.getByTestId('mitigation')).toContainText('fitting')   // exclusions surfaced

  // A 25 mm cable: nothing catalogued fits — surfaced, never faked with a too-small core.
  await page.getByTestId('cable-od').fill('25')
  await page.getByTestId('cable-od').blur()
  await expect(page.getByTestId('mitigation')).toContainText('not fitting a 25 mm cable')
  await expect(page.getByTestId('mitigation-part')).toHaveCount(0)
})
