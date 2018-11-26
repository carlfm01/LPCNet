/* Copyright (c) 2018 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <math.h>
#include "nnet_data.h"
#include "nnet.h"
#include "common.h"
#include "arch.h"
#include "lpcnet.h"

#define NB_FEATURES 38

#define FRAME_INPUT_SIZE (NB_FEATURES + EMBED_PITCH_OUT_SIZE)

#define SAMPLE_INPUT_SIZE (2*EMBED_SIG_OUT_SIZE + EMBED_EXC_OUT_SIZE + FEATURE_DENSE2_OUT_SIZE)

static int ulaw2lin(int u)
{
    float s;
    float scale_1 = 32768.f/255.f;
    u = u - 128;
    s = u >= 0 ? 1 : -1;
    u = abs(u);
    return s*scale_1*(exp(u/128.*log(256))-1);
}

static int lin2ulaw(int x)
{
    float u;
    float scale = 255.f/32768.f;
    int s = x >= 0 ? 1 : -1;
    x = abs(x);
    u = (s*(128*log(1+scale*x)/log(256)));
    u = 128 + u;
    if (u < 0) u = 0;
    if (u > 255) u = 255;
    return (int)floor(.5 + u);
}

void run_frame_network(NNetState *net, float *condition, float *lpc, const float *features, int pitch)
{
    int i;
    float in[FRAME_INPUT_SIZE];
    float conv1_out[FEATURE_CONV1_OUT_SIZE];
    float conv2_out[FEATURE_CONV2_OUT_SIZE];
    float dense1_out[FEATURE_DENSE1_OUT_SIZE];
    RNN_COPY(in, features, NB_FEATURES);
    compute_embedding(&embed_pitch, &in[NB_FEATURES], pitch);
    compute_conv1d(&feature_conv1, conv1_out, net->feature_conv1_state, in);
    compute_conv1d(&feature_conv2, conv2_out, net->feature_conv2_state, conv1_out);
    celt_assert(FRAME_INPUT_SIZE == FEATURE_CONV2_OUT_SIZE);
    for (i=0;i<FEATURE_CONV2_OUT_SIZE;i++) conv2_out[i] += in[i];
    compute_dense(&feature_dense1, dense1_out, conv2_out);
    compute_dense(&feature_dense2, condition, dense1_out);
    /* FIXME: Actually compute the LPC on the middle frame. */
    RNN_CLEAR(lpc, LPC_ORDER);
}

int run_sample_network(NNetState *net, const float *condition, int last_exc, int last_sig, int pred)
{
    float in_a[SAMPLE_INPUT_SIZE];
    float in_b[GRU_A_STATE_SIZE+FEATURE_DENSE2_OUT_SIZE];
    float pdf[DUAL_FC_OUT_SIZE];
    compute_embedding(&embed_sig, &in_a[0], last_sig);
    compute_embedding(&embed_sig, &in_a[EMBED_SIG_OUT_SIZE], pred);
    compute_embedding(&embed_exc, &in_a[2*EMBED_SIG_OUT_SIZE], last_exc);
    RNN_COPY(&in_a[2*EMBED_SIG_OUT_SIZE + EMBED_EXC_OUT_SIZE], condition, FEATURE_DENSE2_OUT_SIZE);
    compute_gru(&gru_a, net->gru_a_state, in_a);
    RNN_COPY(in_b, net->gru_a_state, GRU_A_STATE_SIZE);
    RNN_COPY(&in_b[GRU_A_STATE_SIZE], condition, FEATURE_DENSE2_OUT_SIZE);
    compute_gru(&gru_b, net->gru_b_state, in_b);
    compute_mdense(&dual_fc, pdf, net->gru_b_state);
    /* FIXME: Do the actual sampling here. */
    return 0;
}

void generate_samples(LPCNetState *lpcnet, short *output, const float *features, int pitch, int N)
{
    int i;
    float condition[FEATURE_DENSE2_OUT_SIZE];
    float lpc[LPC_ORDER];
    run_frame_network(&lpcnet->nnet, condition, lpc, features, pitch);
    for (i=0;i<N;i++)
    {
        int j;
        int pred;
        int exc;
        int last_sig_ulaw;
        int pred_ulaw;
        float sum = 0;
        for (j=0;j<LPC_ORDER;j++) sum += lpcnet->last_sig[j]*lpc[j];
        pred = (int)floor(.5f + sum);
        last_sig_ulaw = lin2ulaw(lpcnet->last_sig[0]);
        pred_ulaw = lin2ulaw(pred);
        exc = run_sample_network(&lpcnet->nnet, condition, lpcnet->last_exc, last_sig_ulaw, pred_ulaw);
        output[i] = pred + ulaw2lin(exc);
        RNN_MOVE(&lpcnet->last_sig[1], &lpcnet->last_sig[0], LPC_ORDER-1);
        lpcnet->last_sig[0] = output[i];
        lpcnet->last_exc = exc;
    }
}
