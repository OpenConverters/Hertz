import { defineConfig } from '@playwright/test'

export default defineConfig({
  testDir: 'tests',
  timeout: 60_000,
  use: { baseURL: 'http://127.0.0.1:4181', testIdAttribute: 'data-test' },
  webServer: {
    command: 'npx vite preview --port 4181 --strictPort',
    url: 'http://127.0.0.1:4181',
    reuseExistingServer: !process.env.CI,
  },
})
