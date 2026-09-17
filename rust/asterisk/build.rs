//! Generates the narrow public-Asterisk binding used by the Rust host.

use std::{env, path::PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=wrapper.h");
    println!("cargo:rerun-if-env-changed=USBRADIOPLUS_ASTERISK_INCLUDEDIR");
    println!("cargo:rerun-if-env-changed=USBRADIOPLUS_AGC_PLUGIN_PATH");

    let agc_plugin_path = env::var("USBRADIOPLUS_AGC_PLUGIN_PATH")
        .unwrap_or_else(|_| "/usr/lib/usbradioplus/usbradioplus_agc.so".to_owned());
    println!("cargo:rustc-env=USBRADIOPLUS_AGC_PLUGIN_PATH={agc_plugin_path}");

    let include_dir =
        env::var("USBRADIOPLUS_ASTERISK_INCLUDEDIR").unwrap_or_else(|_| "/usr/include".to_owned());
    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_arg(format!("-I{include_dir}"))
        .clang_arg("-DAST_MODULE_SELF_SYM=__internal_chan_usbradioplus_self")
        .clang_arg("-fblocks")
        .allowlist_function("ast_(audiohook_.*|channel_.*|cli|config_.*|datastore_.*|dsp_.*|format_.*|frame_free|free_ptr|hangup|jb_.*|log|module_.*|moh_.*|pthread_.*|queue_frame|read_textfile|sem_.*|setstate|taskprocessor_.*|verbose)")
        .allowlist_function("__ast_(channel_alloc|format_cap_.*|cli_register_multiple|module_(ref|unref)|taskprocessor_push|verbose)")
        .allowlist_function("__ao2_(lock|trylock|unlock|ref)")
        .allowlist_type("(ao2_lock_req|ast_(assigned_ids|audiohook|audiohook_direction|channel|channel_iterator|channel_tech|cli_args|cli_command|cli_entry|control_frame_type|datastore|datastore_info|dsp|format|format_cap|frame|jb_conf|module|sem|taskprocessor))")
        .allowlist_var("(AO2_.*|AST_.*|CLI_.*|DSP_.*|LOG_.*|RESULT_.*|ast_config_AST_CONFIG_DIR|ast_format_slin|ast_null_frame|__LOG_.*)")
        .opaque_type("ast_(assigned_ids|channel|channel_iterator|dsp|format|format_cap|module|taskprocessor)")
        .layout_tests(false)
        .generate_comments(false)
        .prepend_enum_name(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("generate bindings from installed public Asterisk headers");
    bindings
        .write_to_file(PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("asterisk.rs"))
        .expect("write Asterisk bindings");
}
