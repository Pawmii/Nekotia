# Язык ядра Nekotia

Ядро пишется на подмножестве C# без GC и без .NET runtime. Файлы `.cs` — обычный C#,
файлы `.neko` — C# плюс сахар Nekotia. nekoc проверяет код Roslyn'ом и переводит его в C++20.

## Что поддерживается

| C# | C++ после nekoc | Примечание |
|---|---|---|
| `namespace A.B` | `namespace A::B` | file-scoped тоже |
| `static class K` | `struct K` со статическими членами | основная единица кода ядра |
| `struct S` | `struct S` | порядок полей сохраняется, поля обнуляются (`T x{}`) |
| конструкторы структур, `: this(...)` | конструкторы, делегирующие конструкторы | |
| `enum E : byte` | `enum class E : uint8_t` + `\| & ^ ~` | `[Flags]` работает |
| методы (static и экземплярные) | функции-члены | перегрузки — с явными кастами аргументов |
| свойства `{ get; set; }`, `=> expr` | `get_X()` / `set_X(v)` | авто-свойства получают backing field |
| `operator +` и др. | свободные `operator+` | |
| generic-методы `M<T>() where T : unmanaged` | `template<typename T>` | generic-типы — фаза 1 |
| `T*`, `&x`, `*p`, `p->f`, `p[i]`, арифметика указателей | то же | `unsafe` обязателен, как в C# |
| `fixed byte Buf[16]` | `uint8_t Buf[16]` | |
| `stackalloc T[n]` | `alloca` + обнуление | только в указатель, не в `Span` |
| `fixed (T* p = ...)` | обычный указатель | GC нет, пиннинг не нужен |
| `delegate* unmanaged<int, void>` | `neko::fnptr<void, int32_t>` | `&StaticMethod` |
| `ref` / `out` / `in` параметры | `T&` / `T&` / `const T&` | |
| `const`, `static readonly` | `static constexpr` / статическое поле | |
| `if/while/do/for/switch/break/continue/return/goto label` | то же | `switch` по целым и enum |
| `nameof`, `sizeof`, `default`, `new S { F = 1 }` | константа / `sizeof` / `T{}` / IIFE-лямбда | |
| `string` (только литералы) | `const char*` (UTF-8, NUL в конце) | сравнение — `Str.Equals` |
| `Terminal.WriteLine($"{n} ms, {x:X}, {bytes:size}")` | `(Write("…"), WriteDec(n), …)` | без аллокаций; `:X` — hex, `:size` — KiB/MiB |
| `"const"[i]`, `Const.Length` (константные строки) | `u"…"[i]`, число | семантика UTF-16 как в C# — таблицы символов |
| `throw new T("msg")` | `neko::crash::unhandled_exception(...)` | экран смерти: тип, сообщение, файл:строка, метод |
| `class MyError : Exception` | не генерируется | только как тип для `throw` |
| `[CallerFilePath]`, `[CallerLineNumber]`, `[CallerMemberName]` | относительный путь, строка, `Ns.Type.Method` | |
| `volatile` поля | `volatile T` | |
| `[Irq(n)]` | регистрация в `neko_kernel_init()` | обработчик IRQ |
| `[Syscall(n)] static long Read(int fd, byte* buf, ulong len)` | адаптер `(uint64_t×6) → int64_t` + регистрация в `neko_kernel_init()` | системный вызов для программ; до 6 аргументов (целые, bool, enum, указатели); номера 2 и 32–63 |
| `static int Main(int argc, byte** argv)` (цель `user`) | `extern "C" int neko_user_main(argc, argv)` | точка входа программы; также `Main()` и `void` |

### Типы

`bool byte sbyte short ushort int uint long ulong nint nuint float double char(char16_t) string(const char*)`, указатели,
свои `struct`/`enum`, `[Native]`-структуры SDK (`BootInfo`, `Framebuffer`).

Целочисленная семантика как в C#: nekoc вставляет касты там, где C++ продвигал бы типы иначе
(`uint + int` в C# — `long`), ядро собирается с `-fwrapv` (переполнение знаковых — wrap, как в `unchecked` C#).

## Чего нет (пока) и чем заменить

| Конструкция | Ошибка | Замена |
|---|---|---|
| `class` (не static), `new` ссылочных типов | NEKO0100 | `struct` + `Heap.New<T>()`; ссылочные классы — фаза 3 |
| массивы `T[]` | NEKO0201 | `Heap.NewArray<T>(n)`, `stackalloc`, `fixed`-буфер |
| `foreach` | NEKO0300 | `for i in a..b { }` (.neko) или `for` |
| `try/catch/finally` | NEKO0300 | `defer` для очистки; любой `throw` пока необработанный → экран смерти |
| `throw;`, `throw ex;`, throw-выражения | NEKO0300 | `throw new SomeException("...")` |
| `"a" + b` | NEKO0300 | несколько `Write*` подряд или `$"..."` в `Write/WriteLine` |
| `$"..."` вне `Write/WriteLine`, указатели в `$"..."` | NEKO0300 / CS0306 | отдельный `Write(ptr)` |
| лямбды, делегаты, LINQ, `async` | NEKO0300/0100 | статические методы + function pointers |
| любой API .NET (`System.Console`…) | NEKO0200 / CS0234 | SDK Nekotia (`Nekotia.Hal`, `Nekotia.Mem`) |
| статические конструкторы | NEKO0101 | инициализаторы полей или явный `Init()` |
| вложенные типы, generic-типы, индексаторы | NEKO0100/0101 | фаза 1 |

## Сахар `.neko`

nekoc переписывает сахар в обычный C# **с сохранением номеров строк** (ошибки и `#line` указывают на `.neko`).
Посмотреть результат: `dotnet build/tools/nekoc/nekoc.dll desugar file.neko` или команда VS Code *Show Desugared C#*.

### `asm`

```neko
asm {
    cli                 // комментарии: // или #
    mov rax, cr3
}
asm "pause";
```
Intel-синтаксис без префиксов. → `Asm.Emit("cli\nmov rax, cr3\n")` → `__asm__ volatile(".intel_syntax noprefix ..." ::: "memory")`.
AT&T: `Asm.EmitAtt("...")`. В host-режиме ассемблер вырезается. Операнды-переменные — фаза 1.

### `defer`

```neko
void* buf = Heap.Alloc(4096);
defer Heap.Free(buf);
defer {
    Serial.WriteLine("leaving");
}
```
Выполняется при выходе из текущего блока, в обратном порядке (как в Zig/Go-по-блокам). Внутри нельзя `return`
и `break`/`continue` наружу (NEKO0301). → `if (Sugar.Defer) stmt;` → C++ scope guard на лямбде.

### `lock`

```neko
static Mutex _queueLock;
lock (_queueLock) {
    ...            // разблокируется при любом выходе из блока (return, break)
}
```
→ `{ _queueLock.Lock(); defer _queueLock.Unlock(); { ... } }`. Подходит для `Mutex` и `SpinLock`.

### `unless`, `loop`

```neko
unless (fb->Address != null) return;    // if (!(...))
loop {                                  // while (true)
    Cpu.Halt();
}
```

### `for … in`

```neko
for i in 0..10 { }              // var i = 0; i < 10
for ulong y in 0..fb->Height { } // тип явно, если границы не int
for i in 1..=n { }              // включительно
```

### Атрибуты-сокращения

| Сахар | Атрибут | C++ |
|---|---|---|
| `@entry` | `[KernelEntry]` | вызывается из `neko_kernel_main` |
| `@packed` | `[Packed]` | `__attribute__((packed))` |
| `@align(16)` | `[Align(16)]` | `alignas(16)` |
| `@inline` | `[Inline]` | `[[gnu::always_inline]]` |
| `@noreturn` | `[NoReturn]` | `[[noreturn]]` |
| `@native("::neko::x")` | `[Native("::neko::x")]` | реализация в `runtime/` |
| `@irq(1)` | `[Irq(1)]` | обработчик ISA IRQ, регистрируется до `sti` |
| `@syscall(32)` | `[Syscall(32)]` | системный вызов на C#, выполняется в потоке вызвавшей программы |

## Связь с C++: `[Native]`

```csharp
public static class Port
{
    [Native("::neko::cpu::outb")]
    public static extern void Out8(ushort port, byte value);
}
```
Метод не генерируется, вызов пишется как `::neko::cpu::outb(port, value)`. Так же можно отобразить структуру
(`[Native("::neko::BootInfo")] struct BootInfo` — поля совпадают по именам с C++ или помечены `[Native("c_name")]`),
`enum` (`[Native("::neko::vmm::PageFlags")]` — члены с теми же именами) и статическое поле.
Новый примитив добавляется так: объявление в `runtime/include/neko/*.hpp`, реализация в `runtime/src/`,
C#-обёртка в `sdk/Nekotia.Sdk/`.

## Программы (`"target": "user"`)

`nekotia.json` программы (`user/NAME/`) отличается полем `"target": "user"`: точка входа — `static Main`, сгенерированный
код подключает `<neko/user.hpp>` (`runtime/user/`) вместо рантайма ядра, `[KernelEntry]`/`[Irq]`/`[Syscall]` запрещены.
Язык тот же; `throw` и `Debug.Assert` печатают сообщение и завершают программу (код 134), а не останавливают систему.

| Класс (`sdk/Nekotia.User`) | Что умеет |
|---|---|
| `Nekotia.User.Console` | `Write*`, `WriteLine` (с интерполяцией), `WriteError`, `Read`, `ReadLine`; цвета — `Ansi.*` |
| `Nekotia.User.Process` | `Exit`, `Id`, `Yield`, `Sleep`, `Start(name, args)`, `Wait(pid)`, `GetInfo`, `ArgCount`/`Arg(i)`, `Syscall(n, …)` |
| `Nekotia.User.Memory` | `Alloc`, `AllocZeroed`, `Free`, `New<T>`, `NewArray<T>`, `Delete<T>`, `UsedBytes`, `TotalBytes` |
| `Nekotia.User.Time` | `UptimeMs` |

## SDK (кратко)

| Класс | Что умеет |
|---|---|
| `Nekotia.Hal.Terminal` | экран + serial: `Write*`, `WriteLine`, `WriteCodepoint`, `SetColor(AnsiColor)`, `Clear`, `Redraw`, курсор |
| `Nekotia.Hal.Serial` | только serial: `Write`, `WriteLine`, `WriteChar`, `WriteHex`, `WriteDec`, `WriteSize` |
| `Nekotia.Hal.Time` | `UptimeMs`, `Sleep(ms)`, `TimerHz` |
| `Nekotia.Hal.Irq` | `Register(line, &Handler)`, `Count(line)` |
| `Nekotia.Hal.Power` / `SysInfo` | `Reboot`, `Shutdown`; `CpuBrand`, `CpuCount`, `CpuMhz` |
| `Nekotia.Hal.Cpu` | `Halt`, `HaltForever`, `Enable/DisableInterrupts`, `ReadTimestamp`, `ReadCr2/Cr3` |
| `Nekotia.Hal.Port` | `In8/16/32`, `Out8/16/32`, `Wait` |
| `Nekotia.Mem.Heap` | `Alloc`, `AllocZeroed`, `Realloc`, `Free`, `New<T>`, `NewArray<T>`, `Delete<T>`, статистика, `Check` |
| `Nekotia.Mem.Pages` | `Alloc`/`Free` (4 KiB), `AllocContiguous`/`FreeContiguous` (buddy, выровнены), `FreeBlocks(order)`, `VirtToPhys` |
| `Nekotia.Mem.VirtualMemory` | `Map`, `Unmap`, `Protect`, `Translate`, `MapFresh`, `PageFlags` (Write/Execute/User/Uncached/WriteCombining) |
| `Nekotia.Mem.Memory` | `Set`, `Copy`, `Move`, `Compare`, `Zero` |
| `Nekotia.Threading.Thread` | `Start(name, &Entry, arg, priority)`, `Yield`, `Sleep`, `Exit`, `CurrentId`, `GetInfo` |
| `Nekotia.Processes.Process` | `Start(name, args, detached)`, `Wait(pid)`, `Count`, `GetInfo`, `ProgramCount/Name/Size`, `ExitCurrent`, `ErrorText` |
| `Nekotia.Processes.UserMemory` | `CanRead`, `CanWrite` — проверка указателей программы в `[Syscall]`-обработчиках |
| `Nekotia.Threading.Mutex` / `Semaphore` / `SpinLock` | `Lock/Unlock/TryLock/SetName`; `Wait/Signal/TryWait` (Signal — и из IRQ) |
| `Nekotia.Maths` | `Sqrt`, `Sin`, `Cos`, `Tan`, `Atan2`, `Exp`, `Log`, `Pow`, `Floor`, `Ceiling`, `Round`, `Abs`, `Pi` |
| `Nekotia.Debug` | `Panic`, `Assert` — с файлом/строкой/методом вызывающего, экран смерти |
| `Nekotia.Str` | `Length`, `Equals` для C-строк |
| `Nekotia.Asm`, `Nekotia.Intrinsics` | `Emit`, `EmitAtt`, `Unreachable`, `Breakpoint`, `CompilerBarrier` |
| `BootInfo`, `Framebuffer` | данные от загрузчика: память, HHDM, экран, cmdline |

## Коды диагностик

| Код | Значение |
|---|---|
| NEKO0001 | нет / несколько / неверная сигнатура `[KernelEntry]` |
| NEKO0002 | нет / несколько / неверная сигнатура `Main` в программе |
| NEKO0003 | неверный `[Irq]`-обработчик (сигнатура, номер линии, дубликат) |
| NEKO0004 | неверный `[Syscall]`-обработчик (сигнатура, номер, дубликат) или атрибут ядра в программе |
| NEKO0100 | неподдерживаемый вид типа (class, interface, record, вложенный, generic) |
| NEKO0101 | неподдерживаемый член (статический конструктор, индексатор, params…) |
| NEKO0200 | вызов API .NET |
| NEKO0201 | неподдерживаемый тип (массивы, decimal, managed) |
| NEKO0300 | неподдерживаемое выражение или оператор |
| NEKO0301 | неправильный `defer` |
| NEKO0302 | `asm` не константная строка |
| CSxxxx | обычные ошибки компилятора C# |
