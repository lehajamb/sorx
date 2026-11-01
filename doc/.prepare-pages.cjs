const fs = require("node:fs");
const path = require("node:path");

const commit = process.argv[2];
if (!/^[0-9a-f]{40}$/i.test(commit || "")) {
  throw new Error("Usage: node doc/.prepare-pages.cjs <40-character commit SHA>");
}

const root = path.resolve(__dirname, "..");
const output = path.join(root, "build-pages");
const files = ["index.html", "sorx-docs.css", "sorx-docs.js"];
const sourceBase = `https://github.com/lehajamb/sorx/blob/${commit}/`;
let sourceLinks = 0;

// Keep repository-relative links working locally; only the published copy changes.
const prepared = files.map((name) => {
  let content = fs.readFileSync(path.join(__dirname, name), "utf8");
  if (name.endsWith(".html") || name.endsWith(".js")) {
    content = content.replace(
      /(["'])\.\.\/((?:include|src|tests|configs|examples|docs)\/[^"']+)\1/g,
      (_, quote, target) => {
        const source = path.resolve(root, target);
        if (!source.startsWith(root + path.sep) || !fs.statSync(source).isFile()) {
          throw new Error(`Invalid repository source link: ${target}`);
        }
        sourceLinks += 1;
        return quote + sourceBase + target + quote;
      },
    );
    if (/(["'])\.\.\//.test(content)) {
      throw new Error(`Unconverted repository-relative link in ${name}`);
    }
  }
  return [name, content];
});

// Refuse unexpected output entries instead of publishing stale files or deleting them.
if (fs.existsSync(output)) {
  if (fs.lstatSync(output).isSymbolicLink() || !fs.statSync(output).isDirectory()) {
    throw new Error("build-pages must be a regular directory");
  }
  for (const entry of fs.readdirSync(output, { withFileTypes: true })) {
    if (!files.includes(entry.name) || !entry.isFile()) {
      throw new Error(`Unexpected build-pages entry: ${entry.name}`);
    }
  }
} else {
  fs.mkdirSync(output);
}
for (const [name, content] of prepared) {
  fs.writeFileSync(path.join(output, name), content);
}
console.log(`Prepared ${files.length} Pages files with ${sourceLinks} source links pinned to ${commit}.`);
