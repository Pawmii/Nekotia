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
  исключениях, ACPI (MADT, MCFG), LAPIC-таймер, IOAPIC, PCI/PCIe, VFS (ramfs + initrd + /dev), текстовая консоль
  во фреймбуфере, serial, старт ядра.
- **Логика ядра на C#** (`kernel/`) — подмножество C# без GC: `static class`, `struct`, `enum`, указатели, `unsafe`,
  `fixed`-буферы, `stackalloc`, перегрузка операторов, свойства, generic-методы (→ шаблоны C++), function pointers,
  интерполяция `$"..."` в `Write` (без аллокаций), обработчики прерываний `[Irq(n)]` и системных вызовов `[Syscall(n)]`.
  На C# написаны драйвер клавиатуры, shell (с `ls/cd/cat/mkdir/rm/mv/lspci`) и чтение консоли для программ.
- **Программы в ring 3** (`user/`) — тоже C#: nekoc с `"target": "user"` собирает их в статические ELF, Makefile
  упаковывает их в initrd (`/bin` внутри tar), ядро распаковывает его в VFS и запускает программы в отдельных адресных
  пространствах. У процесса есть файловые дескрипторы и рабочий каталог. Падение программы убивает только её.
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

Кликните в окно QEMU и печатайте. Команды: `help`, `info`, `ps`, `run PROG ARGS` (или просто имя программы; `&` в конце —
в фоне), `progs`, `kill PID`, `spawn spinner|primes|burn|counter`, `mem`, `vm`, `pages`, `alloc MB`, `uptime`, `echo`, `sleep MS`, `irq`, `layout`,
`colors`, `neko` (котик на весь экран), `selftest`, `clear`, `panic`, `crash KIND` (проверка экрана смерти),
`reboot`, `shutdown`. Клавиши: **Alt+Shift** — EN/RU, **↑/↓** — история, **Tab** — дополнение команды,
**Ctrl+L** — очистить, **Ctrl+U** — стереть строку, **Ctrl+C** — отменить.

Всё это — C#-код: драйвер клавиатуры `kernel/Drivers/Keyboard.neko` (обработчик `@irq(1)`), shell — `kernel/Shell/`.

### Программы

```
neko en> hello world 42
Hello from ring 3!
I am a C# program running as process 8, 5125 ms after boot.
arguments: 2 — world, 42
neko en> guess
I am thinking of a number between 1 and 100.
guess #1: 50
lower
```

Программа — каталог `user/NAME` с `nekotia.json` (`"target": "user"`) и C#-кодом со `static int Main(int argc, byte** argv)`;
API — `sdk/Nekotia.User` (`Console`, `Process`, `File`, `Path`, `Memory`, `Time`). `make` сам найдёт новый каталог,
соберёт ELF и положит его в `/bin` initrd (`build/initrd.tar`, туда же попадает всё из `initrd/`). Программы: `sh`
(shell в ring 3: `cd`, `pwd`, `> FILE`, `>> FILE`, `< FILE`, конвейеры `a | b`, запуск по пути), `ls`, `cat`, `cp`, `mkdir`, `rm`, `mv`, `grep`, `wc`, `head`, `ps`,
`echo`, `kill`, `primes` (простые числа в N потоках — видно ускорение на нескольких CPU), `hello`, `guess` (читает
клавиатуру), `usertest` (проверки ABI для `make test`).
**Ctrl+C** завершает программу на переднем плане; `init=sh` в `cmdline` (`boot/limine.conf`) загружает сразу в `sh`.

```
neko en /> sh
sh /$ primes 3000000 4
216816 primes below 3000000
1 thread: 805 ms, 4 threads: 211 ms — x3.82
sh /$ cd /tmp
sh /tmp$ echo hello > a.txt
sh /tmp$ ls /bin | grep s | wc
       6      12     150
sh /tmp$ cat a.txt
hello
```

Файлы живут в памяти (ramfs): `/bin`, `/etc`, `/home` приходят из initrd (файлы initrd читаются прямо из модуля,
пока их не изменят), `/dev` — устройства `console`, `null`, `zero` и диски, `/tmp` — пустой каталог для своих файлов.

**Диск.** `make run` подключает `build/disk.img` (64 МиБ) как virtio-диск `/dev/vda`. Один раз отформатируйте его
из shell ядра — `mkfs vda`, `mount vda /mnt`; дальше ядро само монтирует его в `/mnt` при загрузке. Всё, что
записано в `/mnt`, остаётся на диске между запусками (FAT32 с длинными именами — образ читается и в Linux:
`sudo mount -o loop build/disk.img /media`). `cp`/`mv` переносят файлы между `/tmp` и `/mnt`, программы можно
запускать прямо с диска (`./prog`). `scripts/fatcheck.py build/disk.img` проверяет образ без dosfstools.

## Команды

| Команда | Что делает |
|---|---|
| `make` / `make kernel` | `build/nekotia.elf` |
| `make gen` | только транспиляция → `build/gen/nekotia.gen.{hpp,cpp}` |
| `make check` | диагностика nekoc без сборки (ядро и программы) |
| `make user` | только программы → `build/user/bin/*` |
| `make initrd` | `build/initrd.tar`: программы в `/bin` + содержимое `initrd/` |
| `make iso` | загрузочный гибридный BIOS/UEFI ISO |
| `make run` / `make run-uefi` | запуск в QEMU (`QEMU_FLAGS=...` чтобы поменять); диск `build/disk.img` (64 МиБ, `DISK=`, `DISK_SIZE=`) — `/dev/vda` |
| `make debug` | отладочная `-O0`-сборка в `build/debug`, QEMU на паузе с gdb-сервером `:1234` |
| `make test` | загрузить тестовый образ в QEMU (BIOS + UEFI), прогнать самотесты, код возврата = результат |
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

Текст дублируется в serial. **R** — перезагрузка. Проверить все варианты:
`crash null|div|throw|custom|assert|pf|gp|ud|bp|double-free|stack|nx|wp|deadlock|relock` (`stack` — переполнение стека
в guard-страницу, `nx` — выполнение кода из кучи, `wp` — запись в код ядра, `deadlock` — два потока ждут mutex друг друга).
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
sdk/Nekotia.Sdk/        C# API ядра: атрибуты, Asm, Terminal/Serial/Cpu/Port/Time/Irq/Power/SysInfo, Heap/Pages/Memory, Process
sdk/Nekotia.User/       C# API программ: Console, Process, Thread, SpinLock, Atomic, Memory, Time
user/                   программы (C#, ring 3): sh, ls, ps, echo, kill, primes, hello, guess, usertest
                        + Nekotia.User.Programs.csproj (только для IDE)
runtime/user/           рантайм программ: crt0, системные вызовы, вывод, куча, linker.ld
runtime/include/neko/   C++ заголовки, которые использует сгенерированный код
runtime/src/core/       куча, buddy-аллокатор, консоль (PSF/UTF-8/ANSI), экран смерти, вывод, планировщик, процессы и ELF
runtime/src/arch/x86_64 Limine boot, GDT/TSS, IDT + isr.S, ACPI, APIC/таймер/IRQ, vmm (таблицы страниц), syscall, COM1, linker.ld
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
  PS/2 клавиатура с EN/RU, командная строка, экран смерти.
- **Фаза 3** (основное): свои таблицы страниц с NX/W^X, buddy-аллокатор, растущая куча, стек ядра с guard-страницей.
- **Фаза 4**: потоки, вытесняющий планировщик с приоритетами на нескольких CPU (SMP, QEMU `-smp 4`),
  Mutex/Semaphore/SpinLock, обнаружение deadlock, FPU/SSE (`float`/`double` в C#), `make test`.

- **Фаза 5** (основное): ring 3, `syscall`/`sysret`, процессы со своими адресными пространствами и несколькими потоками,
  загрузчик ELF, `spawn`/`wait`/`exit`/`kill`, Ctrl+C, SMEP/SMAP, программы на C# (`"target": "user"`), системные вызовы
  на C# (`[Syscall(n)]`), изоляция падений, shell и утилиты в userspace.

Дальше — драйверы и файловые системы (фаза 6): PCI, virtio-blk, VFS, initrd, FAT32. См. [ROADMAP.md](ROADMAP.md).
