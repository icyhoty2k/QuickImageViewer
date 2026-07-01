@echo off
setlocal EnableDelayedExpansion

set "sevenzip=D:\08_PortablePrograms\peazip_portable-v11-WIN64\res\bin\7z\7z.exe"
set "base=I:\30_CppSources\QuickImageViewer"
set "src=%base%\src"
set "archiveDir=%base%\archive"
if not exist "%archiveDir%" mkdir "%archiveDir%"

set count=0
set part=1
set files=

cd /d "%base%"

for /r "src" %%F in (*) do (
    set "file=%%F"
    set "file=!file:%base%\=!"

    set /a count+=1
    set files=!files! "!file!"

    if !count! EQU 10 (
        "%sevenzip%" a -tzip -mtp=0 -mm=Deflate -mmt=8 -mmemuse=p50 -mx5 -mfb=32 -mpass=1 -sccUTF-8 -mcu=on -mem=AES256 -bb0 -bse0 -bsp2 ^
        "-w%base%\" -snl -mtc=on -mta=on ^
        "%archiveDir%\QIV_src_part!part!.zip" !files!

        set /a part+=1
        set count=0
        set files=
    )
)

if not "!files!"=="" (
    "%sevenzip%" a -tzip -mtp=0 -mm=Deflate -mmt=8 -mmemuse=p50 -mx5 -mfb=32 -mpass=1 -sccUTF-8 -mcu=on -mem=AES256 -bb0 -bse0 -bsp2 ^
    "-w%base%\" -snl -mtc=on -mta=on ^
    "%archiveDir%\QIV_src_part!part!.zip" !files!
)
ok and for one archive ?