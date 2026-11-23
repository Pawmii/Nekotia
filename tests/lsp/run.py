#!/usr/bin/env python3
"""Tests for `nekoc lsp`: a scripted editor session over stdio against the real sources.

Edits happen only in the server's memory (didOpen/didChange), never on disk.
Usage: tests/lsp/run.py [path/to/nekoc.dll]
"""
import json
import os
import subprocess
import sys
import threading
import queue

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NEKOC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/tools/nekoc/nekoc.dll")


class Server:
    def __init__(self):
        self.p = subprocess.Popen(["dotnet", NEKOC, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, cwd=ROOT)
        self.next_id = 1
        self.responses = {}
        self.notes = queue.Queue()
        self.cond = threading.Condition()
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        out = self.p.stdout
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            msg = json.loads(out.read(length))
            if "id" in msg and "method" not in msg:
                with self.cond:
                    self.responses[msg["id"]] = msg
                    self.cond.notify_all()
            else:
                self.notes.put(msg)

    def send(self, msg):
        msg["jsonrpc"] = "2.0"
        body = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def request(self, method, params, timeout=60):
        i = self.next_id
        self.next_id += 1
        self.send({"id": i, "method": method, "params": params})
        with self.cond:
            if not self.cond.wait_for(lambda: i in self.responses, timeout):
                raise TimeoutError(method)
            r = self.responses.pop(i)
        if "error" in r:
            return {"__error": r["error"]["message"]}
        return r.get("result")

    def notify(self, method, params):
        self.send({"method": method, "params": params})

    def diagnostics_for(self, uri, timeout=60, want=None):
        """The next publishDiagnostics for uri (optionally: the first that satisfies `want`)."""
        import time
        end = time.time() + timeout
        while time.time() < end:
            try:
                n = self.notes.get(timeout=end - time.time())
            except queue.Empty:
                break
            if n.get("method") == "textDocument/publishDiagnostics" and n["params"]["uri"] == uri:
                d = n["params"]["diagnostics"]
                if want is None or want(d):
                    return d
        return None


def uri(rel):
    return "file://" + os.path.join(ROOT, rel)


def text(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def pos_of(src, needle, offset=0, nth=0):
    i = -1
    for _ in range(nth + 1):
        i = src.index(needle, i + 1)
    i += offset
    line = src.count("\n", 0, i)
    col = i - (src.rfind("\n", 0, i) + 1)
    return {"line": line, "character": col}


passed = failed = 0


def check(cond, name, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok    {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}" + (f"  — {detail}" if detail else ""))


def main():
    s = Server()
    init = s.request("initialize", {"processId": None, "rootUri": "file://" + ROOT, "capabilities": {}})
    caps = init["capabilities"]
    check(caps.get("renameProvider") and caps.get("semanticTokensProvider"), "initialize: capabilities")
    s.notify("initialized", {})

    kernel = "kernel/Kernel.cs"
    ksrc = text(kernel)
    s.notify("textDocument/didOpen", {"textDocument": {"uri": uri(kernel), "languageId": "csharp", "version": 1, "text": ksrc}})

    # hover on Log.Info: C# signature, XML summary, C++ name
    h = s.request("textDocument/hover", {"textDocument": {"uri": uri(kernel)}, "position": pos_of(ksrc, "Log.Info(", 5)})
    v = h["contents"]["value"] if h else ""
    check("Log.Info(string text)" in v and "::neko::log::info" in v, "hover: signature and the C++ name of a [Native] method", v[:200])

    # definition: the C# declaration and the C++ implementation
    d = s.request("textDocument/definition", {"textDocument": {"uri": uri(kernel)}, "position": pos_of(ksrc, "Log.Info(", 5)})
    files = [os.path.relpath(x["uri"][7:], ROOT) for x in d or []]
    check("sdk/Nekotia.Sdk/Hal/Log.cs" in files, "definition: the C# declaration", str(files))
    check(any(f.startswith("runtime/") for f in files), "definition: the C++ runtime function behind [Native]", str(files))

    # completion after `Log.`
    edited = ksrc.replace("Log.Info(\"disk: /dev/vda was empty", "Log.\n            Log.Info(\"disk: /dev/vda was empty", 1)
    s.notify("textDocument/didChange", {"textDocument": {"uri": uri(kernel), "version": 2}, "contentChanges": [{"text": edited}]})
    c = s.request("textDocument/completion", {"textDocument": {"uri": uri(kernel)}, "position": pos_of(edited, "Log.\n", 4)})
    labels = {i["label"] for i in (c or {}).get("items", [])}
    check({"Info", "Warn", "Error", "Debug", "SetLevel"} <= labels and "Equals" not in labels, "completion: members after `Log.`", str(sorted(labels))[:200])
    # the edit is a syntax error: diagnostics arrive
    diags = s.diagnostics_for(uri(kernel), want=lambda d: any(x["severity"] == 1 for x in d))
    check(diags is not None, "diagnostics: a syntax error is reported while typing")
    s.notify("textDocument/didChange", {"textDocument": {"uri": uri(kernel), "version": 3}, "contentChanges": [{"text": ksrc}]})
    diags = s.diagnostics_for(uri(kernel), want=lambda d: not d)
    check(diags == [], "diagnostics: cleared when the error is fixed")

    # nekoc's own diagnostics (the supported subset), not only Roslyn's
    bad = ksrc.replace("static void StartNetwork()\n    {", "static void StartNetwork()\n    {\n        object boxed = 1;", 1)
    s.notify("textDocument/didChange", {"textDocument": {"uri": uri(kernel), "version": 4}, "contentChanges": [{"text": bad}]})
    diags = s.diagnostics_for(uri(kernel), want=lambda d: any(x.get("source") == "nekoc" for x in d))
    check(diags is not None, "diagnostics: nekoc's subset checks (NEKOxxxx) in the editor", str(diags)[:200])
    if diags:
        nd = [x for x in diags if x.get("source") == "nekoc"][0]
        check(nd["range"]["start"]["line"] == pos_of(bad, "object boxed")["line"], "diagnostics: on the right line")
    s.notify("textDocument/didChange", {"textDocument": {"uri": uri(kernel), "version": 5}, "contentChanges": [{"text": ksrc}]})

    # .neko: positions on sugared lines map back and forth
    neko = "kernel/Shell/Commands.neko"
    nsrc = text(neko)
    s.notify("textDocument/didOpen", {"textDocument": {"uri": uri(neko), "languageId": "nekotia", "version": 1, "text": nsrc}})
    line = "        for int i in 0..length {"
    p = pos_of(nsrc, line, line.index("length"))
    h = s.request("textDocument/hover", {"textDocument": {"uri": uri(neko)}, "position": p})
    check(h and "length" in h["contents"]["value"] and h["range"]["start"] == p, ".neko: hover on a `for i in` line hits the right identifier",
          str(h)[:200])
    refs = s.request("textDocument/references", {"textDocument": {"uri": uri(neko)}, "position": p, "context": {"includeDeclaration": True}})
    lines_src = nsrc.split("\n")
    ok = refs and all(lines_src[r["range"]["start"]["line"]][r["range"]["start"]["character"]:r["range"]["end"]["character"]] == "length"
                      for r in refs if r["uri"] == uri(neko))
    check(bool(ok) and len(refs) >= 2, ".neko: references land exactly on the name", str(refs)[:200])

    # rename a local in .neko: every edit replaces exactly the old name
    p = pos_of(nsrc, line, line.index(" i ") + 1)
    pr = s.request("textDocument/prepareRename", {"textDocument": {"uri": uri(neko)}, "position": p})
    check(pr and pr.get("placeholder") == "i", ".neko: prepareRename on the loop variable", str(pr))
    r = s.request("textDocument/rename", {"textDocument": {"uri": uri(neko)}, "position": p, "newName": "index"})
    edits = (r or {}).get("changes", {}).get(uri(neko), [])
    check(len(edits) >= 2 and all(lines_src[e["range"]["start"]["line"]][e["range"]["start"]["character"]:e["range"]["end"]["character"]] == "i"
                                  for e in edits), ".neko: rename edits only the variable", str(edits)[:200])

    # rename across files: a method declared in one file and called in another
    p = pos_of(ksrc, "StartNetwork()", 2)
    r = s.request("textDocument/rename", {"textDocument": {"uri": uri(kernel)}, "position": p, "newName": "BringUpNetwork"})
    edits = (r or {}).get("changes", {}).get(uri(kernel), [])
    check(len(edits) == ksrc.count("StartNetwork("), "rename: declaration and calls", str(edits)[:200])

    # rename an SDK member used by the kernel and by user programs
    log = "sdk/Nekotia.Sdk/Hal/Log.cs"
    lsrc = text(log)
    r = s.request("textDocument/rename", {"textDocument": {"uri": uri(log)}, "position": pos_of(lsrc, "void StopBootCapture", 5), "newName": "EndBootCapture"})
    files = sorted(os.path.relpath(u[7:], ROOT) for u in (r or {}).get("changes", {}))
    check(log in files and any(f.startswith("kernel/") for f in files), "rename: SDK declaration and its kernel users", str(files))

    # outline, workspace symbols, signature help
    ds = s.request("textDocument/documentSymbol", {"textDocument": {"uri": uri(neko)}})
    names = [c["name"] for t in (ds or []) for c in t.get("children", [])]
    check("Dmesg" in names and "Run" in names, "outline: methods of a .neko class", str(names[:10]))
    ws = s.request("workspace/symbol", {"query": "StopBootCap"})
    check(any(x["name"] == "StopBootCapture" for x in ws or []), "workspace symbols")
    sh = s.request("textDocument/signatureHelp", {"textDocument": {"uri": uri(kernel)}, "position": pos_of(ksrc, "Fat.Mount(dev, ", 15)})
    check(sh and sh["activeParameter"] == 1 and "Mount" in sh["signatures"][sh["activeSignature"]]["label"], "signature help: active parameter",
          str(sh)[:200])

    # semantic tokens: [Native] methods carry the `native` modifier
    st = s.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri(kernel)}})
    data = (st or {}).get("data", [])
    legend = caps["semanticTokensProvider"]["legend"]
    native_bit = 1 << legend["tokenModifiers"].index("native")
    check(len(data) > 500 and len(data) % 5 == 0, "semantic tokens: identifiers classified", str(len(data)))
    line = col = 0
    found = False
    klines = ksrc.split("\n")
    for k in range(0, len(data), 5):
        line += data[k]
        col = col + data[k + 1] if data[k] == 0 else data[k + 1]
        if klines[line][col:col + data[k + 2]] == "Info" and data[k + 4] & native_bit:
            found = True
    check(found, "semantic tokens: Log.Info is marked native")

    # code lens: "C++" over methods
    cl = s.request("textDocument/codeLens", {"textDocument": {"uri": uri(neko)}})
    check(cl and all(x["command"]["command"] == "nekotia.showGeneratedCpp" for x in cl) and len(cl) > 10, "code lens: generated C++ per method")

    s.request("shutdown", None)
    s.notify("exit", None)
    s.p.wait(timeout=10)
    check(s.p.returncode == 0, "shutdown and exit")

    print(f"lsp tests: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
