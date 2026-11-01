# Paquet Windows

## Ce qu'il y a

`installer.iss` — script Inno Setup. Il produit
`semirprod-plugin-installer-<version>.exe`.

`SourceDir=..\..` en tête : tous les chemins relatifs du script (`Source:`,
`LicenseFile=terms.txt`, `SetupIconFile=logo.ico`, `OutputDir=.`) restent écrits
**depuis la racine du dépôt**, comme avant le déménagement du script dans ce
dossier. L'installeur sort donc toujours à la racine, là où `build_plugin.bat`
le cherche.

## La chaîne complète

```bat
rem 1. compiler + empaqueter le zip (depuis la racine du depot)
build_plugin.bat

rem 2. fabriquer l'installeur
iscc packaging\windows\installer.iss
```

`build_plugin.bat` valide, vérifie le plancher Qt (6.8.3, celui d'OBS 32.0),
compile, range dans `dist\` et écrit
`obs-semirprod-plugin-v<version>.zip` — ou `...-DEV-staging.zip` en variante
staging, qui est gitignorée parce qu'elle parle au backend de test.

## Ce qui manque

**Signature Authenticode.** Ni la DLL ni l'installeur ne sont signés. À la
place, le manifeste de mise à jour republie une empreinte SHA-256 authentifiée
(signature ECDSA P-256 du manifeste, cf. `src/update/UpdateChecker.hpp`), contre
laquelle le client ou le support compare son téléchargement. C'est un contrôle
d'intégrité, pas un contrôle d'origine : SmartScreen continue d'avertir à la
première installation.

Si la signature arrive un jour, elle doit couvrir **les deux** fichiers (DLL
puis installeur, dans cet ordre — signer l'installeur ne signe pas ce qu'il
contient) et le workflow CI doit épingler `signtool`.
