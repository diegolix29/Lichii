import http.client
import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request


EXEMPT_LABELS = {
    "obsolete", "pinned", "enhancement", "accessibility", "deferred",
    "help-wanted", "help wanted", "investigating",
}
TEAM_ROLES = {"OWNER", "MEMBER", "COLLABORATOR"}


class ApiError(Exception):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class GitHub:
    def __init__(self, api_url, repo, token):
        self.base = f"{api_url.rstrip('/')}/repos/{repo}"
        self.opener = urllib.request.build_opener(NoRedirect())
        self.headers = {
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/json",
        }
        self.next_write = 0

    def request(self, path, *, shape=None, payload=None):
        method = "GET" if payload is None else "POST"
        data = None if payload is None else json.dumps(payload).encode()
        request = urllib.request.Request(
            self.base + path, data=data, headers=self.headers, method=method
        )
        attempts = 3 if method == "GET" else 1
        if method == "POST":
            time.sleep(max(0, self.next_write - time.monotonic()))
            self.next_write = time.monotonic() + 1
        for attempt in range(attempts):
            delay = 2 ** attempt
            try:
                with self.opener.open(request, timeout=30) as response:
                    if shape is None:
                        return
                    value = json.load(response)
                    if not isinstance(value, shape):
                        raise ValueError("unexpected JSON shape")
                    return value, response.headers.get("Link", "")
            except urllib.error.HTTPError as error:
                status = error.code
                headers = error.headers
                error.close()
                reason = f"HTTP {status}"
                retry_after = headers.get("Retry-After", "")
                throttled = status == 429 or (status == 403 and (
                    retry_after or headers.get("X-RateLimit-Remaining") == "0"
                ))
                if not throttled and status not in {408, 500, 502, 503, 504}:
                    raise ApiError(f"{method} {path}: {reason}") from None
                if throttled:
                    reset = headers.get("X-RateLimit-Reset", "")
                    if retry_after.isdecimal():
                        delay = max(delay, int(retry_after))
                    elif headers.get("X-RateLimit-Remaining") == "0" and reset.isdecimal():
                        delay = max(delay, int(reset) - int(time.time()) + 1)
                    else:
                        delay = 60
            except ValueError:
                reason = "invalid JSON response"
            except (OSError, http.client.HTTPException) as error:
                reason = type(error).__name__
            if attempt + 1 == attempts or delay > 60:
                raise ApiError(f"{method} {path}: {reason}") from None
            print(f"Retrying {method} {path} in {delay}s ({reason})", flush=True)
            time.sleep(delay)

    def get(self, path):
        return self.request(path, shape=dict)[0]

    def pages(self, path, **filters):
        page = 1
        while True:
            query = urllib.parse.urlencode(dict(filters, per_page=100, page=page))
            items, link = self.request(f"{path}?{query}", shape=list)
            yield from items
            if 'rel="next"' not in link:
                return
            page += 1


def target_issues(body, override):
    if not override:
        opt_in = [line for line in (body or "").splitlines() if line.startswith("<!-- lichii-retest:")]
        if not opt_in:
            return []
        match = re.fullmatch(r"<!-- lichii-retest: (.+) -->", opt_in[0])
        if len(opt_in) != 1 or not match:
            raise ValueError("Use one release-notes line: <!-- lichii-retest: #123, #456 -->")
        override = match[1]
    if not re.fullmatch(r"#?[1-9][0-9]*(?:\s*,\s*#?[1-9][0-9]*)*", override.strip()):
        raise ValueError("Retest issues must be comma-separated issue numbers")
    return list(dict.fromkeys(int(part.strip().lstrip("#")) for part in override.split(",")))


def eligible(issue, published):
    return (
        issue["state"] == "open" and not issue["locked"] and "pull_request" not in issue
        and issue["created_at"] <= published and issue["milestone"] is None
        and not issue["assignees"]
        and not EXEMPT_LABELS.intersection(label["name"].lower() for label in issue["labels"])
    )


def notice_time(comments, marker):
    return next((
        comment["created_at"] for comment in comments
        if comment["user"]["login"] == "github-actions[bot]"
        and isinstance(comment["body"], str) and comment["body"].rstrip().endswith(marker)
    ), None)


def retest_issue(api, number, release, dry_run):
    path = f"/issues/{number}"
    published = release["published_at"]
    if not eligible(api.get(path), published):
        return "not eligible"

    marker = f"<!-- lichii-obsolete:{release['id']} -->"
    comments_path = f"{path}/comments"
    comments = list(api.pages(comments_path, since=published))
    notified_at = notice_time(comments, marker)
    if notified_at and any(
        event["event"] == "labeled" and event["label"]["name"] == "obsolete"
        and event["created_at"] >= notified_at
        for event in api.pages(f"{path}/events")
    ):
        return "already marked for this release"

    if not notified_at and not dry_run:
        tag, url = release["tag_name"], release["html_url"]
        body = (
            f"Lichii [{tag}]({url}) is out. Please check whether this still happens on {tag}.\n\n"
            "If it does, reply here within 7 days. A new log from **Help → Report a Bug** helps. "
            "Otherwise we'll close this issue as not planned. If it still happens later, "
            f"you can ask us to reopen it or file a new issue.\n\n{marker}"
        )
        api.request(comments_path, payload={"body": body})
        time.sleep(1)
        comments = list(api.pages(comments_path, since=published))
        notified_at = notice_time(comments, marker)
        if not notified_at:
            raise ApiError("posted notice is not visible in comment history; check again on the next run")

    if notified_at and any(
        comment["created_at"] >= notified_at and comment["user"]["type"] != "Bot"
        and comment["author_association"] not in TEAM_ROLES
        for comment in comments
    ):
        return "reporter already replied"
    if dry_run:
        return "would mark"
    if not eligible(api.get(path), published):
        return "no longer eligible"
    api.request(f"{path}/labels", payload={"labels": ["obsolete"]})
    return "marked"


def run(api, tag, override, dry_run):
    path = f"/releases/tags/{urllib.parse.quote(tag, safe='')}" if tag else "/releases/latest"
    release = api.get(path)
    if release["draft"] or release["prerelease"] or not release["published_at"]:
        raise ValueError("Retests require a published stable release")
    numbers = target_issues(release["body"], override)
    if not numbers:
        print("No explicit retest targets; no issues changed")
        return 0

    failures = 0
    for number in numbers:
        try:
            result = retest_issue(api, number, release, dry_run)
            print(f"#{number}: {result}", flush=True)
        except ApiError as error:
            failures += 1
            print(f"::warning::Skipped #{number}: {error}", flush=True)
        except (KeyError, TypeError, AttributeError):
            failures += 1
            print(f"::warning::Skipped #{number}: unexpected GitHub API fields", flush=True)
    print(f"Checked {len(numbers)} explicit targets for {release['tag_name']}; {failures} failed")
    return int(failures != 0)


if __name__ == "__main__":
    try:
        raise SystemExit(run(
            GitHub(os.environ["GITHUB_API_URL"], os.environ["GITHUB_REPOSITORY"], os.environ["GH_TOKEN"]),
            os.environ.get("TAG", ""), os.environ.get("ISSUES", ""),
            os.environ.get("DRY_RUN") == "true",
        ))
    except (ApiError, ValueError) as error:
        raise SystemExit(str(error)) from None
