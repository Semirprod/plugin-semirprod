#!/usr/bin/env bash
#
# Equivalent Linux de build_plugin.bat. Les deux scripts font le meme travail
# — valider, garde-fou, compiler, ranger, empaqueter — mais le garde-fou n'a
# PAS le meme sens sur les deux plateformes, et c'est le point important.
#
# Sur Windows on CHOISIT le Qt de compilation (6.8.3, celui d'OBS 32.0), et le
# garde-fou verifie qu'on n'a pas derive. Sous Linux ce choix n'existe pas : le
# plugin est charge dans un OBS compile par la distribution, contre le Qt de la
# distribution. Exiger une version precise ici n'aurait aucun sens ; ce qu'il
# faut verifier, c'est que le Qt avec lequel on compile est bien CELUI que
# l'OBS cible utilise. Un plugin Qt 6.8 dans un OBS Qt 6.4 ne se charge pas.
#
set -uo pipefail

# === CONFIG MODIFIABLE ===
#
# Arbre OBS compile, d'ou viennent libobs et l'API frontend. Meme role que
# OBS_SRC/OBS_BUILD dans le .bat.
OBS_SRC="${OBS_SRC:-$HOME/obs-studio}"
OBS_BUILD="${OBS_BUILD:-$OBS_SRC/build}"

# Plancher declare. Doit rester d'accord avec build_plugin.bat, le README et
# l'installeur : c'est la version minimale d'OBS que le binaire accepte.
FLOOR_OBS="${FLOOR_OBS:-32.0}"

# Ou deposer le plugin pour un test immediat. Laisse vide pour desactiver.
# Disposition per-utilisateur d'OBS sous Linux :
#   ~/.config/obs-studio/plugins/<nom>/bin/64bit/<nom>.so
OBS_USER_PLUGINS="${OBS_USER_PLUGINS:-$HOME/.config/obs-studio/plugins}"

MODULE_NAME="semirprod-studio"

# === DEV BUILD ===
# Meme semantique que le .bat : 1 = les variables SEMIRPROD_*_BASE sont
# respectees (staging), sinon les URLs sont figees (production).
# En shell, `VAR=1 ./build_plugin.sh` n'a pas le piege des espaces de cmd,
# mais on nettoie quand meme par symetrie avec le .bat.
SEMIRPROD_DEV_BUILD_FLAG="${SEMIRPROD_DEV_BUILD_FLAG:-0}"
SEMIRPROD_DEV_BUILD_FLAG="${SEMIRPROD_DEV_BUILD_FLAG// /}"

# === EDITION : public (defaut) ou cloud ===
# public = le plugin du poste du streamer. cloud = l'OBS infogere : UI d'OBS
# verrouillee (Reglages/Quitter masques et desactives), appairage automatique,
# QtKeychain ecarte. cloud est pour les instances en conteneur, JAMAIS pour le
# binaire de bureau que tu distribues. Voir SEMIRPROD_EDITION dans CMakeLists.
#     ./build_plugin.sh --edition cloud
#
# SEMIRPROD_KIOSK_FLAG=1 reste accepte, comme alias de --edition cloud, pour
# les scripts et images qui le posent encore.
SEMIRPROD_EDITION="${SEMIRPROD_EDITION:-public}"
SEMIRPROD_KIOSK_FLAG="${SEMIRPROD_KIOSK_FLAG:-0}"
SEMIRPROD_KIOSK_FLAG="${SEMIRPROD_KIOSK_FLAG// /}"
[ "$SEMIRPROD_KIOSK_FLAG" = "1" ] && SEMIRPROD_EDITION=cloud
while [ $# -gt 0 ]; do
  case "$1" in
    --edition) [ $# -ge 2 ] || { echo "[ERREUR] --edition attend public ou cloud" >&2; exit 1; }
               SEMIRPROD_EDITION="$2"; shift 2 ;;
    --edition=*) SEMIRPROD_EDITION="${1#--edition=}"; shift ;;
    *) echo "[ERREUR] argument inconnu : $1 (attendu : --edition public|cloud)" >&2; exit 1 ;;
  esac
done
case "$SEMIRPROD_EDITION" in
  public|cloud) ;;
  *) echo "[ERREUR] edition inconnue : '$SEMIRPROD_EDITION' (public ou cloud)" >&2; exit 1 ;;
esac
# L'alias et l'option disent la meme chose ; un melange contradictoire
# (SEMIRPROD_KIOSK_FLAG=1 --edition public) est refuse plutot que tranche en
# silence : un cloud livre sur un poste ou l'inverse ne se voit qu'a l'usage.
if [ "$SEMIRPROD_KIOSK_FLAG" = "1" ] && [ "$SEMIRPROD_EDITION" != "cloud" ]; then
  echo "[ERREUR] SEMIRPROD_KIOSK_FLAG=1 contredit --edition $SEMIRPROD_EDITION" >&2
  exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build-linux"
DIST="$ROOT/dist-linux"

die() { echo "[ERREUR] $*" >&2; exit 1; }

echo
echo "========================================"
echo "  Build plugin Linux (module $MODULE_NAME)"
echo "========================================"
echo

# === VALIDATION DEPENDANCES ===
[ -f "$OBS_BUILD/libobs/libobsConfig.cmake" ] \
  || die "libobsConfig.cmake introuvable dans $OBS_BUILD/libobs.
         Compile obs-studio d'abord, ou pointe OBS_SRC ailleurs."
[ -f "$OBS_BUILD/frontend/api/obs-frontend-apiConfig.cmake" ] \
  || die "obs-frontend-apiConfig.cmake introuvable dans $OBS_BUILD/frontend/api.
         Les trois docks en dependent : sans lui, rien a faire."

command -v cmake >/dev/null || die "cmake absent."
command -v ninja >/dev/null || die "ninja absent (apt install ninja-build)."

# === GARDE-FOU DE COMPATIBILITE ===
# Meme raison d'etre que dans le .bat : un plancher qui monte par accident ne
# se voit PAS a la compilation, seulement chez l'utilisateur, en refus de
# chargement.
SDK_MAJOR="$(grep -oP 'define LIBOBS_API_MAJOR_VER\s+\K[0-9]+' "$OBS_SRC/libobs/obs-config.h" || true)"
SDK_MINOR="$(grep -oP 'define LIBOBS_API_MINOR_VER\s+\K[0-9]+' "$OBS_SRC/libobs/obs-config.h" || true)"
[ -n "$SDK_MAJOR" ] && [ -n "$SDK_MINOR" ] || die "version d'API libobs illisible dans $OBS_SRC/libobs/obs-config.h"

QT_VER="$(pkg-config --modversion Qt6Core 2>/dev/null || echo inconnue)"

echo "[INFO] Plancher declare  : OBS $FLOOR_OBS"
echo "[INFO] SDK libobs        : $SDK_MAJOR.$SDK_MINOR   ($OBS_SRC)"
echo "[INFO] Qt de compilation : $QT_VER   (celui de la distribution)"

if [ "$SDK_MAJOR.$SDK_MINOR" != "$FLOOR_OBS" ]; then
  die "Le SDK libobs est en $SDK_MAJOR.$SDK_MINOR, le plancher annonce est $FLOOR_OBS.
         libobs refuse tout module compile contre une libobs plus RECENTE que la
         sienne. Compiler ainsi exclut silencieusement les OBS plus anciens :
         l'echec n'apparait que chez l'utilisateur.
         Soit remets $OBS_SRC sur un arbre $FLOOR_OBS, soit assume le nouveau
         plancher (FLOOR_OBS ici ET dans build_plugin.bat, le README, l'installeur)."
fi

# Le Qt d'OBS et le notre doivent etre le MEME. On le verifie sur l'artefact
# reellement produit plutot que sur une declaration : on lit a quel Qt la
# bibliotheque frontend d'OBS est liee.
OBS_FRONTEND_SO="$(find "$OBS_BUILD" -name 'libobs-frontend-api.so*' -type f 2>/dev/null | head -1)"
if [ -n "$OBS_FRONTEND_SO" ] && command -v objdump >/dev/null; then
  OBS_QT_SONAME="$(objdump -p "$OBS_FRONTEND_SO" 2>/dev/null | grep -oP 'NEEDED\s+\KlibQt6Core\.so\.[0-9.]+' | head -1)"
  if [ -n "$OBS_QT_SONAME" ]; then
    echo "[INFO] Qt attendu par OBS : $OBS_QT_SONAME"
    # Compare les majeur.mineur, pas le patch : Qt garantit la compat binaire
    # sur les patchs, pas entre mineures.
    obs_mm="$(echo "$OBS_QT_SONAME" | grep -oP '6\.[0-9]+' | head -1)"
    our_mm="$(echo "$QT_VER" | grep -oP '^6\.[0-9]+' || true)"
    if [ -n "$obs_mm" ] && [ -n "$our_mm" ] && [ "$obs_mm" != "$our_mm" ]; then
      die "OBS est lie a Qt $obs_mm, on compilerait avec Qt $our_mm.
         Le module ne se chargera pas (imports Qt non resolus). C'est la
         panne d'un DLL Windows compile contre un Qt plus recent, transposee
         a Linux : ici on ne choisit pas le Qt, on s'aligne sur celui d'OBS."
    fi
  fi
fi

# === CONFIGURATION ===
rm -rf "$BUILD"
echo "[INFO] Configuration CMake..."
CMAKE_ARGS=(
  -S "$ROOT" -B "$BUILD" -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -Dlibobs_DIR="$OBS_BUILD/libobs"
  -Dobs-frontend-api_DIR="$OBS_BUILD/frontend/api"
  -DCMAKE_PREFIX_PATH="$OBS_BUILD/libobs;$OBS_BUILD/frontend/api"
  # libobsConfig.cmake fait find_dependency(SIMDe), et le finder correspondant
  # n'est PAS installe par libsimde-dev : il vit dans l'arbre OBS. Sans ce
  # chemin, la configuration echoue sur "Could not find SIMDe" alors que la
  # bibliotheque est bien la. Le .bat fait exactement pareil.
  -DCMAKE_MODULE_PATH="$OBS_SRC/cmake/finders"
)
if [ "$SEMIRPROD_DEV_BUILD_FLAG" = "1" ]; then
  echo "[INFO] SEMIRPROD_DEV_BUILD actif — les env vars SEMIRPROD_*_BASE seront respectees."
  CMAKE_ARGS+=(-DSEMIRPROD_DEV_BUILD=ON)
fi
echo "[INFO] Edition : $SEMIRPROD_EDITION"
CMAKE_ARGS+=(-DSEMIRPROD_EDITION="$SEMIRPROD_EDITION")
if [ "$SEMIRPROD_EDITION" = "cloud" ]; then
  echo "[INFO] CLOUD actif — Reglages/Quitter verrouilles (les docks Semirprod restent)."
  # QtKeychain est ecarte volontairement pour une instance sans bureau.
  #
  # Il ecrit dans le trousseau via le Secret Service de D-Bus. Dans un
  # conteneur il n'y a ni gnome-keyring ni session D-Bus : l'ecriture echoue a
  # CHAQUE fois, et le jeton n'est jamais persiste — il faut re-appairer a
  # chaque demarrage d'OBS.
  #
  # Sans QtKeychain, TokenStore utilise son repli fichier, qui lui fonctionne :
  # chiffre, permissions 0600, ecrit dans le profil. C'est le bon support pour
  # une machine sans utilisateur devant l'ecran.
  CMAKE_ARGS+=(-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Keychain=ON)
fi
cmake "${CMAKE_ARGS[@]}" || die "Echec configuration CMake"

echo "[INFO] Compilation..."
cmake --build "$BUILD" --parallel "$(nproc)" || die "Echec compilation"

SO_PATH="$BUILD/$MODULE_NAME.so"
[ -f "$SO_PATH" ] || SO_PATH="$(find "$BUILD" -name "$MODULE_NAME.so" -type f | head -1)"
[ -f "$SO_PATH" ] || die "$MODULE_NAME.so introuvable dans $BUILD"
echo "[SUCCES] module : $SO_PATH"

# === CONTROLE DE CE QU'ON VIENT DE PRODUIRE ===
# Un .so qui compile mais dont une dependance manque ne se voit qu'au
# chargement, dans OBS, sous la forme d'un "module not loaded" sans detail.
# ldd le dit tout de suite. Equivalent Linux de la sonde LoadLibrary.
if command -v ldd >/dev/null; then
  MISSING="$(ldd "$SO_PATH" 2>/dev/null | grep -c 'not found' || true)"
  if [ "${MISSING:-0}" -gt 0 ]; then
    echo "[ERREUR] $MISSING dependance(s) non resolue(s) :"
    ldd "$SO_PATH" | grep 'not found'
    die "Le module ne se chargerait pas dans OBS."
  fi
  echo "[INFO] Toutes les dependances dynamiques sont resolues."
fi

# === DIST ===
# Disposition per-utilisateur d'OBS sous Linux. Le zip Windows ne contient que
# la DLL, pour la meme raison qu'ici on ne met que le .so et les donnees qui
# nous appartiennent : un paquet qui deborde ecrase des fichiers d'OBS.
rm -rf "$DIST"
mkdir -p "$DIST/$MODULE_NAME/bin/64bit" "$DIST/$MODULE_NAME/data" || die "creation dist"
cp "$SO_PATH" "$DIST/$MODULE_NAME/bin/64bit/" || die "copie du module"
[ -f "$ROOT/resources/fallback.png" ] && cp "$ROOT/resources/fallback.png" "$DIST/$MODULE_NAME/data/"
# Textes localises : data/locale/<langue>.ini, lus par obs_module_text() dans la
# langue d'OBS. Sans eux l'ecran affiche des cles.
mkdir -p "$DIST/$MODULE_NAME/data/locale" && cp "$ROOT"/data/locale/*.ini "$DIST/$MODULE_NAME/data/locale/" || die "copie des textes localises"
# Licence : la GPL impose que le texte et l'adresse du code source accompagnent
# chaque copie distribuee. Une archive sans eux n'est pas publiable.
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$DIST/$MODULE_NAME/data/" || die "copie de la licence"
cp "$ROOT/terms.txt" "$DIST/$MODULE_NAME/data/NOTICE.txt" || die "copie de la notice"
# Rappel : l'interface web est EMBARQUEE dans le binaire (WebUiAsset.gen.cpp),
# il n'y a donc aucun fichier d'UI a deployer.

# === COPIE POUR TEST IMMEDIAT ===
if [ -n "$OBS_USER_PLUGINS" ]; then
  # Ni dev ni cloud ne s'installent automatiquement sur le poste :
  #   - dev pointe sur staging et ecraserait le plugin de production
  #   - cloud VERROUILLERAIT l'OBS de bureau (plus de Reglages, plus de docks),
  #     et le symptome ne se voit qu'a l'usage
  if [ "$SEMIRPROD_DEV_BUILD_FLAG" = "1" ] || [ "$SEMIRPROD_EDITION" = "cloud" ]; then
    echo "[INFO] Build dev/cloud : PAS de copie automatique (deploiement a la main)."
  else
    mkdir -p "$OBS_USER_PLUGINS" \
      && cp -r "$DIST/$MODULE_NAME" "$OBS_USER_PLUGINS/" \
      && echo "[SUCCES] installe dans $OBS_USER_PLUGINS/$MODULE_NAME"
  fi
fi

# === VERSION ET ARCHIVE ===
PLUGIN_VERSION="$(cat "$BUILD/PLUGIN_VERSION.txt" 2>/dev/null | tr -d ' \r\n')"
[ -n "$PLUGIN_VERSION" ] || { echo "[WARN] version illisible — fallback 'unknown'"; PLUGIN_VERSION=unknown; }

# Le nom DIT quelle variante on tient. Trois binaires differents porteraient
# sinon le meme nom, et on deploie un kiosque sur un poste de bureau (ou
# l'inverse) sans qu'aucun message ne previenne : le verrouillage ne se voit
# qu'a l'usage, quand l'operateur ne trouve plus ses Reglages.
# Vecu : le .so kiosque a ecrase le .so bureau dans dist-linux/.
# Le suffixe de l'edition cloud reste « -KIOSK » : c'est le nom que cherchent
# deja les habitudes et les notes de deploiement.
SUFFIX=""
[ "$SEMIRPROD_EDITION" = "cloud" ] && SUFFIX="-KIOSK"
[ "$SEMIRPROD_DEV_BUILD_FLAG" = "1" ] && SUFFIX="$SUFFIX-DEV-staging"
TAR_NAME="obs-semirprod-plugin-v$PLUGIN_VERSION-linux-x86_64$SUFFIX.tar.gz"
rm -f "$ROOT/$TAR_NAME"
tar -C "$DIST" -czf "$ROOT/$TAR_NAME" "$MODULE_NAME" || die "creation de l'archive"
echo "[SUCCES] archive : $ROOT/$TAR_NAME"

echo
echo "========================================"
echo "  Build termine (v$PLUGIN_VERSION)"
echo "  Module  : $DIST/$MODULE_NAME/bin/64bit/$MODULE_NAME.so"
echo "  Archive : $TAR_NAME"
echo "  A extraire dans ~/.config/obs-studio/plugins/"
echo "========================================"
echo
