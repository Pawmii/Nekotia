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
| `x switch { < 0 => -1, 1 or 2 => 1, _ when f(x) => 2, _ => 0 }` | IIFE-лямбда с цепочкой `if … return` | нет совпадения → `SwitchExpressionException` на экране смерти |
| `c is >= 'a' and <= 'z'`, `p is null`, `x is not (1 or 2)` | условие (сложный операнд вычисляется один раз) | паттерны: константы, `null`, `_`, `< <= > >=`, `and`/`or`/`not`, скобки, `when` |
| `case >= 10 when ok:` в `switch`-операторе | `switch (0) { default: if … else if … }` | `break` выходит из switch, `continue` — к циклу, как в C# |
| `nameof`, `sizeof`, `default`, `new S { F = 1 }` | константа / `sizeof` / `T{}` / IIFE-лямбда | |
| `string` (только литералы) | `const char*` (UTF-8, NUL в конце) | сравнение — `Str.Equals` |
| `Terminal.WriteLine($"{n} ms, {x:X}, {bytes:size}")` | `(Write("…"), WriteDec(n), …)` | без аллокаций; `:X` — 0x…, `:X4` — 4 hex-цифры, `:D2` — с нулями, `:size` — KiB/MiB, `:F2` |
| `t.WriteLine($"{name,-12}{size,8:size}")` (`Nekotia.Text`) | то же на построителе в вашем буфере | интерполяция в буфер; `{v,8}` / `{v,-8}` — выравнивание; `byte*` в дырах |
| `Log.Info($"net: {n} packets")` — любой статический метод с перегрузкой `string` | текст собирается в 512-байтовый буфер на стеке и передаётся в эту перегрузку | без аллокаций; длиннее — обрезается |
| `"const"[i]`, `Const.Length` (константные строки) | `u"…"[i]`, число | семантика UTF-16 как в C# — таблицы символов |
| `throw new T("msg")`, `throw;` | `neko::exc::raise(…)` / `rethrow` | без подходящего `try` — экран смерти (в программе — код 134) |
| `try { } catch (T e) when (f) { } finally { }` | кадр `setjmp` + охранные объекты | `e.Message`; перехват по базовому типу; `defer`, `lock` и `finally` выполняются при раскрутке |
| `class MyError : Exception` | не генерируется | тип для `throw`/`catch`, иерархия учитывается |
| `[CallerFilePath]`, `[CallerLineNumber]`, `[CallerMemberName]` | относительный путь, строка, `Ns.Type.Method` | |
| `volatile` поля | `volatile T` | |
| `[Irq(n)]` | регистрация в `neko_kernel_init()` | обработчик IRQ |
| `[Syscall(n)] static long Read(int fd, byte* buf, ulong len)` | адаптер `(uint64_t×6) → int64_t` + регистрация в `neko_kernel_init()` | системный вызов для программ; до 6 аргументов (целые, bool, enum, указатели); номера 40–63 |
| `static int Main(int argc, byte** argv)` (цель `user`) | `extern "C" int neko_user_main(argc, argv)` | точка входа программы; также `Main()` и `void` |

### Типы

`bool byte sbyte short ushort int uint long ulong nint nuint float double char(char16_t) string(const char*)`, указатели,
свои `struct`/`enum`, `[Native]`-структуры SDK (`BootInfo`, `Framebuffer`).

Целочисленная семантика как в C#: nekoc вставляет касты там, где C++ продвигал бы типы иначе
(`uint + int` в C# — `long`), ядро собирается с `-fwrapv` (переполнение знаковых — wrap, как в `unchecked` C#) и `-fno-finite-loops`
(в C# пустой `while (true) { }` законен, а старые clang по правилам C++ до C++26 вправе выбросить его).

## Чего нет (пока) и чем заменить

| Конструкция | Ошибка | Замена |
|---|---|---|
| `class` (не static), `new` ссылочных типов | NEKO0100 | `struct` + `Heap.New<T>()`; ссылочные классы — фаза 3 |
| массивы `T[]` | NEKO0201 | `Heap.NewArray<T>(n)`, `stackalloc`, `fixed`-буфер |
| `foreach` | NEKO0300 | `for i in a..b { }` (.neko) или `for` |
| throw-выражения (`x ?? throw …`), `throw existingObject` | NEKO0300 | `throw new SomeException("...")` отдельным оператором |
| структура или параметр, изменённые внутри `try` и объявленные снаружи | NEKO0303 | объявить внутри `try` или скопировать в локальную скалярную переменную |
| `"a" + b` | NEKO0300 | несколько `Write*` подряд, `$"..."` в `Write/WriteLine` или `Text` |
| `$"..."` вне `Write/WriteLine` (например, в `string s = $"…"`) | NEKO0300 | `Text` над своим буфером: `t.Write($"…")` |
| паттерны с переменными (`x is int y`, `var v`), списки, свойства | NEKO0300 | сравнить значение, которое уже есть |
| лямбды, делегаты, LINQ, `async` | NEKO0300/0100 | статические методы + function pointers |
| любой API .NET (`System.Console`…) | NEKO0200 / CS0234 | SDK Nekotia (`Nekotia.Hal`, `Nekotia.Mem`) |
| статические конструкторы | NEKO0101 | инициализаторы полей или явный `Init()` |
| вложенные типы, generic-типы, индексаторы | NEKO0100/0101 | фаза 1 |

### Исключения

`throw` ищет ближайший `try` текущего потока: выполняет по пути `defer`, `finally` и освобождение `lock`
(каждая такая конструкция регистрирует себя в списке очистки потока) и прыгает в его `catch` (`setjmp`/`longjmp`,
без таблиц размотки и без аллокаций). Нет подходящего `catch` — исключение идёт дальше; нет `try` вовсе — экран
смерти, как раньше.

- Локальные переменные, которые `try` меняет, а объявлены снаружи, nekoc сам объявляет `volatile` (иначе после прыжка
  их значение не определено); для структур и параметров в такой роли — ошибка NEKO0303.
- Функция, чей адрес берётся (`&Method` — её может вызвать C++: обработчики, генераторы `/proc`, сокеты, потоки),
  начинается с кадра-барьера: исключение не перепрыгнет через C++-кадры с их замками — у барьера оно необработанное.
- `throw` внутри обработчика прерывания всегда фатален.

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
| `Nekotia.User.Process` | `Exit`, `Id`, `Yield`, `Sleep`, `Start(name or path, args)`, `Wait(pid)`, `Kill(pid)`, `GetInfo`, `ArgCount`/`Arg(i)`, `ErrorText`, `Syscall(n, …)` |
| `Nekotia.User.File` | `Open`, `Close`, `ReadBytes`, `WriteBytes`, `Seek`, `Stat`, `ReadDirectory`, `CreateDirectory`, `Delete`, `Rename`, `ChangeDirectory`, `CurrentDirectory`, `Duplicate`, `Redirect` (dup2), `SetLength` |
| `Nekotia.User.Net` | `Connect(ip, port)`, `Listen`, `Accept`, `Shutdown`, `UdpOpen`, `SendTo`, `ReceiveFrom`, `Resolve(name)`, `Config`, `WriteIp`, `WriteAll` — сокеты это fd |
| `Nekotia.User.Path` / `Tool` | `BaseName`, `Join`; `Tool.Fail` — сообщение об ошибке утилиты, `Tool.CopyFile` |
| `Nekotia.User.Thread` | `Start(&Entry, arg)` → handle, `Join`, `CurrentId`, `Exit`, `Yield` |
| `Nekotia.User.SpinLock` / `Atomic` / `Futex` | мьютекс `Lock`/`Unlock` (спин, затем сон на futex); `Add`, `Load`, `Store`, `CompareExchange` над `long*`; `Futex.Wait/Wake` |
| `Nekotia.User.Memory` | `Alloc`, `AllocZeroed`, `Free`, `New<T>`, `NewArray<T>`, `Delete<T>`, `UsedBytes`, `TotalBytes`; страницы: `MapPages`, `UnmapPages`, `ProtectPages` |
| `Nekotia.User.Time` | `UptimeMs` |

## SDK (кратко)

| Класс | Что умеет |
|---|---|
| `Nekotia.Hal.Terminal` | экран + serial: `Write*`, `WriteLine`, `WriteCodepoint`, `SetColor(AnsiColor)`, `Clear`, `Redraw`, курсор |
| `Nekotia.Hal.Serial` | только serial: `Write`, `WriteLine`, `WriteChar`, `WriteHex`, `WriteDec`, `WriteSize` |
| `Nekotia.Hal.Time` | `UptimeMs`, `Sleep(ms)`, `TimerHz` |
| `Nekotia.Hal.Irq` | `Register(line, &Handler)`, `Count(line)`, `GetMsi(i, …)` |
| `Nekotia.Hal.Log` | `Debug/Info/Warn/Error(text или $"…")`, `Level`/`SetLevel(LogLevel)`, `Read`/`Size` (журнал), `StopBootCapture` |
| `Nekotia.IO.Block` | `Count`, `GetInfo`, `Find(name)`, `Read/Write(index, lba, count, buffer)`, `ReadBytes/WriteBytes`, `Flush`, `AddRamDisk` |
| `Nekotia.Hal.Power` / `SysInfo` | `Reboot`, `Shutdown`; `CpuBrand`, `CpuCount`, `CpuMhz` |
| `Nekotia.Hal.Cpu` | `Halt`, `HaltForever`, `Enable/DisableInterrupts`, `ReadTimestamp`, `ReadCr2/Cr3` |
| `Nekotia.Hal.Port` | `In8/16/32`, `Out8/16/32`, `Wait` |
| `Nekotia.Mem.Heap` | `Alloc`, `AllocZeroed`, `Realloc`, `Free`, `New<T>`, `NewArray<T>`, `Delete<T>`, статистика, `Check` |
| `Nekotia.Mem.Pages` | `Alloc`/`Free` (4 KiB), `AllocContiguous`/`FreeContiguous` (buddy, выровнены), `FreeBlocks(order)`, `VirtToPhys` |
| `Nekotia.Mem.VirtualMemory` | `Map`, `Unmap`, `Protect`, `Translate`, `MapFresh`, `PageFlags` (Write/Execute/User/Uncached/WriteCombining) |
| `Nekotia.Mem.Memory` | `Set`, `Copy`, `Move`, `Compare`, `Zero` |
| `Nekotia.Threading.Thread` | `Start(name, &Entry, arg, priority)`, `Yield`, `Sleep`, `Exit`, `CurrentId`, `GetInfo` |
| `Nekotia.Processes.Process` | `Start(name, args, detached)`, `Wait(pid)`, `Kill(pid)`, `InterruptForeground`, `Foreground`, `Count`, `GetInfo`, `ExitCurrent`, `ErrorText` |
| `Nekotia.IO.Vfs` | `Open(path, flags, out OpenFile*)`, `ReadFile`, `WriteFile`, `Seek`, `SetLength`, `Stat`, `ReadDirectory`, `Close`, `CreateDirectory`, `Remove`, `Rename`, `ChangeDirectory`, `CurrentDirectory`, `Exists`, `WriteAll`, `UnpackTar`, `Statistics`, `Unmount`, `Sync`, `GetMount` |
| `Nekotia.Net.NetDevice` | `Count`, `GetInfo`, `Send(frame)`, `Receive(buffer, timeout)`, `Inject` (loopback) — кадры Ethernet для стека в `kernel/Net` |
| `Nekotia.IO.Fat` | `Format(device, label)`, `Mount(device, dir)`, `Probe(device)` |
| `Nekotia.Hal.Pci` | `Count`, `GetInfo(i, out PciDevice)`, `Find(vendor, device)`, `FindClass`, `Read8/16/32`, `Write16/32`, `Enable`, `FindCapability`, `MapBar`, `ClassName`, `VendorName` |
| `Nekotia.Processes.UserMemory` | `CopyTo`/`CopyFrom` — единственный способ трогать память программы (SMAP); `CanRead`/`CanWrite` — предварительная проверка |
| `Nekotia.Threading.Mutex` / `Semaphore` / `SpinLock` | `Lock/Unlock/TryLock/SetName`; `Wait/Signal/TryWait` (Signal — и из IRQ) |
| `Nekotia.Maths` | `Sqrt`, `Sin`, `Cos`, `Tan`, `Atan2`, `Exp`, `Log`, `Pow`, `Floor`, `Ceiling`, `Round`, `Abs`, `Pi` |
| `Nekotia.Debug` | `Panic`, `Assert` — с файлом/строкой/методом вызывающего, экран смерти |
| `Nekotia.Str` | `Length`, `Equals`, `StartsWith` для C-строк; `Bytes("литерал")` → `byte*` (только чтение) |
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
