## @file
## @brief Regression guards for native audio callback ownership boundaries.
import re
from pathlib import Path

## Repository root containing the native channel adapters under test.
ROOT = Path(__file__).resolve().parents[1]
## Calls that must remain outside hardware-paced audio callbacks.
FORBIDDEN_AUDIO_OPERATIONS = (
    "ast_radio_hid_set_outputs(",
    "ast_radio_ppwrite(",
    "ast_mutex_lock(&pp_lock)",
    "usbradioplus_parallel_program_write(",
    "usbradioplus_program_radio(o)",
    "kickptt(o)",
)


def _function_body(source: str, name: str) -> str:
    """Return one C definition body without assuming its return type or formatting."""
    definition = re.compile(
        rf"(?m)^[^;{{}}]*\b{name}\s*\([^;{{}}]*\)\s*\{{",
        re.DOTALL,
    ).search(source)
    if not definition:
        raise AssertionError(f"missing definition for {name}")
    opening_brace = source.index("{", definition.start(), definition.end())
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace : index + 1]
    raise AssertionError(f"unterminated {name} body")


def test_shared_ctcss_transition_helper_is_callback_safe():
    """Decoded-tone publication must not log from either audio worker."""
    source = (ROOT / "src/usbradioplus_channel_common.c").read_text(encoding="utf-8")
    start = source.index("void usbradioplus_refresh_ctcss_decode(")
    end = source.index("\nvoid usbradioplus_wait_for_eeprom_idle", start)
    helper = source[start:end]
    forbidden = ("ast_debug(", "ast_log(", "ast_mutex_", "ast_alloc", "ast_free(")
    for operation in forbidden:
        assert operation not in helper


def test_native_graph_and_radio_access_lifetime_gates_use_sc_handoffs():
    """Reload reclamation must not race a callback that has only just entered."""
    source = (ROOT / "src/usbradioplus_channel_common.c").read_text(encoding="utf-8")
    graph_publish = _function_body(source, "native_graph_slot_publish")
    graph_acquire = _function_body(source, "usbradioplus_native_graphs_acquire")
    access_begin = _function_body(source, "radio_access_begin_reconfigure")
    access_acquire = _function_body(source, "usbradioplus_radio_access_acquire")

    assert "atomic_store_explicit(&slot->active, graphs, memory_order_seq_cst)" in graph_publish
    assert "atomic_load_explicit(&slot->readers, memory_order_seq_cst)" in graph_publish
    assert "atomic_fetch_add_explicit(&slot->readers, 1U, memory_order_seq_cst)" in graph_acquire
    assert "atomic_load_explicit(&slot->active, memory_order_seq_cst)" in graph_acquire
    assert "atomic_store_explicit(&slot->reconfiguring, 1, memory_order_seq_cst)" in access_begin
    assert "atomic_load_explicit(&slot->readers, memory_order_seq_cst)" in access_begin
    assert access_acquire.count("memory_order_seq_cst") >= 4


def test_shared_ffmpeg_slot_uses_the_same_safe_reader_handoff():
    """The link worker must never acquire a graph reclaimed by reload."""
    source = (ROOT / "src/txagc/avfilter_processor.c").read_text(encoding="utf-8")
    publish = _function_body(source, "txagc_avfilter_slot_publish_candidate")
    acquire = _function_body(source, "txagc_avfilter_slot_acquire")
    destroy = _function_body(source, "txagc_avfilter_slot_destroy")

    assert (
        "atomic_store_explicit(&slot->active, &candidate->node->filter, memory_order_seq_cst)"
        in publish
    )
    assert "atomic_fetch_add_explicit(&slot->readers, 1U, memory_order_seq_cst)" in acquire
    assert "atomic_load_explicit(&slot->active, memory_order_seq_cst)" in acquire
    assert "atomic_store_explicit(&slot->active, NULL, memory_order_seq_cst)" in destroy


def test_native_tick_has_no_direct_blocking_or_control_plane_operations():
    """The shared DSP tick must remain a prepared, bounded callback path."""
    source = (ROOT / "src/usbradioplus_native_tick.c").read_text(encoding="utf-8")
    callback = _function_body(source, "usbradioplus_native_tick")
    forbidden = (
        "ast_debug(",
        "ast_log(",
        "ast_mutex_",
        "ast_queue_frame(",
        "ast_calloc(",
        "ast_realloc(",
        "ast_free(",
        "malloc(",
        "calloc(",
        "realloc(",
        "free(",
        "fopen(",
        "fwrite(",
        "usleep(",
        "sleep(",
        "poll(",
        "select(",
        "txagc_avfilter_",
        "txagc_rnnoise_",
        "src_process(",
        "av_buffersrc_",
        "native_worker_process_one(",
    )
    for operation in forbidden:
        assert operation not in callback


def test_link_audiohook_callback_is_queue_only():
    """Keep synchronous Asterisk audiohook work out of FFmpeg and worker ownership."""
    source = (ROOT / "src/usbradioplus_processing.c").read_text(encoding="utf-8")
    callback = _function_body(source, "txagc_callback")
    forbidden = (
        "txagc_avfilter_",
        "txagc_rnnoise_",
        "av_",
        "src_process(",
        "ast_mutex_",
        "pthread_",
        "ast_calloc(",
        "ast_realloc(",
        "ast_free(",
        "malloc(",
        "calloc(",
        "realloc(",
        "free(",
        "fopen(",
        "fwrite(",
        "usleep(",
        "sleep(",
        "poll(",
        "select(",
    )
    for operation in forbidden:
        assert operation not in callback, f"txagc_callback must not call {operation}"


def test_legacy_audio_callback_publishes_ptt_without_physical_io():
    """Keep legacy native audio free of hardware control and wake-pipe I/O."""
    source = (ROOT / "src/chan_usbradioplus.c").read_text(encoding="utf-8")
    start = source.index(
        "URP_CHANNEL_LOCAL struct ast_frame *usbradio_read(struct ast_channel *c)\n{"
    )
    end = source.index("\nURP_CHANNEL_LOCAL struct ast_channel *usbradio_new", start)
    callback = source[start:end]
    assert "usbradioplus_publish_hardware_ptt(o, o->radio->txPttOut)" in callback
    for operation in FORBIDDEN_AUDIO_OPERATIONS:
        assert operation not in callback


def test_legacy_unload_quiesces_channel_before_native_worker_teardown():
    """An asynchronous soft hangup must never leave a read callback with freed DSP state."""
    source = (ROOT / "src/chan_usbradioplus.c").read_text(encoding="utf-8")
    hangup = _function_body(source, "usbradio_hangup")
    unload = _function_body(source, "unload_module")

    assert hangup.index("pthread_join(o->hidthread, NULL)") < hangup.index("o->owner = NULL")
    assert unload.index("ast_softhangup(o->owner, AST_SOFTHANGUP_APPUNLOAD)") < unload.index(
        "usbradioplus_dsp_destroy(o)"
    )
    assert unload.index("if (o->owner) { /* XXX how ??? */") < unload.index(
        "usbradioplus_dsp_destroy(o)"
    )


def test_modern_audio_callback_publishes_ptt_without_physical_io():
    """Keep PortAudio's native worker free of hardware control and wake-pipe I/O."""
    source = (ROOT / "src/chan_usbradioplus_modern.c").read_text(encoding="utf-8")
    start = source.index("URP_CHANNEL_LOCAL void *usbradio_audio_thread(void *arg)\n{")
    end = source.index("\nURP_CHANNEL_LOCAL struct ast_channel *usbradio_new", start)
    callback = source[start:end]
    assert "usbradioplus_publish_hardware_ptt(o, o->radio->txPttOut)" in callback
    for operation in FORBIDDEN_AUDIO_OPERATIONS:
        assert operation not in callback


def test_native_audio_ptt_transition_and_output_paths_do_not_log():
    """Audio-rate PTT and DAC handling must not enter Asterisk logging."""
    adapters = (
        ("chan_usbradioplus.c", "usbradio_read"),
        ("chan_usbradioplus_modern.c", "usbradio_audio_thread"),
    )
    for source_name, callback_name in adapters:
        source = (ROOT / "src" / source_name).read_text(encoding="utf-8")
        callback = _function_body(source, callback_name)
        transition_start = callback.index("/* Only app_rpt and tuning own PTT. */")
        transition_end = callback.index("usbradioplus_prepare_squelch_audio", transition_start)
        transition = callback[transition_start:transition_end]
        for operation in ("ast_debug(", "ast_log("):
            assert operation not in transition

    legacy = (ROOT / "src" / "chan_usbradioplus.c").read_text(encoding="utf-8")
    for function_name in ("used_blocks", "soundcard_writeframe"):
        output_path = _function_body(legacy, function_name)
        for operation in ("ast_debug(", "ast_log("):
            assert operation not in output_path

    modern = (ROOT / "src" / "chan_usbradioplus_modern.c").read_text(encoding="utf-8")
    output_path = _function_body(modern, "soundcard_writeframe")
    for operation in ("ast_debug(", "ast_log("):
        assert operation not in output_path


def test_native_graph_and_parser_handoffs_use_seq_cst_lifetime_ordering():
    """Reader admission must order against graph retirement and parser replacement."""
    source = (ROOT / "src" / "usbradioplus_channel_common.c").read_text(encoding="utf-8")
    required = {
        "native_graph_slot_publish": (
            "atomic_store_explicit(&slot->active, graphs, memory_order_seq_cst)",
            "atomic_load_explicit(&slot->readers, memory_order_seq_cst)",
        ),
        "usbradioplus_native_graphs_acquire": (
            "atomic_fetch_add_explicit(&slot->readers, 1U, memory_order_seq_cst)",
            "atomic_load_explicit(&slot->active, memory_order_seq_cst)",
            "atomic_fetch_sub_explicit(&slot->readers, 1U, memory_order_seq_cst)",
        ),
        "usbradioplus_native_graphs_release": (
            "atomic_fetch_sub_explicit(&channel->plus_native_graphs.readers, 1U,",
            "memory_order_seq_cst",
        ),
        "radio_access_begin_reconfigure": (
            "atomic_store_explicit(&slot->reconfiguring, 1, memory_order_seq_cst)",
            "atomic_load_explicit(&slot->readers, memory_order_seq_cst)",
        ),
        "usbradioplus_radio_access_acquire": (
            "atomic_load_explicit(&slot->reconfiguring, memory_order_seq_cst)",
            "atomic_fetch_add_explicit(&slot->readers, 1U, memory_order_seq_cst)",
            "atomic_fetch_sub_explicit(&slot->readers, 1U, memory_order_seq_cst)",
        ),
        "usbradioplus_radio_access_release": (
            "atomic_fetch_sub_explicit(&channel->plus_radio_access.readers, 1U,",
            "memory_order_seq_cst",
        ),
    }
    for function_name, operations in required.items():
        body = _function_body(source, function_name)
        for operation in operations:
            assert operation in body, f"{function_name} must retain {operation}"


def test_channel_adapters_have_no_direct_debug_audio_capture():
    """Audio workers must never perform optional diagnostic file capture."""
    forbidden = (
        "DEBUG_CAPTURES",
        "frxcap",
        "ftxcap",
        "rxcapraw",
        "txcapraw",
        "rxtracecap",
        "txtracecap",
        "fwrite(",
    )
    for source_name in ("chan_usbradioplus.c", "chan_usbradioplus_modern.c"):
        source = (ROOT / "src" / source_name).read_text(encoding="utf-8")
        for operation in forbidden:
            assert operation not in source, f"{source_name} retains {operation}"

    common = (ROOT / "src/usbradioplus_channel_common.c").read_text(encoding="utf-8")
    for operation in ("nocap", "rxtracecap", "txtracecap", "fopen(RX_CAP_"):
        assert operation not in common


def test_native_radio_callback_and_detectors_do_not_log_or_block():
    """Keep the hardware-paced signaling path out of Asterisk's control plane."""
    source = (ROOT / "src/usbradioplus_radio.c").read_text(encoding="utf-8")
    forbidden = (
        "ast_log(",
        "ast_log_ap(",
        "urp_radio_trace_log(",
        "ast_debug_get_by_module(",
        "ast_mutex_",
        "fopen(",
        "open(",
        "write(",
    )
    for name in (
        "urp_radio_process",
        "urp_ctcss_decode",
        "urp_radio_receive_frontend",
        "DelayLine",
    ):
        body = _function_body(source, name)
        for operation in forbidden:
            assert operation not in body, f"{name} must not call {operation}"

    # Parsing remains a control-plane operation with useful diagnostics.
    assert "ast_log(LOG_ERROR" in _function_body(source, "urp_radio_parse_codes")
    assert "urp_radio_trace_log" in source


def test_radio_uses_in_memory_traces_without_capture_toggle_state():
    """Prevent removed capture switches and workspaces from returning to radio state."""
    source = (ROOT / "src/usbradioplus_radio.c").read_text(encoding="utf-8")
    header = (ROOT / "src/usbradioplus_radio.h").read_text(encoding="utf-8")
    for obsolete in (
        "rxCapture",
        "txCapture",
        "pTstTxOut",
        "ptxDebug",
        "prxDebug1",
        "prxDebug2",
        "prxDebug3",
    ):
        assert obsolete not in source
        assert obsolete not in header
    assert "t_sdbg" in header
    assert "strace2(pChan->sdbg)" in source
