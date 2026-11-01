# Installer Semirprod pour OBS sur macOS

Le paquet `semirprod-plugin-X.Y.Z.pkg` installe le plugin dans **ton dossier
utilisateur**. Il ne demande aucun mot de passe administrateur et ne touche pas
à OBS lui-même.

Prérequis : **OBS Studio 32.0 ou plus récent**, **macOS 13 ou plus récent**.

---

## Installation

1. **Ferme OBS.**
2. Ouvre `semirprod-plugin-X.Y.Z.pkg`.
3. Laisse-toi guider, puis **relance OBS**.

Le dock **Semirprod** s'ouvre au démarrage. S'il n'apparaît pas :
menu **Docks → Semirprod**.

### Tant que le paquet n'est pas notarisé

Au double-clic, macOS affiche :

> « semirprod-plugin-X.Y.Z.pkg » ne peut pas être ouvert, car il provient d'un
> développeur non identifié.

Ça ne veut pas dire que le fichier est dangereux : macOS ne le reconnaît pas
encore, faute de signature Apple. Deux façons de passer outre :

* **clic droit** sur le `.pkg` → **Ouvrir** → **Ouvrir** dans la boîte de
  dialogue ; ou
* **Réglages Système → Confidentialité et sécurité**, puis **Ouvrir quand
  même** sous le message de blocage.

Voir « Signature et notarisation » plus bas pour ce qui reste à faire pour que
cet écran disparaisse.

---

## Où le plugin est installé, et pourquoi là

```
~/Library/Application Support/obs-studio/plugins/semirprod-studio.plugin
```

Ce n'est pas un choix de confort : sur les Mac **Apple Silicon**, c'est le
**seul** emplacement qu'OBS examine. Les deux autres chemins historiques —
`/Library/Application Support/...` à l'échelle du système, et l'ancienne
disposition `bin/` + `data/` — sont derrière un `#ifndef __aarch64__` dans le
code d'OBS (`frontend/widgets/OBSBasic.cpp`). Un plugin posé ailleurs
s'installerait sans erreur et resterait invisible pour OBS.

---

## Désinstaller

OBS fermé :

```sh
rm -rf ~/Library/Application\ Support/obs-studio/plugins/semirprod-studio.plugin
```

Les réglages et le jeton de connexion restent dans
`~/Library/Application Support/obs-studio/plugin_config/semirprod-studio/` — à
supprimer aussi pour repartir de zéro.

---

## Ça ne marche pas

**Le dock n'apparaît pas.** Ouvre **Aide → Fichiers journaux → Afficher les
journaux** et cherche la section `Loaded Modules`. Deux cas :

* `semirprod-studio` y figure → le module est chargé, le problème est ailleurs
  (regarde les lignes `[semirprod-…]` du même journal).
* il n'y figure pas → le module a été refusé au chargement. Cause la plus
  fréquente sur macOS : la signature du bundle a été invalidée. Réinstalle le
  paquet.

**Le plugin se charge mais rien ne part sur le réseau.** Cherche la ligne
`[semirprod-tls]` dans le journal. Elle doit dire `backend securetransport
actif`. Si elle dit `AUCUN backend TLS`, le backend embarqué manque ou a été
compilé avec un Qt plus récent que celui de ton OBS — c'est un défaut du
paquet, pas de ta machine.

**OBS est en Mode sans échec.** Au démarrage après un arrêt brutal, OBS propose
le Mode sans échec, qui **désactive tous les plugins tiers**. Choisis
« Lancement normal ». La ligne `[Safe Mode]` en tête du journal dit lequel des
deux a été retenu.

---

## Supprimer l'avertissement : signer et notariser

Tout est câblé dans la chaîne de build. Il manque uniquement **deux
certificats**, à créer une fois, dans un compte **Apple Developer Program**
payant (99 $/an) où tu es *Account Holder* ou *Admin*.

### Ce qui ne marche pas

Un certificat **« Apple Development »** — celui que Xcode crée tout seul, et
que la plupart des machines ont déjà — **ne convient pas**. Il sert au test
local : il signe, tout a l'air normal, et la notarisation le **rejette**. Le
message d'Apple ne dit pas clairement que le type de certificat est en cause.
Il faut des certificats **Developer ID**, et ils ne s'obtiennent qu'avec un
compte payant.

Signer sans notariser ne suffit pas non plus : Gatekeeper interroge Apple,
ne trouve aucun ticket, et bloque exactement comme pour un paquet non signé.
La signature ne fait que **rendre la notarisation possible**.

### 1. Créer les deux certificats

Xcode → **Settings** → **Accounts** → ton Apple ID → ton équipe →
**Manage Certificates…** → bouton **+** :

| À créer | Rôle |
|---|---|
| **Developer ID Application** | signe le bundle `.plugin` |
| **Developer ID Installer** | signe le `.pkg` |

Les deux, pas l'un ou l'autre : ce sont deux certificats distincts pour deux
opérations distinctes. Ils atterrissent dans le trousseau, où la chaîne de
build les trouve **toute seule** — aucune variable à renseigner.

Vérification :

```sh
security find-identity -v | grep "Developer ID"
```

Deux lignes doivent apparaître.

### 2. Enregistrer les identifiants de notarisation (une fois)

```sh
xcrun notarytool store-credentials semirprod \
  --apple-id <ton-apple-id> --team-id <ton-team-id> --password <mot-de-passe-d-app>
```

Le mot de passe est un **mot de passe pour application**, créé sur
[appleid.apple.com](https://appleid.apple.com) → Connexion et sécurité →
Mots de passe pour applications. **Jamais** le mot de passe du compte Apple.

Le Team ID est le code entre parenthèses dans le nom du certificat
(ex. `Developer ID Application: … (ABCDE12345)`).

### 3. Construire

```sh
NOTARY_PROFILE=semirprod ./build_plugin_macos.sh
```

Le script signe le bundle (hardened runtime + horodatage Apple), signe le
paquet, l'envoie à la notarisation, attend le verdict, agrafe le ticket, puis
affiche le verdict de Gatekeeper. La ligne à obtenir :

```
[INFO] Verdict Gatekeeper :
       …pkg: accepted
       source=Notarized Developer ID
```

À partir de là, le paquet s'ouvre par double-clic, sans aucun avertissement,
sur n'importe quel Mac.

### Si la notarisation est refusée

Le refus arrive avec un identifiant de soumission. Le détail, fichier par
fichier :

```sh
xcrun notarytool log <id-de-soumission> --keychain-profile semirprod
```
