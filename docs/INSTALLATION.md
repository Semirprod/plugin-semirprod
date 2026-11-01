# Installer Semirprod pour OBS

Télécharge `semirprod-plugin-installer-X.Y.Z.exe` depuis ton compte Semirprod.
Windows affichera un avertissement parce que le fichier n'est pas signé. Quatre
points, dans cet ordre.

## 1. Fermer OBS

L'installeur refuse de continuer tant qu'OBS est ouvert et ne le ferme jamais :
un OBS ouvert est peut-être en direct. Ferme-le, puis clique **Réessayer**.

## 2. Débloquer le fichier

Si tu as reçu l'installeur par Discord, par mail ou via un navigateur, Windows y
attache une marque de provenance :

> clic droit sur `semirprod-plugin-installer-x.y.z.exe` → **Propriétés** →
> en bas, cocher **Débloquer** → **OK**

## 3. Passer l'écran bleu de Windows

Au lancement, Windows affiche « **Windows a protégé votre PC** » sur fond bleu.
C'est **SmartScreen**, et ça ne veut pas dire que le fichier est dangereux : ça
veut dire que Windows ne le reconnaît pas encore, parce qu'il n'est pas signé
avec un certificat payant.

> cliquer **Informations complémentaires** → le bouton **Exécuter quand même**
> apparaît alors → cliquer dessus

Le bouton n'est **pas visible avant** ce premier clic : c'est pour ça qu'on
croit être bloqué.

## 4. Vérifier que c'est bien notre fichier

Avant de cliquer, l'écran de SmartScreen affiche l'éditeur et le nom du fichier.
Et dans les propriétés de l'exe (onglet **Détails**), on doit lire :

| Champ | Valeur attendue |
|---|---|
| Nom du produit | Semirprod pour OBS |
| Société | Semirprod |
| Version du fichier | celle qu'on t'a annoncée |

Si ces champs sont vides ou différents, **ne l'installe pas** : le fichier ne
vient pas de nous.

## Prérequis

**OBS Studio 32.0 ou supérieur.** En dessous, OBS refuse de charger le plugin :
il n'apparaîtrait nulle part, sans message d'erreur. L'installeur vérifie la
version et s'arrête si elle est trop ancienne.

## Après l'installation

Relance OBS, puis clique **Semirprod** dans la barre de menus. Si le compte n'est pas
encore lié, l'écran d'appairage s'affiche : clique **Connecter**.

## Désinstaller

Menu Démarrer → **Désinstaller Semirprod pour OBS**, ou Paramètres → Applications
installées → *Semirprod pour OBS*. La désinstallation propose d'effacer aussi la
session enregistrée (le jeton de connexion) ; répondre **Non** si tu comptes
réinstaller et rester connecté.
