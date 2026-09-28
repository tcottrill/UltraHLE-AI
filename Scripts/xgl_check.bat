@echo off
rem Compile-only check of OpenGL backend files (no link).
rem Usage: Scripts\xgl_check.bat xgl_main.c xgl_fbo.c ...   (names relative to src\xgl)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if not exist "%TEMP%\xglcheck" mkdir "%TEMP%\xglcheck"
pushd "%~dp0..\src\xgl"
cl /nologo /c /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS /Fo"%TEMP%\xglcheck\\" %*
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
