;--------------------------------
Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!define APP_NAME "Mate++"
!define APP_EXE "mpp.exe"
!define APP_MANAGER "mppmgr.exe"
!define APP_VERSION "1.0.1"
!define APP_PUBLISHER "pmgdev64"
!define APP_ICON "D:\Codeblocks\MatePP\app.ico"
!define APP_SOURCE "D:\Codeblocks\Releases\matepp\at20260805-alpha-test-1.0.1"

; FIX: tên file cert public để cài vào Trusted Root. File này phải nằm
; trong ${APP_SOURCE} (được đóng gói cùng "File /r" ở SecMain) hoặc chỉ
; định đường dẫn riêng nếu để chỗ khác — đổi lại cho khớp file thật.
!define CERT_FILE "PmgTeam_Public_New.cer"
!define CERT_SUBJECT "PmgTeam"

;--------------------------------
Name "${APP_NAME}"
OutFile "Mate++_Setup_v${APP_VERSION}.exe"
InstallDir "$PROFILE\${APP_NAME}"
RequestExecutionLevel user

SetCompressor /FINAL lzma
SetCompressorDictSize 64

;--------------------------------
!define MUI_ABORTWARNING
!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${APP_SOURCE}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Vietnamese"

!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APP_NAME}"
!define MUI_FINISHPAGE_LINK "GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/pmgdev64/matepp"

;--------------------------------
Section "Main Application" SecMain
  SectionIn RO
  SetOutPath $INSTDIR
  File /r "${APP_SOURCE}\*"

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME} Manager.lnk" "$INSTDIR\${APP_MANAGER}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "Publisher" "${APP_PUBLISHER}"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "EstimatedSize" "$0"
SectionEnd

; FIX: section riêng để cài cert vào Trusted Root. Việc này BẮT BUỘC cần
; quyền admin (Cert:\LocalMachine\Root không ghi được ở quyền user), nên
; installer chính vẫn giữ RequestExecutionLevel user (không cần admin cho
; phần copy file/tạo shortcut), chỉ elevate RIÊNG bước certutil này qua
; ShellExecute verb "runas" — UAC prompt chỉ hiện đúng lúc cần, không bắt
; UAC ngay từ đầu installer.
Section "Install Trusted Certificate" SecCert
  SetOutPath $INSTDIR
  ; certutil đã có sẵn trong Windows (System32), không cần đóng gói riêng.
  ; -f: ghi đè nếu cert cùng thumbprint đã tồn tại (tránh lỗi khi cài lại/update).
  ExecShellWait "runas" "certutil.exe" \
    '-addstore -f Root "$INSTDIR\${CERT_FILE}"' SW_HIDE

  ; ExecShellWait không trả về exit code của tiến trình con qua $0 một cách
  ; đáng tin cậy (chỉ báo ShellExecute có launch được không) — không nên
  ; dựa vào $0 để suy ra certutil thành/bại. Nếu người dùng bấm "No" ở UAC,
  ; certutil đơn giản là không chạy được; im lặng bỏ qua, không Abort cả
  ; installer chỉ vì bước cài cert (không bắt buộc) bị từ chối.
SectionEnd

Section "Start Menu Shortcuts" SecStartMenu
SectionEnd

Section "Desktop Shortcut" SecDesktop
SectionEnd

;--------------------------------
Section "Uninstall"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME} Manager.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

  ; FIX: gỡ cert khỏi Trusted Root khi uninstall — cũng cần elevate riêng,
  ; tương tự lúc cài. Match theo subject name (-delstore chấp nhận chuỗi
  ; con khớp CN) thay vì thumbprint, vì thumbprint có thể khác nhau giữa
  ; các lần tạo cert lại (cert cũ bị mất pass, cert mới, v.v.).
  ExecShellWait "runas" "certutil.exe" \
    '-delstore Root "${CERT_SUBJECT}"' SW_HIDE

  RMDir /r "$INSTDIR"
SectionEnd

;--------------------------------
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "Install ${APP_NAME} ${APP_VERSION}"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCert} "Install PmgTeam publisher certificate to Trusted Root (requires admin — prevents Windows SmartScreen/Unknown Publisher warnings)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Create Start Menu shortcuts"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Create Desktop shortcut"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
Function .onInit
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayName"
  StrCmp $0 "${APP_NAME}" 0 +2
  MessageBox MB_YESNO|MB_ICONQUESTION "${APP_NAME} is already installed.$\nUninstall old version?" IDNO +2
  ExecWait '"$INSTDIR\Uninstall.exe" _?=$INSTDIR'
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Function un.onInit
  !insertmacro MUI_UNGETLANGUAGE
  MessageBox MB_YESNO|MB_ICONQUESTION "Uninstall ${APP_NAME}?" IDYES +2
  Abort
FunctionEnd
