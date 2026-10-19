//! hello-rust: the hello mod in Rust. Build for Windows and copy the DLL to the game's mods folder:
//!
//!     cargo build --release --target x86_64-pc-windows-msvc   (or x86_64-pc-windows-gnu)
//!     copy target\x86_64-pc-windows-msvc\release\hello_rust.dll <game>\mods\
//!
//! It logs Faith's position every 2 s (client thread), the height under her (a ray cast on the
//! server thread), and the ServerPlayer messages the engine sends.

use gridnode::{log, Type};

gridnode::gn_mod!("hello-rust", || {
    log!("loaded into GridNode {}", unsafe {
        std::ffi::CStr::from_ptr(gridnode::api().gridnode_version).to_string_lossy()
    });
    if let Some(t) = Type::find("PamClientCharacterEntity") {
        log!("type {} is {:#x} bytes", t.name(), t.size());
    }

    let mut t = 0.0f32;
    gridnode::on_client_tick(move |dt| {
        t += dt;
        if t < 2.0 {
            return;
        }
        t = 0.0;
        match gridnode::position_of(gridnode::local_client_character()) {
            Some(p) => log!("client: Faith at ({:.2}, {:.2}, {:.2})", p[0], p[1], p[2]),
            None => log!("client: no local character"),
        }
    });

    let mut s = 0.0f32;
    gridnode::on_server_tick(move |dt| {
        s += dt;
        if s < 2.0 {
            return;
        }
        s = 0.0;
        let faith = gridnode::local_server_character();
        if let Some(p) = gridnode::position_of(faith) {
            let below = gridnode::raycast([p[0], p[1] + 0.5, p[2]], [p[0], p[1] - 100.0, p[2]], 0.0);
            log!("server: ground {}", below.map(|h| format!("hit at y = {:.2}", h.position[1])).unwrap_or("none".into()));
        }
    });

    let mut n = 0;
    gridnode::on_message("ServerPlayer", move |_msg, info| {
        n += 1;
        if n <= 10 {
            log!("message: {} (category {})", info.type_name, info.category);
        }
    });
    true
});
