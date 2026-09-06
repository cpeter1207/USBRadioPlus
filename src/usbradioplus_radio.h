/*
 * usbradioplus_radio.h - native radio detection and signaling
 *
 * All Rights Reserved. Copyright (C)2007, Xelatec, LLC
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
 *
 * 20160829      inad            added rxlpf rxhpf txlpf txhpf
 */

/*!
 * \file
 *
 * \brief Private Land Mobile Radio Channel Voice and Signaling Processor
 *
 * \author Steven Henke, W9SH <sph@xelatec.com> Xelatec, LLC
 */

#ifndef USBRADIOPLUS_RADIO_H
#define USBRADIOPLUS_RADIO_H 1

#include <stdint.h>

#include "asterisk/rpt_chan_shared.h"
#include "usbradioplus_squelch.h"

#define URP_RADIO_DEVELOPMENT 0 /* when running in test mode */

#define URP_RADIO_TRACE_OVFLW 0

#define URP_RADIO_TRACE_FRONTEND 0

#define URP_RADIO_TRACE_LEVEL 0

#ifdef CHAN_USBRADIO
#ifndef URP_RADIO_DEBUG

#define URP_RADIO_DEBUG 1
#endif
#ifndef URP_RADIO_TRACE

#define URP_RADIO_TRACE 1
#endif

#define TRACEO(level, a)                                                                           \
	{                                                                                          \
		if (o && (o->tracelevel >= level)) {                                               \
			printf a;                                                                  \
		}                                                                                  \
	}
#else
#ifndef URP_RADIO_DEBUG
#define URP_RADIO_DEBUG 1
#endif
#ifndef URP_RADIO_TRACE
#define URP_RADIO_TRACE 1
#endif
#define TRACEO(level, a)
#endif

#define LSD_DFS 5

#define LSD_DFD 1

#if (URP_RADIO_DEBUG == 1)

#define URP_RADIO_DEBUG_CHANNELS 16

#define TSCOPE(a)                                                                                  \
	{                                                                                          \
		strace a;                                                                          \
	}
#else
#define URP_RADIO_DEBUG_CHANNELS 0
#define TSCOPE(a)
#endif

#define URP_RADIO_TRACE_AMP 8192

#if (URP_RADIO_TRACE == 1)

#define TRACEC(level, ...)                                                                         \
	{                                                                                          \
		urp_radio_trace_log(pChan->tracelevel, level, "%08i ", pChan->frameCountRx);       \
		urp_radio_trace_log(pChan->tracelevel, level, __VA_ARGS__);                        \
	}

#define TRACEF(level, ...)                                                                         \
	{                                                                                          \
		urp_radio_trace_log(pChan->tracelevel, level, __VA_ARGS__);                        \
	}
#else
#define TRACEC(...)
#define TRACEF(...)
#endif

#define TRACEJ(...)

#define i8 int8_t

#define u8 uint8_t

#define i16 int16_t

#define u16 uint16_t

#define i32 int32_t

#define u32 uint32_t

#define i64 int64_t

#define u64 uint64_t

#define M_Q31 0x80000000

#define M_Q30 0x40000000

#define M_Q29 0x20000000

#define M_Q28 0x10000000

#define M_Q27 0x08000000

#define M_Q26 0x04000000

#define M_Q25 0x02000000

#define M_Q24 0x01000000

#define M_Q23 0x00800000

#define M_Q22 0x00400000

#define M_Q21 0x00200000 /* undsoweiter */

#define M_Q20 0x00100000 /* 1048576 */

#define M_Q19 0x00080000 /* 524288 */

#define M_Q18 0x00040000 /* 262144 */

#define M_Q17 0x00020000 /* 131072 */

#define M_Q16 0x00010000 /* 65536 */

#define M_Q15 0x00008000 /* 32768 */

#define M_Q14 0x00004000 /* 16384 */

#define M_Q13 0x00002000 /* 8182 */

#define M_Q12 0x00001000 /* 4096 */

#define M_Q11 0x00000800 /* 2048 */

#define M_Q10 0x00000400 /* 1024 */

#define M_Q9 0x00000200 /* 512 */

#define M_Q8 0x00000100 /* 256 */

#define M_Q7 0x00000080 /* 128 */

#define M_Q6 0x00000040 /* 64 */

#define M_Q5 0x00000020 /* 32 */

#define M_Q4 0x00000010 /* 16 */

#define M_Q3 0x00000008 /* 8 */

#define M_Q2 0x00000004 /* 4 */

#define M_Q1 0x00000002 /* 2 */

#define M_Q0 0x00000001 /* 1 */

#define RADIANS_PER_CYCLE (2 * M_PI)

#define SAMPLE_RATE_INPUT 48000

#define SAMPLE_RATE_NETWORK 8000

#define SAMPLES_PER_BLOCK 160

#define MS_PER_FRAME 20

#define SAMPLES_PER_MS 8

#define RXSQDELAYBUFSIZE 4096

#define CTCSS_NULL -1

#define CTCSS_RXONLY -2

#define CTCSS_NUM_CODES 38 /* 0 - 37 */

#define CTCSS_SCOUNT_MUL 100

#define CTCSS_INTEGRATE 3932 /* 32767*.120 -> 120/1000 = 0.120 */

#define CTCSS_INPUT_LIMIT 1000

#define CTCSS_DETECT_POINT 1989

#define CTCSS_HYSTERSIS 200

#define CTCSS_TURN_OFF_TIME 160 /* ms */

#define CTCSS_TURN_OFF_SHIFT 240 /* degrees */

#define TOC_NOTONE_TIME 600 /* ms */

#define DDB_FRAME_SIZE 160 /* clock de-drift defaults */

#define DDB_FRAMES_IN_BUFF 8

#define DDB_ERR_MODULUS 10000

#define CHAN_TXSTATE_IDLE 0

#define CHAN_TXSTATE_ACTIVE 1

#define CHAN_TXSTATE_TOC 2

#define CHAN_TXSTATE_HANGING 3

#define CHAN_TXSTATE_FINISHING 4

#define CHAN_TXSTATE_COMPLETE 5

#define CHAN_TXSTATE_USURPED 9

#define SMODE_NULL 0

#define SMODE_CARRIER 1

#define SMODE_CTCSS 2

#define SMODE_DCS 3

#define SMODE_LSD 4

#define SMODE_MPT 5

#define SMODE_DST 6

#define SMODE_P25 7

#define SMODE_MDC 8

#define SPS_OPT_START 1

#define SPS_OPT_STOP 2

#define SPS_OPT_TURNOFF 3

#define SPS_OPT_STOPNOW 4

#define SPS_STAT_STOPPED 0

#define SPS_STAT_STARTING 1

#define SPS_STAT_RUNNING 2

#define SPS_STAT_HALTING 3

/** Identifiers for selectable radio diagnostic trace points. */
enum dbg_pts {
	RX_INPUT /**< Receiver INPUT trace point. */,
	RX_NOISE_AMP /**< Receiver NOISE AMP trace point. */,
	RX_NOISE_TRIG /**< Receiver NOISE TRIG trace point. */,
	RX_CTCSS_LPF /**< Receiver CTCSS low-pass filter trace point. */,
	RX_CTCSS_CENTER /**< Receiver CTCSS CENTER trace point. */,
	RX_CTCSS_NRZ /**< Receiver CTCSS NRZ trace point. */,
	RX_CTCSS_CLK /**< Receiver CTCSS CLK trace point. */,
	RX_CTCSS_P0 /**< Receiver CTCSS P0 trace point. */,
	RX_CTCSS_P1 /**< Receiver CTCSS P1 trace point. */,
	RX_CTCSS_ACCUM /**< Receiver CTCSS ACCUM trace point. */,
	RX_CTCSS_DVDT /**< Receiver CTCSS DVDT trace point. */,
	RX_CTCSS_DECODE /**< Receiver CTCSS DECODE trace point. */,
	RX_DCS_CENTER /**< Receiver DCS CENTER trace point. */,
	RX_DCS_DEC /**< Receiver DCS DEC trace point. */,
	RX_DCS_DIN /**< Receiver DCS DIN trace point. */,
	RX_DCS_CLK /**< Receiver DCS CLK trace point. */,
	RX_DCS_DAT /**< Receiver DCS DAT trace point. */,
	RX_LSD_LPF /**< Receiver LSD low-pass filter trace point. */,
	RX_LSD_CLK /**< Receiver LSD CLK trace point. */,
	RX_LSD_DAT /**< Receiver LSD DAT trace point. */,
	RX_LSD_DEC /**< Receiver LSD DEC trace point. */,
	RX_LSD_CENTER /**< Receiver LSD CENTER trace point. */,
	RX_LSD_SYNC /**< Receiver LSD SYNC trace point. */,
	RX_LSD_STATE /**< Receiver LSD STATE trace point. */,
	RX_LSD_ERR /**< Receiver LSD ERR trace point. */,
	RX_LSD_INTE /**< Receiver LSD INTE trace point. */,
	RX_SMODE /**< Receiver SMODE trace point. */,
	TX_PTT_IN /**< Transmitter PTT IN trace point. */,
	TX_PTT_OUT /**< Transmitter PTT OUT trace point. */,
	TX_DEDRIFT_LEAD /**< Transmitter DEDRIFT LEAD trace point. */,
	TX_DEDRIFT_ERR /**< Transmitter DEDRIFT ERR trace point. */,
	TX_DEDRIFT_FACTOR /**< Transmitter DEDRIFT FACTOR trace point. */,
	TX_DEDRIFT_DRIFT /**< Transmitter DEDRIFT DRIFT trace point. */,
	TX_DEDRIFT_TWIDDLE /**< Transmitter DEDRIFT TWIDDLE trace point. */,
	TX_CTCSS_GEN /**< Transmitter CTCSS GEN trace point. */,
	TX_SIGGEN_0 /**< Transmitter SIGGEN 0 trace point. */,
	TX_NET_INT /**< Transmitter NET INT trace point. */,
	TX_VOX_HPF /**< Transmitter VOX high-pass filter trace point. */,
	TX_VOX_LIM /**< Transmitter VOX LIM trace point. */,
	TX_VOX_LPF /**< Transmitter VOX low-pass filter trace point. */,
	TX_OUT_A /**< Transmitter OUT A trace point. */,
	TX_OUT_B /**< Transmitter OUT B trace point. */,
	NUM_DEBUG_PTS /**< Number of available trace points. */
};

/** Selectable radio trace points and their interleaved capture workspace. */
typedef struct {
	/** Selected diagnostic or processing mode. */
	i16 mode;
	/** Trace-point selection for each output. */
	i16 point[NUM_DEBUG_PTS];
	/** Enabled trace channels. */
	i16 trace[16];
	/** Scale applied to each diagnostic trace channel. */
	i16 scale[16];
	/** DC offset applied to each diagnostic trace channel. */
	i16 offset[16];
	/** allocate for rx and tx */
	i16 buffer[16 * SAMPLES_PER_BLOCK]; /* allocate for rx and tx */
	/** Input buffer or source endpoint for this state. */
	i16 *source[16];
} t_sdbg;

/*
	one structure for each ctcss tone to decode
*/
/** Correlator, qualification, and reverse-burst state for one receive CTCSS tone. */
typedef struct {
	/** Countdown to the next correlator sample. */
	i16 counter; /* counter to next sample */
	/** Tone-period divisor used to advance the sample counter. */
	i16 counterFactor; /* full divisor used to increment counter */
	/** Quarter-cycle correlator bin spacing. */
	i16 binFactor;
	/** Tone qualification count adjustment. */
	i16 fudgeFactor;
	/** Largest observed absolute sample magnitude. */
	i16 peak; /* peak amplitude now   maw sph now */
	/** Nonzero enables this channel, stage, or detector. */
	i16 enabled;
	/** Current stage or stream state. */
	i16 state; /* dead, running, error */
	/** Current quadrature correlator bin. */
	i16 zIndex; /* z bucket index */
	/** Correlator history bins. */
	i16 z[4];
	/** Integrated quadrature-bin state. */
	i16 zi;
	/** Positive tone-envelope slope. */
	i16 dvu;
	/** Negative tone-envelope slope. */
	i16 dvd;
	/** Previous correlator difference. */
	i16 zd;
	/** Detector decision threshold. */
	i16 setpt;
	/** Detector release hysteresis. */
	i16 hyst;
	/** Current decoded-tone or detector decision. */
	i16 decode;
	/** Peak difference used to detect reverse burst. */
	i16 diffpeak;
	/** Per-detector debug enable state. */
	i16 debug;

#if URP_RADIO_DEBUG == 1
	/** Previous quadrature correlator value for this tone bin. */
	i16 lasttv0;
	/** Previous quadrature correlator value for this tone bin. */
	i16 lasttv1;
	/** Previous quadrature correlator value for this tone bin. */
	i16 lasttv2;
	/** Previous quadrature correlator value for this tone bin. */
	i16 lasttv3;

	/** pointer to debug output */
	i16 *pDebug0; /* pointer to debug output */
	/** pointer to debug output */
	i16 *pDebug1; /* pointer to debug output */
	/** pointer to debug output */
	i16 *pDebug2; /* pointer to debug output */
	/** pointer to debug output */
	i16 *pDebug3; /* pointer to debug output */
#endif

} urp_ctcss_tone_detector;

/** CTCSS decoder bank and receive-to-transmit tone selection state. */
typedef struct {
	/** Nonzero enables this channel, stage, or detector. */
	i16 enabled; /* if 0 none, 0xFFFF all tones, or single tone */
	/** Borrowed subaudible detector input samples. */
	i16 *input; /* source data */
	/** Tracked subaudible signal amplitude. */
	i16 clamplitude;
	/** Tracked subaudible DC center. */
	i16 center;
	/** Current decoded-tone or detector decision. */
	i16 decode; /* current ctcss decode index */
	/** Remaining decoder blanking interval. */
	i32 BlankingTimer;
	/** Remaining CTCSS turn-off qualification interval. */
	u32 TurnOffTimer;
	/** Subaudible detector gain metadata. */
	i16 gain;
	/** Input amplitude bound used by the detector. */
	i16 limit;
	/** Selected diagnostic trace channel. */
	i16 debugIndex;
	/** P Debug0 diagnostic sample workspace. */
	i16 *pDebug0;
	/** P Debug1 diagnostic sample workspace. */
	i16 *pDebug1;
	/** P Debug2 diagnostic sample workspace. */
	i16 *pDebug2;
	/** P Debug3 diagnostic sample workspace. */
	i16 *pDebug3;
	/** Selected detector test index. */
	i16 testIndex;
	/** Nonzero when multiple receive tones are configured. */
	i16 multiFreq;
	/** CTCSS qualification/talk-off tolerance setting. */
	i8 relax;
	/** One correlator for each supported receive tone. */
	urp_ctcss_tone_detector tdet[CTCSS_NUM_CODES];

	/** Number of configured receive signaling codes. */
	i8 numrxcodes;
	/** Map from detected receive tones to transmit tone selections. */
	i16 rxCtcssMap[CTCSS_NUM_CODES];
	/** Configured receive CTCSS code values. */
	char *rxctcss[CTCSS_NUM_CODES]; /* pointers to each tone in string above */
	/** Configured transmit CTCSS code values. */
	char *txctcss[CTCSS_NUM_CODES];

	/** Default transmit CTCSS table index. */
	i32 txctcssdefault_index;
	/** Default transmit CTCSS frequency in Hz. */
	float txctcssdefault_value;

	struct {
		/** Nonzero when the detector configuration is usable. */
		unsigned valid : 1;
		/** Packed radio-signaling status and enable flags. */
	} b; /**< Packed radio-signaling status and enable flags. */
} urp_ctcss_decoder;

/*
	Low Speed Data
*/
/*
	general purpose pmr signal processing element
*/

struct urp_radio_state;

/** One radio-detection or measurement stage and its owned filter workspace. */
typedef struct urp_radio_stage {
	/** Index assigned to this radio or detector stage. */
	i16 index; /* unique to each instance */

	/** Nonzero enables this channel, stage, or detector. */
	i16 enabled; /* enabled/disabled */

	/** Radio channel that owns this detector stage. */
	struct urp_radio_state *parentChan;
	/** Input buffer or source endpoint for this state. */
	i16 *source; /* source buffer */
	/** Secondary detector input. */
	i16 *sourceB; /* source buffer B */
	/** Output buffer or sink endpoint for this state. */
	i16 *sink; /* sink buffer */

	/** Number of output channels. */
	i16 numChanOut; /* allows output direct to interleaved buffer */
	/** Selected output channel index. */
	i16 selChanOut;

	/** Processing tick counter. */
	i32 ticks;
	/** Stage timing counter. */
	i32 timer;
	/** Number of occupied elements. */
	i32 count;

	/** Owned stage sample workspace. */
	void *buff; /* this structure's internal buffer */

	/** debug buffer */
	i16 *debugBuff0; /* debug buffer */
	/** debug buffer */
	i16 *debugBuff1; /* debug buffer */
	/** debug buffer */
	i16 *debugBuff2; /* debug buffer */
	/** debug buffer */
	i16 *debugBuff3; /* debug buffer */

	/** Samples processed per block. */
	i16 nSamples; /* number of samples in the buffer */

	/** Allocated sample-buffer capacity. */
	u32 buffSize; /* buffer maximum index */
	/** Next input position in the stage buffer. */
	u32 buffInIndex; /* index to current input point */
	/** Next output position in the stage buffer. */
	u32 buffOutIndex; /* index to current output point */
	/** Input/output sample lead within the stage buffer. */
	u32 buffLead; /* lead of input over output through cb */

	/** Input-to-output decimation factor. */
	i16 decimate; /* decimation or interpolation factor (could be put in coef's) */
	/** Output interpolation factor. */
	i16 interpolate;
	/** Current decimation phase. */
	i16 decimator; /* like the state this must be saved between calls (could be put in x's) */

	/** Stage sample rate in Hz. */
	u32 sampleRate; /* in Hz for elements in this structure */
	/** Detector operating frequency. */
	u32 freq; /* in 0.1 Hz */

	/** Most recent peak-to-peak calibration measurement. */
	i16 measPeak; /* do measure Peak */
	/** Tracked positive sample extremum. */
	i16 amax; /* buffer amplitude maximum */
	/** Tracked negative sample extremum. */
	i16 amin; /* buffer amplitude minimum */
	/** Tracked absolute amplitude. */
	i16 apeak; /* buffer amplitude peak value (peak to peak)/2 */
	/** Detector decision threshold. */
	i16 setpt; /* amplitude set point for amplitude comparator */
	/** Detector release hysteresis. */
	i16 hyst; /* hysterysis for amplitude comparator */
	/** Detector comparator output. */
	i16 compOut; /* amplitude comparator output */
	/** Nonzero while detector input is blanked. */
	i16 blanking; /* blanking timer in frames */
	/** Noise-dependent carrier qualification state. */
	struct urp_micor_squelch micor_squelch;

	/** Upper-envelope decay counter. */
	i32 discounteru; /* amplitude detector integrator discharge counter upper */
	/** Lower-envelope decay counter. */
	i32 discounterl; /* amplitude detector integrator discharge counter lower */
	/** Envelope decay factor. */
	i32 discfactor; /* amplitude detector integrator discharge factor */

	/** Accumulated detector error. */
	i16 err; /* error condition */
	/** Requested stage operation. */
	i16 option; /* option / request zero */
	/** Current stage or stream state. */
	i16 state; /* stopped, start, stopped assumes zero'd */

	/** Pending detector transition. */
	i16 pending;

	struct {
		/** Current detector match indicator. */
		unsigned hit : 1;
		/** Previous detector match indicator. */
		unsigned hitlast : 1;
		/** First detector match accumulator. */
		unsigned hita : 1;
		/** Second detector match accumulator. */
		unsigned hitb : 1;
		/** Matched signaling-bit indicator. */
		unsigned bithit : 1;
		/** Current stage timing/sample position. */
		unsigned now : 1;
		/** Next configured radio in the channel list. */
		unsigned next : 1;
		/** Previous stage timing/sample position. */
		unsigned prev : 1;
		/** Detector clock recovery state. */
		unsigned clock : 1;
		/** Detector hold counter. */
		unsigned hold : 1;
		/** First stage-specific option. */
		unsigned opt1 : 1;
		/** Second stage-specific option. */
		unsigned opt2 : 1;
		/** Signal polarity inversion selector. */
		unsigned polarity : 1;
		/** Alternating-bit signaling acquisition state. */
		unsigned dotting : 1;
		/** Nonzero when a final signaling bit is pending. */
		unsigned lastbitpending : 1;
		/** Nonzero requests a zeroed stage output. */
		unsigned outzero : 1;
		/** Nonzero while the detector or transmitter settles. */
		unsigned settling : 1;
		/** Nonzero while signaling synchronization is in progress. */
		unsigned syncing : 1;
		/** Nonzero when stage state needs refreshing. */
		unsigned dirty : 1;
		/** Nonzero requests silent stage output. */
		unsigned mute : 1;
		/** Packed radio-signaling status and enable flags. */
	} b; /**< Packed radio-signaling status and enable flags. */

	/** Nonzero after stage history has been cleared. */
	i16 cleared; /* output buffer cleared */

	/** Configured stage delay. */
	i16 delay;
	/** Current decoded-tone or detector decision. */
	i16 decode;

	/** Primary input gain in the stage's fixed-point scale. */
	i32 inputGain; /* apply to input data   ? in Q7.8 format */
	/** Secondary input gain in the stage's fixed-point scale. */
	i32 inputGainB; /* apply to input data   ? in Q7.8 format */
	/** Stage output gain in its fixed-point scale. */
	i32 outputGain; /* apply to output data  ? in Q7.8 format */
	/** Nonzero requests mixing into the existing output. */
	i16 mixOut;
	/** Nonzero selects mono stage output. */
	i16 monoOut;

	/** Detector-filter selection. */
	i16 filterType; /* iir, fir, 1, 2, 3, 4 ... */

	/** Stage processing callback. */
	i16 (*sigProc)(struct urp_radio_stage *sps); /* function to call */

	/** Filter accumulation normalization divisor. */
	i32 calcAdjust; /* final adjustment */
	/** Current filter-history position. */
	i16 nx; /* number of x history elements */
	/** Number of active filter coefficients. */
	i16 ncoef; /* number of coefficients */
	/** Allocated filter-history length. */
	i16 size_x; /* size of each x history element */
	/** Allocated coefficient-table length. */
	i16 size_coef; /* size of each coefficient */
	/** Filter input history. */
	void *x; /* history registers */
	/** Secondary filter input history. */
	void *x2; /* history registers, 2nd bank */
	/** Fixed-point FIR coefficient table. */
	void *coef;

	/** Recursive-filter output history. */
	void *y; /* history registers, y bank */
	/** Recursive-filter feedback coefficients. */
	void *coefa;
	/** Recursive-filter feed-forward coefficients. */
	void *coefb;

	/** Next stage in the owned detector list. */
	void *nextSps; /* next Sps function */

	/** One radio-detection or measurement stage and its owned filter workspace. */
} urp_radio_stage;

struct t_dec_dcs;
struct t_lsd_control;
struct t_decLsd;
;
struct t_encLsd;

/*
	pmr channel
*/
/** Carrier, CTCSS, calibration, and transmitter-signaling state for one radio. */
typedef struct urp_radio_state {
	/** Index assigned to this radio or detector stage. */
	i16 index; /* which one */
	/** Assigned ALSA sound card index. */
	i16 devicenum; /* belongs to */

	/** Symbolic name used to identify this entry. */
	char *name;

	/** Nonzero enables this channel, stage, or detector. */
	i16 enabled; /* enabled/disabled */
	/** Radio-signaling engine status. */
	i16 status; /* ok, error, busy, idle, initializing */

	/** Maximum enabled radio trace verbosity. */
	i16 tracelevel;
	/** Selected radio diagnostic trace format. */
	i16 tracetype;
	/** Selected radio trace-point mask. */
	u32 tracemask;

	/** Base-rate receive samples per processing block. */
	i16 nSamplesRx; /* max frame size */
	/** Base-rate transmitter samples per block. */
	i16 nSamplesTx;

	/** ADC sample rate in Hz. */
	i32 inputSampleRate; /* in S/s  48000 */
	/** Detector sample rate in Hz. */
	i32 baseSampleRate; /* in S/s   8000 */

	/** Primary input gain in the stage's fixed-point scale. */
	i16 inputGain;
	/** DC offset applied to detector input. */
	i16 inputOffset;

	/** Processing tick counter. */
	i32 ticks; /* time ticks */
	/** Number of receive blocks processed. */
	u32 frameCountRx; /* number processed */
	/** Number of transmitter blocks processed. */
	u32 frameCountTx;

	/** Transmitter frame synchronization state. */
	i8 txframelock;

	/** Configured transmitter hang interval. */
	i32 txHangTime;
	/** Remaining transmitter hang interval. */
	i32 txHangTimer;
	/** Transmitter turn-off sequence selection. */
	i32 txTurnOff;
	/** Nonzero requests transmitter history clearing. */
	i16 txBufferClear;

	/** Delay after PTT assertion in milliseconds. */
	i32 txsettletime; /* in samples */
	/** Remaining PTT settling interval in milliseconds. */
	i32 txsettletimer;

	/** Radio-signaling blanking interval in milliseconds. */
	i16 txrxblankingtime; /* in milli-seconds */
	/** Remaining radio-signaling blanking interval. */
	i16 txrxblankingtimer;

	/** Tracked receiver DC offset. */
	i16 rxDC; /* average DC value of input */
	/** DSP squelch opening threshold. */
	i16 rxSqSet; /* carrier squelch threshold */
	/** DSP squelch closing hysteresis. */
	i16 rxSqHyst; /* carrier squelch hysterysis */
	/** Measured discriminator-noise signal-strength value. */
	i16 rxRssi; /* current Rssi level */
	/** Receiver signal-quality estimate. */
	i16 rxQuality; /* signal quality metric */
	/** Current native carrier-detector state. */
	i16 rxCarrierDetect; /* carrier detect */
	/** Configured carrier detector source. */
	enum radio_carrier_detect rxCdType;
	/** VOX carrier hold interval. */
	i16 voxHangTime; /* if rxCdType=CD_DSP_VOX, time to wait for RX audio before setting CD=0 */
	/** VOX detector threshold. */
	i16 rxSqVoxAdj;
	/** External hardware carrier indication. */
	i16 rxExtCarrierDetect;
	/** Input-blanking interval/state. */
	i32 inputBlanking; /* Tx pulse eliminator */

	/** Receiver audio-source assignment. */
	enum radio_rx_audio rxDemod;

	/** Whether noise-squelch detection is enabled. */
	i16 rxNoiseSquelchEnable;
	/** Receiver high-pass selection retained in detector state. */
	i16 rxHpfEnable;
	/** Receiver deemphasis selection retained in detector state. */
	i16 rxDeEmpEnable;
	/** Whether subaudible centering is enabled. */
	i16 rxCenterSlicerEnable;
	/** Whether CTCSS decoding is enabled. */
	i16 rxCtcssDecodeEnable;
	/** DCS decode enable metadata. */
	i16 rxDcsDecodeEnable;
	/** Configured receiver squelch-tail delay. */
	i16 rxSquelchDelay;

	/** Whether receiver operation is permitted during transmit. */
	char radioDuplex;
	/** Selected high-frequency noise-detector filter. */
	char rxNoiseFilType;
	/** Receiver detector low-pass filter selector. */
	int rxlpf;
	/** Receiver detector high-pass filter selector. */
	int rxhpf;

	/** Owned signaling-code parsing workspace. */
	char *pStr;

	/*      start channel signaling codes source */
	/** Receive signaling-code configuration text. */
	char *pRxCodeSrc; /* source */
	/** Transmit signaling-code configuration text. */
	char *pTxCodeSrc; /* source */
	/** Default transmitter signaling-code text. */
	char *pTxCodeDefault; /* source */
	/*      end channel signaling codes source */

	/*      start signaling code info derived from source */
	/** Number of configured receive signaling codes. */
	i16 numrxcodes;
	/** Number of configured transmit signaling codes. */
	i16 numtxcodes;
	/** Owned tokenized receive-code text. */
	char *pRxCodeStr; /* copied and cut up */
	/** Pointers into the tokenized receive-code text. */
	char **pRxCode; /* pointers to subs */
	/** Owned tokenized transmit-code text. */
	char *pTxCodeStr;
	/** Pointers into the tokenized transmit-code text. */
	char **pTxCode;

	/** Default transmitted CTCSS frequency. */
	char txctcssdefault[16]; /* codes from higher level */

	/** Receive CTCSS frequency list in Hz. */
	char *rxctcssfreqs; /* rest are derived from this */
	/** Transmit CTCSS frequency list in Hz. */
	char *txctcssfreqs;

	/** Number of receive CTCSS frequencies. */
	char numrxctcssfreqs;
	/** Number of transmit CTCSS frequencies. */
	char numtxctcssfreqs;

	/** Configured receive CTCSS code values. */
	char *rxctcss[CTCSS_NUM_CODES]; /* pointers to each tone in string above */
	/** Configured transmit CTCSS code values. */
	char *txctcss[CTCSS_NUM_CODES];

	/** Map from detected receive tones to transmit tone selections. */
	i16 rxCtcssMap[CTCSS_NUM_CODES];

	/** Signaling mode selected for the default transmitted code. */
	i8 txcodedefaultsmode;
	/** Default transmit CTCSS table index. */
	i16 txctcssdefault_index;
	/** Default transmit CTCSS frequency in Hz. */
	float txctcssdefault_value;

	/** Selected transmitted CTCSS frequency. */
	char txctcssfreq[32]; /* encode now for upper layers */
	/** Currently decoded receive CTCSS frequency. */
	char rxctcssfreq[32]; /* decode now */
	/*      end most of signaling code info derived from source */

	/** Low-speed-data control metadata. */
	struct t_lsd_control *pLsdCtl;

	/** Configured signaling repeater number. */
	i16 rptnum;
	/** Configured signaling area identifier. */
	i16 area;
	/** Configured signaling user key. */
	char *ukey;
	/** Configured signaling idle interval. */
	u32 idleinterval;
	/** Configured signaling turn-off count. */
	char turnoffs;

	/** Parallel-port signaling lock metadata. */
	char pplock;

	/** Reserved signaling-state field. */
	i16 dummy;

	/** Transmit scrambler frequency metadata. */
	i32 txScramFreq;
	/** Receive scrambler frequency metadata. */
	i32 rxScramFreq;

	/** Voice calibration gain metadata. */
	i16 gainVoice;
	/** Subaudible calibration gain. */
	i16 gainSubAudible;

	/** Transmitter output-A assignment. */
	enum radio_tx_mix txMixA;
	/** Transmitter output-B assignment. */
	enum radio_tx_mix txMixB;

	/** Receiver muting state. */
	i16 rxMuting;

	/** Receiver idle-processing reduction setting. */
	i16 rxCpuSaver;
	/** Transmitter idle-processing reduction setting. */
	i16 txCpuSaver;

	/** Receiver squelch mode metadata. */
	i8 rxSqMode; /* 0 open, 1 carrier, 2 coded */

	/** Carrier detector method metadata. */
	i8 cdMethod;

	/** Receiver closing decision threshold. */
	i16 rxSquelchPoint;

	/** Receiver opening decision threshold. */
	i16 rxCarrierPoint;
	/** Receiver carrier decision hysteresis. */
	i16 rxCarrierHyst;

	/** CTCSS reverse-burst phase shift in degrees. */
	i16 txCtcssTocShift;
	/** CTCSS reverse-burst duration in milliseconds. */
	i16 txCtcssTocTime;
	/** CTCSS turn-off behavior. */
	i8 txTocType;

	/** Current signaling mode. */
	i16 smode; /* ctcss, dcs, lsd */
	/** Code selected by the current signaling mode. */
	i16 smodecode;
	/** Previous signaling mode. */
	i16 smodewas; /* ctcss, dcs, lsd */
	/** Remaining signaling-mode hold interval. */
	i32 smodetimer; /* in ms */
	/** Configured signaling-mode hold interval. */
	i32 smodetime; /* to set in ms */

	/** Owned receive CTCSS decoder bank. */
	urp_ctcss_decoder *rxCtcss;
	/** DCS decoder metadata. */
	struct t_dec_dcs *decDcs;
	/** Low-speed-data decoder metadata. */
	struct t_decLsd *decLsd;
	/** Tracked DCS amplitude metadata. */
	i16 clamplitudeDcs;
	/** Tracked DCS DC-center metadata. */
	i16 centerDcs;
	/** DCS blanking interval metadata. */
	u32 dcsBlankingTimer;
	/** DCS decode result metadata. */
	i16 dcsDecode; /* current dcs decode value */

	/** Tracked low-speed-data amplitude metadata. */
	i16 clamplitudeLsd;
	/** Tracked low-speed-data DC-center metadata. */
	i16 centerLsd;

	/** PTT request from the channel driver. */
	i16 txPttIn; /* from external request */
	/** PTT state produced by the transmit-signaling engine. */
	i16 txPttOut; /* to radio hardware */
	/** PTT state last presented to hardware. */
	i16 txPttHid;

	/** Channel bandwidth metadata. */
	i16 bandwidth; /* wide/narrow */
	/** Transmit compander selection metadata. */
	i16 txCompand; /* type */
	/** Receive compander selection metadata. */
	i16 rxCompand;

	/** muted, flat, pre-emp limited filtered */
	i16 txEqRight; /* muted, flat, pre-emp limited filtered */
	/** Left transmitter equalizer selection metadata. */
	i16 txEqLeft;

	/** Right transmitter potentiometer setting metadata. */
	i16 txPotRight;
	/** Left transmitter potentiometer setting metadata. */
	i16 txPotLeft;

	/** Right receiver potentiometer setting metadata. */
	i16 rxPotRight;
	/** Left receiver potentiometer setting metadata. */
	i16 rxPotLeft;

	/** Signaling-system function selector. */
	i16 function;

	/** Transmit-signaling state-machine state. */
	i16 txState; /* off,settling,on,hangtime,turnoff */

	/** Next detector-stage index. */
	i16 spsIndex;

	/** Previous receive CTCSS decode result. */
	i16 lastrxdecode;

	/** Calibration measurement stage. */
	urp_radio_stage *spsMeasure; /* measurement block */

	/** Head of the receive detector-stage list. */
	urp_radio_stage *spsRx; /* 1st signal processing struct */
	/** Low-speed-data filter stage metadata. */
	urp_radio_stage *spsRxLsd;
	/** Low-speed-data slicer stage metadata. */
	urp_radio_stage *spsRxLsdNrz;
	/** Receiver deemphasis stage metadata. */
	urp_radio_stage *spsRxDeEmp;
	/** Receiver high-pass stage metadata. */
	urp_radio_stage *spsRxHpf;
	/** VOX measurement stage. */
	urp_radio_stage *spsRxVox;
	/** Receiver delay stage. */
	urp_radio_stage *spsDelayLine; /* Last signal processing struct */
	/** Receiver squelch-tail delay stage. */
	urp_radio_stage *spsRxSquelchDelay;
	/** Receiver output stage metadata. */
	urp_radio_stage *spsRxOut; /* Last signal processing struct */

	/* The 48 kHz renderer consumes this keying and squelch-tail state. */
	/** Selected transmit CTCSS frequency in tenths of a Hz. */
	i32 txCtcssFreq10;
	/** Remaining CTCSS turn-off interval in milliseconds. */
	i32 txCtcssTurnoffTimer;
	/** Requested CTCSS signaling operation. */
	i8 txCtcssOption;
	/** Current CTCSS transmit-signaling state. */
	i8 txCtcssState;
	/** Nonzero while the native oscillator should emit CTCSS. */
	i8 txCtcssEnabled;
	/** CTCSS reverse-burst phase shift in degrees. */
	i8 txCtcssPhaseShift;
	/** Nonzero selects the 250 Hz reference-level calibration. */
	i8 txCtcssFilter250;
	/** CTCSS amplitude gain with eight fractional bits. */
	i32 txCtcssGainQ8;
	/** Output-A reference gain with eight fractional bits. */
	i32 txOutputGainA;
	/** Output-B reference gain with eight fractional bits. */
	i32 txOutputGainB;

	/* tune tweaks */

	/** Remaining VOX carrier hold interval. */
	i32 rxVoxTimer; /* Vox Hang Timer */

	/** Pointer to the live DSP squelch calibration value. */
	i16 *prxSquelchAdjust;

	/** Pointer to the receiver voice measurement. */
	i16 *prxVoiceMeasure;
	/** Pointer to receiver voice calibration metadata. */
	i32 *prxVoiceAdjust;

	/** Pointer to the CTCSS decoder level measurement. */
	i16 *prxCtcssMeasure;
	/** Pointer to the CTCSS decoder input-gain setting. */
	i32 *prxCtcssAdjust;

	/** Pointer to transmitter voice calibration metadata. */
	i16 *ptxVoiceAdjust; /* from calling application */
	/** Pointer to transmit CTCSS deviation calibration. */
	i32 *ptxCtcssAdjust; /* from calling application */

	struct {
		/** Noise-squelch selection metadata. */
		unsigned pmrNoiseSquelch : 1;
		/** Receiver high-pass selection metadata. */
		unsigned rxHpf : 1;
		/** Transmitter high-pass selection metadata. */
		unsigned txHpf : 1;
		/** Transmitter low-pass selection metadata. */
		unsigned txLpf : 1;
		/** Receiver deemphasis selection metadata. */
		unsigned rxDeEmphasis : 1;
		/** Transmitter preemphasis selection metadata. */
		unsigned txPreEmphasis : 1;
		/** External hardware carrier indication. */
		unsigned extCarrierDetect : 1;
		/** Transmit diagnostic capture enable. */
		unsigned txCapture : 1;
		/** Receive diagnostic capture enable. */
		unsigned rxCapture : 1;
		/** Receive CTCSS monitor mode. */
		unsigned rxplmon : 1;
		/** Nonzero when remote-radio control is active. */
		unsigned remoted : 1;
		/** Radio loopback diagnostic mode. */
		unsigned loopback : 1;
		/** Receive signaling polarity inversion. */
		unsigned rxpolarity : 1;
		/** Transmit signaling polarity inversion. */
		unsigned txpolarity : 1;
		/** Receive DCS polarity inversion. */
		unsigned dcsrxpolarity : 1;
		/** Transmit DCS polarity inversion. */
		unsigned dcstxpolarity : 1;
		/** Receive low-speed-data polarity inversion. */
		unsigned lsdrxpolarity : 1;
		/** Transmit low-speed-data polarity inversion. */
		unsigned lsdtxpolarity : 1;
		/** Nonzero while the transmitter is settling. */
		unsigned txsettling : 1;
		/** Signaling-mode turn-off state. */
		unsigned smodeturnoff : 1;

		/** Nonzero enables ctcss receiver. */
		unsigned ctcssRxEnable : 1;
		/** Nonzero enables ctcss transmitter. */
		unsigned ctcssTxEnable : 1;
		/** Nonzero enables dcs receiver. */
		unsigned dcsRxEnable : 1;
		/** Nonzero enables dcs transmitter. */
		unsigned dcsTxEnable : 1;
		/** Nonzero enables lmr receiver. */
		unsigned lmrRxEnable : 1;
		/** Nonzero enables lmr transmitter. */
		unsigned lmrTxEnable : 1;
		/** Nonzero enables mdc receiver. */
		unsigned mdcRxEnable : 1;
		/** Nonzero enables mdc transmitter. */
		unsigned mdcTxEnable : 1;
		/** Nonzero enables dst receiver. */
		unsigned dstRxEnable : 1;
		/** Nonzero enables dst transmitter. */
		unsigned dstTxEnable : 1;
		/** Nonzero enables p25 receiver. */
		unsigned p25RxEnable : 1;
		/** Nonzero enables p25 transmitter. */
		unsigned p25TxEnable : 1;
		/** Nonzero enables ax25. */
		unsigned ax25Enable : 1;

		/** Nonzero inhibits transmitted CTCSS. */
		unsigned txCtcssInhibit : 1;
		/** Nonzero when CTCSS transmit signaling is ready. */
		unsigned txCtcssReady : 1;
		/** Nonzero after CTCSS turn-off completes. */
		unsigned txCtcssOff : 1;

		/** Qualified receiver key state. */
		unsigned rxkeyed : 1;
		/** Nonzero while receiver processing is halted. */
		unsigned rxhalted : 1;
		/** Nonzero while transmitter processing is halted. */
		unsigned txhalted : 1;
		/** Nonzero while a radio calibration command is active. */
		unsigned tuning : 1;
		/** Previous transmit PTT request. */
		unsigned pttwas : 1;
		/** Packed radio-signaling status and enable flags. */
	} b; /**< Packed radio-signaling status and enable flags. */

	/** Native discriminator input workspace. */
	i16 *pRxDemod; /* buffers */
	/** Base-rate detector input workspace. */
	i16 *pRxBase; /* decimated lpf input */
	/** Sample-rate noise-squelch gate; one means open at the native input sample. */
	uint8_t *rxCarrierGate;
	/** High-frequency discriminator-noise workspace. */
	i16 *pRxNoise;
	/** subaudible only */
	i16 *pRxLsd; /* subaudible only */
	/** subaudible removed */
	i16 *pRxHpf; /* subaudible removed */
	/** Receiver deemphasis workspace metadata. */
	i16 *pRxDeEmp; /* EIA Audio */
	/** Receiver speaker-audio workspace metadata. */
	i16 *pRxSpeaker; /* EIA Audio */
	/** DC Restored LSD */
	i16 *pRxDcTrack; /* DC Restored LSD */
	/** LSD Limited */
	i16 *pRxLsdLimit; /* LSD Limited */
	/** CTCSS detector input workspace. */
	i16 *pRxCtcss;
	/** Squelched receiver output workspace. */
	i16 *pRxSquelch;
	/** VOX measurement workspace. */
	i16 *prxVoxMeas;
	/** Receiver calibration measurement workspace. */
	i16 *prxMeasure;

	/** First alternate detector workspace. */
	i16 *pAlt0;
	/** Second alternate detector workspace. */
	i16 *pAlt1;

#if URP_RADIO_DEBUG == 1

	/** Centered low-speed-data workspace metadata. */
	i16 *pRxLsdCen;

	/** Transmitter diagnostic output workspace. */
	i16 *pTstTxOut;

	/** aliases sdbg->buffer while receive capture is active */
	i16 *prxDebug; /* aliases sdbg->buffer while receive capture is active */
	/** consolidated debug buffer */
	i16 *ptxDebug; /* consolidated debug buffer */

	/** Prx Debug0 diagnostic sample workspace. */
	i16 *prxDebug0;
	/** Prx Debug1 diagnostic sample workspace. */
	i16 *prxDebug1;
	/** Prx Debug2 diagnostic sample workspace. */
	i16 *prxDebug2;
	/** Prx Debug3 diagnostic sample workspace. */
	i16 *prxDebug3;

	/** Ptx Debug0 diagnostic sample workspace. */
	i16 *ptxDebug0;
	/** Ptx Debug1 diagnostic sample workspace. */
	i16 *ptxDebug1;
	/** Ptx Debug2 diagnostic sample workspace. */
	i16 *ptxDebug2;
	/** Ptx Debug3 diagnostic sample workspace. */
	i16 *ptxDebug3;

#endif

	/** Number of active trace channels. */
	i16 numDebugChannels;

	/** Owned radio trace configuration and buffers. */
	t_sdbg *sdbg;

	/** Low-level calibration/diagnostic setting. */
	i16 fever;

	/** Carrier, CTCSS, calibration, and transmitter-signaling state for one radio. */
} urp_radio_state;

/*
	function prototype declarations
*/
/** @brief Store one scaled signal sample at a selected trace point.
 * @param point Trace-point identifier.
 * @param sdbg Radio trace configuration and sample storage.
 * @param index Sample position within the trace block.
 * @param value Signed sample or detector-state value to record.
 */
void strace(i16 point, t_sdbg *sdbg, i16 index, i16 value);
/** @brief Collect the configured radio trace channels into the debug buffer.
 * @param sdbg Radio trace configuration and sample storage.
 */
void strace2(t_sdbg *sdbg);
/** @brief Emit a radio trace when the configured and requested levels permit it.
 * @param configured_level Maximum enabled radio trace verbosity.
 * @param level Message trace verbosity.
 * @param format printf-style message format.
 * @param ... Values for the preceding format string.
 */
void urp_radio_trace_log(int configured_level, int level, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

/** @brief Allocate a radio-signaling engine and its detector buffers from channel settings.
 * @param tChan Radio-signaling engine state.
 * @param numSamples Samples in one base-rate processing block.
 * @return Owned signaling state, or NULL if allocation fails.
 */
urp_radio_state *urp_radio_create(urp_radio_state *tChan, i16 numSamples);
/** @brief Allocate a zeroed detector stage owned by a radio channel.
 * @param pChan Radio-signaling engine state.
 * @return Owned zeroed stage, or NULL if allocation fails.
 */
urp_radio_stage *urp_radio_stage_create(urp_radio_state *pChan);
/** @brief Release a radio-signaling engine and every stage it owns.
 * @param pChan Radio-signaling engine state.
 * @return Zero after releasing state; one for a NULL channel.
 */
i16 urp_radio_destroy(urp_radio_state *pChan);
/** @brief Release a detector stage and its allocated sample and coefficient buffers.
 * @param pSps Detector stage to release.
 * @return Zero after releasing the stage.
 */
i16 urp_radio_stage_destroy(urp_radio_stage *pSps);
/** @brief Measure discriminator noise and update DSP carrier qualification.
 * @param mySps Detector stage and its input/output workspace.
 * @return Zero after processing; one when the stage is disabled or cannot process.
 */
i16 urp_radio_receive_frontend(urp_radio_stage *mySps);
/** @brief Run the signaling detector's fixed-point FIR with its configured rate conversion.
 * @param mySps Detector stage and its input/output workspace.
 * @return Zero after processing; one when the stage is disabled or cannot process.
 */
i16 urp_radio_fir(urp_radio_stage *mySps);
/** @brief Integrate the detector input using the stage's recursive filter coefficients.
 * @param mySps Detector stage and its input/output workspace.
 * @return Zero after processing; one when the stage is disabled or cannot process.
 */
i16 gp_inte_00(urp_radio_stage *mySps);
/** @brief Track input center and amplitude for subaudible tone discrimination.
 * @param mySps Detector stage and its input/output workspace.
 * @return Zero after processing; one when the stage is disabled or cannot process.
 */
i16 CenterSlicer(urp_radio_stage *mySps);
/** @brief Update the enabled CTCSS tone detectors and decoded-tone state.
 * @param radio Radio-signaling engine state.
 * @return Zero after processing; one when the stage is disabled or cannot process.
 */
i16 urp_ctcss_decode(urp_radio_state *radio);
/** @brief Apply the configured receiver squelch-tail delay.
 * @param mySps Detector stage and its input/output workspace.
 * @return Zero on success; a nonzero status if the operation cannot complete.
 */
i16 DelayLine(urp_radio_stage *mySps);

/** @brief Advance carrier detection, CTCSS decoding, measurements, and TX signaling for one block.
 * @param PmrChan Radio-signaling engine state.
 * @param input Input samples; the caller retains ownership.
 * @param outputrx Base-rate receiver output buffer.
 * @param outputtx Transmitter scratch buffer retained by the radio interface.
 * @return Zero after a processed block; one when processing cannot proceed.
 */
i16 urp_radio_process(urp_radio_state *PmrChan, i16 *input, i16 *outputrx, i16 *outputtx);

/** @brief Split a comma-separated signaling-code list into owned strings and a pointer table.
 * @param src Comma-separated signaling codes; not modified.
 * @param dest Receives allocated mutable token storage; the caller frees it.
 * @param ptrs Receives the allocated table of token pointers.
 * @return Number of tokens, or -1 on allocation failure.
 */
i16 string_parse(const char *src, char **dest, char ***ptrs);
/** @brief Resolve receive/transmit signaling lists and CTCSS tone mappings.
 * @param pChan Radio-signaling engine state.
 * @return Zero on success; one if the signaling lists cannot be resolved.
 */
i16 urp_radio_parse_codes(urp_radio_state *pChan);

/** @brief Find the CTCSS table index corresponding to a requested tone.
 * @param freq CTCSS frequency in Hz.
 * @return CTCSS table index, or CTCSS_NULL if no supported tone matches.
 */
i16 urp_ctcss_frequency_index(float freq);
/** @brief Measure block extrema and peak-to-peak level for radio calibration.
 * @param mySps Detector stage and its input/output workspace.
 * @return Zero after processing; one when the stage is disabled or cannot process.
 */
i16 MeasureBlock(urp_radio_stage *mySps);

#endif /* USBRADIOPLUS_RADIO_H */

/* end of file */

/** @name File-local and build-time constants
 * @{ */
/** @def URP_RADIO_DEVELOPMENT
 * @brief when running in test mode
 */
/** @def URP_RADIO_TRACE_OVFLW
 * @brief Build-time enable for detector overflow traces.
 */
/** @def URP_RADIO_TRACE_FRONTEND
 * @brief Build-time enable for receive-frontend traces.
 */
/** @def URP_RADIO_TRACE_LEVEL
 * @brief Default radio trace verbosity.
 */
/** @def URP_RADIO_DEBUG
 * @brief Build-time enable for radio diagnostic support.
 */
/** @def URP_RADIO_TRACE
 * @brief Build-time enable for radio trace output.
 */
/** @def TRACEO
 * @brief Emit a channel trace when its configured level permits.
 */
/** @def LSD_DFS
 * @brief Low-speed-data detector sampling factor.
 */
/** @def LSD_DFD
 * @brief Low-speed-data detector decimation factor.
 */
/** @def URP_RADIO_DEBUG_CHANNELS
 * @brief Maximum simultaneous radio trace channels.
 */
/** @def TSCOPE
 * @brief Capture selected detector trace samples.
 */
/** @def URP_RADIO_TRACE_AMP
 * @brief PCM amplitude used to display boolean trace states.
 */
/** @def TRACEC
 * @brief Emit a radio trace prefixed by receive-frame count.
 */
/** @def TRACEF
 * @brief Emit a formatted radio trace.
 */
/** @def TRACEJ
 * @brief Reserved trace hook with no emitted output.
 */
/** @def i8
 * @brief Signed 8-bit integer shorthand.
 */
/** @def u8
 * @brief Unsigned 8-bit integer shorthand.
 */
/** @def i16
 * @brief Signed 16-bit integer shorthand.
 */
/** @def u16
 * @brief Unsigned 16-bit integer shorthand.
 */
/** @def i32
 * @brief Signed 32-bit integer shorthand.
 */
/** @def u32
 * @brief Unsigned 32-bit integer shorthand.
 */
/** @def i64
 * @brief Signed 64-bit integer shorthand.
 */
/** @def u64
 * @brief Unsigned 64-bit integer shorthand.
 */
/** @def M_Q31
 * @brief Fixed-point unity with 31 fractional bits.
 */
/** @def M_Q30
 * @brief Fixed-point unity with 30 fractional bits.
 */
/** @def M_Q29
 * @brief Fixed-point unity with 29 fractional bits.
 */
/** @def M_Q28
 * @brief Fixed-point unity with 28 fractional bits.
 */
/** @def M_Q27
 * @brief Fixed-point unity with 27 fractional bits.
 */
/** @def M_Q26
 * @brief Fixed-point unity with 26 fractional bits.
 */
/** @def M_Q25
 * @brief Fixed-point unity with 25 fractional bits.
 */
/** @def M_Q24
 * @brief Fixed-point unity with 24 fractional bits.
 */
/** @def M_Q23
 * @brief Fixed-point unity with 23 fractional bits.
 */
/** @def M_Q22
 * @brief Fixed-point unity with 22 fractional bits.
 */
/** @def M_Q21
 * @brief undsoweiter
 */
/** @def M_Q20
 * @brief Fixed-point unity with 20 fractional bits.
 */
/** @def M_Q19
 * @brief Fixed-point unity with 19 fractional bits.
 */
/** @def M_Q18
 * @brief Fixed-point unity with 18 fractional bits.
 */
/** @def M_Q17
 * @brief Fixed-point unity with 17 fractional bits.
 */
/** @def M_Q16
 * @brief Fixed-point unity with 16 fractional bits.
 */
/** @def M_Q15
 * @brief Fixed-point unity with 15 fractional bits.
 */
/** @def M_Q14
 * @brief Fixed-point unity with 14 fractional bits.
 */
/** @def M_Q13
 * @brief Fixed-point unity with 13 fractional bits.
 */
/** @def M_Q12
 * @brief Fixed-point unity with 12 fractional bits.
 */
/** @def M_Q11
 * @brief Fixed-point unity with 11 fractional bits.
 */
/** @def M_Q10
 * @brief Fixed-point unity with 10 fractional bits.
 */
/** @def M_Q9
 * @brief Fixed-point unity with 9 fractional bits.
 */
/** @def M_Q8
 * @brief Fixed-point unity with 8 fractional bits.
 */
/** @def M_Q7
 * @brief Fixed-point unity with 7 fractional bits.
 */
/** @def M_Q6
 * @brief Fixed-point unity with 6 fractional bits.
 */
/** @def M_Q5
 * @brief Fixed-point unity with 5 fractional bits.
 */
/** @def M_Q4
 * @brief Fixed-point unity with 4 fractional bits.
 */
/** @def M_Q3
 * @brief Fixed-point unity with 3 fractional bits.
 */
/** @def M_Q2
 * @brief Fixed-point unity with 2 fractional bits.
 */
/** @def M_Q1
 * @brief Fixed-point unity with 1 fractional bits.
 */
/** @def M_Q0
 * @brief Fixed-point unity with 0 fractional bits.
 */
/** @def RADIANS_PER_CYCLE
 * @brief Radians in one complete oscillator cycle.
 */
/** @def SAMPLE_RATE_INPUT
 * @brief Native ADC sample rate in Hz.
 */
/** @def SAMPLE_RATE_NETWORK
 * @brief Base detector and app_rpt reference rate in Hz.
 */
/** @def SAMPLES_PER_BLOCK
 * @brief Samples in one base-rate radio processing block.
 */
/** @def MS_PER_FRAME
 * @brief Duration of one app_rpt processing frame in milliseconds.
 */
/** @def SAMPLES_PER_MS
 * @brief Base-rate detector samples per millisecond.
 */
/** @def RXSQDELAYBUFSIZE
 * @brief Maximum base-rate receiver-delay workspace in samples.
 */
/** @def CTCSS_NULL
 * @brief Sentinel for no decoded or selected CTCSS tone.
 */
/** @def CTCSS_RXONLY
 * @brief Sentinel for a receive-only CTCSS mapping.
 */
/** @def CTCSS_NUM_CODES
 * @brief Number of supported CTCSS reference tones.
 */
/** @def CTCSS_SCOUNT_MUL
 * @brief Fixed-point scale for CTCSS sample-count qualification.
 */
/** @def CTCSS_INTEGRATE
 * @brief 32767*.120 -> 120/1000 = 0.120
 */
/** @def CTCSS_INPUT_LIMIT
 * @brief Bound used for CTCSS detector input amplitude.
 */
/** @def CTCSS_DETECT_POINT
 * @brief CTCSS correlator opening decision threshold.
 */
/** @def CTCSS_HYSTERSIS
 * @brief CTCSS correlator release margin.
 */
/** @def CTCSS_TURN_OFF_TIME
 * @brief CTCSS reverse-burst duration in milliseconds.
 */
/** @def CTCSS_TURN_OFF_SHIFT
 * @brief CTCSS reverse-burst phase shift in degrees.
 */
/** @def TOC_NOTONE_TIME
 * @brief Tone-free interval before PTT release in milliseconds.
 */
/** @def DDB_FRAME_SIZE
 * @brief clock de-drift defaults
 */
/** @def DDB_FRAMES_IN_BUFF
 * @brief Reference elastic-buffer depth in frames.
 */
/** @def DDB_ERR_MODULUS
 * @brief Reference fixed-point clock-error scale.
 */
/** @def CHAN_TXSTATE_IDLE
 * @brief CHAN TXSTATE IDLE.
 */
/** @def CHAN_TXSTATE_ACTIVE
 * @brief CHAN TXSTATE ACTIVE.
 */
/** @def CHAN_TXSTATE_TOC
 * @brief CHAN TXSTATE TOC.
 */
/** @def CHAN_TXSTATE_HANGING
 * @brief CHAN TXSTATE HANGING.
 */
/** @def CHAN_TXSTATE_FINISHING
 * @brief CHAN TXSTATE FINISHING.
 */
/** @def CHAN_TXSTATE_COMPLETE
 * @brief CHAN TXSTATE COMPLETE.
 */
/** @def CHAN_TXSTATE_USURPED
 * @brief CHAN TXSTATE USURPED.
 */
/** @def SMODE_NULL
 * @brief SMODE NULL.
 */
/** @def SMODE_CARRIER
 * @brief SMODE CARRIER.
 */
/** @def SMODE_CTCSS
 * @brief SMODE CTCSS.
 */
/** @def SMODE_DCS
 * @brief SMODE DCS.
 */
/** @def SMODE_LSD
 * @brief SMODE LSD.
 */
/** @def SMODE_MPT
 * @brief SMODE MPT.
 */
/** @def SMODE_DST
 * @brief SMODE DST.
 */
/** @def SMODE_P25
 * @brief SMODE P25.
 */
/** @def SMODE_MDC
 * @brief SMODE MDC.
 */
/** @def SPS_OPT_START
 * @brief SPS OPT START.
 */
/** @def SPS_OPT_STOP
 * @brief SPS OPT STOP.
 */
/** @def SPS_OPT_TURNOFF
 * @brief SPS OPT TURNOFF.
 */
/** @def SPS_OPT_STOPNOW
 * @brief SPS OPT STOPNOW.
 */
/** @def SPS_STAT_STOPPED
 * @brief SPS STAT STOPPED.
 */
/** @def SPS_STAT_STARTING
 * @brief SPS STAT STARTING.
 */
/** @def SPS_STAT_RUNNING
 * @brief SPS STAT RUNNING.
 */
/** @def SPS_STAT_HALTING
 * @brief SPS STAT HALTING.
 */
/** @} */
