!include "MUI2.nsh"

!define VER_MAJOR 1
!define VER_MINOR 2
!define NAME "MDMX Fixture"
!define INPUT_FILE "MDMX_Fixture.ofx"
!define OUT_FILE "MDMX Fixture Installer.exe"

!define BUNDLE_DIR "$PROGRAMFILES64\Common Files\OFX\Plugins\${INPUT_FILE}.bundle"
!define CONTENT_DIR "${BUNDLE_DIR}\Contents\Win64"
!define REG_DIR "Software\Microsoft\Windows\CurrentVersion\Uninstall\${INPUT_FILE}"

!define UNINSTALLER_PATH "${BUNDLE_DIR}\Uninstall.exe"

!define HUMAN_VERSION "${VER_MAJOR}.${VER_MINOR}"
!define HUMAN_NAME "${NAME} v${HUMAN_VERSION}"

Name "${HUMAN_NAME}"
OutFile "${OUT_FILE}"
InstallDir "${CONTENT_DIR}"
RequestExecutionLevel admin

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  CreateDirectory "${CONTENT_DIR}"
  SetOutPath "${CONTENT_DIR}"
  
  File "/oname=${INPUT_FILE}" "..\mdmx-ofx-fixtures\x64\Release\mdmx-ofx-fixtures.dll"
  
  WriteUninstaller "${UNINSTALLER_PATH}"
  
  WriteRegStr   HKLM "${REG_DIR}" "DisplayName" "${HUMAN_NAME}"
  WriteRegStr   HKLM "${REG_DIR}" "DisplayVersion" "${HUMAN_VERSION}"
  WriteRegStr   HKLM "${REG_DIR}" "UninstallString" "$\"${UNINSTALLER_PATH}$\""
  WriteRegDWORD HKLM "${REG_DIR}" "NoModify" 1
  WriteRegDWORD HKLM "${REG_DIR}" "NoRepair" 1
SectionEnd

Section "Redistributables"
  File "resources\vc_redist.x64.exe"
  ExecWait "${CONTENT_DIR}\vc_redist.x64.exe /install /passive /norestart"
  Delete "${CONTENT_DIR}\vc_redist.x64.exe"
SectionEnd

Section "Uninstall"
  Delete "${CONTENT_DIR}\${INPUT_FILE}"
  Delete "${BUNDLE_DIR}\Uninstall.exe"

  SetOutPath "$TEMP"
  RMDir /r "${BUNDLE_DIR}"
  
  DeleteRegKey HKLM "${REG_DIR}"
  
  MessageBox MB_OK "${HUMAN_NAME} has been uninstalled."
SectionEnd
