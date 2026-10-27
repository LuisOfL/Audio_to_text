"""
Pipeline de Transcripción de Audio a Texto (Speech-to-Text)
------------------------------------------------------------
Este script implementa un pipeline completo en Python que invoca
las funciones optimizadas en C (compiladas en librerías dinámicas .dll/.so):
1. wav_extractor (C): Extrae el audio del archivo WAV, convierte a mono y 
   resamplea a 16 kHz PCM (requisito estándar para Whisper).
2. wav_to_text (C): Ejecuta la inferencia del modelo Whisper STT directamente
   en C/C++ y guarda la transcripción en un archivo .txt.
"""

import os
import sys
import argparse
import ctypes
from pathlib import Path

# Configurar salida segura en consola para Windows
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    except AttributeError:
        pass


def _preparar_entorno_dll():
    """Configura las rutas necesarias en Windows para cargar DLLs con dependencias (MinGW / OpenMP)."""
    if os.name == 'nt':
        base_dir = Path(__file__).resolve().parent
        for d in [base_dir, base_dir / "whisper.cpp"]:
            if d.is_dir():
                try:
                    os.add_dll_directory(str(d))
                except (AttributeError, OSError):
                    pass

        for p in os.environ.get('PATH', '').split(os.pathsep):
            if p and os.path.isdir(p):
                try:
                    os.add_dll_directory(p)
                except (AttributeError, OSError):
                    pass


def _buscar_archivo(nombre_archivo, directorios_busqueda):
    """Busca un archivo en una lista de directorios prioritarios."""
    for d in directorios_busqueda:
        candidato = Path(d) / nombre_archivo
        if candidato.is_file():
            return candidato.resolve()
    return None


class AudioToTextPipeline:
    def __init__(self, ruta_modelo=None):
        _preparar_entorno_dll()
        
        base_dir = Path(__file__).resolve().parent
        posibles_dirs = [
            base_dir,
            base_dir / "whisper.cpp",
            base_dir.parent,
        ]

        # 1. Cargar librería wav_extractor (C)
        dll_extractor = _buscar_archivo("libwav_extractor.dll", posibles_dirs)
        if not dll_extractor:
            raise FileNotFoundError(
                f"No se encontró 'libwav_extractor.dll'. Verifique la compilación de wav_extractor.c"
            )

        self.lib_extractor = ctypes.CDLL(str(dll_extractor))
        self.lib_extractor.extraer_audio_16k.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.lib_extractor.extraer_audio_16k.restype = ctypes.c_int

        # 2. Cargar librería wav_to_text (C)
        dll_to_text = _buscar_archivo("libwav_to_text.dll", posibles_dirs)
        if not dll_to_text:
            raise FileNotFoundError(
                f"No se encontró 'libwav_to_text.dll'. Verifique la compilación de wav_to_text.c"
            )

        self.lib_to_text = ctypes.CDLL(str(dll_to_text))
        self.lib_to_text.transcribir_wav_a_texto_idioma.argtypes = [
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p
        ]
        self.lib_to_text.transcribir_wav_a_texto_idioma.restype = ctypes.c_int

        # 3. Localizar modelo Whisper (.bin)
        if ruta_modelo:
            self.ruta_modelo = Path(ruta_modelo).resolve()
        else:
            modelo_encontrado = _buscar_archivo("ggml-base.bin", [
                base_dir / "whisper.cpp" / "models",
                base_dir / "models",
                base_dir / "whisper.cpp",
                base_dir,
            ])
            if not modelo_encontrado:
                raise FileNotFoundError(
                    "No se encontró el archivo del modelo 'ggml-base.bin'. Especifique la ruta con --model."
                )
            self.ruta_modelo = modelo_encontrado

        if not self.ruta_modelo.is_file():
            raise FileNotFoundError(f"El modelo no existe en: {self.ruta_modelo}")

    def procesar(self, archivo_audio, archivo_salida=None, idioma="es", guardar_audio_intermedio=False):
        """
        Ejecuta el pipeline de principio a fin:
        Audio entrada -> wav_extractor (C) -> 16kHz WAV -> wav_to_text (C) -> Transcripción .txt
        """
        ruta_audio = Path(archivo_audio).resolve()
        if not ruta_audio.is_file():
            raise FileNotFoundError(f"El archivo de audio '{archivo_audio}' no existe.")

        if archivo_salida:
            ruta_salida = Path(archivo_salida).resolve()
        else:
            ruta_salida = ruta_audio.with_suffix(".txt")

        # Archivo temporal estandarizado a 16 kHz Mono PCM
        audio_temp_16k = ruta_audio.parent / f"_temp_16k_{ruta_audio.stem}.wav"

        print("=" * 65)
        print("          PIPELINE DE TRANSCRIPCIÓN (C + PYTHON)          ")
        print("=" * 65)
        print(f"[*] Audio de entrada  : {ruta_audio.name}")
        print(f"[*] Modelo Whisper    : {self.ruta_modelo.name}")
        print(f"[*] Idioma configurado: {idioma}")
        print(f"[*] Archivo de salida : {ruta_salida.name}")
        print("-" * 65)

        try:
            # PASO 1: Llamada a la función en C (wav_extractor)
            print("[1/2] Ejecutando wav_extractor (C): Normalizando audio a 16 kHz mono...")
            res_ext = self.lib_extractor.extraer_audio_16k(
                str(ruta_audio).encode('utf-8'),
                str(audio_temp_16k).encode('utf-8')
            )
            if res_ext != 0:
                raise RuntimeError(f"Error en wav_extractor (C). Código de retorno: {res_ext}")

            print("      -> Audio preprocesado y listo.")

            # PASO 2: Llamada a la función en C (wav_to_text)
            print("[2/2] Ejecutando wav_to_text (C): Transcribiendo mediante Whisper...")
            res_stt = self.lib_to_text.transcribir_wav_a_texto_idioma(
                str(audio_temp_16k).encode('utf-8'),
                str(self.ruta_modelo).encode('utf-8'),
                str(ruta_salida).encode('utf-8'),
                str(idioma).encode('utf-8')
            )
            if res_stt != 0:
                raise RuntimeError(f"Error en wav_to_text (C). Código de retorno: {res_stt}")

            print("-" * 65)
            print(f"[OK] Transcripción completada exitosamente!")
            print(f"[OK] Archivo generado: {ruta_salida}")

            # Mostrar texto generado en consola
            if ruta_salida.is_file():
                contenido = ruta_salida.read_text(encoding='utf-8').strip()
                print("\n" + "=" * 25 + " TEXTO EXTRAÍDO " + "=" * 25)
                print(contenido if contenido else "(Transcripción vacía o silenciosa)")
                print("=" * 65 + "\n")
                return contenido

        finally:
            # Limpieza del archivo temporal intermedio
            if not guardar_audio_intermedio and audio_temp_16k.is_file():
                try:
                    audio_temp_16k.unlink()
                except OSError:
                    pass


def main():
    parser = argparse.ArgumentParser(
        description="Pipeline en Python que llama a funciones de C (wav_extractor y wav_to_text) para transcribir audio a texto."
    )
    parser.add_argument("audio", help="Nombre o ruta del archivo de audio .wav a transcribir.")
    parser.add_argument("-o", "--output", help="Ruta del archivo de texto .txt resultante (por defecto: <audio>.txt).", default=None)
    parser.add_argument("-m", "--model", help="Ruta al modelo Whisper .bin (por defecto busca ggml-base.bin).", default=None)
    parser.add_argument("-l", "--lang", help="Idioma de transcripción ('es', 'en', 'auto', etc.). Por defecto: 'es'.", default="es")
    parser.add_argument("--keep-temp", action="store_true", help="Conservar el archivo WAV de 16 kHz generado.")

    args = parser.parse_args()

    try:
        pipeline = AudioToTextPipeline(ruta_modelo=args.model)
        pipeline.procesar(
            archivo_audio=args.audio,
            archivo_salida=args.output,
            idioma=args.lang,
            guardar_audio_intermedio=args.keep_temp
        )
    except Exception as e:
        print(f"\n[ERROR] {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
