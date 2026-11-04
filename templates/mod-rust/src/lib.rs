//! @NAME@: @DESCRIPTION@
//!
//! A GridNode mod in Rust: the `gridnode` crate wraps GridNode's C API (docs/modding.md).

use gridnode::log;

gridnode::gn_mod_info!("@ID@", "@TITLE@", "0.1.0", "@AUTHOR@", "@DESCRIPTION@");

gridnode::gn_mod!("@ID@", || {
    log!("loaded from {}", gridnode::mod_dir());
    let mut t = 0.0f32;
    gridnode::on_client_tick(move |dt| {
        t += dt;
        if t < 5.0 {
            return;
        }
        t = 0.0;
        if let Some(p) = gridnode::position_of(gridnode::local_client_character()) {
            log!("Faith is at height {:.1} m", p[1]);
        }
    });
    true
});

// Reloadable while the game runs: GridNode takes out its callbacks and hooks.
gridnode::gn_mod_unload!(|| true);
