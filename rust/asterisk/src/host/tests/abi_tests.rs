use super::link_support::*;
use super::*;

#[test]
fn fixed_signature_exports_match_installed_asterisk_bindings() {
    fn identical<T>(_: T, _: T) {}
    macro_rules! check {
        ($name:ident($($argument:ty),*)) => {
            identical(ffi::$name as unsafe extern "C" fn($($argument),*) -> _, $name);
        };
    }
    check!(ast_format_cache_get_slin_by_rate(_));
    check!(ast_format_get_sample_rate(_));
    check!(__ast_format_cap_alloc(_, _, _, _, _));
    check!(__ast_format_cap_append(_, _, _, _, _, _, _));
    check!(ast_format_cap_iscompatible(_, _));
    check!(__ao2_ref(_, _, _, _, _, _));
    check!(ast_channel_register(_));
    check!(ast_channel_unregister(_));
    check!(ast_taskprocessor_get(_, _));
    check!(ast_taskprocessor_unreference(_));
    check!(ast_dsp_new());
    check!(ast_dsp_free(_));
    check!(ast_dsp_set_features(_, _));
    check!(ast_dsp_set_digitmode(_, _));
    check!(__ast_module_ref(_, _, _, _));
    check!(__ast_module_unref(_, _, _, _));
    check!(__ao2_lock(_, _, _, _, _, _));
    check!(ast_channel_tech_set(_, _));
    check!(ast_channel_internal_fd_set(_, _, _));
    check!(ast_channel_nativeformats_set(_, _));
    check!(ast_channel_set_readformat(_, _));
    check!(ast_channel_set_writeformat(_, _));
    check!(ast_hangup(_));
    check!(ast_channel_tech_pvt(_));
    check!(ast_channel_tech_pvt_set(_, _));
    check!(__ast_taskprocessor_push(_, _, _, _, _, _));
    check!(ast_setstate(_, _));
    check!(ast_moh_start(_, _, _));
    check!(ast_moh_stop(_));
    check!(__ao2_trylock(_, _, _, _, _, _));
    check!(__ao2_unlock(_, _, _, _, _));
    check!(ast_jb_configure(_, _));
    check!(ast_channel_state(_));
    check!(ast_queue_frame(_, _));
    check!(ast_dsp_process(_, _, _));
    check!(ast_frame_free(_, _));
    check!(ast_read_textfile(_));
    check!(ast_free_ptr(_));
    check!(ast_channel_iterator_all_new());
    check!(ast_channel_iterator_next(_));
    check!(ast_channel_iterator_destroy(_));
    check!(ast_channel_appl(_));
    check!(ast_channel_name(_));
    check!(ast_channel_data(_));
    check!(ast_channel_rawreadformat(_));
    check!(__ast_datastore_alloc(_, _, _, _, _, _));
    check!(ast_datastore_free(_));
    check!(ast_channel_datastore_find(_, _, _));
    check!(ast_channel_datastore_add(_, _));
    check!(ast_channel_datastore_remove(_, _));
    check!(ast_audiohook_init(_, _, _, _));
    check!(ast_audiohook_attach(_, _));
    check!(ast_audiohook_detach(_));
    check!(ast_audiohook_destroy(_));
    check!(__ast_pthread_mutex_lock(_, _, _, _, _));
    check!(__ast_pthread_mutex_unlock(_, _, _, _, _));
    // SAFETY: both static declarations name the same immutable directory pointer.
    unsafe {
        identical(ffi::ast_config_AST_CONFIG_DIR, ast_config_AST_CONFIG_DIR);
    }
}
