// Runs editor/vscode-nekotia/lsp-client.js against the real `nekoc lsp` with a stand-in `vscode` module:
// checks that LSP answers become the right VS Code objects. Run with node (or VS Code's: ELECTRON_RUN_AS_NODE=1 code).
'use strict';
const path = require('path');
const fs = require('fs');
const Module = require('module');

const root = path.resolve(__dirname, '..', '..');
const providers = {};
const diagnostics = new Map();

class Range { constructor(a, b, c, d) { this.start = { line: a, character: b }; this.end = { line: c, character: d }; } }
class Uri {
    constructor(fsPath) { this.fsPath = fsPath; this.scheme = 'file'; }
    toString() { return 'file://' + this.fsPath; }
    static file(p) { return new Uri(p); }
    static parse(s) { return new Uri(decodeURIComponent(s.replace(/^file:\/\//, ''))); }
}
const make = (name) => class { constructor(...a) { this._args = a; this.kind = name; Object.assign(this, { a }); } };
const vscode = {
    Range, Uri,
    Location: class { constructor(uri, range) { this.uri = uri; this.range = range; } },
    CompletionItem: class { constructor(label, kind) { this.label = label; this.kind = kind; } },
    MarkdownString: class { constructor(v) { this.value = v; } },
    Hover: class { constructor(c, r) { this.contents = c; this.range = r; } },
    Diagnostic: class { constructor(r, m, s) { this.range = r; this.message = m; this.severity = s; } },
    DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2 },
    WorkspaceEdit: class { constructor() { this.edits = []; } replace(u, r, t) { this.edits.push({ u, r, t }); } },
    DocumentSymbol: class { constructor(name, detail, kind, range, sel) { Object.assign(this, { name, detail, kind, range, sel }); } },
    SymbolInformation: class { constructor(name, kind, container, loc) { Object.assign(this, { name, kind, container, loc }); } },
    SignatureHelp: class {},
    SignatureInformation: class { constructor(label, doc) { this.label = label; this.doc = doc; } },
    ParameterInformation: class { constructor(label) { this.label = label; } },
    SemanticTokens: class { constructor(data) { this.data = data; } },
    SemanticTokensLegend: class { constructor(t, m) { this.tokenTypes = t; this.tokenModifiers = m; } },
    CodeLens: class { constructor(r, c) { this.range = r; this.command = c; } },
    workspace: { textDocuments: [], getConfiguration: () => ({ get: () => true }) },
    window: { showWarningMessage() {} },
    languages: {},
};
for (const kind of ['CompletionItem', 'Hover', 'Definition', 'Reference', 'Rename', 'DocumentSymbol', 'SignatureHelp', 'DocumentSemanticTokens', 'CodeLens'])
    vscode.languages[`register${kind}Provider`] = (sel, p) => { providers[kind] = p; return { dispose() {} }; };
vscode.languages.registerWorkspaceSymbolProvider = (p) => { providers.WorkspaceSymbol = p; return { dispose() {} }; };

const load = Module._load;
Module._load = function (request, ...rest) { return request === 'vscode' ? vscode : load.call(this, request, ...rest); };
const { LspClient } = require(path.join(root, 'editor/vscode-nekotia/lsp-client.js'));

function doc(rel, languageId) {
    const text = fs.readFileSync(path.join(root, rel), 'utf8');
    return { uri: Uri.file(path.join(root, rel)), languageId, version: 1, getText: () => text, text };
}
function pos(text, needle, offset) {
    const i = text.indexOf(needle) + offset;
    const before = text.slice(0, i);
    return { line: before.split('\n').length - 1, character: i - before.lastIndexOf('\n') - 1 };
}

let passed = 0, failed = 0;
function check(cond, name, detail) {
    if (cond) { passed++; console.log(`  ok    ${name}`); } else { failed++; console.log(`  FAIL  ${name}${detail ? '  — ' + detail : ''}`); }
}

(async () => {
    const output = { append() {}, appendLine() {} };
    const client = new LspClient({
        command: 'dotnet', args: [path.join(root, 'build/tools/nekoc/nekoc.dll'), 'lsp'], cwd: root, root, output,
        diagnostics: { set: (u, l) => diagnostics.set(u.fsPath, l) },
        wantsDocument: () => true, filterDiagnostic: () => true,
    });
    await client.start();
    client.register([{ language: 'nekotia' }]);
    const neko = doc('kernel/Shell/Commands.neko', 'nekotia');
    client.didOpen(neko);

    const h = await providers.Hover.provideHover(neko, pos(neko.text, 'Log.Read(', 5));
    check(h && /::neko::log::read/.test(h.contents.value) && h.range instanceof Range, 'hover → vscode.Hover with markdown and range');

    const defs = await providers.Definition.provideDefinition(neko, pos(neko.text, 'Log.Read(', 5));
    check(defs.length >= 2 && defs.every((d) => d.uri instanceof Uri && d.range instanceof Range)
        && defs.some((d) => d.uri.fsPath.endsWith('log.cpp')), 'definition → Locations, C# and C++');

    const items = await providers.CompletionItem.provideCompletionItems(neko, pos(neko.text, 'Log.Read(', 4), { onCancellationRequested() {} });
    check(items.some((i) => i.label === 'Read' && i.kind === 2), 'completion → CompletionItems with VS Code kinds (method = 2)');

    const syms = await providers.DocumentSymbol.provideDocumentSymbols(neko, { onCancellationRequested() {} });
    check(syms.length > 0 && syms[0].kind === 4 && syms[0].children.some((c) => c.name === 'Dmesg' && c.kind === 5), 'outline → DocumentSymbols (class = 4, method = 5)');

    const edit = await providers.Rename.provideRenameEdits(neko, pos(neko.text, 'static void Dmesg', 12), 'KernelLog');
    check(edit.edits.length === 2 && edit.edits.every((e) => e.t === 'KernelLog'), 'rename → WorkspaceEdit');

    let prepError = null;
    try { await providers.Rename.prepareRename(neko, pos(neko.text, '// Shell commands', 3)); } catch (e) { prepError = e; }
    check(prepError && /nothing to rename/.test(prepError.message), 'prepareRename on a comment → an error VS Code shows');

    const tokens = await providers.DocumentSemanticTokens.provideDocumentSemanticTokens(neko, { onCancellationRequested() {} });
    check(tokens.data instanceof Uint32Array && tokens.data.length % 5 === 0 && tokens.data.length > 100, 'semantic tokens → Uint32Array');

    const lenses = await providers.CodeLens.provideCodeLenses(neko, { onCancellationRequested() {} });
    check(lenses.length > 10 && lenses[0].command.command === 'nekotia.showGeneratedCpp' && lenses[0].command.arguments.length === 2, 'code lenses → commands with arguments');

    const help = await providers.SignatureHelp.provideSignatureHelp(neko, pos(neko.text, 'Log.Read(at, ', 13), { onCancellationRequested() {} });
    check(help && help.activeParameter === 1 && help.signatures[0].parameters.length === 3, 'signature help → SignatureHelp');

    // a broken edit → diagnostics in the collection
    const broken = neko.text.replace('static void Dmesg()', 'static void Dmesg(');
    client.didChange({ document: { uri: neko.uri, version: 2, getText: () => broken } });
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline && !(diagnostics.get(neko.uri.fsPath) || []).some((d) => d.severity === 0)) await new Promise((r) => setTimeout(r, 100));
    check((diagnostics.get(neko.uri.fsPath) || []).some((d) => d.severity === 0 && d.range instanceof Range), 'diagnostics → the collection, as vscode.Diagnostic');

    await client.stop();
    console.log(`lsp client tests: ${passed} passed, ${failed} failed`);
    process.exit(failed ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
