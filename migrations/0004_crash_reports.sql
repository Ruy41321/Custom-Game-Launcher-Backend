-- 0004_crash_reports
--
-- Where a launcher's crash lands when its owner has opted in to sending it.
--
-- **There is no user_id, and that is the point.** A crash report is a diagnostic about a
-- program, not a record about a person, and the moment it names an account it becomes personal
-- data that the erasure in 0001's account_deletion_requests then has to reason about — one more
-- table for a future session to remember. Nothing here identifies who sent it: the client
-- strips its own user profile path out of the text before it travels, and the server keeps only
-- what a developer needs to fix the bug. The price is that an operator cannot ask "which of my
-- testers hit this", and that price is accepted rather than overlooked.
--
-- Nothing here is a foreign key either. A report is a leaf: it outlives the launcher version it
-- came from and refers to nothing this schema owns.

CREATE TABLE crash_reports (
    id              uuid        PRIMARY KEY DEFAULT gen_random_uuid(),
    -- Which handler caught it: `unhandled` or `unobserved-task` today, and whatever a later
    -- client invents. Deliberately text rather than an enum, because the set belongs to the
    -- client and a new one must not need a migration on the server to be receivable.
    kind            text        NOT NULL,
    -- When the launcher says it crashed, which is not when the server received it: a report is
    -- written to disk and sent on the *next* start, so the two can be days apart.
    occurred_at     timestamptz NOT NULL,
    launcher_version text       NOT NULL DEFAULT '',
    platform        text        NOT NULL DEFAULT '',
    exception_type  text        NOT NULL DEFAULT '',
    message         text        NOT NULL DEFAULT '',
    stack_trace     text        NOT NULL DEFAULT '',
    -- SHA-256 of the exception type and the shape of the stack, computed server-side so two
    -- clients cannot disagree about what "the same crash" means and no client can choose its
    -- own grouping. This is what turns a list of a thousand reports into a list of nine bugs.
    fingerprint     char(64)    NOT NULL,
    received_at     timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT crash_reports_kind_not_empty     CHECK (kind <> ''),
    CONSTRAINT crash_reports_fingerprint_format CHECK (fingerprint ~ '^[0-9a-f]{64}$')
);

-- The two questions the operator console asks: what has been arriving lately, and how often has
-- this particular crash happened.
CREATE INDEX crash_reports_received_idx    ON crash_reports (received_at DESC);
CREATE INDEX crash_reports_fingerprint_idx ON crash_reports (fingerprint, received_at DESC);

INSERT INTO permissions (key, description) VALUES
    ('admin.crashes.read', 'Read crash reports sent by launchers')
ON CONFLICT (key) DO NOTHING;

-- The admin role holds every permission, and 0001 granted them by cross join at seed time —
-- which cannot know about a permission added later.
INSERT INTO role_permissions (role_id, permission_id)
SELECT r.id, p.id
FROM roles r
JOIN permissions p ON p.key = 'admin.crashes.read'
WHERE r.key = 'admin'
ON CONFLICT DO NOTHING;
