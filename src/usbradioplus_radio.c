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
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>

#include "usbradioplus_radio.h"
#include "usbradioplus_radio_core_adapter.h"
#include "usbradioplus_radio_coefficients.h"
#include "asterisk/utils.h"
#include "asterisk/logger.h"

/** Next signaling-engine instance index. */
static i16 radioIndex = 0; /* Count live detector instances. */
/** Empty signaling-code string used for disabled code lists. */
static char disabled_code[] = "0";

/** @brief Try the optional portable ordinary DSP-squelch receive frontend.
 * @return Zero only when the portable result has been fully committed.
 */
static int urp_radio_receive_frontend_portable(urp_radio_stage *stage);

/** @brief Test whether Asterisk's current debug level enables a radio trace.
 * @param level Message trace verbosity.
 * @return Nonzero if the requested debug level is enabled.
 */
static int urp_radio_debug_atleast(int level)
{
	if (option_debug >= level)
		return 1;
	if (!ast_opt_dbg_module)
		return 0;
	return (int)ast_debug_get_by_module(AST_MODULE) >= level ||
	       (int)ast_debug_get_by_module(__FILE__) >= level;
}

/* @brief Emit a trace only when both channel and Asterisk debug levels permit it.
 * @param configured_level Channel's configured trace verbosity.
 * @param level Minimum verbosity required by this message.
 * @param format Printf-compatible message format.
 * @param ... Values substituted into the message format.
 */
void urp_radio_trace_log(int configured_level, int level, const char *format, ...)
{
	va_list arguments;

	if (configured_level < level || !urp_radio_debug_atleast(level)) {
		return;
	}
	va_start(arguments, format);
	ast_log_ap(__LOG_DEBUG, __FILE__, __LINE__, __func__, format, arguments);
	va_end(arguments);
}

/*
	Trace Routines
*/
void strace(i16 point, t_sdbg *sdbg, i16 index, i16 value)
{
	/* make dbg_trace buffer in structure */
	if (!sdbg || !sdbg->mode || sdbg->point[point] < 0) {
		return;
	} else {
		sdbg->buffer[(index * URP_RADIO_DEBUG_CHANNELS) + sdbg->point[point]] = value;
	}
}

/*

*/
void strace2(t_sdbg *sdbg, i16 samples)
{
	int i;
	i16 count;
	if (!sdbg) {
		return;
	}
	count = samples;
	if (count < 0)
		count = 0;
	if (count > SAMPLES_PER_BLOCK)
		count = SAMPLES_PER_BLOCK;
	for (i = 0; i < URP_RADIO_DEBUG_CHANNELS; i++) {
		if (sdbg->source[i]) {
			int ii;
			for (ii = 0; ii < count; ii++) {
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
i16 string_parse(const char *src, char **dest, char ***ptrs)
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
		ast_free((void *)*ptrs);
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
	TRACEF(1, "pChan->pTxCodeDefault %s \n",
	       pChan->pTxCodeDefault ? pChan->pTxCodeDefault : "(none)");

	maxctcssindex = CTCSS_NULL;
	maxctcsstxfreq = CTCSS_NULL;
	pChan->txctcssdefault_index = CTCSS_NULL;
	pChan->txctcssdefault_value = CTCSS_NULL;

	pChan->b.ctcssRxEnable = pChan->b.ctcssTxEnable = 0;
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

	pChan->rxCtcss->enabled = 0;
	pChan->rxCtcss->input = pChan->pRxLsdLimit;
	pChan->rxCtcss->decode = CTCSS_NULL;

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
		/* A disabled companion list is how the clean-slate layer keeps the
		 * receive and transmit directions independent.  It is not a malformed
		 * CTCSS frequency and must not disable a transmit-default encoder. */
		if (!strcmp(p, "0"))
			continue;

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
				/* A CTCSS receive direction is valid without a matching
				 * transmitter map.  Mark missing TX entries as RX-only so
				 * receive qualification remains independent of transmit mode. */
				frequency = 0.0;
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

	/* DEFAULT TX CODE */
	TRACEF(1, "urp_radio_parse_codes() Default Tx Code %s \n",
	       pChan->pTxCodeDefault ? pChan->pTxCodeDefault : "(none)");
	pChan->txcodedefaultsmode = SMODE_NULL;
	p = pChan->pStr = pChan->pTxCodeDefault;

	if (p && *p && strcmp(p, "0")) {
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
	TRACEF(2, "                  dcsEnabledReceive = %i \n", pChan->dcs.enabled_receive);
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
	const size_t calibration_window =
		(size_t)SAMPLES_PER_BLOCK * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK);
	const int explicit_native_count = mySps && mySps->nativeSamples != 0U;
	const size_t native_capacity =
		mySps && mySps->parentChan && mySps->parentChan->nSamplesRx > 0 &&
				mySps->decimate > 0
			? (size_t)mySps->parentChan->nSamplesRx * (size_t)mySps->decimate
			: 0U;

	i16 nx, *output, *noutput;
	const i16 *input;
	i16 *x;
	i16 decimator, decimate, doNoise, fever, fev1;
	i32 naccum, outputGain, calcAdjust;
	i64 y;
	size_t i, iOutput, samples;

	TRACEJ(5, "urp_radio_receive_frontend()\n");

	if (!mySps || !mySps->enabled || !mySps->parentChan || !mySps->source || !mySps->sink ||
	    !mySps->x || mySps->decimate <= 0 || mySps->nSamples < 0) {
		return 1;
	}
	if (!urp_radio_receive_frontend_portable(mySps))
		return 0;

	decimator = mySps->decimator;
	decimate = mySps->decimate;

	input = mySps->source;
	output = mySps->sink;
	noutput = mySps->parentChan->pRxNoise;
	fever = mySps->parentChan->fever;

	nx = mySps->nx;

	calcAdjust = mySps->calcAdjust;
	outputGain = mySps->outputGain;

	/* A native tick supplies its exact input span.  Direct compatibility
	 * callers retain the historic base-count times decimation contract. */
	samples = explicit_native_count ? (size_t)mySps->nativeSamples
					: (size_t)mySps->nSamples * (size_t)decimate;
	/* Reserve the conservative carried-phase bound, not merely the ordinary
	 * whole-frame quotient. A maximum native callback is normally 960 samples,
	 * while its caller-owned base workspace is deliberately sized for 161. */
	if (samples > native_capacity || samples > SIZE_MAX - (size_t)(2 * decimate - 2) ||
	    (samples + (size_t)(2 * decimate - 2)) / (size_t)decimate >
		    mySps->parentChan->rxBaseCapacity) {
		return 1;
	}
	x = mySps->x;
	iOutput = 0;

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

	for (i = 0U; i < samples; ++i) {
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
			/* Keep calibration on its historic fixed 960-native-sample
			 * window.  Small callbacks therefore cannot lower the displayed
			 * RSSI or perturb a saved squelch calibration. */
			mySps->rssiPower += (i64)naccum * naccum;
			++mySps->rssiSamples;
			if (mySps->rssiSamples == calibration_window) {
				mySps->parentChan->rxRssi = mySps->apeak =
					(i16)(sqrt((double)mySps->rssiPower) / 16.0);
				mySps->rssiPower = 0;
				mySps->rssiSamples = 0U;
			}
			/* The calibration meter remains sqrt(sum(960 samples))/16. Its
			 * equivalent sample-power scale is 960/256, independent of where
			 * the USB block boundary falls relative to carrier loss. */
			mySps->compOut = urp_micor_squelch_update(
				&mySps->micor_squelch, mySps->compOut,
				(double)naccum * naccum * 3.75, (uint32_t)mySps->setpt,
				(uint32_t)mySps->hyst);
			mySps->parentChan->rxCarrierGate[i] = !mySps->compOut;
		}

		--decimator;

		if (decimator <= 0) {
			decimator = decimate;

			y = 0;
			for (n = 0; n < nx; n++) {
				y += fir_rxlpf[mySps->parentChan->rxlpf].coefs[n] * x[n];
			}

			y = ((y / calcAdjust) * outputGain) / M_Q8;

			if (y > 32767) {
				y = 32767;
			} else if (y < -32767) {
				y = -32767;
			}
			output[iOutput++] = (i16)y; /* Rx Baseband decimated */

		} /* if decimator */
	}

#if URP_RADIO_DEBUG == 1
	if (doNoise && mySps->parentChan->tracetype) {
		for (i = 0U; i < iOutput; ++i) {
			noutput[i] = mySps->parentChan->rxRssi;
		}
	}
#endif

	mySps->decimator = decimator;
	mySps->nSamples = (i16)iOutput;
	mySps->nativeSamples = 0U;

	return 0;
}
/*
	pmr general purpose fir
	works on a block of samples
*/
/** @brief Test two signed-16 spans for any overlapping legacy storage.
 * @param left First span's starting address.
 * @param left_count Number of samples in the first span.
 * @param right Second span's starting address.
 * @param right_count Number of samples in the second span.
 * @return Nonzero on overlap or an unrepresentable address range.
 */
static int urp_radio_fir_s16_spans_overlap(const i16 *left, size_t left_count, const i16 *right,
					   size_t right_count)
{
	uintptr_t left_begin;
	uintptr_t left_end;
	uintptr_t right_begin;
	uintptr_t right_end;

	if (!left || !right || !left_count || !right_count)
		return 0;
	if (left_count > UINTPTR_MAX / sizeof(*left) || right_count > UINTPTR_MAX / sizeof(*right))
		return 1;
	left_begin = (uintptr_t)left;
	right_begin = (uintptr_t)right;
	left_end = left_begin + left_count * sizeof(*left);
	right_end = right_begin + right_count * sizeof(*right);
	if (left_end < left_begin || right_end < right_begin)
		return 1;
	return left_begin < right_end && right_begin < left_end;
}

/** @brief Run the ordinary native DSP-squelch frontend through the portable core.
 * @param stage Receive frontend and its preallocated conversion workspace.
 * @return Zero only after the complete portable result has replaced C state.
 *
 * VOX, trace, fever history, malformed stage, and storage-aliasing shapes
 * remain in the retained C frontend.  That narrow boundary preserves every
 * legacy diagnostic and uncommon mode while moving the normal live
 * discriminator-noise path to the F32 portable core without callback
 * allocation, locking, or device interaction.
 */
static int urp_radio_receive_frontend_portable(urp_radio_stage *stage)
{
	const size_t calibration_window =
		(size_t)SAMPLES_PER_BLOCK * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK);
	const size_t native_capacity =
		stage && stage->parentChan && stage->parentChan->nSamplesRx > 0 &&
				stage->decimate > 0
			? (size_t)stage->parentChan->nSamplesRx * (size_t)stage->decimate
			: 0U;
	urp_radio_state *channel;
	const T_FIR *baseband;
	const i16 *noise_coefficients;
	size_t noise_coefficient_count;
	int32_t noise_divisor;
	size_t sample_count;
	size_t output_count;
	int rssi_updated;
	struct rptadv_radio_receive_frontend_state state;
	struct urp_radio_receive_frontend_workspace workspace;

	if (!stage || !stage->parentChan || !stage->source || !stage->sink || !stage->x ||
	    !stage->enabled || stage->nSamples < 0 || !stage->nativeSamples ||
	    stage->nativeSamples > native_capacity || stage->decimate <= 0 || stage->nx <= 0 ||
	    stage->calcAdjust == 0 || stage->parentChan->rxCdType == CD_XPMR_VOX ||
	    stage->parentChan->fever || stage->parentChan->tracetype)
		return -1;
	channel = stage->parentChan;
	if (channel->rxlpf < 0 ||
	    (size_t)channel->rxlpf >= sizeof(fir_rxlpf) / sizeof(fir_rxlpf[0]))
		return -1;
	baseband = &fir_rxlpf[channel->rxlpf];
	if (stage->nx != baseband->taps ||
	    urp_radio_fir_s16_spans_overlap(stage->source, (size_t)stage->nativeSamples * 2U,
					    stage->sink, channel->rxBaseCapacity) ||
	    urp_radio_fir_s16_spans_overlap(stage->source, (size_t)stage->nativeSamples * 2U,
					    (const i16 *)stage->x, (size_t)stage->nx) ||
	    urp_radio_fir_s16_spans_overlap(stage->sink, channel->rxBaseCapacity,
					    (const i16 *)stage->x, (size_t)stage->nx))
		return -1;
	if (channel->rxNoiseFilType == 0) {
		noise_coefficients = coef_fir_bpf_noise_1;
		noise_coefficient_count = taps_fir_bpf_noise_1;
		noise_divisor = 65536;
	} else {
		noise_coefficients = coef_fir_bpf_noise_2;
		noise_coefficient_count = taps_fir_bpf_noise_2;
		noise_divisor = gain_fir_bpf_noise_2;
	}
	sample_count = stage->nativeSamples;
	workspace.input = channel->receiveFrontendF32Input;
	workspace.baseband_output = channel->receiveFrontendF32Output;
	workspace.history = channel->receiveFrontendHistoryScratch;
	workspace.carrier_gate = channel->receiveFrontendCarrierGateScratch;
	workspace.native_frame_capacity = channel->receiveFrontendNativeCapacity;
	workspace.baseband_output_capacity = channel->receiveFrontendBaseCapacity;
	workspace.history_capacity = channel->receiveFrontendHistoryCapacity;
	state.decimator = stage->decimator;
	state.comparator_output = stage->compOut;
	state.rssi_peak = stage->apeak;
	state.reserved = 0U;
	state.rssi_power = stage->rssiPower;
	state.rssi_samples = stage->rssiSamples;
	state.micor_squelch = stage->micor_squelch;
	if (urp_radio_core_receive_frontend_s16(
		    stage->source, stage->sink, channel->rxBaseCapacity, channel->rxCarrierGate,
		    sample_count, (i16 *)stage->x, (size_t)stage->nx, baseband->coefs,
		    stage->calcAdjust, stage->outputGain, noise_coefficients,
		    noise_coefficient_count, noise_divisor, (uint32_t)stage->decimate,
		    (uint32_t)calibration_window, (uint32_t)stage->setpt, (uint32_t)stage->hyst,
		    &state, &output_count, &rssi_updated, &workspace))
		return -1;
	stage->decimator = state.decimator;
	stage->compOut = state.comparator_output;
	stage->rssiPower = state.rssi_power;
	stage->rssiSamples = state.rssi_samples;
	stage->micor_squelch = state.micor_squelch;
	if (rssi_updated) {
		stage->apeak = state.rssi_peak;
		channel->rxRssi = state.rssi_peak;
	}
	stage->nSamples = (i16)output_count;
	stage->nativeSamples = 0U;
	return 0;
}

/** @brief Run the live mono/unit-rate FIR shape through the optional core.
 * @param stage FIR stage and its caller-owned state and conversion spans.
 * @return Zero when the portable result was committed, otherwise nonzero.
 *
 * The two live receive FIR stages have this narrow shape.  The generic legacy
 * stage retains interpolation, output routing, mixing, and detector behavior;
 * any such request deliberately falls through to the original C path.
 */
static int urp_radio_fir_portable(urp_radio_stage *stage)
{
	urp_radio_state *channel;
	struct urp_radio_fir_workspace workspace;
	size_t sample_count;
	size_t history_count;
	i16 *history;
	const i16 *coefficients;

	if (!stage || !stage->parentChan || !stage->source || !stage->sink || !stage->x ||
	    !stage->coef || !stage->enabled || stage->option || stage->nSamples <= 0 ||
	    stage->nx <= 0 || stage->ncoef != stage->nx || stage->size_x != sizeof(i16) ||
	    stage->size_coef != sizeof(i16) || stage->decimate != 1 || stage->interpolate != 1 ||
	    stage->numChanOut != 1 || stage->selChanOut != 0 || stage->mixOut || stage->monoOut ||
	    stage->setpt)
		return -1;
	channel = stage->parentChan;
	sample_count = (size_t)stage->nSamples;
	history_count = (size_t)stage->nx;
	history = (i16 *)stage->x;
	coefficients = (const i16 *)stage->coef;
	workspace.input = channel->firF32Input;
	workspace.output = channel->firF32Output;
	workspace.history = channel->firHistoryScratch;
	workspace.frame_capacity = channel->firF32Capacity;
	workspace.history_capacity = channel->firHistoryCapacity;
	if (urp_radio_fir_s16_spans_overlap(stage->source, sample_count, stage->sink,
					    sample_count) ||
	    urp_radio_fir_s16_spans_overlap(stage->source, sample_count, history, history_count) ||
	    urp_radio_fir_s16_spans_overlap(stage->sink, sample_count, history, history_count) ||
	    urp_radio_fir_s16_spans_overlap(stage->source, sample_count, coefficients,
					    history_count) ||
	    urp_radio_fir_s16_spans_overlap(stage->sink, sample_count, coefficients,
					    history_count) ||
	    urp_radio_fir_s16_spans_overlap(history, history_count, coefficients, history_count) ||
	    urp_radio_core_fir_mono_s16(stage->source, stage->sink, sample_count, coefficients,
					history, history_count, stage->inputGain, stage->outputGain,
					stage->calcAdjust, &workspace))
		return -1;

	/* The original stage publishes these local signed-16 values on every call. */
	stage->apeak = 0;
	stage->discounteru = (i16)stage->discounteru;
	stage->discounterl = (i16)stage->discounterl;
	return 0;
}

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
	if (mySps->option != 3 && !urp_radio_fir_portable(mySps)) {
		return 0;
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

/** @brief Run the required portable receiver-deemphasis integrator.
 * @param stage Enabled legacy recursive-filter stage.
 * @return Zero after a portable update, otherwise nonzero.
 *
 * The compatibility stage owns its coefficient table and two-word history. The
 * shared core receives exact signed-16/F32 conversion spans and only publishes
 * its recursive accumulator after validating its complete output.
 */
static int gp_inte_00_portable(urp_radio_stage *stage)
{
	urp_radio_state *channel;
	struct rptadv_radio_deemphasis_integrator_state state;
	struct urp_radio_deemphasis_integrator_workspace workspace;

	if (!stage || !stage->parentChan || stage->nSamples < 0 || !stage->coef || !stage->x)
		return -1;
	channel = stage->parentChan;
	workspace.input = channel->deemphasisIntegratorF32Input;
	workspace.output = channel->deemphasisIntegratorF32Output;
	workspace.capacity = channel->deemphasisIntegratorF32Capacity;
	state.accumulator = ((i32 *)stage->x)[0];
	if (urp_radio_core_deemphasis_integrator_s16(
		    stage->source, stage->sink, (size_t)stage->nSamples, ((i16 *)stage->coef)[0],
		    ((i16 *)stage->coef)[1], stage->outputGain, &state, &workspace))
		return -1;
	((i32 *)stage->x)[0] = state.accumulator;
	return 0;
}

/*
	general purpose integrator lpf
*/
i16 gp_inte_00(urp_radio_stage *mySps)
{
	TRACEJ(5, "gp_inte_00() %i\n", mySps->enabled);
	if (!mySps->enabled)
		return 1;
	/* Historical signed negative spans are a no-op; they are never emitted by
	 * a live stage but retaining this guard keeps malformed compatibility calls
	 * side-effect free without retaining a second implementation. */
	if (mySps->nSamples < 0)
		return 0;
	return (i16) !!gp_inte_00_portable(mySps);
}

/** @brief Run the optional portable center slicer through caller-owned workspaces.
 * @param stage Enabled CTCSS centering stage.
 * @return Zero after a portable update; nonzero selects the retained C path.
 *
 * Active diagnostic tracing retains the exact C implementation because its
 * historical per-sample min/max trace phase is diagnostic-only and process
 * global. The F32 primitive therefore cannot perturb audio, detector state,
 * or callback partitioning when tracing is enabled.
 */
static int center_slicer_portable(urp_radio_stage *stage)
{
	urp_radio_state *channel;
	struct rptadv_radio_center_slicer_state state;
	struct urp_radio_center_slicer_workspace workspace;

	if (!stage || !stage->parentChan || stage->nSamples < 0)
		return -1;
	channel = stage->parentChan;
#if URP_RADIO_DEBUG == 1
	if (channel->tracetype)
		return -1;
#endif
	workspace.input = channel->centerSlicerF32Input;
	workspace.centered_output = channel->centerSlicerF32CenteredOutput;
	workspace.limited_output = channel->centerSlicerF32LimitedOutput;
	workspace.capacity = channel->centerSlicerF32Capacity;
	state.maximum = stage->amax;
	state.minimum = stage->amin;
	state.peak = stage->apeak;
	state.upper_decay_counter = stage->discounteru;
	state.lower_decay_counter = stage->discounterl;
	if (urp_radio_core_center_slicer_s16(stage->source, stage->sink, (i16 *)stage->buff,
					     (size_t)stage->nSamples, stage->inputGainB,
					     stage->setpt, stage->discfactor, &state, &workspace))
		return -1;
	stage->amax = state.maximum;
	stage->amin = state.minimum;
	stage->apeak = state.peak;
	stage->discounteru = state.upper_decay_counter;
	stage->discounterl = state.lower_decay_counter;
	return 0;
}

/** @brief Run the retained signed-16 center slicer after an optional-core fallback.
 * @param mySps Enabled CTCSS centering stage with legacy sample buffers.
 * @return Zero after processing.
 */
static i16 center_slicer_legacy_active(urp_radio_stage *mySps)
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

		amax -= discfactor;
		if (amax < amin) {
			amax = amin;
		}

		amin += discfactor;
		if (amin > amax) {
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
	CenterSlicer
*/
i16 CenterSlicer(urp_radio_stage *mySps)
{
	TRACEJ(5, "CenterSlicer() %i\n", mySps->enabled);
	if (!mySps->enabled)
		return 1;
	if (!center_slicer_portable(mySps))
		return 0;
	return center_slicer_legacy_active(mySps);
}

/** @brief Run the required portable envelope primitive for one C stage.
 * @param stage Existing compatibility stage whose state remains C-owned.
 * @return Zero after a portable update, otherwise nonzero.
 *
 * The stage and its source/sink buffers belong to the existing radio core.
 * Only exact signed-16/F32 conversion workspaces cross the shared-library
 * boundary. Descriptor validation during setup makes the primitive mandatory.
 */
static int measure_block_portable(urp_radio_stage *stage)
{
	urp_radio_state *channel;
	struct rptadv_radio_envelope_state state;
	int comparator = 0;

	if (!stage || !stage->parentChan || stage->nSamples < 0 ||
	    (stage->nSamples && !stage->source))
		return -1;
	channel = stage->parentChan;
	state.maximum = stage->amax;
	state.minimum = stage->amin;
	state.peak = stage->apeak;
	state.upper_decay_counter = stage->discounteru;
	state.lower_decay_counter = stage->discounterl;
	if (urp_radio_core_measure_envelope_s16(stage->source, stage->sink, (size_t)stage->nSamples,
						stage->discfactor, stage->setpt, &state,
						channel->measureF32Input, channel->measureF32Output,
						channel->measureF32Capacity, &comparator))
		return -1;
	stage->amax = state.maximum;
	stage->amin = state.minimum;
	stage->apeak = state.peak;
	stage->discounteru = state.upper_decay_counter;
	stage->discounterl = state.lower_decay_counter;
	stage->compOut = (i16)comparator;
	return 0;
}

/* 	----------------------------------------------------------------------
	MeasureBlock
	determine peak amplitude
*/
i16 MeasureBlock(urp_radio_stage *mySps)
{
	TRACEJ(5, "MeasureBlock() %i\n", mySps->enabled);

	if (!mySps->enabled) {
		return 1;
	}

	return (i16) !!measure_block_portable(mySps);
}

/** @brief Run the required portable delay primitive for one C stage.
 * @param stage Existing compatibility stage whose cursor remains C-owned.
 * @return Zero after a portable update, otherwise nonzero.
 *
 * The F32 storage is allocated while the receive stage is built and remains
 * authoritative for the lifetime of the stage. The S16 boundary conversion
 * completes before output is published, so an in-place source/sink is safe.
 */
static int delay_line_portable(urp_radio_stage *stage)
{
	urp_radio_state *channel;
	struct urp_radio_delay_workspace workspace;
	unsigned int dirty;
	int result;

	if (!stage || !stage->parentChan)
		return -1;
	channel = stage->parentChan;
	workspace.input = channel->delayF32Input;
	workspace.output = channel->delayF32Output;
	workspace.storage = channel->delayF32Storage;
	workspace.frame_capacity = channel->delayF32FrameCapacity;
	workspace.storage_capacity = channel->delayF32StorageCapacity;
	dirty = stage->b.dirty;
	result = urp_radio_core_delay_line_s16(stage->source, stage->sink, (size_t)stage->nSamples,
					       stage->buffSize, stage->buffLead,
					       &stage->buffInIndex, &dirty, !!stage->enabled,
					       !!stage->b.outzero, &workspace);
	if (result)
		return -1;
	stage->b.dirty = dirty;
	return 0;
}

/*
	DelayLine
*/
i16 DelayLine(urp_radio_stage *mySps)
{
	if (mySps->nSamples < 0)
		return 0;
	return (i16) !!delay_line_portable(mySps);
}

/** @brief Build the portable decoder's selected-tone snapshot from C mappings.
 * @param channel Radio-signaling state owning the CTCSS mapping.
 * @return Bit mask with one bit for each configured receive detector.
 */
static uint64_t urp_ctcss_receive_tone_mask(const urp_radio_state *channel)
{
	uint64_t mask = 0U;
	i16 index;

	for (index = 0; index < CTCSS_NUM_CODES; ++index)
		if (channel->rxCtcssMap[index] != CTCSS_NULL)
			mask |= UINT64_C(1) << index;
	return mask;
}

/** @brief Publish a portable CTCSS decision through the compatibility fields.
 * @param channel Radio-signaling state owning CTCSS status text.
 * @param decoded Portable CTCSS table index or @ref CTCSS_NULL.
 */
static void urp_ctcss_apply_portable_decode(urp_radio_state *channel, int decoded)
{
	if (decoded > CTCSS_NULL && decoded < CTCSS_NUM_CODES) {
		channel->rxCtcss->decode = (i16)decoded;
		snprintf(channel->rxctcssfreq, sizeof(channel->rxctcssfreq), "%.1f",
			 freq_ctcss[decoded]);
		return;
	}
	channel->rxCtcss->decode = CTCSS_NULL;
	strcpy(channel->rxctcssfreq, "0");
}

void urp_ctcss_set_receive_callback(urp_ctcss_decoder *decoder, urp_ctcss_receive_callback callback,
				    void *context)
{
	if (!decoder)
		return;
	decoder->receive_callback = callback;
	decoder->receive_callback_context = context;
}

i16 urp_ctcss_decode(urp_radio_state *channel)
{
	int decoded = CTCSS_NULL;
	int result = -1;

	if (!channel->rxCtcss->enabled)
		return 1;
	if (channel->rxCtcss->receive_callback)
		result = channel->rxCtcss->receive_callback(
			channel->rxCtcss->receive_callback_context, channel->rxCtcss->input,
			(size_t)channel->activeSamplesRx, urp_ctcss_receive_tone_mask(channel),
			channel->rxCtcss->relax, channel->rxCarrierDetect, &decoded);
	if (result != 0 || decoded < CTCSS_NULL || decoded >= CTCSS_NUM_CODES) {
		urp_ctcss_apply_portable_decode(channel, CTCSS_NULL);
		return -1;
	}
	urp_ctcss_apply_portable_decode(channel, decoded);
	return 0;
}

/*
	assumes:
	sampling rate is 48KS/s
	samples are all 16 bits
	samples are filtered and decimated by 1/6th
*/
/** @brief Allocate and append a detector stage to a channel's stage list.
 * @param channel Radio-signaling engine state.
 * @param tail Current tail of the detector-stage list.
 * @return Newly appended stage, or NULL if allocation fails.
 */
static urp_radio_stage *urp_radio_stage_append(urp_radio_state *channel, urp_radio_stage *tail)
{
	urp_radio_stage *next = urp_radio_stage_create(channel);

	if (next) {
		tail->nextSps = next;
	}
	return next;
}

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
	/* The legacy wrapper remains a fixed 160-sample 8 kHz frame.  Native
	 * callbacks can begin mid-decimation, so allocate the conservative
	 * ceil((max_native + factor - 1) / factor) baseband bound separately. */
	pChan->rxBaseCapacity =
		(u32)(((size_t)numSamples * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK) +
		       2U * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK) - 2U) /
		      (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK));
	/* A direct decoder call is valid before the first native tick. */
	pChan->activeSamplesTx = pChan->activeSamplesRx = numSamples;

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
		ast_copy_string(pChan->dcsRxCode, tChan->dcsRxCode, sizeof(pChan->dcsRxCode));
		ast_copy_string(pChan->dcsTxCode, tChan->dcsTxCode, sizeof(pChan->dcsTxCode));
		pChan->dcsTurnoffEnabled = tChan->dcsTurnoffEnabled;
		pChan->dcsTurnoffDuration = tChan->dcsTurnoffDuration;
		pChan->dcsPeak = tChan->dcsPeak;
		pChan->txCtcssTocShift = tChan->txCtcssTocShift;
		pChan->txCtcssTocTime = tChan->txCtcssTocTime;
		pChan->txCtcssTocToneHz = tChan->txCtcssTocToneHz;
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
	if (pChan->txCtcssTocTime <= 0)
		pChan->txCtcssTocTime = CTCSS_TURN_OFF_TIME;
	if (pChan->txCtcssTocShift == 0.0)
		pChan->txCtcssTocShift = CTCSS_TURN_OFF_SHIFT;
	if (pChan->txCtcssTocToneHz <= 0.0)
		pChan->txCtcssTocToneHz = 55.0;
	if (pChan->dcsTurnoffDuration <= 0)
		pChan->dcsTurnoffDuration = 180;
	if (pChan->dcsPeak <= 0.0)
		pChan->dcsPeak = 1000.0;

	if (pChan->rxCdType == CD_XPMR_NOISE) {
		pChan->rxNoiseSquelchEnable = 1;
	}

	if (pChan->rxDemod == RX_AUDIO_FLAT) {
		pChan->rxDeEmpEnable = 1;
	}

	pChan->rxCarrierPoint = (pChan->rxSquelchPoint * 32767) / 100;

	urp_dcs_init(&pChan->dcs);
	{
		int rx_code = -1, tx_code = -1, rx_inverted = 0, tx_inverted = 0;
		if (pChan->dcsRxCode[0] &&
		    urp_dcs_parse_code(pChan->dcsRxCode, &rx_code, &rx_inverted))
			ast_log(LOG_WARNING, "RadioPlus: ignoring invalid DCS receive code '%s'\n",
				pChan->dcsRxCode);
		if (pChan->dcsTxCode[0] &&
		    urp_dcs_parse_code(pChan->dcsTxCode, &tx_code, &tx_inverted))
			ast_log(LOG_WARNING, "RadioPlus: ignoring invalid DCS transmit code '%s'\n",
				pChan->dcsTxCode);
		urp_dcs_configure(&pChan->dcs, rx_code, rx_inverted, tx_code, tx_inverted);
	}

	pChan->lastrxdecode = CTCSS_NULL;

	TRACEF(1, "calloc buffers \n");

	ALLOCATE_OR_FAIL(pChan->pRxDemod, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->pRxNoise, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->pRxBase, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->rxCarrierGate, numSamples * 6, sizeof(*pChan->rxCarrierGate));
	ALLOCATE_OR_FAIL(pChan->pRxHpf, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->pRxLsd, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->pRxSpeaker, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->pRxCtcss, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->pRxDcTrack, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->pRxLsdLimit, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->prxMeasure, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->measureF32Input, pChan->rxBaseCapacity,
			 sizeof(*pChan->measureF32Input));
	ALLOCATE_OR_FAIL(pChan->measureF32Output, pChan->rxBaseCapacity,
			 sizeof(*pChan->measureF32Output));
	pChan->measureF32Capacity = pChan->rxBaseCapacity;
	ALLOCATE_OR_FAIL(pChan->centerSlicerF32Input, pChan->rxBaseCapacity,
			 sizeof(*pChan->centerSlicerF32Input));
	ALLOCATE_OR_FAIL(pChan->centerSlicerF32CenteredOutput, pChan->rxBaseCapacity,
			 sizeof(*pChan->centerSlicerF32CenteredOutput));
	ALLOCATE_OR_FAIL(pChan->centerSlicerF32LimitedOutput, pChan->rxBaseCapacity,
			 sizeof(*pChan->centerSlicerF32LimitedOutput));
	pChan->centerSlicerF32Capacity = pChan->rxBaseCapacity;
	ALLOCATE_OR_FAIL(pChan->firF32Input, pChan->rxBaseCapacity, sizeof(*pChan->firF32Input));
	ALLOCATE_OR_FAIL(pChan->firF32Output, pChan->rxBaseCapacity, sizeof(*pChan->firF32Output));
	/* A history larger than this configured callback bound retains the C fallback. */
	ALLOCATE_OR_FAIL(pChan->firHistoryScratch, pChan->rxBaseCapacity,
			 sizeof(*pChan->firHistoryScratch));
	pChan->firF32Capacity = pChan->rxBaseCapacity;
	pChan->firHistoryCapacity = pChan->rxBaseCapacity;
	/* The frontend keeps native stereo input and a transactional gate span;
	 * all storage is allocated at stream setup so its optional portable call is
	 * allocation-free inside the receive callback. */
	ALLOCATE_OR_FAIL(pChan->receiveFrontendF32Input,
			 (size_t)pChan->nSamplesRx * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK) * 2U,
			 sizeof(*pChan->receiveFrontendF32Input));
	ALLOCATE_OR_FAIL(pChan->receiveFrontendF32Output, pChan->rxBaseCapacity,
			 sizeof(*pChan->receiveFrontendF32Output));
	ALLOCATE_OR_FAIL(pChan->receiveFrontendHistoryScratch, pChan->rxBaseCapacity,
			 sizeof(*pChan->receiveFrontendHistoryScratch));
	ALLOCATE_OR_FAIL(pChan->receiveFrontendCarrierGateScratch,
			 (size_t)pChan->nSamplesRx * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK),
			 sizeof(*pChan->receiveFrontendCarrierGateScratch));
	pChan->receiveFrontendNativeCapacity =
		pChan->nSamplesRx * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK);
	pChan->receiveFrontendBaseCapacity = pChan->rxBaseCapacity;
	pChan->receiveFrontendHistoryCapacity = pChan->rxBaseCapacity;

#if URP_RADIO_DEBUG == 1
	TRACEF(1, "configure tracing\n");

	ALLOCATE_OR_FAIL(pChan->pRxLsdCen, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->prxDebug0, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug0, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug1, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug2, pChan->rxBaseCapacity, 2);
	ALLOCATE_OR_FAIL(pChan->rxCtcss->pDebug3, pChan->rxBaseCapacity, 2);

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
		pChan->sdbg->source[7] = pChan->rxCtcss->pDebug0;
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
		pChan->sdbg->source[7] = pChan->rxCtcss->pDebug0;
		pChan->sdbg->source[8] = pChan->rxCtcss->pDebug1;
		pChan->sdbg->source[9] = pChan->rxCtcss->pDebug2;
		pChan->sdbg->source[10] = pChan->rxCtcss->pDebug3;
		pChan->sdbg->trace[11] = RX_CTCSS_DECODE;
		pChan->sdbg->trace[12] = RX_SMODE;
		pChan->sdbg->trace[13] = TX_PTT_IN;
		pChan->sdbg->trace[14] = TX_PTT_OUT;
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
	pSps = urp_radio_stage_append(pChan, pSps);
	if (!pSps) {
		goto allocation_failed;
	}
	pChan->spsRxLsd = pSps;
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
	pSps = urp_radio_stage_append(pChan, pSps);
	if (!pSps) {
		goto allocation_failed;
	}
	pChan->spsRxLsdNrz = pSps;
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
	pSps = urp_radio_stage_append(pChan, pSps);
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
		pSps = urp_radio_stage_append(pChan, pSps);
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
		ALLOCATE_OR_FAIL(pChan->deemphasisIntegratorF32Input, pChan->rxBaseCapacity,
				 sizeof(*pChan->deemphasisIntegratorF32Input));
		ALLOCATE_OR_FAIL(pChan->deemphasisIntegratorF32Output, pChan->rxBaseCapacity,
				 sizeof(*pChan->deemphasisIntegratorF32Output));
		pChan->deemphasisIntegratorF32Capacity = pChan->rxBaseCapacity;
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
		pSps = urp_radio_stage_append(pChan, pSps);
		if (!pSps) {
			goto allocation_failed;
		}
		pChan->spsDelayLine = pSps;
		pChan->spsRxSquelchDelay = pSps;
		pSps->parentChan = pChan;
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
		/* buffSize retains the active F32 circular-storage length; the old
		 * signed-16 delay buffer is no longer allocated. */
		pSps->buffSize = RXSQDELAYBUFSIZE;
		ALLOCATE_OR_FAIL(pChan->delayF32Input, pChan->rxBaseCapacity,
				 sizeof(*pChan->delayF32Input));
		ALLOCATE_OR_FAIL(pChan->delayF32Output, pChan->rxBaseCapacity,
				 sizeof(*pChan->delayF32Output));
		ALLOCATE_OR_FAIL(pChan->delayF32Storage, RXSQDELAYBUFSIZE,
				 sizeof(*pChan->delayF32Storage));
		pChan->delayF32FrameCapacity = pChan->rxBaseCapacity;
		pChan->delayF32StorageCapacity = RXSQDELAYBUFSIZE;
		pSps->buffLead = pChan->rxSquelchDelay * 8; /* convert ms to samples */
		pSps->buffInIndex = 0;
		pSps->buffOutIndex = 0;
	}

	if (pChan->rxCdType == CD_XPMR_VOX) {
		TRACEF(1, "create vox measureblock\n");
		ALLOCATE_OR_FAIL(pChan->prxVoxMeas, pChan->rxBaseCapacity, 2);

		pSps = urp_radio_stage_append(pChan, pSps);
		if (!pSps) {
			goto allocation_failed;
		}
		pChan->spsRxVox = pSps;
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
	pSps = urp_radio_stage_append(pChan, pSps);
	if (!pSps) {
		goto allocation_failed;
	}
	pChan->spsMeasure = pSps;
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

	if (!pChan) {
		return 1;
	}
	TRACEF(1, "urp_radio_destroy()\n");

	ast_free(pChan->pRxDemod);
	ast_free(pChan->pRxNoise);
	ast_free(pChan->pRxBase);
	ast_free(pChan->rxCarrierGate);
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
	ast_free(pChan->measureF32Input);
	ast_free(pChan->measureF32Output);
	ast_free(pChan->centerSlicerF32Input);
	ast_free(pChan->centerSlicerF32CenteredOutput);
	ast_free(pChan->centerSlicerF32LimitedOutput);
	ast_free(pChan->deemphasisIntegratorF32Input);
	ast_free(pChan->deemphasisIntegratorF32Output);
	ast_free(pChan->firF32Input);
	ast_free(pChan->firF32Output);
	ast_free(pChan->firHistoryScratch);
	ast_free(pChan->receiveFrontendF32Input);
	ast_free(pChan->receiveFrontendF32Output);
	ast_free(pChan->receiveFrontendHistoryScratch);
	ast_free(pChan->receiveFrontendCarrierGateScratch);
	ast_free(pChan->delayF32Input);
	ast_free(pChan->delayF32Output);
	ast_free(pChan->delayF32Storage);
	ast_free(pChan->prxVoxMeas);
	ast_free((void *)pChan->pRxCode);
	ast_free(pChan->pRxCodeStr);
	ast_free((void *)pChan->pTxCode);
	ast_free(pChan->pTxCodeStr);

#if URP_RADIO_DEBUG == 1
	ast_free(pChan->prxDebug0);
	ast_free(pChan->pRxLsdCen);

	if (pChan->rxCtcss) {
		ast_free(pChan->rxCtcss->pDebug0);
		ast_free(pChan->rxCtcss->pDebug1);
		ast_free(pChan->rxCtcss->pDebug2);
		ast_free(pChan->rxCtcss->pDebug3);
	}
#endif

	ast_free(pChan->rxCtcss);

	pmr_sps = pChan->spsRx;

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

/** @brief Set the active detector count for one native callback.
 * @param channel Radio state owning the receive detector stages.
 * @param samples Number of emitted base-rate samples for these stages.
 *
 * Allocation occurs once for the adapter-declared maximum.  The fixed-point
 * detector stages retain their history while this helper changes only the
 * count consumed during the current callback.
 */
static void urp_radio_set_active_samples(urp_radio_state *channel, i16 samples)
{
	urp_radio_stage *stage;

	channel->activeSamplesRx = samples;
	channel->activeSamplesTx = samples;
	/* The receive frontend has already consumed its native span and published
	 * this exact base-rate count.  Every following stage must see only emitted
	 * samples, including zero when a callback ends before a decimator boundary. */
	for (stage = channel->spsRx; stage; stage = stage->nextSps)
		stage->nSamples = samples;
}

/** @brief Convert native PCM duration to whole milliseconds without tick drift.
 * @param remainder Fractional native samples retained between calls.
 * @param native_frames Native samples elapsed in this callback.
 * @return Whole elapsed milliseconds after carrying the fraction.
 */
static i32 urp_radio_elapsed_ms(u32 *remainder, size_t native_frames)
{
	i32 milliseconds;
	uint64_t elapsed = (uint64_t)*remainder + native_frames;

	if (!urp_radio_core_elapsed_ms(remainder, native_frames, &milliseconds))
		return milliseconds;
	milliseconds = (i32)(elapsed / (SAMPLE_RATE_INPUT / 1000U));

	*remainder = (u32)(elapsed % (SAMPLE_RATE_INPUT / 1000U));
	return milliseconds;
}

/** @brief Consume a timer and return PCM time remaining after it expires.
 * @param timer Remaining milliseconds, updated without crossing below zero.
 * @param milliseconds Elapsed native PCM time in milliseconds.
 * @return Elapsed time left after the timer expires.
 *
 * A native callback can be split differently by an adapter.  Returning the
 * residual duration lets a successor state start at the same sample time.
 */
static i32 urp_radio_timer_consume(i32 *timer, i32 milliseconds)
{
	i32 remaining;

	if (!urp_radio_core_timer_consume(timer, milliseconds, &remaining))
		return remaining;
	if (*timer <= 0 || milliseconds <= 0)
		return milliseconds;
	if (milliseconds >= *timer) {
		milliseconds -= *timer;
		*timer = 0;
		return milliseconds;
	}
	*timer -= milliseconds;
	return 0;
}

/** @brief Decrement a millisecond timer by elapsed native PCM duration.
 * @param timer Remaining timer duration, updated in place.
 * @param milliseconds Elapsed native PCM time in milliseconds.
 */
static void urp_radio_timer_advance(i32 *timer, i32 milliseconds)
{
	(void)urp_radio_timer_consume(timer, milliseconds);
}

/** @brief Try the portable post-transmit receive-blanking transition.
 * \param channel Compatibility radio state owning the signed-16 ADC buffer.
 * \param elapsed_ms Whole native PCM milliseconds already consumed this callback.
 * \param remainder_before Fractional native frames before elapsed-time advancement.
 * \param native_frame_count Native frames supplied to the current callback.
 * \param blanked_frames Receives the leading input frames that must be muted.
 * \return Zero only after a validated portable result is committed.
 *
 * C intentionally retains the left-channel PCM mute loop at its original
 * hardware boundary.  The portable operation owns only state arithmetic, so
 * a missing or rejected append-only descriptor can execute the exact retained
 * C computation after the sample remainder has advanced once.
 */
static int urp_radio_rx_blanking_portable(urp_radio_state *channel, i32 elapsed_ms,
					  u32 remainder_before, size_t native_frame_count,
					  size_t *blanked_frames)
{
	const i16 prior_remaining_ms = channel ? channel->txrxblankingtimer : 0;
	struct rptadv_radio_rx_blanking_input input;
	struct rptadv_radio_rx_blanking_state state;

	if (blanked_frames)
		*blanked_frames = 0U;
	if (!channel || !blanked_frames || prior_remaining_ms <= 0 || elapsed_ms < 0 ||
	    native_frame_count > UINT32_MAX)
		return -1;
	input.elapsed_ms = elapsed_ms;
	input.native_frame_count = (u32)native_frame_count;
	input.sample_remainder_before = remainder_before;
	state.remaining_ms = prior_remaining_ms;
	state.blanked_frame_count = 0U;
	if (urp_radio_core_rx_blanking_advance(&input, &state) || state.remaining_ms < 0 ||
	    state.remaining_ms > prior_remaining_ms ||
	    state.blanked_frame_count > input.native_frame_count)
		return -1;
	channel->txrxblankingtimer = (i16)state.remaining_ms;
	*blanked_frames = state.blanked_frame_count;
	return 0;
}

/** @brief Try the portable scalar VOX carrier-hang transition transactionally.
 * @param channel Compatibility state holding the VOX timer and carrier result.
 * @param elapsed_ms Whole native PCM milliseconds consumed in this callback.
 * @return Zero after a validated portable state transition; otherwise C fallback.
 *
 * The retained envelope stage still owns detector measurement at 8 kHz. This
 * helper moves only the timer/carrier decision, so an older shared object or a
 * rejected result leaves all state available to the exact legacy C branch.
 */
static int urp_radio_vox_carrier_portable(urp_radio_state *channel, i32 elapsed_ms)
{
	struct rptadv_radio_vox_carrier_input input;
	struct rptadv_radio_vox_carrier_state state;

	if (!channel || !channel->spsRxVox)
		return -1;
	input.detector_active = channel->spsRxVox->compOut ? 1U : 0U;
	input.hang_time_ms = channel->voxHangTime;
	input.elapsed_ms = elapsed_ms;
	state.remaining_ms = channel->rxVoxTimer;
	state.carrier_detect = channel->rxCarrierDetect ? 1U : 0U;
	if (urp_radio_core_vox_carrier_advance(&input, &state) || state.remaining_ms < 0 ||
	    state.carrier_detect > 1U)
		return -1;
	channel->rxVoxTimer = state.remaining_ms;
	channel->rxCarrierDetect = (i16)state.carrier_detect;
	return 0;
}

/**
 * @brief Try the portable transmitter CPU-saver transition transactionally.
 * \param channel Compatibility transmitter state holding the halt flag.
 * \return Zero after a validated portable state transition; otherwise C fallback.
 *
 * The legacy branch owns its early renderer return. This helper moves only
 * the scalar halt decision, so a missing shared object or rejected result
 * leaves that exact branch available without changing renderer behavior.
 */
static int urp_radio_tx_cpu_saver_portable(urp_radio_state *channel)
{
	struct rptadv_radio_tx_cpu_saver_input input;
	struct rptadv_radio_tx_cpu_saver_state state;

	if (!channel)
		return -1;
	input.enabled = !!channel->txCpuSaver;
	input.tx_ptt_in = !!channel->txPttIn;
	input.tx_ptt_out = !!channel->txPttOut;
	input.tx_idle = channel->txState == CHAN_TXSTATE_IDLE;
	state.halted = channel->b.txhalted;
	if (urp_radio_core_tx_cpu_saver_advance(&input, &state) || state.halted > 1U)
		return -1;
	channel->b.txhalted = state.halted;
	return 0;
}

/**
 * \brief Try the portable receiver CPU-saver transition transactionally.
 * \param channel Compatibility receiver state holding the halt flag.
 * \param action Receives the validated DSP-stage transition action on success.
 * \param next_halted Receives the validated halt bit after the transition.
 * \return Zero after a validated portable transition; otherwise C fallback.
 *
 * The shared core owns only the scalar predicate and transition label. This
 * compatibility boundary retains the physical HPF/deemphasis stage writes at
 * their established location immediately before the native receive frontend.
 */
static int urp_radio_rx_cpu_saver_portable(urp_radio_state *channel, u32 *action, u32 *next_halted)
{
	struct rptadv_radio_rx_cpu_saver_input input;
	struct rptadv_radio_rx_cpu_saver_state state;
	u32 prior_halted;

	if (!channel || !action || !next_halted)
		return -1;
	prior_halted = !!channel->b.rxhalted;
	input.enabled = !!channel->rxCpuSaver;
	input.carrier_detect = !!channel->rxCarrierDetect;
	input.signal_mode_null = channel->smode == SMODE_NULL;
	input.tx_ptt_in = !!channel->txPttIn;
	input.tx_ptt_out = !!channel->txPttOut;
	state.halted = prior_halted;
	state.action = RPTADV_RADIO_RX_CPU_SAVER_ACTION_NONE;
	if (urp_radio_core_rx_cpu_saver_advance(&input, &state) || state.halted > 1U ||
	    state.action > RPTADV_RADIO_RX_CPU_SAVER_ACTION_LEAVE ||
	    (state.halted == prior_halted &&
	     state.action != RPTADV_RADIO_RX_CPU_SAVER_ACTION_NONE) ||
	    (state.halted != prior_halted &&
	     state.action != (state.halted ? RPTADV_RADIO_RX_CPU_SAVER_ACTION_ENTER
					   : RPTADV_RADIO_RX_CPU_SAVER_ACTION_LEAVE)))
		return -1;
	*action = state.action;
	*next_halted = state.halted;
	return 0;
}

/** @brief Try the portable CTCSS/DCS signaling-mode resolver transactionally.
 * @param channel Legacy signaling state to copy at the compatibility boundary.
 * @param elapsed_ms Whole native PCM milliseconds in this callback span.
 * @param decoded_ctcss Current decoder result or @ref CTCSS_NULL.
 * @return Zero only after every portable result has been range-checked and committed.
 *
 * The portable operation receives fixed tenths-of-a-hertz selections instead
 * of parser state or the legacy floating-point tone table.  This preserves
 * the historical table conversion at the C boundary and keeps an unavailable
 * shared object from changing the established signaling decision.
 */
static int urp_radio_signal_mode_portable(urp_radio_state *channel, i32 elapsed_ms,
					  int decoded_ctcss)
{
	struct rptadv_radio_signal_mode_config config = {
		.struct_size = sizeof(struct rptadv_radio_signal_mode_config),
	};
	struct rptadv_radio_signal_mode_input input;
	struct rptadv_radio_signal_mode_state state;
	int index;

	if (!channel || decoded_ctcss < CTCSS_NULL || decoded_ctcss >= CTCSS_NUM_CODES)
		return -1;
	if (decoded_ctcss > CTCSS_NULL) {
		const i16 selected = channel->rxCtcssMap[decoded_ctcss];

		/* Preserve unusual legacy map values by letting the retained C block
		 * handle them; the portable operation only receives defined selections. */
		if (selected != CTCSS_RXONLY && (selected < 0 || selected >= CTCSS_NUM_CODES))
			return -1;
	}
	config.hold_ms = channel->smodetime;
	config.ctcss_tx_enabled = !!channel->b.ctcssTxEnable;
	config.default_tx_ctcss_frequency_tenths_hz = (i32)(channel->txctcssdefault_value * 10.0F);
	for (index = 0; index < CTCSS_NUM_CODES; ++index) {
		const i16 mapped = channel->rxCtcssMap[index];

		if (mapped >= 0 && mapped < CTCSS_NUM_CODES)
			config.mapped_tx_ctcss_frequency_tenths_hz[index] =
				(i32)(freq_ctcss[mapped] * 10.0F);
	}
	input.elapsed_ms = elapsed_ms;
	input.decoded_ctcss = decoded_ctcss;
	input.dcs_valid = !!channel->dcs.valid;
	input.tx_ptt_in = !!channel->txPttIn;
	state.smode = channel->smode;
	state.smode_was = channel->smodewas;
	state.smode_timer_ms = channel->smodetimer;
	state.last_rx_ctcss = channel->lastrxdecode;
	state.tx_ctcss_frequency_tenths_hz = channel->txCtcssFreq10;
	state.tx_ctcss_option = channel->txCtcssOption;
	state.smode_turnoff = !!channel->b.smodeturnoff;
	if (urp_radio_core_signal_mode_advance(&config, &input, &state) ||
	    state.smode < INT16_MIN || state.smode > INT16_MAX || state.smode_was < INT16_MIN ||
	    state.smode_was > INT16_MAX || state.last_rx_ctcss < CTCSS_NULL ||
	    state.last_rx_ctcss >= CTCSS_NUM_CODES || state.tx_ctcss_option > 3U ||
	    state.smode_turnoff > 1U)
		return -1;
	channel->smode = (i16)state.smode;
	channel->smodewas = (i16)state.smode_was;
	channel->smodetimer = state.smode_timer_ms;
	channel->lastrxdecode = (i16)state.last_rx_ctcss;
	channel->txCtcssFreq10 = state.tx_ctcss_frequency_tenths_hz;
	channel->txCtcssOption = (i16)state.tx_ctcss_option;
	channel->b.smodeturnoff = state.smode_turnoff;
	return 0;
}

/** @brief Try the portable final CTCSS oscillator-control transition transactionally.
 * @param channel Legacy transmitter state to copy at the compatibility boundary.
 * @param elapsed_ms Whole native PCM milliseconds in this callback span.
 * @return Zero only after a fully validated portable result is committed.
 *
 * The transmitter state machine has already chosen whether this callback starts
 * CTCSS, begins a phase/tail-tone turn-off, or disables it.  This narrow
 * operation preserves that ownership: it only translates the one-shot request
 * into renderer-facing state.  Any unusual legacy value falls through to the
 * retained C block below without changing the current transition.
 */
static int urp_radio_ctcss_render_state_portable(urp_radio_state *channel, i32 elapsed_ms)
{
	struct rptadv_radio_ctcss_render_state_config config = {
		.struct_size = sizeof(struct rptadv_radio_ctcss_render_state_config),
	};
	const struct rptadv_radio_ctcss_render_state_input input = {
		.elapsed_ms = elapsed_ms,
	};
	struct rptadv_radio_ctcss_render_state state;

	if (!channel || elapsed_ms < 0 || channel->txCtcssTocTime < 0 ||
	    channel->txCtcssOption < 0 || channel->txCtcssOption > 3 || channel->txCtcssState < 0 ||
	    channel->txCtcssState > 2 || channel->txCtcssEnabled < 0 ||
	    channel->txCtcssEnabled > 1 || channel->txCtcssTurnoffTimer < 0 ||
	    !isfinite(channel->txCtcssTocShift) || !isfinite(channel->txCtcssTocToneHz) ||
	    !isfinite(channel->txCtcssPhaseShift) || !isfinite(channel->txCtcssTailToneHz))
		return -1;
	config.turnoff_duration_ms = channel->txCtcssTocTime;
	config.turnoff_phase_shift_degrees = channel->txCtcssTocShift;
	config.turnoff_tail_tone_hz = channel->txCtcssTocToneHz;
	state.option = (uint32_t)channel->txCtcssOption;
	state.oscillator_state = (uint32_t)channel->txCtcssState;
	state.enabled = (uint32_t)channel->txCtcssEnabled;
	state.turnoff_remaining_ms = channel->txCtcssTurnoffTimer;
	state.phase_shift_degrees = channel->txCtcssPhaseShift;
	state.tail_tone_hz = channel->txCtcssTailToneHz;
	if (urp_radio_core_ctcss_render_state_advance(&config, &input, &state) ||
	    state.option > 3U || state.oscillator_state > 2U || state.enabled > 1U ||
	    state.turnoff_remaining_ms < 0 || !isfinite(state.phase_shift_degrees) ||
	    !isfinite(state.tail_tone_hz))
		return -1;
	channel->txCtcssOption = (i8)state.option;
	channel->txCtcssState = (i8)state.oscillator_state;
	channel->txCtcssEnabled = (i8)state.enabled;
	channel->txCtcssTurnoffTimer = state.turnoff_remaining_ms;
	channel->txCtcssPhaseShift = state.phase_shift_degrees;
	channel->txCtcssTailToneHz = state.tail_tone_hz;
	return 0;
}

/** @brief Try the selected DCS transmitter turn-off timing through the core.
 * @param channel Compatibility transmitter state copied at the ABI boundary.
 * @param elapsed_ms Whole native PCM milliseconds in this callback span.
 * @param begin_turnoff Nonzero only when C has selected a new DCS tail.
 * @param finish_requested Receives the retained finishing-helper request.
 * @param finish_elapsed_ms Receives the duration residual for that helper.
 * @return Zero only after all portable state has been validated and committed.
 *
 * DCS eligibility, waveform rendering, PTT, and hardware remain in C. This
 * helper only moves the existing ACTIVE/TOC timer arithmetic, so an absent or
 * rejected descriptor leaves the exact C transition available as fallback.
 */
static int urp_radio_dcs_turnoff_portable(urp_radio_state *channel, i32 elapsed_ms,
					  int begin_turnoff, int *finish_requested,
					  i32 *finish_elapsed_ms)
{
	const i32 prior_dcs_turnoff_ms = channel ? channel->dcsTurnoffTimer : 0;
	const i32 prior_tx_hang_ms = channel ? channel->txHangTime : 0;
	const struct rptadv_radio_dcs_turnoff_config config = {
		.turnoff_duration_ms = channel ? channel->dcsTurnoffDuration : 0,
	};
	const struct rptadv_radio_dcs_turnoff_input input = {
		.elapsed_ms = elapsed_ms,
		.tx_ptt_in = channel ? !!channel->txPttIn : 0U,
		.begin_turnoff = !!begin_turnoff,
	};
	struct rptadv_radio_dcs_turnoff_state state;

	if (finish_requested)
		*finish_requested = 0;
	if (finish_elapsed_ms)
		*finish_elapsed_ms = 0;
	if (!channel || !finish_requested || !finish_elapsed_ms || elapsed_ms < 0 ||
	    channel->dcsTurnoffDuration <= 0)
		return -1;
	if (begin_turnoff) {
		if (channel->txState != CHAN_TXSTATE_ACTIVE || channel->txPttIn ||
		    !channel->dcs.enabled_transmit || !channel->dcsTurnoffEnabled)
			return -1;
	} else if (channel->txState != CHAN_TXSTATE_TOC || channel->dcsTurnoffTimer <= 0) {
		return -1;
	}
	state.tx_state = channel->txState;
	state.dcs_turnoff_remaining_ms = channel->dcsTurnoffTimer;
	state.tx_hang_remaining_ms = channel->txHangTime;
	state.finish_requested = 0U;
	state.finish_elapsed_ms = 0;
	if (urp_radio_core_dcs_turnoff_advance(&config, &input, &state) ||
	    (state.tx_state != CHAN_TXSTATE_ACTIVE && state.tx_state != CHAN_TXSTATE_TOC) ||
	    state.dcs_turnoff_remaining_ms < 0 || state.finish_requested > 1U ||
	    state.finish_elapsed_ms < 0 || state.finish_elapsed_ms > elapsed_ms ||
	    (!state.finish_requested && state.finish_elapsed_ms != 0) ||
	    (begin_turnoff &&
	     (state.tx_state != CHAN_TXSTATE_TOC ||
	      state.dcs_turnoff_remaining_ms > config.turnoff_duration_ms ||
	      state.tx_hang_remaining_ms != 0 ||
	      (state.finish_requested != (state.dcs_turnoff_remaining_ms == 0)))) ||
	    (!begin_turnoff && input.tx_ptt_in &&
	     (state.tx_state != CHAN_TXSTATE_ACTIVE || state.dcs_turnoff_remaining_ms != 0 ||
	      state.tx_hang_remaining_ms != prior_tx_hang_ms || state.finish_requested != 0U ||
	      state.finish_elapsed_ms != 0)) ||
	    (!begin_turnoff && !input.tx_ptt_in &&
	     (state.tx_state != CHAN_TXSTATE_TOC ||
	      state.dcs_turnoff_remaining_ms > prior_dcs_turnoff_ms ||
	      state.tx_hang_remaining_ms != prior_tx_hang_ms ||
	      (state.finish_requested != (state.dcs_turnoff_remaining_ms == 0)))))
		return -1;
	channel->txState = (i16)state.tx_state;
	channel->dcsTurnoffTimer = state.dcs_turnoff_remaining_ms;
	channel->txHangTime = state.tx_hang_remaining_ms;
	*finish_requested = (int)state.finish_requested;
	*finish_elapsed_ms = state.finish_elapsed_ms;
	return 0;
}

void urp_radio_arm_txrx_blanking(urp_radio_state *pChan)
{
	if (!pChan)
		return;
	pChan->txrxblankingtimer = pChan->txrxblankingtime;
	pChan->txrxBlankingSampleRemainder = 0U;
}

/** @brief Try the portable scalar cleanup after a completed transmitter drain.
 * @param channel Legacy transmitter state to copy at the compatibility boundary.
 * @return Zero only after a fully validated portable result is committed.
 *
 * Display-string clearing, CTCSS/DCS waveform state, and device PTT remain in
 * this compatibility unit. The shared object owns only the six scalar writes
 * made by the historical completion branch, so a missing or malformed append
 * member can safely leave this state unchanged for the exact C fallback.
 */
static int urp_radio_complete_tx_portable(urp_radio_state *channel)
{
	const struct rptadv_radio_tx_complete_config config = {
		.struct_size = sizeof(struct rptadv_radio_tx_complete_config),
		.txrx_blanking_time_ms = channel ? channel->txrxblankingtime : 0,
	};
	struct rptadv_radio_tx_complete_state state;

	if (!channel)
		return -1;
	state.tx_state = channel->txState;
	state.tx_ptt_out = !!channel->txPttOut;
	state.tx_ctcss_option = channel->txCtcssOption;
	state.txrx_blanking_timer_ms = channel->txrxblankingtimer;
	state.txrx_blanking_sample_remainder = channel->txrxBlankingSampleRemainder;
	state.tx_ctcss_ready = !!channel->b.txCtcssReady;
	if (urp_radio_core_tx_complete(&config, &state) ||
	    state.tx_state != RPTADV_RADIO_TX_STATE_IDLE || state.tx_ptt_out != 0U ||
	    state.tx_ctcss_option != RPTADV_RADIO_CTCSS_RENDER_OPTION_DISABLE ||
	    state.txrx_blanking_timer_ms != config.txrx_blanking_time_ms ||
	    state.txrx_blanking_sample_remainder != 0U || state.tx_ctcss_ready != 1U)
		return -1;
	channel->txPttOut = (i16)state.tx_ptt_out;
	channel->txCtcssOption = (i8)state.tx_ctcss_option;
	channel->txrxblankingtimer = (i16)state.txrx_blanking_timer_ms;
	channel->txrxBlankingSampleRemainder = state.txrx_blanking_sample_remainder;
	channel->txState = (i16)state.tx_state;
	channel->b.txCtcssReady = state.tx_ctcss_ready;
	return 0;
}

/** @brief Try the portable normal transmitter finishing-drain transition.
 * @param channel Legacy transmitter state to copy at the compatibility boundary.
 * @param elapsed_ms Whole native PCM milliseconds in the entry callback.
 * @return Zero only after a fully validated portable result is committed.
 *
 * The operation covers only the normal three-buffer 80 ms drain.  Special
 * CTCSS-tail drains retain their existing compatibility path, and an older
 * shared object or invalid response leaves the state untouched for C fallback.
 */
static int urp_radio_enter_finishing_portable(urp_radio_state *channel, i32 elapsed_ms)
{
	const struct rptadv_radio_tx_finish_input input = {
		.elapsed_ms = elapsed_ms,
	};
	struct rptadv_radio_tx_finish_state state;

	if (!channel || elapsed_ms < 0)
		return -1;
	state.buffer_clear_frames = channel->txBufferClear;
	state.finish_remaining_ms = channel->txFinishTimer;
	state.tx_state = channel->txState;
	if (urp_radio_core_tx_finish_advance(&input, &state) ||
	    (state.tx_state != RPTADV_RADIO_TX_STATE_FINISHING &&
	     state.tx_state != RPTADV_RADIO_TX_STATE_COMPLETE) ||
	    state.buffer_clear_frames < 0 || state.buffer_clear_frames > 3 ||
	    state.finish_remaining_ms < 0 || state.finish_remaining_ms > (3 + 1) * MS_PER_FRAME)
		return -1;
	channel->txBufferClear = (i16)state.buffer_clear_frames;
	channel->txFinishTimer = state.finish_remaining_ms;
	channel->txState = (i16)state.tx_state;
	return channel->txState == CHAN_TXSTATE_COMPLETE;
}

/** @brief Enter the fixed transmitter drain state at a precise native time.
 * @param channel Radio state owning transmitter drain timing.
 * @param elapsed_ms Elapsed milliseconds in the transition callback.
 * @return Nonzero when the supplied span also completes the drain.
 */
static int urp_radio_enter_finishing(urp_radio_state *channel, i32 elapsed_ms)
{
	const int portable_result = urp_radio_enter_finishing_portable(channel, elapsed_ms);

	if (portable_result >= 0)
		return portable_result;
	channel->txBufferClear = 3;
	/* The legacy frame counter held PTT through the transition callback and
	 * three further 20 ms drain spans. Preserve that real-time dwell even when
	 * the adapter partitions a native callback into smaller spans. */
	channel->txFinishTimer = (channel->txBufferClear + 1) * MS_PER_FRAME;
	channel->txState = CHAN_TXSTATE_FINISHING;
	(void)urp_radio_timer_consume(&channel->txFinishTimer, elapsed_ms);
	if (channel->txFinishTimer == 0) {
		channel->txBufferClear = 0;
		channel->txState = CHAN_TXSTATE_COMPLETE;
		return 1;
	}
	return 0;
}

/** @brief Try the portable continuation of a transmitter finishing drain.
 * @param channel Legacy transmitter state to copy at the compatibility boundary.
 * @param elapsed_ms Whole native PCM milliseconds in this callback span.
 * @return Zero or one after a portable commit, otherwise negative for C fallback.
 *
 * The portable operation is deliberately limited to normal and 55 Hz-tail
 * compatibility counts. An unusual restored count remains in the retained C
 * code, which preserves its historical behavior without widening this ABI.
 */
static int urp_radio_continue_finishing_portable(urp_radio_state *channel, i32 elapsed_ms)
{
	const struct rptadv_radio_tx_finish_input input = {
		.elapsed_ms = elapsed_ms,
	};
	struct rptadv_radio_tx_finish_state state;

	if (!channel || elapsed_ms < 0 || channel->txState != CHAN_TXSTATE_FINISHING ||
	    channel->txBufferClear < 0 || channel->txBufferClear > 8 || channel->txFinishTimer < 0)
		return -1;
	state.buffer_clear_frames = channel->txBufferClear;
	state.finish_remaining_ms = channel->txFinishTimer;
	state.tx_state = channel->txState;
	if (urp_radio_core_tx_finish_continue(&input, &state) ||
	    (state.tx_state != RPTADV_RADIO_TX_STATE_FINISHING &&
	     state.tx_state != RPTADV_RADIO_TX_STATE_COMPLETE) ||
	    state.buffer_clear_frames < 0 || state.buffer_clear_frames > 8 ||
	    state.finish_remaining_ms < 0 || state.finish_remaining_ms > 8 * MS_PER_FRAME)
		return -1;
	channel->txBufferClear = (i16)state.buffer_clear_frames;
	channel->txFinishTimer = state.finish_remaining_ms;
	channel->txState = (i16)state.tx_state;
	return channel->txState == CHAN_TXSTATE_COMPLETE;
}

i16 urp_radio_process_native_timed(urp_radio_state *pChan, i16 *input, i16 *outputrx, i16 *outputtx,
				   size_t native_frame_count, int advance_tx)
{
	int i, hit;
	int decoded_ctcss = CTCSS_NULL;
	i16 active_samples;
	i32 rx_elapsed_ms;
	i32 tx_elapsed_ms;
	i32 tx_remaining_ms;
	size_t blank_native_frames;
	size_t native_capacity;
	u32 blank_timer_remainder_before;
	u32 rx_cpu_saver_action = RPTADV_RADIO_RX_CPU_SAVER_ACTION_NONE;
	u32 rx_cpu_saver_halted = 0U;
	float f = 0;
	urp_radio_stage *pmr_sps;

	if (pChan == NULL || input == NULL || !native_frame_count || pChan->nSamplesRx <= 0 ||
	    !pChan->rxBaseCapacity || !pChan->spsRx || !pChan->spsRxOut) {
		return 1;
	}
	native_capacity = (size_t)pChan->nSamplesRx * (SAMPLE_RATE_INPUT / SAMPLE_RATE_NETWORK);
	if (native_frame_count > native_capacity) {
		return 1;
	}
	rx_elapsed_ms = urp_radio_elapsed_ms(&pChan->rxTimerSampleRemainder, native_frame_count);

#if URP_RADIO_DEBUG == 1
	if (pChan->tracetype) {
		memset((void *)pChan->sdbg->buffer, 0,
		       (size_t)pChan->nSamplesRx * URP_RADIO_DEBUG_CHANNELS * 2U);
	}
#endif
	if (pChan->rxCtcss)
		decoded_ctcss = pChan->rxCtcss->decode;

	pmr_sps = pChan->spsRx; /* first sps */
	pmr_sps->source = input;

	if (outputrx != NULL) {
		pChan->spsRxOut->sink = outputrx; /* last sps */
	}

	if (pChan->txrxblankingtimer > 0) {
		i32 blank_elapsed_ms;

		blank_timer_remainder_before = pChan->txrxBlankingSampleRemainder;
		blank_elapsed_ms = urp_radio_elapsed_ms(&pChan->txrxBlankingSampleRemainder,
							native_frame_count);
		/* Blank only the protected prefix.  The frontend consumes the left
		 * interleaved sample for every native frame, so clearing contiguous
		 * shorts would both miss half the detector input and make the protected
		 * duration depend on how an adapter partitions callbacks. */
		/* The millisecond timer expires after the pre-call fractional remainder.
		 * Subtract that remainder before clamping so adjacent sub-millisecond
		 * callbacks blank one continuous physical interval, not one rounded
		 * interval per callback. */
		if (urp_radio_rx_blanking_portable(pChan, blank_elapsed_ms,
						   blank_timer_remainder_before, native_frame_count,
						   &blank_native_frames)) {
			blank_native_frames =
				(size_t)pChan->txrxblankingtimer * (SAMPLE_RATE_INPUT / 1000U);
			if (blank_native_frames > (size_t)blank_timer_remainder_before)
				blank_native_frames -= (size_t)blank_timer_remainder_before;
			else
				blank_native_frames = 0U;
			if (blank_native_frames > native_frame_count)
				blank_native_frames = native_frame_count;

			if (blank_elapsed_ms >= pChan->txrxblankingtimer)
				pChan->txrxblankingtimer = 0;
			else
				pChan->txrxblankingtimer -= (i16)blank_elapsed_ms;
		}
		for (i = 0; i < (int)blank_native_frames; ++i)
			input[i * 2] = 0;
	}

	/* The core chooses only a scalar transition. The established C stage writes
	 * remain immediately before the frontend, which keeps the first wake-up
	 * callback and an unavailable-core fallback behavior-identical. */
	if (urp_radio_rx_cpu_saver_portable(pChan, &rx_cpu_saver_action, &rx_cpu_saver_halted)) {
		if (pChan->rxCpuSaver && !pChan->rxCarrierDetect && pChan->smode == SMODE_NULL &&
		    !pChan->txPttIn && !pChan->txPttOut) {
			if (!pChan->b.rxhalted) {
				pChan->spsRxHpf->enabled = 0;
				if (pChan->rxDeEmpEnable) {
					pChan->spsRxDeEmp->enabled = 0;
				}

				pChan->b.rxhalted = 1;
			}
		} else if (pChan->b.rxhalted) {
			pChan->spsRxHpf->enabled = 1;
			if (pChan->rxDeEmpEnable) {
				pChan->spsRxDeEmp->enabled = 1;
			}

			pChan->b.rxhalted = 0;
		}
	} else if (rx_cpu_saver_action == RPTADV_RADIO_RX_CPU_SAVER_ACTION_ENTER) {
		pChan->spsRxHpf->enabled = 0;
		if (pChan->rxDeEmpEnable) {
			pChan->spsRxDeEmp->enabled = 0;
		}
		pChan->b.rxhalted = rx_cpu_saver_halted;
	} else if (rx_cpu_saver_action == RPTADV_RADIO_RX_CPU_SAVER_ACTION_LEAVE) {
		pChan->spsRxHpf->enabled = 1;
		if (pChan->rxDeEmpEnable) {
			pChan->spsRxDeEmp->enabled = 1;
		}
		pChan->b.rxhalted = rx_cpu_saver_halted;
	}

	/* The frontend is the native-rate boundary.  It consumes every supplied
	 * hardware frame, persists its decimator phase, and replaces nSamples with
	 * the precise number of 8 kHz samples it emitted. */
	pmr_sps->nativeSamples = (u32)native_frame_count;
	if (pmr_sps->sigProc(pmr_sps)) {
		return 1;
	}
	active_samples = pmr_sps->nSamples;
	if (active_samples < 0 || (u32)active_samples > pChan->rxBaseCapacity) {
		return 1;
	}
	urp_radio_set_active_samples(pChan, active_samples);

	/* Filters, squelch delay, VOX, calibration, and CTCSS operate at 8 kHz;
	 * an incomplete native decimation interval has no sample to give them. */
	for (pmr_sps = (urp_radio_stage *)pmr_sps->nextSps; pmr_sps != NULL;
	     pmr_sps = (urp_radio_stage *)pmr_sps->nextSps) {
		if (active_samples > 0)
			pmr_sps->sigProc(pmr_sps);
	}

	if (pChan->rxCdType == CD_XPMR_VOX) {
		if (urp_radio_vox_carrier_portable(pChan, rx_elapsed_ms)) {
			if (pChan->spsRxVox->compOut) {
				pChan->rxVoxTimer = pChan->voxHangTime; /* VOX HangTime in ms */
			}
			if (pChan->rxVoxTimer > 0) {
				urp_radio_timer_advance(&pChan->rxVoxTimer, rx_elapsed_ms);
				pChan->rxCarrierDetect = 1;
			} else {
				pChan->rxVoxTimer = 0;
				pChan->rxCarrierDetect = 0;
			}
		}
	} else {
		pChan->rxCarrierDetect = !pChan->spsRx->compOut;
		if (pChan->rxSquelchDelay) {
			pChan->spsRxSquelchDelay->b.outzero = pChan->spsRx->compOut;
		}
	}

	/* The stream-owned Rust receiver supplies the DCS decision at this same
	 * post-blanking point. No second decoder accumulates competing history. */
	if (pChan->dcs.enabled_receive)
		(void)urp_dcs_process(&pChan->dcs, input, native_frame_count, 2U,
				      SAMPLE_RATE_INPUT);

	/* stop and start these engines instead to eliminate falsing */
	if (active_samples > 0 && pChan->b.ctcssRxEnable && pChan->rxCtcss &&
	    (!pChan->b.rxhalted || pChan->rxCtcss->decode != CTCSS_NULL)) {
		urp_ctcss_decode(pChan);
	}
	/* A reload can retire the optional decoder while this callback retains the
	 * signaling state. Treat that transient exactly like no decoded tone so COR,
	 * DCS, timers, and transmitter state continue to advance. */
	if (pChan->rxCtcss)
		decoded_ctcss = pChan->rxCtcss->decode;

	if (urp_radio_signal_mode_portable(pChan, rx_elapsed_ms, decoded_ctcss)) {
		/* An older or rejected shared core takes the exact established path. */
		if (pChan->smodetimer > 0 && !pChan->txPttIn) {
			urp_radio_timer_advance(&pChan->smodetimer, rx_elapsed_ms);

			if (pChan->smodetimer == 0) {
				pChan->smodewas = pChan->smode;
				pChan->smode = SMODE_NULL;
				pChan->b.smodeturnoff = 1;
			}
		}

		if (decoded_ctcss > CTCSS_NULL &&
		    (pChan->smode == SMODE_NULL || pChan->smode == SMODE_CTCSS)) {
			if (pChan->smode != SMODE_CTCSS) {
				pChan->smode = pChan->smodewas = SMODE_CTCSS;
			}
			pChan->smodetimer = pChan->smodetime;
		}
		if (pChan->smode == SMODE_CTCSS && pChan->b.ctcssTxEnable) {
			if (decoded_ctcss != pChan->lastrxdecode) {
				pChan->lastrxdecode = decoded_ctcss;
				f = 0;
				if (decoded_ctcss > CTCSS_NULL) {
					if (pChan->rxCtcssMap[decoded_ctcss] != CTCSS_RXONLY) {
						f = freq_ctcss[pChan->rxCtcssMap[decoded_ctcss]];
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
		if (pChan->dcs.valid && (pChan->smode == SMODE_NULL || pChan->smode == SMODE_DCS)) {
			pChan->smode = pChan->smodewas = SMODE_DCS;
			pChan->smodetimer = pChan->smodetime;
		}
	}
	/* The receiver still owns this captured frame, but transmitter timing must
	 * not advance unless the matching native DAC frame will be rendered. */
	if (!advance_tx) {
		if (outputtx)
			memset(outputtx, 0, native_frame_count * 2U * sizeof(*outputtx));
		return 0;
	}
	tx_elapsed_ms = urp_radio_elapsed_ms(&pChan->txTimerSampleRemainder, native_frame_count);
	/* handle radio transmitter ptt input */
	hit = 0;
	{
		if (pChan->txPttIn && (pChan->txState == CHAN_TXSTATE_IDLE)) {
			pChan->txCtcssFreq10 = 0;
			/* Transmit CTCSS is selected by the transmit direction. A received
			 * CTCSS tone may select a mapped TX tone, but received DCS or carrier
			 * must not suppress the configured transmit default. */
			if (pChan->b.ctcssTxEnable && !pChan->b.txCtcssInhibit) {
				if (pChan->smode == SMODE_CTCSS && decoded_ctcss > CTCSS_NULL &&
				    pChan->rxCtcssMap[decoded_ctcss] != CTCSS_RXONLY)
					f = freq_ctcss[pChan->rxCtcssMap[decoded_ctcss]];
				else if (pChan->smode != SMODE_CTCSS || decoded_ctcss == CTCSS_NULL)
					f = pChan->txctcssdefault_value;
				if (f) {
					pChan->txCtcssFreq10 = f * 10;
					pChan->txCtcssOption = 1;
					pChan->txCtcssEnabled = 1;
					pChan->txCtcssTurnoffTimer = 0;
				}
			}
			/* DCS has its own fixed code configuration and does not participate in
			 * CTCSS frequency selection or mapping. */
			if (pChan->dcs.enabled_transmit)
				pChan->dcsTurnoffTimer = 0;

			memset(pChan->txctcssfreq, 0, sizeof(pChan->txctcssfreq));
			sprintf(pChan->txctcssfreq, "%.1f", f);
			pChan->b.txCtcssReady = 1;

			pChan->txState = CHAN_TXSTATE_ACTIVE;
			pChan->txPttOut = 1;

			pChan->txsettletimer = pChan->txsettletime;
		} else if (pChan->txPttIn && pChan->txState == CHAN_TXSTATE_ACTIVE) {
			pChan->smodetimer = pChan->smodetime;
		} else if (!pChan->txPttIn && pChan->txState == CHAN_TXSTATE_ACTIVE) {
			if (pChan->dcs.enabled_transmit && pChan->dcsTurnoffEnabled) {
				int finish_requested;

				if (urp_radio_dcs_turnoff_portable(pChan, tx_elapsed_ms, 1,
								   &finish_requested,
								   &tx_remaining_ms)) {
					pChan->txState = CHAN_TXSTATE_TOC;
					pChan->dcsTurnoffTimer = pChan->dcsTurnoffDuration;
					pChan->txHangTime = 0;
					tx_remaining_ms = urp_radio_timer_consume(
						&pChan->dcsTurnoffTimer, tx_elapsed_ms);
					if (pChan->dcsTurnoffTimer == 0)
						hit = urp_radio_enter_finishing(pChan,
										tx_remaining_ms);
				} else if (finish_requested) {
					hit = urp_radio_enter_finishing(pChan, tx_remaining_ms);
				}
			} else if (pChan->txCtcssEnabled && !pChan->b.txCtcssInhibit) {
				if (pChan->txTocType == TOC_NONE || !pChan->b.ctcssTxEnable) {
					pChan->txCtcssOption = 3;
					hit = urp_radio_enter_finishing(pChan, tx_elapsed_ms);
				} else if (pChan->txTocType == TOC_NOTONE) {
					pChan->txState = CHAN_TXSTATE_TOC;
					pChan->txHangTime = pChan->txCtcssTocTime;
					pChan->txCtcssOption = 3;
					(void)urp_radio_timer_consume(&pChan->txHangTime,
								      tx_elapsed_ms);
					if (pChan->txHangTime == 0) {
						pChan->txState = CHAN_TXSTATE_FINISHING;
						pChan->txBufferClear = 0;
						pChan->txFinishTimer = 0;
						hit = 1;
					}
				} else {
					pChan->txState = CHAN_TXSTATE_TOC;
					pChan->txHangTime = 0;
					pChan->txCtcssOption = 2;
					if (pChan->txTocType == 3) {
						pChan->txCtcssTocShift = 0.0;
					} else {
						pChan->txCtcssTocToneHz = 0.0;
					}
				}
			} else {
				hit = urp_radio_enter_finishing(pChan, tx_elapsed_ms);
			}
		} else if (pChan->txState == CHAN_TXSTATE_TOC) {
			if (pChan->txPttIn && pChan->dcsTurnoffTimer > 0) {
				int finish_requested;

				if (urp_radio_dcs_turnoff_portable(pChan, tx_elapsed_ms, 0,
								   &finish_requested,
								   &tx_remaining_ms)) {
					/* Resume normal DCS immediately; do not finish an obsolete
					 * tail. */
					pChan->dcsTurnoffTimer = 0;
					pChan->txState = CHAN_TXSTATE_ACTIVE;
					hit = 0;
				} else if (finish_requested) {
					hit = urp_radio_enter_finishing(pChan, tx_remaining_ms);
				}
			} else if (pChan->txPttIn && pChan->b.ctcssTxEnable) {
				/* A no-tone tail clears the emitted tone, not the configured
				 * transmit CTCSS selection. Rekeying during that tail restores it.
				 */
				pChan->txState = CHAN_TXSTATE_ACTIVE;
				pChan->txCtcssOption = 1;
				pChan->txCtcssEnabled = 1;
				pChan->txCtcssTurnoffTimer = 0;
				hit = 0;
			} else if (pChan->txHangTime > 0) {
				(void)urp_radio_timer_consume(&pChan->txHangTime, tx_elapsed_ms);
				if (pChan->txHangTime == 0) {
					pChan->txState = CHAN_TXSTATE_FINISHING;
					pChan->txBufferClear = 0;
					pChan->txFinishTimer = 0;
					hit = 1;
				}
			} else if (pChan->dcsTurnoffTimer > 0) {
				int finish_requested;

				if (urp_radio_dcs_turnoff_portable(pChan, tx_elapsed_ms, 0,
								   &finish_requested,
								   &tx_remaining_ms)) {
					tx_remaining_ms = urp_radio_timer_consume(
						&pChan->dcsTurnoffTimer, tx_elapsed_ms);
					if (pChan->dcsTurnoffTimer == 0)
						hit = urp_radio_enter_finishing(pChan,
										tx_remaining_ms);
				} else if (finish_requested) {
					hit = urp_radio_enter_finishing(pChan, tx_remaining_ms);
				}
			} else if (pChan->txCtcssState == 0) {
				/* A 55 Hz tail needs ten post-tone frames: two TOC frames
				 * plus these eight finishing frames keep PTT high for 200 ms. */
				pChan->txBufferClear = pChan->txTocType == 3 ? 8 : 3;
				pChan->txFinishTimer = (pChan->txBufferClear + 1) * MS_PER_FRAME;
				pChan->txState = CHAN_TXSTATE_FINISHING;
				(void)urp_radio_timer_consume(&pChan->txFinishTimer, tx_elapsed_ms);
				if (pChan->txFinishTimer == 0) {
					pChan->txBufferClear = 0;
					pChan->txState = CHAN_TXSTATE_COMPLETE;
					hit = 1;
				}
			}
		} else if (pChan->txState == CHAN_TXSTATE_FINISHING) {
			const int portable_result =
				urp_radio_continue_finishing_portable(pChan, tx_elapsed_ms);

			if (portable_result >= 0) {
				hit = portable_result;
			} else {
				/* Keep externally restored legacy frame counts meaningful while all
				 * normal transitions use the duration-based timer above. */
				if (pChan->txFinishTimer == 0 && pChan->txBufferClear > 0)
					pChan->txFinishTimer = pChan->txBufferClear * MS_PER_FRAME;
				urp_radio_timer_advance(&pChan->txFinishTimer, tx_elapsed_ms);
				if (pChan->txFinishTimer == 0) {
					pChan->txBufferClear = 0;
					pChan->txState = CHAN_TXSTATE_COMPLETE;
					hit = 1;
				}
			}
		} else if (pChan->txState == CHAN_TXSTATE_COMPLETE) {
			hit = 1;
		}
	} /* end of if SMODE==LSD */

	if (hit) {
		if (urp_radio_complete_tx_portable(pChan)) {
			pChan->txPttOut = 0;
			pChan->txCtcssOption = 3;
			urp_radio_arm_txrx_blanking(pChan);
			pChan->txState = CHAN_TXSTATE_IDLE;
			pChan->b.txCtcssReady = 1;
		}

		memset(pChan->txctcssfreq, 0, sizeof(pChan->txctcssfreq));
	}

	if (pChan->txsettletimer && pChan->txPttHid) {
		urp_radio_timer_advance(&pChan->txsettletimer, tx_elapsed_ms);
	}

	/* enable this after we know everything else is working */
	if (urp_radio_tx_cpu_saver_portable(pChan)) {
		if (pChan->txCpuSaver && !pChan->txPttIn && !pChan->txPttOut &&
		    pChan->txState == CHAN_TXSTATE_IDLE) {
			if (!pChan->b.txhalted) {
				pChan->b.txhalted = 1;
			}
		} else if (pChan->b.txhalted) {
			pChan->b.txhalted = 0;
		}
	}

	if (pChan->b.txhalted) {
		return 1;
	}

	/* Preserve established CTCSS start and squelch-tail timing while the channel
	 * driver renders the waveform at the CM119's native sample rate. */
	if (urp_radio_ctcss_render_state_portable(pChan, tx_elapsed_ms)) {
		/* An older or rejected shared core retains the established exact branch. */
		pChan->txCtcssPhaseShift = 0;
		if (pChan->txCtcssOption == 1) {
			pChan->txCtcssOption = 0;
			pChan->txCtcssState = 1;
			pChan->txCtcssTailToneHz = 0.0;
		} else if (pChan->txCtcssOption == 2) {
			pChan->txCtcssOption = 0;
			pChan->txCtcssState = 2;
			pChan->txCtcssTurnoffTimer = pChan->txCtcssTocTime - tx_elapsed_ms;
			if (pChan->txCtcssTurnoffTimer < 0)
				pChan->txCtcssTurnoffTimer = 0;
			pChan->txCtcssPhaseShift = pChan->txCtcssTocShift;
			pChan->txCtcssTailToneHz = pChan->txCtcssTocToneHz;
		} else if (pChan->txCtcssOption == 3) {
			pChan->txCtcssOption = 0;
			pChan->txCtcssState = 0;
			pChan->txCtcssEnabled = 0;
			pChan->txCtcssTailToneHz = 0.0;
		} else if (pChan->txCtcssState == 2) {
			urp_radio_timer_advance(&pChan->txCtcssTurnoffTimer, tx_elapsed_ms);
			if (pChan->txCtcssTurnoffTimer == 0)
				pChan->txCtcssOption = 3;
		}
	}

	/* This engine controls signaling and PTT only; USBRadioPlus renders audio. */
	if (outputtx)
		memset(outputtx, 0, native_frame_count * 2U * sizeof(*outputtx));

#if URP_RADIO_DEBUG == 1
	if (pChan->tracetype) {
		/* The historical trace workspace stores one legacy 160-sample frame.
		 * A carried decimator phase may emit one extra baseband sample, which
		 * remains processed but is intentionally outside this compatibility trace. */
		for (i = 0; i < pChan->activeSamplesRx && i < SAMPLES_PER_BLOCK; i++) {
			pChan->pRxDemod[i] = input[i * 2 * 6];
			TSCOPE((RX_NOISE_TRIG, pChan->sdbg, i,
				(pChan->rxCarrierDetect * URP_RADIO_TRACE_AMP) -
					URP_RADIO_TRACE_AMP / 2));
			TSCOPE((RX_CTCSS_DECODE, pChan->sdbg, i,
				decoded_ctcss * (M_Q14 / CTCSS_NUM_CODES)));
			TSCOPE((RX_SMODE, pChan->sdbg, i,
				pChan->smode * (URP_RADIO_TRACE_AMP / 4)));
			TSCOPE((TX_PTT_IN, pChan->sdbg, i,
				(pChan->txPttIn * URP_RADIO_TRACE_AMP) - URP_RADIO_TRACE_AMP / 2));
			TSCOPE((TX_PTT_OUT, pChan->sdbg, i,
				(pChan->txPttOut * URP_RADIO_TRACE_AMP) - URP_RADIO_TRACE_AMP / 2));
		}
	}
#endif

	strace2(pChan->sdbg, pChan->activeSamplesRx);
	return 0;
}

#if GCC_VERSION > 40600
#pragma GCC diagnostic pop
#endif

/* end of file */

/** @name File-local and build-time constants
 * @{ */
/** @def GCC_VERSION
 * @brief Compiler version encoded for feature selection.
 */
/** @def N_FMT
 * @brief Generate a numeric-setting format fragment.
 */
/** @def DCgainBpfNoise
 * @brief Noise-detector filter DC-gain normalization.
 */
/** @def ALLOCATE_OR_FAIL
 * @brief Allocate stage storage and jump to cleanup if allocation fails.
 */
/** @} */
