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
  coalescing, проверка заголовков, детект double free), физический page allocator, GDT/TSS/IDT с дампом регистров на
  исключениях, ACPI (MADT), LAPIC-таймер, IOAPIC, текстовая консоль во фреймбуфере, serial, старт ядра.
- **Логика ядра на C#** (`kernel/`) — подмножество C# без GC: `static class`, `struct`, `enum`, указатели, `unsafe`,
  `fixed`-буферы, `stackalloc`, перегрузка операторов, свойства, generic-методы (→ шаблоны C++), function pointers,
  интерполяция `$"..."` в `Write` (без аллокаций), обработчики прерываний `[Irq(n)]`. На C# написаны драйвер клавиатуры и shell.
- **Свой сахар** (`.neko` файлы) — `asm { }`, `defer`, `unless`, `loop`, `for T i in a..b`, `@entry`/`@packed`/`@native(...)`/`@irq(n)`.
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

На экране — консоль ядра (шрифт Terminus, кириллица, цвета), лог дублируется в serial-терминал:

```
[neko] runtime: x86_64, Limine protocol, framebuffer console
[neko] heap 16 MiB, 60962 free pages, 1 CPU(s) @ 3919 MHz — entering C# kernel

   /\_/\
  ( o.o )   Nekotia 0.2.0
   > ^ <    kernel in C# → C++ (nekoc) → x86_64

selftest: ok — 21 passed, 0 failed
Type help for commands. Alt+Shift switches EN/RU, ↑/↓ history, Tab completes.

neko en> info
cpu:        QEMU Virtual CPU version 2.5+
memory:     254 MiB usable of 255 MiB
...
```

### Командная строка ядра

Кликните в окно QEMU и печатайте. Команды: `help`, `info`, `mem`, `uptime`, `echo`, `sleep MS`, `irq`, `layout`,
`colors`, `neko` (котик на весь экран), `selftest`, `clear`, `panic`, `crash KIND` (проверка экрана смерти),
`reboot`, `shutdown`. Клавиши: **Alt+Shift** — EN/RU, **↑/↓** — история, **Tab** — дополнение команды,
**Ctrl+L** — очистить, **Ctrl+U** — стереть строку, **Ctrl+C** — отменить.

Всё это — C#-код: драйвер клавиатуры `kernel/Drivers/Keyboard.neko` (обработчик `@irq(1)`), shell — `kernel/Shell/`.

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

**Экран смерти.** Любое фатальное событие — необработанный `throw`, `Debug.Assert`/`Debug.Panic`, повреждение кучи,
нехватка памяти, исключение процессора — показывает экран «NEKOTIA HAS STOPPED»:

- **STOP-код** и имя: `0x0000000E PAGE_FAULT`, `0x000000E0 UNHANDLED_EXCEPTION`, `0x000000E1 ASSERTION_FAILED`,
  `0x000000E2 KERNEL_PANIC`, `0x000000E3 HEAP_CORRUPTION`, `0x000000E4 OUT_OF_MEMORY` (коды 0x00–0x1F — векторы CPU);
- **тип исключения** — для аппаратных ошибок в терминах .NET: `#DE` → `DivideByZeroException`, `#PF` около нуля →
  `NullReferenceException`, `#GP` → `AccessViolationException`, `#UD` → `InvalidProgramException`, `#DF` → `StackOverflowException`;
- **сообщение, место** (`Nekotia.OS.Commands.Crash at kernel/Shell/Commands.neko:190`), для `#PF` — адрес и причина;
- **стек вызовов** с C#-методами и строками `.cs`/`.neko` (таблица символов вшивается в ядро вторым проходом линковки,
  `scripts/gen-symbols.py`), **регистры** с `cr0–cr4`, **состояние системы** (uptime, куча, страницы, CPU).

Текст дублируется в serial. **R** — перезагрузка. Проверить все варианты: `crash null|div|throw|custom|assert|pf|gp|ud|bp|double-free`.
`addr2line -e build/nekotia.elf <адрес>` тоже указывает **на строку в C#-файле** — nekoc пишет `#line` перед каждым оператором.
- **F5 в VS Code** (нужно расширение C/C++ `ms-vscode.cpptools`): собирает `-O0`-ядро, запускает QEMU с gdb-сервером
  и подключается. Breakpoint'ы ставятся кликом прямо в `.cs` и `.neko`, видны стек вызовов и переменные.
  Окно QEMU до подключения отладчика пишет «guest has not initialized the display» — это нормально.
- Без VS Code: `make debug`, затем `gdb build/debug/nekotia.elf -ex 'target remote :1234'`, `break Graphics.neko:29`, `continue`.
  В обычной `-O2`-сборке строки C# могут не совпадать с кодом — для отладки используйте `make debug`.
- *Nekotia: Show Generated C++ for This Line* открывает C++, в который превратилась текущая строка.

## Структура

```
kernel/                 ядро на C# (.cs и .neko) + Nekotia.Kernel.csproj (только для IDE)
kernel/Drivers/         драйверы на C# (PS/2 клавиатура)
kernel/Shell/           командная строка ядра
sdk/Nekotia.Sdk/        C# API ядра: атрибуты, Asm, Terminal/Serial/Cpu/Port/Time/Irq/Power/SysInfo, Heap/Pages/Memory
runtime/include/neko/   C++ заголовки, которые использует сгенерированный код
runtime/src/core/       куча, page allocator, консоль (PSF/UTF-8/ANSI), вывод, panic (общие для всех платформ)
runtime/src/arch/x86_64 Limine boot, GDT/TSS, IDT + isr.S, ACPI, APIC/таймер/IRQ, vmm (MMIO), COM1, linker.ld
runtime/assets/fonts/   шрифт консоли Terminus (OFL 1.1)
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

- **Фаза 0** (фундамент): загрузка BIOS/UEFI, куча, page allocator, исключения, транспилятор с диагностикой, сахар, VS Code.
- **Фаза 2** (основное): своя GDT/TSS с IST, ACPI MADT, LAPIC-таймер 1 кГц, IOAPIC, IRQ из C#, консоль во фреймбуфере,
  PS/2 клавиатура с EN/RU, командная строка.

Дальше — свои таблицы страниц (фаза 3), потоки и планировщик (фаза 4), userspace (фаза 5). См. [ROADMAP.md](ROADMAP.md).
