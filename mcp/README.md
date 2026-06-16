# arkdecompiler-mcp

An MCP server that exposes HarmonyOS NEXT decompilation (`.abc` / `.hap` / `.hsp` / `.app` → ArkTS)
through the [arkdecompiler](https://github.com/jd-opensource/arkdecompiler) `xabc` engine.

## What it adds over raw `xabc`

`xabc` only decompiles a single `.abc` file given on the command line. This server adds:

- **HAP/app unpacking** — `.hap`/`.hsp`/`.app` are ZIP archives; the server extracts every
  `.abc` (e.g. `ets/modules.abc`), reads `module.json`, and decompiles **per module**.
- **Isolation** — each decompile runs in its own temp CWD (xabc hard-codes the AST output name
  `arkdemo.ast` and writes `logs/`), so multiple modules don't clobber each other.
- **MCP tools** for use from Claude Code / Claude Desktop / any MCP client.

## Tools

| Tool | Purpose |
|------|---------|
| `backend_info` | Report active backend and whether xabc is wired up |
| `inspect_package` | List modules + abc files inside a hap/app (no decompile) |
| `decompile_abc` | Decompile one `.abc` → ArkTS (optional AST) |
| `decompile_package` | Decompile a whole hap/app per module; optionally write `.ts` to disk |
| `disassemble_abc` | Disassemble `.abc` → Panda assembly (works even if decompile fails) |

## Backends

Selected via env var `ARKDEC_BACKEND`:

- `mock` (default) — placeholder output; lets you wire up & test the MCP client with no binary.
- `local` — call a natively-built `xabc`. Set:
  - `ARKDEC_XABC` = path to `xabc`
  - `ARKDEC_DISASM` = path to `ark_disasm`
  - `ARKDEC_LD_LIBRARY_PATH` = `…/runtime_core:…/zlib`
- `docker` — run `xabc` inside the built image. Set `ARKDEC_DOCKER_IMAGE` (default `arkdecompiler:latest`).

## Install & run

```bash
cd mcp
python -m venv .venv && . .venv/bin/activate
pip install -e .
ARKDEC_BACKEND=mock arkdecompiler-mcp        # stdio MCP server
```

## Register in Claude Code

```bash
claude mcp add arkdecompiler -- \
  env ARKDEC_BACKEND=docker ARKDEC_DOCKER_IMAGE=arkdecompiler:latest \
  /path/to/mcp/.venv/bin/arkdecompiler-mcp
```

(For the `local` backend, pass `ARKDEC_XABC` / `ARKDEC_DISASM` / `ARKDEC_LD_LIBRARY_PATH` instead.)

## Tests

```bash
. .venv/bin/activate && pip install -e ".[dev]" && pytest
```

All tests run on the `mock` backend, so they need no native binary.
