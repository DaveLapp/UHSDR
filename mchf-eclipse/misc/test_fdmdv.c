/*---------------------------------------------------------------------------*\

  FILE........: tfdmdv.c
  AUTHOR......: David Rowe
  DATE CREATED: April 16 2012

  Tests for the C version of the FDMDV modem.  This program outputs a
  file of Octave vectors that are loaded and automatically tested
  against the Octave version of the modem by the Octave script
  tfmddv.m

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2012 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "fdmdv_internal.h"
#include "codec2_fdmdv.h"
#include "octave.h"


#ifdef ARM_MATH_CM4
#include "Trace.h"
#define FPRINTF(f,...) trace_printf(__VA_ARGS__)
#else
#define FPRINTF(...) fprintf(__VA_ARGS__)
#endif

#include "profiling.h"


//  0 -> each frame is stored separately
//  1 -> after all frames store all data, this is currently required for 
//       octave analysis
#define CUMULATIVE 0

// 0 -> Minimal memory consumption, can be run on targets with little memory
// 1 -> normal memory consumption, this is currently required for 
//      octave analisys
#define MULTIFRAME 0


#if MULTIFRAME 
#define FRAMERUNS  1 // number of runs
#define FRAMES    35 // allocate space for n frames
#define FRAMEINCR 35 // number of frames to process per run
#else
#define FRAMERUNS 35
#define FRAMES     1
#define FRAMEINCR  1
#endif

#define CHANNEL_BUF_SIZE (10*M)
FILE         *fout;
struct FDMDV *fdmdv;

extern float pilot_coeff[];
float         channel[CHANNEL_BUF_SIZE];
int           channel_count;

int run(int cumulativeOutput, int numberOfRuns, int currentRun, int next_nin)
{
    int           tx_bits[FDMDV_BITS_PER_FRAME];
    COMP          tx_symbols[FDMDV_NC+1];
    COMP          tx_fdm[M];
    COMP          rx_fdm[M+M/P];
    float         foff_coarse;
    int           nin;
    COMP          rx_fdm_fcorr[M+M/P];
    COMP          rx_fdm_filter[M+M/P];
    COMP          rx_filt[NC+1][P+1];
    float         rx_timing;
    float         env[NT*P];
    COMP          rx_symbols[FDMDV_NC+1];
    int           rx_bits[FDMDV_BITS_PER_FRAME];
    float         foff_fine;
    int           sync_bit, reliable_sync_bit;

    int           tx_bits_log[FDMDV_BITS_PER_FRAME*FRAMES];
    COMP          tx_symbols_log[(FDMDV_NC+1)*FRAMES];
    COMP          tx_fdm_log[M*FRAMES];
    COMP          pilot_baseband1_log[NPILOTBASEBAND*FRAMES];
    COMP          pilot_baseband2_log[NPILOTBASEBAND*FRAMES];
    COMP          pilot_lpf1_log[NPILOTLPF*FRAMES];
    COMP          pilot_lpf2_log[NPILOTLPF*FRAMES];
    COMP          S1_log[MPILOTFFT*FRAMES];
    COMP          S2_log[MPILOTFFT*FRAMES];
    float         foff_coarse_log[FRAMES];
    float         foff_log[FRAMES];
    COMP          rx_fdm_filter_log[(M+M/P)*FRAMES];
    int           rx_fdm_filter_log_index;
    COMP          rx_filt_log[NC+1][(P+1)*FRAMES];
    int           rx_filt_log_col_index;
    float         env_log[NT*P*FRAMES];
    float         rx_timing_log[FRAMES];
    COMP          rx_symbols_log[FDMDV_NC+1][FRAMES];
    COMP          phase_difference_log[FDMDV_NC+1][FRAMES];
    float         sig_est_log[FDMDV_NC+1][FRAMES];
    float         noise_est_log[FDMDV_NC+1][FRAMES];
    int           rx_bits_log[FDMDV_BITS_PER_FRAME*FRAMES];
    float         foff_fine_log[FRAMES];
    int           sync_bit_log[FRAMES];
    int           sync_log[FRAMES];
    int           nin_log[FRAMES];

    int           f,c,i,j,r;


    rx_fdm_filter_log_index = 0;
    rx_filt_log_col_index = 0;

    printf("sizeof FDMDV states: %zd bytes\n", sizeof(struct FDMDV));


    for(r=0; r<numberOfRuns; r++) {
        f = (currentRun*numberOfRuns+r)%FRAMES;

        /* --------------------------------------------------------*\
	                          Modulator
	\*---------------------------------------------------------*/

        fdmdv_get_test_bits(fdmdv, tx_bits);
        bits_to_dqpsk_symbols(tx_symbols, FDMDV_NC, fdmdv->prev_tx_symbols, tx_bits, &fdmdv->tx_pilot_bit, 0);
        memcpy(fdmdv->prev_tx_symbols, tx_symbols, sizeof(COMP)*(FDMDV_NC+1));
        tx_filter_and_upconvert(tx_fdm, FDMDV_NC , tx_symbols, fdmdv->tx_filter_memory,
                fdmdv->phase_tx, fdmdv->freq, &fdmdv->fbb_phase_tx, fdmdv->fbb_rect);

        /* --------------------------------------------------------*\
	                          Channel
	\*---------------------------------------------------------*/

        nin = next_nin;

        // nin = M;  // when debugging good idea to uncomment this to "open loop"

        /* add M tx samples to end of buffer */

        assert((channel_count + M) < CHANNEL_BUF_SIZE);
        for(i=0; i<M; i++)
            channel[channel_count+i] = tx_fdm[i].real;
        channel_count += M;

        /* take nin samples from start of buffer */

        for(i=0; i<nin; i++) {
            rx_fdm[i].real = channel[i];
            rx_fdm[i].imag = 0;
        }

        /* shift buffer back */

        for(i=0,j=nin; j<channel_count; i++,j++)
            channel[i] = channel[j];
        channel_count -= nin;

        /* --------------------------------------------------------*\
	                        Demodulator
	\*---------------------------------------------------------*/

        /* shift down to complex baseband */
        profileTimedEventStart(ProfileTP0);
        fdmdv_freq_shift(rx_fdm, rx_fdm, -FDMDV_FCENTRE, &fdmdv->fbb_phase_rx, nin);
        profileTimedEventStop(ProfileTP0);
        /* freq offset estimation and correction */

        // fdmdv->sync = 0; // when debugging good idea to uncomment this to "open loop"
        profileTimedEventStart(ProfileTP1);
        foff_coarse = rx_est_freq_offset(fdmdv, rx_fdm, nin, !fdmdv->sync);
        profileTimedEventStop(ProfileTP1);

        if (fdmdv->sync == 0)
            fdmdv->foff = foff_coarse;
        profileTimedEventStart(ProfileTP2);
        fdmdv_freq_shift(rx_fdm_fcorr, rx_fdm, -fdmdv->foff, &fdmdv->foff_phase_rect, nin);
        profileTimedEventStop(ProfileTP2);
        /* baseband processing */

        rxdec_filter(rx_fdm_filter, rx_fdm_fcorr, fdmdv->rxdec_lpf_mem, nin);

        profileTimedEventStart(ProfileTP3);
        down_convert_and_rx_filter(rx_filt, fdmdv->Nc, rx_fdm_filter, fdmdv->rx_fdm_mem, fdmdv->phase_rx, fdmdv->freq,
                fdmdv->freq_pol, nin, M/Q);
        profileTimedEventStop(ProfileTP3);
        profileTimedEventStart(ProfileTP4);
        rx_timing = rx_est_timing(rx_symbols, FDMDV_NC, rx_filt, fdmdv->rx_filter_mem_timing, env, nin, M);
        profileTimedEventStop(ProfileTP4);
        profileTimedEventStart(ProfileTP5);
        foff_fine = qpsk_to_bits(rx_bits, &sync_bit, FDMDV_NC, fdmdv->phase_difference, fdmdv->prev_rx_symbols, rx_symbols, 0);
        profileTimedEventStop(ProfileTP5);

        //for(i=0; i<FDMDV_NC;i++)
        //    printf("rx_symbols: %f %f prev_rx_symbols: %f %f phase_difference: %f %f\n", rx_symbols[i].real, rx_symbols[i].imag,
        //          fdmdv->prev_rx_symbols[i].real, fdmdv->prev_rx_symbols[i].imag, fdmdv->phase_difference[i].real, fdmdv->phase_difference[i].imag);
        //if (f==1)
        //   exit(0);
        profileTimedEventStart(ProfileTP6);
        snr_update(fdmdv->sig_est, fdmdv->noise_est, FDMDV_NC, fdmdv->phase_difference);
        profileTimedEventStop(ProfileTP6);
        memcpy(fdmdv->prev_rx_symbols, rx_symbols, sizeof(COMP)*(FDMDV_NC+1));

        next_nin = M;

        if (rx_timing > 2*M/P)
            next_nin += M/P;

        if (rx_timing < 0)
            next_nin -= M/P;
        profileTimedEventStart(ProfileTP7);
        fdmdv->sync = freq_state(&reliable_sync_bit, sync_bit, &fdmdv->fest_state, &fdmdv->timer, fdmdv->sync_mem);
        profileTimedEventStop(ProfileTP7);
        fdmdv->foff  -= TRACK_COEFF*foff_fine;

        /* --------------------------------------------------------*\
	                    Log each vector
	\*---------------------------------------------------------*/

        memcpy(&tx_bits_log[FDMDV_BITS_PER_FRAME*f], tx_bits, sizeof(int)*FDMDV_BITS_PER_FRAME);
        memcpy(&tx_symbols_log[(FDMDV_NC+1)*f], tx_symbols, sizeof(COMP)*(FDMDV_NC+1));
        memcpy(&tx_fdm_log[M*f], tx_fdm, sizeof(COMP)*M);

        memcpy(&pilot_baseband1_log[f*NPILOTBASEBAND], fdmdv->pilot_baseband1, sizeof(COMP)*NPILOTBASEBAND);
        memcpy(&pilot_baseband2_log[f*NPILOTBASEBAND], fdmdv->pilot_baseband2, sizeof(COMP)*NPILOTBASEBAND);
        memcpy(&pilot_lpf1_log[f*NPILOTLPF], fdmdv->pilot_lpf1, sizeof(COMP)*NPILOTLPF);
        memcpy(&pilot_lpf2_log[f*NPILOTLPF], fdmdv->pilot_lpf2, sizeof(COMP)*NPILOTLPF);
        memcpy(&S1_log[f*MPILOTFFT], fdmdv->S1, sizeof(COMP)*MPILOTFFT);
        memcpy(&S2_log[f*MPILOTFFT], fdmdv->S2, sizeof(COMP)*MPILOTFFT);
        foff_coarse_log[f] = foff_coarse;
        foff_log[f] = fdmdv->foff;

        /* rx filtering */

        for(i=0; i<nin; i++)
            rx_fdm_filter_log[rx_fdm_filter_log_index + i] = rx_fdm_filter[i];
        rx_fdm_filter_log_index += nin;

        for(c=0; c<NC+1; c++) {
            for(i=0; i<(P*nin)/M; i++)
                rx_filt_log[c][rx_filt_log_col_index + i] = rx_filt[c][i];
        }
        rx_filt_log_col_index += (P*nin)/M;

        /* timing estimation */

        memcpy(&env_log[NT*P*f], env, sizeof(float)*NT*P);
        rx_timing_log[f] = rx_timing;
        nin_log[f] = nin;

        for(c=0; c<FDMDV_NC+1; c++) {
            rx_symbols_log[c][f] = rx_symbols[c];
            phase_difference_log[c][f] = fdmdv->phase_difference[c];
        }

        /* qpsk_to_bits() */

        memcpy(&rx_bits_log[FDMDV_BITS_PER_FRAME*f], rx_bits, sizeof(int)*FDMDV_BITS_PER_FRAME);
        for(c=0; c<FDMDV_NC+1; c++) {
            sig_est_log[c][f] = fdmdv->sig_est[c];
            noise_est_log[c][f] = fdmdv->noise_est[c];
        }
        foff_fine_log[f] = foff_fine;
        sync_bit_log[f] = sync_bit;

        sync_log[f] = fdmdv->sync;


        /*---------------------------------------------------------*\
               Dump logs to Octave file for evaluation
                      by tfdmdv.m Octave script
    \*---------------------------------------------------------*/

        if (cumulativeOutput == 0 || (currentRun*FRAMES+f) == (FRAMES*FRAMERUNS - 1))
        {
            int range=cumulativeOutput?FRAMES:1;
            int start=cumulativeOutput?0:f;

#ifdef PROFILE_EVENTS
            for (int i = 0;i < 10;i++)
            {
                ProfilingTimedEvent* ev_ptr = profileTimedEventGet(i);
                if (ev_ptr->count != 0)
                {
#if CUMULATIVE
                    trace_printf("%d: %d uS per run\n",i, (ev_ptr->duration/(ev_ptr->count*168)));
#endif
                }
            }
#endif
            octave_save_int(fout, "tx_bits_log_c", &tx_bits_log[start * FDMDV_BITS_PER_FRAME], 1, FDMDV_BITS_PER_FRAME*range);
            octave_save_complex(fout, "tx_symbols_log_c", &tx_symbols_log[(FDMDV_NC+1)*start], 1, (FDMDV_NC+1)*range, (FDMDV_NC+1)*FRAMES);
            octave_save_complex(fout, "tx_fdm_log_c", &tx_fdm_log[M*start], 1, M*range, M*FRAMES);
            octave_save_complex(fout, "pilot_lut_c", fdmdv->pilot_lut, 1, NPILOT_LUT, NPILOT_LUT);
            octave_save_complex(fout, "pilot_baseband1_log_c", &pilot_baseband1_log[start*NPILOTBASEBAND], 1, NPILOTBASEBAND*range, NPILOTBASEBAND*FRAMES);
            octave_save_complex(fout, "pilot_baseband2_log_c", &pilot_baseband2_log[start*NPILOTBASEBAND], 1, NPILOTBASEBAND*range, NPILOTBASEBAND*FRAMES);
            octave_save_float(fout, "pilot_coeff_c", pilot_coeff, 1, NPILOTCOEFF, NPILOTCOEFF);
            octave_save_complex(fout, "pilot_lpf1_log_c", &pilot_lpf1_log[NPILOTLPF*start], 1, NPILOTLPF*range, NPILOTLPF*FRAMES);
            octave_save_complex(fout, "pilot_lpf2_log_c", &pilot_lpf2_log[NPILOTLPF*start], 1, NPILOTLPF*range, NPILOTLPF*FRAMES);
            octave_save_complex(fout, "S1_log_c", &S1_log[MPILOTFFT*start], 1, MPILOTFFT*range, MPILOTFFT*FRAMES);
            octave_save_complex(fout, "S2_log_c", &S2_log[MPILOTFFT*start], 1, MPILOTFFT*range, MPILOTFFT*FRAMES);
            octave_save_float(fout, "foff_log_c", &foff_log[start], 1, range, FRAMES);
            octave_save_float(fout, "foff_coarse_log_c", &foff_coarse_log[start], 1, range, FRAMES);
            octave_save_complex(fout, "rx_fdm_filter_log_c", &rx_fdm_filter_log[cumulativeOutput?0:rx_fdm_filter_log_index-nin_log[start]], 1, cumulativeOutput?rx_fdm_filter_log_index:nin_log[start], cumulativeOutput?rx_fdm_filter_log_index:nin_log[start]);

            int rx_f_l_diff = cumulativeOutput?rx_filt_log_col_index:(P*nin_log[start])/M;
            int rx_f_l_start = rx_filt_log_col_index - rx_f_l_diff;

            octave_save_complex(fout, "rx_filt_log_c", &rx_filt_log[0][rx_f_l_start], (FDMDV_NC+1), rx_f_l_diff, (P+1)*FRAMES);
            octave_save_float(fout, "env_log_c", &env_log[start*NT*P], 1, NT*P*range, NT*P*FRAMES);
            octave_save_float(fout, "rx_timing_log_c", &rx_timing_log[start], 1, range, FRAMES);
            octave_save_complex(fout, "rx_symbols_log_c", &rx_symbols_log[0][start], (FDMDV_NC+1), range, FRAMES);
            octave_save_complex(fout, "phase_difference_log_c", &phase_difference_log[0][start], (FDMDV_NC+1), range, FRAMES);
            octave_save_float(fout, "sig_est_log_c", &sig_est_log[0][start], (FDMDV_NC+1), range, FRAMES);
            octave_save_float(fout, "noise_est_log_c", &noise_est_log[0][start], (FDMDV_NC+1), range, FRAMES);
            octave_save_int(fout, "rx_bits_log_c", &rx_bits_log[FDMDV_BITS_PER_FRAME*start], 1, FDMDV_BITS_PER_FRAME*range);
            octave_save_float(fout, "foff_fine_log_c", &foff_fine_log[start], 1, range , FRAMES);
            octave_save_int(fout, "sync_bit_log_c", &sync_bit_log[start], 1, range);
            octave_save_int(fout, "sync_log_c", &sync_log[start], 1, range);
            octave_save_int(fout, "nin_log_c", &nin_log[start], 1, range);
        }
    }

    return next_nin;
}


int tfdmdv_main(int argc, char* argv[])
{
#ifndef ARM_MATH_CM4
    fout = fopen("tfdmdv_out.txt","wt");
    assert(fout != NULL);
#endif
#ifdef PROFILE_EVENTS
    profileTimedEventInit();
#endif
    FPRINTF(fout, "# Created by tfdmdv.c\n");
    fdmdv = fdmdv_create(FDMDV_NC);

    int i = 0;
    int next_nin = M;
    channel_count = 0;
    for (i = 0; i < FRAMERUNS; i++)
    {
        next_nin = run(CUMULATIVE,FRAMEINCR,i,next_nin);
    }
#ifndef ARM_MATH_CM4
    fclose(fout);
#endif

    fdmdv_destroy(fdmdv);
    return 0;
}