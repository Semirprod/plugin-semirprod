#!/usr/bin/env bash
#
# Fabrique l'installeur macOS (.pkg) a partir d'un bundle .plugin deja
# construit et signe. Appele par build_plugin_macos.sh, utilisable seul :
#
#   BUNDLE_PATH=../../build-macos-dist/semirprod-studio.plugin ./build_pkg.sh
#
# ---------------------------------------------------------------------------
# POURQUOI UN PAQUET « DOMAINE UTILISATEUR » ET NON UN PAQUET SYSTEME
#
# Sur Apple Silicon, OBS ne scanne qu'UN seul emplacement de plugins :
#   ~/Library/Application Support/obs-studio/plugins/<module>.plugin
# Les deux replis historiques — /Library system-wide, et la disposition
# bin/ + data/ — sont derriere un `#ifndef __aarch64__` dans
# frontend/widgets/OBSBasic.cpp (32.2.2, lignes 161-177). Un .pkg qui
# installerait dans /Library produirait donc, sur tout mac Apple Silicon, une
# installation « reussie » suivie d'un plugin parfaitement invisible.
#
# D'ou `enable_currentUserHome="true"` et `enable_localSystem="false"` dans le
# distribution.xml : le paquet s'installe dans le dossier de l'utilisateur
# courant, sans mot de passe administrateur, et l'install-location est
# relative a ce dossier.
# ---------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

MODULE_NAME="semirprod-studio"
BUNDLE_NAME="$MODULE_NAME.plugin"
IDENTIFIER="fr.semirprod.obs-plugin"

BUNDLE_PATH="${BUNDLE_PATH:-$ROOT/build-macos-dist/$BUNDLE_NAME}"
DIST_DIR="${DIST_DIR:-$ROOT/dist-macos}"

# Identite « Developer ID Installer: ... » — DISTINCTE de l'identite
# « Developer ID Application » qui signe le bundle. Deux certificats, deux
# roles : l'un scelle le code, l'autre scelle le paquet. Vide = paquet non
# signe (Gatekeeper le refusera au double-clic, voir docs/INSTALLATION-macos.md).
INSTALLER_IDENTITY="${INSTALLER_IDENTITY:-}"

# Detection automatique, meme logique que pour le bundle : on ne cherche que
# « Developer ID Installer ». Noter que cette identite n'apparait PAS dans
# `security find-identity -p codesigning` — elle ne sert pas a signer du code,
# seulement des paquets — d'ou l'absence de `-p` ici. Chercher au mauvais
# endroit et conclure « pas de certificat » est une perte de temps classique.
if [ -z "$INSTALLER_IDENTITY" ]; then
  INSTALLER_IDENTITY="$(security find-identity -v 2>/dev/null \
    | grep "Developer ID Installer" | head -1 \
    | sed 's/.*"\(.*\)".*/\1/')"
fi

die() { echo "[ERREUR] $*" >&2; exit 1; }

[ -d "$BUNDLE_PATH" ] || die "bundle introuvable : $BUNDLE_PATH"

# Version : lue dans l'Info.plist du bundle, jamais recopiee a la main. Le
# CMakeLists l'y a mise depuis project(VERSION), qui reste la source unique.
VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
  "$BUNDLE_PATH/Contents/Info.plist" 2>/dev/null)"
[ -n "$VERSION" ] || die "version illisible dans $BUNDLE_PATH/Contents/Info.plist"

echo "[INFO] Paquet macOS $MODULE_NAME $VERSION"

# Verification de derniere minute : un paquet est un bon moyen de distribuer
# tres vite un binaire casse.
codesign --verify --strict "$BUNDLE_PATH" \
  || die "le bundle n'est pas correctement signe — ne pas empaqueter ca."
[ -f "$BUNDLE_PATH/Contents/PlugIns/tls/libqsecuretransportbackend.dylib" ] \
  || die "backend TLS absent du bundle : le plugin serait muet chez le client."

STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

# Arborescence RELATIVE au dossier de l'utilisateur (domaine currentUserHome).
PAYLOAD="$STAGING/payload/Library/Application Support/obs-studio/plugins"
mkdir -p "$PAYLOAD"
ditto "$BUNDLE_PATH" "$PAYLOAD/$BUNDLE_NAME" || die "copie du bundle echouee."

# --- Ressources de l'installeur (pages + logo) ---
#
# Copiees dans un dossier temporaire plutot qu'utilisees en place : le logo y
# est genere a la volee depuis logo.png, a la taille d'affichage. On evite ainsi
# de versionner une deuxieme copie du logo qui pourrait diverger de l'originale.
RESOURCES="$STAGING/resources"
mkdir -p "$RESOURCES"
cp "$HERE/resources/"*.html "$RESOURCES/" 2>/dev/null

LOGO="$ROOT/logo.png"
if [ -f "$LOGO" ]; then
  # 128 px : assez grand pour etre net sur ecran Retina, assez petit pour ne pas
  # manger le panneau. sips est livre avec macOS, aucune dependance ajoutee.
  sips -Z 128 "$LOGO" --out "$RESOURCES/background.png" >/dev/null 2>&1 \
    && echo "[INFO] logo Semirprod integre a l'installeur" \
    || echo "[ATTENTION] logo illisible — l'installeur sera sans image"
else
  echo "[ATTENTION] $LOGO introuvable — l'installeur sera sans image"
fi

mkdir -p "$DIST_DIR"
COMPONENT="$STAGING/$MODULE_NAME-component.pkg"
FINAL="$DIST_DIR/semirprod-plugin-$VERSION.pkg"

# --- Composant ---
#
# `--install-location` est relative au domaine choisi par le distribution.xml.
PKGBUILD_ARGS=(
  --root "$STAGING/payload"
  --identifier "$IDENTIFIER"
  --version "$VERSION"
  --install-location "/"
  --scripts "$HERE/scripts"
)
pkgbuild "${PKGBUILD_ARGS[@]}" "$COMPONENT" >/dev/null \
  || die "pkgbuild a echoue."

# --- Distribution ---
PRODUCTBUILD_ARGS=(
  --distribution "$HERE/distribution.xml"
  --package-path "$STAGING"
  --resources "$RESOURCES"
)
if [ -n "$INSTALLER_IDENTITY" ]; then
  PRODUCTBUILD_ARGS+=(--sign "$INSTALLER_IDENTITY")
else
  echo "[ATTENTION] paquet NON SIGNE : Gatekeeper le refusera au double-clic"
  echo "            chez un tiers. Renseigne INSTALLER_IDENTITY pour un paquet"
  echo "            distribuable (identite « Developer ID Installer »)."
fi

# Le distribution.xml reference le composant par son nom de fichier : on le
# renomme donc de facon stable avant l'appel.
mv "$COMPONENT" "$STAGING/$MODULE_NAME.pkg"

productbuild "${PRODUCTBUILD_ARGS[@]}" "$FINAL" >/dev/null \
  || die "productbuild a echoue."

echo "[OK] $FINAL"
echo "     $(du -h "$FINAL" | cut -f1)"

# ---------------------------------------------------------------------------
# NOTARISATION
#
# C'est elle, et pas la signature seule, qui supprime l'ecran « developpeur non
# identifie ». Un paquet signe Developer ID mais NON notarise declenche le meme
# blocage qu'un paquet non signe : Gatekeeper interroge Apple, ne trouve pas de
# ticket, refuse. La signature seule ne sert qu'a rendre la notarisation
# possible.
#
# Prealable, UNE FOIS, pour ne pas avoir a repasser les identifiants :
#   xcrun notarytool store-credentials semirprod \
#     --apple-id <compte> --team-id <equipe> --password <mot de passe d'app>
# puis NOTARY_PROFILE=semirprod.
#
# Le mot de passe est un « mot de passe pour application » cree sur
# appleid.apple.com, jamais le mot de passe du compte Apple.
# ---------------------------------------------------------------------------
if [ -n "${NOTARY_PROFILE:-}" ]; then
  if [ -z "$INSTALLER_IDENTITY" ]; then
    die "NOTARY_PROFILE est renseigne mais le paquet n'est pas signe.
         Apple refuse de notariser un paquet non signe : renseigne aussi
         INSTALLER_IDENTITY (identite « Developer ID Installer »)."
  fi

  echo
  echo "[INFO] Notarisation (profil « $NOTARY_PROFILE »)... quelques minutes."
  if xcrun notarytool submit "$FINAL" --keychain-profile "$NOTARY_PROFILE" --wait; then
    # `stapler` agrafe le ticket DANS le fichier : sans lui, le paquet exige
    # qu'Apple soit joignable au moment de l'installation chez le client. Un
    # client hors ligne verrait le blocage revenir.
    xcrun stapler staple "$FINAL" || die "stapler a echoue — ticket non agrafe."
    echo "[OK] notarise et agrafe."
  else
    # On n'efface pas le paquet : le rapport de notarisation se consulte avec
    # l'identifiant de soumission affiche juste au-dessus,
    #   xcrun notarytool log <id> --keychain-profile "$NOTARY_PROFILE"
    # et il dit fichier par fichier ce qui manque (signature, hardened runtime,
    # horodatage). C'est la seule facon d'avancer, d'ou ce message plutot qu'un
    # « echec » sec.
    die "notarisation refusee. Recupere le detail avec :
           xcrun notarytool log <id-de-soumission> --keychain-profile $NOTARY_PROFILE"
  fi
fi

# Ce que le paquet fera reellement, imprime plutot que promis.
echo
echo "[INFO] Contenu declare :"
pkgutil --payload-files "$FINAL" 2>/dev/null | head -5

# Verdict de Gatekeeper, mesure et non suppose. C'est la reponse a la seule
# question qui compte : « est-ce que ca s'ouvre sans avertissement chez un
# tiers ». `-t install` est le type d'evaluation propre aux paquets.
echo
echo "[INFO] Verdict Gatekeeper :"
if spctl --assess --type install -vv "$FINAL" 2>&1 | sed 's/^/       /'; then
  :
else
  echo "       (refuse : paquet non signe ou non notarise — voir"
  echo "        docs/INSTALLATION-macos.md)"
fi
