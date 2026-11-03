//! Preserve the installed shared AGC plug-in path for native profile preparation.

fn main() {
    println!("cargo:rerun-if-env-changed=USBRADIOPLUS_AGC_PLUGIN_PATH");
    let path = std::env::var("USBRADIOPLUS_AGC_PLUGIN_PATH")
        .unwrap_or_else(|_| "/usr/lib/usbradioplus/usbradioplus_agc.so".to_owned());
    println!("cargo:rustc-env=USBRADIOPLUS_AGC_PLUGIN_PATH={path}");
}
