#!/usr/bin/env python3
"""Tests for parade_relay.py: tokens and roles, the log's order, catch-up, long polls, presence."""

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
    print("relay tests:", "ok" if not fails else "%d failures" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
