@echo off
setlocal
rem ==================================================================
rem  FNK0104N-project naar GitHub zetten (openbaar)
rem  Staat in: ...\Arduino\FNK0104N   -  dubbelklikken.
rem  Stap 1 is een droogloop: je ziet eerst welke bestanden meegaan.
rem  De repository bestaat al op github.com (leeg).
rem ==================================================================
set "GHUSER=fvdschrier-creator"
set "REPO=freenove-fnk0104n-esp32-s3-case-and-schematics"
set "LOG=%~dp0publiceren_log.txt"
cd /d "%~dp0"
echo Map: %CD%
echo Repository: https://github.com/%GHUSER%/%REPO%
echo.

where git >nul 2>nul
if errorlevel 1 goto :geengit

if exist ".git" goto :heeftgit
git init -b main
if errorlevel 1 goto :fout
:heeftgit

rem --- naam en e-mail voor commits (alleen vragen als ze ontbreken) ---
git config user.name >nul 2>nul
if not errorlevel 1 goto :naamok
set /p GN=Naam voor commits (bijv. Frans): 
git config user.name "%GN%"
:naamok
git config user.email >nul 2>nul
if not errorlevel 1 goto :mailok
echo Tip: gebruik je GitHub "noreply"-adres (GitHub > Settings > Emails).
set /p GE=E-mail voor commits: 
git config user.email "%GE%"
:mailok

rem --- STAP 1: droogloop ---
git add -A
echo.
echo ===== Bestanden in de eerste versie (al vastgelegd) =====
git ls-tree -r --name-only HEAD 2>nul
echo ===== Nieuwe of gewijzigde bestanden (nog niet vastgelegd) =====
git status --short
echo ==============================================================
git ls-tree -r --name-only HEAD > "%LOG%" 2>nul
git status --short >> "%LOG%"
echo.
choice /c JN /m "Klopt dit? J = vastleggen en uploaden, N = stoppen"
if errorlevel 2 goto :stop

rem --- STAP 2: commit (alleen als er iets nieuws is) ---
git diff --cached --quiet
if not errorlevel 1 goto :geencommit
git commit -q -m "Update: startmenu, Bin Maker, tests, behuizing en bouwgids" >> "%LOG%" 2>&1
:geencommit
git log --oneline -3

rem --- STAP 3: naar GitHub ---
git remote remove origin >nul 2>nul
git remote add origin https://github.com/%GHUSER%/%REPO%.git
git branch -M main
git push -u origin main
if errorlevel 1 goto :fout
goto :klaar

:geengit
echo [FOUT] git is niet gevonden. Installeer Git for Windows: https://git-scm.com/download/win
goto :einde
:stop
git reset -q
echo Gestopt. Er is niets vastgelegd of geupload. Lijst staat in publiceren_log.txt
goto :einde
:fout
echo [FOUT] Er ging iets mis. Kopieer de tekst hierboven en plak hem in de chat.
goto :einde
:klaar
echo.
echo KLAAR: https://github.com/%GHUSER%/%REPO%
:einde
echo.
pause
endlocal
