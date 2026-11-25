import { fileURLToPath } from "node:url";
import adapter from "@sveltejs/adapter-static";
import { sveltekit } from "@sveltejs/kit/vite";
import { tiramisuPlugin } from "@tiramisu-docs/kit/vite";
import tailwindcss from "@tailwindcss/vite";
import { defineConfig, type Plugin } from "vite";
import { kalandraHighlightPlugin } from "../../scripts/website-highlighting.ts";
import config from "./src/lib/tiramisu.config.ts";

function tiramisuSvelteKit3Compat(): Plugin {
  return {
    name: "tiramisu-sveltekit3-compat",
    enforce: "pre",
    transform(code, id) {
      if (
        !id.includes("@tiramisu-docs/kit") ||
        !id.endsWith(".svelte") ||
        !code.includes("$app/stores")
      ) {
        return null;
      }
      return code
        .replace(
          /import\s*\{\s*page\s*\}\s*from\s*["']\$app\/stores["'];?/,
          'import { page } from "$app/state";',
        )
        .replaceAll("$page.", "page.");
    },
  };
}

export default defineConfig({
  resolve: {
    alias: {
      $lib: fileURLToPath(new URL("./src/lib", import.meta.url)),
    },
  },
  plugins: [
    tiramisuSvelteKit3Compat(),
    tiramisuPlugin({ config, docsDir: "src/content/native" }),
    kalandraHighlightPlugin(),
    tailwindcss(),
    sveltekit({
      adapter: adapter({ pages: "build", assets: "build" }),
    }),
  ],
});
