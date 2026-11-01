# Embarque le backend TLS Qt dans le bundle .plugin, puis signe l'ensemble.
#
# Appele en `cmake -P` depuis un POST_BUILD (voir CMakeLists.txt, bloc
# « BACKEND TLS »). Script separe et non chaine de COMMAND parce que les trois
# etapes ont un ORDRE obligatoire que le chainage rendait invisible :
#
#   1. copier le backend dans Contents/PlugIns/tls/
#   2. lui ajouter le rpath d'OBS — ce qui DETRUIT sa signature
#   3. signer la dylib, PUIS le bundle
#
# Signer le bundle avant la dylib echoue sur « code object is not signed at all
# In subcomponent » : codesign refuse de sceller un bundle dont un binaire
# imbrique n'est pas signe. C'est exactement le piege dans lequel la premiere
# version est tombee.
#
# Variables attendues : BACKEND (chemin de la dylib source), BUNDLE (chemin du
# .plugin).

if(NOT EXISTS "${BACKEND}")
  message(FATAL_ERROR "Backend TLS introuvable : ${BACKEND}")
endif()
if(NOT IS_DIRECTORY "${BUNDLE}")
  message(FATAL_ERROR "Bundle introuvable : ${BUNDLE}")
endif()

get_filename_component(_backend_name "${BACKEND}" NAME)
set(_tls_dir "${BUNDLE}/Contents/PlugIns/tls")
set(_installed "${_tls_dir}/${_backend_name}")

file(MAKE_DIRECTORY "${_tls_dir}")
file(COPY "${BACKEND}" DESTINATION "${_tls_dir}")

# rpath : le backend d'obs-deps ne porte que `@loader_path/../../lib`, qui
# decrit la disposition d'obs-deps et ne designe plus rien une fois le fichier
# chez nous. On ajoute celui d'OBS.
#
# Verifier AVANT d'ajouter : install_name_tool echoue quand l'entree existe
# deja, et file(COPY) recopie a chaque build. Sans ce test, toute
# reconstruction casserait le build sur une erreur sans consequence.
set(_rpath "@executable_path/../Frameworks")
execute_process(
  COMMAND otool -l "${_installed}"
  OUTPUT_VARIABLE _load_commands
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(FIND "${_load_commands}" "${_rpath}" _rpath_pos)
if(_rpath_pos EQUAL -1)
  execute_process(
    COMMAND install_name_tool -add_rpath "${_rpath}" "${_installed}"
    COMMAND_ERROR_IS_FATAL ANY
  )
endif()

# Signature. Deux regimes, et ils ne different pas que par l'identite.
#
# AD-HOC (`-`, le defaut) : ne prouve l'identite de personne, sert uniquement a
# ce que macOS accepte de CHARGER le code — sur Apple Silicon un binaire non
# signe, ou dont la signature ne correspond plus au contenu, est refuse net.
#
# DEVELOPER ID (une vraie identite) : il faut alors DEUX options de plus, sans
# lesquelles la notarisation rejette le paquet apres coup, avec un rapport JSON
# qu'il faut aller chercher a la main :
#
#   --timestamp        horodatage par le serveur Apple. Sans lui, la signature
#                      « expire » avec le certificat au lieu de rester valide.
#                      (`--timestamp=none` du regime ad-hoc est exactement son
#                      contraire, d'ou le branchement plutot qu'un ajout.)
#   --options runtime  hardened runtime. Apple REFUSE de notariser un
#                      executable qui ne l'a pas.
#
# Les deux sont inutiles en ad-hoc : l'horodatage demande un certificat valide,
# et le hardened runtime ne se verifie qu'a partir d'une signature de confiance.
if(NOT DEFINED CODESIGN_IDENTITY OR CODESIGN_IDENTITY STREQUAL "")
  set(CODESIGN_IDENTITY "-")
endif()

if(CODESIGN_IDENTITY STREQUAL "-")
  set(_sign_opts --timestamp=none)
else()
  set(_sign_opts --timestamp --options runtime)
  message(STATUS "[semirprod] signature « ${CODESIGN_IDENTITY} » + hardened runtime")
endif()

# Ordre obligatoire : la dylib imbriquee d'abord, le bundle ensuite. Voir
# l'en-tete de ce fichier.
execute_process(
  COMMAND codesign --force --sign "${CODESIGN_IDENTITY}" ${_sign_opts} "${_installed}"
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND codesign --force --sign "${CODESIGN_IDENTITY}" ${_sign_opts} "${BUNDLE}"
  COMMAND_ERROR_IS_FATAL ANY
)
