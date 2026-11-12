; Per-user Windows installer for Turing Screen, built by .github/workflows/ci.yml.
;
; Example:
;   makensis /DAPP_VERSION=0.2.0 /DAPP_VERSION_QUAD=0.2.0.0 ^
;     /DSOURCE_DIR=dist\turing-screen-qt-0.2.0-windows-x64 ^
;     /DOUTPUT_FILE=dist\turing-screen-qt-0.2.0-windows-x64-installer.exe ^
;     scripts\windows-installer.nsi
;
; SOURCE_DIR must contain the fully deployed application (EXE, Qt DLLs, plugins,
; assets and documentation). LICENSE is read from the repository root.

Unicode true
SetCompressor /SOLID lzma

!ifndef APP_NAME
  !define APP_NAME "Turing Screen"
!endif
!ifndef APP_EXE
  !define APP_EXE "turing-screen-qt.exe"
!endif
!ifndef APP_VERSION
  !define APP_VERSION "0.2.0"
!endif
!ifndef APP_VERSION_QUAD
  !define APP_VERSION_QUAD "${APP_VERSION}.0"
!endif
!ifndef SOURCE_DIR
  !error "SOURCE_DIR is required; pass /DSOURCE_DIR=<staged application directory>"
!endif
!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "turing-screen-qt-${APP_VERSION}-windows-x64-installer.exe"
!endif
; Relative paths are resolved from this script's directory.
!ifndef LICENSE_FILE
  !define LICENSE_FILE "../LICENSE"
!endif

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\TuringScreenQt"

Name "${APP_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Turing Screen"
InstallDirRegKey HKCU "Software\TuringScreenQt" "InstallDir"
RequestExecutionLevel user
BrandingText "${APP_NAME} ${APP_VERSION}"

!include "MUI2.nsh"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APP_NAME}"
!define MUI_LANGDLL_REGISTRY_ROOT "HKCU"
!define MUI_LANGDLL_REGISTRY_KEY "Software\TuringScreenQt"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

VIProductVersion "${APP_VERSION_QUAD}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} installer"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

Section "Turing Screen" SEC_APP
  SectionIn RO
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  File /r "${SOURCE_DIR}/*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\Turing Screen"
  CreateShortcut "$SMPROGRAMS\Turing Screen\Turing Screen.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$SMPROGRAMS\Turing Screen\Uninstall Turing Screen.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\TuringScreenQt" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\TuringScreenQt" "Version" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Desktop shortcut" SEC_DESKTOP
  SetShellVarContext current
  CreateShortcut "$DESKTOP\Turing Screen.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_APP} "Install ${APP_NAME} for the current user."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} "Add a shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Function un.onInit
  !insertmacro MUI_UNGETLANGUAGE
FunctionEnd

Section "Uninstall"
  SetShellVarContext current
  Delete "$SMPROGRAMS\Turing Screen\Turing Screen.lnk"
  Delete "$SMPROGRAMS\Turing Screen\Uninstall Turing Screen.lnk"
  RMDir "$SMPROGRAMS\Turing Screen"
  Delete "$DESKTOP\Turing Screen.lnk"
  ; Remove the "start at sign-in" value so it cannot point at a deleted EXE.
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "TuringScreen"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "Software\TuringScreenQt"
  RMDir /r "$INSTDIR"
SectionEnd
