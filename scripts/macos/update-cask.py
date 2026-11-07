#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0
"""Update the in-repository cask from a published release's checksum manifest."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[2]
REPOSITORY = "https://github.com/MohamedElashri/kwi-mac"
REPO = "MohamedElashri/kwi-mac"
CASK = "Casks/zirq.rb"


def render(tag, checksums, text):
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("Use a stable release tag such as vX.Y.Z")
    # Until the first Zirq release is published, the tap still serves the
    # verified Kwi archive. Switch both artifact and bundle names together.
    matches = re.findall(r"^([0-9a-f]{64})  (Zirq|Kwi)-arm64\.zip$", checksums, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError("Expected exactly one terminal app ZIP SHA-256")
    checksum, product = matches[0]
    text, urls = re.subn(r'/[A-Za-z]+-arm64\.zip"', f'/{product}-arm64.zip"', text)
    app = '  app "Zirq.app"' if product == "Zirq" else '  app "Kwi.app", target: "Zirq.app"'
    text, apps = re.subn(r'^  app .*$', app, text, flags=re.MULTILINE)
    text, versions = re.subn(r'^  version "[^"]+"$', f'  version "{tag[1:]}"', text, flags=re.MULTILINE)
    text, hashes = re.subn(r'^  sha256 "[0-9a-f]{64}"$', f'  sha256 "{checksum}"', text, flags=re.MULTILINE)
    if (versions, hashes, urls, apps) != (1, 1, 1, 1):
        raise ValueError("Cask format changed; review it before updating")
    return text


def update(tag, checksums, path):
    path.write_text(render(tag, checksums, path.read_text()))


def fetch_checksums(tag):
    return subprocess.check_output(
        ["curl", "--fail", "--location", "--silent", "--show-error", "--proto", "=https", "--tlsv1.2",
         "--connect-timeout", "20", "--max-time", "120", f"{REPOSITORY}/releases/download/{tag}/SHA256SUMS"], text=True)


def api(endpoint, data=None):
    command = ["gh", "api", endpoint]
    if data is not None:
        command += ["--input", "-"]
    output = subprocess.check_output(command, input=json.dumps(data) if data is not None else None, text=True)
    result = json.loads(output) if output.strip() else None
    if isinstance(result, dict) and result.get("errors"):
        raise RuntimeError(f"GitHub API rejected the request: {result['errors']}")
    return result


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def sync_pull_request(tag=""):
    release = api(f"repos/{REPO}/releases/latest")
    if release["draft"] or release["prerelease"] or (tag and tag != release["tag_name"]):
        print("Only the latest published stable release updates the cask.")
        return
    tag = release["tag_name"]
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("Unsupported stable release tag")
    if git("status", "--porcelain"):
        raise RuntimeError("Start cask synchronization from a clean checkout of main")
    base = git("rev-parse", "HEAD")
    if api(f"repos/{REPO}/git/ref/heads/main")["object"]["sha"] != base:
        raise RuntimeError("Main advanced; retry from its current commit")
    path = ROOT / CASK
    old = path.read_text()
    version = re.search(r'^  version "([0-9]+\.[0-9]+\.[0-9]+)"$', old, re.MULTILINE)
    if not version:
        raise ValueError("Cannot determine the current cask version")
    if tuple(map(int, tag[1:].split("."))) < tuple(map(int, version[1].split("."))):
        print("The cask already tracks a newer release.")
        return
    new = render(tag, fetch_checksums(tag), old)
    if new == old:
        print(f"The cask already matches {tag}.")
        return
    path.write_text(new)
    subprocess.run(["make", "check"], cwd=ROOT, check=True)
    branch = f"automation/homebrew-{tag}"
    refs = api(f"repos/{REPO}/git/matching-refs/heads/{branch}")
    refs = [ref for ref in refs if ref["ref"] == f"refs/heads/{branch}"]
    if not refs:
        api(f"repos/{REPO}/git/refs", {"ref": f"refs/heads/{branch}", "sha": base})
    head = refs[0]["object"]["sha"] if refs else base
    if head == base:
        result = api("graphql", {
            "query": """mutation($input: CreateCommitOnBranchInput!) {
              createCommitOnBranch(input: $input) {
                commit { oid signature { isValid } }
              }
            }""",
            "variables": {"input": {
                "branch": {"repositoryNameWithOwner": REPO, "branchName": branch},
                "expectedHeadOid": head,
                "message": {"headline": f"Update Homebrew cask to {tag}"},
                "fileChanges": {"additions": [{"path": CASK, "contents": base64.b64encode(new.encode()).decode()}]},
            }},
        })
        commit = result["data"]["createCommitOnBranch"]["commit"]
        if not (commit.get("signature") or {}).get("isValid"):
            raise RuntimeError("GitHub did not verify the automated commit signature")
        head = commit["oid"]
    else:
        content = api(f"repos/{REPO}/contents/{CASK}?ref={quote(branch, safe='')}")
        comparison = api(f"repos/{REPO}/compare/{base}...{head}")
        verification = api(f"repos/{REPO}/commits/{head}")["commit"]["verification"]
        if (base64.b64decode(content["content"]).decode() != new
                or [file["filename"] for file in comparison["files"]] != [CASK] or not verification["verified"]):
            raise RuntimeError("Existing automation branch differs or is unsigned; review it before retrying")
    pulls = api(f"repos/{REPO}/pulls?state=open&base=main&head={quote(REPO.split('/')[0] + ':' + branch, safe='')}")
    pr = pulls[0] if pulls else api(f"repos/{REPO}/pulls", {
        "base": "main", "head": branch, "title": f"Update Homebrew cask to {tag}",
        "body": f"This PR is updating the Homebrew cask to Zirq {tag[1:]} using the published release checksum.",
    })
    # GITHUB_TOKEN-created PRs do not trigger CI, so dispatch their read-only checks explicitly.
    api(f"repos/{REPO}/actions/workflows/checks.yml/dispatches", {
        "ref": branch, "inputs": {"cask_check": "true"},
    })
    print(f"Cask update PR ready: {pr['html_url']} ({head})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default=os.environ.get("TAG", ""))
    parser.add_argument("--checksums", type=Path, help="Use an already verified release manifest")
    parser.add_argument("--pull-request", action="store_true", help="Open a GitHub-signed cask update PR for the latest stable release")
    args = parser.parse_args()
    if args.pull_request:
        sync_pull_request(args.tag)
        return
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", args.tag):
        parser.error("Set TAG to the published stable release tag")
    checksums = args.checksums.read_text() if args.checksums else fetch_checksums(args.tag)
    update(args.tag, checksums, ROOT / "Casks/zirq.rb")
    print(f"Updated Casks/zirq.rb for {args.tag}; review and commit the change.")


if __name__ == "__main__":
    main()
