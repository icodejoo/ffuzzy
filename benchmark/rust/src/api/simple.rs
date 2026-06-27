#[flutter_rust_bridge::frb(init)]
pub fn init_app() {
    // setup_default_user_utils sets up a non-WASM panic hook; with panic=abort
    // in release the hook is never invoked, but the code still gets linked.
    // Keep empty here; callers still get proper FRB initialization via frb_generated.
}
