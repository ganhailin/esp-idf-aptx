#pragma once

typedef struct {
	int Bits;
	int Size;
    ldacdec_float_t Scale;
    ldacdec_float_t ImdctPrevious[MAX_FRAME_SAMPLES];
    ldacdec_float_t* Window;
    ldacdec_float_t* SinTable;
    ldacdec_float_t* CosTable;
} Mdct;

void InitMdct();
void RunImdct(Mdct* mdct, float* input, float* output);
