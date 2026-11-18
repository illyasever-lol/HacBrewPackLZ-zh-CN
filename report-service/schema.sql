-- The inbox the report service keeps, in a D1 database bound to the worker as DB. Run it once, in the database's console.
CREATE TABLE IF NOT EXISTS problems (
  key TEXT PRIMARY KEY,          -- the app and its fingerprint for the problem, or a message of its own
  kind TEXT,                     -- manual, failed-job, crash, freeze or game-didnt-start
  title TEXT,
  status TEXT DEFAULT 'new',     -- new, known, fixed or dismissed
  fixed_in TEXT,
  issue INTEGER,                 -- the GitLab issue it was opened as, if it was
  reports INTEGER DEFAULT 0,
  occurrences INTEGER DEFAULT 0, -- what the apps counted, as each sends a problem once and counts it after that
  devices INTEGER DEFAULT 0,
  versions TEXT DEFAULT '[]',
  platforms TEXT DEFAULT '[]',
  first_seen TEXT,
  last_seen TEXT
);
CREATE TABLE IF NOT EXISTS reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  problem TEXT,
  received TEXT,
  app TEXT,
  version TEXT,
  platform TEXT,
  kind TEXT,
  said TEXT,                     -- what the person wrote
  body TEXT,                     -- the report itself, with no keys in it
  device TEXT                    -- a random name the app made up for itself, nothing to do with the device's own
);
CREATE INDEX IF NOT EXISTS reports_problem ON reports (problem);
CREATE TABLE IF NOT EXISTS sightings (problem TEXT, device TEXT, PRIMARY KEY (problem, device));
CREATE TABLE IF NOT EXISTS limits (who TEXT, hour INTEGER, count INTEGER, PRIMARY KEY (who, hour));
