// Cross-view handoff: Spectrum's verdict seeds the filter designer.
import { reactive } from 'vue'

export const store = reactive({
  mode: 'spectrum',
  handoff: null, // { aReqDb, fSwHz? } from Spectrum's "design the fix"
})
