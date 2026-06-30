# Tencent SCF Build And Deploy

Tencent Cloud Function does not require WeClawBot to build packages on the
`weclawbot` server. A function can be packaged locally, in CI, on the
`weclawbot` host, or by Tencent's online dependency installer.

WeClawBot should still keep a Linux server or CI build path because WeChat
content is hard to predict. Attachment processing will eventually include
native packages, OCR tools, Office/PDF converters, and reproducible regression
tests. A Mac-only package is not a reliable production artifact.

The `weclawbot` server should be able to dynamically build and deploy SCF
variants from signed skill and extractor templates. Dynamic build means
expanding trusted runtime capabilities on demand; it does not mean deploying
arbitrary code generated from a user message.

## Policy

| Function | Build target | Reason |
| --- | --- | --- |
| Text curator | Zip from local, CI, or `weclawbot` | Pure Node.js runtime with LangGraph dependency bundled from lockfile |
| Voice transcript curator | Zip from local, CI, or `weclawbot` | Uses WeChat `voice_item.text`, no audio decoder |
| Image OCR | Linux CI or container image | OCR engines and image libraries often include native binaries |
| PDF extractor | Linux CI initially; image if native tools grow | Embedded-text extraction can be pure JS, OCR needs native tools |
| DOCX / PPTX / XLSX / CSV | Linux CI or `weclawbot`; image if conversion tools are needed | OOXML parsing can be pure JS, Office conversion is native-heavy |

The first shipping artifact should be a zip package for the text curator. As of
runtime `0.1.9`, the package includes `@langchain/langgraph` and production
dependencies inside the zip so SCF does not depend on online install at cold
start.
Attachment functions can move to container image deployment only when zip +
layers become awkward.

## Repository Shape

```text
runtime/
├── package.json
├── package-lock.json
├── src/
│   ├── runtime/
│   ├── skills/
│   └── adapters/
│       ├── tencent-scf.ts
│       ├── node-host.ts
│       └── offline-eval.ts
└── scripts/
    ├── build-scf.sh
    ├── test-runtime.sh
    └── package-scf.sh
```

`package-scf.sh` should produce:

```text
dist/weclawbot-curator-scf.zip
dist/weclawbot-attachment-scf.zip
```

The zip root must contain the SCF entry file and its runtime dependencies.
Secrets are never packaged. Tencent API keys, DeepSeek keys, device-signing
secrets, and model configuration are provided as cloud-function environment
variables.

## Build Steps

The build should be reproducible:

1. Install exact dependencies from the lockfile.
2. Run unit tests and JSON schema tests.
3. Run skill regression fixtures.
4. Copy runtime source and skills.
5. Install production dependencies from `package-lock.json`.
6. Create a zip with the entry file at the package root.
7. Optionally deploy with Tencent CLI or Serverless Cloud Framework.

## Online Dependency Install

Tencent SCF supports online dependency installation for Node.js, but WeClawBot
should not depend on it for production artifacts. Use it only for quick
experiments.

Reasons:

- reproducibility is weaker than lockfile-based CI packaging;
- install scripts are restricted for security;
- native dependencies are easier to debug in a controlled Linux build image;
- large attachment-processing dependencies should be tested before upload.

## Builder Role

The `weclawbot` host can be used as a lightweight builder and staging verifier:

- build zip packages from a clean checkout;
- run `offline-eval` against fixture files;
- smoke-test the HTTP handler locally;
- deploy to Tencent SCF when credentials are present.
- create versioned SCF variants for newly enabled skills or extractors.

It is not a required runtime hop. Once deployed, ESP32 devices call Tencent SCF
directly.

See [dynamic-scf-builder.md](dynamic-scf-builder.md) for the control-plane
design that creates SCF variants from trusted skill packages.

## Deployment Mode

Start with SCF code deployment for the text curator because it is tiny.

Use SCF image deployment later if attachment processing needs:

- system OCR binaries;
- LibreOffice or document conversion tools;
- larger native libraries;
- custom startup behavior;
- package sizes that make zip deployment uncomfortable.

## References

- [Tencent SCF Node.js deployment methods](https://cloud.tencent.com/document/product/583/67791)
- [Tencent SCF online dependency install](https://cloud.tencent.com/document/product/583/37920)
- [Tencent SCF deployment selection](https://cloud.tencent.com/document/product/583/73923)
