# Nekotia

```
   /\_/\    Nekotia — операционная система, ядро которой пишется на C#
  ( o.o )   и транслируется в C++ (nekoc), а затем собирается clang'ом
   > ^ <    в обычное freestanding-ядро x86_64.
```

Nekotia — настоящее ядро: грузится Limine'ом на «голом железе» или в QEMU (BIOS и UEFI),
работает в long mode в higher half, сам управляет памятью и прерываниями. В итоговом образе нет ни .NET runtime,
ни GC, ни libc: C# здесь — только язык исходников.

```csharp
// kernel/Main.neko — .neko = C# + сахар Nekotia (defer, asm, for-in…)
using Nekotia;
using Nekotia.Hal;
using Nekotia.Mem;

namespace Nekotia.OS;

public static unsafe class Kernel
{
    [KernelEntry]
    public static void Start(BootInfo* info)
    {
        Serial.WriteLine("Hello from C# in ring 0!");

        Node* n = Heap.New<Node>();       // C++ аллокатор ядра
        defer Heap.Delete(n);             // сахар .neko — освобождение при выходе из scope

        while (true)
            Cpu.Halt();
    }
}
```

## Как это устроено

```
 kernel/*.cs, *.neko        sdk/Nekotia.Sdk (C# API: Serial, Cpu, Port, Heap, Pages…)
          │                         │
          └──────────┬──────────────┘
                     ▼
     nekoc  (tools/nekoc, .NET + Roslyn)
       1. .neko → C#            (десугаринг: asm, defer, unless, loop, for-in, @attr)
       2. Roslyn: разбор + семантика, проверка подмножества языка (NEKO0xxx ошибки)
       3. C# → C++20            (build/gen/nekotia.gen.{hpp,cpp}, с #line на исходный C#)
                     │
                     ▼
     clang++ --target=x86_64-unknown-none-elf -ffreestanding -mcmodel=kernel …
       + runtime/ (C++): boot (Limine), IDT/исключения, COM1, куча, page allocator, libc-шимы
                     │
                     ▼
     ld.lld → build/nekotia.elf → xorriso + Limine → build/nekotia.iso → QEMU / железо
```

- **База на C++** (`runtime/`) — всё, что чувствительно к памяти и железу: аллокатор кучи (first-fit free list,
  coalescing, проверка заголовков, детект double free), физический page allocator, IDT с дампом регистров на исключениях,
  serial, старт ядра.
- **Логика ядра на C#** (`kernel/`) — подмножество C# без GC: `static class`, `struct`, `enum`, указатели, `unsafe`,
  `fixed`-буферы, `stackalloc`, перегрузка операторов, свойства, generic-методы (→ шаблоны C++), function pointers.
- **Свой сахар** (`.neko` файлы) — `asm { }`, `defer`, `unless`, `loop`, `for T i in a..b`, `@entry`/`@packed`/`@native(...)`.
  Подсветка и подсказки — в VS Code-расширении (`editor/vscode-nekotia`).

Подробности: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), язык и сахар: [docs/LANGUAGE.md](docs/LANGUAGE.md),
план: [ROADMAP.md](ROADMAP.md).

## Быстрый старт (Arch Linux)

```sh
scripts/setup-arch.sh     # dotnet-sdk, clang, lld, limine, xorriso, qemu, edk2-ovmf, gdb
make doctor               # проверить тулчейн

make run                  # C# → C++ → ELF → ISO → QEMU (BIOS), serial в этот терминал
make run-uefi             # то же через OVMF
```

Ожидаемый вывод:

```
[neko] runtime: x86_64, Limine protocol
[neko] heap 16 MiB, 61025 free pages — entering C# kernel
   /\_/\    Nekotia
  ( o.o )   kernel 0.1.0
...
selftest: 19 passed, 0 failed
graphics: 1280x800 framebuffer
kernel: idle (hlt loop)
```

В окне QEMU ядро рисует градиент и котика во фреймбуфере.

## Команды

| Команда | Что делает |
|---|---|
| `make` / `make kernel` | `build/nekotia.elf` |
| `make gen` | только транспиляция → `build/gen/nekotia.gen.{hpp,cpp}` |
| `make check` | диагностика nekoc без сборки |
| `make iso` | загрузочный гибридный BIOS/UEFI ISO |
| `make run` / `make run-uefi` | запуск в QEMU (`QEMU_FLAGS=...` чтобы поменять) |
| `make debug` | отладочная `-O0`-сборка в `build/debug`, QEMU на паузе с gdb-сервером `:1234` |
| `make run-host` | ядро как Linux-процесс с ASan/UBSan — быстрый прогон самотестов (не замена QEMU) |
| `make tools` | собрать nekoc |
| `make vscode-ext` | собрать `.vsix` расширения |
| `make clean` | удалить `build/` |

Параметры: `HEAP_SIZE=33554432` (размер кучи ядра), `OPT=-O0`, `CXX=...`. После смены флагов — `make clean`.

## VS Code

```sh
make vscode-ext
code --install-extension build/vscode-nekotia-0.1.0.vsix
```

После установки: подсветка `.neko` (включая инструкции/регистры в `asm { }`), сниппеты (`kentry`, `kclass`, `forin`, `defer`…),
ошибки nekoc при сохранении, команды *Nekotia: Run in QEMU*, *Show Desugared C#*, *Show Generated C++ for This Line*,
кнопка `▶ Nekotia` в статус-баре. `Nekotia.slnx` даёт IntelliSense для `.cs` (C# Dev Kit / C# extension);
проекты `.csproj` нужны только редактору — ядро ими не собирается.

## Отладка

- Исключение CPU печатает регистры (для `#PF` — `cr2` и причину) и паникует.
  `addr2line -e build/nekotia.elf <rip>` указывает **на строку в C#-файле** — nekoc пишет `#line` перед каждым оператором.
- **F5 в VS Code** (нужно расширение C/C++ `ms-vscode.cpptools`): собирает `-O0`-ядро, запускает QEMU с gdb-сервером
  и подключается. Breakpoint'ы ставятся кликом прямо в `.cs` и `.neko`, видны стек вызовов и переменные.
  Окно QEMU до подключения отладчика пишет «guest has not initialized the display» — это нормально.
- Без VS Code: `make debug`, затем `gdb build/debug/nekotia.elf -ex 'target remote :1234'`, `break Graphics.neko:29`, `continue`.
  В обычной `-O2`-сборке строки C# могут не совпадать с кодом — для отладки используйте `make debug`.
- *Nekotia: Show Generated C++ for This Line* открывает C++, в который превратилась текущая строка.

## Структура

```
kernel/                 ядро на C# (.cs и .neko) + Nekotia.Kernel.csproj (только для IDE)
sdk/Nekotia.Sdk/        C# API ядра: атрибуты, Asm, Serial/Cpu/Port, Heap/Pages/Memory, BootInfo
runtime/include/neko/   C++ заголовки, которые использует сгенерированный код
runtime/src/core/       куча, page allocator, вывод, panic (общие для всех платформ)
runtime/src/arch/x86_64 Limine boot, IDT + isr.S, COM1, linker.ld
runtime/src/freestanding memcpy/memset…, operator new/delete
runtime/src/host/       host-режим (ядро как Linux-процесс)
runtime/third_party/    limine.h (0BSD)
tools/nekoc/            транспилятор C# → C++ (Roslyn)
editor/vscode-nekotia/  VS Code расширение (JS без зависимостей)
boot/limine.conf        конфигурация загрузчика
scripts/                setup-arch.sh, doctor.sh, fetch-limine.sh, package-vscode-extension.py
nekotia.json            проект nekoc: исходники и каталог вывода
```

## Статус

Фаза 0 (фундамент) готова и проверена в QEMU (BIOS и UEFI): загрузка, куча, page allocator, исключения, framebuffer,
транспилятор с диагностикой, сахар, VS Code-расширение. Дальше — таймер и прерывания устройств, свои page tables,
потоки, userspace. См. [ROADMAP.md](ROADMAP.md).
