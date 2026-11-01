# Contributing

Issues and pull requests are welcome.

## How this repository works

This repository is a **mirror**. Development happens in Semirprod's main
repository, and each release is published here as a single commit. As a
consequence:

- a pull request is reviewed here, then **re-applied upstream** by a
  maintainer; it is not merged with the GitHub button;
- the change reaches this repository with the next release, credited to you
  in its release notes;
- a pull request may have to be rebased on the latest release before review.

## Pull requests

- Keep them focused: one fix or one feature per pull request.
- Follow the style of the surrounding code.
- Make sure the build passes on your platform (see "Build from source" in the
  [README](README.md)). The workflow builds Windows, macOS and Linux on every
  pull request.
- Sign off your commits (`git commit -s`): it certifies that you wrote the
  change or have the right to submit it under the project license, as stated
  in the [Developer Certificate of Origin](https://developercertificate.org/).

By contributing, you agree that your contribution is licensed under the
GNU General Public License, version 2 or any later version, like the rest of
the project.

## Bugs

Open an issue with the plugin version, the OBS Studio version, your operating
system and the relevant lines of the OBS log (`Help > Log Files`). Remove
tokens and stream keys from logs first.

Account, billing or service questions go to [semirprod.fr](https://semirprod.fr),
not to this repository.

## Security

Never report a vulnerability in a public issue. See [SECURITY.md](SECURITY.md).
