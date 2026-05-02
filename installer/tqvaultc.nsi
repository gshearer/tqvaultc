; NSIS installer script for TQVaultC.
;
; Built with `makensis` from a Linux host (or wine on Windows). Expects the
; `dist-win/` tree to already exist — produced by scripts/package-windows.sh.
;
; Variables passed in via /D on the command line:
;   APP_VERSION   — e.g. "0.5"
;   DIST_DIR      — path to the staged dist tree (default: dist-win)
;   OUTPUT_FILE   — path of the .exe to produce (default: tqvaultc-setup.exe)

!ifndef APP_VERSION
  !define APP_VERSION "0.0"
!endif
!ifndef DIST_DIR
  !define DIST_DIR "dist-win"
!endif
!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "tqvaultc-setup.exe"
!endif

!define APP_NAME    "TQVaultC"
!define APP_PUBLISHER "TQVaultC"
!define APP_EXE     "tqvaultc.exe"
!define UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

SetCompressor /SOLID lzma

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUTPUT_FILE}"
; Per-user install: no UAC prompt, no admin needed, lands in
; %LOCALAPPDATA%\Programs\TQVaultC and registers the uninstaller in HKCU.
; Right approach for a single-user game tool.
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel user
Unicode true

!include "MUI2.nsh"

!define MUI_ICON "tqvaultc.ico"
!define MUI_UNICON "tqvaultc.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"

  ; Stage the entire dist tree (bin/ + share/)
  File /r "${DIST_DIR}\*.*"

  ; Start menu shortcut
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut  "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" \
                  "$INSTDIR\bin\${APP_EXE}" "" "$INSTDIR\bin\${APP_EXE}" 0
  CreateShortCut  "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk" \
                  "$INSTDIR\uninstall.exe"

  ; Desktop shortcut
  CreateShortCut  "$DESKTOP\${APP_NAME}.lnk" \
                  "$INSTDIR\bin\${APP_EXE}" "" "$INSTDIR\bin\${APP_EXE}" 0

  ; Add/Remove Programs entry
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\bin\${APP_EXE}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

  WriteRegStr HKCU "Software\${APP_NAME}" "InstallDir" "$INSTDIR"

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"

  ; Wipe install dir contents (RMDir /r is recursive)
  RMDir /r "$INSTDIR"

  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\${APP_NAME}"
SectionEnd
