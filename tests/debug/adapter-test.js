// A real debugging session with the Nekotia debug adapter (editor/vscode-nekotia/debug-adapter.js):
// make debug → QEMU (headless) → gdb/MI, driven through DAP messages the way VS Code sends them.
// Run with node (or scripts/run-js.sh). Takes a minute the first time: it builds the -O0 kernel.
'use strict';
const path = require('path');
const fs = require('fs');
const { NekotiaDebugSession, parseMiLine, csType, csFunction } = require('../../editor/vscode-nekotia/debug-adapter.js');

const root = path.resolve(__dirname, '..', '..');
let passed = 0, failed = 0;
function check(cond, name, detail) {
    if (cond) { passed++; console.log(`  ok    ${name}`); } else { failed++; console.log(`  FAIL  ${name}${detail ? '  — ' + String(detail).slice(0, 300) : ''}`); }
}
function lineOf(rel, needle) {
    const lines = fs.readFileSync(path.join(root, rel), 'utf8').split('\n');
    const i = lines.findIndex((l) => l.includes(needle));
    if (i < 0) throw new Error(`${needle} not in ${rel}`);
    return i + 1;
}

// ---- pure helpers
const r = parseMiLine('12^done,bkpt={number="3",addr="0xffffffff80001234",line="42",fullname="/a/b c.neko"},x=["1","2"]');
check(r.token === 12 && r.cls === 'done' && r.results.bkpt.number === '3' && r.results.bkpt.fullname === '/a/b c.neko' && r.results.x.length === 2, 'MI parser: tuples, lists, strings');
check(parseMiLine('~"\\320\\277\\321\\200\\320\\270\\n"').text === 'при\n', 'MI parser: octal-escaped UTF-8');
check(csType('unsigned char *') === 'byte*' && csType('Nekotia::OS::Net::Tcb *') === 'Tcb*' && csType('uint64_t') === 'ulong', 'C# type names');
check(csFunction('Nekotia::OS::Commands::Dmesg') === 'Commands.Dmesg' && csFunction('neko::sched::sleep_ms') === 'neko::sched::sleep_ms', 'C# frame names');
check(csFunction('Nekotia::OS::Kernel::MountDisk()::$_1::operator()() const') === 'Kernel.MountDisk (lambda)', 'C# frame names: nekoc lambdas');

(async () => {
    const s = new NekotiaDebugSession({ log: process.env.NEKO_DAP_LOG ? (l) => console.error(l) : undefined });
    const events = [];
    const waiters = [];
    const responses = new Map();
    let seq = 1;
    s.onDidSendMessage((m) => {
        if (m.type === 'event') {
            const w = waiters.find((x) => x.test(m));
            if (w) { waiters.splice(waiters.indexOf(w), 1); w.resolve(m); } else events.push(m);
        } else if (m.type === 'response') {
            const p = responses.get(m.request_seq);
            if (p) { responses.delete(m.request_seq); p(m); }
        }
    });
    const request = (command, args) => new Promise((resolve) => {
        const n = seq++;
        responses.set(n, resolve);
        s.handleMessage({ type: 'request', seq: n, command, arguments: args });
    });
    const waitEvent = (test, ms = 60000) => new Promise((resolve, reject) => {
        const old = events.find(test);
        if (old) { events.splice(events.indexOf(old), 1); resolve(old); return; }
        const w = { test, resolve };
        waiters.push(w);
        setTimeout(() => { const i = waiters.indexOf(w); if (i >= 0) { waiters.splice(i, 1); reject(new Error('timeout waiting for event')); } }, ms);
    });
    const stopped = (ms) => waitEvent((e) => e.event === 'stopped', ms);

    const init = await request('initialize', { adapterID: 'nekotia' });
    check(init.success && init.body.exceptionBreakpointFilters.some((f) => f.filter === 'panic'), 'initialize: capabilities and the panic filter');

    const port = 20000 + Math.floor(Math.random() * 20000);
    const launch = await request('launch', {
        cwd: root, display: false, gdbPort: port,
        makeArgs: ['NET_FLAGS=-netdev user,id=net0 -device virtio-net-pci,netdev=net0'],
    });
    check(launch.success, 'launch: make debug, QEMU paused, gdb connected', launch.message);
    if (!launch.success) return finish(s);
    await waitEvent((e) => e.event === 'initialized');

    const kernel = path.join(root, 'kernel/Kernel.cs');
    const kline = lineOf('kernel/Kernel.cs', 'Log.Info($"disk: /dev/vda (FAT32) mounted');
    const shell = path.join(root, 'kernel/Shell/Shell.neko');
    const sline = lineOf('kernel/Shell/Shell.neko', 'Log.StopBootCapture();');
    const b1 = await request('setBreakpoints', { source: { path: kernel }, breakpoints: [{ line: kline }] });
    check(b1.success && b1.body.breakpoints[0].verified, 'breakpoint in a .cs file is verified', JSON.stringify(b1.body || b1.message));
    const b2 = await request('setBreakpoints', { source: { path: shell }, breakpoints: [{ line: sline }] });
    check(b2.success && b2.body.breakpoints[0].verified, 'breakpoint in a .neko file is verified', JSON.stringify(b2.body || b2.message));
    const fb = await request('setFunctionBreakpoints', { breakpoints: [{ name: 'Commands.Dmesg' }] });
    check(fb.success && fb.body.breakpoints[0].verified, 'function breakpoint by C# name (Commands.Dmesg)', JSON.stringify(fb.body || fb.message));
    const ex = await request('setExceptionBreakpoints', { filters: ['panic'] });
    check(ex.success, 'exception filter: kernel panic');
    await request('configurationDone', {});

    let st = await stopped(120000);
    check(st.body.reason === 'breakpoint', 'stops at the .cs breakpoint', JSON.stringify(st.body));
    const threads = await request('threads', {});
    check(threads.body.threads.length === 4 && threads.body.threads[0].name === 'CPU 0', 'threads are the 4 CPUs', JSON.stringify(threads.body));
    let stack = await request('stackTrace', { threadId: st.body.threadId });
    let top = stack.body.stackFrames[0];
    check(top.name === 'Kernel.MountDisk' && top.source.path === kernel && top.line === kline, 'top frame: Kernel.MountDisk at the C# line', JSON.stringify(top));
    check(stack.body.stackFrames.some((f) => f.name === 'Kernel.Start'), 'the stack reaches Kernel.Start', stack.body.stackFrames.map((f) => f.name).join(' < '));

    const scopes = await request('scopes', { frameId: top.id });
    const locals = await request('variables', { variablesReference: scopes.body.scopes[0].variablesReference });
    const m = locals.body.variables.find((v) => v.name === 'm');
    check(m && m.type === 'MountInfo' && m.variablesReference > 0, 'locals: `m` is a MountInfo struct', JSON.stringify(locals.body.variables));
    const dev = locals.body.variables.find((v) => v.name === 'dev');
    check(dev && dev.type === 'byte*' && /"vda"/.test(dev.value), 'locals: byte* strings show their text', JSON.stringify(dev));
    if (m) {
        const fields = await request('variables', { variablesReference: m.variablesReference });
        const free = fields.body.variables.find((v) => v.name === 'free_bytes');
        check(free && free.type === 'ulong' && parseInt(free.value, 10) > 0, 'expand m: free_bytes is a ulong > 0', JSON.stringify(fields.body.variables));
    }
    const ev = await request('evaluate', { expression: 'm.TotalBytes > m.FreeBytes', frameId: top.id, context: 'watch' });
    check(ev.success && ev.body.result === 'true', 'evaluate a C# expression in the frame', JSON.stringify(ev.body || ev.message));
    const raw = await request('evaluate', { expression: '`info breakpoints', context: 'repl' });
    check(raw.success && /crash::fail/.test(raw.body.result), 'raw gdb command in the Debug Console; panic breakpoints are set', raw.body && raw.body.result);
    const regs = await request('variables', { variablesReference: scopes.body.scopes[1].variablesReference });
    check(regs.body.variables.some((v) => v.name === 'rip' && /^0xffffffff8/.test(v.value)), 'registers scope: rip in the higher half', JSON.stringify(regs.body.variables.slice(0, 2)));

    await request('next', { threadId: st.body.threadId });
    st = await stopped();
    stack = await request('stackTrace', { threadId: st.body.threadId });
    top = stack.body.stackFrames[0];
    check(st.body.reason === 'step' && top.line > kline, 'step over moves to a later line', JSON.stringify(top));

    await request('continue', { threadId: st.body.threadId });
    st = await stopped(120000);
    stack = await request('stackTrace', { threadId: st.body.threadId });
    top = stack.body.stackFrames[0];
    check(st.body.reason === 'breakpoint' && top.source && top.source.path === shell && top.line === sline, 'stops at the .neko breakpoint (Shell.Run)', JSON.stringify(top));

    // breakpoints can change while the kernel runs
    await request('continue', { threadId: st.body.threadId });
    await new Promise((r) => setTimeout(r, 500));
    const b3 = await request('setBreakpoints', { source: { path: shell }, breakpoints: [] });
    check(b3.success, 'breakpoints change while the kernel runs');
    check(!events.some((e) => e.event === 'stopped'), 'that does not show up as a stop');

    await request('pause', { threadId: 1 });
    st = await stopped();
    check(st.body.reason === 'pause', 'pause interrupts the running kernel', JSON.stringify(st.body));

    return finish(s, request);
})().catch((e) => { console.error(e); process.exit(1); });

async function finish(s, request) {
    if (request) await request('disconnect', {});
    else await s.shutdown();
    await new Promise((r) => setTimeout(r, 1500));
    const qemuGone = !s.qemu || s.qemu.exitCode !== null || s.qemu.signalCode !== null;
    check(qemuGone, 'disconnect stops QEMU');
    console.log(`debugger tests: ${passed} passed, ${failed} failed`);
    process.exit(failed ? 1 : 0);
}
