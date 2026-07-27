import '@fontsource/ibm-plex-sans/400.css'
import '@fontsource/ibm-plex-sans/600.css'
import '@fontsource/ibm-plex-mono/400.css'
import '@fontsource/rajdhani/600.css'
import '@fontsource/rajdhani/700.css'
import './style.css'
import { createApp } from 'vue'
import App from './App.vue'
import { initTelemetry } from './telemetry.js'

// Reused OpenMagnetics Umami instance, served same-origin under /stats by the
// hertz vhost; production-only (never fires on localhost or the dev server).
const UMAMI_WEBSITE_ID = '7b771b7f-4531-4775-aa8b-7ed05b2a854d' // hertz.openconverters.com (OM Umami)
initTelemetry({ site: 'hertz', umamiWebsiteId: UMAMI_WEBSITE_ID, appVersion: '0.1.0' })

createApp(App).mount('#app')
