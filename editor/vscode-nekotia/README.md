# Nekotia for VS Code

Editor support for the [Nekotia](../../README.md) OS project.

## Features

- **`.neko` language** — C# highlighting plus the Nekotia sugar: `asm { }` blocks with
  instruction/register highlighting, `defer`, `unless`, `loop`, `for T i in a..b`, `@entry` / `@packed` / `@native(...)`.
- **nekoc diagnostics** on save (`NEKO0xxx` errors such as "foreach is not supported"), for `.neko` and `.cs`.
- **Commands** (Ctrl+Shift+P → "Nekotia"): Build Kernel, Run in QEMU (BIOS / UEFI), Run as Linux Process,
  Start QEMU Paused for gdb, Check, Show Desugared C#, Show Generated C++ for This Line.
- **Status bar** button `▶ Nekotia` → `make run`.
- **Hover docs** for every sugar keyword, **snippets**: `kentry`, `kclass`, `knative`, `knew`, `klog`, `asm`, `defer`, `forin`, `unless`, `loop`, `@packed`…

## Install

```sh
make vscode-ext          # builds build/vscode-nekotia-<version>.vsix
code --install-extension build/vscode-nekotia-0.1.0.vsix
```

Then reload the window (Ctrl+Shift+P → "Developer: Reload Window").
Diagnostics need nekoc: `make tools`.

## Settings

| Setting | Default | |
|---|---|---|
| `nekotia.checkOnSave` | `true` | run `nekoc check` on save |
| `nekotia.nekocPath` | `build/tools/nekoc/nekoc.dll` | relative to the folder with `nekotia.json` |
| `nekotia.dotnetPath` | `dotnet` | |

## Limitations

IntelliSense (completion, go-to-definition) for `.neko` files is not there yet: the C# language server only sees
`.cs`. The `kernel/Nekotia.Kernel.csproj` project desugars `.neko` files so `.cs` code can use their types.
A Nekotia language server is planned (ROADMAP, phase 7).
