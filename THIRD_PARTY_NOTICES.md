# Third-party notices

Semirprod for OBS is licensed under the GNU General Public License,
version 2 or (at your option) any later version. See `LICENSE`.

"Or any later version" is required, not decorative: Qt 6 (LGPL-3.0) and
OpenSSL 3 (Apache-2.0) are compatible with GPL-3.0 but not with GPL-2.0-only.
Do not narrow the license to GPL-2.0-only.

## Linked at run time (not shipped, provided by OBS Studio)

| Component | License | Source |
|---|---|---|
| libobs, obs-frontend-api | GPL-2.0-or-later | https://github.com/obsproject/obs-studio |
| Qt 6 (Core, Gui, Widgets, Network) | LGPL-3.0 | https://download.qt.io/official_releases/qt/ |

## Linked at build time, depending on the platform

| Component | Platforms | License | Source |
|---|---|---|---|
| QtKeychain | Windows, macOS, Linux (public edition) | BSD-3-Clause | https://github.com/frankosterfeld/qtkeychain |
| OpenSSL (libcrypto) | macOS, Linux | Apache-2.0 | https://www.openssl.org/source/ |

## Shipped as a file next to the plugin

| Component | When | License | Source |
|---|---|---|---|
| Qt TLS backend (`qschannelbackend.dll` on Windows, `libqsecuretransportbackend.dylib` on macOS) | Windows installer only when OBS has none; always inside the macOS bundle | LGPL-3.0 | https://download.qt.io/official_releases/qt/ (qtbase, `src/plugins/tls`) |

This file is the unmodified Qt build shipped by obs-deps
(https://github.com/obsproject/obs-deps). Under LGPL-3.0 you may replace it
with your own build of the same Qt version.

## Copied into this source tree

| File | Origin | License |
|---|---|---|
| `src/ui/browser-panel.hpp` | obs-browser, `panel/browser-panel.hpp` (https://github.com/obsproject/obs-browser) | GPL-2.0-or-later |

## Embedded web interface (`resources/ui/index.html`, built from `webui/`)

Bundled into the plugin binary. Exact versions are in `webui/package-lock.json`.

| Package | License |
|---|---|
| react, react-dom | MIT |
| @radix-ui/react-dialog, react-label, react-select, react-slot, react-switch | MIT |
| class-variance-authority | Apache-2.0 |
| clsx | MIT |
| tailwind-merge | MIT |
| lucide-react | ISC |
| tslib | 0BSD |
| Tailwind CSS (generated styles) | MIT |

## Name and logo

The Semirprod name and logo (`logo.ico`, `logo.png`) identify the publisher.
They are not a trademark license: a modified version must not present itself
as published by Semirprod.
