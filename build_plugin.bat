@echo off
setlocal ENABLEDELAYEDEXPANSION

rem === CONFIG MODIFIABLE ===
rem
rem PLANCHER DE COMPATIBILITE : OBS 32.0 (SDK libobs de compilation).
rem
rem Deux valeurs ci-dessous le fixent, et les changer casse des utilisateurs
rem sans qu'aucune ligne de code ne bouge :
rem
rem  * OBS_SRC — libobs refuse tout module compile contre une libobs plus
rem    RECENTE que la sienne (obs-module.c, MODULE_INCOMPATIBLE_VER). Pointer ce
rem    chemin sur un arbre 32.2 exclurait instantanement les OBS 32.0 et 32.1.
rem  * QT_DIR — OBS 32.0/32.1 livrent Qt 6.8.3, OBS 32.2+ livre Qt 6.11.1.
rem    Compiler avec 6.11 rendrait le DLL inchargeable sur 32.0/32.1 (imports
rem    Qt non resolus). On compile donc TOUJOURS avec le Qt de la plus ancienne
rem    version visee, jamais avec celui de l'OBS installe sur ce poste.
rem
rem Le garde-fou plus bas verifie ces deux points et refuse de compiler sinon.
rem Surchargeables depuis l appelant, sinon ces valeurs. Elles etaient posees
rem SANS condition : sur une autre machine que celle qui a ecrit ce script, la
rem seule facon de compiler etait d editer le fichier.
if not defined OBS_SRC set "OBS_SRC=%USERPROFILE%\Desktop\obs-studio"
if not defined OBS_BUILD set "OBS_BUILD=%OBS_SRC%\build"
if not defined OBS_DEPS set "OBS_DEPS=%OBS_SRC%\.deps\obs-deps-2025-08-23-x64"
if not defined QT_DIR set "QT_DIR=C:\Qt\6.8.3\msvc2022_64"

rem Plancher declare, en dur : sert au garde-fou et doit rester d'accord avec
rem le README et l'installeur.
set "FLOOR_OBS=32.0"
set "FLOOR_QT=6.8"

rem === DEV BUILD ===
rem Definis a 1 (=on) pour compiler un binaire qui respecte les variables
rem d'environnement SEMIRPROD_*_BASE (utile pour tester contre staging).
rem Laisse vide ou a 0 pour un build de production : les URLs sont hardcodees
rem et les env vars sont silencieusement ignorees.
rem Le build par defaut de ce script (.bat) reste production — c'est ce qu'on
rem distribue. Passe a 1 explicitement quand tu bosses sur staging.
rem Surchargeable depuis l'appelant, pour ne pas avoir a editer ce fichier a
rem chaque aller-retour prod/staging. NOTER LES GUILLEMETS :
rem     set "SEMIRPROD_DEV_BUILD_FLAG=1" && .\build_plugin.bat
rem
rem Sans eux, cmd affecte TOUT ce qui suit le `=` jusqu'au `&&`, espace
rem compris : la variable vaut « 1 » et non « 1 », la comparaison plus bas
rem echoue, et on obtient un build de PRODUCTION en croyant tester le staging.
rem Vecu le 15 aout : le binaire parlait a la prod sans que rien ne le dise.
rem
rem Sans rien, le defaut reste production — c'est ce qu'on distribue.
if not defined SEMIRPROD_DEV_BUILD_FLAG set "SEMIRPROD_DEV_BUILD_FLAG=0"
rem Filet de securite : on retire les espaces plutot que de compter sur
rem l'appelant. Meme traitement que PLUGIN_VERSION plus bas.
set "SEMIRPROD_DEV_BUILD_FLAG=%SEMIRPROD_DEV_BUILD_FLAG: =%"

rem Chemin vers un OBS installe (laisse vide pour desactiver la copie auto)
if not defined OBS_INSTALL set "OBS_INSTALL=%USERPROFILE%\Desktop\OBS-Pro\obs-studio"

rem Mets a 1 pour NE PAS copier dans l'OBS de ce poste.
rem
rem Un build de RELEASE copie dans OBS_INSTALL — seul un build dev saute cette
rem etape. Fabriquer une dist a distribuer ecrasait donc au passage le plugin de
rem l'OBS de travail, et si cet OBS tournait la copie echouait bruyamment au
rem milieu d'un build par ailleurs bon. Pour une dist :
rem     set "SEMIRPROD_SKIP_OBS_COPY=1" && .\build_plugin.bat
rem
rem Drapeau separe plutot que `set "OBS_INSTALL="` : en cmd, affecter une chaine
rem vide SUPPRIME la variable, donc un `if not defined` la remettrait a sa valeur
rem par defaut — exactement l'inverse de ce que l'appelant demandait.
if not defined SEMIRPROD_SKIP_OBS_COPY set "SEMIRPROD_SKIP_OBS_COPY=0"
set "SEMIRPROD_SKIP_OBS_COPY=%SEMIRPROD_SKIP_OBS_COPY: =%"

rem Plugin TLS Qt — OBS utilise Schannel (TLS natif Windows), pas OpenSSL.
rem Sans ce plugin, Qt dit "No functional TLS backend" et HTTPS echoue.
set "QT_TLS_PLUGIN=%OBS_DEPS%\..\obs-deps-qt6-2025-08-23-x64\plugins\tls\qschannelbackend.dll"

rem Laisse vide pour auto-detect, OU mets le nom exact si tu veux forcer
set "DLL_NAME="

rem Ce dossier DOIT matcher le nom de la DLL sans .dll (Windows)
rem CMakeLists.txt -> OUTPUT_NAME "semirprod-studio"
set "MODULE_DIR=semirprod-studio"

set "CMAKE_GEN=Visual Studio 17 2022"
set "CMAKE_ARCH=x64"

echo.
echo ========================================
echo   Build plugin (module %MODULE_DIR%)
echo ========================================
echo.

set "ROOT=%cd%"

rem === VALIDATION DEPENDANCES ===
if not exist "%QT_DIR%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo [ERREUR] Qt6 introuvable a "%QT_DIR%".
    echo          Installe Qt 6.8.x via Qt Online Installer, ou modifie QT_DIR en haut du script.
    exit /b 1
)
rem NOTE: Qt6 WebSockets n'est PLUS requis — le plugin implemente RFC 6455 sur
rem Qt6::Network (QSslSocket / QTcpServer), modules fournis par OBS. Plus aucun
rem Qt6WebSockets.dll embarque = plus de casse a chaque bump Qt d'OBS.
if not exist "%OBS_BUILD%\libobs\libobsConfig.cmake" (
    echo [ERREUR] libobsConfig.cmake introuvable a "%OBS_BUILD%\libobs".
    echo          Verifie que obs-studio est bien compile.
    exit /b 1
)
if not exist "%OBS_BUILD%\frontend\api\obs-frontend-apiConfig.cmake" (
    echo [ERREUR] obs-frontend-apiConfig.cmake introuvable a "%OBS_BUILD%\frontend\api".
    exit /b 1
)

rem === GARDE-FOU DE COMPATIBILITE ===
rem Lit la version d'API reellement declaree par le SDK et la version de Qt
rem reellement pointee, et compare au plancher annonce. Un plancher qui monte
rem par accident ne se voit PAS a la compilation : le binaire se construit, et
rem c'est seulement chez l'utilisateur qu'OBS refuse de le charger.
set "SDK_MAJOR="
set "SDK_MINOR="
for /f "tokens=3" %%v in ('findstr /r /c:"define LIBOBS_API_MAJOR_VER" "%OBS_SRC%\libobs\obs-config.h"') do set "SDK_MAJOR=%%v"
for /f "tokens=3" %%v in ('findstr /r /c:"define LIBOBS_API_MINOR_VER" "%OBS_SRC%\libobs\obs-config.h"') do set "SDK_MINOR=%%v"
rem "...\6.8.3\msvc2022_64\.." se normalise en "...\6.8.3" : ~nx en extrait 6.8.3.
for %%d in ("%QT_DIR%\..") do set "QT_VER=%%~nxd"
for /f "tokens=1,2 delims=." %%a in ("%QT_VER%") do set "QT_MM=%%a.%%b"

echo [INFO] Plancher declare : OBS %FLOOR_OBS% / Qt %FLOOR_QT%
echo [INFO] SDK libobs        : %SDK_MAJOR%.%SDK_MINOR%   (%OBS_SRC%)
echo [INFO] Qt de compilation : %QT_VER%

if not "%SDK_MAJOR%.%SDK_MINOR%"=="%FLOOR_OBS%" (
    echo [ERREUR] Le SDK libobs est en %SDK_MAJOR%.%SDK_MINOR%, le plancher annonce est %FLOOR_OBS%.
    echo          Compiler ainsi change la version minimale d'OBS requise SANS
    echo          aucun signe a la compilation : l'echec n'apparait que chez
    echo          l'utilisateur, en "Module not loaded" ou "compiled with newer libobs".
    echo          Soit remets %OBS_SRC% sur un arbre %FLOOR_OBS%, soit assume le
    echo          nouveau plancher : FLOOR_OBS ici, le README et installer.iss.
    exit /b 1
)
if not "%QT_MM%"=="%FLOOR_QT%" (
    echo [ERREUR] QT_DIR pointe sur Qt %QT_VER%, le plancher annonce est Qt %FLOOR_QT%.x.
    echo          Rappel : OBS 32.0/32.1 livrent Qt 6.8.3, OBS 32.2+ livre 6.11.1.
    echo          Compiler avec un Qt plus recent rend le DLL inchargeable sur les
    echo          OBS plus anciens.
    exit /b 1
)

if not exist "build" goto rmdir_done
echo [INFO] Suppression de l'ancien dossier build...
set "RM_TRIES=0"
:retry_rmdir
rmdir /S /Q "build" 2>nul
if not exist "build" goto rmdir_done
set /a RM_TRIES+=1
if !RM_TRIES! GEQ 8 (
    echo [ERREUR] Impossible de supprimer build\ apres 8 tentatives.
    echo          Windows Defender / antivirus / Explorer tient un handle.
    echo          Ajoute une exclusion AV sur ce dossier ou reessaie dans 10s.
    exit /b 1
)
ping -n 2 127.0.0.1 >nul
goto retry_rmdir
:rmdir_done

mkdir "build" || (echo [ERREUR] Impossible de creer build & exit /b 1)
pushd "build"

echo [INFO] Configuration CMake...
rem Edition publique, explicitement : ce script ne produit que le binaire du
rem poste. L'edition cloud se compile sous Linux (packaging/cloud).
rem CMake n'aime pas les backslashes dans les chemins (escape \U, \D, etc.) : on convertit en /
set "QT_DIR_F=%QT_DIR:\=/%"
set "OBS_SRC_F=%OBS_SRC:\=/%"
set "OBS_BUILD_F=%OBS_BUILD:\=/%"
set "OBS_DEPS_F=%OBS_DEPS:\=/%"

cmake .. -G "%CMAKE_GEN%" -A %CMAKE_ARCH% -DCMAKE_PREFIX_PATH="%QT_DIR_F%;%OBS_BUILD_F%/libobs;%OBS_BUILD_F%/frontend/api;%OBS_BUILD_F%/deps/w32-pthreads;%OBS_DEPS_F%" -DCMAKE_MODULE_PATH="%OBS_SRC_F%/cmake/finders" -Dlibobs_DIR="%OBS_BUILD_F%/libobs" -Dobs-frontend-api_DIR="%OBS_BUILD_F%/frontend/api" -DSEMIRPROD_EDITION=public
if errorlevel 1 (
    echo [ERREUR] Echec configuration CMake
    popd & exit /b 1
)

rem Optionnellement active le mode dev (env-var overrides actifs)
if defined SEMIRPROD_DEV_BUILD_FLAG if "%SEMIRPROD_DEV_BUILD_FLAG%"=="1" (
    echo [INFO] SEMIRPROD_DEV_BUILD actif — reconfigure avec le flag...
    cmake . -DSEMIRPROD_DEV_BUILD=ON
    if errorlevel 1 (
        echo [ERREUR] Echec reconfiguration dev build
        popd & exit /b 1
    )
)

rem Sauvegarde/import de collections. ON par defaut cote CMake : on ne
rem reconfigure ici que pour la RETIRER, quand le backend vise n'a pas encore
rem les routes `/api/obs/scene-collections`.
rem     set "SEMIRPROD_COLLECTIONS_FLAG=0" && .\build_plugin.bat
if "%SEMIRPROD_COLLECTIONS_FLAG%"=="0" (
    echo [INFO] COLLECTIONS desactivees — reconfigure sans la fonction...
    cmake . -DSEMIRPROD_COLLECTIONS=OFF
    if errorlevel 1 (
        echo [ERREUR] Echec reconfiguration sans collections
        popd & exit /b 1
    )
)

echo [INFO] Compilation...
cmake --build . --config RelWithDebInfo
if errorlevel 1 (
    echo [ERREUR] Echec compilation
    popd & exit /b 1
)

set "BUILD_OUT=%cd%\RelWithDebInfo"

rem ====== Résolution du nom de DLL ======
if not defined DLL_NAME (
    for %%F in ("%BUILD_OUT%\*.dll") do (
        set "FOUND_DLL=%%~nxF"
    )
    if not defined FOUND_DLL (
        echo [ERREUR] Aucune DLL trouvee dans "%BUILD_OUT%"
        popd & exit /b 1
    )
    set "DLL_NAME=!FOUND_DLL!"
)

set "DLL_PATH=%BUILD_OUT%\%DLL_NAME%"
if not exist "%DLL_PATH%" (
    echo [ERREUR] DLL introuvable : "%DLL_PATH%"
    echo          -^> Verifie le nom genere par CMake / DLL_NAME.
    popd & exit /b 1
)

popd

rem ====== Prépare dist ======
if exist "dist" (
    echo [INFO] Suppression de l'ancien dossier dist...
    rmdir /S /Q "dist"
)

echo [INFO] Creation arborescence dist...
mkdir "dist\obs-plugins\64bit" || (echo [ERREUR] Creation dist plugins & exit /b 1)
mkdir "dist\data\obs-plugins\%MODULE_DIR%" || (echo [ERREUR] Creation dist data & exit /b 1)

set "DIST_PLUGIN_DIR=%ROOT%\dist\obs-plugins\64bit"
set "DIST_DATA_DIR=%ROOT%\dist\data\obs-plugins\%MODULE_DIR%"

echo [INFO] Copie DLL -^> dist...
copy /Y "%DLL_PATH%" "%DIST_PLUGIN_DIR%" >nul || (echo [ERREUR] Copie DLL vers dist & exit /b 1)
echo [SUCCES] DLL: "%DIST_PLUGIN_DIR%\%DLL_NAME%"

rem Qt6WebSockets.dll : PLUS embarque (le plugin ne l'utilise plus). C'etait
rem la cause du "Module not loaded" apres chaque maj Qt d'OBS.

rem Backend TLS Qt Schannel : embarque en FALLBACK seulement (dist\bin\64bit\tls),
rem pour une install OBS fraiche qui n'en aurait pas. A l'installation on ne
rem l'ecrit QUE s'il est absent (voir plus bas + installer.iss onlyifdoesntexist) :
rem on n'ecrase JAMAIS le backend d'OBS, qui matche toujours sa version de Qt.
rem Un backend Qt est charge via QPluginLoader (compat minor-forward), donc un
rem 6.8.x reste valide dans un OBS 6.11+ — contrairement a Qt6WebSockets.dll.
mkdir "%ROOT%\dist\bin\64bit\tls" >nul 2>&1
if exist "%QT_TLS_PLUGIN%" (
    echo [INFO] Copie qschannelbackend.dll -^> dist\bin\64bit\tls ^(fallback^)...
    copy /Y "%QT_TLS_PLUGIN%" "%ROOT%\dist\bin\64bit\tls\" >nul
) else (
    echo [WARN] qschannelbackend.dll introuvable a "%QT_TLS_PLUGIN%" ^(fallback indisponible^)
)

if exist "%ROOT%\resources\fallback.png" (
    echo [INFO] Copie fallback.png -^> dist...
    copy /Y "%ROOT%\resources\fallback.png" "%DIST_DATA_DIR%" >nul
) else (
    echo [WARN] resources\fallback.png introuvable
)

rem Textes localises (FR/EN) : data\locale\*.ini, lus par obs_module_text() dans
rem la langue d'OBS. Sans eux l'ecran affiche des cles.
if exist "%ROOT%\data\locale" (
    echo [INFO] Copie locale\*.ini -^> dist...
    mkdir "%DIST_DATA_DIR%\locale" >nul 2>&1
    copy /Y "%ROOT%\data\locale\*.ini" "%DIST_DATA_DIR%\locale\" >nul
) else (
    echo [WARN] data\locale introuvable
)

if exist "%ROOT%\resources\config.json" (
    echo [INFO] Copie config.json -^> dist...
    copy /Y "%ROOT%\resources\config.json" "%DIST_DATA_DIR%" >nul
) else (
    echo [WARN] resources\config.json introuvable
)

rem Licence : la GPL impose que le texte et l'adresse du code source accompagnent
rem chaque copie distribuee. Le zip part sans eux = zip non publiable, donc erreur.
copy /Y "%ROOT%\LICENSE" "%DIST_DATA_DIR%" >nul || (echo [ERREUR] Copie LICENSE & exit /b 1)
copy /Y "%ROOT%\THIRD_PARTY_NOTICES.md" "%DIST_DATA_DIR%" >nul || (echo [ERREUR] Copie THIRD_PARTY_NOTICES.md & exit /b 1)
copy /Y "%ROOT%\terms.txt" "%DIST_DATA_DIR%\NOTICE.txt" >nul || (echo [ERREUR] Copie NOTICE.txt & exit /b 1)

rem ====== Copie vers OBS build local (rundir) ======
set "OBS_PLUGIN_DIR=%OBS_BUILD%\rundir\RelWithDebInfo\obs-plugins\64bit"
set "OBS_DATA_DIR=%OBS_BUILD%\rundir\RelWithDebInfo\data\obs-plugins\%MODULE_DIR%"

echo [INFO] Copie dans build OBS local...
if exist "%OBS_PLUGIN_DIR%" (
    copy /Y "%DLL_PATH%" "%OBS_PLUGIN_DIR%" >nul
) else (
    echo [WARN] Dossier plugins OBS introuvable: "%OBS_PLUGIN_DIR%"
)

if not exist "%OBS_DATA_DIR%" mkdir "%OBS_DATA_DIR%" >nul 2>&1

if exist "%ROOT%\resources\fallback.png" copy /Y "%ROOT%\resources\fallback.png" "%OBS_DATA_DIR%" >nul
if exist "%ROOT%\resources\config.json" copy /Y "%ROOT%\resources\config.json" "%OBS_DATA_DIR%" >nul
if exist "%ROOT%\data\locale" (
    if not exist "%OBS_DATA_DIR%\locale" mkdir "%OBS_DATA_DIR%\locale" >nul 2>&1
    copy /Y "%ROOT%\data\locale\*.ini" "%OBS_DATA_DIR%\locale\" >nul
)

rem ====== Copie vers OBS installe (optionnel) ======
rem
rem JAMAIS pour un build dev : il pointe sur staging, et cette copie ecrasait
rem silencieusement le plugin de production de l'OBS du poste. Symptome vecu le
rem 15 aout : « appairage echoue » — le binaire parlait a la pre-production,
rem ou le compte de production n'existe pas. Un build de test se deploie a la
rem main, sur le banc que l'on veut.
if "%SEMIRPROD_DEV_BUILD_FLAG%"=="1" (
    echo [INFO] Build dev : PAS de copie vers l'OBS installe ^(deploiement manuel^).
    goto :skip_obs_install
)
if "%SEMIRPROD_SKIP_OBS_COPY%"=="1" (
    echo [INFO] Copie vers l'OBS installe desactivee ^(SEMIRPROD_SKIP_OBS_COPY=1^).
    goto :skip_obs_install
)
if defined OBS_INSTALL if exist "%OBS_INSTALL%\obs-plugins\64bit" (
    echo [INFO] Copie dans OBS installe: %OBS_INSTALL%
    copy /Y "%DLL_PATH%" "%OBS_INSTALL%\obs-plugins\64bit\" >nul
    if errorlevel 1 (
        echo [ERREUR] Copie DLL vers OBS_INSTALL echouee.
        echo          Ferme OBS ^(fichier locke^) et relance le script.
    ) else (
        rem NE PAS embarquer Qt6WebSockets.dll (plus utilise) et si un vieux
        rem exemplaire traine dans OBS, le supprimer : il ferait echouer le
        rem chargement du plugin (mismatch Qt).
        if exist "%OBS_INSTALL%\obs-plugins\64bit\Qt6WebSockets.dll" del /Q "%OBS_INSTALL%\obs-plugins\64bit\Qt6WebSockets.dll" >nul 2>&1
        rem Backend TLS : ne PAS ecraser celui d'OBS. On ne le pose que s'il manque.
        if not exist "%OBS_INSTALL%\bin\64bit\tls" mkdir "%OBS_INSTALL%\bin\64bit\tls" >nul 2>&1
        if not exist "%OBS_INSTALL%\bin\64bit\tls\qschannelbackend.dll" if exist "%QT_TLS_PLUGIN%" copy /Y "%QT_TLS_PLUGIN%" "%OBS_INSTALL%\bin\64bit\tls\" >nul 2>&1
        if not exist "%OBS_INSTALL%\data\obs-plugins\%MODULE_DIR%" mkdir "%OBS_INSTALL%\data\obs-plugins\%MODULE_DIR%" >nul 2>&1
        if exist "%ROOT%\resources\fallback.png" copy /Y "%ROOT%\resources\fallback.png" "%OBS_INSTALL%\data\obs-plugins\%MODULE_DIR%\" >nul
        if exist "%ROOT%\resources\config.json" copy /Y "%ROOT%\resources\config.json" "%OBS_INSTALL%\data\obs-plugins\%MODULE_DIR%\" >nul
        if exist "%ROOT%\data\locale" (
            if not exist "%OBS_INSTALL%\data\obs-plugins\%MODULE_DIR%\locale" mkdir "%OBS_INSTALL%\data\obs-plugins\%MODULE_DIR%\locale" >nul 2>&1
            copy /Y "%ROOT%\data\locale\*.ini" "%OBS_INSTALL%\data\obs-plugins\%MODULE_DIR%\locale\" >nul
        )
        echo [SUCCES] Copie OBS installe: %OBS_INSTALL%\obs-plugins\64bit\%DLL_NAME%
    )
)
:skip_obs_install

rem ====== Lecture de la version (single source: CMakeLists.txt) ======
rem CMake ecrit PLUGIN_VERSION.txt au configure ; on relit ici pour
rem nommer le zip sans dupliquer la version dans plusieurs fichiers.
set "PLUGIN_VERSION="
if exist "%ROOT%\build\PLUGIN_VERSION.txt" (
    set /p PLUGIN_VERSION=<"%ROOT%\build\PLUGIN_VERSION.txt"
)
rem set /p peut laisser des CR/espaces — nettoie au cas ou
if defined PLUGIN_VERSION set "PLUGIN_VERSION=%PLUGIN_VERSION: =%"
if not defined PLUGIN_VERSION (
    echo [WARN] Version introuvable dans build\PLUGIN_VERSION.txt — fallback "unknown"
    set "PLUGIN_VERSION=unknown"
)

rem Un build dev ne doit JAMAIS pouvoir etre distribue par inadvertance : il
rem accepte les variables SEMIRPROD_*_BASE et les hotes de staging. Son archive
rem porte donc un nom qui le dit, au lieu de se confondre avec la release.
if "%SEMIRPROD_DEV_BUILD_FLAG%"=="1" (
    set "ZIP_NAME=obs-semirprod-plugin-v%PLUGIN_VERSION%-DEV-staging.zip"
) else (
    set "ZIP_NAME=obs-semirprod-plugin-v%PLUGIN_VERSION%.zip"
)
set "ZIP_PATH=%ROOT%\%ZIP_NAME%"

if exist "%ZIP_PATH%" (
    echo [INFO] Suppression de l'ancien zip "%ZIP_NAME%"...
    del /Q "%ZIP_PATH%"
)

echo [INFO] Creation du zip "%ZIP_NAME%"...
rem Le zip est destine a un GLISSER-DEPOSER dans le dossier d'OBS : il ne doit
rem donc contenir QUE ce qui nous appartient, c'est-a-dire la DLL du plugin.
rem
rem Il zippait dist\* en entier, dont bin\64bit\tls\qschannelbackend.dll : extrait
rem par-dessus une installation d'OBS, il ECRASAIT le backend TLS d'OBS par notre
rem copie Qt 6.8.3. L'installeur, lui, s'en protege avec `onlyifdoesntexist` — un
rem glisser-deposer n'a aucune garde. Et data\...\fallback.png ne sert plus a rien
rem (plus aucune reference dans le code).
rem
rem Le repli TLS reste donc reserve a l'installeur, ou la garde existe.
rem
rem dist\data est zippe AVEC la DLL : il porte data\obs-plugins\<module>\locale\*.ini,
rem les textes FR/EN lus par obs_module_text(). Une DLL deposee seule affiche des
rem cles a la place du texte des docks et du Journal.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%ROOT%\dist\obs-plugins','%ROOT%\dist\data' -DestinationPath '%ZIP_PATH%' -Force" 1>nul
if errorlevel 1 (
    echo [ERREUR] Echec creation du zip
    exit /b 1
)
echo [SUCCES] Zip cree: %ZIP_PATH%

rem ====== INSTALLEUR + EMPREINTES (chemin de mise a jour) ======
rem
rem Ce que le serveur attend pour publier une version (ecran admin > OBS plugin) :
rem   * un installeur PAR PLATEFORME      -> type=installer, platform=windows
rem   * une archive de source facultative -> type=source
rem
rem Le SHA-256 affiche ici ne se DECLARE pas au serveur : il le recalcule au
rem depot, puis le signe avec le reste du manifeste. Il sert a verifier qu'on a
rem publie le fichier qu'on vient de construire. Deux artefacts differents sous
rem un meme numero de version est exactement le defaut qu'il rend visible.

rem ISCC est facultatif : sans Inno Setup, on s'arrete au zip.
set "ISCC_EXE="
for %%i in (ISCC.exe) do if not defined ISCC_EXE if not "%%~$PATH:i"=="" set "ISCC_EXE=%%~$PATH:i"
if not defined ISCC_EXE if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC_EXE=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC_EXE if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC_EXE=%ProgramFiles%\Inno Setup 6\ISCC.exe"

set "INSTALLER_PATH="
rem SEMIRPROD_DEV_INSTALLER=1 : la CI du depot public en fabrique un pour tester
rem installation et mise a jour sur staging. Il ne part que vers l admin de staging.
set "SKIP_DEV_INSTALLER=0"
if "%SEMIRPROD_DEV_BUILD_FLAG%"=="1" if not "%SEMIRPROD_DEV_INSTALLER%"=="1" set "SKIP_DEV_INSTALLER=1"
if "%SKIP_DEV_INSTALLER%"=="1" (
    echo [INFO] Build DEV : installeur non fabrique ^(il ne doit pas etre publie^).
) else if defined ISCC_EXE (
    echo [INFO] Fabrication de l'installeur avec "%ISCC_EXE%"...
    rem /DMyAppVersion : la version vient de CMakeLists, jamais du .iss. Sans ce
    rem passage, le .iss retombe sur build\PLUGIN_VERSION.txt - meme source.
    "%ISCC_EXE%" /Q "/DMyAppVersion=%PLUGIN_VERSION%" "%ROOT%\packaging\windows\installer.iss"
    if errorlevel 1 (
        echo [ERREUR] Echec de la fabrication de l'installeur.
        exit /b 1
    )
    set "INSTALLER_PATH=%ROOT%\semirprod-plugin-installer-%PLUGIN_VERSION%.exe"
    echo [SUCCES] Installeur cree: !INSTALLER_PATH!
) else (
    echo [INFO] Inno Setup introuvable - pas d'installeur. Le zip seul ne se
    echo        publie PAS comme installeur Windows ^(extensions admises : .exe, .msi^).
)

echo.
echo ====== A PUBLIER ^(admin ^> OBS plugin ^> version %PLUGIN_VERSION%^) ======
if defined INSTALLER_PATH if exist "!INSTALLER_PATH!" (
    echo [installer/windows] !INSTALLER_PATH!
    for /f "skip=1 delims=" %%h in ('certutil -hashfile "!INSTALLER_PATH!" SHA256') do if not defined INSTALLER_SHA set "INSTALLER_SHA=%%h"
    echo     sha256 : !INSTALLER_SHA: =!
)
echo [source]            %ZIP_PATH%
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%ZIP_PATH%" SHA256') do if not defined ZIP_SHA set "ZIP_SHA=%%h"
echo     sha256 : !ZIP_SHA: =!
echo.
rem Rappel du reste de la chaine, qui ne depend PAS de ce script : son absence
rem ne se voit que dans le journal d'OBS d'un client.
echo [RAPPEL] Publier ne suffit pas : le manifeste doit etre SIGNE cote serveur
echo          ^(cle de signature des mises a jour^). Sans signature, le plugin
echo          refuse le manifeste et personne n'est jamais averti.

echo.
echo ========================================
echo   Build termine ! (v%PLUGIN_VERSION%)
echo   dist\obs-plugins  et  dist\data  prets pour OBS installe
echo   Archive : %ZIP_NAME%
echo   Copie locale (si chemins valides) :
echo     %OBS_PLUGIN_DIR%
echo     %OBS_DATA_DIR%
echo ========================================
echo.

pause
endlocal
