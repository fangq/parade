#!/usr/bin/env python3
"""Parade relay: the server collaborating editors share a document through.

Every update an editor sends is appended to the document's log under the
next sequence number, and every editor reads the log in that order -- so an
update always arrives after the ones it depends on, which is what pd_sync
(yrs) needs. The relay does not understand the updates; merging happens in
the editors. Catch-up after any absence is "everything after the last
sequence number I have", live updates are a long poll on the same request.

    parade_relay.py serve --db relay.sqlite --secret-file relay.secret [--port 8765]
    parade_relay.py serve --db postgresql://user@host/dbname --secret-file relay.secret
    parade_relay.py token --secret-file relay.secret --user ann --doc proposal --role editor [--days 30]
    parade_relay.py secret > relay.secret      # a new random signing key

HTTP API (Authorization: Bearer <token>):
    POST /d/<doc>/updates            body: one update (binary); X-Client: the replica's id
                                     -> {"seq": n}; 403 for a viewer
    GET  /d/<doc>/updates?after=N&wait=S
                                     -> the updates after N, each as [seq u64][length u32][bytes]
                                        (big-endian); waits up to S seconds (at most 55) for one
                                        when there are none; X-Last-Seq: the newest
    POST /d/<doc>/presence           body: JSON {client, name, color, key, offset, anchor_key,
                                     anchor_offset} -> JSON list of the others seen in the last 30 s
    GET  /health                     -> "ok"

Tokens are HS256 JWTs: {"sub": user, "doc": doc or "*", "role": "viewer" | "commenter" |
"editor", "exp": ...}. A commenter is let write like an editor: the relay cannot see inside an
update to keep it to comments (see README). Storage is the stdlib's sqlite3, or Postgres through
asyncpg when the --db is a postgresql:// URL (then several relay processes can share it; each
wakes its own waiters, so put one relay per document behind a load balancer's sticky routing,
or add LISTEN/NOTIFY).
"""

import argparse
import asyncio
import json
import os
import secrets
import sqlite3
import struct
import sys
import time

try:
    import jwt      # PyJWT
except ImportError:  # pragma: no cover
    jwt = None

from aiohttp import web

ROLES = ("viewer", "commenter", "editor")
MAX_UPDATE = 16 << 20       # bytes in one update
MAX_BATCH = 4 << 20         # bytes of updates returned by one read
PRESENCE_TTL = 30.0


class SqliteStore:
    """The log in an SQLite file; calls run in a worker thread so the loop never blocks."""

    def __init__(self, path):
        self.path = path
        self.db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.db.execute("pragma journal_mode=wal")
        self.db.execute("pragma synchronous=full")
        self.db.execute("create table if not exists doc_update (doc text not null, seq integer not null,"
                        " client text, author text, data blob not null, at real not null, primary key (doc, seq))")
        self.lock = asyncio.Lock()

    async def append(self, doc, client, author, data):
        async with self.lock:   # one writer: sequence numbers in order, none twice
            return await asyncio.to_thread(self._append, doc, client, author, data)

    def _append(self, doc, client, author, data):
        cur = self.db.execute("select coalesce(max(seq), 0) from doc_update where doc = ?", (doc,))
        seq = cur.fetchone()[0] + 1
        self.db.execute("insert into doc_update values (?, ?, ?, ?, ?, ?)", (doc, seq, client, author, data, time.time()))
        return seq

    async def after(self, doc, seq, limit_bytes):
        return await asyncio.to_thread(self._after, doc, seq, limit_bytes)

    def _after(self, doc, seq, limit_bytes):
        out, size = [], 0
        for s, d in self.db.execute("select seq, data from doc_update where doc = ? and seq > ? order by seq", (doc, seq)):
            if out and size + len(d) > limit_bytes:
                break
            out.append((s, bytes(d)))
            size += len(d)
        return out

    async def last(self, doc):
        return await asyncio.to_thread(
            lambda: self.db.execute("select coalesce(max(seq), 0) from doc_update where doc = ?", (doc,)).fetchone()[0])


class PgStore:
    """The log in Postgres (asyncpg)."""

    def __init__(self, dsn):
        self.dsn = dsn
        self.pool = None

    async def open(self):
        import asyncpg  # only needed for Postgres
        self.pool = await asyncpg.create_pool(self.dsn)
        async with self.pool.acquire() as c:
            await c.execute("create table if not exists doc_update (doc text not null, seq bigint not null,"
                            " client text, author text, data bytea not null, at timestamptz not null default now(),"
                            " primary key (doc, seq))")

    async def append(self, doc, client, author, data):
        async with self.pool.acquire() as c:
            async with c.transaction():
                # the document's rows locked while the next number is taken
                await c.execute("select pg_advisory_xact_lock(hashtext($1))", doc)
                seq = await c.fetchval("select coalesce(max(seq), 0) + 1 from doc_update where doc = $1", doc)
                await c.execute("insert into doc_update (doc, seq, client, author, data) values ($1, $2, $3, $4, $5)",
                                doc, seq, client, author, data)
                return seq

    async def after(self, doc, seq, limit_bytes):
        out, size = [], 0
        async with self.pool.acquire() as c:
            for r in await c.fetch("select seq, data from doc_update where doc = $1 and seq > $2 order by seq", doc, seq):
                if out and size + len(r["data"]) > limit_bytes:
                    break
                out.append((r["seq"], bytes(r["data"])))
                size += len(r["data"])
        return out

    async def last(self, doc):
        async with self.pool.acquire() as c:
            return await c.fetchval("select coalesce(max(seq), 0) from doc_update where doc = $1", doc)


class Relay:
    def __init__(self, store, secret):
        self.store = store
        self.secret = secret
        self.cond = {}          # doc -> asyncio.Condition, notified at every append
        self.presence = {}      # doc -> {client: (time, dict)}

    def condition(self, doc):
        if doc not in self.cond:
            self.cond[doc] = asyncio.Condition()
        return self.cond[doc]

    def auth(self, request, doc, write=False):
        """the token's claims, checked for this document; raises an HTTP error otherwise"""
        h = request.headers.get("Authorization", "")
        if not h.startswith("Bearer "):
            raise web.HTTPUnauthorized(text="token needed")
        try:
            claims = jwt.decode(h[7:], self.secret, algorithms=["HS256"], options={"require": ["exp", "sub", "role"]})
        except jwt.PyJWTError as e:
            raise web.HTTPUnauthorized(text="bad token: %s" % e)
        if claims.get("doc", "*") not in ("*", doc):
            raise web.HTTPForbidden(text="not this document")
        if claims["role"] not in ROLES:
            raise web.HTTPForbidden(text="unknown role")
        if write and claims["role"] == "viewer":
            raise web.HTTPForbidden(text="read only")
        return claims

    @staticmethod
    def doc_of(request):
        doc = request.match_info["doc"]
        if not doc or len(doc) > 200 or any(c in doc for c in "/\\\0"):
            raise web.HTTPBadRequest(text="bad document name")
        return doc

    async def post_update(self, request):
        doc = self.doc_of(request)
        claims = self.auth(request, doc, write=True)
        if request.content_length is not None and request.content_length > MAX_UPDATE:
            raise web.HTTPRequestEntityTooLarge(max_size=MAX_UPDATE, actual_size=request.content_length)
        data = await request.read()
        if not data or len(data) > MAX_UPDATE:
            raise web.HTTPBadRequest(text="empty or too large")
        seq = await self.store.append(doc, request.headers.get("X-Client", "")[:64], claims["sub"][:200], data)
        cond = self.condition(doc)
        async with cond:
            cond.notify_all()
        return web.json_response({"seq": seq})

    async def get_updates(self, request):
        doc = self.doc_of(request)
        self.auth(request, doc)
        try:
            after = max(0, int(request.query.get("after", "0")))
            wait = min(55.0, max(0.0, float(request.query.get("wait", "0"))))
        except ValueError:
            raise web.HTTPBadRequest(text="bad after or wait")
        rows = await self.store.after(doc, after, MAX_BATCH)
        if not rows and wait > 0:
            cond = self.condition(doc)
            deadline = time.monotonic() + wait
            while not rows:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                async with cond:
                    try:
                        await asyncio.wait_for(cond.wait(), timeout=left)
                    except asyncio.TimeoutError:
                        pass
                rows = await self.store.after(doc, after, MAX_BATCH)
        body = b"".join(struct.pack(">QI", s, len(d)) + d for s, d in rows)
        last = rows[-1][0] if rows else after
        return web.Response(body=body, content_type="application/octet-stream", headers={"X-Last-Seq": str(last)})

    async def post_presence(self, request):
        doc = self.doc_of(request)
        claims = self.auth(request, doc)
        try:
            p = json.loads(await request.read() or b"{}")
            if not isinstance(p, dict):
                raise ValueError
        except ValueError:
            raise web.HTTPBadRequest(text="bad presence")
        client = str(p.get("client", ""))[:64]
        now = time.time()
        room = self.presence.setdefault(doc, {})
        if client:
            p = {k: p[k] for k in ("client", "name", "color", "key", "offset", "anchor_key", "anchor_offset") if k in p}
            p["user"] = claims["sub"]
            room[client] = (now, p)
        for c in [c for c, (t, _) in room.items() if now - t > PRESENCE_TTL]:
            del room[c]
        return web.json_response([v for c, (t, v) in room.items() if c != client])

    async def health(self, request):
        return web.Response(text="ok")


def make_app(relay):
    app = web.Application(client_max_size=MAX_UPDATE + 1024)
    app.router.add_get("/health", relay.health)
    app.router.add_post("/d/{doc}/updates", relay.post_update)
    app.router.add_get("/d/{doc}/updates", relay.get_updates)
    app.router.add_post("/d/{doc}/presence", relay.post_presence)
    return app


def read_secret(path):
    with open(path, "rb") as f:
        s = f.read().strip()
    if len(s) < 32:
        sys.exit("the secret is too short (want 32 bytes or more)")
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--db", default="relay.sqlite")
    s.add_argument("--secret-file", required=True)
    s.add_argument("--host", default="127.0.0.1")
    s.add_argument("--port", type=int, default=8765)
    t = sub.add_parser("token")
    t.add_argument("--secret-file", required=True)
    t.add_argument("--user", required=True)
    t.add_argument("--doc", default="*")
    t.add_argument("--role", choices=ROLES, default="editor")
    t.add_argument("--days", type=float, default=30)
    sub.add_parser("secret")
    a = ap.parse_args()

    if a.cmd == "secret":
        print(secrets.token_urlsafe(48))
        return
    if jwt is None:
        sys.exit("PyJWT is needed (pip install pyjwt)")
    secret = read_secret(a.secret_file)
    if a.cmd == "token":
        print(jwt.encode({"sub": a.user, "doc": a.doc, "role": a.role, "exp": int(time.time() + a.days * 86400)},
                         secret, algorithm="HS256"))
        return

    async def start():
        if a.db.startswith("postgresql://") or a.db.startswith("postgres://"):
            store = PgStore(a.db)
            await store.open()
        else:
            store = SqliteStore(a.db)
        runner = web.AppRunner(make_app(Relay(store, secret)))
        await runner.setup()
        await web.TCPSite(runner, a.host, a.port).start()
        print("relay on http://%s:%d, log in %s" % (a.host, a.port, a.db), flush=True)
        await asyncio.Event().wait()

    try:
        asyncio.run(start())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
