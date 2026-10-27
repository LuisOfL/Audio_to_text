#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#if defined(_WIN32) || defined(_WIN64)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#pragma pack(push, 1)
typedef struct {
    char riff[4];              // "RIFF"
    uint32_t overall_size;     // filesize - 8
    char wave[4];              // "WAVE"
    char fmt_chunk_marker[4];  // "fmt "
    uint32_t length_of_fmt;    // 16
    uint16_t format_type;      // 1 = PCM
    uint16_t channels;         // 1 = Mono
    uint32_t sample_rate;      // 16000
    uint32_t byterate;         // 16000 * 1 * 2 = 32000
    uint16_t block_align;      // 2
    uint16_t bits_per_sample;  // 16
    char data_chunk_header[4]; // "data"
    uint32_t data_size;        // num_samples * 2
} WavHeader16k;
#pragma pack(pop)

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

        // Saltar chunk no deseado (alineado a 2 bytes)
        long skip = (long)sz;
        if (sz % 2 != 0) skip++;
        if (fseek(f, skip, SEEK_CUR) != 0) return 0;
    }
}

// Extrae el audio de cualquier archivo WAV y lo convierte a formato estándar 16 kHz Mono PCM
EXPORT int extraer_audio_16k(const char *input_wav, const char *output_wav) {
    FILE *fin = fopen(input_wav, "rb");
    if (!fin) {
        printf("[wav_extractor] Error: No se pudo abrir '%s'\n", input_wav);
        return -1;
    }

    char riff[4], wave[4];
    uint32_t overall_size;
    if (fread(riff, 1, 4, fin) != 4 || fread(&overall_size, 4, 1, fin) != 1 || fread(wave, 1, 4, fin) != 4) {
        fclose(fin);
        return -2;
    }
    if (strncmp(riff, "RIFF", 4) != 0 || strncmp(wave, "WAVE", 4) != 0) {
        fclose(fin);
        return -3;
    }

    // Buscar chunk "fmt "
    uint32_t fmt_size = 0;
    if (!find_wav_chunk(fin, "fmt ", &fmt_size)) {
        fclose(fin);
        return -4;
    }

    uint16_t audio_format = 0, num_channels = 0, block_align = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, byte_rate = 0;

    fread(&audio_format, 2, 1, fin);
    fread(&num_channels, 2, 1, fin);
    fread(&sample_rate, 4, 1, fin);
    fread(&byte_rate, 4, 1, fin);
    fread(&block_align, 2, 1, fin);
    fread(&bits_per_sample, 2, 1, fin);

    if (fmt_size > 16) {
        fseek(fin, (long)(fmt_size - 16), SEEK_CUR);
    }
    if (fmt_size % 2 != 0) {
        fseek(fin, 1, SEEK_CUR);
    }

    // Volver a posición después de cabecera RIFF para buscar "data"
    fseek(fin, 12, SEEK_SET);
    uint32_t data_size = 0;
    if (!find_wav_chunk(fin, "data", &data_size)) {
        fclose(fin);
        return -5;
    }

    if (num_channels == 0 || sample_rate == 0 || bits_per_sample == 0) {
        fclose(fin);
        return -6;
    }

    size_t bytes_per_sample = bits_per_sample / 8;
    size_t num_frames = data_size / (num_channels * bytes_per_sample);
    if (num_frames == 0) {
        fclose(fin);
        return -7;
    }

    // Leer muestras a buffer mono en float [-1.0, 1.0]
    float *mono_in = (float *)malloc(num_frames * sizeof(float));
    if (!mono_in) {
        fclose(fin);
        return -8;
    }

    for (size_t i = 0; i < num_frames; i++) {
        float sum = 0.0f;
        for (int ch = 0; ch < num_channels; ch++) {
            if (bits_per_sample == 16) {
                int16_t s;
                if (fread(&s, sizeof(int16_t), 1, fin) != 1) { s = 0; }
                sum += (float)s / 32768.0f;
            } else if (bits_per_sample == 8) {
                uint8_t s;
                if (fread(&s, sizeof(uint8_t), 1, fin) != 1) { s = 128; }
                sum += ((float)s - 128.0f) / 128.0f;
            } else if (bits_per_sample == 32 && audio_format == 3) {
                float s;
                if (fread(&s, sizeof(float), 1, fin) != 1) { s = 0.0f; }
                sum += s;
            } else {
                // Formato no estándar: omitir muestra
                fseek(fin, (long)bytes_per_sample, SEEK_CUR);
            }
        }
        mono_in[i] = sum / (float)num_channels;
    }
    fclose(fin);

    // Resamplear a 16000 Hz si es necesario
    size_t target_frames = (size_t)((double)num_frames * 16000.0 / (double)sample_rate);
    int16_t *pcm16_out = (int16_t *)malloc(target_frames * sizeof(int16_t));
    if (!pcm16_out) {
        free(mono_in);
        return -9;
    }

    if (sample_rate == 16000) {
        for (size_t i = 0; i < target_frames; i++) {
            float val = mono_in[i];
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;
            pcm16_out[i] = (int16_t)(val * 32767.0f);
        }
    } else {
        // Interpolación lineal rápida
        double ratio = (double)sample_rate / 16000.0;
        for (size_t j = 0; j < target_frames; j++) {
            double src_pos = (double)j * ratio;
            size_t idx0 = (size_t)src_pos;
            size_t idx1 = (idx0 + 1 < num_frames) ? idx0 + 1 : idx0;
            double frac = src_pos - (double)idx0;
            float val = (float)((1.0 - frac) * mono_in[idx0] + frac * mono_in[idx1]);
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;
            pcm16_out[j] = (int16_t)(val * 32767.0f);
        }
    }
    free(mono_in);

    // Guardar archivo WAV estandarizado (16kHz Mono 16-bit PCM)
    FILE *fout = fopen(output_wav, "wb");
    if (!fout) {
        free(pcm16_out);
        return -10;
    }

    WavHeader16k hdr;
    memcpy(hdr.riff, "RIFF", 4);
    hdr.data_size = (uint32_t)(target_frames * sizeof(int16_t));
    hdr.overall_size = (uint32_t)(sizeof(WavHeader16k) - 8 + hdr.data_size);
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt_chunk_marker, "fmt ", 4);
    hdr.length_of_fmt = 16;
    hdr.format_type = 1;      // PCM
    hdr.channels = 1;         // Mono
    hdr.sample_rate = 16000;
    hdr.byterate = 16000 * 1 * 2;
    hdr.block_align = 2;
    hdr.bits_per_sample = 16;
    memcpy(hdr.data_chunk_header, "data", 4);

    fwrite(&hdr, sizeof(WavHeader16k), 1, fout);
    fwrite(pcm16_out, sizeof(int16_t), target_frames, fout);

    fclose(fout);
    free(pcm16_out);
    return 0; // Éxito
}

// Mantiene compatibilidad con la función anterior
EXPORT int extract_wav_to_text(const char *wav_filename, const char *txt_filename) {
    FILE *file = fopen(wav_filename, "rb");
    if (!file) return -1;

    char riff[4], wave[4];
    uint32_t overall_size;
    if (fread(riff, 1, 4, file) != 4 || fread(&overall_size, 4, 1, file) != 1 || fread(wave, 1, 4, file) != 4) {
        fclose(file);
        return -2;
    }
    if (strncmp(riff, "RIFF", 4) != 0 || strncmp(wave, "WAVE", 4) != 0) {
        fclose(file);
        return -3;
    }

    uint32_t fmt_size = 0;
    if (!find_wav_chunk(file, "fmt ", &fmt_size)) {
        fclose(file);
        return -4;
    }

    uint16_t audio_format = 0, num_channels = 0, block_align = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, byte_rate = 0;
    fread(&audio_format, 2, 1, file);
    fread(&num_channels, 2, 1, file);
    fread(&sample_rate, 4, 1, file);
    fread(&byte_rate, 4, 1, file);
    fread(&block_align, 2, 1, file);
    fread(&bits_per_sample, 2, 1, file);

    fseek(file, 12, SEEK_SET);
    uint32_t data_size = 0;
    if (!find_wav_chunk(file, "data", &data_size)) {
        fclose(file);
        return -5;
    }

    FILE *txtFile = fopen(txt_filename, "w");
    if (!txtFile) {
        fclose(file);
        return -6;
    }

    fprintf(txtFile, "Muestra,Valor\n");
    size_t sampleIndex = 0;
    if (bits_per_sample == 16) {
        int16_t sample;
        while (fread(&sample, sizeof(int16_t), 1, file) == 1) {
            fprintf(txtFile, "%zu,%d\n", sampleIndex++, sample);
        }
    } else if (bits_per_sample == 8) {
        uint8_t sample;
        while (fread(&sample, sizeof(uint8_t), 1, file) == 1) {
            fprintf(txtFile, "%zu,%u\n", sampleIndex++, sample);
        }
    }
    fclose(file);
    fclose(txtFile);
    return 0;
}