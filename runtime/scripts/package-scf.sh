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

cat > "$PACKAGE_DIR/package.json" <<'JSON'
{
  "name": "@weclawbot/curator-scf",
  "version": "0.1.8",
  "private": true,
  "engines": {
    "node": ">=18"
  }
}
JSON

cat > "$PACKAGE_DIR/index.js" <<'JS'
exports.main_handler = async function main_handler(event, context) {
  const mod = await import("./src/adapters/tencent-scf.js");
  return mod.main_handler(event, context);
};
JS

cp "$ROOT/README.md" "$PACKAGE_DIR/README.md"
cp -R "$ROOT/src" "$PACKAGE_DIR/src"
cp -R "$ROOT/skills" "$PACKAGE_DIR/skills"
cat > "$PACKAGE_DIR/src/package.json" <<'JSON'
{
  "type": "module"
}
JSON

find "$PACKAGE_DIR" -name ".DS_Store" -delete

(cd "$PACKAGE_DIR" && zip -qr "$ZIP" .)

echo "$ZIP"
