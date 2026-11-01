# Semirprod for OBS

OBS Studio plugin that connects OBS to your [Semirprod](https://semirprod.fr)
account, so you can drive OBS remotely from the Semirprod dashboard.

Using the plugin requires a Semirprod account. The plugin only talks to the
Semirprod service; it does nothing on its own.

*Version française plus bas.*

## Requirements

- OBS Studio **32.0 or newer**
- Windows 10/11 x64, macOS 13 or newer (Apple Silicon and Intel), or
  Linux x86_64 (built on Ubuntu 24.04)
- A Semirprod account

## Install

Official builds are distributed from [semirprod.fr](https://semirprod.fr).
Every release of this repository also carries its binaries, a
`SHA256SUMS.txt` file and the complete source archive.

| Platform | File |
|---|---|
| Windows | `semirprod-plugin-installer-<version>.exe` (installer) or `obs-semirprod-plugin-v<version>.zip` (drop `obs-plugins` into the OBS folder) |
| macOS | `semirprod-plugin-<version>.pkg`, signed and notarized |
| Linux | `obs-semirprod-plugin-v<version>-linux-x86_64.tar.gz`, extract into `~/.config/obs-studio/plugins/` |

Detailed steps: [`docs/INSTALLATION.md`](docs/INSTALLATION.md) (Windows) and
[`docs/INSTALLATION-macos.md`](docs/INSTALLATION-macos.md) (macOS), in French.

## Build from source

The plugin is built against the **OBS 32.0 SDK**, never a newer one: libobs
refuses a module built against a newer libobs than its own, and OBS 32.2
changed its Qt version. The build scripts check this and refuse to build
otherwise.

Common requirements: CMake 3.28+, Git, a C++17 compiler.

### 1. Prepare the OBS SDK

`.github/scripts/prepare-obs-sdk.sh` clones OBS Studio 32.0.4 and builds only
libobs and obs-frontend-api. It is the exact recipe the release builds use.
It prints the variables the build scripts read (`OBS_SRC`, `OBS_BUILD`, and on
Windows `OBS_DEPS`, `OBS_QT6_DEPS`).

```sh
# Linux (Ubuntu 24.04): also installs the OBS build dependencies with apt
bash .github/scripts/prepare-obs-sdk.sh --install-deps

# macOS (full Xcode required) or Windows (Git Bash, Visual Studio 2022)
bash .github/scripts/prepare-obs-sdk.sh
```

### 2. Build the plugin

**Linux**

```sh
OBS_SRC=... OBS_BUILD=... ./build_plugin.sh
# -> obs-semirprod-plugin-v<version>-linux-x86_64.tar.gz
```

The plugin uses the Qt of the distribution, which must be the Qt your OBS is
built against.

**macOS** (universal arm64 + x86_64)

```sh
OBS_SRC=... OBS_BUILD=... ./build_plugin_macos.sh
# -> dist-macos/semirprod-plugin-<version>.pkg
```

Without a Developer ID certificate in your keychain the plugin is signed
ad hoc: it loads on the machine that built it, Gatekeeper blocks it elsewhere.
Signing and notarization: [`packaging/macos/README.md`](packaging/macos/README.md).

**Windows** (Visual Studio 2022, x64)

Requires Qt **6.8.3** for MSVC 2022 x64, the Qt shipped with OBS 32.0.
`build_plugin.bat` reads its version from the folder layout
`<somewhere>\6.8.3\msvc2022_64`. The installer needs Inno Setup 6 and the Qt
TLS backend in `deps\qt6\plugins\tls\qschannelbackend.dll` (for instance a
link from `deps\qt6` to the Qt folder).

```bat
set "OBS_SRC=..."
set "OBS_BUILD=..."
set "OBS_DEPS=..."
set "QT_DIR=C:\Qt\6.8.3\msvc2022_64"
set "SEMIRPROD_SKIP_OBS_COPY=1"
build_plugin.bat
rem -> obs-semirprod-plugin-v<version>.zip and semirprod-plugin-installer-<version>.exe
```

Each script lists at its top every variable it accepts.

### Web interface

The interface embedded in the plugin is `resources/ui/index.html`, built from
`webui/`. It is committed, so a plugin build does not need Node.js. To rebuild
it:

```sh
cd webui
npm ci
npm run build
```

## Scene collection backups

A backup contains the scene collection and its media. Secrets are removed
before upload (stream keys and destinations of Semirprod modules, secret-looking
keys of other plugins' modules, tokens in StreamElements / Streamlabs URLs).

Importing a backup restores scenes, sources and media, with these limits:

- a backup the server confirms **you created yourself** is restored in full
  (media files, hotkeys, saved projectors, browser source CSS);
- a backup created by **another member** of the workspace brings the scenes and
  their layout only: **no media file is written to disk**, and no hotkeys,
  saved projectors or browser source CSS;
- the Journal dock lists every source left without its file (up to 50 names,
  the rest is counted);
- the list of backups, and which ones are yours, is forgotten when you
  disconnect the plugin or switch accounts;
- the configuration of **third-party plugin modules** (e.g. Advanced Scene
  Switcher) is **not restored**, even from your own backup: reconfigure them;
- OBS's own modules are kept but nothing starts by itself (automatic scene
  switcher off, output timers not armed);
- browser sources lose local files and any control over OBS, and keep their
  URL only when it points to a known overlay provider (list in `src/Config.cpp`,
  `isKnownOverlayHost`); every removed URL is listed in the Journal dock;
- media sources only keep embedded local files; settings of unknown source
  types are cleared;
- embedded media whose content does not match its extension is discarded, and
  so is an image, video or audio file whose content would make the player follow
  another file or URL (DASH, IMF, QuickTime reference movie; for a VLC
  playlist, also a line starting with a URL or a network path). URLs inside
  image metadata, audio tags or text files are fine.

## Repository layout

| Path | Content |
|---|---|
| `src/` | Plugin sources (C++/Qt) |
| `webui/` | Sources of the embedded web interface |
| `resources/`, `data/` | Embedded interface, localized strings |
| `packaging/windows/`, `packaging/macos/` | Windows installer, macOS package |
| `cmake/` | CMake helpers |
| `.github/` | Release workflow and its OBS SDK script |

This repository is a mirror: development happens in a private repository and
each release is published here as one commit. See
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

GNU General Public License, version 2 or (at your option) any later version.
See [`LICENSE`](LICENSE), and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
for the components the plugin uses.

The Semirprod name and logo are not covered by this license. A modified
version must not present itself as published by Semirprod.

Security issues: see [`SECURITY.md`](SECURITY.md). Do not open a public issue.

---

## Français

**Semirprod pour OBS** est le plugin OBS Studio qui relie OBS à ton compte
[Semirprod](https://semirprod.fr) : tu pilotes OBS à distance depuis le tableau
de bord Semirprod. Il faut un compte Semirprod et OBS Studio 32.0 ou plus
récent.

Les versions officielles se téléchargent sur [semirprod.fr](https://semirprod.fr).
Chaque version publiée ici contient aussi les binaires, leurs empreintes
(`SHA256SUMS.txt`) et l'archive complète du code source. Installation :
[`docs/INSTALLATION.md`](docs/INSTALLATION.md) (Windows) et
[`docs/INSTALLATION-macos.md`](docs/INSTALLATION-macos.md) (macOS).

Sauvegardes de collections : la configuration des modules de plugins tiers
n'est pas restaurée à l'import, même depuis ta propre sauvegarde (voir « Scene
collection backups » ci-dessus).

Compilation : voir « Build from source » ci-dessus. Licence : GPL version 2 ou
toute version ultérieure. Une faille de sécurité se signale à
contact@semirprod.fr, jamais dans une issue publique (voir
[`SECURITY.md`](SECURITY.md)).
