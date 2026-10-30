import { defineConfig } from "@tiramisu-docs/kit";

export default defineConfig({
  title: "Kalandra",
  logo: {
    light: "/brand/kalandra-mark.svg",
    dark: "/brand/kalandra-mark-light.svg",
  },
  description: "A native language for CPU and GPU computing.",
  url: "https://kalandra.dev",
  sidebar: {
    groupOrder: [
      "Start here",
      "Tutorial",
      "Language reference",
      "Library reference",
      "HOWTO guides",
      "Using Kalandra",
      "Contributing",
    ],
  },
  nav: [
    { label: "Docs", href: "/docs" },
    { label: "GitHub", href: "https://github.com/kalandra-lang/kalandra" },
  ],
  footer: {
    copyright: "Kalandra · Apache-2.0",
    socials: { github: "https://github.com/kalandra-lang/kalandra" },
  },
});
