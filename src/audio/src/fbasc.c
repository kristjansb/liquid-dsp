/*
 * Copyright (c) 2007, 2008, 2009, 2010 Joseph Gaeddert
 * Copyright (c) 2007, 2008, 2009, 2010 Virginia Polytechnic
 *                                      Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// fbasc : filterbank audio synthesizer codec
// 
// The fbasc audio codec implements an AAC-like compression
// algorithm, using the modified discrete cosine transform as a
// loss-less channelizer.  The resulting channelized data are
// then quantized based on their spectral energy levels and then
// packed into a frame which the decoder can then interpret.
// The result is a lossy encoder (as a result of quantization)
// whose compression/quality levels can be easily varied.
// 
// More information available in src/audio/readme.fbasc.txt
//

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "liquid.internal.h"

#define FBASC_DEBUG     1

// fbasc object structure
struct fbasc_s {
    int type;                       // encoder/decoder
    unsigned int num_channels;      // MDCT size (half-length)
    unsigned int samples_per_frame; // input audio samples per frame
    unsigned int bytes_per_frame;   // number of bytes per frame

    // derived values
    unsigned int symbols_per_frame; // samples_per_frame/num_channels
    unsigned int header_len;        // header length (bytes)
    unsigned int bits_per_frame;
    unsigned int bits_per_symbol;
    unsigned int max_bits_per_sample;

    // common objects
    float * w;                      // MDCT window [size: 2*num_channels x 1]
    float * buffer;                 // MDCT buffer [size: 2*num_channels x 1]
    float * X;                      // channelized matrix [size: num_channels x symbols_per_frame]
    unsigned int * bk;              // bits per channel [size: num_channels x 1]
    float * gk;                     // channel gain [size: num_channels x 1]
    unsigned char * data;           // quantized frame data (bytes) [size: num_channels x symbols_per_frame]
    // TODO : extend 'data' variable to unsigned short to allow more than 8 bits / sample
    //unsigned char * packed_data;    // packed quantized data

    // analysis/synthesis
    float * channel_var;            // signal variance on each channel [size: num_channels x 1]
    float gain;                     // nominal gain
    float mu;                       // compression factor (see module:quantization)

    // frame info
    unsigned int fid;               // frame id
    unsigned int g0;                // nominal channel gain (quantized)
};

// compute length of header
//
//  id      name                # bytes
//  --      ----------          ---------
//  fid     frame id            2
//  g0      nominal gain        1
//  bk      bit allocation      num_channels
//  --      ----------          ---------
//          total:              num_channels + 3
unsigned int fbasc_compute_header_length(unsigned int _num_channels,
                                         unsigned int _samples_per_frame,
                                         unsigned int _bytes_per_frame)
{
    return _num_channels + 3;
}

// create options
//  _type               :   analysis/synthesis (encoder/decoder)
//  _num_channels       :   number of filterbank channels
//  _samples_per_frame  :   number of real samples per frame (must be even multiple of _num_channels)
//  _bytes_per_frame    :   number of encoded data bytes per frame
fbasc fbasc_create(int _type,
                   unsigned int _num_channels,
                   unsigned int _samples_per_frame,
                   unsigned int _bytes_per_frame)
{
    fbasc q = (fbasc) malloc(sizeof(struct fbasc_s));

    // initialize parametric values/lengths
    q->type = _type;
    q->num_channels = _num_channels;
    q->samples_per_frame = _samples_per_frame;
    q->bytes_per_frame = _bytes_per_frame;

    // validate input
    if (q->type == FBASC_ENCODER) {
    } else if (q->type == FBASC_DECODER) {
    } else {
        fprintf(stderr,"error: fbasc_create(), unknown type: %d\n", _type);
        exit(1);
    }

    // initialize derived values/lengths
    q->symbols_per_frame = (q->samples_per_frame) / (q->num_channels);
    q->bits_per_frame = 8*q->bytes_per_frame;
    q->bits_per_symbol = q->bits_per_frame / q->symbols_per_frame;
    q->max_bits_per_sample = 8;

    // ensure num_channels evenly divides samples_per_frame
    if ( q->symbols_per_frame * q->num_channels != q->samples_per_frame) {
        fprintf(stderr,"error: fbasc_create(), _num_channels must evenly divide _samples_per_frame\n");
        exit(1);
    }

    // ensure num_channels evenly divides samples_per_frame
    unsigned int bytes_per_symbol = q->bytes_per_frame / q->num_channels;
    if ( bytes_per_symbol * q->num_channels != q->bytes_per_frame) {
        fprintf(stderr,"error: fbasc_create(), _num_channels must evenly divide _bytes_per_frame\n");
        exit(1);
    }

    // compute header length (bytes)
    q->header_len = fbasc_compute_header_length(q->num_channels,
                                                q->samples_per_frame,
                                                q->bytes_per_frame);

    // analysis/synthesis
    q->w =              (float*) malloc( 2*(q->num_channels)*sizeof(float) );
    q->buffer =         (float*) malloc( 2*(q->num_channels)*sizeof(float) );
    q->X =              (float*) malloc( (q->samples_per_frame)*sizeof(float) );
    q->channel_var =    (float*) malloc( (q->num_channels)*sizeof(float) );
    q->gk =             (float*) malloc( (q->num_channels)*sizeof(float) );
    q->mu = 255.0f;

    // data
    q->bk = (unsigned int *) malloc( (q->num_channels)*sizeof(unsigned int));
    q->data = (unsigned char *) malloc( (q->samples_per_frame)*sizeof(unsigned char));

    unsigned int i;

    // initialize window
    for (i=0; i<2*q->num_channels; i++)
        q->w[i] = liquid_kbd_window(i,2*q->num_channels,10.0f);

    // reset buffer
    for (i=0; i<2*q->num_channels; i++)
        q->buffer[i] = 0.0f;

    // reset frame id
    q->fid = 0;

    return q;
}

// destroy fbasc object, free internally-allocated memory
void fbasc_destroy(fbasc _q)
{
    // free common arrays
    free(_q->w);            // windowing function for MDCT
    free(_q->buffer);       // buffer for MDCT
    free(_q->X);            // channelized samples
    free(_q->channel_var);  // channel variance
    free(_q->gk);

    free(_q->bk);           // bit allocations
    free(_q->data);         // sampled data

    // free memory structure
    free(_q);
}

void fbasc_print(fbasc _q)
{
    printf("filterbank audio synthesizer codec:\n");
    printf("    channels:       %u\n", _q->num_channels);
    printf("    type:           %s\n", 
        _q->type == FBASC_ENCODER ? "encoder" : "decoder");
    printf("    samples/frame:  %u\n", _q->samples_per_frame);
    printf("    symbols/frame:  %u\n", _q->symbols_per_frame);
    printf("    bytes/frame:    %u\n", _q->bytes_per_frame);
    printf("    header length:  %u bytes\n", _q->header_len);
}

// encode frame of audio
//  _q      :   fbasc object
//  _audio  :   audio samples [size: samples_per_frame x 1]
//  _header :   encoded header [size: ???]
//  _frame  :   encoded frame bytes [size: bytes_per_frame x 1]
void fbasc_encode(fbasc _q,
                  float * _audio,
                  unsigned char * _header,
                  unsigned char * _frame)
{
    // run analyzer
    fbasc_encoder_run_analyzer(_q, _audio, _q->X);

    // compute channel variance
    fbasc_encoder_compute_channel_energy(_q);

    // compute bit partitioning
    fbasc_compute_bit_allocation(_q->num_channels,
                                 _q->channel_var,
                                 _q->bits_per_symbol,
                                 _q->max_bits_per_sample,
                                 _q->bk);

    // compute metrics for encoding
    fbasc_encoder_compute_metrics(_q);

    // quantize samples
    fbasc_encoder_quantize_samples(_q);

    // pack header
    fbasc_encoder_pack_header(_q, _header);

    // pack frame data
    fbasc_encoder_pack_frame(_q, _frame);

    // increment frame id
    _q->fid++;
}

// decode frame of audio
//  _q      :   fbasc object
//  _header :   encoded header [size: ???]
//  _frame  :   encoded frame bytes [size: bytes_per_frame x 1]
//  _audio  :   decoded audio samples [size: samples_per_frame x 1]
void fbasc_decode(fbasc _q,
                  unsigned char * _header,
                  unsigned char * _frame,
                  float * _audio)
{
    // unpack header
    fbasc_decoder_unpack_header(_q, _header);

    // unpack frame data
    fbasc_decoder_unpack_frame(_q, _frame);

    // compute metrics for decoding
#if FBASC_DEBUG
    printf("**** DECODER METRICS\n");
#endif
    fbasc_encoder_compute_metrics(_q);

    // de-quantize samples
    fbasc_decoder_dequantize_samples(_q);

    // run synthesizer
    fbasc_decoder_run_synthesizer(_q, _q->X, _audio);
}


// 
// internal methods
//

// fbasc_encoder_run_analyzer()
//
// run analyzer on a frame of data
//  _q      :   fbasc object
//  _x      :   input audio samples [size: samples_per_frame x 1]
//  _X      :   output channelized samples [size: num_channels x symbols_per_frame]
//  NOTE: num_channels * symbols_per_frame = samples_per_frame
void fbasc_encoder_run_analyzer(fbasc _q,
                                float * _x,
                                float * _X)
{
    unsigned int i;

    for (i=0; i<_q->symbols_per_frame; i++) {
        // copy last half of buffer to first half
        memmove(_q->buffer,
                &_q->buffer[_q->num_channels],
                _q->num_channels*sizeof(float));

        // copy input block [size: num_channels x 1] to last half of buffer
        memmove(&_q->buffer[_q->num_channels],
                &_x[i*_q->num_channels],
                _q->num_channels*sizeof(float));

        // run transform on internal buffer, store result in output
        mdct(_q->buffer,
             &_X[i*_q->num_channels],
             _q->w,
             _q->num_channels);
    }
}

// fbasc_decoder_run_synthesizer()
//
// run synthesizer on a frame of data
//  _q      :   fbasc object
//  _X      :   intput channelized samples [size: num_channels x symbols_per_frame]
//  _x      :   output audio samples [size: samples_per_frame x 1]
//  NOTE: num_channels * symbols_per_frame = samples_per_frame
void fbasc_decoder_run_synthesizer(fbasc _q,
                                   float * _X,
                                   float * _x)
{
    unsigned int i,j;

    // copy last half of buffer to beginning of output; this
    // preserves continuity between frames
    memmove(_x,
            &_q->buffer[_q->num_channels],
            _q->num_channels*sizeof(float));

    for (i=0; i<_q->symbols_per_frame; i++) {
        // run inverse transform on input [size: num_channels x 1]
        imdct(&_X[i*_q->num_channels],
              _q->buffer,
              _q->w,
              _q->num_channels);

        // accumulate first half of buffer to output
        for (j=0; j<_q->num_channels; j++)
            _x[i*_q->num_channels + j] += _q->buffer[j];

        // copy last half of buffer to output (only if the
        // index isn't on the last symbol)
        if (i==_q->symbols_per_frame-1)
            continue;
        memmove(&_x[(i+1)*_q->num_channels],
                &_q->buffer[_q->num_channels],
                _q->num_channels*sizeof(float));
    }
}

// compute normalized channel variance
void fbasc_encoder_compute_channel_energy(fbasc _q)
{
    unsigned int i,j;

    // clear channel variance array
    for (i=0; i<_q->num_channels; i++)
        _q->channel_var[i] = 0.0f;

    // compute channel variance, maximum amplitude
    float max_amp = 0.0f;
    for (i=0; i<_q->symbols_per_frame; i++) {
        for (j=0; j<_q->num_channels; j++) {
            // strip sample from channelized data
            float v = _q->X[i*_q->num_channels+j];

            // accumulate variance on channel
            _q->channel_var[j] += v*v;

            if ( fabsf(v) > max_amp || (i==0 && j==0) )
                max_amp = fabsf(v);
        }
    }

    // normalize channel variance by number of symbols per frame
    // TODO : determine if this is really necessary
    float max_var = 0.0f;
    for (i=0; i<_q->num_channels; i++) {
        _q->channel_var[i] = _q->channel_var[i] / _q->symbols_per_frame;
        max_var = _q->channel_var[i] > max_var ? _q->channel_var[i] : max_var;
    }
#if FBASC_DEBUG
    printf("max variance:  %16.12f\n", max_var);
    printf("max amplitude: %16.12f\n", max_amp);
#endif
}


// TODO: document this method
// fbasc_compute_bit_allocation()
//
// computes optimal bit allocation based on channel variance
//
//  _num_channels   :   number of channels
//  _var            :   channel variance array [size: _num_channels x 1]
//  _num_bits       :   total number of bits per symbol
//  _max_bits       :   maximum number of bits per channel
//  _k              :   resulting bit allocation per channel [size: _num_channels x 1]
void fbasc_compute_bit_allocation(unsigned int _num_channels,
                                  float * _var,
                                  unsigned int _num_bits,
                                  unsigned int _max_bits,
                                  unsigned int * _k)
{
    // copy variance internally
    float var[_num_channels];
    memmove(var, _var, _num_channels*sizeof(float));

    unsigned int idx[_num_channels];    // sorted indices

    unsigned int i, j;
    for (i=0; i<_num_channels; i++)
        idx[i] = i;

    // sort (inefficient, but easy to implement)
    unsigned int i_tmp;
    for (i=0; i<_num_channels; i++) {
        for (j=0; j<_num_channels; j++) {
            if ( (i!=j) && (var[idx[i]] > var[idx[j]]) ) {
                // swap values
                i_tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = i_tmp;
            }
        }
    }

    float var_max = var[idx[0]];
    if (var_max < 1e-6f) {
        // maximum variance extremely low
        for (i=0; i<_num_channels; i++)
            var[i] = 1.0f;
    }

    // compute bit allocation
    float b = (float)_num_bits / (float)_num_channels;
    unsigned int n=_num_channels;

    // compute log of geometric mean of variances: log2( prod(var[])^(1/M) )
    float log2p=0.0f;
    for (i=0; i<_num_channels; i++)
        log2p += (var[i] == 0.0f) ? -60.0f : log2f(var[i]);
    log2p /= n;
    printf("max var : %12.8f\n", var_max);
    printf("log2p   : %12.8f\n", log2p);

    // compute theoretical bit allocations
    float bkf[_num_channels];
    for (i=0; i<_num_channels; i++)
        bkf[i] = (var[i]==0.0f) ? 0.0f : b + 0.5f*log2f(var[i]) - 0.5f*log2p;

#if FBASC_DEBUG
    for (i=0; i<_num_channels; i++)
        printf("  var[%3u] = %12.4e, bk = %12.6f\n", idx[i], var[idx[i]], bkf[idx[i]]);
#endif

#if 0
    // assert sum is roughly equal to _num_bits
    float bkf_sum = 0.0f;
    for (i=0; i<_num_channels; i++)
        bkf_sum += bkf[i];
    printf(" num bits : %u (%12.8f)\n", _num_bits, bkf_sum);
#endif

    // TODO : explain what is going on here
    int continue_iterating = 1;
    unsigned int num_iterations=0;
    do {
        num_iterations++;

        // find num and sum sum of soft bit allocations greater than zero
        float bkf_valid_sum = 0.0f;
        unsigned int num_valid = 0;
        for (i=0; i<_num_channels; i++) {
            if (bkf[idx[i]] > 0.0f) {
                bkf_valid_sum += bkf[idx[i]];
                num_valid++;
            } else {
                break;
            }
        }
        //printf("bkf valid sum : %f\n", bkf_valid_sum);

        // shift valid bit allocations
        float shift = (bkf_valid_sum - (float)_num_bits) / (float)num_valid;
        for (i=0; i<_num_channels; i++)
            bkf[i] -= shift;

        // check exit criteria
        if ( fabsf(bkf_valid_sum - _num_bits) < 0.1 || num_iterations > 4)
            continue_iterating = 0;
    } while (continue_iterating);

    // re-compute bit allocation based on validity of soft bit partitions
    unsigned int bits_available = _num_bits;
    for (i=0; i<_num_channels; i++) {
        // if invalid, set bit allocation to zero and continue
        if ( bkf[idx[i]] <= 0.0f) {
            _k[idx[i]] = 0;
            continue;
        }

        unsigned int bk = (unsigned int)roundf(bkf[idx[i]]);
        if (bk > _max_bits)         bk = _max_bits;
        if (bk > bits_available)    bk = bits_available;
        if (bk < 2)                 bk = 0;

        bits_available -= bk;

        _k[idx[i]] = bk;
        
    }

    // assign any remaining available bits to channel with highest variance
    _k[idx[0]] += bits_available;

    // print results
#if FBASC_DEBUG
    printf("*******************\n");
    for (i=0; i<_num_channels; i++)
        printf("  var[%3u] = %12.4e, bk = %12.6f, b = %u\n", idx[i], var[idx[i]], bkf[idx[i]], _k[idx[i]]);
#endif
}

// compute encoder metrics
void fbasc_encoder_compute_metrics(fbasc _q)
{
    unsigned int i;

    // compute nominal gain
#if 0
    int gi = (int)(-log2f(max_var)) - 16;   // use variance
    //int gi = (int)(-log2f(max_amp)) - 1;    // use peak amplitude
    gi = gi > 255 ? 255 : gi;
    gi = gi <   0 ?   0 : gi;
    _q->gain = (float)(1<<gi);
#else
    // TODO FIXME : compute nominal gain properly
    int gi = 0;
    _q->gain = (float)(1<<gi);
    _q->gain = 1.0f;
#endif

    // find maximum bit allocation
    unsigned int bk_max = 0;
    for (i=0; i<_q->num_channels; i++) {
        if (_q->bk[i] > bk_max || i==0)
            bk_max = _q->bk[i];
    }

    // compute relative gains: gk = 2^(max(bk) - bk)
    for (i=0; i<_q->num_channels; i++)
        _q->gk[i] = (float)(1<<(bk_max-_q->bk[i])) * _q->gain;

#if FBASC_DEBUG
    printf("encoder metrics:\n");
    printf("    nominal gain : %12.4e (gi = %3u)\n", _q->gain, gi);
    for (i=0; i<_q->num_channels; i++) {
        if (_q->bk[i] > 0)
            printf("  %3u : e = %12.8f, b = %3u, g=%12.4f\n", i, _q->channel_var[i],_q->bk[i], _q->gk[i]);
        else
            printf("  %3u : e = %12.8f, b = %3u\n",           i, _q->channel_var[i],_q->bk[i]);
    }
#endif
}


// quantize channelized data
void fbasc_encoder_quantize_samples(fbasc _q)
{
    unsigned int i;     // symbol counter
    unsigned int j;     // channel counter
    unsigned int s=0;   // output sample counter
    float sample;       // channelized sample
    float z;            // compressed sample
    unsigned int b;     // quantized sample

    // cycle through symbols in each channel and quantize
    for (i=0; i<_q->symbols_per_frame; i++) {
        for (j=0; j<_q->num_channels; j++) {
            if (_q->bk[j] > 0) {
                // acquire sample, applying proper gain
                sample = _q->X[i*(_q->num_channels)+j] * _q->gk[j];

                // compress using mu-law encoder
                z = compress_mulaw(sample, _q->mu);

                // quantize
                b = quantize_adc(z, _q->bk[j]);
#if 0
                if (s < 10)
                    printf("  %3u : b=0x%4.4x, z=%12.8f, sample=%12.8f\n", s, b,z,sample);
#endif
            } else {
                b = 0;
            }
            _q->data[s] = b;

            s++;
        }
    }

#if FBASC_DEBUG
    printf("encoder data...\n");
    for (i=0; i<10; i++)
        printf("  %3u : %3u\n", i, _q->data[i]);
#endif
}

// de-quantize channelized data
void fbasc_decoder_dequantize_samples(fbasc _q)
{
    unsigned int i;     // symbol counter
    unsigned int j;     // channel counter
    unsigned int s=0;   // input sample counter
    float sample;       // channelized sample
    float z;            // compressed sample
    unsigned int b;     // quantized sample

#if FBASC_DEBUG
    printf("decoder data...\n");
    for (i=0; i<10; i++)
        printf("  %3u : %3u\n", i, _q->data[i]);
#endif

    // cycle through symbols in each channel and quantize
    for (i=0; i<_q->symbols_per_frame; i++) {
        for (j=0; j<_q->num_channels; j++) {
            if (_q->bk[j] > 0) {
                // acquire digital sample
                b = _q->data[s];

                // quantize, digital-to-analog conversion
                z = quantize_dac(b, _q->bk[j]);

                // de-compress (expand) using mu-law decoder
                sample = expand_mulaw(z, _q->mu) / _q->gk[j];

#if 0
                if (s < 10)
                    printf("  %3u : b=0x%4.4x, z=%12.8f, sample=%12.8f\n", s, b,z,sample);
#endif
            } else {
                sample = 0.0f;
            }
            _q->X[i*(_q->num_channels)+j] = sample;
            s++;
        }
    }
}

// pack header
void fbasc_encoder_pack_header(fbasc _q,
                               unsigned char * _header)
{
    unsigned int i;
    unsigned int n=0;

    // fid: frame id
    _header[n+0] = (_q->fid >> 8) & 0x00ff;
    _header[n+1] = (_q->fid     ) & 0x00ff;
    n += 2;

    // g0 : nominal gain
    _header[n++] = _q->g0 && 0x00ff;

    // bk : bit allocation
#if 0
    for (i=0; i<_q->num_channels; i+=2)
        _header[n++] = _q->gk[i+0] | (_q->gk[i+1] << 4);
#else
    for (i=0; i<_q->num_channels; i++)
        _header[n++] = _q->bk[i];
#endif

    // TODO : add redundancy check

    assert(n == _q->header_len);
}

// unpack header
void fbasc_decoder_unpack_header(fbasc _q,
                                 unsigned char * _header)
{
    unsigned int i;
    unsigned int n=0;

    // fid: frame id
    _q->fid = (_header[0] << 8) | _header[1];
    n += 2;

    // g0 : nominal gain
    _q->g0 = _header[n++];

    // bk : bit allocation
#if 0
    for (i=0; i<_q->num_channels; i+=2) {
        _q->bk[i+0] = (_header[n]     ) && 0x00ff
        _q->bk[i+1] = (_header[n] >> 4) && 0x00ff
        n++;
    }
#else
    for (i=0; i<_q->num_channels; i++)
        _q->bk[i] = _header[n++];
#endif

    assert(n == _q->header_len);

    // TODO : validate bit allocation
    // TODO : validate redundancy check

    printf("frame id : %u\n", _q->fid);
}

// pack frame
//
// Example:
//
//  bk  data    buffer
//  --  -----   ----------------
//  3     101   .............101
//  0       -   .............101
//  1       0   ............0101
//  0       -   ............0101
//  5   10010   .......100100101
//  0       -   .......100100101
//  4    1101   ...1101100100101
//  3     001   0011101100100101
void fbasc_encoder_pack_frame(fbasc _q,
                              unsigned char * _frame)
{
    // TODO : there is a bug in this code; data written outside of _frame bounds
    unsigned int i,j;

    unsigned int n=0;           // output byte counter
    unsigned short int buffer=0;// symbol buffer, 16 bits wide (more than max_bits_per_sample)
    unsigned int buffer_len=0;  // length of buffer
    unsigned int s;             // data sample

    // test...
    unsigned int bk_total=0;
    for (i=0; i<_q->num_channels; i++)
        bk_total += _q->bk[i];
    printf("bk (total) : %3u (%3u)\n", bk_total, 0);

    for (i=0; i<_q->symbols_per_frame; i++) {
        for (j=0; j<_q->num_channels; j++) {
            // strip data sample
            s = _q->data[i*_q->num_channels + j];

            // push sample into left side of buffer
            buffer |= (s << buffer_len);

            // increment buffer length
            buffer_len += _q->bk[j];

            // while buffer length exceeds one byte, strip byte off end
            while (buffer_len >= 8) {
                // strip byte off end of buffer and store in output
                _frame[n++] = buffer & 0x00ff;

                // shift buffer
                buffer >>= 8;

                // decrement buffer length
                buffer_len -= 8;
            }
        }
    }

    printf(" n = %3u (%3u)\n", n, _q->bytes_per_frame);
    assert( n == _q->bytes_per_frame );
}

// unpack frame
//
// Example:
//
//  bk  buffer              data
//  --  ----------------    -----
//  3   0011101100100101    101
//  0   0011101100100...    -
//  1   0011101100100...    0
//  0   001110110010....    -
//  5   001110110010....    10010
//  0   0011101.........    -
//  4   0011101.........    1101
//  3   001.............    001
void fbasc_decoder_unpack_frame(fbasc _q,
                                unsigned char * _frame)
{
    unsigned int i,j;

    unsigned int n=0;           // input byte counter
    unsigned short int buffer=0;// symbol buffer, 16 bits wide (more than max_bits_per_sample)
    unsigned int buffer_len=0;  // length of buffer
    unsigned int s;             // data sample

    for (i=0; i<_q->symbols_per_frame; i++) {
        for (j=0; j<_q->num_channels; j++) {

            // skip if no bits are allocated to this channel
            if (_q->bk[j] == 0)
                continue;

            // while buffer length is too small, strip byte from frame
            while (buffer_len < _q->bk[j]) {
                // shift input into left side of buffer
                buffer |= (_frame[n++] << buffer_len);

                // increment buffer length
                buffer_len += 8;
            }

            // strip sample bits off right side of buffer
            s = buffer & ((1 << _q->bk[j])-1);

            // shift buffer
            buffer >>= _q->bk[j];

            // decrement buffer length
            buffer_len -= _q->bk[j];

            // save data sample
            _q->data[i*_q->num_channels + j] = s;
        }
    }
}


