; WindiroOS — installer Windows (NSIS)
; Costruito da macOS con: makensis -DVERSION=... -DOUTDIR=... -DROOTDIR=... windiroos.nsi

!include "MUI2.nsh"

!define APP_NAME "WindiroOS"
!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!ifndef OUTDIR
  !define OUTDIR "."
!endif

Name "${APP_NAME}"
OutFile "${OUTDIR}/WindiroOS-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\WindiroOS"
RequestExecutionLevel user
Unicode True

!define MUI_ICON "${OUTDIR}/assets/app.ico"
!define MUI_UNICON "${OUTDIR}/assets/app.ico"

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Italian"

Section "WindiroOS" SEC_MAIN
    SetOutPath "$INSTDIR"
    ; launcher + ISO + QEMU
    File /r "${OUTDIR}\payload\*.*"

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    CreateDirectory "$SMPROGRAMS\WindiroOS"
    CreateShortCut "$SMPROGRAMS\WindiroOS\WindiroOS.lnk" "$INSTDIR\windiroos.exe"
    CreateShortCut "$SMPROGRAMS\WindiroOS\Disinstalla.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortCut "$DESKTOP\WindiroOS.lnk" "$INSTDIR\windiroos.exe"

    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindiroOS" "DisplayName" "WindiroOS"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindiroOS" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindiroOS" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindiroOS" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindiroOS" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindiroOS" "NoRepair" 1
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\Uninstall.exe"
    Delete "$INSTDIR\windiroos.exe"
    Delete "$INSTDIR\viteza.iso"
    RMDir /r "$INSTDIR\qemu"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\WindiroOS\WindiroOS.lnk"
    Delete "$SMPROGRAMS\WindiroOS\Disinstalla.lnk"
    RMDir "$SMPROGRAMS\WindiroOS"
    Delete "$DESKTOP\WindiroOS.lnk"

    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindiroOS"
SectionEnd
