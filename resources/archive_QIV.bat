@echo off
set "sevenzip=D:\08_PortablePrograms\peazip_portable-v11-WIN64\res\bin\7z\7z.exe"
set "base=I:\30_CppSources\QuickImageViewer"
set "archiveDir=%base%"

if not exist "%archiveDir%" mkdir "%archiveDir%"

"%sevenzip%" a -tzip -mtp=0 -mm=Deflate -mmt=8 -mmemuse=p50 -mx5 -mfb=32 -mpass=1 -sccUTF-8 -mcu=on -mem=AES256 -bb0 -bse0 -bsp2 ^
"-w%base%\" -snl -mtc=on -mta=on ^
"%archiveDir%\QIV_src.zip" ^
"%base%\src"