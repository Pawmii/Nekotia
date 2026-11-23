// Nekotia kernel debugger: a Debug Adapter Protocol implementation over gdb/MI and QEMU's gdb server.
// Plain JavaScript, no dependencies: VS Code runs it inline (DebugAdapterInlineImplementation), and the
// tests drive it directly (tests/debug/adapter-test.js).
//
//   launch  → `make debug` (-O0 kernel in build/debug, QEMU paused with a gdb server) → gdb --interpreter=mi3
//   CPUs are the threads; frames and variables are shown with C# names (Commands.Dmesg, byte*, ulong);
//   breakpoints work in .cs and .neko files (nekoc's #line directives put C# lines into the DWARF);
//   exception filters stop on a kernel panic (the stop screen) and on every C# `throw`.
'use strict';

const cp = require('child_process');
const path = require('path');

// ---------------------------------------------------------------- gdb/MI output parser

function parseMiValue(s, i) {
    if (s[i] === '"') {
        let out = '';
        i++;
        while (i < s.length && s[i] !== '"') {
            if (s[i] === '\\') {
                const c = s[++i];
                if (c === 'n') out += '\n';
                else if (c === 't') out += '\t';
                else if (c === 'r') out += '\r';
                else if (c >= '0' && c <= '7') {   // octal escape: bytes of UTF-8 text
                    let j = i;
                    while (j < i + 3 && s[j] >= '0' && s[j] <= '7') j++;
                    out += String.fromCharCode(parseInt(s.slice(i, j), 8));
                    i = j - 1;
                } else out += c;
                i++;
            } else out += s[i++];
        }
        // gdb escapes non-ASCII bytes one by one: turn the byte string back into UTF-8 text
        try { out = Buffer.from(out, 'latin1').toString('utf8'); } catch (e) { /* keep as is */ }
        return [out, i + 1];
    }
    if (s[i] === '{' || s[i] === '[') {
        const close = s[i] === '{' ? '}' : ']';
        const isList = s[i] === '[';
        i++;
        const obj = isList ? [] : {};
        while (i < s.length && s[i] !== close) {
            if (s[i] === ',') { i++; continue; }
            let key = null;
            const eq = s.slice(i).search(/^[A-Za-z_][\w-]*=/);
            if (eq === 0) {
                const m = /^([A-Za-z_][\w-]*)=/.exec(s.slice(i));
                key = m[1];
                i += m[0].length;
            }
            const [value, next] = parseMiValue(s, i);
            i = next;
            if (isList) obj.push(key === null ? value : { [key]: value });
            else if (key !== null) {
                if (key in obj) obj[key] = [].concat(obj[key], value);   // repeated keys (e.g. bkpt in lists)
                else obj[key] = value;
            }
        }
        return [obj, i + 1];
    }
    return [null, i];
}

/** One line of MI output → { token, kind ('^','*','=','~','@','&'), cls, results } */
function parseMiLine(line) {
    const m = /^(\d*)([\^*=~@&+])(.*)$/.exec(line);
    if (!m) return null;
    const token = m[1] ? parseInt(m[1], 10) : null;
    const kind = m[2];
    if (kind === '~' || kind === '@' || kind === '&') return { token, kind, text: parseMiValue(m[3], 0)[0] };
    const comma = m[3].indexOf(',');
    const cls = comma < 0 ? m[3] : m[3].slice(0, comma);
    const results = comma < 0 ? {} : parseMiValue('{' + m[3].slice(comma + 1) + '}', 0)[0];
    return { token, kind, cls, results };
}

// ---------------------------------------------------------------- C# presentation

const CS_TYPES = [
    [/\bunsigned char\b|\buint8_t\b/g, 'byte'], [/\bsigned char\b|\bint8_t\b/g, 'sbyte'],
    [/\bunsigned short\b|\buint16_t\b/g, 'ushort'], [/\bshort\b|\bint16_t\b/g, 'short'],
    [/\bunsigned int\b|\buint32_t\b/g, 'uint'], [/\bint32_t\b/g, 'int'],
    [/\bunsigned long( long)?\b|\buint64_t\b|\bsize_t\b|\buintptr_t\b/g, 'ulong'], [/\blong long\b|\bint64_t\b|\bintptr_t\b/g, 'long'],
    [/\bchar16_t\b/g, 'char'], [/\bconst char \*/g, 'string'], [/::Nekotia::|\bNekotia::(\w+::)*/g, ''],
    [/(::)?\bneko::(\w+::)*/g, ''],
];

function csType(t) {
    if (!t) return t;
    for (const [re, to] of CS_TYPES) t = t.replace(re, to);
    return t.replace(/ \*/g, '*');
}

/** Nekotia::OS::Commands::Dmesg(unsigned char*) → Commands.Dmesg; C++ runtime names stay as they are.
 *  nekoc's lambdas (interpolation, switch expressions): Kernel::MountDisk()::$_1::operator()() → Kernel.MountDisk (lambda) */
function csFunction(name) {
    if (!name) return '??';
    let lambda = '';
    const m = /^(.*?)\(\)(?: const)?::\$_\d+::operator\(\)/.exec(name);
    if (m) {
        name = m[1];
        lambda = ' (lambda)';
    }
    const bare = name.replace(/\(.*\)( const)?$/, '');
    if (!/^(::)?Nekotia::/.test(bare)) return bare + lambda;
    const parts = bare.replace(/^::/, '').split('::');
    return parts.slice(-2).join('.') + lambda;
}

/** C# member names of [Native] structs are C++ snake_case names: FreeBytes → free_bytes. */
function snake(expr) {
    return expr.replace(/(\.|->)([A-Z][A-Za-z0-9]*)/g, (all, sep, member) => sep + member.replace(/([a-z0-9])([A-Z])/g, '$1_$2').toLowerCase());
}

// ---------------------------------------------------------------- the session

const PANIC_FUNCTIONS = ['neko::crash::unhandled_exception', 'neko::crash::assert_failed', 'neko::crash::panic_at',
    'neko::crash::fail', 'neko::crash::cpu_exception'];
const THROW_FUNCTIONS = ['neko::exc::raise'];

class NekotiaDebugSession {
    constructor(options = {}) {
        this.listeners = [];
        this.seq = 1;
        this.log = options.log || (() => {});
        this.token = 1;
        this.pending = new Map();
        this.console = null;          // collects ~"…" output of a raw gdb command
        this.sourceBreakpoints = new Map();   // path → [gdb numbers]
        this.functionBreakpoints = [];
        this.exceptionBreakpoints = new Map(); // gdb number → 'panic' | 'throw'
        this.handles = new Map();
        this.nextHandle = 1;
        this.varobjs = [];
        this.stopped = false;
        this.onDidSendMessage = (listener) => {
            this.listeners.push(listener);
            return { dispose: () => { this.listeners = this.listeners.filter((l) => l !== listener); } };
        };
    }

    // ------------------------------------------------------------ DAP plumbing

    emit(msg) {
        msg.seq = this.seq++;
        for (const l of this.listeners) l(msg);
    }

    event(event, body) { this.emit({ type: 'event', event, body }); }

    output(text, category = 'console') { this.event('output', { category, output: text }); }

    async handleMessage(msg) {
        if (msg.type !== 'request') return;
        const respond = (body) => this.emit({ type: 'response', request_seq: msg.seq, command: msg.command, success: true, body });
        try {
            const handler = this['on_' + msg.command];
            if (!handler) {
                respond({});
                return;
            }
            respond(await handler.call(this, msg.arguments || {}));
        } catch (e) {
            this.emit({
                type: 'response', request_seq: msg.seq, command: msg.command, success: false, message: e.message,
                body: { error: { id: 1, format: e.message, showUser: msg.command === 'launch' } },
            });
        }
    }

    dispose() { this.shutdown(); }

    // ------------------------------------------------------------ gdb

    mi(command) {
        const token = this.token++;
        return new Promise((resolve, reject) => {
            if (!this.gdb || this.gdb.exitCode !== null) {
                reject(new Error('gdb is not running'));
                return;
            }
            this.pending.set(token, { resolve, reject, command });
            this.log(`→ ${token}${command}`);
            this.gdb.stdin.write(`${token}${command}\n`);
        });
    }

    /** A gdb CLI command; resolves with what it printed. */
    async cli(command) {
        this.console = [];
        try {
            await this.mi(`-interpreter-exec console ${JSON.stringify(command)}`);
            return this.console.join('');
        } finally {
            this.console = null;
        }
    }

    onGdbLine(line) {
        if (!line || line === '(gdb) ') return;
        this.log(`← ${line}`);
        const r = parseMiLine(line);
        if (!r) return;
        if (r.kind === '~' || r.kind === '&') {
            if (this.console) this.console.push(r.text);
            return;
        }
        if (r.kind === '^' && r.token !== null && this.pending.has(r.token)) {
            const p = this.pending.get(r.token);
            this.pending.delete(r.token);
            if (r.cls === 'error') p.reject(new Error(r.results.msg || 'gdb error'));
            else p.resolve(r.results);
            return;
        }
        if (r.kind === '*' && r.cls === 'stopped') this.onStopped(r.results);
        else if (r.kind === '*' && r.cls === 'running') this.stopped = false;
    }

    onStopped(s) {
        this.stopped = true;
        this.clearVariables();
        if (this.quietStop) {   // the stop right after connecting (QEMU starts paused)
            this.quietStop = false;
            return;
        }
        const threadId = parseInt(s['thread-id'] || '1', 10) || 1;
        let reason = 'pause';
        let description;
        let text;
        if (s.reason === 'breakpoint-hit') {
            const kind = this.exceptionBreakpoints.get(s.bkptno);
            if (kind) {
                reason = 'exception';
                description = kind === 'panic' ? 'Kernel panic — the stop screen is next' : 'C# exception thrown';
                text = description;
            } else reason = 'breakpoint';
        } else if (s.reason === 'end-stepping-range' || s.reason === 'function-finished' || s.reason === 'location-reached') {
            reason = 'step';
        } else if (s.reason === 'exited' || s.reason === 'exited-normally' || s.reason === 'exited-signalled') {
            this.event('terminated', {});
            return;
        }
        this.event('stopped', { reason, threadId, allThreadsStopped: true, description, text,
            hitBreakpointIds: s.bkptno ? [parseInt(s.bkptno, 10)] : undefined });
    }

    // ------------------------------------------------------------ lifecycle

    on_initialize() {
        return {
            supportsConfigurationDoneRequest: true,
            supportsFunctionBreakpoints: true,
            supportsConditionalBreakpoints: true,
            supportsHitConditionalBreakpoints: true,
            supportsEvaluateForHovers: true,
            supportsSetVariable: true,
            supportsTerminateRequest: true,
            exceptionBreakpointFilters: [
                { filter: 'panic', label: 'Kernel panic (stop screen)', default: true,
                  description: 'Stop before the stop screen: crash, failed assert, unhandled C# exception, CPU fault' },
                { filter: 'throw', label: 'C# throw', default: false, description: 'Stop on every `throw` in kernel code' },
            ],
        };
    }

    async on_launch(args) {
        const cwd = args.cwd;
        const elf = path.resolve(cwd, args.program || 'build/debug/nekotia.elf');
        const port = args.gdbPort || 1234;
        const makeArgs = [`GDB_PORT=${port}`, ...(args.makeArgs || [])];
        if (args.display === false) makeArgs.push('QEMU_EXTRA=-display none');

        // 1. build and start QEMU (paused) — make's output (build, then the serial console) goes to the Debug Console
        this.output(`nekotia: make debug ${makeArgs.join(' ')}\n`);
        this.qemu = cp.spawn(args.make || 'make', ['--no-print-directory', 'debug', ...makeArgs], { cwd, detached: true, stdio: ['pipe', 'pipe', 'pipe'] });
        const ready = new Promise((resolve, reject) => {
            let buffered = '';
            const onData = (chunk) => {
                const text = chunk.toString();
                this.output(text, 'stdout');
                buffered += text;
                if (/QEMU is waiting for gdb/.test(buffered)) resolve();
            };
            this.qemu.stdout.on('data', onData);
            this.qemu.stderr.on('data', (c) => this.output(c.toString(), 'stderr'));
            this.qemu.on('exit', (code) => {
                reject(new Error(`make debug exited with code ${code} before QEMU started — see the Debug Console`));
                if (!this.terminating) this.event('terminated', {});
                this.qemu = null;
            });
        });
        await ready;

        // 2. gdb, connected to QEMU
        this.gdb = cp.spawn(args.gdb || 'gdb', ['--interpreter=mi3', '-q', '-nx', elf], { cwd, stdio: ['pipe', 'pipe', 'pipe'] });
        let rest = '';
        this.gdb.stdout.on('data', (chunk) => {
            rest += chunk.toString('latin1');
            const lines = rest.split('\n');
            rest = lines.pop();
            for (const l of lines) this.onGdbLine(Buffer.from(l.replace(/\r$/, ''), 'latin1').toString('latin1'));
        });
        this.gdb.stderr.on('data', (c) => this.output(c.toString(), 'stderr'));
        this.gdb.on('exit', () => {
            for (const p of this.pending.values()) p.reject(new Error('gdb exited'));
            this.pending.clear();
        });
        await this.mi('-gdb-set mi-async on');   // commands (interrupt, breakpoints) while the kernel runs
        await this.mi('-gdb-set pagination off');
        await this.mi('-gdb-set confirm off');
        await this.cli('set disassembly-flavor intel').catch(() => {});
        this.quietStop = true;
        let connected = false;
        for (let attempt = 0; attempt < 20 && !connected; attempt++) {
            try {
                await this.mi(`-target-select remote localhost:${port}`);
                connected = true;
            } catch (e) {
                await new Promise((r) => setTimeout(r, 250));
            }
        }
        if (!connected) throw new Error(`gdb could not connect to QEMU on :${port}`);
        this.stopped = true;
        this.stopOnEntry = !!args.stopOnEntry;
        this.event('initialized', {});
        return {};
    }

    async on_configurationDone() {
        if (this.stopOnEntry) {
            this.event('stopped', { reason: 'entry', threadId: 1, allThreadsStopped: true });
            return {};
        }
        await this.mi('-exec-continue');
        return {};
    }

    async on_disconnect() { await this.shutdown(); return {}; }

    async on_terminate() {
        await this.shutdown();
        this.event('terminated', {});
        return {};
    }

    async shutdown() {
        if (this.terminating) return;
        this.terminating = true;
        if (this.gdb && this.gdb.exitCode === null) {
            try { this.gdb.stdin.write('-gdb-exit\n'); } catch (e) { /* gone */ }
            setTimeout(() => { if (this.gdb && this.gdb.exitCode === null) this.gdb.kill(); }, 1000);
        }
        if (this.qemu && this.qemu.exitCode === null) {
            try { process.kill(-this.qemu.pid, 'SIGTERM'); } catch (e) { /* gone */ }
        }
    }

    // ------------------------------------------------------------ breakpoints

    async insert(location, bp = {}) {
        let cmd = '-break-insert -f';
        if (bp.condition) cmd += ` -c ${JSON.stringify(bp.condition)}`;
        if (bp.hitCondition && /^\d+$/.test(bp.hitCondition.trim())) cmd += ` -i ${parseInt(bp.hitCondition, 10) - 1}`;
        const r = await this.mi(`${cmd} ${JSON.stringify(location)}`);
        const b = Array.isArray(r.bkpt) ? r.bkpt[0] : r.bkpt;
        // A C# line can also produce a lambda (string interpolation, switch expression): gdb then puts the
        // breakpoint in both. Keep the method's own location, otherwise every step stops in the lambda too.
        const locations = [].concat(b.locations || []).map((l) => l.location || l);
        if (locations.length > 1) {
            const lambdas = locations.filter((l) => /::\$_\d+::operator\(\)/.test(l.func || ''));
            if (lambdas.length && lambdas.length < locations.length)
                await this.mi(`-break-disable ${lambdas.map((l) => l.number).join(' ')}`).catch(() => {});
            b.addr = (locations.find((l) => !lambdas.includes(l)) || locations[0]).addr;
            b.line = b.line || (locations[0] || {}).line;
        }
        return b;
    }

    /** gdb refuses to insert while the target runs: interrupt, change, resume. */
    async whileStopped(work) {
        if (this.stopped) return work();
        const stopped = new Promise((resolve) => {
            const orig = this.onStopped.bind(this);
            this.onStopped = (s) => { this.onStopped = orig; this.stopped = true; resolve(); };
        });
        await this.mi('-exec-interrupt');
        await stopped;
        try {
            return await work();
        } finally {
            await this.mi('-exec-continue');
        }
    }

    async on_setBreakpoints(args) {
        const file = args.source.path;
        return this.whileStopped(async () => {
            const old = this.sourceBreakpoints.get(file) || [];
            if (old.length) await this.mi(`-break-delete ${old.join(' ')}`);
            const numbers = [];
            const breakpoints = [];
            for (const bp of args.breakpoints || []) {
                try {
                    const b = await this.insert(`${file}:${bp.line}`, bp);
                    numbers.push(b.number);
                    const pending = !b.addr || b.addr === '<PENDING>';
                    breakpoints.push({ id: parseInt(b.number, 10), verified: !pending, line: b.line ? parseInt(b.line, 10) : bp.line,
                        message: pending ? 'no code on this line (yet)' : undefined });
                } catch (e) {
                    breakpoints.push({ verified: false, line: bp.line, message: e.message });
                }
            }
            this.sourceBreakpoints.set(file, numbers);
            return { breakpoints };
        });
    }

    async on_setFunctionBreakpoints(args) {
        return this.whileStopped(async () => {
            if (this.functionBreakpoints.length) await this.mi(`-break-delete ${this.functionBreakpoints.join(' ')}`);
            this.functionBreakpoints = [];
            const breakpoints = [];
            for (const bp of args.breakpoints || []) {
                // C# names: Commands.Dmesg → Commands::Dmesg (gdb matches it in any namespace)
                const name = /::/.test(bp.name) ? bp.name : bp.name.replace(/\./g, '::');
                try {
                    const b = await this.insert(name, bp);
                    this.functionBreakpoints.push(b.number);
                    breakpoints.push({ id: parseInt(b.number, 10), verified: b.addr !== '<PENDING>' });
                } catch (e) {
                    breakpoints.push({ verified: false, message: e.message });
                }
            }
            return { breakpoints };
        });
    }

    async on_setExceptionBreakpoints(args) {
        return this.whileStopped(async () => {
            if (this.exceptionBreakpoints.size) await this.mi(`-break-delete ${[...this.exceptionBreakpoints.keys()].join(' ')}`);
            this.exceptionBreakpoints.clear();
            const filters = args.filters || [];
            const wanted = [];
            if (filters.includes('panic')) wanted.push(...PANIC_FUNCTIONS.map((f) => [f, 'panic']));
            if (filters.includes('throw')) wanted.push(...THROW_FUNCTIONS.map((f) => [f, 'throw']));
            for (const [fn, kind] of wanted) {
                try {
                    const b = await this.insert(fn);
                    this.exceptionBreakpoints.set(b.number, kind);
                } catch (e) { /* not in this build */ }
            }
            return {};
        });
    }

    // ------------------------------------------------------------ execution

    async on_threads() {
        const r = await this.mi('-thread-info');
        const threads = (r.threads || []).map((t) => ({ id: parseInt(t.id, 10), name: `CPU ${parseInt(t.id, 10) - 1}` }));
        return { threads: threads.length ? threads : [{ id: 1, name: 'CPU 0' }] };
    }

    async on_continue() { await this.mi('-exec-continue'); return { allThreadsContinued: true }; }
    async on_next(a) { await this.mi(`-exec-next --thread ${a.threadId}`); return {}; }
    async on_stepIn(a) { await this.mi(`-exec-step --thread ${a.threadId}`); return {}; }
    async on_stepOut(a) { await this.mi(`-exec-finish --thread ${a.threadId} --frame 0`); return {}; }
    async on_pause() { await this.mi('-exec-interrupt'); return {}; }

    // ------------------------------------------------------------ stack and variables

    handle(value) {
        const id = this.nextHandle++;
        this.handles.set(id, value);
        return id;
    }

    clearVariables() {
        const names = this.varobjs;
        this.varobjs = [];
        this.handles.clear();
        this.nextHandle = 1;
        for (const n of names) this.mi(`-var-delete ${n}`).catch(() => {});
    }

    async on_stackTrace(args) {
        const r = await this.mi(`-stack-list-frames --thread ${args.threadId}`);
        const frames = (r.stack || []).map((f) => f.frame || f);
        const start = args.startFrame || 0;
        const count = args.levels || frames.length;
        const stackFrames = frames.slice(start, start + count).map((f) => {
            const level = parseInt(f.level, 10);
            const file = f.fullname || f.file;
            return {
                id: this.handle({ kind: 'frame', thread: args.threadId, level }),
                name: csFunction(f.func) + (file ? '' : ` [${f.addr}]`),
                source: file ? { name: path.basename(file), path: file } : undefined,
                line: f.line ? parseInt(f.line, 10) : 0,
                column: 1,
                presentationHint: file ? 'normal' : 'subtle',
            };
        });
        return { stackFrames, totalFrames: frames.length };
    }

    async on_scopes(args) {
        const frame = this.handles.get(args.frameId);
        if (!frame) return { scopes: [] };
        return {
            scopes: [
                { name: 'Locals', presentationHint: 'locals', expensive: false,
                  variablesReference: this.handle({ kind: 'locals', thread: frame.thread, level: frame.level }) },
                { name: 'Registers', presentationHint: 'registers', expensive: true,
                  variablesReference: this.handle({ kind: 'registers', thread: frame.thread, level: frame.level }) },
            ],
        };
    }

    /** A varobj → DAP variable (expandable when it has children). */
    variable(name, v) {
        const numchild = parseInt(v.numchild || '0', 10);
        const dynamic = v.dynamic === '1';
        return {
            name,
            value: v.value === undefined ? '' : v.value,
            type: csType(v.type),
            evaluateName: v.exp || name,
            variablesReference: numchild > 0 || dynamic ? this.handle({ kind: 'children', varobj: v.name }) : 0,
        };
    }

    async on_variables(args) {
        const h = this.handles.get(args.variablesReference);
        if (!h) return { variables: [] };
        if (h.kind === 'locals') {
            const r = await this.mi(`-stack-list-variables --thread ${h.thread} --frame ${h.level} --no-values`);
            const variables = [];
            for (const v of r.variables || []) {
                try {
                    const o = await this.mi(`-var-create --thread ${h.thread} --frame ${h.level} - * ${JSON.stringify(v.name)}`);
                    this.varobjs.push(o.name);
                    const dap = this.variable(v.name, o);
                    this.handles.get(args.variablesReference).names = Object.assign(this.handles.get(args.variablesReference).names || {}, { [v.name]: o.name });
                    variables.push(dap);
                } catch (e) {
                    variables.push({ name: v.name, value: `<${e.message}>`, variablesReference: 0 });
                }
            }
            return { variables };
        }
        if (h.kind === 'children') return { variables: await this.children(h.varobj, h) };
        if (h.kind === 'registers') {
            const names = ['rip', 'rsp', 'rbp', 'rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'eflags', 'cr0', 'cr2', 'cr3', 'cr4'];
            const variables = [];
            for (const n of names) {
                try {
                    const r = await this.mi(`-data-evaluate-expression --thread ${h.thread} --frame ${h.level} ${JSON.stringify('$' + n)}`);
                    const value = /^-?\d+$/.test(r.value) ? '0x' + BigInt.asUintN(64, BigInt(r.value)).toString(16).padStart(16, '0') : r.value;
                    variables.push({ name: n, value, variablesReference: 0 });
                } catch (e) { /* not available on this target */ }
            }
            return { variables };
        }
        return { variables: [] };
    }

    async children(varobj, holder) {
        const r = await this.mi(`-var-list-children --all-values ${varobj}`);
        const out = [];
        for (const c of [].concat(r.children || []).map((x) => x.child || x)) {
            if (['public', 'private', 'protected'].includes(c.exp)) {   // C++ access sections: flatten
                out.push(...await this.children(c.name, holder));
                continue;
            }
            holder.names = Object.assign(holder.names || {}, { [c.exp]: c.name });
            out.push(this.variable(c.exp, c));
        }
        return out;
    }

    async on_setVariable(args) {
        const h = this.handles.get(args.variablesReference);
        const varobj = h && h.names && h.names[args.name];
        if (!varobj) throw new Error('cannot change this value');
        const r = await this.mi(`-var-assign ${varobj} ${JSON.stringify(args.value)}`);
        return { value: r.value };
    }

    async on_evaluate(args) {
        const expr = args.expression.trim();
        if (args.context === 'repl' && /^(`|-|gdb\s)/.test(expr)) {   // raw gdb: `info registers, -data-…, gdb x/4gx $rsp
            const command = expr.replace(/^`|^gdb\s+/, '');
            if (command.startsWith('-')) return { result: JSON.stringify(await this.mi(command)), variablesReference: 0 };
            return { result: (await this.cli(command)).trimEnd(), variablesReference: 0 };
        }
        const frame = args.frameId && this.handles.get(args.frameId);
        const where = frame ? `--thread ${frame.thread} --frame ${frame.level} ` : '';
        // C# `Kernel.Version` is C++ `Kernel::Version`; try as written first
        const candidates = [expr];
        if (/(\.|->)[A-Z]/.test(expr)) candidates.push(snake(expr));   // fields of [Native] structs
        if (/\b[A-Z]\w*\.\w/.test(expr)) candidates.push(expr.replace(/\b([A-Z]\w*)\.(?=\w)/g, '$1::'));
        let lastError;
        for (const e of candidates) {
            try {
                const o = await this.mi(`-var-create ${where}- * ${JSON.stringify(e)}`);
                this.varobjs.push(o.name);
                const v = this.variable(expr, o);
                return { result: v.value, type: v.type, variablesReference: v.variablesReference };
            } catch (err) {
                lastError = err;
            }
        }
        throw lastError;
    }
}

module.exports = { NekotiaDebugSession, parseMiLine, csType, csFunction };
