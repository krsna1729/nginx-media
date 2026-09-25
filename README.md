# nginx-media capacity history

Published by .github/workflows/bench.yml: every merge to main (tier branch),
the nightly run and the weekly run append one record to data/<tier>.jsonl;
index.html charts them.  The viewer source is tests/bench/history/index.html
on main; the publish job copies it here.
