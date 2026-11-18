// The inbox kept in memory, as d1Store keeps it in D1, for the tests.
export function memoryStore() {
  const problems = new Map();
  const reports = [];
  const sightings = new Set();
  const limits = new Map();
  const Serious = { 'game-didnt-start': 0, crash: 1, freeze: 2, 'failed-job': 3, manual: 4 };
  return {
    async countThisHour(who, hour) {
      const key = `${who}|${hour}`;
      limits.set(key, (limits.get(key) || 0) + 1);
      return limits.get(key);
    },
    async addReport(key, r, now) {
      reports.push({ id: reports.length + 1, problem: key, received: now, app: r.app, version: r.version, platform: r.platform, kind: r.kind, said: r.said, body: r.body, device: r.device });
      const seenBefore = r.device && sightings.has(`${key}|${r.device}`);
      if (r.device)
        sightings.add(`${key}|${r.device}`);
      const add = (list, value) => [...new Set([...list, value].filter(Boolean))].slice(-50);
      const old = problems.get(key);
      if (old) {
        old.reports++;
        old.occurrences += r.count;
        old.devices += r.device && !seenBefore ? 1 : 0;
        old.versions = add(old.versions, r.version);
        old.platforms = add(old.platforms, r.platform);
        old.last_seen = now;
      } else {
        problems.set(key, { key, kind: r.kind, title: r.title, status: 'new', fixed_in: '', issue: 0, reports: 1, occurrences: r.count, devices: r.device ? 1 : 0,
          versions: add([], r.version), platforms: add([], r.platform), first_seen: now, last_seen: now });
      }
      const p = problems.get(key);
      return { status: p.status, fixed_in: p.fixed_in, issue: p.issue, reportId: reports.length };
    },
    async addSighting(key, count, version, now) {
      const p = problems.get(key);
      if (!p)
        return null;
      p.occurrences += count;
      p.versions = [...new Set([...p.versions, version].filter(Boolean))].slice(-50);
      p.last_seen = now;
      return { status: p.status, fixed_in: p.fixed_in, issue: p.issue };
    },
    async problems(status, limit) {
      return [...problems.values()].filter(p => p.status === status)
        .map(p => ({ ...p, latest: reports.filter(r => r.problem === p.key).slice(-5).reverse() }))
        .sort((a, b) => (Serious[a.kind] ?? 9) - (Serious[b.kind] ?? 9) || b.devices - a.devices || b.reports - a.reports)
        .slice(0, limit);
    },
    async report(id) {
      return reports[id - 1] || null;
    },
    async mark(key, status, fixedIn, issue) {
      const p = problems.get(key);
      if (!p)
        return false;
      Object.assign(p, { status, fixed_in: fixedIn, issue });
      return true;
    },
  };
}
