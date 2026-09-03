/*
 * usbradioplus_radio.c - native radio detection and signaling
 *
 * All Rights Reserved. Copyright (C)2007-2009, Xelatec, LLC
 *
 * 20070808 1235 Steven Henke, W9SH, sph@xelatec.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * This version may be optionally licenced under the GNU LGPL licence.
 *
 * A license has been granted to Digium (via disclaimer) for the use of
 * this code.

 * A license has been granted to Digium (via disclaimer) for the use of
 * this code.
 *
 * 20160829	inad	added rxlpf rxhpf txlpf txhpf
 * 20161024	inad	fixed set the number of coefficients
 * 20161027	WN3A    allow filters of different tap counts
 * 20090725 2039 sph@xelatec.com improved rxfrontend and squelch
  */

/*!
 * \file
 *
 * \brief Private Land Mobile Radio Channel Voice and Signaling Processor
 *
 * \author Steven Henke, W9SH <sph@xelatec.com> Xelatec, LLC
 */
/*
	FYI 	= For Your Information
	PMR 	= Private Mobile Radio
	RX  	= Receive
	TX  	= Transmit
	CTCSS	= Continuous Tone Coded Squelch System
	TONE	= Same as above.
	LSD 	= Low Speed Data, subaudible signaling. May be tones or codes.
	VOX 	= Voice Operated Transmit
	DSP 	= Digital Signal Processing
	LPF 	= Low Pass Filter
	FIR 	= Finite Impulse Response (Filter)
	IIR 	= Infinite Impulse Response (Filter)
*/

#define GCC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#if GCC_VERSION > 40600
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsequence-point"
#endif

#define N_FMT(duf) "%30" #duf /* Maximum sscanf conversion to numeric strings */
#include "asterisk.h"

#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>

#include "usbradioplus_radio.h"
#include "usbradioplus_radio_coefficients.h"
#include "asterisk/logger.h"

static i16 radioIndex = 0; /* Count live detector instances. */
static char disabled_code[] = "0";

/*
	Trace Routines
*/
void strace(i16 point, t_sdbg *sdbg, i16 index, i16 value)
{
	/* make dbg_trace buffer in structure */
	if (!sdbg->mode || sdbg->point[point] < 0) {
		return;
	} else {
		sdbg->buffer[(index * URP_RADIO_DEBUG_CHANNELS) + sdbg->point[point]] = value;
	}
}

/*

*/
void strace2(t_sdbg *sdbg)
{
	int i;
	for (i = 0; i < URP_RADIO_DEBUG_CHANNELS; i++) {
		if (sdbg->source[i]) {
			int ii;
			for (ii = 0; ii < SAMPLES_PER_BLOCK; ii++) {
				sdbg->buffer[ii * URP_RADIO_DEBUG_CHANNELS + i] =
					sdbg->source[i][ii];
			}
		}
	}
}

/*
	take source string allocate and copy
	copy is modified, delimiters are replaced with zeros to mark
	end of string
	count set pointers
	string_parse( char *src, char *dest, char **sub)
*/
i16 string_parse(char *src, char **dest, char ***ptrs)
{
	char *p, *pd;
	char *ptstr[1000];
	i16 i, slen, numsub;

	TRACEJ(2, "string_parse(%s)\n", src);

	slen = strlen(src);
	TRACEJ(2, " source len = %i\n", slen);

	pd = *dest;
	if (pd) {
		ast_free(pd);
		*dest = NULL;
	}
	pd = ast_calloc(slen + 1, 1);
	if (!pd) {
		return -1;
	}
	memcpy(pd, src, slen);
	*dest = pd;

	p = 0;
	numsub = 0;
	for (i = 0; i < slen + 1; i++) {
		TRACEJ(5, " pd[%i] = %c\n", i, pd[i]);

		if (p == 0 && pd[i] != ',' && pd[i] != ' ') {
			p = &(pd[i]);
		} else if (pd[i] == ',' || pd[i] == 0) {
			ptstr[numsub] = p;
			pd[i] = 0;
			p = 0;
			numsub++;
		}
	}

	for (i = 0; i < numsub; i++) {
		TRACEJ(5, " ptstr[%i] = %p %s\n", i, ptstr[i], ptstr[i]);
	}

	if (*ptrs) {
		ast_free(*ptrs);
		*ptrs = NULL;
	}
	*ptrs = ast_calloc(numsub, sizeof(char *));
	if (!*ptrs) {
		ast_free(*dest);
		*dest = NULL;
		return -1;
	}
	for (i = 0; i < numsub; i++) {
		(*ptrs)[i] = ptstr[i];
		TRACEJ(5, " %i = %s\n", i, (*ptrs)[i]);
	}
	TRACEJ(5, "string_parse()=%i\n\n", numsub);

	return numsub;
}

/*
	the parent program defines
	pRxCodeSrc and pTxCodeSrc string pointers to the list of codes
	pTxCodeDefault the default Tx Code.

*/
i16 urp_radio_parse_codes(urp_radio_state *pChan)
{
	i16 i, ii, hit, ti;
	char *p;
	float f, maxctcsstxfreq;

	urp_radio_stage *pSps;
	i16 maxctcssindex;

	TRACEF(1, "urp_radio_parse_codes(%i)\n", 0);
	TRACEF(1, "pChan->pRxCodeSrc %s \n", pChan->pRxCodeSrc);
	TRACEF(1, "pChan->pTxCodeSrc %s \n", pChan->pTxCodeSrc);
	TRACEF(1, "pChan->pTxCodeDefault %s \n", pChan->pTxCodeDefault);

	maxctcssindex = CTCSS_NULL;
	maxctcsstxfreq = CTCSS_NULL;
	pChan->txctcssdefault_index = CTCSS_NULL;
	pChan->txctcssdefault_value = CTCSS_NULL;

	pChan->b.ctcssRxEnable = pChan->b.ctcssTxEnable = 0;
	pChan->b.dcsRxEnable = pChan->b.dcsTxEnable = 0;
	pChan->b.lmrRxEnable = pChan->b.lmrTxEnable = 0;
	pChan->b.mdcRxEnable = pChan->b.mdcTxEnable = 0;
	pChan->b.dstRxEnable = pChan->b.dstTxEnable = 0;
	pChan->b.p25RxEnable = pChan->b.p25TxEnable = 0;

	TRACEF(1, "urp_radio_parse_codes(%i) 05\n", 0);

	pChan->numrxcodes =
		string_parse(pChan->pRxCodeSrc, &(pChan->pRxCodeStr), &(pChan->pRxCode));
	if (pChan->numrxcodes < 0) {
		return 1;
	}
	pChan->numtxcodes =
		string_parse(pChan->pTxCodeSrc, &(pChan->pTxCodeStr), &(pChan->pTxCode));
	if (pChan->numtxcodes < 0) {
		return 1;
	}

	if (pChan->numrxcodes != pChan->numtxcodes) {
		ast_log(LOG_ERROR, "numrxcodes != numtxcodes \n");
	}
	pChan->rxCtcss->enabled = 0;
	pChan->rxCtcss->gain = 1 * M_Q8;
	pChan->rxCtcss->limit = 8192;
	pChan->rxCtcss->input = pChan->pRxLsdLimit;
	pChan->rxCtcss->decode = CTCSS_NULL;

	pChan->rxCtcss->testIndex = 3;

	pChan->rxctcssfreq[0] = 0; /* decode now   CTCSS_RXONLY */

	for (i = 0; i < CTCSS_NUM_CODES; i++) {
		pChan->rxctcss[i] = 0;
		pChan->txctcss[i] = 0;
		pChan->rxCtcssMap[i] = CTCSS_NULL;
	}

	TRACEF(1, "urp_radio_parse_codes(%i) 10\n", 0);

	/* Do Receive Codes String */
	for (i = 0; i < pChan->numrxcodes; i++) {
		p = pChan->pStr = pChan->pRxCode[i];

		{
			i16 rx_index, tx_index;
			float frequency;

			sscanf(p, N_FMT(frequency), &frequency);
			rx_index = urp_ctcss_frequency_index(frequency);
			if (rx_index == CTCSS_NULL) {
				ast_log(LOG_ERROR,
					"Invalid RX CTCSS code detected and ignored. %i %s\n", i,
					pChan->pRxCode[i]);

			} else if (rx_index > maxctcssindex) {
				maxctcssindex = rx_index;
			}

			if (i < pChan->numtxcodes) { /* more rx codes than tx codes */
				sscanf(pChan->pTxCode[i], N_FMT(frequency), &frequency);
				tx_index = urp_ctcss_frequency_index(frequency);
				if (tx_index == CTCSS_NULL) {
					if (frequency != 0.0) {
						frequency = -1.0; /* tone freq not valid */
						ast_log(LOG_ERROR,
							"Invalid TX CTCSS code detected and "
							"ignored. %i %s\n",
							i, pChan->pTxCode[i]);
					}
				} else if (frequency > maxctcsstxfreq) {
					maxctcsstxfreq = frequency;
				}
			} else {
				tx_index = CTCSS_NULL;
				frequency = -1.0; /* tone freq not provided */
				ast_log(LOG_ERROR, "Invalid CTCSS configuration. Number of rx "
						   "codes > number of tx codes\n");
			}

			if (rx_index > CTCSS_NULL && tx_index > CTCSS_NULL) {
				pChan->b.ctcssRxEnable = 1;
				pChan->b.ctcssTxEnable = 1;
				pChan->rxCtcssMap[rx_index] = tx_index;
				pChan->numrxctcssfreqs++;
				TRACEF(1, "pChan->rxctcss[%i]=%s  pChan->rxCtcssMap[%i]=%i\n", i,
				       pChan->rxctcss[i], rx_index, tx_index);
			} else if (rx_index > CTCSS_NULL && frequency == 0) {
				pChan->b.ctcssRxEnable = 1;
				pChan->rxCtcssMap[rx_index] = CTCSS_RXONLY;
				pChan->numrxctcssfreqs++;
				TRACEF(1,
				       "pChan->rxctcss[%i]=%s  pChan->rxCtcssMap[%i]=%i RXONLY\n",
				       i, pChan->rxctcss[i], rx_index, tx_index);
			} else {
				i16 clear_index;

				pChan->numrxctcssfreqs = 0;
				ast_log(LOG_ERROR,
					"Invalid CTCSS configuration. CTCSS has been disabled\n");
				for (clear_index = 0; clear_index < CTCSS_NUM_CODES;
				     clear_index++) {
					pChan->rxCtcssMap[clear_index] = CTCSS_NULL;
				}
			}
		}
	}

	TRACEF(1, "urp_radio_parse_codes() CTCSS Init Struct  %i  %i\n", pChan->b.ctcssRxEnable,
	       pChan->b.ctcssTxEnable);
	if (pChan->b.ctcssRxEnable) {
		pChan->rxHpfEnable = 1;
		pChan->spsRxLsdNrz->enabled = pChan->rxCenterSlicerEnable = 1;
		pChan->rxCtcssDecodeEnable = 1;
		pChan->rxCtcss->enabled = 1;
	} else {
		pChan->rxHpfEnable = 1;
		pChan->spsRxLsdNrz->enabled = pChan->rxCenterSlicerEnable = 0;
		pChan->rxCtcssDecodeEnable = 0;
		pChan->rxCtcss->enabled = 0;
	}

	TRACEF(1, "urp_radio_parse_codes() CTCSS Init Decoders \n");
	for (i = 0; i < CTCSS_NUM_CODES; i++) {
		urp_ctcss_tone_detector *ptdet;
		ptdet = &(pChan->rxCtcss->tdet[i]);
		ptdet->counterFactor = coef_ctcss_div[i];
		ptdet->state = 1;
		ptdet->setpt = (M_Q15 * 0.041); /* 0.069 */
		ptdet->hyst = (M_Q15 * 0.0130);
		ptdet->binFactor = (M_Q15 * 0.135); /* was 0.140 */
		ptdet->fudgeFactor = 8;
	}

	/* DEFAULT TX CODE */
	TRACEF(1, "urp_radio_parse_codes() Default Tx Code %s \n", pChan->pTxCodeDefault);
	pChan->txcodedefaultsmode = SMODE_NULL;
	p = pChan->pStr = pChan->pTxCodeDefault;

	{
		sscanf(p, N_FMT(f), &f);
		ti = urp_ctcss_frequency_index(f);
		if (ti == CTCSS_NULL) {
			ast_log(LOG_ERROR,
				"Invalid default TX CTCSS code detected and ignored. %s\n",
				pChan->pTxCodeDefault);
		} else if (f > maxctcsstxfreq) {
			maxctcsstxfreq = f;
		}

		if (ti > CTCSS_NULL) {
			pChan->b.ctcssTxEnable = 1;
			pChan->txctcssdefault_index = ti;
			pChan->txctcssdefault_value = f;
			pChan->txCtcssFreq10 = f * 10;
			pChan->txcodedefaultsmode = SMODE_CTCSS;
			TRACEF(1, "urp_radio_parse_codes() Tx Default CTCSS = %s %i %f\n", p, ti,
			       f);
		}
	}

	/* Native CTCSS uses the same legacy filter selection for level matching. */
	TRACEF(1, "urp_radio_parse_codes() Filter Config \n");
	if (maxctcsstxfreq > 203.5) {
		pChan->txCtcssFilter250 = 1;
		TRACEF(1, "urp_radio_parse_codes() Tx Filter Freq High\n");
	} else {
		pChan->txCtcssFilter250 = 0;
		TRACEF(1, "urp_radio_parse_codes() Tx Filter Freq Low\n");
	}

	/* CTCSS Rx Decoder Low Pass Filter */
	hit = 0;
	ii = urp_ctcss_frequency_index(203.5);
	for (i = ii; i < CTCSS_NUM_CODES; i++) {
		if (pChan->rxCtcssMap[i] > CTCSS_NULL) {
			hit = 1;
		}
	}

	pSps = pChan->spsRxLsd;
	ast_free(pSps->x);
	pSps->x = NULL;

	if (hit) {
		pSps->ncoef = taps_fir_lpf_250_9_66;
		pSps->size_coef = 2;
		pSps->coef = (void *)coef_fir_lpf_250_9_66;
		pSps->nx = taps_fir_lpf_250_9_66;
		pSps->size_x = 2;
		pSps->x = ast_calloc(pSps->nx, pSps->size_x);
		if (pSps->x == NULL) {
			return 1;
		}
		pSps->calcAdjust = gain_fir_lpf_250_9_66;
		TRACEF(1, "urp_radio_parse_codes() Rx Filter Freq High\n");
	} else {
		pSps->ncoef = taps_fir_lpf_215_9_88;
		pSps->size_coef = 2;
		pSps->coef = (void *)coef_fir_lpf_215_9_88;
		pSps->nx = taps_fir_lpf_215_9_88;
		pSps->size_x = 2;
		pSps->x = ast_calloc(pSps->nx, pSps->size_x);
		if (pSps->x == NULL) {
			return 1;
		}
		pSps->calcAdjust = gain_fir_lpf_215_9_88;
		TRACEF(1, "urp_radio_parse_codes() Rx Filter Freq Low\n");
	}

	if (pChan->b.ctcssRxEnable) {
		pChan->rxCenterSlicerEnable = 1;
		pSps->enabled = 1;
	} else {
		pChan->rxCenterSlicerEnable = 0;
		pSps->enabled = 0;
	}

#if URP_RADIO_DEBUG == 1
	TRACEF(2, "urp_radio_parse_codes() ctcssRxEnable = %i \n", pChan->b.ctcssRxEnable);
	TRACEF(2, "                    ctcssTxEnable = %i \n", pChan->b.ctcssTxEnable);
	TRACEF(2, "                      dcsRxEnable = %i \n", pChan->b.dcsRxEnable);
	TRACEF(2, "                      lmrRxEnable = %i \n", pChan->b.lmrRxEnable);
	TRACEF(2, "               txcodedefaultsmode = %i \n", pChan->txcodedefaultsmode);
	for (i = 0; i < CTCSS_NUM_CODES; i++) {
		TRACEF(2, "rxCtcssMap[%i] = %i \n", i, pChan->rxCtcssMap[i]);
	}
#endif

	TRACEF(1, "urp_radio_parse_codes(%i) end\n", 0);

	return 0;
}

/*
	Convert a Frequency in Hz to a zero based CTCSS Table index
*/
i16 urp_ctcss_frequency_index(float freq)
{
	i16 i, hit = CTCSS_NULL;

	for (i = 0; i < CTCSS_NUM_CODES; i++) {
		if (freq == freq_ctcss[i]) {
			hit = i;
		}
	}
	return hit;
}

/*
	urp_radio_receive_frontend
	Takes a block of data and low pass filters it.
	Determines the amplitude of high frequency noise for carrier detect.
	Decimates input data to change the rate.
*/
i16 urp_radio_receive_frontend(urp_radio_stage *mySps)
{
#define DCgainBpfNoise 65536

	i16 samples, nx, iOutput, *output, *noutput;
	const i16 *input;
	i16 *x;
	i16 decimator, decimate, doNoise, fever, fev1;
	i32 i, naccum, outputGain, calcAdjust;
	i64 y, npwr;

	TRACEJ(5, "urp_radio_receive_frontend()\n");

	if (!mySps->enabled) {
		return 1;
	}

	decimator = mySps->decimator;
	decimate = mySps->decimate;

	input = mySps->source;
	output = mySps->sink;
	noutput = mySps->parentChan->pRxNoise;
	fever = mySps->parentChan->fever;

	nx = mySps->nx;

	calcAdjust = mySps->calcAdjust;
	outputGain = mySps->outputGain;

	samples = mySps->nSamples * decimate;
	x = mySps->x;
	iOutput = 0;
	npwr = 0;

	if (mySps->parentChan->rxCdType != CD_XPMR_VOX) {
		doNoise = 1;
	} else {
		doNoise = 0;
	}

	if (fever) {
		fev1 = (nx - 1) * 2;
	} else {
		fev1 = nx - 1;
	}

	for (i = 0; i < samples; i++) {
		i16 n;

		/* shift the old samples */
		memmove(x + 1, x, fev1);
		x[0] = input[i * 2];

#if URP_RADIO_TRACE_FRONTEND == 1
		y = 0;
		for (n = 0; n < nx; n++) {
			y += fir_rxlpf[mySps->parentChan->rxlpf].coefs[n] * x[n];
		}

		y = ((y / calcAdjust) * outputGain) / M_Q8;
		input[i * 2] = y; /* debug output LowPass at 48KS/s */
#endif

		if (doNoise) {
			/* calculate noise filter output */
			naccum = 0;
			if (mySps->parentChan->rxNoiseFilType == 0) {
				for (n = 0; n < taps_fir_bpf_noise_1; n++) {
					naccum += coef_fir_bpf_noise_1[n] * x[n];
				}
				naccum /= DCgainBpfNoise;
			} else {
				for (n = 0; n < taps_fir_bpf_noise_2; n++) {
					naccum += coef_fir_bpf_noise_2[n] * x[n];
				}
				naccum /= gain_fir_bpf_noise_2;
			}
#if URP_RADIO_TRACE_FRONTEND == 1
			input[i * 2 + 1] = naccum; /* output noise filter results */
#endif
			npwr += naccum * naccum;
		}

		--decimator;

		if (decimator <= 0) {
			decimator = decimate;

			y = 0;
			for (n = 0; n < nx; n++) {
				y += fir_rxlpf[mySps->parentChan->rxlpf].coefs[n] * x[n];
			}

			y = ((y / calcAdjust) * outputGain) / M_Q8;

#if URP_RADIO_TRACE_OVFLW == 1
			if (y > 32767) {
				y = 32767;
				ast_log(LOG_ERROR, "urp_radio_receive_frontend() OVRFLW \n");
			} else if (y < -32767) {
				y = -32767;
				ast_log(LOG_ERROR, "urp_radio_receive_frontend() UNDFLW \n");
			}
#else
			if (y > 32767) {
				y = 32767;
			} else if (y < -32767) {
				y = -32767;
			}
#endif
			output[iOutput++] = y; /* Rx Baseband decimated */

		} /* if decimator */
	}

	if (doNoise) {
		npwr = sqrt(npwr) / 16;

		/* compOut means muted. MICOR-style bi-level timing suppresses clean
		 * tails while keeping weak, fluttering speech from being chopped. */
		mySps->compOut = urp_micor_squelch_update(&mySps->micor_squelch, mySps->compOut,
							  (uint32_t)npwr, (uint32_t)mySps->setpt,
							  (uint32_t)mySps->hyst, MS_PER_FRAME);

#if URP_RADIO_DEBUG == 1
		if (mySps->parentChan->tracetype) {
			for (i = 0; i < mySps->nSamples; i++) {
				noutput[i] = npwr;
			}
		}
#endif

		((urp_radio_state *)(mySps->parentChan))->rxRssi = mySps->apeak = npwr;
	}

	return 0;
}
/*
	pmr general purpose fir
	works on a block of samples
*/
i16 urp_radio_fir(urp_radio_stage *mySps)
{
	i32 nsamples, inputGain, outputGain, calcAdjust;
	const i16 *input;
	i16 *output;
	i16 *x;
	const i16 *coef;
	i32 i, ii;
	i16 nx, hyst, setpt, compOut;
	i16 amax, amin, apeak = 0, discounteru = 0, discounterl = 0, discfactor;
	i16 decimator, decimate, interpolate;
	i16 numChanOut, selChanOut, mixOut, monoOut;

	TRACEJ(5, "urp_radio_fir() %i %i\n", mySps->index, mySps->enabled);

	if (!mySps->enabled) {
		return 1;
	}

	inputGain = mySps->inputGain;
	calcAdjust = mySps->calcAdjust;
	outputGain = mySps->outputGain;

	input = mySps->source;
	output = mySps->sink;
	x = mySps->x;
	nx = mySps->nx;
	coef = mySps->coef;

	decimator = mySps->decimator;
	decimate = mySps->decimate;
	interpolate = mySps->interpolate;

	setpt = mySps->setpt;
	compOut = mySps->compOut;

	numChanOut = mySps->numChanOut;
	selChanOut = mySps->selChanOut;
	mixOut = mySps->mixOut;
	monoOut = mySps->monoOut;

	amax = mySps->amax;
	amin = mySps->amin;
	discounteru = mySps->discounteru;
	discounterl = mySps->discounterl;

	discfactor = mySps->discfactor;
	hyst = mySps->hyst;
	nsamples = mySps->nSamples;

	if (mySps->option == 3) {
		mySps->option = 0;
		mySps->enabled = 0;
		for (i = 0; i < nsamples; i++) {
			if (monoOut) {
				output[(i * 2)] = output[(i * 2) + 1] = 0;
			} else {
				output[(i * numChanOut) + selChanOut] = 0;
			}
		}
		return 0;
	}

	ii = 0;
	for (i = 0; i < nsamples; i++) {
		int ix;

		int64_t y = 0;

		if (decimate < 0) {
			decimator = decimate;
		}

		for (ix = 0; ix < interpolate; ix++) {
			i16 n;
			y = 0;

			for (n = nx - 1; n > 0; n--) {
				x[n] = x[n - 1];
			}
			x[0] = (input[i] * inputGain) / M_Q8;

			for (n = 0; n < nx; n++) {
				y += coef[n] * x[n];
			}

			y = ((y / calcAdjust) * outputGain) / M_Q8;

			if (y > 32767) {
				y = 32767; /* overflow */
			} else if (y < -32767) {
				y = -32767;
			}

			if (mixOut) {
				if (monoOut) {
					output[(ii * 2)] = output[(ii * 2) + 1] += y;
				} else {
					output[(ii * numChanOut) + selChanOut] += y;
				}
			} else {
				if (monoOut) {
					output[(ii * 2)] = output[(ii * 2) + 1] = y;
				} else {
					output[(ii * numChanOut) + selChanOut] = y;
				}
			}
			ii++;
		}

		/* amplitude detector */
		if (setpt) {
			i16 accum = y;

			if (accum > amax) {
				amax = accum;
				discounteru = discfactor;
			} else if (--discounteru <= 0) {
				discounteru = discfactor;
				amax = (i32)((amax * 32700) / 32768);
			}

			if (accum < amin) {
				amin = accum;
				discounterl = discfactor;
			} else if (--discounterl <= 0) {
				discounterl = discfactor;
				amin = (i32)((amin * 32700) / 32768);
			}

			apeak = (i32)(amax - amin) / 2;

			if (apeak > setpt) {
				compOut = 1;
			} else if (compOut && (apeak < (setpt - hyst))) {
				compOut = 0;
			}
		}
	}

	mySps->decimator = decimator;

	mySps->amax = amax;
	mySps->amin = amin;
	mySps->apeak = apeak;
	mySps->discounteru = discounteru;
	mySps->discounterl = discounterl;

	mySps->compOut = compOut;

	return 0;
}

/*
	general purpose integrator lpf
*/
i16 gp_inte_00(urp_radio_stage *mySps)
{
	i16 npoints;
	const i16 *input;
	i16 *output;

	i32 outputGain;
	i32 i;
	i32 state00;
	i16 coeff00, coeff01;

	TRACEJ(5, "gp_inte_00() %i\n", mySps->enabled);
	if (!mySps->enabled) {
		return 1;
	}

	input = mySps->source;
	output = mySps->sink;

	npoints = mySps->nSamples;

	outputGain = mySps->outputGain;

	coeff00 = ((i16 *)mySps->coef)[0];
	coeff01 = ((i16 *)mySps->coef)[1];
	state00 = ((i32 *)mySps->x)[0];

	/* note fixed gain of 2 to compensate for attenuation */
	/* in passband */

	for (i = 0; i < npoints; i++) {
		i32 accum;

		accum = input[i];
		state00 = accum + (state00 * coeff01) / M_Q15;
		accum = (state00 * coeff00) / (M_Q15 / 4);
		output[i] = (accum * outputGain) / M_Q8;
	}

	((i32 *)(mySps->x))[0] = state00;

	return 0;
}

/* 	----------------------------------------------------------------------
	CenterSlicer
*/
i16 CenterSlicer(urp_radio_stage *mySps)
{
	i16 npoints;
	const i16 *input;
	i16 *output, *buff;

	i32 inputGainB;
	i32 i;
	i32 accum;

	i32 amax;  /* buffer amplitude maximum */
	i32 amin;  /* buffer amplitude minimum */
	i32 apeak; /* buffer amplitude peak */
	i32 center;
	i32 setpt; /* amplitude set point for peak tracking */

	i32 discounteru; /* amplitude detector integrator discharge counter upper */
	i32 discounterl; /* amplitude detector integrator discharge counter lower */
	i32 discfactor;	 /* amplitude detector integrator discharge factor */

	TRACEJ(5, "CenterSlicer() %i\n", mySps->enabled);
	if (!mySps->enabled) {
		return 1;
	}

	input = mySps->source;
	output = mySps->sink; /* limited output */
	buff = mySps->buff;

	npoints = mySps->nSamples;

	inputGainB = mySps->inputGainB;

	amax = mySps->amax;
	amin = mySps->amin;
	setpt = mySps->setpt;
	apeak = mySps->apeak;
	discounteru = mySps->discounteru;
	discounterl = mySps->discounterl;

	discfactor = mySps->discfactor;
	for (i = 0; i < npoints; i++) {
		static i32 tfx;
		accum = input[i];

		if (accum > amax) {
			amax = accum;
			if (amin < (amax - setpt)) {
				amin = (amax - setpt);
			}
		} else if (accum < amin) {
			amin = accum;
			if (amax > (amin + setpt)) {
				amax = (amin + setpt);
			}
		}

		if ((amax -= discfactor) < amin) {
			amax = amin;
		}

		if ((amin += discfactor) > amax) {
			amin = amax;
		}

		apeak = (amax - amin) / 2;
		center = (amax + amin) / 2;
		accum = accum - center;

		output[i] = accum; /* sink output unlimited/centered. */

		/* do limiter function */
		if (accum > inputGainB) {
			accum = inputGainB;
		} else if (accum < -inputGainB) {
			accum = -inputGainB;
		}

		buff[i] = accum;

#if URP_RADIO_DEBUG == 1
		if ((tfx++ / 8) & 1) { /* trace min/max levels */
			mySps->parentChan->pRxLsdCen[i] = amax;
		} else {
			mySps->parentChan->pRxLsdCen[i] = amin;
		}
#endif
	}

	mySps->amax = amax;
	mySps->amin = amin;
	mySps->apeak = apeak;
	mySps->discounteru = discounteru;
	mySps->discounterl = discounterl;

	return 0;
}

/* 	----------------------------------------------------------------------
	MeasureBlock
	determine peak amplitude
*/
i16 MeasureBlock(urp_radio_stage *mySps)
{
	i16 npoints;
	const i16 *input;
	i16 *output;

	i32 i;
	i16 amax;      /* buffer amplitude maximum */
	i16 amin;      /* buffer amplitude minimum */
	i16 apeak = 0; /* buffer amplitude peak (peak to peak)/2 */
	i16 setpt;     /* amplitude set point for amplitude comparator */

	i32 discounteru; /* amplitude detector integrator discharge counter upper */
	i32 discounterl; /* amplitude detector integrator discharge counter lower */
	i32 discfactor;	 /* amplitude detector integrator discharge factor */

	TRACEJ(5, "MeasureBlock() %i\n", mySps->enabled);

	if (!mySps->enabled) {
		return 1;
	}

	if (mySps->option == 3) {
		mySps->amax = mySps->amin = mySps->apeak = mySps->discounteru = mySps->discounterl =
			mySps->enabled = 0;
		return 1;
	}

	input = mySps->source;
	output = mySps->sink;

	npoints = mySps->nSamples;

	amax = mySps->amax;
	amin = mySps->amin;
	setpt = mySps->setpt;
	discounteru = mySps->discounteru;
	discounterl = mySps->discounterl;

	discfactor = mySps->discfactor;
	for (i = 0; i < npoints; i++) {
		i32 accum;

		accum = input[i];

		if (accum > amax) {
			amax = accum;
			discounteru = discfactor;
		} else if (--discounteru <= 0) {
			discounteru = discfactor;
			amax = (i32)((amax * 32700) / 32768);
		}

		if (accum < amin) {
			amin = accum;
			discounterl = discfactor;
		} else if (--discounterl <= 0) {
			discounterl = discfactor;
			amin = (i32)((amin * 32700) / 32768);
		}

		apeak = (i32)(amax - amin) / 2;
		if (output) {
			output[i] = apeak;
		}
	}

	mySps->amax = amax;
	mySps->amin = amin;
	mySps->apeak = apeak;
	mySps->discounteru = discounteru;
	mySps->discounterl = discounterl;
	if (apeak >= setpt) {
		mySps->compOut = 1;
	} else {
		mySps->compOut = 0;
	}

	return 0;
}

/*
	DelayLine
*/
i16 DelayLine(urp_radio_stage *mySps)
{
	const i16 *input;
	i16 *output, *buff;
	i16 i, npoints, buffsize, inindex, outindex;

	const urp_radio_state *pChan;
	pChan = mySps->parentChan;
	(void)pChan; /* Used only by trace macros when tracing is compiled in. */
	TRACEF(5, " DelayLine() %i\n", mySps->enabled);

	if (!mySps->enabled || mySps->b.outzero) {
		if (mySps->b.dirty) {
			mySps->b.dirty = 0;
			mySps->buffInIndex = 0;
			memset((void *)(mySps->buff), 0, mySps->buffSize * 2);
			memset((void *)(mySps->sink), 0, mySps->nSamples * 2);
		}
		return 0;
	}

	input = mySps->source;
	output = mySps->sink;
	buff = (i16 *)(mySps->buff);
	buffsize = mySps->buffSize;
	npoints = mySps->nSamples;
	inindex = mySps->buffInIndex;
	outindex = inindex - mySps->buffLead;

	if (outindex < 0) {
		outindex += buffsize;
	}

	for (i = 0; i < npoints; i++) {
		inindex %= buffsize;
		outindex %= buffsize;
		buff[inindex] = input[i];
		output[i] = buff[outindex];
		inindex++;
		outindex++;
	}
	mySps->buffInIndex = inindex;
	mySps->b.dirty = 1;
	return 0;
}

/*
	Continuous Tone Coded Squelch (CTCSS) Detector
*/
i16 urp_ctcss_decode(urp_radio_state *pChan)
{
	i16 i, points2do, thit, relax;
	const i16 *pInput;
	i16 tnum, tmp, indexNow, diffpeak;
	i16 tv0, tv1, tv2, tv3, indexDebug;
	i16 points = 0;
	i16 indexWas = 0;

	TRACEF(5, "urp_ctcss_decode(%p) %i %i %i %i\n", pChan, pChan->rxCtcss->enabled, 0,
	       pChan->rxCtcss->testIndex, pChan->rxCtcss->decode);

	if (!pChan->rxCtcss->enabled) {
		return 1;
	}

	relax = pChan->rxCtcss->relax;
	pInput = pChan->rxCtcss->input;

	thit = -1;

	for (tnum = 0; tnum < CTCSS_NUM_CODES; tnum++) {
		i32 accum, peak;
		urp_ctcss_tone_detector *ptdet;
		i16 fudgeFactor;
		i16 binFactor;

		TRACEF(6, " urp_ctcss_decode() tnum=%i %i\n", tnum, pChan->rxCtcssMap[tnum]);

		if ((pChan->rxCtcssMap[tnum] == CTCSS_NULL) ||
		    (pChan->rxCtcss->decode > CTCSS_NULL && (tnum != pChan->rxCtcss->decode))) {
			continue;
		}

		TRACEF(6, " urp_ctcss_decode() tnum=%i\n", tnum);

		ptdet = &(pChan->rxCtcss->tdet[tnum]);
		indexDebug = 0;
		points = points2do = pChan->nSamplesRx;
		fudgeFactor = ptdet->fudgeFactor;
		binFactor = ptdet->binFactor;

		while (ptdet->counter < (points2do * CTCSS_SCOUNT_MUL)) {
			tmp = (ptdet->counter / CTCSS_SCOUNT_MUL) + 1;
			ptdet->counter -= (tmp * CTCSS_SCOUNT_MUL);
			points2do -= tmp;
			indexNow = points - points2do;

			ptdet->counter += ptdet->counterFactor;

			accum = pInput[indexNow - 1]; /* duuuude's major bug fix! */

			ptdet->z[ptdet->zIndex] +=
				(((accum - ptdet->z[ptdet->zIndex]) * binFactor) / M_Q15);

			peak = abs(ptdet->z[0] - ptdet->z[2]) + abs(ptdet->z[1] - ptdet->z[3]);

			if (ptdet->peak < peak) {
				ptdet->peak += (((peak - ptdet->peak) * binFactor) / M_Q15);
			} else {
				ptdet->peak = peak;
			}

			{
				static const i16 a0 = 13723;
				static const i16 a1 = -13723;
				i32 temp0, temp1;
				i16 x0;

				/* differentiate */
				x0 = ptdet->zd;
				temp0 = x0 * a1;
				ptdet->zd = ptdet->peak;
				temp1 = ptdet->peak * a0;
				diffpeak = (temp0 + temp1) / 1024;
			}

			if (diffpeak < (-0.03 * M_Q15)) {
				ptdet->dvd -= 4;
			} else if (ptdet->dvd < 0) {
				ptdet->dvd++;
			}

			if ((ptdet->dvd < -12) && diffpeak > (-0.02 * M_Q15)) {
				ptdet->dvu += 2;
			} else if (ptdet->dvu) {
				ptdet->dvu--;
			}

			tmp = ptdet->setpt;
			if (pChan->rxCtcss->decode == tnum) {
				if (relax) {
					tmp = (tmp * 55) / 100;
				} else {
					tmp = (tmp * 80) / 100;
				}
			}

			if (ptdet->peak > tmp) {
				if (ptdet->decode < (fudgeFactor * 32)) {
					ptdet->decode++;
				}
			} else if (pChan->rxCtcss->decode == tnum) {
				if (ptdet->peak > ptdet->hyst) {
					ptdet->decode--;
				} else if (relax) {
					ptdet->decode--;
				} else {
					ptdet->decode -= 4;
				}
			} else {
				ptdet->decode = 0;
			}

			if ((pChan->rxCtcss->decode == tnum) && !relax &&
			    (ptdet->dvu > (0.00075 * M_Q15))) {
				ptdet->decode = 0;
				ptdet->z[0] = ptdet->z[1] = ptdet->z[2] = ptdet->z[3] = ptdet->dvu =
					0;
				TRACEF(4,
				       "urp_ctcss_decode() turnoff detected by dvdt for tnum = "
				       "%i.\n",
				       tnum);
			}

			if (ptdet->decode < 0 || !pChan->rxCarrierDetect) {
				ptdet->decode = 0;
			}

			if (ptdet->decode >= fudgeFactor) {
				thit = tnum;
				if (pChan->rxCtcss->decode != tnum) {
					ptdet->zd = ptdet->dvu = ptdet->dvd = 0;
				}
			}

#if URP_RADIO_DEBUG == 1
			if (thit >= 0 && thit == tnum) {
				TRACEF(6, " urp_ctcss_decode() %i %i %i %i \n", tnum, ptdet->peak,
				       ptdet->setpt, ptdet->hyst);
			}

			tv0 = ptdet->peak;
			tv1 = ptdet->decode;
			tv2 = tmp;
			tv3 = ptdet->dvu * 32;

			if (indexDebug == 0) {
				ptdet->lasttv0 = ptdet->pDebug0[points - 1];
				ptdet->lasttv1 = ptdet->pDebug1[points - 1];
				ptdet->lasttv2 = ptdet->pDebug2[points - 1];
				ptdet->lasttv3 = ptdet->pDebug3[points - 1];
			}

			while (indexDebug < indexNow) {
				ptdet->pDebug0[indexDebug] = ptdet->lasttv0;
				ptdet->pDebug1[indexDebug] = ptdet->lasttv1;
				ptdet->pDebug2[indexDebug] = ptdet->lasttv2;
				ptdet->pDebug3[indexDebug] = ptdet->lasttv3;
				indexDebug++;
			}
			ptdet->lasttv0 = tv0;
			ptdet->lasttv1 = tv1;
			ptdet->lasttv2 = tv2;
			ptdet->lasttv3 = tv3;
#endif
			indexWas = indexNow;
			ptdet->zIndex = (ptdet->zIndex + 1) % 4;
		}
		ptdet->counter -= (points2do * CTCSS_SCOUNT_MUL);

#if URP_RADIO_DEBUG == 1
		for (i = indexWas; i < points; i++) {
			ptdet->pDebug0[i] = ptdet->lasttv0;
			ptdet->pDebug1[i] = ptdet->lasttv1;
			ptdet->pDebug2[i] = ptdet->lasttv2;
			ptdet->pDebug3[i] = ptdet->lasttv3;
		}
#endif
	}

	if (pChan->rxCtcss->BlankingTimer > 0) {
		pChan->rxCtcss->BlankingTimer -= points;
	}

	if (pChan->rxCtcss->BlankingTimer < 0) {
		pChan->rxCtcss->BlankingTimer = 0;
	}

	if (thit > CTCSS_NULL && pChan->rxCtcss->decode <= CTCSS_NULL &&
	    !pChan->rxCtcss->BlankingTimer) {
		pChan->rxCtcss->decode = thit;
		sprintf(pChan->rxctcssfreq, "%.1f", freq_ctcss[thit]);
		TRACEC(1, "ctcss decode  %i  %.1f\n", thit, freq_ctcss[thit]);
	} else if (thit <= CTCSS_NULL && pChan->rxCtcss->decode > CTCSS_NULL) {
		pChan->rxCtcss->BlankingTimer = SAMPLE_RATE_NETWORK / 5;
		pChan->rxCtcss->decode = CTCSS_NULL;
		strcpy(pChan->rxctcssfreq, "0");
		TRACEC(1, "ctcss decode  NULL\n");
		for (tnum = 0; tnum < CTCSS_NUM_CODES; tnum++) {
			urp_ctcss_tone_detector *ptdet = NULL;
			ptdet = &(pChan->rxCtcss->tdet[tnum]);
			ptdet->decode = 0;
			ptdet->z[0] = ptdet->z[1] = ptdet->z[2] = ptdet->z[3] = 0;
		}
	}
	return 0;
}

/*
	assumes:
	sampling rate is 48KS/s
	samples are all 16 bits
	samples are filtered and decimated by 1/6th
*/
urp_radio_state *urp_radio_create(urp_radio_state *tChan, i16 numSamples)
{
#define ALLOCATE_OR_FAIL(target, count, size)                                                      \
	do {                                                                                       \
		(target) = ast_calloc((count), (size));                                            \
		if (!(target)) {                                                                   \
			goto allocation_failed;                                                    \
		}                                                                                  \
	} while (0)
	i16 i;
	urp_radio_state *pChan;
	urp_radio_stage *pSps;
	urp_ctcss_decoder *pDecCtcss;

	TRACEJ(1, "urp_radio_create(%p,%i)\n", tChan, numSamples);

	pChan = (urp_radio_state *)ast_calloc(sizeof(urp_radio_state), 1);
	if (pChan == NULL) {
		ast_log(LOG_ERROR, "urp_radio_create() failed\n");
		return NULL;
	}

	pChan->index = radioIndex++;
	pChan->nSamplesTx = pChan->nSamplesRx = numSamples;

	ALLOCATE_OR_FAIL(pDecCtcss, sizeof(*pDecCtcss), 1);
	pChan->rxCtcss = pDecCtcss;
	pChan->rxctcssfreq[0] = 0;

	if (tChan == NULL) {
		ast_log(LOG_WARNING, "urp_radio_create() WARNING: NULL tChan!\n");
		pChan->pRxCodeSrc = disabled_code;
		pChan->pTxCodeSrc = disabled_code;
		pChan->pTxCodeDefault = disabled_code;
		pChan->rxNoiseSquelchEnable = 0;
		pChan->rxHpfEnable = 0;
		pChan->rxDeEmpEnable = 0;
		pChan->rxCenterSlicerEnable = 0;
		pChan->rxCtcssDecodeEnable = 0;
		pChan->rxDcsDecodeEnable = 0;

		pChan->rxCarrierHyst = 2500;

		pChan->txMixA = TX_OUT_VOICE;
		pChan->txMixB = TX_OUT_LSD;
	} else {
		pChan->rxDemod = tChan->rxDemod;
		pChan->rxCdType = tChan->rxCdType;
		pChan->voxHangTime = tChan->voxHangTime;
		pChan->rxSquelchPoint = tChan->rxSquelchPoint;
		pChan->rxCarrierHyst = tChan->rxCarrierHyst;
		pChan->rxSqVoxAdj = tChan->rxSqVoxAdj;
		pChan->rxSquelchDelay = tChan->rxSquelchDelay;
		pChan->rxNoiseFilType = tChan->rxNoiseFilType;

		pChan->txMod = tChan->txMod;

		pChan->pTxCodeDefault = tChan->pTxCodeDefault;
		pChan->pRxCodeSrc = tChan->pRxCodeSrc;
		pChan->pTxCodeSrc = tChan->pTxCodeSrc;

		pChan->txMixA = tChan->txMixA;
		pChan->txMixB = tChan->txMixB;
		pChan->radioDuplex = tChan->radioDuplex;
		pChan->area = tChan->area;
		pChan->rptnum = tChan->rptnum;
		pChan->idleinterval = tChan->idleinterval;
		pChan->turnoffs = tChan->turnoffs;
		pChan->b.rxpolarity = tChan->b.rxpolarity;
		pChan->b.txpolarity = tChan->b.txpolarity;
		pChan->b.dcsrxpolarity = tChan->b.dcsrxpolarity;
		pChan->b.dcstxpolarity = tChan->b.dcstxpolarity;
		pChan->b.lsdrxpolarity = tChan->b.lsdrxpolarity;
		pChan->b.lsdtxpolarity = tChan->b.lsdtxpolarity;

		pChan->txsettletime = tChan->txsettletime;
		pChan->tracelevel = tChan->tracelevel;
		pChan->tracetype = tChan->tracetype;
		pChan->ukey = tChan->ukey;
		pChan->name = tChan->name;
		pChan->fever = tChan->fever;

		if (tChan->rxlpf >= 0 && (size_t)tChan->rxlpf < MAX_RXLPF) {
			pChan->rxlpf = tChan->rxlpf;
		} else {
			pChan->rxlpf = 0;
		}

		if (tChan->rxhpf >= 0 && (size_t)tChan->rxhpf < MAX_RXHPF) {
			pChan->rxhpf = tChan->rxhpf;
		} else {
			pChan->rxhpf = 0;
		}

		ast_log(LOG_NOTICE, "native detector rxlpf: %d\n", pChan->rxlpf);
		ast_log(LOG_NOTICE, "native detector rxhpf: %d\n", pChan->rxhpf);
	}

	if (pChan->rxCarrierHyst == 0) {
		pChan->rxCarrierHyst = 3000;
	}

	if (pChan->rxCdType == CD_XPMR_NOISE) {
		pChan->rxNoiseSquelchEnable = 1;
	}

	if (pChan->rxDemod == RX_AUDIO_FLAT) {
		pChan->rxDeEmpEnable = 1;
	}

	pChan->rxCarrierPoint = (pChan->rxSquelchPoint * 32767) / 100;

	pChan->rxDcsDecodeEnable = 0;

	pChan->lastrxdecode = CTCSS_NULL;

	TRACEF(1, "calloc buffers \n");

	ALLOCATE_OR_FAIL(pChan->pRxDemod, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxNoise, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxBase, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxHpf, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxLsd, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxSpeaker, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxCtcss, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxDcTrack, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxLsdLimit, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->prxMeasure, numSamples, 2);

#if URP_RADIO_DEBUG == 1
	TRACEF(1, "configure tracing\n");

	ALLOCATE_OR_FAIL(pChan->pTstTxOut, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->pRxLsdCen, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->prxDebug0, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->prxDebug1, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->prxDebug2, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->prxDebug3, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->ptxDebug0, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->ptxDebug1, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->ptxDebug2, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->ptxDebug3, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug0, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug1, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug2, numSamples, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug3, numSamples, 2);

	for (i = 0; i < CTCSS_NUM_CODES; i++) {
		ALLOCATE_OR_FAIL(pChan->rxCtcss->tdet[i].pDebug0, numSamples, 2);
		ALLOCATE_OR_FAIL(pChan->rxCtcss->tdet[i].pDebug1, numSamples, 2);
		ALLOCATE_OR_FAIL(pChan->rxCtcss->tdet[i].pDebug2, numSamples, 2);
		ALLOCATE_OR_FAIL(pChan->rxCtcss->tdet[i].pDebug3, numSamples, 2);
	}

	/* buffer, 2 bytes per sample, and 16 channels */
	ALLOCATE_OR_FAIL(pChan->ptxDebug, numSamples * 16, 2);

	/* TSCOPE CONFIGURATION SETSCOPE configure debug traces and sources for each channel of the
	 * output */
	ALLOCATE_OR_FAIL(pChan->sdbg, sizeof(*pChan->sdbg), 1);

	for (i = 0; i < URP_RADIO_DEBUG_CHANNELS; i++) {
		pChan->sdbg->trace[i] = -1;
	}

	TRACEF(1, "pChan->tracetype = %i\n", pChan->tracetype);

	if (pChan->tracetype == 1) { /* CTCSS DECODE */
		pChan->sdbg->source[0] = pChan->pRxDemod;
		pChan->sdbg->source[1] = pChan->pRxBase;
		pChan->sdbg->source[2] = pChan->pRxNoise;
		pChan->sdbg->trace[3] = RX_NOISE_TRIG;
		pChan->sdbg->source[4] = pChan->pRxLsd;
		pChan->sdbg->source[5] = pChan->pRxLsdCen;
		pChan->sdbg->source[6] = pChan->pRxLsdLimit;
		pChan->sdbg->source[7] = pChan->rxCtcss->tdet[3].pDebug0;
		pChan->sdbg->trace[8] = RX_CTCSS_DECODE;
		pChan->sdbg->trace[9] = RX_SMODE;
		pChan->sdbg->source[10] = pChan->pRxBase;
		pChan->sdbg->source[11] = pChan->pRxSpeaker;
	} else if (pChan->tracetype == 2) { /* CTCSS DECODE */
		pChan->sdbg->source[0] = pChan->pRxDemod;
		pChan->sdbg->source[1] = pChan->pRxBase;
		pChan->sdbg->trace[2] = RX_NOISE_TRIG;
		pChan->sdbg->source[3] = pChan->pRxLsd;
		pChan->sdbg->source[4] = pChan->pRxLsdCen;
		pChan->sdbg->source[5] = pChan->pRxDcTrack;
		pChan->sdbg->source[6] = pChan->pRxLsdLimit;
		pChan->sdbg->source[7] = pChan->rxCtcss->tdet[3].pDebug0;
		pChan->sdbg->source[8] = pChan->rxCtcss->tdet[3].pDebug1;
		pChan->sdbg->source[9] = pChan->rxCtcss->tdet[3].pDebug2;
		pChan->sdbg->source[10] = pChan->rxCtcss->tdet[3].pDebug3;
		pChan->sdbg->trace[11] = RX_CTCSS_DECODE;
		pChan->sdbg->trace[12] = RX_SMODE;
		pChan->sdbg->trace[13] = TX_PTT_IN;
		pChan->sdbg->trace[14] = TX_PTT_OUT;
	} else if (pChan->tracetype == 3) { /* DCS DECODE */
		pChan->sdbg->source[0] = pChan->pRxDemod;
		pChan->sdbg->source[1] = pChan->pRxBase;
		pChan->sdbg->trace[2] = RX_NOISE_TRIG;
		pChan->sdbg->source[3] = pChan->pRxLsd;
		pChan->sdbg->source[4] = pChan->pRxLsdCen;
		pChan->sdbg->source[5] = pChan->pRxDcTrack;
		pChan->sdbg->trace[6] = RX_DCS_CLK;
		pChan->sdbg->trace[7] = RX_DCS_DIN;
		pChan->sdbg->trace[8] = RX_DCS_DEC;
		pChan->sdbg->trace[9] = RX_SMODE;
		pChan->sdbg->trace[10] = TX_PTT_IN;
		pChan->sdbg->trace[11] = TX_PTT_OUT;
	} else if (pChan->tracetype == 4) { /* LSD DECODE */
		pChan->sdbg->source[0] = pChan->pRxDemod;
		pChan->sdbg->source[1] = pChan->pRxBase;
		pChan->sdbg->trace[2] = RX_NOISE_TRIG;
		pChan->sdbg->source[3] = pChan->pRxLsd;
		pChan->sdbg->source[4] = pChan->pRxLsdCen;
		pChan->sdbg->source[5] = pChan->pRxDcTrack;
		pChan->sdbg->trace[6] = RX_LSD_CLK;
		pChan->sdbg->trace[7] = RX_LSD_DAT;
		pChan->sdbg->trace[8] = RX_LSD_ERR;
		pChan->sdbg->trace[9] = RX_LSD_SYNC;
		pChan->sdbg->trace[10] = RX_SMODE;
		pChan->sdbg->trace[11] = TX_PTT_IN;
		pChan->sdbg->trace[12] = TX_PTT_OUT;
	} else if (pChan->tracetype == 5) { /* LSD LOGIC */
		pChan->sdbg->source[0] = pChan->pRxBase;
		pChan->sdbg->trace[1] = RX_NOISE_TRIG;
		pChan->sdbg->source[2] = pChan->pRxDcTrack;
		pChan->sdbg->trace[3] = RX_LSD_SYNC;
		pChan->sdbg->trace[4] = RX_SMODE;
		pChan->sdbg->trace[5] = TX_PTT_IN;
		pChan->sdbg->trace[6] = TX_PTT_OUT;
	} else if (pChan->tracetype == 6) {
		/* tx clock skew and jitter buffer */
		pChan->sdbg->source[0] = pChan->pRxDemod;
		pChan->sdbg->trace[6] = TX_DEDRIFT_LEAD;
		pChan->sdbg->trace[7] = TX_DEDRIFT_ERR;
		pChan->sdbg->trace[8] = TX_DEDRIFT_FACTOR;
		pChan->sdbg->trace[9] = TX_DEDRIFT_DRIFT;
	} else if (pChan->tracetype == 7) {
		/* tx path */
		pChan->sdbg->source[0] = pChan->pRxBase;
		pChan->sdbg->trace[1] = RX_NOISE_TRIG;
		pChan->sdbg->source[2] = pChan->pRxLsd;
		pChan->sdbg->trace[3] = RX_CTCSS_DECODE;
		pChan->sdbg->source[4] = pChan->pRxHpf;

		pChan->sdbg->trace[5] = TX_PTT_IN;
		pChan->sdbg->trace[6] = TX_PTT_OUT;
	}

	for (i = 0; i < URP_RADIO_DEBUG_CHANNELS; i++) {
		if (pChan->sdbg->trace[i] >= 0) {
			pChan->sdbg->point[pChan->sdbg->trace[i]] = i;
		}
	}
	pChan->sdbg->mode = 1;
#endif

	pChan->txCtcssFreq10 = 1000;
	pChan->txCtcssGainQ8 = M_Q8;
	pChan->txOutputGainA = M_Q8;
	pChan->txOutputGainB = M_Q8;

	/* RX Process */
	TRACEF(1, "create rx\n");
	pSps = NULL;

	/* allocate space for first sps and set pointers */
	pSps = pChan->spsRx = urp_radio_stage_create(pChan);
	if (!pSps) {
		goto allocation_failed;
	}
	pSps->source = NULL; /* set when called */
	pSps->sink = pChan->pRxBase;
	pSps->sigProc = urp_radio_receive_frontend;
	pSps->enabled = 1;
	pSps->decimator = pSps->decimate = 6;
	pSps->interpolate = 1;
	pSps->nSamples = pChan->nSamplesRx;
	pSps->ncoef = fir_rxlpf[pChan->rxlpf].taps;
	pSps->size_coef = 2;
	pSps->coef = (void *)fir_rxlpf[pChan->rxlpf].coefs;
	pSps->nx = fir_rxlpf[pChan->rxlpf].taps;
	pSps->size_x = 2;
	ALLOCATE_OR_FAIL(pSps->x, pSps->nx, pSps->size_coef);
	pSps->calcAdjust = (fir_rxlpf[pChan->rxlpf].gain * 256) / 0x0100;
	pSps->outputGain = (1.0 * M_Q8);
	pSps->discfactor = 2;
	pSps->hyst = pChan->rxCarrierHyst;
	pSps->setpt = pChan->rxCarrierPoint;
	pSps->compOut = 1;
	pChan->prxSquelchAdjust = &pSps->setpt;
#if URP_RADIO_DEBUG == 1
	pSps->debugBuff0 = pChan->pRxDemod;
	pSps->debugBuff1 = pChan->pRxNoise;
	pSps->debugBuff2 = pChan->prxDebug0;
#endif

	/* allocate space for next sps and set pointers */
	/* Rx SubAudible Decoder Low Pass Filter */
	pSps = pChan->spsRxLsd = pSps->nextSps = urp_radio_stage_create(pChan);
	if (!pSps) {
		goto allocation_failed;
	}
	pSps->source = pChan->pRxBase;
	pSps->sink = pChan->pRxLsd;
	pSps->sigProc = urp_radio_fir;
	pSps->enabled = 1;
	pSps->numChanOut = 1;
	pSps->selChanOut = 0;
	pSps->nSamples = pChan->nSamplesRx;
	pSps->decimator = pSps->decimate = 1;
	pSps->interpolate = 1;

	/* configure the the larger, lower cutoff filter by default */
	pSps->ncoef = taps_fir_lpf_215_9_88;
	pSps->size_coef = 2;
	pSps->coef = (void *)coef_fir_lpf_215_9_88;
	pSps->nx = taps_fir_lpf_215_9_88;
	pSps->size_x = 2;
	ALLOCATE_OR_FAIL(pSps->x, pSps->nx, pSps->size_x);
	pSps->calcAdjust = gain_fir_lpf_215_9_88;

	pSps->inputGain = (1 * M_Q8);
	pSps->outputGain = (1 * M_Q8);
	pChan->prxCtcssMeasure = pSps->sink;
	pChan->prxCtcssAdjust = &(pSps->outputGain);

	/* CTCSS CenterSlicer */
	pSps = pChan->spsRxLsdNrz = pSps->nextSps = urp_radio_stage_create(pChan);
	if (!pSps) {
		goto allocation_failed;
	}
	pSps->source = pChan->pRxLsd;
	pSps->sink = pChan->pRxDcTrack;
	pSps->buff = pChan->pRxLsdLimit;
	pSps->sigProc = CenterSlicer;
	pSps->nSamples = pChan->nSamplesRx;
	pSps->discfactor = LSD_DFS; /* centering time constant */
	pSps->inputGain = (1 * M_Q8);
	pSps->outputGain = (1 * M_Q8);
	pSps->setpt = 4900;	/* ptp clamp for DC centering */
	pSps->inputGainB = 625; /* peak output limiter clip point */
	pSps->enabled = 0;

	/* Rx HPF */
	pSps = pSps->nextSps = urp_radio_stage_create(pChan);
	if (!pSps) {
		goto allocation_failed;
	}
	pChan->spsRxHpf = pSps;
	pSps->source = pChan->pRxBase;
	pSps->sink = pChan->pRxHpf;
	pSps->sigProc = urp_radio_fir;
	pSps->enabled = 1;
	pSps->numChanOut = 1;
	pSps->selChanOut = 0;
	pSps->nSamples = pChan->nSamplesRx;
	pSps->decimator = pSps->decimate = 1;
	pSps->interpolate = 1;
	pSps->ncoef = fir_rxhpf[pChan->rxhpf].taps;
	pSps->size_coef = 2;
	pSps->coef = (void *)fir_rxhpf[pChan->rxhpf].coefs;
	pSps->nx = fir_rxhpf[pChan->rxhpf].taps;
	pSps->size_x = 2;
	ALLOCATE_OR_FAIL(pSps->x, pSps->nx, pSps->size_x);
	pSps->calcAdjust = fir_rxhpf[pChan->rxhpf].gain;
	pSps->inputGain = (1 * M_Q8);
	pSps->outputGain = (1 * M_Q8);
	pChan->prxVoiceAdjust = &(pSps->outputGain);
	pChan->spsRxOut = pSps;

	/* allocate space for next sps and set pointers */
	/* Rx DeEmp */
	if (pChan->rxDeEmpEnable) {
		pSps = pSps->nextSps = urp_radio_stage_create(pChan);
		if (!pSps) {
			goto allocation_failed;
		}
		pChan->spsRxDeEmp = pSps;
		pSps->source = pChan->pRxHpf;
		pSps->sink = pChan->pRxSpeaker;
		pChan->spsRxOut = pSps; /* OUTPUT STRUCTURE! */
		pSps->sigProc = gp_inte_00;
		pSps->enabled = 1;
		pSps->nSamples = pChan->nSamplesRx;

		pSps->ncoef = taps_int_lpf_300_1_2;
		pSps->size_coef = 2;
		pSps->coef = (void *)coef_int_lpf_300_1_2;

		pSps->nx = taps_int_lpf_300_1_2;
		pSps->size_x = 4;
		ALLOCATE_OR_FAIL(pSps->x, pSps->nx, pSps->size_x);
		pSps->calcAdjust = gain_int_lpf_300_1_2 / 2;
		pSps->inputGain = (1.0 * M_Q8);
		pSps->outputGain = (1.0 * M_Q8);
		pChan->prxVoiceMeasure = pSps->sink;
		pChan->prxVoiceAdjust = &(pSps->outputGain);
	} else {
		/* force delay to be true */
		if (pChan->rxSquelchDelay == 0) {
			pChan->rxSquelchDelay = 30;
		}
	}

	if (pChan->rxSquelchDelay > RXSQDELAYBUFSIZE / 8 - 1) {
		pChan->rxSquelchDelay = RXSQDELAYBUFSIZE / 8 - 1;
	}
	if (pChan->rxSquelchDelay > 0) {
		TRACEF(1, "create rx squelch delay\n");
		pSps = pChan->spsDelayLine = pSps->nextSps = urp_radio_stage_create(pChan);
		if (!pSps) {
			goto allocation_failed;
		}
		pChan->spsRxSquelchDelay = pSps;
		pSps->sigProc = DelayLine;
		if (pChan->rxDeEmpEnable) {
			pSps->source = pChan->pRxSpeaker;
		} else {
			pSps->source = pChan->pRxHpf;
		}

		pSps->sink = pChan->pRxSpeaker;
		pChan->spsRxOut = pSps; /* OUTPUT STRUCTURE! */
		pSps->enabled = 1;
		pSps->b.outzero = 0;
		pSps->inputGain = 1 * M_Q8;
		pSps->outputGain = 1 * M_Q8;
		pSps->nSamples = pChan->nSamplesRx;
		pSps->buffSize = RXSQDELAYBUFSIZE;
		ALLOCATE_OR_FAIL(pSps->buff, RXSQDELAYBUFSIZE, 2);
		pSps->buffLead = pChan->rxSquelchDelay * 8; /* convert ms to samples */
		pSps->buffInIndex = 0;
		pSps->buffOutIndex = 0;
	}

	if (pChan->rxCdType == CD_XPMR_VOX) {
		TRACEF(1, "create vox measureblock\n");
		ALLOCATE_OR_FAIL(pChan->prxVoxMeas, pChan->nSamplesRx, 2);

		pSps = pChan->spsRxVox = pSps->nextSps = urp_radio_stage_create(pChan);
		if (!pSps) {
			goto allocation_failed;
		}
		pSps->sigProc = MeasureBlock;
		pSps->parentChan = pChan;
		pSps->source = pChan->pRxBase;
		pSps->sink = pChan->prxVoxMeas;
		pSps->inputGain = 1 * M_Q8;
		pSps->outputGain = 1 * M_Q8;
		pSps->nSamples = pChan->nSamplesRx;
		pSps->discfactor = 3;
		if (pChan->rxSqVoxAdj == 0) {
			pSps->setpt = (0.011 * M_Q15);
		} else {
			pSps->setpt = (pChan->rxSqVoxAdj);
		}

		pSps->hyst = (pSps->setpt / 10);
		pSps->enabled = 1;
	}

	/* tuning measure block */
	pSps = pChan->spsMeasure = pSps->nextSps = urp_radio_stage_create(pChan);
	if (!pSps) {
		goto allocation_failed;
	}
	pSps->source = pChan->spsRx->sink;
	pSps->sink = pChan->prxMeasure;
	pSps->sigProc = MeasureBlock;
	pSps->enabled = 0;
	pSps->nSamples = pChan->nSamplesRx;
	pSps->discfactor = 10;

	pSps->nextSps = NULL; /* last sps in chain RX */

	/* USBRadioPlus owns waveform generation and CM119 output scaling. */
	pChan->ptxCtcssAdjust = &pChan->txCtcssGainQ8;

	/* Configure Coded Signaling */
	if (urp_radio_parse_codes(pChan)) {
		goto allocation_failed;
	}

	pChan->smode = SMODE_NULL;
	pChan->smodewas = SMODE_NULL;
	pChan->smodetime = 2500;
	pChan->smodetimer = 0;
	pChan->b.smodeturnoff = 0;

	pChan->txsettletimer = 0;
	pChan->txrxblankingtimer = 0;

	TRACEF(1, "urp_radio_create() end\n");

#undef ALLOCATE_OR_FAIL
	return pChan;

allocation_failed:
	ast_log(LOG_ERROR, "urp_radio_create(): memory allocation failed\n");
	urp_radio_destroy(pChan);
#undef ALLOCATE_OR_FAIL
	return NULL;
}

/*
 */
i16 urp_radio_destroy(urp_radio_state *pChan)
{
	urp_radio_stage *pmr_sps, *tmp_sps;
	i16 i;

	if (!pChan) {
		return 1;
	}
	TRACEF(1, "urp_radio_destroy()\n");

	ast_free(pChan->pRxDemod);
	ast_free(pChan->pRxNoise);
	ast_free(pChan->pRxBase);
	ast_free(pChan->pRxHpf);
	ast_free(pChan->pRxLsd);
	ast_free(pChan->pRxSpeaker);
	ast_free(pChan->pRxCtcss);
	ast_free(pChan->pRxDcTrack);
	if (pChan->pRxLsdLimit) {
		ast_free(pChan->pRxLsdLimit);
	}

	if (pChan->prxMeasure) {
		ast_free(pChan->prxMeasure);
	}
	ast_free(pChan->prxVoxMeas);
	ast_free(pChan->pRxCode);
	ast_free(pChan->pRxCodeStr);
	ast_free(pChan->pTxCode);
	ast_free(pChan->pTxCodeStr);

#if URP_RADIO_DEBUG == 1
	if (pChan->ptxDebug) {
		ast_free(pChan->ptxDebug);
	}

	ast_free(pChan->prxDebug0);
	ast_free(pChan->prxDebug1);
	ast_free(pChan->prxDebug2);
	ast_free(pChan->prxDebug3);

	ast_free(pChan->ptxDebug0);
	ast_free(pChan->ptxDebug1);
	ast_free(pChan->ptxDebug2);
	ast_free(pChan->ptxDebug3);
	ast_free(pChan->pTstTxOut);
	ast_free(pChan->pRxLsdCen);

	if (pChan->rxCtcss) {
		ast_free(pChan->rxCtcss->pDebug0);
		ast_free(pChan->rxCtcss->pDebug1);
		ast_free(pChan->rxCtcss->pDebug2);
		ast_free(pChan->rxCtcss->pDebug3);

		for (i = 0; i < CTCSS_NUM_CODES; i++) {
			ast_free(pChan->rxCtcss->tdet[i].pDebug0);
			ast_free(pChan->rxCtcss->tdet[i].pDebug1);
			ast_free(pChan->rxCtcss->tdet[i].pDebug2);
			ast_free(pChan->rxCtcss->tdet[i].pDebug3);
		}
	}
#endif

	ast_free(pChan->rxCtcss);

	pmr_sps = pChan->spsRx;
	if (pChan->spsDelayLine) {
		ast_free(pChan->spsDelayLine->buff);
		pChan->spsDelayLine->buff = NULL;
	}

	if (pChan->sdbg) {
		ast_free(pChan->sdbg);
	}

	while (pmr_sps) {
		tmp_sps = pmr_sps;
		pmr_sps = tmp_sps->nextSps;
		urp_radio_stage_destroy(tmp_sps);
	}

	ast_free(pChan);

	return 0;
}

/*
 */
urp_radio_stage *urp_radio_stage_create(urp_radio_state *pChan)
{
	urp_radio_stage *pSps;

	TRACEF(1, "urp_radio_stage_create()\n");

	pSps = (urp_radio_stage *)ast_calloc(sizeof(urp_radio_stage), 1);

	if (!pSps) {
		ast_log(LOG_ERROR, "Error: urp_radio_stage_create()\n");
		return NULL;
	}

	pSps->parentChan = pChan;
	pSps->index = pChan->spsIndex++;

	return pSps;
}

/*
 */
i16 urp_radio_stage_destroy(urp_radio_stage *pSps)
{
	TRACEJ(1, "urp_radio_stage_destroy(%i)\n", pSps->index);

	if (pSps->x != NULL) {
		ast_free(pSps->x);
	}

	ast_free(pSps);
	return 0;
}

/*
	urp_radio_process handles a block of data from the usb audio device
*/
i16 urp_radio_process(urp_radio_state *pChan, i16 *input, i16 *outputrx, i16 *outputtx)
{
	int i, hit;
	float f = 0;
	urp_radio_stage *pmr_sps;

	TRACEC(5, "urp_radio_process(%p %p %p %p)\n", pChan, input, outputrx, outputtx);

	if (pChan == NULL) {
		ast_log(LOG_ERROR, "urp_radio_process() pChan == NULL\n");
		return 1;
	}

	pChan->frameCountRx++;

#if URP_RADIO_DEBUG == 1
	if (pChan->b.rxCapture) {
		memset((void *)pChan->ptxDebug, 0,
		       pChan->nSamplesRx * URP_RADIO_DEBUG_CHANNELS * 2);

		memset((void *)pChan->sdbg->buffer, 0,
		       pChan->nSamplesRx * URP_RADIO_DEBUG_CHANNELS * 2);
	}
#endif

#ifndef URP_RADIO_VOTER
	pmr_sps = pChan->spsRx; /* first sps */
	pmr_sps->source = input;

	if (outputrx != NULL) {
		pChan->spsRxOut->sink = outputrx; /* last sps */
	}

	if (pChan->txrxblankingtimer > 0) {
		for (i = 0; i < pChan->nSamplesRx * 6; i++) {
			input[i] = 0;
		}

		pChan->txrxblankingtimer -= MS_PER_FRAME;
		if (pChan->txrxblankingtimer <= 0) {
			pChan->txrxblankingtimer = 0;
			TRACEC(1, "TXRXBLANKING TIME OUT **********\n");
		}
	}

	if (pChan->rxCpuSaver && !pChan->rxCarrierDetect && pChan->smode == SMODE_NULL &&
	    !pChan->txPttIn && !pChan->txPttOut) {
		if (!pChan->b.rxhalted) {
			pChan->spsRxHpf->enabled = 0;
			if (pChan->rxDeEmpEnable) {
				pChan->spsRxDeEmp->enabled = 0;
			}

			pChan->b.rxhalted = 1;
			TRACEC(1, "urp_radio_process() rx sps halted\n");
		}
	} else if (pChan->b.rxhalted) {
		pChan->spsRxHpf->enabled = 1;
		if (pChan->rxDeEmpEnable) {
			pChan->spsRxDeEmp->enabled = 1;
		}

		pChan->b.rxhalted = 0;
		TRACEC(1, "urp_radio_process() rx sps un-halted\n");
	}

	i = 0;
	while (pmr_sps != NULL) {
		TRACEC(5, "urp_radio_process() sps %i\n", i++);
		pmr_sps->sigProc(pmr_sps);
		pmr_sps = (urp_radio_stage *)(pmr_sps->nextSps);
	}

	if (pChan->rxCdType == CD_XPMR_VOX) {
		if (pChan->spsRxVox->compOut) {
			pChan->rxVoxTimer = pChan->voxHangTime; /* VOX HangTime in ms */
		}
		if (pChan->rxVoxTimer > 0) {
			pChan->rxVoxTimer -= MS_PER_FRAME;
			pChan->rxCarrierDetect = 1;
		} else {
			pChan->rxVoxTimer = 0;
			pChan->rxCarrierDetect = 0;
		}
	} else {
		pChan->rxCarrierDetect = !pChan->spsRx->compOut;
		if (pChan->rxSquelchDelay) {
			pChan->spsRxSquelchDelay->b.outzero = pChan->spsRx->compOut;
		}
	}

	/* stop and start these engines instead to eliminate falsing */
	if (pChan->b.ctcssRxEnable &&
	    (!pChan->b.rxhalted || pChan->rxCtcss->decode != CTCSS_NULL)) {
		urp_ctcss_decode(pChan);
	}

	if (pChan->txPttIn != pChan->b.pttwas) {
		pChan->b.pttwas = pChan->txPttIn;
		TRACEC(1, "urp_radio_process() txPttIn=%i\n", pChan->b.pttwas);
	}

	if (pChan->smodetimer > 0 && !pChan->txPttIn) {
		pChan->smodetimer -= MS_PER_FRAME;

		if (pChan->smodetimer <= 0) {
			pChan->smodetimer = 0;
			pChan->smodewas = pChan->smode;
			pChan->smode = SMODE_NULL;
			pChan->b.smodeturnoff = 1;
			TRACEC(1, "smode timeout. smode was=%i\n", pChan->smodewas);
		}
	}

	if (pChan->rxCtcss->decode > CTCSS_NULL &&
	    (pChan->smode == SMODE_NULL || pChan->smode == SMODE_CTCSS)) {
		if (pChan->smode != SMODE_CTCSS) {
			TRACEC(1, "smode set=%i  code=%i\n", pChan->smode, pChan->rxCtcss->decode);
			pChan->smode = pChan->smodewas = SMODE_CTCSS;
		}
		pChan->smodetimer = pChan->smodetime;
	}
	if (pChan->smode == SMODE_CTCSS) {
		if (pChan->rxCtcss->decode != pChan->lastrxdecode) {
			pChan->lastrxdecode = pChan->rxCtcss->decode;
			f = 0;
			if (pChan->rxCtcss->decode > CTCSS_NULL) {
				if (pChan->rxCtcssMap[pChan->rxCtcss->decode] != CTCSS_RXONLY) {
					f = freq_ctcss[pChan->rxCtcssMap[pChan->rxCtcss->decode]];
				}
			} else {
				f = pChan->txctcssdefault_value;
			}
			if (f && pChan->txCtcssFreq10 != f * 10) {
				pChan->txCtcssFreq10 = f * 10;
				pChan->txCtcssOption = 1;
			}
		}
	} else {
		pChan->lastrxdecode = CTCSS_NULL;
	}
#endif
	/* handle radio transmitter ptt input */
	hit = 0;
	{
		if (pChan->txPttIn && (pChan->txState == CHAN_TXSTATE_IDLE)) {
			TRACEC(1,
			       "txPttIn==1 from CHAN_TXSTATE_IDLE && !SMODE_LSD. codeindex=%i  %i "
			       "\n",
			       pChan->rxCtcss->decode, pChan->rxCtcssMap[pChan->rxCtcss->decode]);
			pChan->txCtcssFreq10 = 0;
			if (pChan->smode == SMODE_CTCSS && !pChan->b.txCtcssInhibit) {
				if (pChan->rxCtcss->decode > CTCSS_NULL) {
					if (pChan->rxCtcssMap[pChan->rxCtcss->decode] !=
					    CTCSS_RXONLY) {
						f = freq_ctcss
							[pChan->rxCtcssMap[pChan->rxCtcss->decode]];
					}
				} else {
					f = pChan->txctcssdefault_value;
				}
				TRACEC(1, "txPttIn - Start CTCSSGen  %f \n", f);
				if (f) {
					pChan->txCtcssFreq10 = f * 10;
					pChan->txCtcssOption = 1;
					pChan->txCtcssEnabled = 1;
					pChan->txCtcssTurnoffTimer = 0;
				}
			} else if (pChan->smode == SMODE_NULL &&
				   pChan->txcodedefaultsmode == SMODE_CTCSS &&
				   !pChan->b.txCtcssInhibit) {
				TRACEC(1, "txPtt Encode txcodedefaultsmode==SMODE_CTCSS %f\n",
				       pChan->txctcssdefault_value);
				f = pChan->txctcssdefault_value;
				pChan->txCtcssFreq10 = f * 10;
				pChan->txCtcssOption = 1;
				pChan->txCtcssEnabled = 1;
				pChan->txCtcssTurnoffTimer = 0;
				pChan->smode = SMODE_CTCSS;
				pChan->smodetimer = pChan->smodetime;
			}

			memset(pChan->txctcssfreq, 0, sizeof(pChan->txctcssfreq));
			sprintf(pChan->txctcssfreq, "%.1f", f);
			pChan->b.txCtcssReady = 1;

			pChan->txState = CHAN_TXSTATE_ACTIVE;
			pChan->txPttOut = 1;

			pChan->txsettletimer = pChan->txsettletime;

			TRACEC(1, "urp_radio_process() TxOn\n");
		} else if (pChan->txPttIn && pChan->txState == CHAN_TXSTATE_ACTIVE) {
			pChan->smodetimer = pChan->smodetime;
		} else if (!pChan->txPttIn && pChan->txState == CHAN_TXSTATE_ACTIVE) {
			TRACEC(1, "txPttIn==0 from CHAN_TXSTATE_ACTIVE\n");
			if (pChan->smode == SMODE_CTCSS && !pChan->b.txCtcssInhibit) {
				if (pChan->txTocType == TOC_NONE || !pChan->b.ctcssTxEnable) {
					TRACEC(1, "Tx Off Immediate.\n");
					pChan->txCtcssOption = 3;
					pChan->txBufferClear = 3;
					pChan->txState = CHAN_TXSTATE_FINISHING;
				} else if (pChan->txTocType == TOC_NOTONE) {
					pChan->txState = CHAN_TXSTATE_TOC;
					pChan->txHangTime = TOC_NOTONE_TIME / MS_PER_FRAME;
					pChan->txCtcssOption = 3;
					TRACEC(1, "Tx Turn Off No Tone Start.\n");
				} else {
					pChan->txState = CHAN_TXSTATE_TOC;
					pChan->txHangTime = 0;
					pChan->txCtcssOption = 2;
					TRACEC(1, "Tx Turn Off Phase Shift Start.\n");
				}
			} else {
				pChan->txBufferClear = 3;
				pChan->txState = CHAN_TXSTATE_FINISHING;
				TRACEC(1, "Tx Off No SMODE to Finish.\n");
			}
		} else if (pChan->txState == CHAN_TXSTATE_TOC) {
			if (pChan->txPttIn && pChan->smode == SMODE_CTCSS) {
				TRACEC(1, "Tx Key During HangTime\n");
				pChan->txState = CHAN_TXSTATE_ACTIVE;
				pChan->txCtcssOption = 1;
				pChan->txCtcssEnabled = 1;
				pChan->txCtcssTurnoffTimer = 0;
				hit = 0;
			} else if (pChan->txHangTime) {
				if (--pChan->txHangTime == 0) {
					pChan->txState = CHAN_TXSTATE_FINISHING;
				}
			} else if (pChan->txCtcssState == 0) {
				pChan->txBufferClear = 3;
				pChan->txState = CHAN_TXSTATE_FINISHING;
				TRACEC(1, "Tx Off TOC.\n");
			}
		} else if (pChan->txState == CHAN_TXSTATE_FINISHING) {
			if (--pChan->txBufferClear <= 0) {
				pChan->txState = CHAN_TXSTATE_COMPLETE;
			}
		} else if (pChan->txState == CHAN_TXSTATE_COMPLETE) {
			hit = 1;
		}
	} /* end of if SMODE==LSD */

	if (hit) {
		pChan->txPttOut = 0;
		pChan->txCtcssOption = 3;
		pChan->txrxblankingtimer = pChan->txrxblankingtime;
		TRACEC(1, "urp_radio_process() txrxblankingtimer=%i\n", pChan->txrxblankingtimer);
		pChan->txState = CHAN_TXSTATE_IDLE;

		memset(pChan->txctcssfreq, 0, sizeof(pChan->txctcssfreq));
		pChan->b.txCtcssReady = 1;
		TRACEC(1, "Tx Off hit.\n");
	}

	if (pChan->txsettletimer && pChan->txPttHid) {
		pChan->txsettletimer -= MS_PER_FRAME;
		if (pChan->txsettletimer < 0) {
			pChan->txsettletimer = 0;
		}
	}

	/* enable this after we know everything else is working */
	if (pChan->txCpuSaver && !pChan->txPttIn && !pChan->txPttOut &&
	    pChan->txState == CHAN_TXSTATE_IDLE) {
		if (!pChan->b.txhalted) {
			pChan->b.txhalted = 1;
			TRACEC(1, "urp_radio_process() tx sps halted\n");
		}
	} else if (pChan->b.txhalted) {
		pChan->b.txhalted = 0;
		TRACEC(1, "urp_radio_process() tx sps un-halted\n");
	}

	if (pChan->b.txhalted) {
		return 1;
	}

	/* Preserve established CTCSS start and squelch-tail timing while the channel
	 * driver renders the waveform at the CM119's native sample rate. */
	pChan->txCtcssPhaseShift = 0;
	if (pChan->txCtcssOption == 1) {
		pChan->txCtcssOption = 0;
		pChan->txCtcssState = 1;
	} else if (pChan->txCtcssOption == 2) {
		pChan->txCtcssOption = 0;
		pChan->txCtcssState = 2;
		pChan->txCtcssTurnoffTimer = CTCSS_TURN_OFF_TIME - (2 * MS_PER_FRAME);
		pChan->txCtcssPhaseShift = 1;
	} else if (pChan->txCtcssOption == 3) {
		pChan->txCtcssOption = 0;
		pChan->txCtcssState = 0;
		pChan->txCtcssEnabled = 0;
	} else if (pChan->txCtcssState == 2) {
		pChan->txCtcssTurnoffTimer -= MS_PER_FRAME;
		if (pChan->txCtcssTurnoffTimer <= 0)
			pChan->txCtcssOption = 3;
	}

	/* This engine controls signaling and PTT only; USBRadioPlus renders audio. */
	if (outputtx)
		memset(outputtx, 0, pChan->nSamplesTx * 2 * 6 * sizeof(*outputtx));

#if URP_RADIO_DEBUG == 1
	if (pChan->b.rxCapture) {
		for (i = 0; i < pChan->nSamplesRx; i++) {
			pChan->pRxDemod[i] = input[i * 2 * 6];
			pChan->pTstTxOut[i] = outputtx[i * 2 * 6 + 0]; /* txa */
			TSCOPE((RX_NOISE_TRIG, pChan->sdbg, i,
				(pChan->rxCarrierDetect * URP_RADIO_TRACE_AMP) -
					URP_RADIO_TRACE_AMP / 2));
			TSCOPE((RX_CTCSS_DECODE, pChan->sdbg, i,
				pChan->rxCtcss->decode * (M_Q14 / CTCSS_NUM_CODES)));
			TSCOPE((RX_SMODE, pChan->sdbg, i,
				pChan->smode * (URP_RADIO_TRACE_AMP / 4)));
			TSCOPE((TX_PTT_IN, pChan->sdbg, i,
				(pChan->txPttIn * URP_RADIO_TRACE_AMP) - URP_RADIO_TRACE_AMP / 2));
			TSCOPE((TX_PTT_OUT, pChan->sdbg, i,
				(pChan->txPttOut * URP_RADIO_TRACE_AMP) - URP_RADIO_TRACE_AMP / 2));
		}
	}
#endif

	strace2(pChan->sdbg);
	TRACEC(5, "urp_radio_process() return  cd=%i smode=%i  txPttIn=%i  txPttOut=%i \n",
	       pChan->rxCarrierDetect, pChan->smode, pChan->txPttIn, pChan->txPttOut);
	return 0;
}

#if GCC_VERSION > 40600
#pragma GCC diagnostic pop
#endif

/* end of file */
