// Build the dashboard into dist/ for embedding into the Go binary.
import * as esbuild from "esbuild";
import { cpSync, mkdirSync, rmSync } from "fs";

rmSync("dist", { recursive: true, force: true });
mkdirSync("dist", { recursive: true });

await esbuild.build({
  entryPoints: ["src/app.tsx"],
  bundle: true,
  minify: true,
  format: "iife",
  target: ["es2020"],
  jsx: "automatic",
  jsxImportSource: "preact",
  outfile: "dist/app.js",
  logLevel: "info",
});

cpSync("index.html", "dist/index.html");
cpSync("src/styles.css", "dist/styles.css");
cpSync("node_modules/uplot/dist/uPlot.min.css", "dist/uplot.css");

console.log("dashboard built -> dist/");
