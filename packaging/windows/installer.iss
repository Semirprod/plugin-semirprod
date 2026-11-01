; Script Inno Setup du plugin Semirprod pour OBS
;
; Seuls les noms AFFICHES ont change (ex « Semirprod Studio »). AppId, nom de
; la DLL et dossiers d'installation restent ceux d'origine : les changer
; installerait une deuxieme copie a cote de l'ancienne.
#define MyAppName "Semirprod pour OBS"
#define MyAppNameEn "Semirprod for OBS"

; ---------------------------------------------------------------------------
; VERSION : source unique = project(VERSION) dans CMakeLists.txt.
;
; Elle etait ecrite en dur ici. Deux fichiers portaient donc le meme numero, et
; seul l'un des deux bougeait a la release suivante : l'installeur s'appelait
; alors `...-0.1.5.exe` en installant un binaire 0.1.6, et le manifeste de mise
; a jour annoncait une version que le plugin installe ne portait pas. Rien ne
; le signalait, ni a la compilation ni a l'installation.
;
; Ordre de resolution :
;   1. /DMyAppVersion=x.y.z passe a ISCC (ce que fait build_plugin.bat) ;
;   2. sinon build\PLUGIN_VERSION.txt, ecrit par CMake au configure ;
;   3. sinon on ARRETE. Pas de valeur de repli : un installeur qui se nomme
;      tout seul « unknown » finit publie sous ce nom.
; ---------------------------------------------------------------------------
#ifndef MyAppVersion
  #define VersionFile AddBackslash(SourcePath) + "..\..\build\PLUGIN_VERSION.txt"
  #if FileExists(VersionFile)
    ; Handle nomme puis referme : FileOpen() imbrique dans FileRead() laisse le
    ; fichier ouvert pour toute la compilation du script.
    #define VersionHandle FileOpen(VersionFile)
    #define MyAppVersion Trim(FileRead(VersionHandle))
    #expr FileClose(VersionHandle)
  #else
    #error Version introuvable : compile d'abord (build\PLUGIN_VERSION.txt) ou passe /DMyAppVersion=x.y.z a ISCC.
  #endif
#endif
#define MyAppPublisher "Semirprod"
#define MyAppURL "https://semirprod.fr"
#define MyAppSupportURL "https://semirprod.fr"
#define MyAppCopyright "© 2026 Semirprod"
#define MyAppDescription "Semirprod pour OBS, plugin OBS pour la diffusion live Semirprod"
; Plancher de compatibilite annonce, affiche a l'utilisateur avant installation.
; Doit rester d'accord avec FLOOR_OBS dans build_plugin.bat et avec le README.
#define MyAppMinObs "32.0"

[Setup]
AppId={{D3B3E5B1-4A2D-4C9D-A3D5-F1735B7C2A1E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppSupportURL}
AppUpdatesURL={#MyAppURL}
AppCopyright={#MyAppCopyright}
AppContact={#MyAppPublisher}
; Métadonnées embarquées dans l'exe de l'installeur (clic droit -> Propriétés)
VersionInfoVersion={#MyAppVersion}.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppDescription}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCopyright={#MyAppCopyright}
DefaultDirName={code:GetObsPath}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
; Le dossier de destination EST celui d'OBS : il existe donc toujours, et il est
; plein. Avec la valeur par defaut (auto), Inno demandait « ce dossier existe
; deja, voulez-vous quand meme y installer ? » — un avertissement qui laisse
; croire a un probleme alors que c'est precisement le dossier attendu. On le
; coupe, et on le remplace par une verification qui a du sens : NextButtonClick
; refuse un dossier qui ne contient pas bin\64bit\obs64.exe.
DirExistsWarning=no
; LA PAGE DE CHOIX DU DOSSIER S'AFFICHE TOUJOURS.
;
; Sans cette ligne, Inno applique son defaut `auto`, qui MASQUE cette page des
; qu'une installation precedente du meme AppId existe : l'installeur reprend
; alors le dossier memorise sans jamais le demander. Constate a la premiere mise
; a jour — plus aucun moyen de designer son OBS, et rien ne le dit.
;
; Ce n'est pas qu'une gene. TOUTE la validation vit dans NextButtonClick sur
; wpSelectDir, plus bas : elle refuse un dossier sans bin\64bit\obs64.exe et
; bloque en dessous d'OBS {#MyAppMinObs}. Page masquee = validation jamais
; executee. L'installeur ecrivait donc dans le dossier d'hier sans verifier
; qu'OBS s'y trouve encore, ni qu'il est assez recent pour charger le module.
;
; `UsePreviousAppDir` reste a son defaut (yes) : la page est PRE-REMPLIE avec le
; dossier precedent. Celui qui enchaine les mises a jour ne fait que cliquer
; Suivant, et la verification repasse a chaque fois.
DisableDirPage=no
; OBS verrouille semirprod-studio.dll tant qu'il tourne. On ne le ferme JAMAIS
; d'office : avec CloseApplications=yes, le Restart Manager proposait de fermer
; OBS, et un clic sur Suivant coupait un direct en cours. L'installeur refuse
; donc de continuer tant qu'OBS est ouvert (IsObsRunning, verifie au lancement
; et juste avant la copie : InitializeSetup / PrepareToInstall), et c'est le
; streamer qui ferme OBS quand son direct le permet.
CloseApplications=no
RestartApplications=no
; On demande les droits admin car OBS est souvent dans Program Files
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os

; SourceDir : le script a demenage dans packaging/windows/, tous les chemins
; relatifs ci-dessous (Source:, LicenseFile, SetupIconFile, OutputDir) restent
; ecrits DEPUIS LA RACINE du depot. Une seule ligne a changer plutot que dix
; « ..\.. » disperses, et l'installeur sort toujours a la racine, la ou
; build_plugin.bat le cherche.
SourceDir=..\..
OutputDir=.
OutputBaseFilename=semirprod-plugin-installer-{#MyAppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
LicenseFile=terms.txt
; Icone de l'executable d'installation lui-meme. Sans cette ligne, Inno posait
; son icone par defaut : l'installeur ne portait aucune marque Semirprod.
SetupIconFile=logo.ico
WizardSmallImageFile=logo.png
UninstallDisplayName={#MyAppName} {#MyAppVersion}
; --- DESINSTALLATION ------------------------------------------------------
; Par defaut, Inno pose son desinstalleur dans {app} — donc DANS le dossier
; d'OBS, ou OBS a deja le sien. Les deux se retrouvent cote a cote sous des noms
; auto-incrementes (unins000 pour OBS, unins001 pour nous), impossibles a
; distinguer pour un humain, et un dossier d'OBS reinstalle ou deplace laissait
; une entree morte dans « Applications installees ».
;
; Le notre vit donc dans son propre dossier. OBS ne recoit plus que la DLL.
UninstallFilesDir={commonappdata}\Semirprod Studio
; Icone reelle plutot que la DLL du plugin, qui n'en porte aucune : sans elle
; l'entree de desinstallation s'affiche sans image et se reperait mal.
UninstallDisplayIcon={commonappdata}\Semirprod Studio\logo.ico

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
; Le plancher est dit sur le premier ecran. Sur un OBS plus ancien, libobs
; refuse le module en silence : l'utilisateur ne verrait qu'un plugin absent.
french.WelcomeLabel2=Ce programme va installer {#MyAppName} {#MyAppVersion} sur ton ordinateur.%n%nNécessite OBS Studio {#MyAppMinObs} ou supérieur.%n%nFerme OBS avant de continuer : le fichier du plugin est verrouillé quand OBS tourne.
english.WelcomeLabel2=This will install {#MyAppNameEn} {#MyAppVersion} on your computer.%n%nRequires OBS Studio {#MyAppMinObs} or newer.%n%nClose OBS before continuing: the plugin file is locked while OBS is running.

[CustomMessages]
; OBS ouvert : l'installeur attend, il ne ferme rien (voir CloseApplications).
french.ObsRunning=OBS est ouvert. Ferme-le avant d'installer {#MyAppName}.
english.ObsRunning=OBS is open. Close it before installing {#MyAppNameEn}.

[InstallDelete]
; Nettoie un éventuel Qt6WebSockets.dll laissé par une ancienne version du
; plugin : il n'est plus utilisé et, s'il ne matche pas le Qt d'OBS, il faisait
; échouer le chargement du plugin ("Module not loaded").
Type: files; Name: "{app}\obs-plugins\64bit\Qt6WebSockets.dll"
; Raccourci de desinstallation pose sous l'ancien nom du produit. Le groupe du
; menu Demarrer est repris tel quel a la mise a jour (UsePreviousGroup) : sans
; cette ligne, l'ancien raccourci resterait a cote du nouveau.
Type: files; Name: "{group}\Désinstaller Semirprod Studio.lnk"

[Files]
; --- POURQUOI on installe dans le dossier d'OBS et pas dans ProgramData ---
;
; OBS cherche ses modules à plusieurs endroits. Le dossier de l'application
; (obs-plugins\64bit) est scanné DANS TOUS LES CAS. Le second emplacement,
; %ProgramData%\obs-studio\plugins\<module>\bin\64bit, souvent présenté comme
; « le bon » parce qu'il évite l'UAC, est ajouté par la fenêtre principale —
; mais uniquement hors mode portable :
;
;   frontend/widgets/OBSBasic.cpp
;     if (portable_mode) return;               <-- sortie AVANT ProgramData
;     GetProgramDataPath(..., "obs-studio/plugins/%module%");
;     obs_add_module_path(path + "/bin/64bit", path + "/data");
;
; Et le mode portable s'active aussi par un simple drapeau de ligne de commande
; (--portable), qu'un installeur ne peut pas détecter (frontend/obs-main.cpp).
; Installer dans ProgramData nous ferait donc perdre les installations
; portables, sans rien gagner d'autre que l'UAC. On reste dans le dossier d'OBS.
;
; Note: On cible le dossier RelWithDebInfo généré par ton build_plugin.bat
Source: "build\RelWithDebInfo\semirprod-studio.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion

; --- Textes localises (FR/EN) ---
; OBS lit data\obs-plugins\<module>\locale\<langue>.ini dans la langue de son
; interface (obs_module_text). Sans ces fichiers les docks, le Journal et les
; messages du plugin affichent des cles au lieu de texte. Chemin relatif au
; dossier d'ou ISCC est lance (la racine du depot), comme la DLL ci-dessus.
Source: "data\locale\*.ini"; DestDir: "{app}\data\obs-plugins\semirprod-studio\locale"; Flags: ignoreversion

; --- Licence (GPL-2.0-or-later) ---
; Obligation de la GPL : le texte de la licence et les mentions des composants
; tiers accompagnent chaque copie distribuee du binaire. terms.txt (affiche par
; l'assistant via LicenseFile) donne l'adresse du code source de la version.
Source: "LICENSE"; DestDir: "{app}\data\obs-plugins\semirprod-studio"; Flags: ignoreversion
Source: "THIRD_PARTY_NOTICES.md"; DestDir: "{app}\data\obs-plugins\semirprod-studio"; Flags: ignoreversion
Source: "terms.txt"; DestDir: "{app}\data\obs-plugins\semirprod-studio"; DestName: "NOTICE.txt"; Flags: ignoreversion

; --- PLUS de Qt6WebSockets.dll ---
; Le plugin implémente désormais RFC 6455 sur Qt6::Network (fourni par OBS), donc
; on n'embarque plus AUCUN module Qt qu'OBS ne fournit pas. C'est ce qui rend le
; plugin résistant aux mises à jour de Qt d'OBS (voir src/ws/WsFraming.hpp).

; --- Backend TLS (Schannel) — FALLBACK uniquement ---
; OBS fournit déjà son propre backend TLS (bin\64bit\tls\qschannelbackend.dll),
; toujours à la bonne version. On ne l'écrase JAMAIS : onlyifdoesntexist ne pose
; le nôtre que si OBS n'en a aucun (install exotique). Un backend Qt est chargé
; via QPluginLoader (compat minor-forward), donc un 6.8.x reste valide en 6.11+.
; Chemin paramétrable : il était en dur sur le poste du développeur, ce qui
; empêchait tout autre mainteneur de construire l'installeur et exposait le nom
; d'utilisateur du développeur dans l'exécutable distribué. Surchargeable au
; build : ISCC.exe /DQT_TLS_BACKEND_DIR="<chemin>" installer.iss
;
; ⚠️ CE FICHIER N'EST PAS OPTIONNEL. Sans lui, Qt n'a aucun backend TLS et le
; plugin ne peut faire AUCUNE requete HTTPS : « TLS initialization failed »
; (err=99) sur tout, donc pas d'appairage, pas de mise a jour, rien. Le plugin
; s'installe, se charge, et ne sert a rien.
;
; C'est arrive le 20/08/2026 sur un banc d'installation propre : le defaut
; ci-dessous pointait `deps\qt6\plugins\tls`, qui n'existe pas dans le depot, et
; `skipifsourcedoesntexist` transformait cette absence en OMISSION SILENCIEUSE.
; L'installeur se compilait sans un mot et livrait un plugin muet.
;
; La correction : `skipifsourcedoesntexist` est RETIRE et remplace par un refus
; au build. Un mainteneur dont le chemin differe voit l'erreur ici, il ne la
; decouvre pas chez un client.
;
; Le defaut reste VOLONTAIREMENT relatif et non le chemin de ce poste : un
; chemin absolu `C:\Users\<nom>\...` pose en defaut finit dans l'executable
; distribue et y expose le nom d'utilisateur du developpeur. C'est ce qui avait
; ete corrige avant, et ca ne doit pas revenir. Le prix est qu'un `iscc` sans
; /D echoue — c'est le comportement voulu, la commande complete est dans
; packaging/windows/README.md.
;
; Sur le poste de dev, le dossier est celui que build_plugin.bat nomme
; QT_TLS_PLUGIN : <arbre obs-studio>\.deps\obs-deps-qt6-<date>-x64\plugins\tls
#ifndef QT_TLS_BACKEND_DIR
  #define QT_TLS_BACKEND_DIR "deps\qt6\plugins\tls"
#endif
#if !FileExists(AddBackslash(QT_TLS_BACKEND_DIR) + "qschannelbackend.dll")
  #error qschannelbackend.dll introuvable. Passe le bon dossier : ISCC.exe /DQT_TLS_BACKEND_DIR="<...>\plugins\tls" installer.iss  (sans ce fichier, le plugin livre ne peut faire aucune requete HTTPS)
#endif
; `onlyifdoesntexist` reste : on ne remplace jamais le backend d'un OBS qui a
; deja le sien, seule une install exotique recoit le notre.
Source: "{#QT_TLS_BACKEND_DIR}\qschannelbackend.dll"; DestDir: "{app}\bin\64bit\tls"; Flags: onlyifdoesntexist

; --- Icone de l'entree de desinstallation ---
; Posee hors du dossier d'OBS, avec le desinstalleur.
Source: "logo.ico"; DestDir: "{commonappdata}\Semirprod Studio"; Flags: ignoreversion

[Icons]
; Un raccourci de desinstallation dans le menu Demarrer. Sans lui, le seul
; chemin etait « Parametres > Applications installees », que personne ne pense a
; ouvrir pour retirer un plugin OBS. L'utilisateur peut refuser le groupe du
; menu Demarrer (AllowNoIcons) ; l'entree Windows reste dans tous les cas.
Name: "{group}\Désinstaller {#MyAppName}"; Filename: "{uninstallexe}"; IconFilename: "{commonappdata}\Semirprod Studio\logo.ico"

[UninstallDelete]
; Notre propre dossier : le desinstalleur ne peut pas se supprimer lui-meme en
; cours d'execution, Inno s'en charge a la fin.
Type: filesandordirs; Name: "{commonappdata}\Semirprod Studio"

; --- La Caméra sans fil n'installe plus rien ---
; Elle recevait le flux du téléphone via un relais média lancé en process
; enfant, qui écoutait sur le réseau local. Ce relais n'existe plus : OBS
; embarque CEF, donc une source navigateur est un pair WebRTC complet et
; reçoit le flux directement. Plus de binaire tiers à poser, plus de port
; ouvert en entrée, plus de règle de pare-feu.

[Run]
; Nettoyage des règles de pare-feu posées par les versions précédentes pour
; l'ancien relais. Ici et pas seulement en désinstallation : une mise à jour
; ne passe jamais par [UninstallRun], et laisser une autorisation d'entrée au
; nom d'un binaire qu'on ne livre plus est exactement le genre de résidu qui
; traîne des années sur les postes.
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Semirprod - Relais camera sans fil (TCP)"""; Flags: runhidden
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Semirprod - Relais camera sans fil (UDP)"""; Flags: runhidden

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Semirprod - Relais camera sans fil (TCP)"""; Flags: runhidden; RunOnceId: "RelayFwTcp"
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Semirprod - Relais camera sans fil (UDP)"""; Flags: runhidden; RunOnceId: "RelayFwUdp"

[Code]
// Fonction pour détecter dynamiquement le chemin d'installation d'OBS
function GetObsPath(Param: String): String;
var
  ObsPath: String;
begin
  // Le registre FAIT FOI, plus le dossier de travail du développeur.
  //
  // La version précédente essayait d'abord 'C:\Users\<dev>\Desktop\OBS-Pro\
  // obs-studio'. Deux problèmes : ce chemin exposait le nom d'utilisateur du
  // développeur dans l'exécutable distribué, et sur une machine où il existerait,
  // l'installeur — qui tourne en administrateur — y aurait déposé la DLL sans
  // consulter le registre, donnant une écriture pilotée par un chemin sous
  // C:\Users\ à un processus élevé.
  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\OBS Studio', '', ObsPath) then
    Result := ObsPath
  else if RegQueryStringValue(HKEY_CURRENT_USER, 'SOFTWARE\OBS Studio', '', ObsPath) then
    Result := ObsPath
  else
    Result := ExpandConstant('{pf}\obs-studio');
end;

// Plancher de compatibilite, lu sur le binaire d'OBS lui-meme.
//
// En dessous de 32.0, libobs REFUSE le module (il est compile contre le SDK
// 32.0) : l'utilisateur installerait un fichier qu'OBS ne chargerait jamais, et
// ne verrait qu'un plugin absent, sans message. On lit donc la version reelle de
// obs64.exe — GetVersionNumbers renvoie les champs de la ressource VERSIONINFO,
// c'est fiable et ca ne suppose rien — et on bloque avant de copier quoi que ce
// soit. Le registre ne porte que le CHEMIN d'OBS, jamais sa version : il sert a
// trouver l'executable, pas a le dater.
const
  MinObsMajor = 32;
  MinObsMinor = 0;

// True si la version a pu etre lue ET qu'elle est suffisante. `Version` recoit
// la version lue ('' si illisible) et `Readable` dit si la lecture a abouti :
// on ne bloque JAMAIS sur une version qu'on n'a pas su lire (build portable
// exotique, ressource absente) — un faux refus serait pire que le probleme.
function ObsVersionSupported(const ExePath: String; var Version: String;
                             var Readable: Boolean): Boolean;
var
  VerMS, VerLS: Cardinal;
  Major, Minor: Cardinal;
begin
  Version := '';
  Readable := GetVersionNumbers(ExePath, VerMS, VerLS);
  if not Readable then begin
    Result := True;
    exit;
  end;

  Major := VerMS shr 16;
  Minor := VerMS and $FFFF;
  Version := IntToStr(Major) + '.' + IntToStr(Minor);

  Result := (Major > MinObsMajor) or
            ((Major = MinObsMajor) and (Minor >= MinObsMinor));
end;

// Une copie du module dans %ProgramData% ne remplace pas la notre, elle s'y
// AJOUTE : OBS scanne les deux emplacements, chargerait le plugin deux fois et
// enregistrerait deux fois les memes types de source. On previent, et on propose
// de retirer l'autre copie — jamais en silence, ce n'est pas un dossier que cet
// installeur a cree.
//
// Declaree AVANT InitializeSetup : le Pascal d'Inno exige la declaration avant
// l'appel.
procedure WarnOnDuplicateCopy();
var
  Other: String;
begin
  Other := ExpandConstant('{commonappdata}\obs-studio\plugins\semirprod-studio');
  if not FileExists(Other + '\bin\64bit\semirprod-studio.dll') then
    exit;

  if MsgBox('Une autre copie de {#MyAppName} est deja installee ici :' + #13#10
            + Other + #13#10#13#10 +
            'OBS scanne les deux emplacements : le plugin serait charge DEUX fois,'
            + #13#10 + 'et les sources Semirprod apparaitraient en double.'
            + #13#10#13#10 + 'Supprimer cette autre copie maintenant ?',
            mbConfirmation, MB_YESNO) = IDYES then
    DelTree(Other, True, True, True);
end;

// NOTE — desinstalleurs orphelins des versions precedentes.
//
// Avant que UninstallFilesDir ne les envoie ailleurs, nos anciens installeurs
// posaient leur desinstalleur dans le dossier d'OBS, ou il prenait le nom libre
// suivant : OBS occupe unins000, nous avions donc unins001. Une desinstallation
// interrompue (typiquement : OBS ouvert, la DLL verrouillee) laisse ce couple
// .exe/.dat derriere elle, sans entree dans le registre.
//
// Ces fichiers ne sont volontairement PAS supprimes ici. Le seul discriminant
// envisageable etait le contenu du .dat, or il est binaire et compresse : la
// verification sur les fichiers reels du poste de developpement ne trouve ni
// « Semirprod » ni « obs-studio », ni en ANSI ni en UTF-16. Sans discriminant
// fiable, une suppression par motif de nom risquerait d'emporter le
// desinstalleur d'OBS lui-meme. On prefere 3 Mo de residu a ce risque ; leur
// suppression manuelle se fait par date de fichier.

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ConfigDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;

  // La session enregistree (jeton chiffre + reglages) survit a la
  // desinstallation par defaut : c'est ce qu'on veut lors d'une simple mise a
  // jour. Mais quand on retire vraiment le plugin, laisser un jeton de compte
  // sur le disque n'a aucune raison d'etre — on propose donc, sans l'imposer.
  ConfigDir := ExpandConstant('{userappdata}\obs-studio\plugin_config\semirprod-studio');
  if not DirExists(ConfigDir) then
    exit;

  if MsgBox('Supprimer aussi la session enregistree ({#MyAppName}) ?' + #13#10#13#10 +
            'Cela efface le jeton de connexion et les reglages :' + #13#10 +
            ConfigDir + #13#10#13#10 +
            'Reponds Non si tu comptes reinstaller le plugin et rester connecte.',
            mbConfirmation, MB_YESNO) = IDYES then
    DelTree(ConfigDir, True, True, True);
end;

// OBS tourne-t-il ? Deux sondes, parce qu'aucune ne suffit seule :
//   * le mutex nomme d'une installation classique (OBSStudioCore) ;
//   * la liste des processus (WMI) pour une installation PORTABLE, dont le
//     mutex porte le chemin de sa configuration et ne se devine pas.
// WMI indisponible : on s'en tient au mutex, sans bloquer pour autant.
function IsObsRunning(): Boolean;
var
  Locator, Service, Processes: Variant;
begin
  Result := CheckForMutexes('OBSStudioCore');
  if Result then
    exit;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Processes.Count > 0;
  except
    Result := False;
  end;
end;

// Juste avant la copie : OBS a pu etre rouvert pendant l'assistant. Une chaine
// non vide arrete l'installation sur la page « Preparation », sans rien copier.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if IsObsRunning() then
    Result := CustomMessage('ObsRunning');
end;

function InitializeSetup(): Boolean;
var
  ObsPath, ExePath, Version: String;
  Readable: Boolean;
begin
  Result := True;

  // OBS ouvert = peut-etre un direct en cours : on ne ferme rien, on attend.
  while IsObsRunning() do begin
    if MsgBox(CustomMessage('ObsRunning'), mbError, MB_RETRYCANCEL) <> IDRETRY then begin
      Result := False;
      exit;
    end;
  end;

  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\OBS Studio', '', ObsPath) then
    if not RegQueryStringValue(HKEY_CURRENT_USER, 'SOFTWARE\OBS Studio', '', ObsPath) then
      ObsPath := '';

  // Pas d'OBS au registre : installation portable ou chemin choisi a la main.
  // On laisse l'utilisateur designer le dossier a l'ecran suivant.
  if ObsPath = '' then
    exit;

  ExePath := ObsPath + '\bin\64bit\obs64.exe';
  if not FileExists(ExePath) then begin
    MsgBox('OBS Studio est enregistre dans le systeme, mais son executable est introuvable :'
           + #13#10 + ExePath + #13#10#13#10 +
           'Verifie que l''installation d''OBS n''est pas abimee avant de continuer.',
           mbInformation, MB_OK);
    exit;
  end;

  if not ObsVersionSupported(ExePath, Version, Readable) then begin
    MsgBox('{#MyAppName} {#MyAppVersion} exige OBS Studio {#MyAppMinObs} ou superieur.'
           + #13#10#13#10 + 'Version detectee : OBS ' + Version + #13#10#13#10 +
           'Sur une version plus ancienne, OBS refuse purement et simplement de'
           + #13#10 + 'charger le plugin : il n''apparaitrait nulle part, sans message'
           + #13#10 + 'd''erreur. Mets OBS a jour, puis relance cette installation.',
           mbError, MB_OK);
    Result := False;
    exit;
  end;

  if not Readable then
    MsgBox('La version d''OBS Studio n''a pas pu etre lue sur :' + #13#10 + ExePath
           + #13#10#13#10 + '{#MyAppName} exige OBS {#MyAppMinObs} ou superieur.'
           + #13#10 + 'L''installation continue, mais verifie ta version d''OBS si le'
           + #13#10 + 'plugin n''apparait pas apres redemarrage.',
           mbInformation, MB_OK);

  WarnOnDuplicateCopy();
end;

// Validation du dossier REELLEMENT choisi, et pas seulement de celui du
// registre : un utilisateur en version portable, ou qui corrige le chemin a la
// main, passe par ici. C'est le dernier moment ou l'on peut refuser proprement,
// avant toute copie de fichier.
//
// Remplace l'avertissement « ce dossier existe deja » d'Inno, qui etait un faux
// positif (le dossier d'OBS existe toujours) par le seul test qui compte : ce
// dossier est-il bien une installation d'OBS, et est-elle assez recente.
function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir, ExePath, Version: String;
  Readable: Boolean;
begin
  Result := True;
  if CurPageID <> wpSelectDir then
    exit;

  Dir := WizardDirValue;
  ExePath := AddBackslash(Dir) + 'bin\64bit\obs64.exe';

  if not FileExists(ExePath) then begin
    MsgBox('Ce dossier ne contient pas OBS Studio :' + #13#10 + Dir + #13#10#13#10 +
           'Attendu : bin\64bit\obs64.exe' + #13#10#13#10 +
           'Choisis le dossier d''installation d''OBS (celui qui contient bin, data'
           + #13#10 + 'et obs-plugins). Un plugin pose ailleurs n''est jamais charge.',
           mbError, MB_OK);
    Result := False;
    exit;
  end;

  if not ObsVersionSupported(ExePath, Version, Readable) then begin
    MsgBox('Cette installation d''OBS est en version ' + Version + '.' + #13#10#13#10 +
           '{#MyAppName} {#MyAppVersion} exige OBS {#MyAppMinObs} ou superieur :'
           + #13#10 + 'en dessous, OBS refuse de charger le plugin.',
           mbError, MB_OK);
    Result := False;
  end;
end;
