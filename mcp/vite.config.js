import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

// MCP App resources render in a deny-by-default CSP iframe, so each widget must
// be ONE self-contained file: no external script/style/font requests.
//
// The Vue plugin is here so the widgets can import the web app's real
// components (LogChart.vue, FilterSchematic.vue) straight out of ../web/src
// instead of reimplementing them — an engineer sees the same chart and the
// same schematic in Claude as on hertz.openconverters.com, from one definition.
//
// vite-plugin-singlefile inlines a single entry per build, so widgets are built
// one at a time via INPUT and emptyOutDir is off (the second build must not
// wipe the first).
export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  build: {
    outDir: "dist",
    emptyOutDir: false,
    rollupOptions: { input: process.env.INPUT || "spectrum.html" },
  },
});
