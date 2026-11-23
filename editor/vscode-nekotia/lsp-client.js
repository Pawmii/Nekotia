// A small Language Server Protocol client for `nekoc lsp` — plain JavaScript, no npm packages.
//
// It starts the server, keeps it in sync with the open .neko (and, without the C# extension, .cs) files,
// forwards file changes, and registers VS Code providers that turn LSP requests and answers into
// VS Code objects: completion, hover, go to definition (into the C++ runtime for [Native]), references,
// rename, outline, workspace symbols, signature help, semantic highlighting and "C++" code lenses.
'use strict';

const vscode = require('vscode');
const cp = require('child_process');

class LspClient {
    constructor({ command, args, cwd, root, output, diagnostics, wantsDocument, filterDiagnostic }) {
        Object.assign(this, { command, args, cwd, root, output, diagnostics, wantsDocument, filterDiagnostic });
        this.nextId = 1;
        this.pending = new Map();
        this.buffer = Buffer.alloc(0);
        this.opened = new Set();
        this.restarts = 0;
        this.disposables = [];
        this.legend = null;
    }

    // ------------------------------------------------------------ process and framing

    async start() {
        this.proc = cp.spawn(this.command, this.args, { cwd: this.cwd, stdio: ['pipe', 'pipe', 'pipe'] });
        this.proc.stdout.on('data', (chunk) => this.onData(chunk));
        this.proc.stderr.on('data', (chunk) => this.output.append(chunk.toString()));
        this.proc.on('exit', (code) => this.onExit(code));
        this.proc.on('error', (e) => this.output.appendLine(`nekoc lsp: ${e.message}`));

        const init = await this.request('initialize', {
            processId: process.pid,
            rootUri: vscode.Uri.file(this.root).toString(),
            capabilities: {},
        });
        this.legend = init.capabilities.semanticTokensProvider.legend;
        this.notify('initialized', {});
        for (const doc of vscode.workspace.textDocuments) this.didOpen(doc);
        this.output.appendLine('nekoc lsp: started');
    }

    onExit(code) {
        for (const { reject } of this.pending.values()) reject(new Error('nekoc lsp exited'));
        this.pending.clear();
        this.opened.clear();
        if (this.stopping) return;
        this.output.appendLine(`nekoc lsp: exited with code ${code}`);
        if (this.restarts++ < 3) {
            setTimeout(() => this.start().catch((e) => this.output.appendLine(`nekoc lsp: ${e.message}`)), 1000);
        } else {
            vscode.window.showWarningMessage('Nekotia: the language server keeps exiting — see the Nekotia output. `Nekotia: Restart Language Server` tries again.');
        }
    }

    onData(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        while (true) {
            const headerEnd = this.buffer.indexOf('\r\n\r\n');
            if (headerEnd < 0) return;
            const header = this.buffer.slice(0, headerEnd).toString('ascii');
            const m = /Content-Length:\s*(\d+)/i.exec(header);
            if (!m) {
                this.buffer = this.buffer.slice(headerEnd + 4);
                continue;
            }
            const length = parseInt(m[1], 10);
            if (this.buffer.length < headerEnd + 4 + length) return;
            const body = this.buffer.slice(headerEnd + 4, headerEnd + 4 + length).toString('utf8');
            this.buffer = this.buffer.slice(headerEnd + 4 + length);
            let msg;
            try {
                msg = JSON.parse(body);
            } catch (e) {
                continue;
            }
            this.onMessage(msg);
        }
    }

    onMessage(msg) {
        if (msg.id !== undefined && !msg.method) {
            const p = this.pending.get(msg.id);
            if (!p) return;
            this.pending.delete(msg.id);
            if (msg.error) p.reject(new Error(msg.error.message));
            else p.resolve(msg.result);
            return;
        }
        if (msg.method === 'textDocument/publishDiagnostics') {
            this.publish(msg.params);
            return;
        }
        if (msg.id !== undefined) this.send({ id: msg.id, result: null });   // server → client requests: none needed
    }

    send(msg) {
        if (!this.proc || this.proc.exitCode !== null) return;
        msg.jsonrpc = '2.0';
        const body = Buffer.from(JSON.stringify(msg), 'utf8');
        this.proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
        this.proc.stdin.write(body);
    }

    request(method, params, token) {
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.send({ id, method, params });
            if (token) token.onCancellationRequested(() => {
                if (this.pending.delete(id)) resolve(null);
            });
        });
    }

    notify(method, params) {
        this.send({ method, params });
    }

    async stop() {
        this.stopping = true;
        try {
            await Promise.race([this.request('shutdown', null), new Promise((r) => setTimeout(r, 1000))]);
            this.notify('exit', null);
        } catch (e) { /* already gone */ }
        setTimeout(() => this.proc && this.proc.kill(), 500);
        for (const d of this.disposables) d.dispose();
    }

    // ------------------------------------------------------------ documents

    didOpen(doc) {
        if (!this.wantsDocument(doc) || this.opened.has(doc.uri.toString())) return;
        this.opened.add(doc.uri.toString());
        this.notify('textDocument/didOpen', {
            textDocument: { uri: doc.uri.toString(), languageId: doc.languageId, version: doc.version, text: doc.getText() },
        });
    }

    didChange(e) {
        const doc = e.document;
        if (!this.opened.has(doc.uri.toString())) return;
        this.notify('textDocument/didChange', {
            textDocument: { uri: doc.uri.toString(), version: doc.version },
            contentChanges: [{ text: doc.getText() }],
        });
    }

    didClose(doc) {
        if (!this.opened.delete(doc.uri.toString())) return;
        this.notify('textDocument/didClose', { textDocument: { uri: doc.uri.toString() } });
    }

    fileChanged(uri, type) {
        this.notify('workspace/didChangeWatchedFiles', { changes: [{ uri: uri.toString(), type }] });
    }

    publish({ uri, diagnostics }) {
        const target = vscode.Uri.parse(uri);
        const list = diagnostics.filter((d) => this.filterDiagnostic(target, d)).map((d) => {
            const diag = new vscode.Diagnostic(range(d.range), d.message,
                d.severity === 1 ? vscode.DiagnosticSeverity.Error : d.severity === 2 ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Information);
            diag.source = d.source;
            diag.code = d.code;
            return diag;
        });
        this.diagnostics.set(target, list);
    }

    // ------------------------------------------------------------ providers

    register(selector) {
        const at = (doc, pos) => ({ textDocument: { uri: doc.uri.toString() }, position: { line: pos.line, character: pos.character } });
        const docOnly = (doc) => ({ textDocument: { uri: doc.uri.toString() } });
        const safe = (f) => async (...a) => {
            try {
                return await f(...a);
            } catch (e) {
                if (!/nothing to rename|not declared|not a valid/.test(e.message)) this.output.appendLine(`nekoc lsp: ${e.message}`);
                throw e;
            }
        };
        const L = vscode.languages;
        this.disposables.push(
            L.registerCompletionItemProvider(selector, {
                provideCompletionItems: safe(async (doc, pos, token) => {
                    const r = await this.request('textDocument/completion', at(doc, pos), token);
                    if (!r) return undefined;
                    return r.items.map((i) => {
                        const item = new vscode.CompletionItem(i.label, (i.kind || 1) - 1);
                        item.detail = i.detail;
                        item.sortText = i.sortText;
                        if (i.documentation) item.documentation = new vscode.MarkdownString(i.documentation.value);
                        return item;
                    });
                }),
            }, '.', '>'),
            L.registerHoverProvider(selector, {
                provideHover: safe(async (doc, pos, token) => {
                    const r = await this.request('textDocument/hover', at(doc, pos), token);
                    if (!r) return undefined;
                    return new vscode.Hover(new vscode.MarkdownString(r.contents.value), r.range && range(r.range));
                }),
            }),
            L.registerDefinitionProvider(selector, {
                provideDefinition: safe(async (doc, pos, token) => locations(await this.request('textDocument/definition', at(doc, pos), token))),
            }),
            L.registerReferenceProvider(selector, {
                provideReferences: safe(async (doc, pos, ctx, token) => locations(await this.request('textDocument/references',
                    { ...at(doc, pos), context: { includeDeclaration: ctx.includeDeclaration } }, token))),
            }),
            L.registerRenameProvider(selector, {
                prepareRename: async (doc, pos) => {
                    const r = await this.request('textDocument/prepareRename', at(doc, pos)).catch((e) => { throw new Error(e.message); });
                    return r ? { range: range(r.range), placeholder: r.placeholder } : undefined;
                },
                provideRenameEdits: safe(async (doc, pos, newName) => {
                    const r = await this.request('textDocument/rename', { ...at(doc, pos), newName });
                    const edit = new vscode.WorkspaceEdit();
                    for (const [uri, edits] of Object.entries((r && r.changes) || {}))
                        for (const e of edits) edit.replace(vscode.Uri.parse(uri), range(e.range), e.newText);
                    return edit;
                }),
            }),
            L.registerDocumentSymbolProvider(selector, {
                provideDocumentSymbols: safe(async (doc, token) => {
                    const r = await this.request('textDocument/documentSymbol', docOnly(doc), token);
                    const convert = (s) => {
                        const d = new vscode.DocumentSymbol(s.name, s.detail || '', s.kind - 1, range(s.range), range(s.selectionRange));
                        d.children = (s.children || []).map(convert);
                        return d;
                    };
                    return (r || []).map(convert);
                }),
            }),
            L.registerWorkspaceSymbolProvider({
                provideWorkspaceSymbols: safe(async (query, token) => {
                    const r = await this.request('workspace/symbol', { query }, token);
                    return (r || []).map((s) => new vscode.SymbolInformation(s.name, s.kind - 1, s.containerName || '', location(s.location)));
                }),
            }),
            L.registerSignatureHelpProvider(selector, {
                provideSignatureHelp: safe(async (doc, pos, token) => {
                    const r = await this.request('textDocument/signatureHelp', at(doc, pos), token);
                    if (!r) return undefined;
                    const help = new vscode.SignatureHelp();
                    help.signatures = r.signatures.map((s) => {
                        const sig = new vscode.SignatureInformation(s.label, s.documentation && new vscode.MarkdownString(s.documentation.value));
                        sig.parameters = s.parameters.map((p) => new vscode.ParameterInformation(p.label));
                        return sig;
                    });
                    help.activeSignature = r.activeSignature;
                    help.activeParameter = r.activeParameter;
                    return help;
                }),
            }, '(', ','),
            L.registerDocumentSemanticTokensProvider(selector, {
                provideDocumentSemanticTokens: safe(async (doc, token) => {
                    const r = await this.request('textDocument/semanticTokens/full', docOnly(doc), token);
                    return r ? new vscode.SemanticTokens(new Uint32Array(r.data)) : undefined;
                }),
            }, new vscode.SemanticTokensLegend(this.legend.tokenTypes, this.legend.tokenModifiers)),
            L.registerCodeLensProvider(selector, {
                provideCodeLenses: safe(async (doc, token) => {
                    if (!vscode.workspace.getConfiguration('nekotia').get('codeLens')) return [];
                    const r = await this.request('textDocument/codeLens', docOnly(doc), token);
                    return (r || []).map((c) => new vscode.CodeLens(range(c.range), {
                        title: c.command.title,
                        command: c.command.command,
                        arguments: c.command.arguments,
                        tooltip: 'Show the C++ that nekoc generated for this method',
                    }));
                }),
            }),
        );
    }
}

function range(r) {
    return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

function location(l) {
    return new vscode.Location(vscode.Uri.parse(l.uri), range(l.range));
}

function locations(list) {
    return (list || []).map(location);
}

module.exports = { LspClient };
