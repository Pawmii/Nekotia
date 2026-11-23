# Архитектура Nekotia

## Конвейер сборки

```
nekotia.json ──► nekoc build
                  │ ProjectConfig: собрать *.cs / *.neko из sdk/ и kernel/ (без bin/ obj/)
                  │ Desugarer:     .neko → C# (CodeMask пропускает строки и комментарии, строки не сдвигаются)
                  │ Roslyn:        CSharpCompilation против System.Private.CoreLib (только для атрибутов и примитивов)
                  │                ошибки CS* → стоп
                  │ CppEmitter:    символы + семантическая модель → C++
                  ▼
        build/gen/nekotia.gen.hpp   forward-декларации → enum → struct (топосорт по полям) → static class
        build/gen/nekotia.gen.cpp   статические поля → тела методов (#line перед каждым оператором) → neko_kernel_main
                  │
clang++ (freestanding, -mcmodel=kernel, -mno-red-zone, без SSE) ──► *.o
                  │  + runtime/src/core, freestanding, arch/x86_64 (boot.cpp, idt.cpp, isr.S, serial.cpp)
ld.lld -T runtime/src/arch/x86_64/linker.ld ──► build/nekotia.elf
xorriso + limine bios-install ──► build/nekotia.iso (гибрид BIOS/UEFI)
```

## nekoc (`tools/nekoc`)

| Файл | Роль |
|---|---|
| `Program.cs` | CLI: `build`, `check`, `desugar`, `version`; `--format json` для редактора |
| `ProjectConfig.cs` | чтение `nekotia.json` |
| `Sugar/Desugarer.cs`, `Sugar/CodeMask.cs` | переписывание сахара в C# |
| `Compiler/Driver.cs` | разбор, компиляция Roslyn, запуск эмиттера, запись файлов (только при изменении) |
| `Compiler/CppEmitter*.cs` | генерация: объявления, операторы, выражения |
| `Compiler/TypeMapper.cs` | C#-тип → C++-тип |
| `Compiler/Symbols.cs` | атрибуты SDK, экранирование имён (ключевые слова C++ → `name_`) |
| `Compiler/Diagnostics.cs` | диагностики в формате MSBuild (`$msCompile`) и JSON |

Ключевые решения:

- **Один translation unit** для всего C#-кода: инлайнинг между модулями, шаблоны без явных инстанциаций,
  простой порядок объявлений. При росте ядра — разбиение по файлам (фаза 1).
- **Семантика C# важнее красоты C++**: неявные числовые преобразования, которые в C++ дали бы другой результат, становятся
  явными кастами; аргументы перегруженных методов приводятся к типам выбранной Roslyn'ом перегрузки.
- **`#line`**: каждое C#-выражение-оператор помечается исходной строкой — clang-ошибки, `addr2line` и gdb работают в терминах C#.
- **Сахар — препроцессор, а не новый парсер**: Roslyn остаётся единственным источником истины о C#; `defer` кодируется
  валидным C# (`if (Sugar.Defer) stmt;`) и распознаётся эмиттером по символу.

## Рантайм (`runtime/`)

```
include/neko/types.hpp    fnptr, defer guard, NEKO_ENUM_OPERATORS, NEKO_ASM*
include/neko/boot.hpp     BootInfo, Framebuffer (поля = поля C#-структур SDK)
include/neko/cpu.hpp      hlt/cli/sti, in/out, cr2/cr3, rdtsc (в host-режиме — заглушки)
include/neko/memory.hpp   mem::*, heap::*, pmm::*
include/neko/serial.hpp   serial::*, str::*
src/core/                 heap.cpp, pmm.cpp, print.cpp (+ panic)
src/freestanding/         memcpy/memset/memmove/memcmp, new/delete, __cxa_* заглушки
src/arch/x86_64/          boot.cpp (kmain), idt.cpp + isr.S, serial.cpp, linker.ld
src/host/                 host_main.cpp — платформа «Linux-процесс»
```

### Загрузка (x86_64)

1. Limine (BIOS или UEFI) загружает ELF по адресу `0xffffffff80000000`, включает long mode и paging, отображает
   физическую память в HHDM, даёт 64 KiB стека и передаёт управление `kmain`.
2. `kmain`: COM1 → base revision → своя GDT/TSS (IST для #DF/NMI/#MC) → IDT (256 векторов) → память (куча, фреймы) →
   framebuffer-консоль → ACPI (RSDP → XSDT → MADT) → APIC: 8259 замаскирован, LAPIC включён, таймер 1 кГц откалиброван
   по PIT, линии IOAPIC замаскированы → `__init_array` → `neko_kernel_init()` (регистрация `[Irq]`) → `sti` →
   `neko_kernel_main(&info)`.
3. Сгенерированный `neko_kernel_main` вызывает C#-метод с `[KernelEntry]`.

### Прерывания

| Векторы | Что |
|---|---|
| 0x00–0x1F | исключения CPU → `crash::cpu_exception` (экран смерти) |
| 0x20 | LAPIC-таймер 1 кГц → `time::uptime_ms` |
| 0x30–0x3F | ISA IRQ 0–15 через IOAPIC (с учётом interrupt source overrides) → обработчики `[Irq(n)]` |
| 0xFF | spurious |

`isr.S` (сгенерирован) кладёт номер вектора и код ошибки, сохраняет регистры в формате `crash::CpuState`,
`neko_isr_dispatch` отправляет исключения на экран смерти, остальное — в `irq_dispatch` (EOI для LAPIC).

### Консоль

`core/console.cpp`: сетка ячеек (кодпоинт + цвета), перерисовываются только изменившиеся ячейки. Шрифт PSF1/PSF2
вшит через `.incbin` (`runtime/assets/fonts`, Terminus 8×16, OFL), таблица Unicode → глифы. Вход — байты UTF-8 и
ANSI-последовательности (`ESC[…m`, `ESC[2J`, `ESC[H`, `ESC[K`), поэтому один и тот же поток идёт и на экран, и в serial
(`neko::term`).

### Экран смерти

`core/crash.cpp`: STOP-код (векторы CPU 0x00–0x1F, программные 0xE0–0xE4), тип исключения (аппаратные — в терминах
.NET), сообщение, место в C#, стек вызовов по цепочке `rbp` (ядро собрано с `-fno-omit-frame-pointer
-fno-optimize-sibling-calls`), регистры, состояние системы; затем опрос клавиатуры (R — перезагрузка) при выключенных
прерываниях. Символы: ядро линкуется дважды — `scripts/gen-symbols.py` берёт из первого ELF функции (`llvm-nm`) и
таблицу строк DWARF (`llvm-dwarfdump`), пишет их в секцию `.neko_symbols`, которая стоит после `.bss`, поэтому адреса кода
во втором проходе не меняются (это проверяется `--verify`).

### Память

```
виртуальное адресное пространство (свои таблицы, vmm.cpp)
 0xffff800000000000  HHDM: вся RAM из карты памяти, 2 МиБ страницы, NX; framebuffer — write-combining (PAT 4)
 0xffffc00000000000  куча ядра: растёт по 1 МиБ+ (vmm::grow_heap → heap::add_region)
 0xffffd00000000000  свободно для VirtualMemory.Map
 0xffffe00000000000  стеки ядра: [guard 4 КиБ, не отображена][стек 64 КиБ] …
 0xffffffff80000000  образ ядра: .text RX, .rodata R, .data/.bss RW+NX, .neko_symbols R
```

- **Загрузка памяти**: `kmain` (на стеке Limine) — карта памяти → buddy-аллокатор → свои таблицы страниц
  (EFER.NXE, PAT, CR0.WP, `mov cr3`) → куча 4 МиБ → новый стек с guard-страницей → `kmain_main`.
  Таблицы Limine после этого не используются; bootloader-reclaimable память пока не освобождается (там ответы Limine).
- **Buddy** (`pmm.cpp`): каждая usable-область — зона; байт метаданных на кадр (`0x80 | order` у головы свободного
  блока) в начале области; двусвязные списки свободных блоков по порядкам 0…10 хранятся в самих блоках. Блоки выровнены
  по размеру в физических адресах; при освобождении склеиваются с «близнецом» (`pfn ^ 2^order`). Двойное освобождение
  → `HEAP_CORRUPTION`.
- **Куча** (`heap.cpp`): двусвязный список блоков в порядке адресов; заголовок 32 байта (`magic`, `size`, `prev`, `next`);
  first-fit; разбиение, если остаток ≥ 48 байт; слияние с соседями при освобождении и при `add_region`;
  `realloc` растёт на месте за счёт свободного соседа. Если места нет — `Grower` (VMM) отображает новые страницы сразу
  за концом кучи, и они сливаются со свободным хвостом. Неверный указатель или повторное освобождение → экран смерти.
  Без блокировок (одно ядро, без вытеснения) — фаза 4.
- **Переполнение стека**: запись в guard-страницу → #PF, который процессор не может положить на тот же стек → #DF на
  IST-стеке → экран смерти распознаёт адрес guard-страницы и пишет `StackOverflowException`.

### Исключения

`isr.S` выравнивает кадр стека (код ошибки 0 для исключений без него), сохраняет все GPR и вызывает
`neko_isr_dispatch(InterruptFrame*)`, который печатает вектор, имя, `rip`, `rflags`, регистры, для `#PF` — `cr2` и причину,
затем `panic`. `addr2line -e build/nekotia.elf <rip>` даёт C#-строку.

## Host-режим

Тот же рантайм (`src/core`) + `src/host/host_main.cpp` вместо `arch/` и `freestanding/`, сборка обычным clang++ с
ASan/UBSan. Куча и фреймы — из `aligned_alloc`, HHDM = 0, framebuffer 800×600 в RAM (сохраняется в
`build/host/framebuffer.ppm`), serial → stdout, `Cpu.Halt()` завершает процесс, `asm` вырезается.
Нужен для быстрых проверок транспилятора и алгоритмов; всё, что связано с железом, проверяется только в QEMU.

## Редактор

- `editor/vscode-nekotia` — расширение на чистом JS: язык `nekotia` (`source.neko` = `source.cs` + инъекция сахара),
  диагностика через `nekoc check --format json`, команды `make`, переход к сгенерированному C++ по `#line`.
- `Nekotia.slnx` + `Nekotia.Common.props` — IDE-проекты: таргет `NekoDesugar` прогоняет `.neko` через `nekoc desugar --out`
  в `obj/neko/*.neko.g.cs` (с `#line` на оригинал), чтобы C#-сервер видел типы из `.neko` и ошибки указывали на `.neko`.
  Ядро MSBuild'ом не собирается.
