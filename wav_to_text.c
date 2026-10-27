#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "whisper.h"

#if defined(_WIN32) || defined(_WIN64)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

// Helper para buscar chunks en formato RIFF
static int find_wav_chunk(FILE *f, const char *target_id, uint32_t *chunk_size) {
    while (1) {
        char id[4];
        uint32_t sz;
        if (fread(id, 1, 4, f) != 4) return 0;
        if (fread(&sz, sizeof(uint32_t), 1, f) != 1) return 0;

        if (strncmp(id, target_id, 4) == 0) {
            *chunk_size = sz;
            return 1;
        }

        long skip = (long)sz;
        if (sz % 2 != 0) skip++;
        if (fseek(f, skip, SEEK_CUR) != 0) return 0;
    }
}

// Carga muestras de un WAV estándar a 16 kHz Mono y las normaliza a float [-1.0, 1.0]
static float* cargar_muestras_wav(const char* ruta_wav, size_t* num_muestras) {
    FILE* file = fopen(ruta_wav, "rb");
    if (!file) {
        printf("[wav_to_text] Error: No se pudo abrir el archivo %s\n", ruta_wav);
        return NULL;
    }

    char riff[4], wave[4];
    uint32_t overall_size;
    if (fread(riff, 1, 4, file) != 4 || fread(&overall_size, 4, 1, file) != 1 || fread(wave, 1, 4, file) != 4) {
        fclose(file);
        return NULL;
    }
    if (strncmp(riff, "RIFF", 4) != 0 || strncmp(wave, "WAVE", 4) != 0) {
        fclose(file);
        return NULL;
    }

    uint32_t fmt_size = 0;
    if (!find_wav_chunk(file, "fmt ", &fmt_size)) {
        fclose(file);
        return NULL;
    }

    uint16_t audio_format = 0, num_channels = 0, block_align = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, byte_rate = 0;
    fread(&audio_format, 2, 1, file);
    fread(&num_channels, 2, 1, file);
    fread(&sample_rate, 4, 1, file);
    fread(&byte_rate, 4, 1, file);
    fread(&block_align, 2, 1, file);
    fread(&bits_per_sample, 2, 1, file);

    // Validar requisitos de Whisper (16 kHz, Mono, 16 bits PCM)
    if (num_channels != 1 || sample_rate != 16000) {
        printf("[wav_to_text] Error: Se requiere audio MONO a 16000 Hz. (Tiene %u canales, %u Hz)\n",
               num_channels, sample_rate);
        fclose(file);
        return NULL;
    }

    fseek(file, 12, SEEK_SET);
    uint32_t data_size = 0;
    if (!find_wav_chunk(file, "data", &data_size)) {
        fclose(file);
        return NULL;
    }

    size_t total_samples = data_size / sizeof(int16_t);
    int16_t* pcm16 = (int16_t*)malloc(total_samples * sizeof(int16_t));
    float* pcmf32 = (float*)malloc(total_samples * sizeof(float));

    if (!pcm16 || !pcmf32) {
        if (pcm16) free(pcm16);
        if (pcmf32) free(pcmf32);
        fclose(file);
        return NULL;
    }

    size_t samples_read = fread(pcm16, sizeof(int16_t), total_samples, file);
    fclose(file);

    for (size_t i = 0; i < samples_read; i++) {
        pcmf32[i] = (float)pcm16[i] / 32768.0f;
    }
    free(pcm16);

    *num_muestras = samples_read;
    return pcmf32;
}

// Función principal de transcripción (con selección de idioma)
EXPORT int transcribir_wav_a_texto_idioma(const char* ruta_wav, const char* ruta_modelo, const char* ruta_salida_txt, const char* idioma) {
    size_t num_muestras = 0;
    float* pcmf32 = cargar_muestras_wav(ruta_wav, &num_muestras);
    if (!pcmf32) return -1;

    struct whisper_context_params cparams = whisper_context_default_params();
    struct whisper_context* ctx = whisper_init_from_file_with_params(ruta_modelo, cparams);
    if (!ctx) {
        printf("[wav_to_text] Error: No se pudo cargar el modelo desde '%s'\n", ruta_modelo);
        free(pcmf32);
        return -2;
    }

    struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_realtime   = false;
    params.print_progress   = false;
    params.print_timestamps = false;
    params.language         = (idioma && strlen(idioma) > 0) ? idioma : "es";

    printf("[wav_to_text] Procesando audio con Whisper en C (idioma: %s)...\n", params.language);

    if (whisper_full(ctx, params, pcmf32, (int)num_muestras) != 0) {
        printf("[wav_to_text] Error durante la transcripción del audio.\n");
        whisper_free(ctx);
        free(pcmf32);
        return -3;
    }

    FILE* txt_out = fopen(ruta_salida_txt, "w");
    if (!txt_out) {
        printf("[wav_to_text] Error al crear archivo de salida '%s'\n", ruta_salida_txt);
        whisper_free(ctx);
        free(pcmf32);
        return -4;
    }

    int num_segmentos = whisper_full_n_segments(ctx);
    for (int i = 0; i < num_segmentos; ++i) {
        const char* texto = whisper_full_get_segment_text(ctx, i);
        fprintf(txt_out, "%s", texto);
    }
    fprintf(txt_out, "\n");

    fclose(txt_out);
    whisper_free(ctx);
    free(pcmf32);

    printf("[wav_to_text] Transcripción guardada exitosamente en '%s'\n", ruta_salida_txt);
    return 0;
}

// Firma simplificada por defecto (en español)
EXPORT int transcribir_wav_a_texto(const char* ruta_wav, const char* ruta_modelo, const char* ruta_salida_txt) {
    return transcribir_wav_a_texto_idioma(ruta_wav, ruta_modelo, ruta_salida_txt, "es");
}