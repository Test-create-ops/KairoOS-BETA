; KairoOS — installer Windows (NSIS)
; Costruito da macOS con: makensis -DVERSION=... -DOUTDIR=... -DROOTDIR=... kairoos.nsi

!include "MUI2.nsh"

!define APP_NAME "KairoOS"
!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!ifndef OUTDIR
  !define OUTDIR "."
!endif

Name "${APP_NAME}"
OutFile "${OUTDIR}/KairoOS-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\KairoOS"
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

Section "KairoOS" SEC_MAIN
    SetOutPath "$INSTDIR"
    ; launcher + ISO + QEMU
    File /r "${OUTDIR}\payload\*.*"

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    CreateDirectory "$SMPROGRAMS\KairoOS"
    CreateShortCut "$SMPROGRAMS\KairoOS\KairoOS.lnk" "$INSTDIR\kairoos.exe"
    CreateShortCut "$SMPROGRAMS\KairoOS\Disinstalla.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortCut "$DESKTOP\KairoOS.lnk" "$INSTDIR\kairoos.exe"

    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KairoOS" "DisplayName" "KairoOS"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KairoOS" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KairoOS" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KairoOS" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KairoOS" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KairoOS" "NoRepair" 1
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\Uninstall.exe"
    Delete "$INSTDIR\kairoos.exe"
    Delete "$INSTDIR\viteza.iso"
    RMDir /r "$INSTDIR\qemu"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\KairoOS\KairoOS.lnk"
    Delete "$SMPROGRAMS\KairoOS\Disinstalla.lnk"
    RMDir "$SMPROGRAMS\KairoOS"
    Delete "$DESKTOP\KairoOS.lnk"

    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KairoOS"
SectionEnd
