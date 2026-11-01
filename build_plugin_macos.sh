#!/usr/bin/env bash
#
# Equivalent macOS de build_plugin.bat / build_plugin.sh.
#
# Les trois scripts font le meme travail — valider, garde-fou, compiler,
# ranger, empaqueter — mais le garde-fou n'a PAS le meme sens sur les trois
# plateformes, et c'est tout l'interet de ne pas les fusionner.
#
#   Windows : on CHOISIT le Qt de compilation (6.8.3, celui d'OBS 32.0), le
#             garde-fou verifie qu'on n'a pas derive.
#   Linux   : ce choix n'existe pas, le plugin est charge dans un OBS compile
#             par la distribution ; on verifie qu'on compile avec SON Qt.
#   macOS   : ce choix existe de nouveau, mais il est PIEGE. Les deps
#             prebuilt d'OBS changent de Qt a l'interieur d'une meme ligne
#             majeure : l'arbre 32.0.4 apporte Qt 6.8.3, l'arbre 32.2.2
#             apporte Qt 6.11.1. Compiler contre le second produit un binaire
#             qui marche parfaitement sur le mac de developpement (OBS 32.2)
#             et refuse de charger chez un client en OBS 32.0 ou 32.1. Rien,
#             absolument rien, ne le signale a la compilation.
#
# D'ou le garde-fou central ici : on compile contre le SDK et le Qt de la PLUS
# ANCIENNE cible supportee, jamais contre ceux de l'OBS installe localement.
# Un module lie a un Qt plus recent refuse de charger dans un OBS plus ancien.
#
set -uo pipefail

# === CONFIG MODIFIABLE ===
#
# Arbre OBS compile, d'ou viennent libobs et l'API frontend. Il doit etre a la
# version PLANCHER (voir FLOOR_OBS), pas a la derniere version parue.
#
# Pour le preparer :
#   git -C ~/obs-studio worktree add ~/obs-studio-32.0 32.0.4
#   cd ~/obs-studio-32.0
#   git submodule update --init plugins/obs-browser plugins/obs-websocket
#   cmake --preset macos -DENABLE_BROWSER=OFF -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#   cmake --build build_macos --config RelWithDebInfo --target libobs obs-frontend-api
#
# (Les submodules sont exiges par la configuration d'OBS 32.0 meme quand on ne
# construit pas les plugins ; ENABLE_PLUGINS=OFF, lui, casse la detection du
# langage Swift de libobs-metal. Deux impasses deja explorees.)
OBS_SRC="${OBS_SRC:-$HOME/obs-studio-32.0}"
OBS_BUILD="${OBS_BUILD:-$OBS_SRC/build_macos}"

# Deps prebuilt d'OBS (Qt + SIMDe + le reste). Sur macOS, OBS 32.0.x les
# depose dans ~/.deps ; 32.2.x les depose dans <arbre>/.deps. On accepte les
# deux dispositions, la detection est plus bas.
DEPS_DIR="${DEPS_DIR:-}"

# Planchers declares. Doivent rester d'accord avec build_plugin.bat,
# build_plugin.sh, le README et l'installeur.
FLOOR_OBS="${FLOOR_OBS:-32.0}"
FLOOR_QT="${FLOOR_QT:-6.8}"

MODULE_NAME="semirprod-studio"
BUNDLE_NAME="$MODULE_NAME.plugin"

# Ou deposer le plugin pour un test immediat. Laisse vide pour desactiver.
#
# C'est le SEUL emplacement scanne par OBS sur Apple Silicon : les deux replis
# « legacy » (bin/ + data/, et /Library system-wide) sont derriere un
# `#ifndef __aarch64__` dans frontend/widgets/OBSBasic.cpp. Un paquet qui
# installerait ailleurs produirait un plugin invisible, sans message.
OBS_USER_PLUGINS="${OBS_USER_PLUGINS:-$HOME/Library/Application Support/obs-studio/plugins}"

# === DEV BUILD ===
# Meme semantique que les deux autres scripts : 1 = les variables
# SEMIRPROD_*_BASE sont respectees (staging), sinon les URLs sont figees.
SEMIRPROD_DEV_BUILD_FLAG="${SEMIRPROD_DEV_BUILD_FLAG:-0}"
SEMIRPROD_DEV_BUILD_FLAG="${SEMIRPROD_DEV_BUILD_FLAG// /}"

# === SAUVEGARDE / IMPORT DE COLLECTIONS ===
#
# 1 = l'onglet « Collections » est expose. Meme defaut que build_plugin.bat et
# que l'option du CMakeLists : c'est l'etat vise, et le script Windows ne
# desactive la fonction que si on lui passe explicitement 0.
#
# Ce defaut a d'abord ete a 0 ici, le temps d'un paquet ou la fonction n'etait
# pas voulue. Le garder aurait fait diverger le binaire macOS du binaire Windows
# sans que rien ne le signale : meme depot, meme version, une fonction en moins.
#
# Pour un binaire destine a un backend qui n'a pas encore les routes
# `/api/obs/scene-collections` :
#     SEMIRPROD_COLLECTIONS_FLAG=0 ./build_plugin_macos.sh
SEMIRPROD_COLLECTIONS_FLAG="${SEMIRPROD_COLLECTIONS_FLAG:-1}"
SEMIRPROD_COLLECTIONS_FLAG="${SEMIRPROD_COLLECTIONS_FLAG// /}"

# === SIGNATURE ===
# Vide = signature ad-hoc : le plugin se charge sur la machine qui l'a
# compile, mais Gatekeeper le bloquera ailleurs. Renseigner l'identite
# « Developer ID Application: ... » pour un binaire distribuable.
CODESIGN_IDENTITY="${CODESIGN_IDENTITY:-}"

# Detection automatique de l'identite, si elle n'a pas ete imposee.
#
# On cherche « Developer ID Application » et RIEN D'AUTRE. Le trousseau d'un
# developpeur Apple contient presque toujours aussi un certificat « Apple
# Development » : il signe, il a l'air de marcher, et il ne sert a rien ici —
# il est fait pour le test local et la notarisation le REFUSE. Prendre la
# premiere identite venue produirait donc un paquet signe, refuse a la
# notarisation, et le message d'erreur d'Apple ne dit pas que le certificat
# est du mauvais type.
if [ -z "$CODESIGN_IDENTITY" ]; then
  CODESIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | grep "Developer ID Application" | head -1 \
    | sed 's/.*"\(.*\)".*/\1/')"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build-macos-dist"
DIST="$ROOT/dist-macos"

die() { echo "[ERREUR] $*" >&2; exit 1; }

echo
echo "========================================"
echo "  Build plugin macOS (module $MODULE_NAME)"
echo "========================================"
echo

# === XCODE ===
#
# Les Command Line Tools ne suffisent pas : la configuration d'OBS exige un
# Xcode complet (cmake/macos/compilerconfig.cmake). On passe par DEVELOPER_DIR
# plutot que d'exiger un `sudo xcode-select -s`, qui modifierait le reglage de
# toute la machine pour un seul build.
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app ]; then
  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
command -v cmake >/dev/null || die "cmake absent (brew install cmake)."
command -v ninja >/dev/null || die "ninja absent (brew install ninja)."

# === VALIDATION DEPENDANCES ===
[ -f "$OBS_BUILD/libobs/libobsConfig.cmake" ] \
  || die "libobsConfig.cmake introuvable dans $OBS_BUILD/libobs.
         Compile l'arbre OBS $FLOOR_OBS d'abord (recette en tete de ce script),
         ou pointe OBS_SRC ailleurs."
[ -f "$OBS_BUILD/frontend/api/obs-frontend-apiConfig.cmake" ] \
  || die "obs-frontend-apiConfig.cmake introuvable dans $OBS_BUILD/frontend/api.
         Les trois docks en dependent : sans lui, rien a faire."

# Deps prebuilt : deux dispositions possibles selon la version d'OBS.
if [ -z "$DEPS_DIR" ]; then
  for candidate in "$HOME/.deps" "$OBS_SRC/.deps"/obs-deps-qt6-*-universal; do
    if [ -f "$candidate/lib/cmake/Qt6/Qt6Config.cmake" ]; then
      DEPS_DIR="$candidate"; break
    fi
  done
fi
# Disposition <arbre>/.deps : Qt et le reste (SIMDe...) sont deux dossiers, et
# libobsConfig.cmake a besoin du second. Dans ~/.deps, tout est au meme endroit.
OBS_BASE_DEPS=""
for candidate in "$OBS_SRC/.deps"/obs-deps-20*-universal; do
  [ -d "$candidate/include" ] && OBS_BASE_DEPS="$candidate"
done
[ -n "$DEPS_DIR" ] && [ -d "$DEPS_DIR" ] \
  || die "Deps Qt d'obs-deps introuvables. Renseigne DEPS_DIR."

# === GARDE-FOU 1 : LE SDK LIBOBS ===
#
# libobs refuse tout module compile contre une libobs plus RECENTE que la
# sienne. Un plancher qui monte par accident ne se voit donc PAS ici : il se
# voit chez l'utilisateur, en refus de chargement muet.
SDK_MAJOR="$(sed -n 's/.*define LIBOBS_API_MAJOR_VER *\([0-9]*\).*/\1/p' "$OBS_SRC/libobs/obs-config.h" | head -1)"
SDK_MINOR="$(sed -n 's/.*define LIBOBS_API_MINOR_VER *\([0-9]*\).*/\1/p' "$OBS_SRC/libobs/obs-config.h" | head -1)"
[ -n "$SDK_MAJOR" ] && [ -n "$SDK_MINOR" ] \
  || die "version d'API libobs illisible dans $OBS_SRC/libobs/obs-config.h"

# === GARDE-FOU 2 : LE QT DE COMPILATION ===
#
# Celui qui manquait, et celui qui coute le plus cher. Mesure sur les deps
# reellement utilisees, pas sur une declaration.
QT_VER="$(sed -n 's/.*set(PACKAGE_VERSION "\([0-9.]*\)").*/\1/p' \
  "$DEPS_DIR/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" 2>/dev/null | head -1)"
[ -n "$QT_VER" ] || die "version de Qt illisible dans $DEPS_DIR"
QT_MM="${QT_VER%.*}"

echo "[INFO] Plancher declare  : OBS $FLOOR_OBS / Qt $FLOOR_QT"
echo "[INFO] SDK libobs        : $SDK_MAJOR.$SDK_MINOR   ($OBS_SRC)"
echo "[INFO] Qt de compilation : $QT_VER   ($DEPS_DIR)"
echo "[INFO] Xcode             : ${DEVELOPER_DIR:-Command Line Tools}"
if [ -n "$CODESIGN_IDENTITY" ]; then
  echo "[INFO] Signature         : $CODESIGN_IDENTITY"
else
  echo "[INFO] Signature         : ad-hoc (aucune identite Developer ID Application"
  echo "                           dans le trousseau — le module se chargera ici,"
  echo "                           mais Gatekeeper le bloquera ailleurs)"
fi
echo

if [ "$SDK_MAJOR.$SDK_MINOR" != "$FLOOR_OBS" ]; then
  die "Le SDK libobs est en $SDK_MAJOR.$SDK_MINOR, le plancher annonce est $FLOOR_OBS.
         Compiler ainsi exclut silencieusement les OBS plus anciens : l'echec
         n'apparait que chez l'utilisateur, en module qui ne charge pas.
         Soit remets OBS_SRC sur un arbre $FLOOR_OBS, soit assume le nouveau
         plancher (FLOOR_OBS ici ET dans les deux autres scripts, le README
         et l'installeur)."
fi

if [ "$QT_MM" != "$FLOOR_QT" ]; then
  die "Qt de compilation $QT_VER, plancher annonce $FLOOR_QT.
         C'est le piege decrit en tete de ce script : un module lie a Qt $QT_MM
         ne se charge pas dans un OBS qui livre un Qt plus ancien, et OBS change
         de Qt a l'interieur de la ligne 32.x (32.0 -> 6.8.3, 32.2 -> 6.11.1).
         Le mac de developpement ne verra rien, les clients en OBS 32.0/32.1 si.
         Utilise les deps de l'arbre $FLOOR_OBS."
fi

# === CONFIGURATION ===
rm -rf "$BUILD"
echo "[INFO] Configuration CMake..."
CMAKE_ARGS=(
  -S "$ROOT" -B "$BUILD" -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  # Universel obligatoire : OBS distribue un paquet Apple Silicon ET un paquet
  # Intel, et un module arm64 seul ne se charge pas dans l'OBS x86_64 (ni
  # l'inverse). Verifie apres coup au lipo.
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
  # Aligne sur OBS lui-meme (cmake/macos/defaults.cmake du meme arbre).
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
  -DCMAKE_PREFIX_PATH="$DEPS_DIR;${OBS_BASE_DEPS:+$OBS_BASE_DEPS;}$OBS_BUILD/libobs;$OBS_BUILD/frontend/api"
  # libobsConfig.cmake fait find_dependency(SIMDe), et le finder correspondant
  # vit dans l'arbre OBS, pas dans les deps. Les deux autres scripts font
  # exactement pareil.
  -DCMAKE_MODULE_PATH="$OBS_SRC/cmake/finders"
  -DSEMIRPROD_CODESIGN_IDENTITY="$CODESIGN_IDENTITY"
  # Edition publique, explicitement : ce script ne produit que le binaire du
  # poste. L'edition cloud se compile sous Linux (packaging/cloud).
  -DSEMIRPROD_EDITION=public
)
if [ "$SEMIRPROD_DEV_BUILD_FLAG" = "1" ]; then
  echo "[INFO] SEMIRPROD_DEV_BUILD actif — les env vars SEMIRPROD_*_BASE seront respectees."
  CMAKE_ARGS+=(-DSEMIRPROD_DEV_BUILD=ON)
fi
if [ "$SEMIRPROD_COLLECTIONS_FLAG" = "1" ]; then
  echo "[INFO] COLLECTIONS actives — l'onglet sera visible dans l'interface."
  CMAKE_ARGS+=(-DSEMIRPROD_COLLECTIONS=ON)
else
  echo "[INFO] COLLECTIONS desactivees — onglet absent de l'interface."
  CMAKE_ARGS+=(-DSEMIRPROD_COLLECTIONS=OFF)
fi

cmake "${CMAKE_ARGS[@]}" || die "configuration CMake echouee."

echo "[INFO] Compilation..."
cmake --build "$BUILD" || die "compilation echouee."

BUNDLE="$BUILD/$BUNDLE_NAME"
[ -d "$BUNDLE" ] || die "$BUNDLE_NAME introuvable apres compilation."

# === VERIFICATIONS DE L'ARTEFACT ===
#
# Sur le fichier reellement produit, jamais sur l'intention : c'est ce qui
# distingue un garde-fou d'un commentaire.
BIN="$BUNDLE/Contents/MacOS/$MODULE_NAME"
ARCHS="$(lipo -archs "$BIN" 2>/dev/null)"
case "$ARCHS" in
  *arm64*x86_64*|*x86_64*arm64*) : ;;
  *) die "le module n'est pas universel (lipo : '$ARCHS'). Il ne chargera que
         dans l'un des deux OBS macOS." ;;
esac

# Le backend TLS : sans lui le plugin se charge, s'affiche, et n'atteint aucun
# serveur. CMake echoue deja si le fichier source manque, on verifie ici qu'il
# est bien ARRIVE dans le bundle.
TLS="$BUNDLE/Contents/PlugIns/tls/libqsecuretransportbackend.dylib"
[ -f "$TLS" ] || die "backend TLS absent du bundle — le plugin serait muet
         (« TLS initialization failed » sur chaque requete)."

# Aucun chemin de la machine de compilation ne doit subsister : c'est ce qui
# rend le paquet installable ailleurs.
if otool -l "$BIN" | grep -A2 LC_RPATH | grep -q "$HOME"; then
  die "le module porte encore un rpath absolu vers $HOME — il ne trouvera ni Qt
         ni libobs chez le client. Verifie BUILD_WITH_INSTALL_RPATH."
fi

codesign --verify --strict "$BUNDLE" \
  || die "signature invalide — macOS refusera de charger le module."

echo
echo "[OK] $BUNDLE_NAME construit."
echo "     arches   : $ARCHS"
echo "     signature: $(codesign -dv "$BUNDLE" 2>&1 | sed -n 's/^Signature=//p')"

# === INSTALLATION LOCALE ===
if [ -n "$OBS_USER_PLUGINS" ]; then
  mkdir -p "$OBS_USER_PLUGINS"
  rm -rf "${OBS_USER_PLUGINS:?}/$BUNDLE_NAME"
  # ditto et non cp : preserve les attributs etendus et la signature du bundle.
  ditto "$BUNDLE" "$OBS_USER_PLUGINS/$BUNDLE_NAME" \
    && echo "[OK] installe dans $OBS_USER_PLUGINS"
fi

# === PAQUET ===
mkdir -p "$DIST"
if [ -x "$ROOT/packaging/macos/build_pkg.sh" ]; then
  CODESIGN_IDENTITY="$CODESIGN_IDENTITY" \
  BUNDLE_PATH="$BUNDLE" DIST_DIR="$DIST" "$ROOT/packaging/macos/build_pkg.sh" \
    || die "empaquetage echoue."
else
  echo "[INFO] packaging/macos/build_pkg.sh absent — pas de .pkg produit."
fi

# === CE QU'IL FAUT PUBLIER, ET SON EMPREINTE ===
#
# La version vient du bundle, qui la tient de project(VERSION) dans CMakeLists :
# une seule source, aucun numero recopie a la main.
#
# Le SHA-256 affiche ici ne se DECLARE pas au serveur — il le recalcule au
# depot, puis le signe avec le reste du manifeste. Il sert a verifier qu'on a
# publie le fichier qu'on vient de construire, et c'est exactement ce qui
# manquait le jour ou deux binaires differents ont circule sous le meme numero.
VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$BUNDLE/Contents/Info.plist" 2>/dev/null)"
PKG="$DIST/semirprod-plugin-${VERSION}.pkg"

echo
echo "====== A PUBLIER (admin > OBS plugin > version ${VERSION:-?}) ======"
if [ -f "$PKG" ]; then
  echo "[installer/macos] $PKG"
  echo "    sha256 : $(shasum -a 256 "$PKG" | cut -d" " -f1)"
else
  echo "[installer/macos] AUCUN paquet — sans .pkg, rien a publier pour macOS,"
  echo "                  et l'updater des postes Mac prendra un 404 a chaque"
  echo "                  passage (toutes les 6 h)."
fi

# Le reste de la chaine ne depend pas de ce script, mais son absence ne se voit
# QUE dans le journal d'OBS d'un client — autant le rappeler ici.
cat <<'RAPPEL'

[RAPPEL] Publier ne suffit pas :
  * le manifeste doit etre SIGNE cote serveur (cle de signature des mises a
    jour), sinon le plugin refuse le manifeste et personne n'est jamais averti ;
  * un paquet macOS se publie avec platform=macos. Sans lui, /api/plugin/latest
    ?platform=macos repond 404 meme si la version Windows est a jour.
  Format du manifeste : src/update/UpdateChecker.hpp.
RAPPEL

echo
echo "[TERMINE]"
