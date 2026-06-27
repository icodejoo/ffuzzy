call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
mkdir build 2>nul
cl.exe /std:c11 /utf-8 /W3 /Iinclude src\ffz_alloc.c src\ffz_chars.c src\ffz_class_table.c src\ffz_corpus.c src\ffz_fuzzy.c src\ffz_match.c src\ffz_pattern.c src\ffz_prefilter.c src\ffz_score.c src\ffz_string.c src\ffz_unicode_tables.c tests\test_ffz.c /Fe:build\test_ffz.exe
if %errorlevel% neq 0 exit /b %errorlevel%
build\test_ffz.exe
