// Smoke test: load the VS Code extension and run activate() against a permissive stand-in `vscode`
// (every property is a callable no-op) — catches typos and missing modules in the registration code.
'use strict';
const path = require('path');
const Module = require('module');

const registered = [];
function stub(name) {
    const fn = function () {};
    return new Proxy(fn, {
        get(_, prop) {
            if (prop === Symbol.iterator) return function* () {};
            if (prop === Symbol.toPrimitive) return () => '';
            if (prop === 'then') return undefined;   // not a promise
            return stub(`${name}.${String(prop)}`);
        },
        apply(_, __, args) {
            if (/\.register\w*$/.test(name)) registered.push(name.split('.').pop() + (typeof args[0] === 'string' ? ` ${args[0]}` : ''));
            return stub(`${name}()`);
        },
        construct() { return stub(`new ${name}`); },
    });
}
const vscode = stub('vscode');
const load = Module._load;
Module._load = function (request, ...rest) { return request === 'vscode' ? vscode : load.call(this, request, ...rest); };

const ext = require(path.resolve(__dirname, '../../editor/vscode-nekotia/extension.js'));
const context = { subscriptions: [] };
ext.activate(context);
const want = ['registerCommand nekotia.debugKernel', 'registerCommand nekotia.restartLanguageServer', 'registerDebugAdapterDescriptorFactory nekotia',
    'registerDebugConfigurationProvider nekotia', 'registerHoverProvider nekotia'];
const missing = want.filter((w) => !registered.includes(w));
if (missing.length) {
    console.log(`  FAIL  extension activates — not registered: ${missing.join(', ')}`);
    process.exit(1);
}
console.log(`  ok    extension activates (${registered.length} registrations)`);
