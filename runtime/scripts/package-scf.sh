#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/dist"
PACKAGE_DIR="$DIST/scf-package"
ZIP="$DIST/weclawbot-curator-scf.zip"

cd "$ROOT"
npm run --silent eval

rm -rf "$PACKAGE_DIR" "$ZIP"
mkdir -p "$PACKAGE_DIR"

node --input-type=module - "$PACKAGE_DIR/package.json" <<'NODE'
import { readFileSync, writeFileSync } from "node:fs";

const output = process.argv[2];
const root = JSON.parse(readFileSync("package.json", "utf8"));
writeFileSync(output, `${JSON.stringify({
  name: root.name,
  version: root.version,
  private: true,
  engines: root.engines || { node: ">=22" },
  dependencies: root.dependencies || {},
}, null, 2)}\n`);
NODE

cat > "$PACKAGE_DIR/index.js" <<'JS'
exports.main_handler = async function main_handler(event, context) {
  const mod = await import("./src/adapters/tencent-scf.js");
  return mod.main_handler(event, context);
};
JS

cp "$ROOT/README.md" "$PACKAGE_DIR/README.md"
cp -R "$ROOT/src" "$PACKAGE_DIR/src"
cp -R "$ROOT/skills" "$PACKAGE_DIR/skills"
cp "$ROOT/package-lock.json" "$PACKAGE_DIR/package-lock.json"
cat > "$PACKAGE_DIR/src/package.json" <<'JSON'
{
  "type": "module"
}
JSON

(cd "$PACKAGE_DIR" && npm ci --omit=dev --ignore-scripts --no-audit --no-fund)

find "$PACKAGE_DIR" -name ".DS_Store" -delete

(cd "$PACKAGE_DIR" && zip -qr "$ZIP" .)

echo "$ZIP"
