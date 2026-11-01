# Security policy

## Reporting a vulnerability

Email **contact@semirprod.fr** with `[security] Semirprod for OBS` in the
subject. Do **not** open a public issue, pull request or discussion for a
vulnerability.

If private vulnerability reporting is enabled on this repository, you can also
use **Security > Report a vulnerability**.

Please include:

- the plugin version (OBS log line `[semirprod-studio]`, or the installer name),
  the OBS Studio version and the operating system;
- what an attacker can do, and the steps to reproduce it;
- a proof of concept if you have one.

Remove tokens, stream keys and account identifiers from logs before sending
them.

We aim to acknowledge reports within 5 working days, and keep you informed
until the fix ships. We credit reporters in the release notes, unless you prefer not to
be named.

## Supported versions

Only the **latest release** receives security fixes. The plugin checks for
updates itself; please update before reporting.

## Scope

In scope: the plugin code in this repository and the release packages built
from it (Windows installer, macOS package, Linux archive).

The Semirprod online service (website, dashboard, APIs) is out of scope for
this repository, but reports about it are welcome at the same address.

## How releases are built

Release binaries are built by the public workflow
[`.github/workflows/release.yml`](.github/workflows/release.yml) from the tagged
commit. They are downloaded from a Semirprod account, not from this repository;
each release here carries their SHA-256 checksums (`SHA256SUMS.txt`) and the
complete source archive. Signing credentials are only available to jobs that
run for an approved release tag, never to pull requests or forks.

The workflow follows these rules, and changes to it must keep them:

- actions pinned by full commit SHA; no third-party action;
- secrets passed to steps through environment variables only, never printed,
  never placed on a command line when a file or stdin can carry them. The one
  exception is macOS `security`, which accepts the temporary keychain password
  and the `.p12` passwords only as arguments (`-p`, `-k`, `-P`): they are
  visible in the process list of the ephemeral GitHub-hosted runner while
  those commands run;
- the `release` environment and all its secrets (signing material and
  `SEMIRPROD_RELEASE_UPLOAD_TOKEN`) are configured in this public repository,
  and only the two jobs bound to that environment can read them;
- every script runs with `set -euo pipefail`;
- the temporary macOS signing keychain and all decoded key files are deleted
  at the end of the job, including when it fails.
