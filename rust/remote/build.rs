//! Generate client records from the installed public C contracts, without linking providers.
use std::{env, path::PathBuf};

fn main() {
    for header in [
        "/usr/include/rptadvremote/rptadvremote.h",
        "/usr/include/rptadv_hamlib_adapter/rptadv_hamlib_adapter.h",
    ] {
        println!("cargo:rerun-if-changed={header}");
    }
    bindgen::Builder::default()
        .header_contents("remote-client.h", "#include <rptadvremote/rptadvremote.h>\n#include <rptadv_hamlib_adapter/rptadv_hamlib_adapter.h>\n")
        .allowlist_type("RptAdvRemote.*")
        .allowlist_type("rptadv_hamlib_.*")
        .allowlist_var("RPTADV_.*")
        .generate_comments(false)
        .layout_tests(false)
        .generate()
        .expect("installed remote-base C contracts")
        .write_to_file(PathBuf::from(env::var_os("OUT_DIR").expect("Cargo OUT_DIR")).join("bindings.rs"))
        .expect("generated client bindings");
}
