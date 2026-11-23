// Nekotia VS Code extension — plain JavaScript, no dependencies (no npm needed).
//
//  * .neko language: C# grammar + sugar injection (syntaxes/)
//  * the nekoc language server (`nekoc lsp`, lsp-client.js): completion, hover, go to definition (into the
//    C++ runtime for [Native]), references, rename, outline, semantic highlighting, live diagnostics
//    including nekoc's own checks, "C++" lenses
//  * without the server: nekoc diagnostics on save (nekoc check --format json)
//  * commands: build / run in QEMU / host mode / gdb, show desugared C#, jump to generated C++
//  * hover docs for the sugar keywords
'use strict';

const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const { LspClient } = require('./lsp-client');

let diagnostics;
let output;
let checkTimer;
let checkRunning = false;
let checkQueued = false;
let warnedMissingNekoc = false;
let lsp;
let lspDiagnostics;

function activate(context) {
    diagnostics = vscode.languages.createDiagnosticCollection('nekoc');
    output = vscode.window.createOutputChannel('Nekotia');
    context.subscriptions.push(diagnostics, output);

    const make = (target) => () => runMake(target);
    context.subscriptions.push(
        vscode.commands.registerCommand('nekotia.build', make('kernel')),
        vscode.commands.registerCommand('nekotia.run', make('run')),
        vscode.commands.registerCommand('nekotia.runUefi', make('run-uefi')),
        vscode.commands.registerCommand('nekotia.runHost', make('run-host')),
        vscode.commands.registerCommand('nekotia.debug', make('debug')),
        vscode.commands.registerCommand('nekotia.check', () => runCheck(true)),
        vscode.commands.registerCommand('nekotia.showDesugared', showDesugared),
        vscode.commands.registerCommand('nekotia.showGeneratedCpp', showGeneratedCpp),
        vscode.commands.registerCommand('nekotia.restartLanguageServer', () => restartLanguageServer(true)),
        vscode.languages.registerHoverProvider('nekotia', { provideHover }),
        vscode.workspace.onDidSaveTextDocument((doc) => {
            if (lsp || !config().get('checkOnSave')) return;   // the server checks continuously
            if (doc.languageId === 'nekotia' || doc.fileName.endsWith('.cs')) scheduleCheck();
        }),
    );

    const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 10);
    status.text = '$(play) Nekotia';
    status.tooltip = 'Build and boot the kernel in QEMU (make run)';
    status.command = 'nekotia.run';
    context.subscriptions.push(status);
    if (workspaceRoot()) status.show();

    lspDiagnostics = vscode.languages.createDiagnosticCollection('nekoc-lsp');
    context.subscriptions.push(
        lspDiagnostics,
        vscode.workspace.onDidOpenTextDocument((doc) => lsp && lsp.didOpen(doc)),
        vscode.workspace.onDidChangeTextDocument((e) => lsp && lsp.didChange(e)),
        vscode.workspace.onDidCloseTextDocument((doc) => lsp && lsp.didClose(doc)),
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration('nekotia.languageServer')) restartLanguageServer(false);
        }),
    );
    const root = workspaceRoot();
    if (root) {
        const sources = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(root, '**/*.{cs,neko}'));
        const forward = (type) => (uri) => {
            if (lsp && !uri.fsPath.startsWith(path.join(root, 'build') + path.sep)) lsp.fileChanged(uri, type);
        };
        sources.onDidCreate(forward(1));
        sources.onDidChange(forward(2));
        sources.onDidDelete(forward(3));
        // `make tools` rebuilt nekoc: run the new server.
        const dll = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(root, config().get('nekocPath')));
        let rebuilt;
        const onDll = () => {
            clearTimeout(rebuilt);
            rebuilt = setTimeout(() => restartLanguageServer(false), 1500);
        };
        dll.onDidChange(onDll);
        dll.onDidCreate(onDll);
        context.subscriptions.push(sources, dll);
    }

    if (!startLanguageServer() && root && config().get('checkOnSave')) scheduleCheck();
}

function deactivate() {
    return lsp ? lsp.stop() : undefined;
}

// ---------------------------------------------------------------- language server

/** .cs files go to the server too unless the C# extension already serves them (setting: auto). */
function serverTakesCsharp() {
    const mode = config().get('languageServer.csharpFiles');
    if (mode === 'always') return true;
    if (mode === 'never') return false;
    return !vscode.extensions.getExtension('ms-dotnettools.csharp');
}

function startLanguageServer() {
    const root = workspaceRoot();
    if (!root || !config().get('languageServer.enabled')) return false;
    const nekoc = nekocCommand(root);
    if (!nekoc.exists) {
        output.appendLine(`nekoc lsp: ${nekoc.dll} is not built yet — run \`make tools\`; the server starts by itself afterwards.`);
        return false;
    }
    const csharp = serverTakesCsharp();
    const inRoot = (uri) => uri.scheme === 'file' && uri.fsPath.startsWith(root + path.sep) && !uri.fsPath.startsWith(path.join(root, 'build') + path.sep);
    lsp = new LspClient({
        command: nekoc.dotnet,
        args: [nekoc.dll, 'lsp'],
        cwd: root,
        root,
        output,
        diagnostics: lspDiagnostics,
        wantsDocument: (doc) => inRoot(doc.uri) && (doc.languageId === 'nekotia' || (csharp && doc.languageId === 'csharp')),
        // With the C# extension, Roslyn's own errors in .cs files are already shown: keep only nekoc's.
        filterDiagnostic: (uri, d) => csharp || !uri.fsPath.endsWith('.cs') || d.source === 'nekoc',
    });
    const selector = [{ language: 'nekotia', scheme: 'file' }];
    if (csharp) selector.push({ language: 'csharp', scheme: 'file' });
    lsp.start().then(() => lsp && lsp.register(selector)).catch((e) => output.appendLine(`nekoc lsp: ${e.message}`));
    diagnostics.clear();   // the CLI check's results are superseded
    return true;
}

async function restartLanguageServer(explicit) {
    if (lsp) {
        const old = lsp;
        lsp = undefined;
        await old.stop();
        lspDiagnostics.clear();
    }
    const started = startLanguageServer();
    if (explicit) vscode.window.setStatusBarMessage(started ? '$(sync) nekoc language server restarted' : '$(warning) nekoc is not built — run make tools', 4000);
}

// ---------------------------------------------------------------- helpers

function config() {
    return vscode.workspace.getConfiguration('nekotia');
}

/** Folder containing nekotia.json (the repository root). */
function workspaceRoot() {
    for (const folder of vscode.workspace.workspaceFolders || []) {
        if (fs.existsSync(path.join(folder.uri.fsPath, 'nekotia.json'))) return folder.uri.fsPath;
    }
    return undefined;
}

function nekocCommand(root) {
    const dll = path.resolve(root, config().get('nekocPath'));
    return { dotnet: config().get('dotnetPath'), dll, exists: fs.existsSync(dll) };
}

function requireRoot() {
    const root = workspaceRoot();
    if (!root) vscode.window.showErrorMessage('Nekotia: open the folder that contains nekotia.json.');
    return root;
}

function runMake(target) {
    const root = requireRoot();
    if (!root) return;
    let terminal = vscode.window.terminals.find((t) => t.name === 'Nekotia');
    if (!terminal) terminal = vscode.window.createTerminal({ name: 'Nekotia', cwd: root });
    terminal.show();
    terminal.sendText(`make ${target}`);
}

// ---------------------------------------------------------------- diagnostics

function scheduleCheck() {
    clearTimeout(checkTimer);
    checkTimer = setTimeout(() => runCheck(false), 300);
}

function runCheck(explicit) {
    const root = requireRoot();
    if (!root) return;
    if (checkRunning) {
        checkQueued = true;
        return;
    }
    const nekoc = nekocCommand(root);
    if (!nekoc.exists) {
        if (explicit || !warnedMissingNekoc) {
            warnedMissingNekoc = true;
            vscode.window.showWarningMessage(`Nekotia: nekoc is not built (${nekoc.dll}). Run \`make tools\`.`, 'Run make tools').then((choice) => {
                if (choice) runMake('tools');
            });
        }
        return;
    }

    checkRunning = true;
    cp.execFile(nekoc.dotnet, [nekoc.dll, 'check', '--format', 'json'], { cwd: root, maxBuffer: 16 * 1024 * 1024 }, (err, stdout, stderr) => {
        checkRunning = false;
        if (checkQueued) {
            checkQueued = false;
            scheduleCheck();
        }

        let items;
        try {
            items = JSON.parse(stdout.trim().split('\n').pop() || '[]');
        } catch (e) {
            output.appendLine(`nekoc check produced no JSON:\n${stdout}\n${stderr}`);
            return;
        }
        publish(items, explicit);
    });
}

function publish(items, explicit) {
    diagnostics.clear();
    const byFile = new Map();
    let errors = 0;
    for (const d of items) {
        if (!d.file) {
            output.appendLine(`nekoc: ${d.severity} ${d.code}: ${d.message}`);
            continue;
        }
        // The C# extension already reports Roslyn errors for .cs files; avoid duplicates there.
        if (d.file.endsWith('.cs') && !d.code.startsWith('NEKO')) continue;
        const range = new vscode.Range(
            Math.max(0, d.line - 1), Math.max(0, d.column - 1),
            Math.max(0, d.endLine - 1), Math.max(0, d.endColumn - 1));
        const severity = d.severity === 'error' ? vscode.DiagnosticSeverity.Error
            : d.severity === 'warning' ? vscode.DiagnosticSeverity.Warning
            : vscode.DiagnosticSeverity.Information;
        if (severity === vscode.DiagnosticSeverity.Error) errors++;
        const diag = new vscode.Diagnostic(range, d.message, severity);
        diag.source = 'nekoc';
        diag.code = d.code;
        if (!byFile.has(d.file)) byFile.set(d.file, []);
        byFile.get(d.file).push(diag);
    }
    for (const [file, list] of byFile) diagnostics.set(vscode.Uri.file(file), list);
    if (explicit) {
        vscode.window.setStatusBarMessage(errors ? `$(error) nekoc: ${errors} error(s)` : '$(check) nekoc: no errors', 4000);
    }
}

// ---------------------------------------------------------------- views

function showDesugared() {
    const editor = vscode.window.activeTextEditor;
    const root = requireRoot();
    if (!editor || !root) return;
    const nekoc = nekocCommand(root);
    if (!nekoc.exists) {
        vscode.window.showWarningMessage('Nekotia: nekoc is not built. Run `make tools`.');
        return;
    }
    const doc = editor.document;
    const run = () => cp.execFile(nekoc.dotnet, [nekoc.dll, 'desugar', doc.fileName], { cwd: root }, async (err, stdout, stderr) => {
        if (err) {
            vscode.window.showErrorMessage(`nekoc desugar failed: ${stderr || err.message}`);
            return;
        }
        const view = await vscode.workspace.openTextDocument({ language: 'csharp', content: stdout });
        await vscode.window.showTextDocument(view, { viewColumn: vscode.ViewColumn.Beside, preview: true });
    });
    if (doc.isDirty) doc.save().then(run);
    else run();
}

/**
 * Opens build/gen/nekotia.gen.cpp at the C++ generated for the current C# line.
 * nekoc writes `#line N "file"` before every statement, so we look for the closest one.
 */
async function showGeneratedCpp(uriArg, lineArg) {
    const editor = vscode.window.activeTextEditor;
    const root = requireRoot();
    if (!root || (!editor && !uriArg)) return;
    const source = uriArg ? vscode.Uri.parse(uriArg).fsPath : editor.document.fileName;
    const wanted = lineArg || editor.selection.active.line + 1;

    // user/<name>/… is a program with its own generated C++; everything else is the kernel.
    const rel = path.relative(root, source).split(path.sep);
    const genPath = rel[0] === 'user' && rel.length > 2
        ? path.join(root, 'build', 'user', rel[1], 'gen', 'nekotia.gen.cpp')
        : path.join(root, 'build', 'gen', 'nekotia.gen.cpp');
    if (!fs.existsSync(genPath)) {
        vscode.window.showWarningMessage('Nekotia: no generated C++ yet — run `make gen` (or Nekotia: Build Kernel).');
        return;
    }

    const lines = fs.readFileSync(genPath, 'utf8').split('\n');
    const re = /^#line (\d+) "(.*)"$/;
    let best = -1;
    let bestLine = -1;
    for (let i = 0; i < lines.length; i++) {
        const m = re.exec(lines[i]);
        if (!m || m[2] !== source) continue;
        const n = parseInt(m[1], 10);
        if (n <= wanted && n > bestLine) {
            bestLine = n;
            best = i;
        }
    }

    const doc = await vscode.workspace.openTextDocument(genPath);
    const target = best >= 0 ? best + 1 : 0;
    const shown = await vscode.window.showTextDocument(doc, { viewColumn: vscode.ViewColumn.Beside, preview: true });
    const pos = new vscode.Position(Math.min(target, doc.lineCount - 1), 0);
    shown.selection = new vscode.Selection(pos, pos);
    shown.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
    if (best < 0) vscode.window.setStatusBarMessage('Nekotia: this line has no generated code (declaration or not built yet)', 4000);
}

// ---------------------------------------------------------------- hover

const SUGAR_DOCS = {
    asm: '**asm** — inline assembly, Intel syntax.\n\n```neko\nasm { cli; hlt }\nasm "pause";\n```\nLowered to `Asm.Emit(...)` → `__asm__ volatile(...)`. Compiled out in host mode. Comments: `//` or `#`.',
    defer: '**defer** — runs the statement when the enclosing scope exits, in reverse order (like Zig/Go).\n\n```neko\nvoid* p = Heap.Alloc(64);\ndefer Heap.Free(p);\n```\nThe body cannot `return` or `break` out of the scope.',
    unless: '**unless (cond)** — the same as `if (!(cond))`.',
    loop: '**loop { }** — infinite loop, the same as `while (true) { }`.',
    in: '**for [T] i in a..b { }** — half-open range loop `a <= i < b`; `a..=b` includes `b`.\nGive the type when the bounds are not `int`: `for ulong y in 0..fb->Height { }`.',
    entry: '**@entry** → `[KernelEntry]` — the kernel entry point: `static void Start(BootInfo* info)`.',
    packed: '**@packed** → `[Packed]` — struct without padding (`__attribute__((packed))`).',
    inline: '**@inline** → `[Inline]` — always inline (`[[gnu::always_inline]]`).',
    noreturn: '**@noreturn** → `[NoReturn]` — the method never returns (`[[noreturn]]`).',
    native: '**@native("cpp::name")** → `[Native(...)]` — implemented in the C++ runtime; calls are emitted as `cpp::name(...)`.',
    align: '**@align(n)** → `[Align(n)]` — struct alignment (`alignas(n)`).',
    irq: '**@irq(n)** → `[Irq(n)]` — interrupt handler for ISA IRQ `n` (1 = keyboard). Must be `static void Name()`; registered by nekoc before interrupts are enabled; runs with interrupts off, must not use the heap.',
    syscall: '**@syscall(n)** → `[Syscall(n)]` — system call `n` for user programs (2 = read, 32–63 free). `static`, up to 6 integer/bool/enum/pointer parameters, returns an integer (negative = error) or void. Runs on the calling program\'s thread; check its pointers with `UserMemory.CanRead/CanWrite`.',
};

function provideHover(document, position) {
    const range = document.getWordRangeAtPosition(position, /@?[A-Za-z_]+/);
    if (!range) return undefined;
    const word = document.getText(range).replace(/^@/, '');
    const isAttribute = document.getText(range).startsWith('@');
    const attributes = ['entry', 'packed', 'inline', 'noreturn', 'native', 'align', 'irq', 'syscall'];
    if (attributes.includes(word) && !isAttribute) return undefined;
    if (!SUGAR_DOCS[word]) return undefined;
    if (word === 'in' && !/\bfor\s+.*\bin\b/.test(document.lineAt(position.line).text)) return undefined;
    return new vscode.Hover(new vscode.MarkdownString(SUGAR_DOCS[word]), range);
}

module.exports = { activate, deactivate };
