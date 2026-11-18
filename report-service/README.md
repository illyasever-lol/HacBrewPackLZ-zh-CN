# The report service

Where Report a problem sends reports, from every version of hacBrewPackLZ: a Cloudflare Worker on the free plan, with its own inbox in a D1
database. It groups reports by problem, answers known problems at once, turns away floods, and holds no GitLab key: issues are opened from
the inbox by whoever reads it, with the read key.

## Setting it up, once (about ten minutes, in the browser)

1. Make a free account at https://dash.cloudflare.com/sign-up.
2. **The database.** Storage & Databases > D1 > Create, named `hacbrewpacklz-reports`. In its Console, paste all of `schema.sql` and run it.
3. **The worker.** Compute (Workers) > Create > Start with Hello World, named `hacbrewpacklz-reports`, then Deploy. Edit code, replace
   everything with `worker.js`, and Deploy again.
4. **Its database.** In the worker: Settings > Bindings > Add > D1 database, variable name `DB`, the database from step 2.
5. **The read key.** On this PC, make one with
   `python -c "import secrets; print(secrets.token_urlsafe(32))"`.
   In the worker: Settings > Variables and Secrets > Add, type Secret, name `READ_KEY`, and paste it as the value. Keep it for step 6 too.
6. **For Claude.** So Claude's sessions can read the inbox, in a Command Prompt:
   `setx HBPLZ_REPORTS_URL "https://hacbrewpacklz-reports.<your subdomain>.workers.dev"` and
   `setx HBPLZ_REPORTS_KEY "<the read key>"`, then open a new terminal. The address is on the worker's page.
7. **Check it.** `python tools\report_smoke.py` sends a report, finds it in the inbox, and marks it dismissed.

Then give Claude the worker's address, so the apps send there. The read key never goes in the apps: all they can do is send.

## Reading the inbox

With the read key in `Authorization: Bearer <key>`:
- `GET /inbox?status=new` lists new problems, the most serious first: games that won't start after being compressed, then crashes, freezes,
  jobs that failed, and messages. Each has how many reports and devices it has, its versions and platforms, and its latest reports.
- `GET /inbox/report/<number>` is one whole report.
- `POST /seen` is how the apps count a problem they've sent before, seen again: an app sends a problem it finds itself once per version, and
  after that only how many more times it saw it, so `occurrences` goes up without another report.
- `POST /problem/<key>` with `{"status": "known" | "fixed" | "dismissed" | "new", "fixedIn": "1.5.1", "issue": 42}` marks a problem.
  Known and fixed problems are answered at once from then on, and the apps tell the person.

## Tests

`python report-service\test\run_tests.py` runs the worker's handling in Edge, against an inbox kept in memory, with nothing to install.
