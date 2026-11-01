# Paquet macOS

Le plugin se compile, s'installe et se charge sur macOS. Un `.pkg` est produit
par la chaîne décrite ici. Ce qui manque encore est **administratif** (compte
Apple), plus une CI.

## Chaîne de production

```sh
# 1. La base OBS, UNE FOIS, a la version plancher (voir plus bas)
git -C ~/obs-studio worktree add ~/obs-studio-32.0 32.0.4
cd ~/obs-studio-32.0
git submodule update --init plugins/obs-browser plugins/obs-websocket
cmake --preset macos -DENABLE_BROWSER=OFF -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build_macos --config RelWithDebInfo --target libobs obs-frontend-api

# 2. Le plugin + le paquet
./build_plugin_macos.sh          # -> dist-macos/semirprod-plugin-X.Y.Z.pkg
```

`build_plugin_macos.sh` compile en universel, vérifie l'artefact produit,
installe dans `~/Library/Application Support/obs-studio/plugins/` et appelle
`build_pkg.sh`. `packaging/macos/build_pkg.sh` s'utilise aussi seul.

Deux variantes, par variables d'environnement :

| Variable | Défaut mac | Effet |
|---|---|---|
| `SEMIRPROD_COLLECTIONS_FLAG` | `0` | `1` réaffiche l'onglet Collections |
| `SEMIRPROD_DEV_BUILD_FLAG` | `0` | `1` = build staging (env vars respectées) |
| `NOTARY_PROFILE` | — | profil `notarytool` : signe, notarise et agrafe |

`SEMIRPROD_COLLECTIONS_FLAG` vaut **0** ici alors que le `CMakeLists` est à
`ON` par défaut : le paquet mac est destiné à des postes clients où la fonction
n'est pas voulue. Ce que `OFF` retire est le champ `backups` de l'état envoyé à
l'interface et les trois commandes du pont ; l'interface masque son onglet
quand le champ manque, donc **un seul bundle WebUI sert les deux variantes**.

## Les trois pièges déjà payés

**1. Le Qt de compilation.** Les deps prebuilt d'OBS changent de Qt à
l'intérieur de la ligne 32.x : l'arbre **32.0.4 apporte Qt 6.8.3**, l'arbre
**32.2.2 apporte Qt 6.11.1**. Compiler contre le second produit un binaire qui
marche parfaitement sur le mac de développement et **refuse de charger chez un
client en OBS 32.0 ou 32.1**. Rien ne le signale à la compilation. D'où le
worktree sur `32.0.4` ci-dessus, et le garde-fou `FLOOR_QT` du script.

**2. Le backend TLS.** OBS.app ne livre **aucun** backend TLS Qt — son
`Contents/PlugIns/` contient `platforms`, `styles`, `imageformats`,
`iconengines`, et c'est tout. OBS fait son HTTPS avec libcurl/mbedTLS, donc
personne côté OBS n'en a jamais eu besoin ; nous faisons tout en `QSslSocket`.
Sans backend, **chaque requête** rend « TLS initialization failed » : le plugin
se charge, l'interface s'affiche, et rien ne part. On embarque donc
`libqsecuretransportbackend.dylib` dans `Contents/PlugIns/tls/` du bundle
(CMakeLists, bloc « BACKEND TLS ») et `src/mac/QtTlsBackend.cpp` déclare ce
dossier à Qt au chargement.

**3. L'emplacement d'installation.** Sur Apple Silicon, OBS ne scanne qu'un
seul chemin : `~/Library/Application Support/obs-studio/plugins/<module>.plugin`.
Les replis `/Library` et `bin/`+`data/` sont derrière `#ifndef __aarch64__`
(`frontend/widgets/OBSBasic.cpp`). D'où le paquet en domaine
`currentUserHome` — qui, effet de bord bienvenu, n'exige aucun mot de passe
administrateur.

## Détails qui ont coûté du temps

* **Xcode complet requis** pour configurer OBS (les Command Line Tools ne
  suffisent pas). `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`
  évite un `sudo xcode-select -s` qui changerait le réglage de toute la machine.
* **`ENABLE_PLUGINS=OFF`** sur l'arbre 32.0 casse la détection du langage Swift
  de `libobs-metal`. Il faut initialiser les submodules et passer
  `ENABLE_BROWSER=OFF`.
* **`install(TARGETS … BUNDLE DESTINATION)`** ne configure pas : une
  bibliothèque `MODULE` marquée `BUNDLE TRUE` reste classée `LIBRARY` par
  `install()`, qui installe alors l'arbre `.plugin` complet.
* **Ordre de signature** : retoucher un fichier imbriqué (`install_name_tool`)
  invalide la signature ; il faut signer la dylib **puis** le bundle, sinon
  `codesign` refuse avec « code object is not signed at all In subcomponent ».
* **Scripts de paquet** : `$2` est le chemin d'installation, `$3` le volume.
  Pour un paquet en domaine utilisateur, `$3` vaut `/` et mène au mauvais
  dossier.
* **`open -a OBS --env`** ne transmet pas les variables d'environnement à une
  app déjà connue de launchd. Pour un build de dev, lancer l'exécutable
  directement.

## Ce qu'il reste

1. **Les deux certificats Developer ID.** Toute la chaîne est câblée —
   signature du bundle avec hardened runtime et horodatage, signature du
   paquet, `notarytool submit --wait`, `stapler`, puis verdict `spctl` — et les
   identités sont **détectées automatiquement** dans le trousseau. Il ne manque
   que les certificats eux-mêmes :

   * **Developer ID Application** (signe le `.plugin`)
   * **Developer ID Installer** (signe le `.pkg`)

   Ils exigent un compte **Apple Developer Program payant**. Attention au faux
   ami : un certificat **« Apple Development »**, présent sur presque toutes les
   machines de développeur, signe sans erreur mais est **rejeté à la
   notarisation** — d'où la recherche ciblée sur `Developer ID` dans les deux
   scripts plutôt que « la première identité venue ». Marche à suivre complète :
   `docs/INSTALLATION-macos.md`.
2. **CI.** Un runner macOS auto-hébergé. Les runners hébergés par GitHub sont
   interdits sur ce projet, y compris `macos-latest`.
3. **Trousseau.** `HAVE_QTKEYCHAIN` n'est pas activé : QtKeychain n'est pas dans
   les deps d'OBS, et la version Homebrew tirerait un second Qt dans le
   processus d'OBS. Les jetons passent donc par le repli fichier (XOR salé +
   HMAC, lié à la machine via `QSysInfo::machineUniqueId`), qui fonctionne. Le
   jour où on voudra le vrai Trousseau, c'est `Security.framework` en direct
   qu'il faudra appeler, pas QtKeychain.
