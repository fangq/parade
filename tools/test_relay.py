#!/usr/bin/env python3
"""Tests for parade_relay.py: tokens and roles, the log's order, catch-up, long polls, presence,
compaction (with a stand-in compactor that joins its input; pd_compact itself, if built, must refuse
what is not yrs)."""

import asyncio
import os
import struct
import sys
import tempfile
import time

import jwt
from aiohttp import ClientSession, web

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parade_relay as pr  # noqa: E402

SECRET = b"x" * 48
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("CHECK failed:", what)


def token(user, doc="*", role="editor", exp=3600):
    return jwt.encode({"sub": user, "doc": doc, "role": role, "exp": int(time.time() + exp)}, SECRET, algorithm="HS256")


def frames(body):
    out, i = [], 0
    while i < len(body):
        seq, n = struct.unpack(">QI", body[i:i + 12])
        out.append((seq, body[i + 12:i + 12 + n]))
        i += 12 + n
    return out


async def main():
    tmp = tempfile.mkdtemp()
    relay = pr.Relay(pr.SqliteStore(os.path.join(tmp, "r.sqlite")), SECRET)
    runner = web.AppRunner(pr.make_app(relay))
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    base = "http://127.0.0.1:%d" % port
    ann = {"Authorization": "Bearer " + token("ann"), "X-Client": "1"}
    bob = {"Authorization": "Bearer " + token("bob", doc="proposal"), "X-Client": "2"}
    vic = {"Authorization": "Bearer " + token("vic", role="viewer")}

    async with ClientSession() as s:
        r = await s.get(base + "/health")
        check(r.status == 200, "health")
        r = await s.post(base + "/d/proposal/updates", data=b"u1")
        check(r.status == 401, "no token: refused")
        r = await s.post(base + "/d/proposal/updates", data=b"u1",
                         headers={"Authorization": "Bearer " + token("ann", exp=-10)})
        check(r.status == 401, "expired token: refused")
        r = await s.post(base + "/d/proposal/updates", data=b"u1",
                         headers={"Authorization": "Bearer " + jwt.encode({"sub": "m", "role": "editor", "exp": 9e9}, b"y" * 48)})
        check(r.status == 401, "foreign signature: refused")
        r = await s.post(base + "/d/proposal/updates", data=b"u1", headers=vic)
        check(r.status == 403, "viewer cannot write")
        r = await s.post(base + "/d/other/updates", data=b"u1", headers=bob)
        check(r.status == 403, "token for another document")

        for i, h in enumerate([ann, bob, ann]):
            r = await s.post(base + "/d/proposal/updates", data=b"update%d" % i, headers=h)
            check(r.status == 200 and (await r.json())["seq"] == i + 1, "sequence numbers in order")

        r = await s.get(base + "/d/proposal/updates?after=0", headers=vic)
        f = frames(await r.read())
        check([q for q, _ in f] == [1, 2, 3] and f[1][1] == b"update1", "everything from the start, in order")
        check(r.headers["X-Last-Seq"] == "3", "the newest number")
        r = await s.get(base + "/d/proposal/updates?after=2", headers=vic)
        check([q for q, _ in frames(await r.read())] == [3], "catch-up: only what comes after")

        # a long poll returns as soon as someone writes
        async def later():
            await asyncio.sleep(0.3)
            await s.post(base + "/d/proposal/updates", data=b"late", headers=ann)

        t0 = time.monotonic()
        task = asyncio.create_task(later())
        r = await s.get(base + "/d/proposal/updates?after=3&wait=10", headers=bob)
        f = frames(await r.read())
        dt = time.monotonic() - t0
        await task
        check(f == [(4, b"late")] and dt < 3, "long poll woken by a new update (%.2f s)" % dt)
        r = await s.get(base + "/d/proposal/updates?after=4&wait=0.5", headers=bob)
        check(await r.read() == b"" and r.headers["X-Last-Seq"] == "4", "long poll times out empty")

        # concurrent writers still get distinct, gapless numbers
        rs = await asyncio.gather(*[s.post(base + "/d/proposal/updates", data=b"c%d" % i, headers=ann) for i in range(20)])
        seqs = sorted([(await r.json())["seq"] for r in rs])
        check(seqs == list(range(5, 25)), "concurrent appends numbered without gaps")

        # presence: each sees the others
        r = await s.post(base + "/d/proposal/presence", json={"client": "1", "name": "Ann", "key": "3e8.2", "offset": 4}, headers=ann)
        check(await r.json() == [], "alone")
        r = await s.post(base + "/d/proposal/presence", json={"client": "2", "name": "Bob"}, headers=bob)
        p = await r.json()
        check(len(p) == 1 and p[0]["name"] == "Ann" and p[0]["user"] == "ann" and p[0]["offset"] == 4, "sees the other")

    await runner.cleanup()
    await compaction(tmp)
    print("relay tests:", "ok" if not fails else "%d failures" % fails)
    return 1 if fails else 0


FAKE = r"""import struct, sys
d = sys.stdin.buffer.read(); out = []; i = 0
while i < len(d):
    n, = struct.unpack(">I", d[i:i + 4]); out.append(d[i + 4:i + 4 + n]); i += 4 + n
if b"bad" in out: sys.exit(1)
sys.stdout.buffer.write(b"[" + b"+".join(out) + b"]")
"""


async def compaction(tmp):
    fake = os.path.join(tmp, "fake_compact")
    with open(fake, "w") as f:
        f.write("#!%s\n%s" % (sys.executable, FAKE))
    os.chmod(fake, 0o755)
    relay = pr.Relay(pr.SqliteStore(os.path.join(tmp, "c.sqlite")), SECRET, fake, 3)
    runner = web.AppRunner(pr.make_app(relay))
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    base = "http://127.0.0.1:%d/d/doc" % site._server.sockets[0].getsockname()[1]
    ann = {"Authorization": "Bearer " + token("ann")}

    async def info():
        return await (await s.get(base + "/info", headers=ann)).json()

    async def settle():
        for _ in range(100):
            if not relay.compacting:
                return
            await asyncio.sleep(0.05)

    async with ClientSession() as s:
        for i in range(1, 3):
            await s.post(base + "/updates", data=b"u%d" % i, headers=ann)
        check(await info() == {"last": 2, "snapshot": 0, "updates": 2}, "no compaction below the threshold")
        await s.post(base + "/updates", data=b"u3", headers=ann)
        await settle()
        check(await info() == {"last": 3, "snapshot": 3, "updates": 0}, "compacted: %s" % await info())
        for i in range(4, 6):
            await s.post(base + "/updates", data=b"u%d" % i, headers=ann)
        f = frames(await (await s.get(base + "/updates?after=0", headers=ann)).read())
        check(f == [(3, b"[u1+u2+u3]"), (4, b"u4"), (5, b"u5")], "a newcomer gets the snapshot, then the rest: %s" % f)
        f = frames(await (await s.get(base + "/updates?after=1", headers=ann)).read())
        check([q for q, _ in f] == [3, 4, 5], "a reader inside the snapshot gets it whole")
        f = frames(await (await s.get(base + "/updates?after=3", headers=ann)).read())
        check(f == [(4, b"u4"), (5, b"u5")], "a reader past it does not")
        await s.post(base + "/updates", data=b"u6", headers=ann)
        await settle()
        f = frames(await (await s.get(base + "/updates?after=0", headers=ann)).read())
        check(f == [(6, b"[[u1+u2+u3]+u4+u5+u6]")], "the snapshot folds into the next: %s" % f)
        r = await s.post(base + "/updates", data=b"u7", headers=ann)
        check((await r.json())["seq"] == 7, "numbers go on after the dropped updates")
        await s.post(base + "/updates", data=b"bad", headers=ann)
        await s.post(base + "/updates", data=b"u9", headers=ann)
        await settle()
        check(await info() == {"last": 9, "snapshot": 6, "updates": 3}, "a refused compaction keeps the log")
    await runner.cleanup()

    real = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build-sync", "pd_compact")
    if os.path.exists(real):
        check(await pr.merge(real, [b"not yrs at all"]) is None, "pd_compact refuses garbage")
        check(await pr.merge(real, []) is not None, "pd_compact: nothing in, an empty document out")


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
