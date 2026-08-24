#!/bin/sh
set -eu

# Applying migrations at boot keeps a single-node deployment simple. Set
# RUN_MIGRATIONS=false when a separate job owns the schema, e.g. in a multi-replica setup
# where concurrent migrations would race.
if [ "${RUN_MIGRATIONS:-true}" = "true" ]; then
    echo "entrypoint: applying database migrations"
    /app/launcher-api --migrate
fi

exec /app/launcher-api "$@"
