// The inbox for problem reports from hacBrewPackLZ, run as a Cloudflare Worker with a D1 database bound as DB and a secret READ_KEY.
//
// The apps send reports to POST /report. Each one is kept whole, and grouped with the others about the same problem: what the app worked out
// as its fingerprint, such as the same error at the same step. A report written by hand with no error behind it is a message of its own.
// An app sends a problem it finds itself once per version, and after that only says how many more times it saw it, to POST /seen.
// Whoever reads the inbox, with the read key, sees each problem once, with how many reports and devices it has, and marks it known, fixed in
// a version, or dismissed. A problem marked known or fixed is answered at once, so the app can tell the person straight away. Nothing here
// talks to GitLab: issues are opened from the inbox by whoever reads it, so the only secret this holds is the key for reading it.

const MaxBody = 1 << 20;
const MaxReport = 900_000;
const MaxSaid = 4000;
/// Reports a device, or an address, can send in an hour, so nothing can flood the inbox.
const PerHour = 10;
const Kinds = ['manual', 'failed-job', 'crash', 'freeze', 'game-didnt-start'];
/// What's looked at first: a game that won't start after being compressed, then the app going wrong, then a job that couldn't be done.
const Serious = { 'game-didnt-start': 0, crash: 1, freeze: 2, 'failed-job': 3, manual: 4 };

export default {
  async fetch(request, env) {
    return handle(request, env, d1Store(env.DB));
  },
};

export async function handle(request, env, store) {
  const url = new URL(request.url);
  try {
    if (request.method === 'POST' && url.pathname === '/report')
      return await receive(request, env, store);
    if (request.method === 'POST' && url.pathname === '/seen')
      return await seen(request, env, store);
    if (url.pathname === '/inbox' || url.pathname.startsWith('/inbox/') || url.pathname.startsWith('/problem/')) {
      if (!(await authorized(request, env)))
        return json({ error: 'not allowed' }, 401);
      if (request.method === 'GET' && url.pathname === '/inbox')
        return json({ problems: await store.problems(url.searchParams.get('status') || 'new', Number(url.searchParams.get('limit') || 50)) });
      if (request.method === 'GET' && url.pathname.startsWith('/inbox/report/')) {
        const report = await store.report(Number(url.pathname.slice('/inbox/report/'.length)));
        return report ? json(report) : json({ error: 'no such report' }, 404);
      }
      if (request.method === 'POST' && url.pathname.startsWith('/problem/'))
        return await mark(request, decodeURIComponent(url.pathname.slice('/problem/'.length)), store);
    }
    return json({ error: 'nothing here' }, 404);
  } catch (e) {
    return json({ error: 'something went wrong here, so it was kept nowhere' }, 500);
  }
}

async function receive(request, env, store) {
  const length = Number(request.headers.get('content-length') || 0);
  if (length > MaxBody)
    return json({ error: 'too big' }, 413);
  const text = await request.text();
  if (text.length > MaxBody)
    return json({ error: 'too big' }, 413);
  let r;
  try {
    r = JSON.parse(text);
  } catch {
    return json({ error: 'not a report' }, 400);
  }
  const str = (value, most) => (typeof value === 'string' ? value.slice(0, most) : '');
  const report = {
    app: str(r.app, 40),
    version: str(r.version, 40),
    platform: str(r.platform, 40),
    kind: Kinds.includes(r.kind) ? r.kind : 'manual',
    fingerprint: str(r.fingerprint, 200),
    title: str(r.title, 200),
    said: str(r.said, MaxSaid),
    body: str(r.report, MaxReport),
    device: str(r.device, 64),
    count: Math.min(Math.max(Number.isInteger(r.count) ? r.count : 1, 1), 1000),
  };
  if (!report.app || !report.version || (!report.body && !report.said))
    return json({ error: 'not a report' }, 400);

  if (await tooMany(request, env, store, report.device))
    return json({ error: 'too many reports for now, try again later' }, 429);

  // What it's about: the problem it came with, or a message of its own.
  const key = report.fingerprint ? `${report.app}|${report.fingerprint}` : `message|${crypto.randomUUID()}`;
  const problem = await store.addReport(key, report, new Date().toISOString());
  if (problem.status === 'known' || problem.status === 'fixed')
    return json({ status: problem.status, fixedIn: problem.fixed_in || '', issue: problem.issue || 0, id: problem.reportId });
  return json({ status: 'received', id: problem.reportId });
}

/// A problem the app has sent before, seen again: only counted, so the inbox keeps one report of it per device and version. It's answered
/// with the problem's status, as a report is, and turned away when there's no such problem, such as one that was never sent.
async function seen(request, env, store) {
  const text = await request.text();
  if (text.length > 4096)
    return json({ error: 'too big' }, 413);
  let r;
  try {
    r = JSON.parse(text);
  } catch {
    return json({ error: 'not a count' }, 400);
  }
  const str = (value, most) => (typeof value === 'string' ? value.slice(0, most) : '');
  const app = str(r.app, 40), fingerprint = str(r.fingerprint, 200), device = str(r.device, 64), version = str(r.version, 40);
  const count = Math.min(Math.max(Number.isInteger(r.count) ? r.count : 1, 1), 1000);
  if (!app || !fingerprint)
    return json({ error: 'not a count' }, 400);
  if (await tooMany(request, env, store, device))
    return json({ error: 'too many reports for now, try again later' }, 429);
  const problem = await store.addSighting(`${app}|${fingerprint}`, count, version, new Date().toISOString());
  if (!problem)
    return json({ error: 'no such problem' }, 404);
  if (problem.status === 'known' || problem.status === 'fixed')
    return json({ status: problem.status, fixedIn: problem.fixed_in || '', issue: problem.issue || 0 });
  return json({ status: 'counted' });
}

/// Whether a device, or an address, has sent its share for this hour. The address is only ever kept as part of a hash, which is enough to
/// tell one sender from another and no more.
async function tooMany(request, env, store, device) {
  const address = await hash(`${env.READ_KEY || ''}|${request.headers.get('CF-Connecting-IP') || ''}`);
  const hour = Math.floor(Date.now() / 3600000);
  for (const who of [device && `device:${device}`, `address:${address}`].filter(Boolean)) {
    if ((await store.countThisHour(who, hour)) > PerHour)
      return true;
  }
  return false;
}

async function mark(request, key, store) {
  const r = await request.json().catch(() => ({}));
  if (!['new', 'known', 'fixed', 'dismissed'].includes(r.status))
    return json({ error: 'status is new, known, fixed or dismissed' }, 400);
  const done = await store.mark(key, r.status, typeof r.fixedIn === 'string' ? r.fixedIn.slice(0, 40) : '', Number.isInteger(r.issue) ? r.issue : 0);
  return done ? json({ ok: true }) : json({ error: 'no such problem' }, 404);
}

async function authorized(request, env) {
  const given = (request.headers.get('Authorization') || '').replace(/^Bearer /, '');
  if (!env.READ_KEY || !given)
    return false;
  // Compared as hashes, which takes the same time whatever was given.
  return (await hash(given)) === (await hash(env.READ_KEY));
}

async function hash(text) {
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, '0')).join('');
}

function json(value, status = 200) {
  return new Response(JSON.stringify(value), { status, headers: { 'content-type': 'application/json' } });
}

/// The inbox in the D1 database. See schema.sql.
export function d1Store(db) {
  const add = (list, value) => JSON.stringify([...new Set([...JSON.parse(list || '[]'), value].filter(Boolean))].slice(-50));
  return {
    async countThisHour(who, hour) {
      await db.prepare('INSERT INTO limits (who, hour, count) VALUES (?1, ?2, 1) ON CONFLICT (who, hour) DO UPDATE SET count = count + 1').bind(who, hour).run();
      const row = await db.prepare('SELECT count FROM limits WHERE who = ?1 AND hour = ?2').bind(who, hour).first();
      return row ? row.count : 1;
    },
    async addReport(key, r, now) {
      const added = await db.prepare('INSERT INTO reports (problem, received, app, version, platform, kind, said, body, device) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)')
        .bind(key, now, r.app, r.version, r.platform, r.kind, r.said, r.body, r.device).run();
      const seenBefore = r.device ? await db.prepare('SELECT 1 FROM sightings WHERE problem = ?1 AND device = ?2').bind(key, r.device).first() : null;
      if (r.device && !seenBefore)
        await db.prepare('INSERT INTO sightings (problem, device) VALUES (?1, ?2)').bind(key, r.device).run();
      const old = await db.prepare('SELECT * FROM problems WHERE key = ?1').bind(key).first();
      if (old) {
        await db.prepare('UPDATE problems SET reports = reports + 1, occurrences = occurrences + ?2, devices = devices + ?3, versions = ?4, platforms = ?5, last_seen = ?6 WHERE key = ?1')
          .bind(key, r.count, r.device && !seenBefore ? 1 : 0, add(old.versions, r.version), add(old.platforms, r.platform), now).run();
      } else {
        await db.prepare('INSERT INTO problems (key, kind, title, status, reports, occurrences, devices, versions, platforms, first_seen, last_seen) VALUES (?1, ?2, ?3, \'new\', 1, ?4, ?5, ?6, ?7, ?8, ?8)')
          .bind(key, r.kind, r.title, r.count, r.device ? 1 : 0, add('[]', r.version), add('[]', r.platform), now).run();
      }
      const problem = await db.prepare('SELECT status, fixed_in, issue FROM problems WHERE key = ?1').bind(key).first();
      return { ...problem, reportId: added.meta.last_row_id };
    },
    async addSighting(key, count, version, now) {
      const old = await db.prepare('SELECT * FROM problems WHERE key = ?1').bind(key).first();
      if (!old)
        return null;
      await db.prepare('UPDATE problems SET occurrences = occurrences + ?2, versions = ?3, last_seen = ?4 WHERE key = ?1').bind(key, count, add(old.versions, version), now).run();
      return old;
    },
    async problems(status, limit) {
      const { results } = await db.prepare('SELECT * FROM problems WHERE status = ?1 ORDER BY last_seen DESC LIMIT ?2').bind(status, Math.min(Math.max(limit, 1), 500)).all();
      for (const p of results) {
        const latest = await db.prepare('SELECT id, received, version, platform, said FROM reports WHERE problem = ?1 ORDER BY id DESC LIMIT 5').bind(p.key).all();
        p.latest = latest.results;
      }
      return results.sort((a, b) => (Serious[a.kind] ?? 9) - (Serious[b.kind] ?? 9) || b.devices - a.devices || b.reports - a.reports);
    },
    async report(id) {
      return await db.prepare('SELECT * FROM reports WHERE id = ?1').bind(id).first();
    },
    async mark(key, status, fixedIn, issue) {
      const done = await db.prepare('UPDATE problems SET status = ?2, fixed_in = ?3, issue = ?4 WHERE key = ?1').bind(key, status, fixedIn, issue).run();
      return done.meta.changes > 0;
    },
  };
}
