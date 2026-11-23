# Nekotia for VS Code

Editor support for the [Nekotia](../../README.md) OS project.

## Features

- **`.neko` language** — C# highlighting plus the Nekotia sugar: `asm { }` blocks with
  instruction/register highlighting, `defer`, `unless`, `loop`, `for T i in a..b`, `@entry` / `@packed` / `@native(...)` / `@irq(n)`.
- **Language server** (`nekoc lsp`, started automatically): completion, hover with XML docs, signature help,
  go to definition — for `[Native]` methods also into the C++ runtime (F12 on `Mutex.Lock()` opens
  `Mutex::lock()` in `sync.cpp`), find references and rename across the kernel, the SDK and user programs,
  outline, workspace symbols, semantic highlighting (`[Native]` calls stand out), live diagnostics from Roslyn
  and from nekoc itself (`NEKO0xxx`: the supported C# subset) — all of it in `.neko` files too.
- **"C++" lens** above every method: opens the C++ nekoc generated for it.
- **Kernel debugger** — F5 (no `launch.json` needed) or the bug button in the status bar: builds the `-O0` kernel,
  boots it in QEMU, attaches gdb. Breakpoints in `.cs` and `.neko` (also conditions and hit counts), function
  breakpoints by C# name (`Commands.Dmesg`), CPUs as threads, a call stack with C# names, locals with C# types,
  registers, watch expressions, the serial console in the Debug Console, and exception filters that stop before the
  stop screen (kernel panic) or on every `throw`. Raw gdb in the Debug Console: `` `info registers ``.
  Needs only `gdb`; Microsoft's C/C++ extension is not required.
- **Commands** (Ctrl+Shift+P → "Nekotia"): Build Kernel, Run in QEMU (BIOS / UEFI), Run as Linux Process,
  Start QEMU Paused for gdb, Check, Show Desugared C#, Show Generated C++ for This Line.
- **Status bar** button `▶ Nekotia` → `make run`.
- **Hover docs** for every sugar keyword, **snippets**: `kentry`, `kclass`, `knative`, `knew`, `klog`, `asm`, `defer`, `forin`, `unless`, `loop`, `@packed`…

## Install

```sh
make vscode-ext          # builds build/vscode-nekotia-<version>.vsix
code --install-extension build/vscode-nekotia-0.3.0.vsix
```

Then reload the window (Ctrl+Shift+P → "Developer: Reload Window").
The language server is nekoc itself: `make tools`. After every `make tools` the extension restarts it on its own;
`Nekotia: Restart Language Server` does it by hand, and the **Nekotia** output channel shows its log.

## Settings

| Setting | Default | |
|---|---|---|
| `nekotia.languageServer.enabled` | `true` | run `nekoc lsp` |
| `nekotia.languageServer.csharpFiles` | `auto` | serve `.cs` too: `auto` = only without the C# extension (nekoc's own errors are shown in `.cs` either way) |
| `nekotia.codeLens` | `true` | the "C++" lens |
| `nekotia.debug.traceGdb` | `false` | log the debugger's gdb/MI traffic to the Nekotia output |
| `nekotia.checkOnSave` | `true` | without the server: run `nekoc check` on save |
| `nekotia.nekocPath` | `build/tools/nekoc/nekoc.dll` | relative to the folder with `nekotia.json` |
| `nekotia.dotnetPath` | `dotnet` | |

## Other editors

`nekoc lsp` speaks plain LSP over stdio. Neovim:

```lua
vim.filetype.add({ extension = { neko = 'cs' } })
vim.lsp.start({ name = 'nekoc', cmd = { 'dotnet', 'build/tools/nekoc/nekoc.dll', 'lsp' }, root_dir = vim.fs.root(0, 'nekotia.json') })
```

Helix (`languages.toml`): a language server `nekoc = { command = "dotnet", args = ["build/tools/nekoc/nekoc.dll", "lsp"] }`
for a language with `file-types = ["neko"]` and `roots = ["nekotia.json"]`.
