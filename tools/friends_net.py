"""Friends over the real network: starts headless peers (build\\friends_peer.exe,
each the same module the menu uses, with a throw-away data folder and a
made-up user, "Prueba X") that find each other through the public BitTorrent
DHT, and measures how long everything takes.

    python tools/friends_net.py <work folder> <scenario> [--poll 60] [--inbox 120] [--answer 60] [--tag name]

Scenarios (every code is made by its peer and thrown away with its folder):
  flow       B sends A a request, A accepts; both change state in turns and
             see each other; then A removes B: B must drop A and must never
             show anything A publishes after that.
  more       two requests at once (E and F ask G, who accepts both), a decline
             (C asks H, who declines: C must still see it waiting), and a
             withdrawal (W asks J, then withdraws: J must drop it).
  play       playing together (netplay.h): B asks A, A accepts; then A invites
             B to a game --invites times (one after each outcome, a minute
             apart) and B answers "join" each time. Measured: invitation
             written, read at B, answer written, seen by A (both "start").
  stranger   A and B become friends; then someone who has only A's code reads
             what it can, writes a card of its own over A's and fills one of
             A's inbox places; B must keep seeing A, A must notice and put its
             card back, and Z, who looks A up afterwards, must never be shown
             the impostor.

--poll / --inbox / --answer are the module's intervals (60 / 120 / 60 are
the menu's own). Everything the peers print goes to <work>/<tag>_<peer>.log;
the summary goes to the screen and to <work>/<tag>_summary.txt.
"""
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "friends_peer.exe")
if not os.path.exists(EXE):
    EXE = os.path.join(ROOT, "build", "friends_peer")


class Run:
    def __init__(self, a):
        self.a = a
        self.procs, self.logs, self.codes, self.t0 = {}, {}, {}, time.time()
        os.makedirs(a.work, exist_ok=True)

    def start(self, name, args, duration):
        data = os.path.join(self.a.work, f"{self.a.tag}_data_{name}")
        shutil.rmtree(data, ignore_errors=True)
        os.makedirs(data)
        cmd = [EXE, data, f"Prueba {name}", "--poll", str(self.a.poll), "--inbox", str(self.a.inbox), "--answer", str(self.a.answer),
               "--duration", str(duration)] + args
        self.logs[name] = os.path.join(self.a.work, f"{self.a.tag}_{name}.log")
        self.procs[name] = subprocess.Popen(cmd, stdout=open(self.logs[name], "w", encoding="utf-8"), stderr=subprocess.STDOUT,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        # The code is made by the peer (from its new identity): read from its first line.
        for _ in range(200):
            for line in open(self.logs[name], encoding="utf-8", errors="replace"):
                m = re.search(r" START .*code=(\d{12})", line)
                if m:
                    self.codes[name] = m.group(1)
                    return m.group(1)
            time.sleep(0.1)
        raise RuntimeError(f"peer {name} did not start")

    def stranger(self, name, code, overwrite):
        log = os.path.join(self.a.work, f"{self.a.tag}_{name}.log")
        self.logs[name] = log
        subprocess.run([EXE, "--stranger", code] + (["--overwrite"] if overwrite else []), stdout=open(log, "w", encoding="utf-8"),
                       stderr=subprocess.STDOUT, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=300)

    def wait(self):
        for p in self.procs.values():
            p.wait()

    def lines(self, name):
        rows = []
        for raw in open(self.logs[name], encoding="utf-8", errors="replace"):
            m = re.match(r"(\d+) (\w+) ?(.*)", raw.strip())
            if m:
                rows.append((int(m.group(1)) / 1000.0, m.group(2), m.group(3)))
        return rows

    def first(self, name, kind, pattern=""):
        for t, k, text in self.lines(name):
            if k == kind and re.search(pattern, text):
                return t
        return None

    def anon(self, text):
        for n, c in self.codes.items():
            text = text.replace(c, f"<{n}>")
        return text


def field(text, key, cast=float):
    m = re.search(rf"\b{key}=(\S+)", text)
    return cast(m.group(1)) if m else None


def traffic(r, say, names):
    for n in names:
        rows = r.lines(n)
        end = [x for x in rows if x[1] == "END"]
        if not end or not rows:
            continue
        secs = rows[-1][0] - rows[0][0]
        e = end[0][2]
        nets = [x[2] for x in rows if x[1] == "NET"]
        gets = [field(x, "queries", int) for x in nets if x.startswith(("get ", "inbox ", "note ", "answer ", "lookup "))]
        gsec = [field(x, "seconds") for x in nets if x.startswith(("get ", "inbox ", "note ", "answer "))]
        puts = [x for x in nets if x.startswith("put ")]
        stored = [x.split("stored=")[1].split()[0] for x in puts]
        say(f"   Prueba {n}: {e}  over {secs:.0f} s"
            + (f"; reads: median {statistics.median(gets)} queries, {statistics.median(gsec):.2f} s" if gets and gsec else "")
            + (f"; writes stored on {' '.join(stored[:12])}{' ...' if len(stored) > 12 else ''}" if puts else ""))


def flow(r, a, say):
    rounds, period = 4, max(60.0, a.poll * 1.5)
    removeAt = 150 + rounds * period
    dur = removeAt + 300
    ca = r.start("A", ["--accept", "--rounds", str(rounds + 3), "--period", str(period), "--first", "150", "--remove-at", str(removeAt)], dur)
    time.sleep(20)  # A's card is in the network before B looks for it
    cb = r.start("B", ["--request", ca, "--rounds", str(rounds), "--period", str(period), "--first", str(150 + period / 2)], dur - 20)
    r.wait()
    sent = r.first("B", "SENT")
    got = r.first("A", "RECEIVED")
    acc = r.first("A", "ACCEPTED")
    fr = r.first("B", "FRIEND")
    say(f"request: looked up and sent {sent - r.first('B', 'START'):.1f} s after B started" if sent else "request: NOT SENT")
    if sent and got:
        say(f"request arrived at A {got - sent:.1f} s after it was sent (inbox places read every {a.inbox:.0f} s)")
    else:
        say("request: NOT RECEIVED")
    if acc and fr:
        say(f"acceptance seen by B {fr - acc:.1f} s after A accepted (answer looked for every {a.answer:.0f} s at first)")
    else:
        say("acceptance: NOT SEEN")
    for src, dst in (("A", "B"), ("B", "A")):
        selfs = [(t, field(x, "round", int)) for t, k, x in r.lines(src) if k == "SELF"]
        seen = {}
        for t, k, x in r.lines(dst):
            if k == "SEEN" and x.startswith(r.codes[src]):
                seen.setdefault(field(x, "round", int), t)
        delays, missed = [], []
        rmT = r.first("A", "REMOVED")
        for t, k in selfs:
            if src == "A" and rmT and t > rmT:
                continue  # judged below: B must NOT see these
            if k in seen:
                delays.append(seen[k] - t)
            else:
                missed.append(k)
        if delays:
            say(f"change on {src} -> shown on {dst}: n={len(delays)} median={statistics.median(delays):.1f}s min={min(delays):.1f}s "
                f"max={max(delays):.1f}s" + (f"  not seen: rounds {missed}" if missed else ""))
        else:
            say(f"change on {src} -> shown on {dst}: none seen (missed {missed})")
    rm = r.first("A", "REMOVED")
    gone = r.first("B", "GONE")
    if rm and gone:
        say(f"removal: B dropped A {gone - rm:.1f} s after A removed B")
    else:
        say("removal: B did NOT drop A" if rm else "removal: not reached")
    if rm:
        after = [x for t, k, x in r.lines("A") if k == "SELF" and t > rm]
        leaked = [x for t, k, x in r.lines("B") if k == "SEEN" and t > rm and x.startswith(r.codes["A"])
                  and any(f"round={field(s, 'round', int)} " in x and "state=1" in x for s in after)]
        say(f"after the removal A changed state {len(after)} times; B showed any of them: {len(leaked)}")
        aGone = r.first("A", "GONE")
        say("A's own list dropped B at once" if aGone and aGone - rm < 2 else "A's list: B not dropped at once")
    traffic(r, say, ["A", "B"])


def more(r, a, say):
    dur = 600
    g = r.start("G", ["--accept"], dur)
    h = r.start("H", ["--decline"], dur)
    j = r.start("J", [], dur)
    time.sleep(20)
    r.start("E", ["--request", g], dur - 20)
    r.start("F", ["--request", g], dur - 20)
    r.start("C", ["--request", h], dur - 20)
    r.start("W", ["--request", j, "--withdraw-at", "300"], dur - 20)
    r.wait()
    for n in ("E", "F"):
        s, got, fr = r.first(n, "SENT"), r.first("G", "RECEIVED", r.codes[n]), r.first(n, "FRIEND")
        say(f"two at once: {n}'s request sent at +{s - r.t0:.0f}s, at G after {got - s:.1f}s, {n} sees the friendship "
            f"{fr - got:.1f}s after G accepted" if s and got and fr else f"two at once: {n} sent={bool(s)} received={bool(got)} friend={bool(fr)}")
    seenG = [x for t, k, x in r.lines("E") + r.lines("F") if k == "SEEN" and x.startswith(g) and "known=1" in x]
    say(f"two at once: E and F read G's status: {len(seenG) > 0}")
    s, got, dec = r.first("C", "SENT"), r.first("H", "RECEIVED"), r.first("H", "DECLINED")
    still = r.first("C", "SENT_GONE") is None and r.first("C", "FRIEND") is None
    say(f"decline: C's request at H after {got - s:.1f}s, declined; C still shows it waiting at the end: {still}; "
        f"H shown it again afterwards: {len([1 for t, k, x in r.lines('H') if k == 'RECEIVED' and t > dec])}" if s and got and dec
        else f"decline: sent={bool(s)} received={bool(got)}")
    s, got, wd, gone = r.first("W", "SENT"), r.first("J", "RECEIVED"), r.first("W", "WITHDRAWN"), r.first("J", "RECEIVED_GONE")
    say(f"withdraw: W's request at J after {got - s:.1f}s; withdrawn at +{wd - s:.0f}s; gone from J {gone - wd:.1f}s later"
        if s and got and wd and gone else f"withdraw: sent={bool(s)} received={bool(got)} withdrawn={bool(wd)} gone={bool(gone)}")
    traffic(r, say, ["G", "H", "J", "E", "F", "C", "W"])


def play(r, a, say):
    n = a.invites
    dur = 240 + n * (a.poll * 2 + 120) + 120
    ca = r.start("A", ["--accept", "--invite-at", "60", "--invites", str(n), "--message", str(a.message)], dur)
    time.sleep(20)
    r.start("B", ["--request", ca, "--join", "--message", str(a.message)], dur - 20)
    r.wait()
    rows_a, rows_b = r.lines("A"), r.lines("B")
    def find(rows, kind, nonce):
        for t, k, text in rows:
            if k == kind and f"nonce={nonce}" in text:
                return t
        return None
    stats = {"stored": [], "read": [], "answer": [], "ready": [], "total": []}
    for t0, k, text in rows_a:
        if k != "INVITE":
            continue
        nonce = field(text, "nonce", str)
        st = next((t for t, kk, tx in rows_a if kk == "INVITE_STORED" and t >= t0), None)
        got = find(rows_b, "INVITED", nonce)
        ans = next((t for t, kk, tx in rows_b if kk == "ANSWERED" and got and t >= got), None)
        rb = find(rows_b, "READY", nonce)
        ra = find(rows_a, "READY", nonce)
        out = find(rows_a, "DECLINED", nonce) or find(rows_a, "NOANSWER", nonce) or find(rows_a, "UNSENT", nonce)
        def d(x, y):
            return f"{y - x:.1f} s" if x and y else "never"
        say(f"invitation {nonce}: stored {d(t0, st)}, read by B {d(t0, got)}, B's 'join' stored (B starts) {d(got, rb)}, "
            f"seen by A (A starts) {d(got, ra)}; from invite to both started {d(t0, max(ra or 0, rb or 0) or None)}"
            + (" OUTCOME " + r.anon(next(tx for t, kk, tx in rows_a if t == out)) if out else ""))
        if st: stats["stored"].append(st - t0)
        if got: stats["read"].append(got - t0)
        if got and ra: stats["ready"].append(ra - got)
        if ra and rb: stats["total"].append(max(ra, rb) - t0)
    for key, label in (("stored", "invitation stored"), ("read", "invitation read by the friend"), ("ready", "answer seen by the inviter"),
                       ("total", "invite to both started")):
        v = stats[key]
        if v:
            say(f"{label}: median {statistics.median(v):.1f} s, min {min(v):.1f}, max {max(v):.1f} (n={len(v)})")
    pa = [field(tx, "passphrase", str) for t, kk, tx in rows_a if kk == "READY"]
    pb = [field(tx, "passphrase", str) for t, kk, tx in rows_b if kk == "READY"]
    say(f"passphrases at A {pa} / at B {pb}: {'the same' if pa == pb and pa else 'DIFFERENT'}")
    traffic(r, say, ["A", "B"])


def stranger(r, a, say):
    dur = 720
    # (A looks at its own card every 2 minutes here instead of 15, to see it notice within the run.)
    ca = r.start("A", ["--accept", "--rounds", "12", "--period", "40", "--first", "120", "--card", "120"], dur)
    time.sleep(20)
    r.start("B", ["--request", ca], dur - 20)
    deadline = time.time() + 400
    while time.time() < deadline and r.first("B", "FRIEND") is None:
        time.sleep(5)
    say("A and B are friends" if r.first("B", "FRIEND") else "A and B did NOT become friends in time")
    t = time.time()
    r.stranger("X", ca, True)
    for line in open(r.logs["X"], encoding="utf-8", errors="replace"):
        say("   stranger: " + r.anon(line.split(" ", 1)[1].strip()))
    time.sleep(30)
    r.start("Z", ["--request", ca], dur - (time.time() - r.t0) - 10)
    r.wait()
    tr = [(tt, x) for tt, k, x in r.lines("A") if k == "TROUBLE"]
    say("A's own warnings: " + "; ".join(f"+{tt - t:.0f}s {x}" for tt, x in tr))
    back = [tt for tt, k, x in r.lines("A") if k == "NET" and x.startswith("put card ") and tt > t]
    say(f"A wrote its card back {back[0] - t:.0f}s after the stranger" if back else "A did NOT write its card back")
    zs = [(tt, k, x) for tt, k, x in r.lines("Z") if k in ("SENT", "LOOKUP")]
    say("Z looking A up: " + "; ".join(f"+{tt - t:.0f}s {k} {r.anon(x)}" for tt, k, x in zs))
    imp = [x for tt, k, x in r.lines("Z") + r.lines("B") if "Impostor" in x]
    say(f"anyone shown the impostor's name: {len(imp)}")
    seenAfter = [tt for tt, k, x in r.lines("B") if k == "SEEN" and tt > t and "state=1" in x]
    say(f"B kept seeing A's changes after the stranger: {len(seenAfter)} changes shown")
    traffic(r, say, ["A", "B", "Z"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("scenario", choices=["flow", "more", "stranger", "play"])
    ap.add_argument("--invites", type=int, default=3)
    ap.add_argument("--message", type=float, default=4)
    ap.add_argument("--poll", type=float, default=60)
    ap.add_argument("--inbox", type=float, default=120)
    ap.add_argument("--answer", type=float, default=60)
    ap.add_argument("--tag", default=None)
    a = ap.parse_args()
    a.tag = a.tag or a.scenario
    r = Run(a)
    out = []

    def say(s):
        out.append(s)
        print(s, flush=True)

    say(f"scenario={a.scenario} poll={a.poll}s inbox={a.inbox}s answer={a.answer}s")
    {"flow": flow, "more": more, "stranger": stranger, "play": play}[a.scenario](r, a, say)
    with open(os.path.join(a.work, f"{a.tag}_summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    sys.exit(main())
