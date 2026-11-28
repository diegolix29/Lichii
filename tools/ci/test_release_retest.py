import contextlib
import io
import json
import threading
import unittest
import urllib.parse
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, HTTPServer
from unittest.mock import patch

from release_retest import GitHub, run


PUBLISHED = "2026-11-25T15:23:20Z"
NOTIFIED = "2026-11-25T15:48:25Z"
MARKER = "<!-- lichii-obsolete:4100 -->"


def bot_notice(body=MARKER):
    return {
        "body": body, "created_at": NOTIFIED,
        "user": {"login": "github-actions[bot]", "type": "Bot"},
        "author_association": "NONE",
    }


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, value, status=200, headers=None):
        body = value if isinstance(value, bytes) else json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        for key, val in (headers or {}).items():
            self.send_header(key, val)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        request = urllib.parse.urlsplit(self.path)
        path = request.path.removeprefix("/repos/test/lichii")
        page = int(urllib.parse.parse_qs(request.query).get("page", ["1"])[0])
        state = self.server.state
        state.reads.append((path, page))
        failures = state.read_failures[path, page]
        if failures:
            status, body = failures.pop(0)
            return self.respond(body, status)
        if path in {"/releases/latest", "/releases/tags/build-410"}:
            return self.respond(state.release)
        _, _, number, *suffix = path.split("/")
        number = int(number)
        if suffix == ["comments"] or suffix == ["events"]:
            records = (state.comments if suffix == ["comments"] else state.events)[number]
            start = (page - 1) * 100
            headers = {"Link": '<http://example.invalid>; rel="next"'} if len(records) > start + 100 else {}
            return self.respond(records[start:start + 100], headers=headers)
        issue = {
            "state": "open", "locked": False, "created_at": "2026-11-20T00:00:00Z",
            "milestone": None, "assignees": [], "labels": state.labels[number],
        }
        self.respond(issue)

    def do_POST(self):
        _, _, _, _, _, number, operation = self.path.split("/")
        number = int(number)
        state = self.server.state
        state.writes.append((number, operation))
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if operation == "comments":
            state.comments[number].append(bot_notice(data["body"]))
            return self.respond(b"", state.comment_status.get(number, 201))
        state.labels[number].append({"name": data["labels"][0]})
        state.events[number].append({
            "event": "labeled", "label": {"name": "obsolete"}, "created_at": NOTIFIED,
        })
        self.respond(b"{", 200)


class ReleaseRetestTests(unittest.TestCase):
    def setUp(self):
        self.release = {
            "id": 4100, "tag_name": "build-410", "html_url": "https://example.invalid/build-410",
            "published_at": PUBLISHED, "draft": False, "prerelease": False,
            "body": "Loading text fix for #2422 ([#2452](https://example.invalid/pull/2452)).",
        }
        self.reads, self.writes = [], []
        self.read_failures = defaultdict(list)
        self.comments, self.events, self.labels = (defaultdict(list) for _ in range(3))
        self.comment_status = {}
        server = HTTPServer(("127.0.0.1", 0), Fixture)
        server.state = self
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
        thread.start()
        self.addCleanup(lambda: (server.shutdown(), thread.join(), server.server_close()))
        self.api = GitHub(f"http://127.0.0.1:{server.server_port}", "test/lichii", "test-only")
        self.sleep = self.enterContext(patch("release_retest.time.sleep"))

    def retest(self, issues="", dry_run=False, tag="build-410"):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = run(self.api, tag, issues, dry_run)
        return result, output.getvalue()

    def test_only_explicit_scope_applies_and_dry_run_never_writes(self):
        self.assertEqual(self.retest(tag="")[0], 0)
        self.assertEqual(self.reads, [("/releases/latest", 1)])
        self.release["body"] += "\n<!-- lichii-retest: #2422, #2422 -->"
        self.labels[2422].append({"name": "Investigating"})
        self.assertIn("#2422: not eligible", self.retest()[1])
        self.labels[2422].clear()
        self.assertIn("#2422: would mark", self.retest(dry_run=True)[1])
        self.assertIn("#1: would mark", self.retest("1", dry_run=True)[1])
        self.assertNotIn(("/issues/2581", 1), self.reads)
        self.assertEqual(self.writes, [])
        with self.assertRaisesRegex(ValueError, "comma-separated"):
            self.retest("#2422 or all")

    def test_retries_bad_reads_and_skips_only_the_exhausted_issue(self):
        self.read_failures["/issues/1/comments", 1] = [(200, b"")] * 3
        self.read_failures["/issues/2/comments", 1] = [(200, b"["), (502, b"")]
        result, output = self.retest("1,2")
        self.assertEqual(result, 1)
        self.assertIn("Skipped #1: GET /issues/1/comments?", output)
        self.assertIn("invalid JSON response", output)
        self.assertIn("HTTP 502", output)
        self.assertIn("#2: marked", output)
        self.assertEqual(self.reads.count(("/issues/1/comments", 1)), 3)
        self.assertEqual(self.writes, [(2, "comments"), (2, "labels")])
        self.assertEqual(self.retest("2")[0], 0)
        self.assertEqual(self.writes, [(2, "comments"), (2, "labels")])

    def test_lost_comment_response_resumes_without_reposting(self):
        self.comment_status[1] = 502
        result, output = self.retest("1,2")
        self.assertEqual(result, 1)
        self.assertIn("Skipped #1: POST /issues/1/comments: HTTP 502", output)
        self.assertEqual(self.writes, [(1, "comments"), (2, "comments"), (2, "labels")])
        self.assertEqual(self.retest("1,2")[0], 0)
        self.assertEqual(self.writes, [
            (1, "comments"), (2, "comments"), (2, "labels"), (1, "labels"),
        ])

    def test_paginated_history_prevents_remarking_or_double_posting(self):
        self.comments[1].extend([bot_notice(None)] * 100 + [bot_notice()])
        self.events[1].extend([{"event": "unlabeled"}] * 100 + [{
            "event": "labeled", "label": {"name": "obsolete"}, "created_at": NOTIFIED,
        }])
        self.comments[2].extend([bot_notice()] + [bot_notice(None)] * 99 + [{
            "body": "Still happens", "created_at": NOTIFIED,
            "user": {"login": "reporter", "type": "User"}, "author_association": "NONE",
        }])
        self.read_failures["/issues/1/comments", 2] = [(200, b"")]
        result, output = self.retest("1,2")
        self.assertEqual(result, 0)
        self.assertIn("#1: already marked for this release", output)
        self.assertIn("#2: reporter already replied", output)
        self.assertEqual(self.reads.count(("/issues/1/comments", 2)), 2)
        self.assertIn(("/issues/1/events", 2), self.reads)
        self.assertEqual(self.writes, [])


if __name__ == "__main__":
    unittest.main()
