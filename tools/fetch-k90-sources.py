import os, sys, json, urllib.request

TOKEN = open(r"C:\hjkkik\ios7pro\.token").read().strip()
STAGE = r"C:\hjkkik\ios7pro\port-stage"
os.makedirs(STAGE, exist_ok=True)

def api(url):
    req = urllib.request.Request(url, headers={
        "Authorization": "Bearer " + TOKEN,
        "User-Agent": "i",
        "Accept": "application/vnd.github+json",
    })
    return urllib.request.urlopen(req, timeout=90).read()

def raw(repo, path):
    req = urllib.request.Request(
        "https://api.github.com/repos/" + repo + "/contents/" + path,
        headers={"Authorization": "Bearer " + TOKEN, "User-Agent": "i",
                 "Accept": "application/vnd.github.raw"})
    return urllib.request.urlopen(req, timeout=120).read()

def tree(repo, ref):
    d = json.loads(api("https://api.github.com/repos/" + repo + "/git/trees/" + ref + "?recursive=1"))
    return d.get("tree", [])

jobs = []
for e in tree("iola1999/hackios-xnu-kernel", "k90-port"):
    p = e["path"]
    if e["type"] != "blob":
        continue
    if p.startswith("xnu/files/bsd/dev/adreno8") or p.startswith("xnu/files/bsd/dev/redwood"):
        jobs.append(("iola1999/hackios-xnu-kernel", p, "kernel/" + os.path.basename(p)))
    elif p.startswith("xnu/files/iokit/Kernel/RedwoodGPU"):
        jobs.append(("iola1999/hackios-xnu-kernel", p, "kernel/" + os.path.basename(p)))
    elif p.startswith("tools/test-adreno8-") and p.endswith(".py"):
        jobs.append(("iola1999/hackios-xnu-kernel", p, "tools/" + os.path.basename(p)))

for e in tree("iola1999/hackios-xnu-userland", "k90-port"):
    p = e["path"]
    if e["type"] != "blob":
        continue
    if p.startswith("tools/mesa/") or "eagl" in p.lower():
        jobs.append(("iola1999/hackios-xnu-userland", p, "userland/" + p.replace("tools/", "").replace("/", "__")))

print("files to fetch:", len(jobs))
ok = 0
for repo, path, dst in jobs:
    out = os.path.join(STAGE, dst.replace("/", os.sep))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    try:
        data = raw(repo, path)
        if len(data) < 20 or data.lstrip().startswith(b'{"message"'):
            print("  skip", path)
            continue
        open(out, "wb").write(data)
        ok += 1
    except Exception as ex:
        print("  fail", path, ex)
print("downloaded:", ok)
total = 0
for root, _, files in os.walk(STAGE):
    for f in files:
        total += os.path.getsize(os.path.join(root, f))
print("total KB:", total // 1024)
