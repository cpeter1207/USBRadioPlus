#include "../src/txagc/avfilter_processor.c"

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <wchar.h>

static struct txagc_config base_config(void)
{
	struct txagc_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.emphasis_corner_hz = 300.0;
	cfg.emphasis_reference_hz = 1000.0;
	cfg.ctcss_notch_width_hz = 5.0;
	cfg.deesser_width_octaves = 1.0;
	cfg.expander_ratio = 2.0;
	cfg.compressor_ratio = 2.0;
	return cfg;
}

static void test_scalar_and_append_helpers(void)
{
	char graph[8] = "";
	double value;
	AVFrame *frame = av_frame_alloc();
	assert(frame);
	assert(clamp(-2.0, -1.0, 1.0) == -1.0);
	assert(clamp(2.0, -1.0, 1.0) == 1.0);
	assert(clamp(0.0, -1.0, 1.0) == 0.0);
	assert(db_to_linear(0.0) == 1.0);
	assert(!graph_append(graph, sizeof(graph), "%s", "ok"));
	assert(graph_append(graph, sizeof(graph), "%s", "overflow") < 0);
	setlocale(LC_CTYPE, "C");
	graph[0] = '\0';
	assert(graph_append(graph, sizeof(graph), "%lc", (wint_t)0x100) < 0);
	memset(graph, 'x', sizeof(graph));
	graph[sizeof(graph) - 1] = '\0';
	assert(graph_append(graph, sizeof(graph) - 1, "x") < 0);
	assert(astats_value(frame, "missing", &value) < 0);
	av_dict_set(&frame->metadata, "bad", "text", 0);
	assert(astats_value(frame, "bad", &value) < 0);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.Peak_level", "-3", 0);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.RMS_level", "-12", 0);
	assert(!astats_value(frame, "lavfi.astats.Overall.Peak_level", &value));
	assert(value == -3.0);
	av_frame_free(&frame);
}

static void test_meter_updates(void)
{
	struct txagc_avfilter state;
	AVFrame *frame = av_frame_alloc();
	assert(frame);
	txagc_avfilter_init(&state);
	update_astats(&state, frame, 1);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.Peak_level", "-3", 0);
	update_astats(&state, frame, 1);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.Peak_level", "-3", 0);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.RMS_level", "-12", 0);
	update_astats(&state, frame, 1);
	update_astats(&state, frame, 0);
	assert(state.input_peak_dbfs == -3.0 && state.output_rms_dbfs == -12.0);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.Peak_level", "-20", 0);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.RMS_level", "-30", 0);
	update_astats(&state, frame, 1);
	update_astats(&state, frame, 0);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.RMS_level", "-100", 0);
	update_astats(&state, frame, 1);

	av_dict_set(&frame->metadata, "lavfi.astats.Overall.Peak_level", "-4", 0);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.RMS_level", "-15", 0);
	update_cleanup_astats(&state, frame, CLEANUP_PRE_FULL);
	update_cleanup_astats(&state, frame, CLEANUP_PRE_5_8);
	update_cleanup_astats(&state, frame, CLEANUP_PRE_8_PLUS);
	update_cleanup_astats(&state, frame, CLEANUP_POST_5_8);
	update_cleanup_astats(&state, frame, CLEANUP_POST_8_PLUS);
	update_cleanup_astats(&state, frame, CLEANUP_PRE_FULL);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.RMS_level", "bad", 0);
	update_cleanup_astats(&state, frame, CLEANUP_PRE_FULL);
	av_frame_free(&frame);
	frame = av_frame_alloc();
	assert(frame);
	av_dict_set(&frame->metadata, "lavfi.astats.Overall.RMS_level", "-20", 0);
	update_cleanup_astats(&state, frame, CLEANUP_PRE_FULL);
	av_frame_free(&frame);
}

static void test_graph_stage_helpers(void)
{
	struct txagc_config cfg = base_config();
	char graph[GRAPH_SIZE];
	enum txagc_stage stages[] = {TXAGC_STAGE_DEESSER,    TXAGC_STAGE_EQUALIZER,
				     TXAGC_STAGE_AGC,	     TXAGC_STAGE_EXPANDER,
				     TXAGC_STAGE_COMPRESSOR, TXAGC_STAGE_LIMITER};
	size_t index;
	graph[0] = '\0';
	assert(!add_sidechain_stage(graph, sizeof(graph), "a", "b", "p", "sidechaingate", 100.0,
				    3000.0, "threshold=0.1"));
	assert(!add_brickwall_bandpass(graph, sizeof(graph), "a", "b", "p", 100.0, 5000.0));
	assert(!add_emphasis(graph, sizeof(graph), "a", "b", 0, 300.0, 1000.0, 48000));
	assert(!add_emphasis(graph, sizeof(graph), "a", "b", 1, 300.0, 1000.0, 48000));
	graph[0] = '\0';
	assert(add_sidechain_stage(graph, 1, "a", "b", "p", "sidechaingate", 100.0, 3000.0,
				   "threshold=0.1") < 0);
	assert(add_brickwall_bandpass(graph, 1, "a", "b", "p", 100.0, 5000.0) < 0);
	assert(add_emphasis(graph, 1, "a", "b", 0, 300.0, 1000.0, 48000) < 0);
	for (index = 0; index < sizeof(stages) / sizeof(stages[0]); ++index) {
		graph[0] = '\0';
		assert(!add_dynamic_stage(graph, sizeof(graph), &cfg, stages[index], "a", "b", 1));
	}
	assert(add_dynamic_stage(graph, sizeof(graph), &cfg, (enum txagc_stage)99, "a", "b", 1) <
	       0);
	cfg.deesser_enabled = cfg.equalizer_enabled = cfg.agc_enabled = 1;
	cfg.expander_enabled = cfg.compressor_enabled = cfg.limiter_enabled = 1;
	for (index = 0; index < sizeof(stages) / sizeof(stages[0]); ++index) {
		graph[0] = '\0';
		assert(!add_dynamic_stage(graph, sizeof(graph), &cfg, stages[index], "a", "b", 1));
		graph[0] = '\0';
		assert(add_dynamic_stage(graph, 1, &cfg, stages[index], "a", "b", 1) < 0);
	}
	cfg.deesser_threshold_dbfs = -18.0;
	cfg.deesser_max_reduction_db = 4.0;
	cfg.deesser_ratio = 1.01;
	graph[0] = '\0';
	assert(!add_dynamic_stage(graph, sizeof(graph), &cfg, TXAGC_STAGE_DEESSER, "a", "b", 1));
}

static void expect_post_input_stage_overflow(struct txagc_config *cfg)
{
	struct txagc_config base = base_config();
	char graph[GRAPH_SIZE];
	char *final_stage;
	size_t prefix_size;
	assert(!build_description(graph, sizeof(graph), &base, 48000));
	final_stage = strstr(graph, "[s0]astats");
	assert(final_stage);
	prefix_size = (size_t)(final_stage - graph) + 1;
	assert(build_description(graph, prefix_size, cfg, 48000) < 0);
}

static void test_description_variants(void)
{
	struct txagc_config cfg = base_config();
	char graph[GRAPH_SIZE];
	assert(!build_description(graph, sizeof(graph), &cfg, 48000));
	assert(build_description(graph, 1, &cfg, 48000) < 0);
	cfg.deemphasis_enabled = 1;
	assert(build_description(graph, 1, &cfg, 48000) < 0);
	cfg = base_config();
	cfg.receive_bandpass_enabled = 1;
	assert(build_description(graph, 1, &cfg, 48000) < 0);
	cfg = base_config();
	cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	strcpy(cfg.ctcss_notch_frequencies, "100");
	assert(build_description(graph, 1, &cfg, 48000) < 0);
	cfg = base_config();
	cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
	cfg.ctcss_highpass_hz = 250.0;
	assert(build_description(graph, 1, &cfg, 48000) < 0);
	cfg = base_config();
	cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	strcpy(cfg.ctcss_notch_frequencies, "x 20 400");
	assert(!build_description(graph, sizeof(graph), &cfg, 48000));
	cfg = base_config();
	cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
	assert(!build_description(graph, sizeof(graph), &cfg, 48000));
	for (unsigned int stage = 0; stage < TXAGC_MAX_DYNAMICS_STAGES; ++stage) {
		cfg = base_config();
		cfg.stage_count = 1;
		cfg.stage_order[0] = (enum txagc_stage)stage;
		assert(!build_description(graph, sizeof(graph), &cfg, 48000));
		cfg = base_config();
		cfg.stage_count = 1;
		cfg.stage_order[0] = (enum txagc_stage)stage;
		cfg.deesser_enabled = cfg.equalizer_enabled = cfg.agc_enabled = 1;
		cfg.expander_enabled = cfg.compressor_enabled = cfg.limiter_enabled = 1;
		expect_post_input_stage_overflow(&cfg);
	}
	cfg = base_config();
	cfg.preemphasis_enabled = 1;
	expect_post_input_stage_overflow(&cfg);
	cfg = base_config();
	cfg.splatter_filter_enabled = 1;
	cfg.output_highpass_hz = 150.0;
	expect_post_input_stage_overflow(&cfg);
	cfg = base_config();
	cfg.splatter_filter_enabled = 1;
	cfg.output_lowpass_hz = 5000.0;
	expect_post_input_stage_overflow(&cfg);
	cfg.output_lowpass_hz = 0.0;
	assert(!build_description(graph, sizeof(graph), &cfg, 48000));
	cfg = base_config();
	cfg.output_gain_db = 1.0;
	expect_post_input_stage_overflow(&cfg);
	cfg = base_config();
	cfg.lookahead_limiter_enabled = 1;
	expect_post_input_stage_overflow(&cfg);
	cfg = base_config();
	cfg.post_limiter_lowpass_enabled = 1;
	cfg.post_limiter_lowpass_hz = 5000.0;
	expect_post_input_stage_overflow(&cfg);
	cfg.deemphasis_enabled = 1;
	cfg.receive_bandpass_enabled = 1;
	cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	strcpy(cfg.ctcss_notch_frequencies, "x 20 100 400");
	cfg.input_gain_db = 3.0;
	cfg.stage_count = TXAGC_MAX_DYNAMICS_STAGES;
	for (unsigned int index = 0; index < cfg.stage_count; ++index)
		cfg.stage_order[index] = (enum txagc_stage)index;
	cfg.deesser_enabled = cfg.equalizer_enabled = cfg.agc_enabled = 1;
	cfg.expander_enabled = cfg.compressor_enabled = cfg.limiter_enabled = 1;
	cfg.preemphasis_enabled = cfg.splatter_filter_enabled = 1;
	cfg.output_highpass_hz = 150.0;
	cfg.output_lowpass_hz = 5000.0;
	cfg.output_gain_db = 2.0;
	cfg.lookahead_limiter_enabled = 1;
	cfg.post_limiter_lowpass_enabled = 1;
	cfg.post_limiter_lowpass_hz = 5000.0;
	assert(!build_description(graph, sizeof(graph), &cfg, 48000));
	cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_HIGHPASS;
	cfg.ctcss_highpass_hz = 250.0;
	assert(!build_description(graph, sizeof(graph), &cfg, 48000));
}

static void test_graph_lifecycle_and_invalid_configuration(void)
{
	struct txagc_avfilter state;
	struct txagc_config cfg = base_config();
	double samples[960] = {0};
	AVFrame *frame;

	txagc_avfilter_init(&state);
	assert(!configure(&state, &cfg, 48000));
	assert(state.configured && state.graph && state.fifo);
	txagc_avfilter_reset(&state);
	assert(!state.configured && !state.graph && !state.fifo);
	txagc_avfilter_reset(&state);

	cfg.post_limiter_lowpass_enabled = 1;
	cfg.post_limiter_lowpass_hz = 5000.0;
	assert(!configure(&state, &cfg, 48000));
	assert(state.cleanup_pre_sink && state.cleanup_post_8_plus_sink);
	assert(txagc_avfilter_process(&state, &cfg, samples, 0, 48000) < 0);
	assert(!av_buffersrc_add_frame_flags(state.source, NULL, 0));
	frame = av_frame_alloc();
	assert(frame);
	assert(!drain_cleanup_meter(&state, state.cleanup_pre_sink, frame, CLEANUP_PRE_FULL));
	av_frame_free(&frame);
	txagc_avfilter_destroy(&state);

	cfg = base_config();
	assert(!txagc_avfilter_process(&state, &cfg, samples, 960, 48000));
	assert(!txagc_avfilter_process(&state, &cfg, samples, 960, 48000));
	assert(!txagc_avfilter_process(&state, &cfg, samples, 960, 44100));
	cfg.input_gain_db = 1.0;
	assert(!txagc_avfilter_process(&state, &cfg, samples, 960, 44100));
	txagc_avfilter_destroy(&state);

	cfg = base_config();
	assert(configure(&state, &cfg, 0) < 0);
	memset(cfg.ctcss_notch_frequencies, 0, sizeof(cfg.ctcss_notch_frequencies));
	for (size_t index = 0; index + 4 < sizeof(cfg.ctcss_notch_frequencies); index += 4)
		memcpy(cfg.ctcss_notch_frequencies + index, "100 ", 4);
	cfg.ctcss_filter_mode = TXAGC_CTCSS_FILTER_NOTCH;
	assert(configure(&state, &cfg, 48000) < 0);
	cfg = base_config();
	cfg.receive_bandpass_enabled = 1;
	cfg.receive_bandpass_highpass_hz = -1.0;
	cfg.receive_bandpass_lowpass_hz = 100.0;
	assert(configure(&state, &cfg, 48000) < 0);
	txagc_avfilter_destroy(&state);
}

int main(void)
{
	test_scalar_and_append_helpers();
	test_meter_updates();
	test_graph_stage_helpers();
	test_description_variants();
	test_graph_lifecycle_and_invalid_configuration();
	puts("FFmpeg graph internal tests passed");
	return 0;
}
