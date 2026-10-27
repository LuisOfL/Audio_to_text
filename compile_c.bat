@echo off
echo Compilando librerias en C...

gcc -shared -O3 -o libwav_extractor.dll wav_extractor.c
if %errorlevel% neq 0 (
    echo Error al compilar wav_extractor.c
    exit /b %errorlevel%
)
echo [OK] libwav_extractor.dll compilada con exito.

gcc -shared -O3 -fopenmp -static-libgcc -static-libstdc++ ^
    -I./include -I./ggml/include wav_to_text.c ^
    -L./build/src -L./build/ggml/src ^
    -l:libwhisper.a -l:ggml.a -l:ggml-base.a -l:ggml-cpu.a -lstdc++ ^
    -o libwav_to_text.dll

if %errorlevel% neq 0 (
    echo Error al compilar wav_to_text.c
    exit /b %errorlevel%
)
echo [OK] libwav_to_text.dll compilada con exito.
echo Compilacion terminada exitosamente.
