import { sveltekit } from "@sveltejs/kit/vite";
import { tiramisuPlugin } from "@tiramisu-docs/kit/vite";
import tailwindcss from "@tailwindcss/vite";
import { defineConfig } from "vite";
import { kalandraHighlightPlugin } from "../../scripts/website-highlighting";
import config from "./src/lib/tiramisu.config";

export default defineConfig({
  plugins: [
    tiramisuPlugin({ config, docsDir: "src/content/native" }),
    kalandraHighlightPlugin(),
    tailwindcss(),
    sveltekit(),
  ],
});
