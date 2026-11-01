cmake_minimum_required(VERSION 3.28)

project(obs-semirprod-plugin
  VERSION 0.2.4
  LANGUAGES CXX
  DESCRIPTION "Semirprod for OBS — OBS plugin for live stream automation"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(POLICY CMP0177)
  cmake_policy(SET CMP0177 NEW)
endif()

option(ENABLE_FRONTEND_API "Enable OBS frontend API integration" ON)
option(ENABLE_QT "Enable Qt-based dock UI" ON)

# ============================================================================
# Production vs. dev build
#
# A release build (the default and the only thing we ship) hardcodes the
# production endpoints in Config.cpp and IGNORES every SEMIRPROD_*_BASE env
# var — the plugin is fully tied to semirprod.fr infra.
#
# Set SEMIRPROD_DEV_BUILD=ON (or build type Debug) to enable env-var overrides
# for local development against staging. The host allowlist in Config.cpp
# STILL applies, so a dev build can't be repointed at an unrelated host even
# by accident.
# ============================================================================
# WARNING — do not put a generator expression in the default value here.
#
# `option()` runs at CONFIGURE time; generator expressions are only evaluated at
# GENERATE time. The previous default was:
#
#     option(SEMIRPROD_DEV_BUILD "..." "$<STREQUAL:${CMAKE_BUILD_TYPE},Debug>")
#
# which stored the literal string `$<STREQUAL:RelWithDebInfo,Debug>` in the
# cache. `if()` treats any string that is not a false constant as TRUE, so
# SEMIRPROD_DEV_BUILD was ON for EVERY build type, including the RelWithDebInfo
# builds we ship. That re-enabled the SEMIRPROD_*_BASE env-var override path in
# release binaries handed to clients, leaving the Config.cpp host allowlist as
# the only remaining guard.
#
# Explicit configure-time logic instead: default ON only for a Debug build.
if(NOT DEFINED SEMIRPROD_DEV_BUILD)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SEMIRPROD_DEV_BUILD_DEFAULT ON)
  else()
    set(SEMIRPROD_DEV_BUILD_DEFAULT OFF)
  endif()
else()
  set(SEMIRPROD_DEV_BUILD_DEFAULT ${SEMIRPROD_DEV_BUILD})
endif()
option(SEMIRPROD_DEV_BUILD "Enable env-var endpoint overrides for staging/dev"
       ${SEMIRPROD_DEV_BUILD_DEFAULT})

# Fail loudly rather than ship a client binary with the override path compiled in.
if(SEMIRPROD_DEV_BUILD AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(WARNING
    "SEMIRPROD_DEV_BUILD=ON with CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. "
    "The SEMIRPROD_*_BASE env-var override path WILL be compiled in. "
    "Never ship this binary to clients.")
endif()
message(STATUS "SEMIRPROD_DEV_BUILD=${SEMIRPROD_DEV_BUILD} (CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})")

# ============================================================================
# EDITION : « public » (defaut) ou « cloud ».
#
#   public  le plugin que le streamer installe sur son poste (Windows, macOS,
#           Linux). C'est le seul binaire distribue.
#   cloud   l'OBS infogere : il tourne en kiosque dans un conteneur, pilote a
#           distance, et personne ne doit atteindre ses reglages ni le quitter.
#           Appairage automatique au demarrage, adresses lues dans
#           l'environnement, QtKeychain ecarte.
#
# Le code partage demande a src/Edition.hpp ce que l'edition veut ; chaque
# edition y repond depuis ses propres fichiers (src/public/, src/cloud/), et
# CMake ne compile que ceux de l'edition choisie (voir plus bas, apres
# add_library : une cible doit exister avant qu'on lui ajoute des sources).
# Pas de #ifdef d'edition dans le code partage.
#
# Compile-time et non runtime, volontairement : dans un binaire public le
# code de verrouillage et d'appairage n'existe pas du tout, donc aucune
# variable d'environnement ni reglage ne peut l'activer par accident.
# ============================================================================
set(SEMIRPROD_EDITION "public" CACHE STRING
    "Edition du plugin : public (poste du streamer) ou cloud (OBS infogere)")
set_property(CACHE SEMIRPROD_EDITION PROPERTY STRINGS public cloud)

# ANCIEN NOM, garde comme alias pour les scripts qui le passent encore :
# -DSEMIRPROD_KIOSK_BUILD=ON vaut -DSEMIRPROD_EDITION=cloud.
#
# Variable NORMALE et non entree de cache forcee : elle masque la valeur du
# cache pour cette configuration seulement. Repasser l'alias a OFF sur le meme
# repertoire de build rend donc l'edition du cache, comme avant ou OFF voulait
# dire « bureau », au lieu de laisser un « cloud » fige en silence.
option(SEMIRPROD_KIOSK_BUILD
       "OBSOLETE : alias de SEMIRPROD_EDITION=cloud" OFF)
if(SEMIRPROD_KIOSK_BUILD)
  message(WARNING
    "SEMIRPROD_KIOSK_BUILD est obsolete : utilise -DSEMIRPROD_EDITION=cloud. "
    "Edition cloud retenue.")
  set(SEMIRPROD_EDITION "cloud")
endif()

if(NOT SEMIRPROD_EDITION STREQUAL "public" AND NOT SEMIRPROD_EDITION STREQUAL "cloud")
  message(FATAL_ERROR
    "SEMIRPROD_EDITION='${SEMIRPROD_EDITION}' inconnue : public ou cloud.")
endif()
message(STATUS "[semirprod] EDITION=${SEMIRPROD_EDITION}")

# Sauvegarde / import de collections. ON par defaut : c'est l'etat vise.
#
# A passer a OFF pour un binaire destine a un backend qui n'a pas encore les
# routes `/api/obs/scene-collections`. Sans ca l'onglet s'affiche, l'operateur
# clique, et le plugin collectionne des 404 sans que rien n'explique pourquoi —
# une fonction visible qui ne peut pas marcher coute plus cher qu'une fonction
# absente.
#
# Option DISTINCTE de SEMIRPROD_DEV_BUILD, et il faut qu'elle le reste : ce
# dernier veut dire « les surcharges d'adresses par variables d'environnement
# sont compilees », pas « la prod est en retard sur cette fonction ». Les
# confondre obligerait a les demeler le jour ou la prod aura les routes ; ainsi,
# ce jour-la, c'est ce seul drapeau qui bascule et aucune ligne de code.
#
# Ce que OFF retire : le champ `backups` de l'etat envoye a l'interface, et les
# trois commandes du pont. L'interface masque son onglet quand le champ manque,
# donc UN SEUL bundle WebUI sert les deux variantes. Le code de `src/backup/`
# reste compile dans le binaire, mais sans aucun point d'entree.
option(SEMIRPROD_COLLECTIONS
       "Exposer la sauvegarde/import de collections (backend requis)" ON)

# Identite de signature macOS. Vide = signature ad-hoc (`codesign -s -`), ce qui
# suffit a CHARGER le plugin sur la machine qui l'a compile mais ne passe pas
# Gatekeeper ailleurs. Renseigner une identite « Developer ID Application: ... »
# pour un binaire distribuable, prealable obligatoire a la notarisation.
set(SEMIRPROD_CODESIGN_IDENTITY "" CACHE STRING
    "Identite codesign macOS (vide = ad-hoc)")

# NOTE: intentionally NO WebSockets component. The plugin implements RFC 6455
# on top of Qt6::Network (QSslSocket / QTcpServer) — modules OBS actually ships
# — so it never bundles Qt6WebSockets.dll, which pinned us to one exact Qt
# version and broke on every OBS Qt bump. See src/ws/WsFraming.hpp.
# PLANCHER QT : 6.4, dicte par Linux et non par un besoin d'API.
#
# Sur Windows on choisit le Qt de compilation (6.8.3, celui d'OBS 32.0). Sous
# Linux ce choix n'existe pas : le plugin DOIT utiliser le Qt de l'OBS qui le
# charge, donc celui de la distribution — Ubuntu 24.04 LTS livre 6.4.2. Exiger
# 6.5 ici rendait le plugin inconstructible sur la LTS courante, sans qu'aucune
# ligne de code n'en ait besoin.
#
# Ce plancher n'est pas declaratif : la compilation Linux tourne SUR 6.4.2,
# donc toute utilisation d'une API 6.5+ casse le build au lieu de casser chez
# l'utilisateur. C'est ce qui rend le chiffre honnete.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

# QtKeychain, SAUF en edition cloud, et ce n'est pas un detail de confort.
#
# TokenStore utilise le trousseau EXCLUSIVEMENT quand HAVE_QTKEYCHAIN est
# defini : il n'y a AUCUN repli sur fichier dans ce cas (voir TokenStore.cpp,
# save() et load()). Or une instance infogeree tourne dans un conteneur sans
# session de bureau et sans D-Bus — le journal du conteneur le dit a chaque
# demarrage : « Could not create dbus connection: Cannot spawn a message bus
# without a machine-id ».
#
# Consequence si on laisse le trousseau actif en cloud : l'appairage
# automatique reussit cote reseau, TokenStore::save() echoue a persister, et
# personne ne s'en apercoit — cached_ est bien rempli, la session en cours
# fonctionne, et tout disparait au redemarrage suivant du conteneur. Le client
# retrouve une instance non authentifiee sans qu'aucun message ne l'explique.
#
# L'image installe libqt6keychain1 (elle en a besoin pour les dependances
# d'OBS), donc find_package la TROUVE : sans cette condition, le piege se
# referme tout seul a la premiere compilation Linux.
if(SEMIRPROD_EDITION STREQUAL "public")
  find_package(Qt6Keychain QUIET)
else()
  # Ne pas appeler find_package NE SUFFIT PAS a garantir Qt6Keychain_FOUND
  # faux : Qt6Keychain_DIR est une entree de CACHE, et Qt6Keychain_FOUND peut
  # deja valoir vrai dans un repertoire de build reconfigure (premiere
  # configuration en public, puis -DSEMIRPROD_EDITION=cloud par-dessus).
  # Le piege se refermerait alors en silence : HAVE_QTKEYCHAIN redevient
  # defini, TokenStore repart sur le trousseau, il n'y a pas de D-Bus dans le
  # conteneur, la persistance echoue sans que rien ne casse a l'ecran, et
  # l'instance se reveille non appairee au redemarrage suivant.
  #
  # On force donc la valeur et on invalide l'entree de cache : la garantie ne
  # depend plus du comportement de find_package sur un build deja configure.
  unset(Qt6Keychain_DIR CACHE)
  unset(Qt6Keychain_FOUND CACHE)
  set(Qt6Keychain_FOUND FALSE)
  message(STATUS "[semirprod] EDITION CLOUD — QtKeychain ecarte, jetons sur fichier")
endif()

add_library(${PROJECT_NAME} MODULE)

target_sources(${PROJECT_NAME} PRIVATE
  src/plugin.cpp
  src/Edition.hpp
  src/Config.hpp
  src/Config.cpp
  src/Settings.hpp
  src/Settings.cpp
  src/StreamRegistry.hpp
  src/StreamRegistry.cpp
  src/auth/DeviceAuthFlow.hpp
  src/auth/DeviceAuthFlow.cpp
  src/auth/JwtClaims.hpp
  src/auth/JwtClaims.cpp
  src/auth/TokenStore.hpp
  src/auth/TokenStore.cpp
  src/http/HttpClient.hpp
  src/http/HttpClient.cpp
  src/backup/ZipWriter.hpp
  src/backup/ZipWriter.cpp
  src/backup/ZipReader.hpp
  src/backup/ZipReader.cpp
  src/backup/CollectionImporter.hpp
  src/backup/CollectionImporter.cpp
  src/backup/CollectionPackager.hpp
  src/backup/CollectionPackager.cpp
  src/backup/BackupService.hpp
  src/backup/BackupService.cpp
  src/ui/SemirprodDock.hpp
  src/ui/SemirprodDock.cpp
  src/ui/SemirprodTheme.hpp
  src/ui/SemirprodTheme.cpp
  src/ui/StreamStatusDock.hpp
  src/ui/StreamStatusDock.cpp
  src/ui/StudioWindow.hpp
  src/ui/StudioWindow.cpp
  src/ui/EventLogDock.hpp
  src/ui/EventLogDock.cpp
  src/ui/WebUiServer.hpp
  src/ui/WebUiServer.cpp
  src/ui/WebUiAsset.hpp
  src/ui/WebUiWindow.hpp
  src/ui/WebUiWindow.cpp
  src/ws/WsFraming.hpp
  src/ws/WsFraming.cpp
  src/ws/WebSocketClient.hpp
  src/ws/WebSocketClient.cpp
  src/ws/LiveClient.hpp
  src/ws/LiveClient.cpp
  src/obs/SceneController.hpp
  src/obs/SceneController.cpp
  src/obs/VerticalBridge.hpp
  src/obs/VerticalBridge.cpp
  src/obs/MultistreamBridge.hpp
  src/obs/MultistreamBridge.cpp
  src/obs/ProgramPreviewOutput.hpp
  src/obs/ProgramPreviewOutput.cpp
  src/obs/AudioLevels.hpp
  src/obs/AudioLevels.cpp
  src/obs/PreviewSceneMix.hpp
  src/obs/PreviewSceneMix.cpp
  src/obs/SemirprodLiveSource.hpp
  src/obs/SemirprodLiveSource.cpp
  src/update/UpdateChecker.hpp
  src/update/UpdateChecker.cpp
  src/mac/QtTlsBackend.hpp
  src/mac/QtTlsBackend.cpp
  src/mac/StudioDockPolicy.hpp
  src/mac/CefHostView.hpp
  # Plus de resources.qrc : son unique lecteur etait le logo d'en-tete du dock
  # natif, retire avec son arbre de widgets. Les PNG restent a la racine du
  # depot (l'installateur s'en sert), ils ne sont juste plus embarques dans la
  # DLL. L'interface est celle de webui/, qui porte sa propre image de marque.
)

# Fichiers propres a l'edition : UNE implementation de src/Edition.hpp par
# edition, et le code qui n'a de sens que la. Rien de src/cloud/ n'entre dans un
# binaire public, rien de src/public/ dans un binaire cloud.
#
# La liste des fichiers cloud vit DANS src/cloud/sources.cmake : le depot public
# (tools/export_source.sh) ne contient pas src/cloud/, et ce fichier ne doit donc
# rien en citer. Un fichier d'edition qui prendrait un Q_OBJECT devra figurer
# dans sa liste, en-tete comprise, pour que moc le voie.
if(SEMIRPROD_EDITION STREQUAL "cloud")
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cloud/sources.cmake")
    message(FATAL_ERROR
      "Edition cloud absente de cette copie du code : seule l'edition public "
      "se compile ici.")
  endif()
  include("${CMAKE_CURRENT_SOURCE_DIR}/src/cloud/sources.cmake")
else()
  target_sources(${PROJECT_NAME} PRIVATE
    src/public/Edition.cpp
    src/public/PublicConfig.cpp
  )
endif()

# Objective-C++ : macOS uniquement.
#
# Le .mm parle a AppKit pour replacer la vue native du navigateur, ce
# qu'obs-browser ne fait pas sur cette plateforme (son resizeEvent y est vide).
# Il est ajoute ICI, sous garde, plutot que dans la liste commune : un binaire
# Windows ou Linux ne doit pas contenir une ligne de ce code, ni meme avoir a
# le compiler.
if(APPLE)
  enable_language(OBJCXX)
  target_sources(${PROJECT_NAME} PRIVATE src/mac/CefHostView.mm)
  set_source_files_properties(src/mac/CefHostView.mm PROPERTIES
    # ARC : le fichier ne retient ni ne relache rien (il ne fait que poser des
    # cadres sur des vues dont AppKit reste proprietaire), mais l'activer evite
    # d'avoir a y penser si le fichier grossit.
    COMPILE_OPTIONS "-fobjc-arc"
  )
  target_link_libraries(${PROJECT_NAME} PRIVATE "-framework AppKit")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE src)

# ----------------------------------------------------------------------------
# Embed the built web UI into the DLL.
#
# resources/ui/index.html (single-file React build) is turned into a byte array
# compiled straight into the plugin, so WebUiServer can serve it over HTTP with
# no external file. The custom command DEPENDS on the html, so any rebuild after
# `npm run build` re-embeds the fresh UI — the DLL and its UI can never drift
# (the old "copied the DLL but the data-dir index.html stayed stale" trap).
# ----------------------------------------------------------------------------
set(WEBUI_HTML "${CMAKE_CURRENT_SOURCE_DIR}/resources/ui/index.html")
set(WEBUI_ASSET_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/WebUiAsset.gen.cpp")
add_custom_command(
  OUTPUT "${WEBUI_ASSET_CPP}"
  COMMAND ${CMAKE_COMMAND}
          -DINPUT=${WEBUI_HTML}
          -DOUTPUT=${WEBUI_ASSET_CPP}
          -DVAR=kWebUiHtml
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
  DEPENDS "${WEBUI_HTML}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
  COMMENT "Embedding web UI into the plugin (${WEBUI_HTML})"
  VERBATIM
)
set_source_files_properties("${WEBUI_ASSET_CPP}" PROPERTIES
  GENERATED TRUE
  SKIP_AUTOMOC ON
  SKIP_AUTOGEN ON
)
target_sources(${PROJECT_NAME} PRIVATE "${WEBUI_ASSET_CPP}")

target_link_libraries(${PROJECT_NAME} PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::Network
  OBS::libobs
  OBS::obs-frontend-api
)

if(Qt6Keychain_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE Qt6Keychain::Qt6Keychain)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_QTKEYCHAIN)
endif()

# libsecret (Secret Service, Linux) : PAS de dependance a l'edition de liens.
# TokenStore la charge a l'execution (QLibrary, libsecret-1.so.0) : le module
# se charge partout, se sert du trousseau quand il est la, et revient au
# fichier 0600 sinon. Jamais en edition cloud (conteneur sans D-Bus), decide
# a l'execution par edition::isCloud(). Rien a installer sur la CI.

# DPAPI (CryptProtectData / CryptUnprotectData) for the TokenStore fallback
# lives in crypt32 on Windows.
#
# bcrypt = CNG, utilise par UpdateChecker pour verifier la signature ECDSA
# P-256 du manifeste de mise a jour. DLL systeme toujours presente : on
# n'embarque aucune bibliotheque de crypto tierce, volontairement (ce depot a
# deja paye le prix d'un binaire tiers embarque, cf. Qt6WebSockets).
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE crypt32 bcrypt)
elseif(APPLE)
  # macOS : Security.framework, et surtout PAS OpenSSL.
  #
  # Le bundle OBS macOS n'embarque aucun libcrypto — son TLS est mbedTLS. Un
  # find_package(OpenSSL) n'y trouverait donc que celui de Homebrew, et le
  # binaire produit porterait une dependance absolue vers /opt/homebrew/...
  # absente de toute machine cliente. Deux symptomes possibles, tous deux
  # mauvais : le module refuse de charger, ou l'updater meurt en silence.
  #
  # Security.framework est l'equivalent exact de bcrypt.dll : composant
  # SYSTEME, toujours present, jamais embarque. La regle du depot (aucune
  # bibliotheque de crypto tierce) tient donc sur les trois plateformes.
  #
  # Pas de garde `if(FOUND)` ici, volontairement : ces deux frameworks font
  # partie du systeme. S'ils manquaient, l'edition de liens echouerait, ce qui
  # est le bon mode d'echec — bruyant, a la compilation, et non silencieux chez
  # le client comme l'aurait ete un OpenSSL absent.
  target_link_libraries(${PROJECT_NAME} PRIVATE
    "-framework Security"
    "-framework CoreFoundation")
  message(STATUS "[semirprod] Security.framework — verification de signature des MAJ active")
else()
  # Hors Windows et macOS, libcrypto tient le role de CNG pour verifier la signature
  # ECDSA P-256 du manifeste. Meme nature que bcrypt.dll : une bibliotheque
  # SYSTEME, pas un binaire tiers embarque — la regle du depot est respectee.
  # OBS la charge deja de toute facon (backend TLS de Qt, librist, libsrt).
  find_package(OpenSSL QUIET COMPONENTS Crypto)
  if(OpenSSL_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::Crypto)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SEMIRPROD_HAVE_OPENSSL)
    message(STATUS "[semirprod] OpenSSL ${OPENSSL_VERSION} — verification de signature des MAJ active")
  else()
    # On n'echoue PAS la configuration : le plugin reste parfaitement
    # utilisable sans updater. Mais on le dit fort, parce que le symptome
    # (aucune mise a jour ne sera jamais proposee) est silencieux a l'usage.
    message(WARNING
      "OpenSSL introuvable : verifyEcdsaP256() refusera TOUT manifeste de mise "
      "a jour (echec ferme, jamais de contournement). Installe libssl-dev pour "
      "activer les mises a jour.")
  endif()
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
  SEMIRPROD_PLUGIN_VERSION="${PROJECT_VERSION}"
)

# Hotes supplementaires acceptes par un build de dev SEULEMENT (pre-production),
# separes par des virgules. Vide par defaut : un build sans cette option ne
# connait que les hotes de production. A defaut, la liste est lue dans le
# fichier optionnel tools/dev-hosts.txt (un hote par ligne, `#` = commentaire),
# qui ne fait pas partie de l'archive source publique.
set(SEMIRPROD_DEV_HOSTS "" CACHE STRING
    "Comma-separated extra hosts accepted by dev builds only (empty by default)")

if(SEMIRPROD_DEV_BUILD)
  target_compile_definitions(${PROJECT_NAME} PRIVATE SEMIRPROD_DEV_BUILD)
  message(STATUS "[semirprod] DEV BUILD — SEMIRPROD_* env vars will override URLs (host allowlist still enforced)")

  set(_semirprod_dev_hosts "${SEMIRPROD_DEV_HOSTS}")
  set(_semirprod_dev_hosts_file "${CMAKE_CURRENT_SOURCE_DIR}/tools/dev-hosts.txt")
  if(_semirprod_dev_hosts STREQUAL "" AND EXISTS "${_semirprod_dev_hosts_file}")
    file(STRINGS "${_semirprod_dev_hosts_file}" _semirprod_dev_lines)
    set(_semirprod_dev_list "")
    foreach(_line IN LISTS _semirprod_dev_lines)
      string(REPLACE "\r" "" _line "${_line}")
      string(STRIP "${_line}" _line)
      if(_line STREQUAL "" OR _line MATCHES "^#")
        continue()
      endif()
      list(APPEND _semirprod_dev_list "${_line}")
    endforeach()
    list(JOIN _semirprod_dev_list "," _semirprod_dev_hosts)
  endif()
  string(REPLACE ";" "," _semirprod_dev_hosts "${_semirprod_dev_hosts}")
  if(NOT _semirprod_dev_hosts STREQUAL "")
    # Le texte finit dans une chaine C : rien d'autre que des noms d'hote.
    if(NOT _semirprod_dev_hosts MATCHES "^[A-Za-z0-9.,-]+$")
      message(FATAL_ERROR "SEMIRPROD_DEV_HOSTS : noms d'hote invalides (${_semirprod_dev_hosts})")
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
      SEMIRPROD_DEV_HOSTS="${_semirprod_dev_hosts}")
    message(STATUS "[semirprod] DEV BUILD — extra hosts: ${_semirprod_dev_hosts}")
  endif()
else()
  message(STATUS "[semirprod] RELEASE BUILD — endpoints hardcoded, env vars ignored")
endif()

# Plus de SEMIRPROD_KIOSK : l'edition se choisit par les fichiers compiles (voir
# plus haut), pas par une macro lue dans le code partage.
if(SEMIRPROD_EDITION STREQUAL "cloud")
  message(STATUS "[semirprod] EDITION CLOUD — Reglages et Quitter masques ET desactives, appairage automatique")
endif()

if(SEMIRPROD_COLLECTIONS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE SEMIRPROD_COLLECTIONS)
  message(STATUS "[semirprod] COLLECTIONS — onglet expose (le backend doit avoir les routes)")
else()
  message(STATUS "[semirprod] COLLECTIONS OFF — champ `backups` et commandes du pont retires, onglet masque")
endif()

# Stamp the resolved project version into the build dir so external tooling
# (build_plugin.bat, CI archive step) can read it without re-parsing this
# file. Single source of truth stays the project() call above.
file(WRITE "${CMAKE_BINARY_DIR}/PLUGIN_VERSION.txt" "${PROJECT_VERSION}")

# Embed Windows VERSIONINFO into the DLL so File Explorer's properties
# pane shows "Semirprod" as company / proper version / copyright. Helps
# AV reputation and gives the binary a non-anonymous identity even when
# unsigned.
if(WIN32)
  # Chemin de l'icone pour le .rc : ABSOLU, et les antislashs doublés — le
  # compilateur de ressources lit `\` comme une échappement dans une chaîne,
  # donc "C:\Users\..." y perdrait ses séparateurs.
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/logo.ico" SEMIRPROD_ICON_PATH)
  string(REPLACE "\\" "\\\\" SEMIRPROD_ICON_PATH "${SEMIRPROD_ICON_PATH}")

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
  )
  target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
endif()

if(MSVC)
  # Durcissement. /GS, /DYNAMICBASE et /NXCOMPAT sont deja actifs par defaut en
  # x64 ; ce qui manquait et n est JAMAIS implicite :
  #   /guard:cf   Control Flow Guard — le plugin parse du JSON et des trames
  #               WebSocket maison alimentees par le reseau ; CFG transforme une
  #               corruption de pointeur de fonction en crash plutot qu en
  #               detournement de flot d execution.
  #   /Qspectre   attenuations Spectre v1.
  #   /CETCOMPAT  shadow stack materielle (link).
  # /sdl est volontairement OMIS : il promeut certains /W4 en erreurs et
  # casserait le build sur les avertissements existants. A activer dans une passe
  # dediee, apres nettoyage.
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8 /guard:cf /Qspectre)
  target_link_options(${PROJECT_NAME} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "semirprod-studio"
)

# macOS : le module doit etre un BUNDLE `.plugin`, pas un dylib nu.
#
# libobs/obs-cocoa.m:40-44 et frontend/widgets/OBSBasic.cpp:142 le disent sans
# ambiguite — OBS cherche
#   ~/Library/Application Support/obs-studio/plugins/<module>.plugin/Contents/MacOS/<module>
# et les donnees dans .../Contents/Resources/. L'ancien `LIBRARY DESTINATION
# "PlugIns"` produisait un dylib nu qu'OBS n'aurait jamais trouve : le plugin
# se compilait, s'installait, et n'apparaissait nulle part.
#
# Il existe encore un repli « legacy » en <module>/bin + <module>/data
# (OBSBasic.cpp:164-166), mais c'est le format d'avant les bundles : on livre
# la disposition courante, pas celle qu'OBS tolere par compatibilite.
#
# `get_module_extension()` renvoie "" sur macOS (obs-cocoa.m:31) : l'executable
# du bundle porte donc le nom du module SANS extension, ce que produit
# exactement OUTPUT_NAME + BUNDLE.
if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "plugin"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "fr.semirprod.obs-plugin"

    # RPATH : UNE seule entree, `@executable_path/../Frameworks`, et elle n'est
    # pas cosmetique — c'est ce qui rend le binaire deplacable.
    #
    # Le module est lie en @rpath a QtCore/QtGui/QtWidgets/QtNetwork, a
    # libobs.framework et a obs-frontend-api.dylib. Aucune de ces bibliotheques
    # n'est embarquee : elles vivent TOUTES dans OBS.app/Contents/Frameworks, et
    # `@executable_path` designe le dossier de l'executable qui a charge le
    # module — donc OBS.app/Contents/MacOS. C'est exactement la convention des
    # plugins livres par OBS lui-meme (cmake/macos/defaults.cmake:38 chez eux,
    # et `otool -l` sur n'importe quel .plugin de OBS.app le confirme).
    #
    # BUILD_WITH_INSTALL_RPATH TRUE parce que sans lui CMake grave dans le
    # binaire COMPILE les chemins absolus de l'arbre de build de la machine de
    # compilation (/Users/<qui-compile>/obs-studio/.deps/...), et ne les
    # remplace qu'a l'`install()`. Le .plugin pris directement dans build-macos/
    # — c'est-a-dire celui qu'on empaquette — partirait donc chez le client avec
    # des rpaths qui n'existent que chez nous. Le symptome serait le pire
    # possible : rien a la compilation, rien a l'installation, et un module qui
    # ne se charge pas chez le client avec, dans le log d'OBS, un « Library not
    # loaded: @rpath/QtCore.framework » qui ne designe personne.
    #
    # INSTALL_RPATH_USE_LINK_PATH reste FALSE (defaut) : le passer a TRUE
    # rajouterait les dossiers de l'edition de liens, ce qui reintroduirait
    # precisement les chemins absolus qu'on vient d'ecarter.
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH TRUE
  )
endif()

# ----------------------------------------------------------------------------
# BACKEND TLS — le fichier sans lequel le plugin mac ne parle a personne.
#
# OBS.app ne livre AUCUN backend TLS Qt (`Contents/PlugIns/` : platforms,
# styles, imageformats, iconengines, et c'est tout). OBS n'en a pas besoin — son
# HTTPS passe par libcurl/mbedTLS — mais nous faisons TOUT en QSslSocket. Sans
# backend, chaque requete rend « TLS initialization failed » (err 99) : le
# plugin se charge, l'interface s'affiche, et rien ne part.
#
# On copie donc le backend SecureTransport d'obs-deps dans notre propre bundle.
# src/mac/QtTlsBackend.cpp declare ce dossier a Qt au chargement du module.
#
# `certonly` n'est deliberement PAS copie : il sait lire des certificats mais ne
# sait pas ouvrir une connexion, donc sa seule contribution possible serait de
# se faire elire backend actif et de transformer une panne bruyante en panne
# subtile.
if(APPLE)
  set(_semirprod_tls_backend
      "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/tls/libqsecuretransportbackend.dylib")
  if(NOT EXISTS "${_semirprod_tls_backend}")
    # Erreur FATALE et non avertissement : un bundle sans backend produit
    # exactement le meme plugin en apparence, qui se charge, s'affiche, et
    # n'atteint jamais le serveur. Mieux vaut ne pas pouvoir construire le
    # paquet que d'en construire un muet.
    message(FATAL_ERROR
      "Backend TLS introuvable : ${_semirprod_tls_backend}\n"
      "Sans lui le plugin macOS ne peut ouvrir AUCUNE connexion HTTPS/wss. "
      "Verifie que CMAKE_PREFIX_PATH pointe bien sur le Qt d'obs-deps.")
  endif()

  # Textes localises (obs_module_text) : OBS lit locale/<langue>.ini dans
  # Contents/Resources du bundle. Copie AVANT le script ci-dessous, qui signe le
  # bundle : un fichier ajoute apres la signature l'invaliderait.
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/locale"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/locale"
    COMMENT "Copie les textes localises dans le bundle"
    VERBATIM
  )

  # Licence (GPL) : texte, mentions tierces et adresse du code source dans le
  # bundle, AVANT la signature pour la meme raison que les textes localises.
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/terms.txt"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/NOTICE.txt"
    COMMENT "Copie la licence dans le bundle"
    VERBATIM
  )

  # Une seule COMMAND : copie, rpath et signature vivent dans
  # cmake/macos/embed_tls_backend.cmake, ou leur ordre obligatoire est explique
  # et ou une erreur reste une erreur (un chainage de COMMAND avale trop
  # facilement un echec derriere un `|| true`).
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DBACKEND=${_semirprod_tls_backend}
            -DBUNDLE=$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>
            -DCODESIGN_IDENTITY=${SEMIRPROD_CODESIGN_IDENTITY}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/embed_tls_backend.cmake"
    COMMENT "Embarque le backend TLS Qt et signe le bundle"
    VERBATIM
  )
endif()

# Textes localises : data/locale/<langue>.ini, lus par obs_module_text() dans la
# langue de l'interface d'OBS. Sur macOS ils vivent DANS le bundle (voir plus
# haut) ; ailleurs ils vont dans le dossier de donnees du module.
if(WIN32)
  install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION obs-plugins/64bit
    LIBRARY DESTINATION obs-plugins/64bit)
  install(DIRECTORY data/locale DESTINATION data/obs-plugins/semirprod-studio)
  install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION data/obs-plugins/semirprod-studio)
  install(FILES terms.txt DESTINATION data/obs-plugins/semirprod-studio RENAME NOTICE.txt)
elseif(APPLE)
  # LIBRARY DESTINATION, et non BUNDLE, et c'est contre-intuitif : la cible EST
  # un bundle `.plugin` (BUNDLE TRUE ci-dessus), mais le mot-cle `BUNDLE` de
  # install() ne vaut que pour les EXECUTABLES marques MACOSX_BUNDLE (les .app).
  # Une bibliotheque MODULE portant BUNDLE TRUE reste classee « LIBRARY » par
  # install(), qui installe alors l'arbre `.plugin` COMPLET — pas seulement le
  # binaire qu'il contient.
  #
  # Ecrire `BUNDLE DESTINATION` ici ne produit pas une mauvaise installation :
  # ca ne configure pas du tout, sur
  #   install TARGETS given no LIBRARY DESTINATION for module target
  # Le mode d'echec est donc bruyant, ce qui est la seule bonne nouvelle.
  install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ".")
else()
  install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION lib/obs-plugins)
  install(DIRECTORY data/locale DESTINATION share/obs/obs-plugins/semirprod-studio)
  install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION share/obs/obs-plugins/semirprod-studio)
  install(FILES terms.txt DESTINATION share/obs/obs-plugins/semirprod-studio RENAME NOTICE.txt)
endif()
