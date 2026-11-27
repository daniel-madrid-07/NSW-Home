"""Listing (path, size, modification time) of the owner's Ryujinx data folder,
to prove a test left it alone.   python tools/appdata_listing.py <out.txt>
                                 python tools/appdata_listing.py --diff <before.txt> <after.txt>"""
import os, sys

def listing(root):
    rows = []
    for d, dirs, files in os.walk(root):
        for n in files:
            p = os.path.join(d, n)
            try:
                st = os.stat(p)
                rows.append("%s|%d|%d" % (os.path.relpath(p, root), st.st_size, st.st_mtime_ns))
            except OSError:
                rows.append("%s|?|?" % os.path.relpath(p, root))
        if not files and not dirs:
            rows.append("%s|dir|" % os.path.relpath(d, root))
    return sorted(rows)

if sys.argv[1] == "--diff":
    a = set(open(sys.argv[2], encoding="utf-8").read().splitlines())
    b = set(open(sys.argv[3], encoding="utf-8").read().splitlines())
    for r in sorted(a - b): print("- " + r.split("|")[0])
    for r in sorted(b - a): print("+ " + r.split("|")[0])
    print("changed entries: %d" % len(a ^ b))
else:
    rows = listing(os.path.join(os.environ["APPDATA"], "Ryujinx"))
    open(sys.argv[1], "w", encoding="utf-8").write("\n".join(rows) + "\n")
    print(len(rows), "entries")
