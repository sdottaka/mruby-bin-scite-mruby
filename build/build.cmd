set PATH=%ProgramFiles%\Git\bin;%ProgramFiles(x86)%\Git\bin;%PATH%
call "%VS120COMNTOOLS%\vsvars32.bat"
set CFLAGS=/c /nologo /W3 /we4013 /Zi /MTd /Od /D_CRT_SECURE_NO_WARNINGS
set CXXFLAGS=/c /nologo /W3 /we4013 /Zi /MTd /Od /D_CRT_SECURE_NO_WARNINGS
set MRUBY_CONFIG=%~dp0\build_config.rb
call rake
pause
