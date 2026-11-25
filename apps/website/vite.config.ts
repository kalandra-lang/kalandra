import { fileURLToPath } from "node:url";
import adapter from "@sveltejs/adapter-static";
import { sveltekit } from "@sveltejs/kit/vite";
import { tiramisuPlugin } from "@tiramisu-docs/kit/vite";
import tailwindcss from "@tailwindcss/vite";
import { defineConfig } from "vite";
import { kalandraHighlightPlugin } from "../../scripts/website-highlighting.ts";
import config from "./src/lib/tiramisu.config.ts";

export default defineConfig({
  resolve: {
    alias: {
      $lib: fileURLToPath(new URL("./src/lib", import.meta.url)),
    },
  },
  plugins: [
    tiramisuPlugin({ config, docsDir: "src/content/native" }),
    kalandraHighlightPlugin(),
    tailwindcss(),
    sveltekit({
      adapter: adapter({ pages: "build", assets: "build" }),
    }),
  ],
});
