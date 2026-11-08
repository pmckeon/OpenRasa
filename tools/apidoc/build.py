# Builds the HTML API reference from the spec_*.py files into OUT.
import html, json, os, re, sys, importlib.util, glob

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "site")
os.makedirs(OUT, exist_ok=True)

MODULES, OBJECTS = [], []
for path in sorted(glob.glob(os.path.join(HERE, "spec_*.py"))):
    spec = importlib.util.spec_from_file_location(os.path.basename(path)[:-3], path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    MODULES += getattr(m, "MODULES", [])
    OBJECTS += getattr(m, "OBJECTS", [])
order = ["extern_trmain_trmain", "extern_gameclient_entitybody", "extern_gameclient_clientmovingentitycontroller",
         "extern_game_characterstatemachine", "extern_gameclient_gameclient", "extern_gameclient_optionsmanager_wrap",
         "extern_gameclient_option_wrap", "extern_gameclient_usercontroller", "extern_gameclient_world", "extern_util_config"]
MODULES.sort(key=lambda m: order.index(m["name"]))
OBJECTS.sort(key=lambda o: o["name"])

e = html.escape


def page_id(*parts):
    return ".".join(p.lower() for p in parts)


def inline(text):
    """Escape, then `code` spans and [[target|label]] links."""
    t = e(text)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\[\[([^|\]]+)\|([^\]]+)\]\]", lambda m: f'<a href="{m.group(1).lower()}.html">{m.group(2)}</a>', t)
    return t


def paras(text):
    if not text:
        return ""
    return "".join(f"<p>{inline(p.strip())}</p>" for p in text.strip().split("\n\n"))


def first_sentence(text):
    text = (text or "").strip().split("\n\n")[0]
    m = re.match(r"(.+?[.!?])(\s|$)", text)
    s = m.group(1) if m else text
    return re.sub(r"\[\[([^|\]]+)\|([^\]]+)\]\]", r"\2", s).replace("`", "")


# ---- navigation data (one shared nav.js) ---------------------------------------------------------------------------------------
nav = []
for m in MODULES:
    mid = page_id(m["name"])
    kids = []
    for en in m.get("enums", []):
        kids.append({"t": en["name"], "h": page_id(m["name"], en["name"]), "k": "enum"})
    for c in m.get("classes", []):
        cid = page_id(m["name"], c["name"])
        kids.append({"t": c["name"], "h": cid, "k": "class",
                     "c": [{"t": f["name"], "h": page_id(m["name"], c["name"], f["name"])} for f in c["methods"]]})
    for f in m.get("functions", []):
        kids.append({"t": f["name"], "h": page_id(m["name"], f["name"])})
    nav.append({"t": m["name"], "h": mid, "k": "module", "c": kids})
navobj = [{"t": o["name"], "h": page_id(o["name"]), "k": "object",
           "c": [{"t": f["name"], "h": page_id(o["name"], f["name"])} for f in o["methods"]]} for o in OBJECTS]

NAVJS = """// Sidebar: the API tree and its filter.
const TREE = %s;
const OBJECTS = %s;
(function () {
  const here = (location.pathname.split('/').pop() || 'index.html').replace(/\\.html$/, '');
  const box = document.getElementById('tree');
  function item(n, depth) {
    const li = document.createElement('li');
    const a = document.createElement('a');
    a.href = n.h + '.html'; a.textContent = n.t; a.dataset.name = n.t.toLowerCase();
    if (n.k) a.className = 'k-' + n.k;
    if (n.h === here) { a.classList.add('current'); li.classList.add('open'); }
    li.appendChild(a);
    if (n.c && n.c.length) {
      const t = document.createElement('button'); t.className = 'tog'; t.setAttribute('aria-label', 'Expand');
      t.onclick = () => li.classList.toggle('open'); li.insertBefore(t, a);
      const ul = document.createElement('ul');
      n.c.forEach(c => ul.appendChild(item(c, depth + 1)));
      li.appendChild(ul);
      if (here === n.h || here.startsWith(n.h + '.')) li.classList.add('open');
    }
    return li;
  }
  function section(title, nodes) {
    const h = document.createElement('p'); h.className = 'cap'; h.textContent = title; box.appendChild(h);
    const ul = document.createElement('ul'); nodes.forEach(n => ul.appendChild(item(n, 0))); box.appendChild(ul);
  }
  section('Modules', TREE); section('Objects', OBJECTS);
  const q = document.getElementById('q');
  q.addEventListener('input', () => {
    const v = q.value.trim().toLowerCase();
    box.classList.toggle('filtering', !!v);
    box.querySelectorAll('li').forEach(li => {
      const own = li.querySelector(':scope > a').dataset.name.includes(v);
      const sub = [...li.querySelectorAll('a')].some(a => a.dataset.name.includes(v));
      li.style.display = !v || own || sub ? '' : 'none';
      if (v) li.classList.toggle('open', sub && !own || sub);
    });
  });
  const cur = box.querySelector('a.current'); if (cur) cur.scrollIntoView({block: 'center'});
})();
""" % (json.dumps(nav), json.dumps(navobj))

CSS = """
:root { --bg:#fcfcfc; --fg:#404040; --muted:#6a6a6a; --head:#2b2b2b; --link:#2980b9; --visited:#9b59b6; --side:#343131;
  --sidefg:#d9d9d9; --sidecap:#55a5d9; --top:#2980b9; --code:#f8f8f8; --border:#e1e4e5; --row:#f3f6f6; --pill:#e7f2fa;
  --pillfg:#2980b9; --note:#fff8e6; --notebd:#f0b37e; --cur:#fcfcfc; --curfg:#404040; color-scheme:light }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --bg:#1e1f22; --fg:#d4d4d4; --muted:#9a9a9a;
  --head:#ececec; --link:#6cb4e8; --visited:#c18fdc; --side:#151516; --sidefg:#c8c8c8; --sidecap:#6cb4e8; --top:#1f5f8b;
  --code:#26282c; --border:#3a3d42; --row:#24262a; --pill:#20384b; --pillfg:#8cc6ef; --note:#3a3020; --notebd:#a8763e;
  --cur:#2a2c30; --curfg:#ececec; color-scheme:dark } }
:root[data-theme="dark"] { --bg:#1e1f22; --fg:#d4d4d4; --muted:#9a9a9a; --head:#ececec; --link:#6cb4e8; --visited:#c18fdc;
  --side:#151516; --sidefg:#c8c8c8; --sidecap:#6cb4e8; --top:#1f5f8b; --code:#26282c; --border:#3a3d42; --row:#24262a;
  --pill:#20384b; --pillfg:#8cc6ef; --note:#3a3020; --notebd:#a8763e; --cur:#2a2c30; --curfg:#ececec; color-scheme:dark }
* { box-sizing:border-box }
body { margin:0; background:var(--bg); color:var(--fg); font:16px/1.6 Lato, "Segoe UI", Helvetica, Arial, sans-serif }
a { color:var(--link); text-decoration:none } a:hover { text-decoration:underline }
code, pre, .sig { font-family:"Consolas", "SFMono-Regular", Menlo, monospace; font-size:0.85em }
.wrap { display:flex; min-height:100vh }
nav.side { width:300px; flex:none; background:var(--side); color:var(--sidefg); position:sticky; top:0; height:100vh;
  overflow-y:auto; font-size:0.9em }
.side .top { background:var(--top); padding:14px 16px; text-align:center; position:sticky; top:0; z-index:1 }
.side .top a { color:#fff; font-weight:700; font-size:1.05em }
.side .top input { margin-top:10px; width:100%; border:1px solid #2472a4; border-radius:50px; padding:6px 12px; font:inherit;
  font-size:0.85em; background:#fff; color:#404040 }
.side .cap { color:var(--sidecap); text-transform:uppercase; font-weight:700; font-size:0.85em; margin:16px 16px 4px;
  letter-spacing:0.04em }
.side ul { list-style:none; margin:0; padding:0 }
.side li { position:relative }
.side li ul { display:none; padding-left:14px } .side li.open > ul { display:block }
.side a { display:block; color:var(--sidefg); padding:3px 12px 3px 30px; word-break:break-all }
.side a:hover { background:#4e4a4a; text-decoration:none; color:#fff }
.side a.current { background:var(--cur); color:var(--curfg); font-weight:700 }
.side a.k-module { font-weight:700 }
.side .tog { position:absolute; left:10px; top:6px; width:14px; height:14px; border:0; background:none; cursor:pointer; padding:0 }
.side .tog::before { content:""; display:block; width:0; height:0; border:5px solid transparent; border-left-color:#999;
  margin-left:3px; transition:transform .15s }
.side li.open > .tog::before { transform:rotate(90deg); transform-origin:2px 5px }
main { flex:1; min-width:0; padding:24px max(16px, env(safe-area-inset-right, 0px)) 48px max(16px, env(safe-area-inset-left, 0px)) }
.doc { max-width:860px; margin:0 auto }
.crumbs { font-size:0.85em; color:var(--muted); padding-bottom:10px; border-bottom:1px solid var(--border); margin-bottom:24px }
h1, h2, h3 { font-family:"Roboto Slab", Georgia, serif; color:var(--head); font-weight:700; line-height:1.25; text-wrap:balance }
h1 { font-size:1.9em; margin:0 0 20px; overflow-wrap:anywhere } h2 { font-size:1.4em; margin:32px 0 12px } h3 { font-size:1.1em; margin:20px 0 8px }
.sig, pre { display:block; background:var(--code); border:1px solid var(--border); padding:10px 12px; overflow-x:auto;
  white-space:pre-wrap; word-break:break-word }
p code, td code, li code { background:var(--code); border:1px solid var(--border); padding:0 4px; color:#e74c3c }
.tbl { overflow-x:auto; margin:12px 0 }
table { border-collapse:collapse; width:100%; font-size:0.92em }
th, td { border:1px solid var(--border); padding:7px 12px; text-align:left; vertical-align:top }
th { font-weight:700 } tbody tr:nth-child(odd) { background:var(--row) }
td:first-child { white-space:nowrap }
.param { margin:14px 0 } .param .sig { margin-bottom:6px }
.ptype { color:var(--muted); font-style:italic; margin-left:6px; font-size:0.9em }
.pill { display:inline-block; background:var(--pill); color:var(--pillfg); border-radius:3px; padding:0 6px; font-size:0.75em;
  font-weight:700; vertical-align:middle; margin-left:8px; font-family:Lato, sans-serif; text-transform:uppercase; letter-spacing:.03em }
.note { background:var(--note); border-left:4px solid var(--notebd); padding:8px 12px; margin:12px 0 }
.note p { margin:4px 0 }
.pn { display:flex; justify-content:space-between; gap:16px; border-top:1px solid var(--border); margin-top:40px; padding-top:14px;
  font-size:0.9em }
.muted { color:var(--muted) }
@media (max-width: 800px) { .wrap { display:block } nav.side { position:static; width:auto; height:auto; max-height:45vh } }
"""


def layout(title, crumbs, body, prevnext=None):
    cr = " » ".join(f'<a href="{h}.html">{e(t)}</a>' if h else e(t) for t, h in crumbs)
    pn = ""
    if prevnext:
        p, n = prevnext
        pn = '<div class="pn"><span>' + (f'« <a href="{p[1]}.html">{e(p[0])}</a>' if p else "") + "</span><span>" + \
             (f'<a href="{n[1]}.html">{e(n[0])}</a> »' if n else "") + "</span></div>"
    return f"""<title>{e(title)} - Tabula Rasa Python API</title>
<meta name="claude:viewport-fit-sides" content="cover">
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Lato:wght@400;700&family=Roboto+Slab:wght@700&display=swap">
<link rel="stylesheet" href="style.css">
<div class="wrap">
<nav class="side"><div class="top"><a href="index.html">Tabula Rasa Python API</a><input id="q" type="search" placeholder="Search docs" aria-label="Filter the API tree"></div><div id="tree"></div></nav>
<main><div class="doc"><div class="crumbs">{cr}</div>
{body}{pn}
</div></main></div>
<script src="nav.js"></script>
"""


def write(name, content):
    with open(os.path.join(OUT, name + ".html"), "w", encoding="utf-8", newline="\n") as f:
        f.write("<!doctype html>\n<html lang=\"en\">\n" + content + "</html>\n")


def sig_of(prefix, f):
    return f"{prefix}{f['name']}({f.get('sig', '')})"


def value_table(rows, head=("Value", "Meaning")):
    out = '<div class="tbl"><table><thead><tr>' + "".join(f"<th>{e(h)}</th>" for h in head) + "</tr></thead><tbody>"
    for r in rows:
        out += "<tr>" + "".join(f"<td>{inline(str(c))}</td>" for c in r) + "</tr>"
    return out + "</tbody></table></div>"


def func_page(f, owner_title, prefix, crumbs, pid, prevnext, kind):
    title = f"{owner_title}.{f['name']}"
    b = f"<h1>{e(title)} {kind}</h1>"
    b += "<h2 id=\"definition\">Definition</h2>" + paras(f.get("desc", ""))
    b += f'<span class="sig">{e(sig_of(prefix, f))}</span>'
    params = f.get("params", [])
    if params:
        b += '<h2 id="parameters">Parameters</h2>'
        for p in params:
            name, typ, desc = p[0], p[1], p[2]
            b += f'<div class="param"><span class="sig">{e(name)}</span><span class="ptype">{e(typ)}</span>{paras(desc)}'
            if len(p) > 3 and p[3]:
                b += value_table(p[3])
            b += "</div>"
    b += '<h2 id="returns">Returns</h2>' + paras(f.get("returns") or "None.")
    if f.get("returns_table"):
        b += value_table(f["returns_table"])
    if f.get("errors"):
        b += '<h2 id="exceptions">Exceptions</h2>' + value_table(f["errors"], ("Exception", "Condition"))
    if f.get("remarks"):
        b += '<h2 id="remarks">Remarks</h2>' + paras(f["remarks"])
    if f.get("openrasa"):
        b += '<div class="note"><p><strong>OpenRasa</strong></p>' + paras(f["openrasa"]) + "</div>"
    if f.get("engine"):
        b += f'<h2 id="engine">Engine</h2><p class="muted">tabula_rasa.exe: {inline(f["engine"])}</p>'
    write(pid, layout(title, crumbs, b, prevnext))


def member_table(rows, head):
    out = '<div class="tbl"><table><thead><tr>' + "".join(f"<th>{h}</th>" for h in head) + "</tr></thead><tbody>"
    for link, sig, desc in rows:
        out += f'<tr><td><a href="{link}.html">{e(sig)}</a></td><td>{e(desc)}</td></tr>'
    return out + "</tbody></table></div>"


def prevnext_list(items):
    res = []
    for i in range(len(items)):
        res.append((items[i - 1] if i else None, items[i + 1] if i + 1 < len(items) else None))
    return res


root = ("Docs", "index")
api = ("Python API", "index")
count = 0
for m in MODULES:
    mid = page_id(m["name"])
    crumbs = [root, (m["name"], mid)]
    b = f"<h1>{e(m['name'])} Module</h1><h2>Description</h2>" + paras(m["desc"])
    attrs = [(page_id(m["name"], en["name"]), en["name"], f"Enumeration - {first_sentence(en['desc'])}") for en in m.get("enums", [])]
    attrs += [(page_id(m["name"], c["name"]), c["name"], f"Class - {first_sentence(c['desc'])}") for c in m.get("classes", [])]
    if attrs:
        b += "<h2>Attributes</h2>" + member_table(attrs, ("Name", "Description"))
    if m.get("functions"):
        b += "<h2>Functions</h2>" + member_table([(page_id(m["name"], f["name"]), f"{f['name']}({f.get('sig', '')})",
                                                   first_sentence(f.get("desc"))) for f in m["functions"]], ("Function", "Description"))
    if m.get("remarks"):
        b += "<h2>Remarks</h2>" + paras(m["remarks"])
    write(mid, layout(m["name"], crumbs, b))
    count += 1
    funcs = m.get("functions", [])
    pn = prevnext_list([(f["name"], page_id(m["name"], f["name"])) for f in funcs])
    for f, (p, n) in zip(funcs, pn):
        pid = page_id(m["name"], f["name"])
        func_page(f, m["name"], "", crumbs + [(f["name"], None)], pid, (p, n), "Function")
        count += 1
    for en in m.get("enums", []):
        eid = page_id(m["name"], en["name"])
        b = f"<h1>{e(m['name'])}.{e(en['name'])} Enumeration</h1><h2>Definition</h2>" + paras(en["desc"])
        b += "<h2>Values</h2>" + value_table([(f"`{n}`", v, d) for n, v, d in en["values"]], ("Name", "Value", "Meaning"))
        if en.get("remarks"):
            b += "<h2>Remarks</h2>" + paras(en["remarks"])
        write(eid, layout(en["name"], crumbs + [(en["name"], None)], b))
        count += 1
    for c in m.get("classes", []):
        cid = page_id(m["name"], c["name"])
        title = f"{m['name']}.{c['name']}"
        b = f"<h1>{e(title)} Class</h1><h2>Definition</h2>" + paras(c["desc"])
        if c.get("bases"):
            b += f'<p class="muted">Base: {inline(c["bases"])}</p>'
        if c.get("properties"):
            b += "<h2>Properties</h2>" + value_table([(f"`{n}`", d) for n, d in c["properties"]], ("Property", "Description"))
        b += "<h2>Methods</h2>" + member_table([(page_id(m["name"], c["name"], f["name"]), f"{f['name']}({f.get('sig', '')})",
                                                 first_sentence(f.get("desc"))) for f in c["methods"]], ("Method", "Description"))
        if c.get("remarks"):
            b += "<h2>Remarks</h2>" + paras(c["remarks"])
        write(cid, layout(title, crumbs + [(c["name"], cid)], b))
        count += 1
        pn = prevnext_list([(f["name"], page_id(m["name"], c["name"], f["name"])) for f in c["methods"]])
        for f, (p, n) in zip(c["methods"], pn):
            func_page(f, title, "", crumbs + [(c["name"], cid), (f["name"], None)], page_id(m["name"], c["name"], f["name"]),
                      (p, n), "Method")
            count += 1
for o in OBJECTS:
    oid = page_id(o["name"])
    crumbs = [root, (o["name"], oid)]
    b = f"<h1>{e(o['name'])} Object</h1><h2>Definition</h2>" + paras(o["desc"])
    b += "<h2>Methods</h2>" + member_table([(page_id(o["name"], f["name"]), f"{f['name']}({f.get('sig', '')})",
                                             first_sentence(f.get("desc"))) for f in o["methods"]], ("Method", "Description"))
    if o.get("remarks"):
        b += "<h2>Remarks</h2>" + paras(o["remarks"])
    write(oid, layout(o["name"], crumbs, b))
    count += 1
    pn = prevnext_list([(f["name"], page_id(o["name"], f["name"])) for f in o["methods"]])
    for f, (p, n) in zip(o["methods"], pn):
        func_page(f, o["name"], "", crumbs + [(f["name"], None)], page_id(o["name"], f["name"]), (p, n), "Method")
        count += 1

# index
b = "<h1>Python API</h1><p>The engine objects <code>tabula_rasa.exe</code> exposes to the client scripts (<code>trpython.zip</code>), as traced in the executable and implemented in OpenRasa. Each module is registered at start-up by the engine; its classes cannot be instantiated from Python - the engine hands out the instances.</p>"
b += "<h2>Modules</h2>" + member_table([(page_id(m["name"]), m["name"], first_sentence(m["desc"])) for m in MODULES], ("Module", "Description"))
b += "<h2>Objects</h2>" + member_table([(page_id(o["name"]), o["name"], first_sentence(o["desc"])) for o in OBJECTS], ("Object", "Description"))
b += "<h2>Conventions</h2>" + paras(
    "Signatures list the arguments the engine parses; `[, x]` marks an optional one. Types are Python 2.4's: `int`, `long` (entity ids are 64-bit and arrive as `long`), `float`, `str`, `unicode`, `bool` (any object; its truth is used unless noted).\n\n"
    "The Engine section names the function in `tabula_rasa.exe` (client 1.16.5.0) that implements the method: an address `0x...` is the bound function itself, `FUN_...` the routine it calls. UNVERIFIED marks behaviour read from the code but not checked against the running game.\n\n"
    "An OpenRasa note says where OpenRasa does not reproduce the engine's behaviour exactly.")
write("index", layout("Python API", [root], b))
count += 1
with open(os.path.join(OUT, "style.css"), "w", encoding="utf-8") as f:
    f.write(CSS)
with open(os.path.join(OUT, "nav.js"), "w", encoding="utf-8") as f:
    f.write(NAVJS)
print("pages:", count)
