//! GridNode mods in Rust.
//!
//! A mod is a `cdylib` in the game's `mods/` folder. [`gn_mod!`] defines its entry point; the
//! functions below wrap GridNode's C API ([`sys`], generated from `sdk/include/gridnode/api.h`),
//! the same API C++ mods use through `gridnode/mod.hpp`.
//!
//! ```ignore
//! gridnode::gn_mod!("my-mod", || {
//!     gridnode::log!("hello from Rust");
//!     gridnode::on_client_tick(|dt| { /* client game thread */ });
//!     true
//! });
//! ```
//!
//! Threads matter: engine functions must be called on the thread of their realm. Callbacks say
//! which thread they run on; use [`run_on_server`] / [`run_on_client`] to get there.
//! The Dear ImGui overlay is C++ only for now.

pub mod sys;

use core::ffi::{c_char, c_int, c_void};
use std::ffi::{CStr, CString};
use std::sync::OnceLock;

/// The host's table: immutable for the life of the process, so sharing it between threads is fine.
struct Api(&'static sys::gn_api);
unsafe impl Send for Api {}
unsafe impl Sync for Api {}
static API: OnceLock<Api> = OnceLock::new();
static NAME: OnceLock<CString> = OnceLock::new();

#[doc(hidden)]
pub unsafe fn __load(api: *const sys::gn_api, name: &str, main: fn() -> bool) -> c_int {
    if api.is_null() {
        return 1;
    }
    let a = &*api;
    if a.version < sys::GN_API_VERSION || (a.size as usize) < core::mem::size_of::<sys::gn_api>() {
        return 1; // an older GridNode than this mod was built for
    }
    let _ = API.set(Api(a));
    let _ = NAME.set(CString::new(name).unwrap_or_default());
    if guard("the mod's main", false, main) { 0 } else { 1 }
}

/// Defines the mod's entry point. The closure runs once, on GridNode's thread, and returns
/// whether the mod loaded.
#[macro_export]
macro_rules! gn_mod {
    ($name:expr, $main:expr) => {
        #[no_mangle]
        pub unsafe extern "C" fn gn_mod_load(api: *const $crate::sys::gn_api) -> core::ffi::c_int {
            $crate::__load(api, $name, $main)
        }
    };
}

/// Optional: makes the mod reloadable while the game runs (GridNode's menu, Mods: Reload, or when its
/// DLL changes). The closure runs before GridNode takes out the mod's callbacks and hooks and frees
/// it: stop the mod's own threads; return true to go ahead, false to stay loaded.
#[macro_export]
macro_rules! gn_mod_unload {
    ($unload:expr) => {
        #[no_mangle]
        pub unsafe extern "C" fn gn_mod_unload() -> core::ffi::c_int {
            let f: fn() -> bool = $unload;
            if f() { 0 } else { 1 }
        }
    };
}

/// The API table (panics if called before the mod was loaded).
pub fn api() -> &'static sys::gn_api {
    API.get().expect("gridnode: the mod is not loaded yet").0
}
fn name() -> *const c_char {
    NAME.get().map(|n| n.as_ptr()).unwrap_or(c"mod".as_ptr())
}
fn cstr(s: &str) -> CString {
    CString::new(s).unwrap_or_default()
}
unsafe fn string(p: *const c_char) -> String {
    if p.is_null() { String::new() } else { CStr::from_ptr(p).to_string_lossy().into_owned() }
}

// ---- callbacks: boxed closures kept for the life of the process (GridNode never drops them) ----
/// Runs a mod's closure called from GridNode; a panic is logged and stops there, because unwinding
/// out of an `extern "C"` function would abort the game.
fn guard<R>(what: &str, fallback: R, f: impl FnOnce() -> R) -> R {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(r) => r,
        Err(_) => {
            if API.get().is_some() {
                log(&format!("{} panicked; GridNode carries on", what));
            }
            fallback
        }
    }
}
fn keep<T>(v: T) -> *mut c_void {
    Box::into_raw(Box::new(v)) as *mut c_void
}

/// One line into gridnode.log, prefixed with the mod's name.
pub fn log(message: &str) {
    let m = cstr(message);
    unsafe { (api().log.unwrap())(name(), m.as_ptr()) }
}

/// `log!("x = {}", x)`: formatted [`log`].
#[macro_export]
macro_rules! log {
    ($($arg:tt)*) => { $crate::log(&format!($($arg)*)) };
}

/// Image VA (as in Ghidra and `gridnode/engine.hpp`) to a runtime address.
pub fn rva(va: u64) -> u64 {
    unsafe { (api().rva.unwrap())(va) }
}

// ---- threads --------------------------------------------------------------------------------
unsafe extern "C" fn tick_tramp<F: FnMut(f32)>(dt: f32, user: *mut c_void) {
    guard("a tick callback", (), || (*(user as *mut F))(dt))
}
/// `f(dt)` every server tick, on the server game thread.
pub fn on_server_tick<F: FnMut(f32) + Send + 'static>(f: F) -> bool {
    unsafe { (api().on_server_tick.unwrap())(Some(tick_tramp::<F>), keep(f)) == 0 }
}
/// `f(dt)` every client tick, on the client game thread.
pub fn on_client_tick<F: FnMut(f32) + Send + 'static>(f: F) -> bool {
    unsafe { (api().on_client_tick.unwrap())(Some(tick_tramp::<F>), keep(f)) == 0 }
}
unsafe extern "C" fn task_tramp<F: FnOnce()>(user: *mut c_void) {
    let f = Box::from_raw(user as *mut F);
    guard("a posted task", (), move || f())
}
/// `f()` once, at the next server tick (on the server thread).
pub fn run_on_server<F: FnOnce() + Send + 'static>(f: F) {
    unsafe { (api().run_on_server.unwrap())(Some(task_tramp::<F>), keep(f)) }
}
/// `f()` once, at the next client tick (on the client thread).
pub fn run_on_client<F: FnOnce() + Send + 'static>(f: F) {
    unsafe { (api().run_on_client.unwrap())(Some(task_tramp::<F>), keep(f)) }
}
pub fn is_server_thread() -> bool {
    unsafe { (api().is_server_thread.unwrap())() != 0 }
}
pub fn is_client_thread() -> bool {
    unsafe { (api().is_client_thread.unwrap())() != 0 }
}

// ---- reflection ---------------------------------------------------------------------------
/// An engine type (TypeInfo).
#[derive(Copy, Clone, PartialEq, Eq)]
pub struct Type(pub *const sys::gn_type);
unsafe impl Send for Type {}
unsafe impl Sync for Type {}

impl Type {
    pub fn find(name: &str) -> Option<Type> {
        let n = cstr(name);
        let t = unsafe { (api().type_find.unwrap())(n.as_ptr()) };
        (!t.is_null()).then_some(Type(t))
    }
    /// The type of an engine object, from its vtable.
    pub fn of(object: *const c_void) -> Option<Type> {
        let t = unsafe { (api().type_of.unwrap())(object) };
        (!t.is_null()).then_some(Type(t))
    }
    pub fn name(&self) -> String {
        unsafe { string((api().type_name.unwrap())(self.0)) }
    }
    pub fn size(&self) -> u32 {
        unsafe { (api().type_size.unwrap())(self.0) }
    }
    pub fn base(&self) -> Option<Type> {
        let t = unsafe { (api().type_super.unwrap())(self.0) };
        (!t.is_null()).then_some(Type(t))
    }
    pub fn is_kind_of(&self, base: Type) -> bool {
        unsafe { (api().is_kind_of.unwrap())(self.0, base.0) != 0 }
    }
    /// A field by name, in this type or its bases.
    pub fn field(&self, name: &str) -> Option<sys::gn_field> {
        let n = cstr(name);
        let mut f = core::mem::MaybeUninit::<sys::gn_field>::zeroed();
        let ok = unsafe { (api().field_find.unwrap())(self.0, n.as_ptr(), f.as_mut_ptr()) } == 0;
        ok.then(|| unsafe { f.assume_init() })
    }
}

/// Reads a `T` at `address`, fault tolerantly (None if unreadable).
pub fn read<T: Copy>(address: *const c_void) -> Option<T> {
    let mut v = core::mem::MaybeUninit::<T>::uninit();
    let ok = unsafe { (api().read.unwrap())(address, v.as_mut_ptr() as *mut c_void, core::mem::size_of::<T>()) } == 0;
    ok.then(|| unsafe { v.assume_init() })
}
/// Writes a `T` at `address`, fault tolerantly.
pub fn write<T: Copy>(address: *mut c_void, value: T) -> bool {
    unsafe { (api().write.unwrap())(address, &value as *const T as *const c_void, core::mem::size_of::<T>()) == 0 }
}
/// A field of `object` (of type `ty` or a subclass) by name; the field's size must match `T`.
pub fn get<T: Copy>(object: *const c_void, ty: Type, field: &str) -> Option<T> {
    let f = ty.field(field)?;
    if f.size as usize != core::mem::size_of::<T>() {
        return None;
    }
    read::<T>(unsafe { (object as *const u8).add(f.offset as usize) } as *const c_void)
}

// ---- entities -----------------------------------------------------------------------------------
unsafe extern "C" fn created_tramp<F: FnMut(*mut c_void)>(e: *mut c_void, _ci: *mut c_void, user: *mut c_void) {
    guard("an on_created callback", (), || (*(user as *mut F))(e))
}
/// `f(entity)` for every entity of `class` (or a subclass) the game creates from now on, on the
/// thread that created it.
pub fn on_created<F: FnMut(*mut c_void) + Send + 'static>(class: &str, f: F) -> bool {
    let c = cstr(class);
    unsafe { (api().on_created.unwrap())(name(), c.as_ptr(), Some(created_tramp::<F>), keep(f)) == 0 }
}

// ---- world ----------------------------------------------------------------------------------------
pub type Transform = sys::gn_transform;
pub type RayHit = sys::gn_ray_hit;

/// Faith on the client (client thread: now; elsewhere: as of the last client tick).
pub fn local_client_character() -> *mut c_void {
    unsafe { (api().local_client_character.unwrap())() }
}
/// Faith on the server (the character `teleport` moves).
pub fn local_server_character() -> *mut c_void {
    unsafe { (api().local_server_character.unwrap())() }
}
pub fn transform_of(entity: *const c_void) -> Option<Transform> {
    let mut t = Transform { right: [0.0; 4], up: [0.0; 4], forward: [0.0; 4], trans: [0.0; 4] };
    (unsafe { (api().entity_transform.unwrap())(entity, &mut t) } == 0).then_some(t)
}
pub fn position_of(entity: *const c_void) -> Option<[f32; 3]> {
    transform_of(entity).map(|t| [t.trans[0], t.trans[1], t.trans[2]])
}
/// The game's teleport. Server thread only (`Err(GN_ERR_THREAD)` elsewhere).
pub fn teleport(server_character: *mut c_void, to: &Transform) -> Result<(), u32> {
    match unsafe { (api().teleport.unwrap())(server_character, to) } {
        0 => Ok(()),
        e => Err(e as u32),
    }
}
/// The nearest hit between `from` and `to` (radius > 0: a sphere sweep). Server thread only.
pub fn raycast(from: [f32; 3], to: [f32; 3], radius: f32) -> Option<RayHit> {
    let mut h = RayHit { position: [0.0; 3], normal: [0.0; 3], distance: 0.0, fraction: 0.0 };
    (unsafe { (api().raycast.unwrap())(from.as_ptr(), to.as_ptr(), radius, &mut h) } == 0).then_some(h)
}
pub fn level_name() -> String {
    let mut buf = [0 as c_char; 256];
    unsafe {
        (api().level_name.unwrap())(buf.as_mut_ptr(), buf.len());
        string(buf.as_ptr())
    }
}
unsafe extern "C" fn level_tramp<F: FnMut(&str, bool)>(level: *const c_char, loaded: c_int, user: *mut c_void) {
    guard("a level callback", (), || (*(user as *mut F))(&string(level), loaded != 0))
}
/// `f(level, loaded)` when a level starts or ends (GridNode's thread).
pub fn on_level<F: FnMut(&str, bool) + Send + 'static>(f: F) -> bool {
    unsafe { (api().on_level.unwrap())(name(), Some(level_tramp::<F>), keep(f)) == 0 }
}

// ---- keys, movement, tuning, game state -----------------------------------------------------------
/// A key as the game reads it (`"Space"` or `"IDK_Space"`); None if unknown.
pub fn key_find(name: &str) -> Option<i32> {
    let c = cstr(name);
    let k = unsafe { (api().key_find.unwrap())(c.as_ptr()) };
    (k >= 0).then_some(k)
}
/// Holds the key for `ticks` client ticks, then lets go.
pub fn key_press(key: i32, ticks: u32) -> bool { unsafe { (api().key_press.unwrap())(key, ticks) == 0 } }
/// Holds an input action (`sys::GN_ACTION_JUMP`, ...) for `ticks` client ticks, whatever the binding.
pub fn action_press(action_hash: u32, ticks: u32) -> bool { unsafe { (api().action_press.unwrap())(action_hash, ticks) == 0 } }
/// Makes Faith jump: the jump action, whatever key is bound (the game decides what it becomes).
pub fn jump() -> bool { unsafe { (api().jump.unwrap())() == 0 } }
pub use crate::sys::{gn_game_state as GameState, gn_movement as Movement};
/// Faith's movement state, wall counters, ground, speed and camera direction.
pub fn movement() -> Option<Movement> {
    let mut m: Movement = unsafe { core::mem::zeroed() };
    (unsafe { (api().movement_get.unwrap())(&mut m) } == 0).then_some(m)
}
/// Writes the fields in `fields` (`sys::GN_MOVE_*`).
pub fn set_movement(m: &Movement, fields: u32) -> bool { unsafe { (api().movement_set.unwrap())(m, fields) == 0 } }
pub use crate::sys::gn_landing as Landing;
/// The game's own landing prediction for Faith (ticks to hit in 60ths of a second, hit height, soft
/// landing); None until she has been in the air.
pub fn landing_prediction() -> Option<Landing> {
    let mut l: Landing = unsafe { core::mem::zeroed() };
    (unsafe { (api().landing_prediction.unwrap())(&mut l) } == 0).then_some(l)
}

// ---- movement events, settings, hotkeys (API 9) ---------------------------------------------------
pub use crate::sys::gn_movement_event as MovementEvent;
unsafe extern "C" fn movement_tramp<F: FnMut(&MovementEvent, &Movement)>(e: *const MovementEvent, now: *const Movement, user: *mut c_void) {
    guard("a movement callback", (), || (*(user as *mut F))(&*e, &*now))
}
/// `f(event, now)` for every landing, take-off and movement state change (`sys::GN_MOVE_EVENT_*`),
/// on the client thread, on the tick it happens.
pub fn on_movement<F: FnMut(&MovementEvent, &Movement) + Send + 'static>(f: F) -> bool {
    unsafe { (api().on_movement.unwrap())(name(), Some(movement_tramp::<F>), keep(f)) == 0 }
}
/// The mod's setting `key` (kept in mods/<mod>.ini), or None when unset.
pub fn config_get(key: &str) -> Option<String> {
    let k = cstr(key);
    let mut buf = [0 as c_char; 1024];
    let r = unsafe { (api().config_get.unwrap())(name(), k.as_ptr(), buf.as_mut_ptr(), buf.len()) };
    (r == 0 || r == crate::sys::GN_ERR_FULL as c_int).then(|| unsafe { string(buf.as_ptr()) })
}
/// Sets the mod's setting `key` (written to disk a second after the last change).
pub fn config_set(key: &str, value: &str) -> bool {
    let (k, v) = (cstr(key), cstr(value));
    unsafe { (api().config_set.unwrap())(name(), k.as_ptr(), v.as_ptr()) == 0 }
}
unsafe extern "C" fn hotkey_tramp<F: FnMut()>(user: *mut c_void) {
    guard("a hotkey callback", (), || (*(user as *mut F))())
}
/// A key the player can rebind (kept in the mod's settings as key.<id>); `f()` on the client thread
/// when it goes down.
/// `default_vk`: a Windows virtual-key code (`b'F' as i32`), 0 for none.
pub fn hotkey<F: FnMut() + Send + 'static>(id: &str, label: &str, default_vk: i32, f: F) -> bool {
    let (i, l) = (cstr(id), cstr(label));
    unsafe { (api().hotkey_add.unwrap())(name(), i.as_ptr(), l.as_ptr(), default_vk, Some(hotkey_tramp::<F>), keep(f)) == 0 }
}
// ---- events between entities (API 11) -------------------------------------------------------------
/// An event's id from its name, as the engine hashes it (djb2-xor).
pub const fn event_hash(name: &str) -> u32 {
    let b = name.as_bytes();
    let (mut h, mut i) = (5381u32, 0);
    while i < b.len() {
        h = h.wrapping_mul(33) ^ b[i] as u32;
        i += 1;
    }
    h
}
/// An id's name, or None for an unnamed one.
pub fn event_name(id: u32) -> Option<String> {
    let mut buf = [0 as c_char; 128];
    (unsafe { (api().event_name.unwrap())(id, buf.as_mut_ptr(), buf.len()) } == 0).then(|| unsafe { string(buf.as_ptr()) })
}
unsafe extern "C" fn event_tramp<F: FnMut(*mut c_void, Type, u32)>(e: *mut c_void, t: *const crate::sys::gn_type, id: u32, user: *mut c_void) {
    guard("an event callback", (), || (*(user as *mut F))(e, Type(t), id))
}
/// `f(entity, type, event_id)` for every event an entity receives (before it handles it), on the thread
/// that delivers it. `event`: a name or "0x<hash>", "" for all; `class`: an entity class, "" for all.
pub fn on_event<F: FnMut(*mut c_void, Type, u32) + Send + 'static>(event: &str, class: &str, f: F) -> bool {
    let (e, c) = (cstr(event), cstr(class));
    unsafe { (api().on_event.unwrap())(name(), e.as_ptr(), c.as_ptr(), Some(event_tramp::<F>), keep(f)) == 0 }
}

/// Sends an event to an entity as its bus would (at its realm's next tick). A name or "0x<hash>".
pub fn send_event(entity: *mut c_void, event: &str) -> bool {
    let e = cstr(event);
    unsafe { (api().event_send.unwrap())(name(), entity, e.as_ptr()) == 0 }
}
/// Sends an event to every live entity of the class (and subclasses); how many, or None.
pub fn send_event_to_class(class: &str, event: &str) -> Option<u32> {
    let (c, e) = (cstr(class), cstr(event));
    let n = unsafe { (api().event_send_class.unwrap())(name(), c.as_ptr(), e.as_ptr()) };
    (n >= 0).then_some(n as u32)
}

// ---- entities of a class (API 14) ----------------------------------------------------------------
unsafe extern "C" fn each_tramp<F: FnMut(*mut c_void, Type)>(e: *mut c_void, t: *const crate::sys::gn_type, user: *mut c_void) {
    guard("an entity callback", (), || (*(user as *mut F))(e, Type(t)))
}
/// `f(entity, type)` for every live entity of the class (and subclasses), logic entities too; how
/// many, or None for an unknown class. Entities made before GridNode loaded are not seen.
pub fn entities_of_class<F: FnMut(*mut c_void, Type)>(class: &str, mut f: F) -> Option<u32> {
    let c = cstr(class);
    let n = unsafe { (api().entities_of_class.unwrap())(c.as_ptr(), Some(each_tramp::<F>), &mut f as *mut F as *mut c_void) };
    (n >= 0).then_some(n as u32)
}

// ---- unloading (API 15) -------------------------------------------------------------------------
/// This mod unloaded (its `gn_mod_unload!` runs, everything it registered goes) once the current
/// callback returns; it stays unloaded until the GridNode menu's Load. False without `gn_mod_unload!`.
pub fn unload_self() -> bool { unsafe { (api().mod_unload.unwrap())() == 0 } }
/// Every mod and GridNode itself unloaded, the game put back as it was (the menu's Unload section).
pub fn unload_gridnode() { unsafe { (api().gridnode_unload.unwrap())(); } }

// ---- code patches (API 15) -----------------------------------------------------------------------
/// The call or jmp at `site_va`, which must go to `expected_target_va`, goes to `detour`; `*original`
/// receives the game's function. Undone when the mod is unloaded.
/// # Safety
/// `detour` must be an `extern "C"` function with the called function's signature.
pub unsafe fn patch_call(site_va: u64, expected_target_va: u64, detour: *const c_void, original: *mut *mut c_void) -> bool {
    (api().patch_call.unwrap())(name(), site_va, expected_target_va, detour as *mut c_void, original) == 0
}
/// `bytes` at `va`, only if `expected` is there now (the same length, at most 64). Undone when the mod
/// is unloaded.
pub fn patch_bytes(va: u64, expected: &[u8], bytes: &[u8]) -> bool {
    if expected.len() != bytes.len() {
        return false;
    }
    unsafe {
        (api().patch_bytes.unwrap())(name(), va, expected.as_ptr() as *const c_void, bytes.as_ptr() as *const c_void, bytes.len()) == 0
    }
}

// ---- function detours and other mods (API 16) ---------------------------------------------------------
/// Calls to the game's function at `function_va` go to `detour`, with the same arguments; `*original`
/// receives what to call for the game's behaviour. Taken out when the mod is unloaded.
/// # Safety
/// `detour` must be an `extern "C"` function with the game function's signature, safe on any thread.
pub unsafe fn hook_function(function_va: u64, detour: *const c_void, original: *mut *mut c_void) -> bool {
    (api().hook_function.unwrap())(name(), function_va, detour as *mut c_void, original) == 0
}
/// Offers a function of this mod to other mods as "<its id>.<name>".
pub fn export_fn(fn_name: &str, f: *const c_void) -> bool {
    let n = cstr(fn_name);
    unsafe { (api().mod_export.unwrap())(n.as_ptr(), f as *mut c_void) == 0 }
}
/// Another mod's function (null when that mod is not loaded or does not offer it); cast it to its type.
pub fn import_fn(mod_id: &str, fn_name: &str) -> *mut c_void {
    let (m, n) = (cstr(mod_id), cstr(fn_name));
    unsafe { (api().mod_import.unwrap())(m.as_ptr(), n.as_ptr()) }
}
/// The version of a loaded mod; None when it is not loaded.
pub fn mod_version(mod_id: &str) -> Option<String> {
    let m = cstr(mod_id);
    let mut b = [0 as c_char; 64];
    unsafe {
        if (api().mod_version.unwrap())(m.as_ptr(), b.as_mut_ptr(), b.len()) != 0 {
            return None;
        }
        Some(CStr::from_ptr(b.as_ptr()).to_string_lossy().into_owned())
    }
}

// ---- the free camera (API 13) ---------------------------------------------------------------------
pub use crate::sys::gn_freecam as Freecam;
/// GridNode's free camera on / off (at the next frame); false without Faith.
pub fn freecam(on: bool) -> bool { unsafe { (api().freecam_set.unwrap())(on as c_int) == 0 } }
/// Whether it flies, and where it is.
pub fn freecam_state() -> Option<Freecam> {
    let mut f: Freecam = unsafe { core::mem::zeroed() };
    (unsafe { (api().freecam_get.unwrap())(&mut f) } == 0).then_some(f)
}
/// Faith to where the free camera's crosshair points (`to_camera` false) or to the camera itself.
pub fn freecam_teleport(to_camera: bool, keep_flying: bool) -> bool {
    unsafe { (api().freecam_teleport.unwrap())(if to_camera { 2 } else { 1 }, keep_flying as c_int) == 0 }
}

// ---- the view (API 10) ----------------------------------------------------------------------------
pub use crate::sys::gn_view as View;
/// The camera as drawn this frame, with the screen size; None with no camera. Render thread.
pub fn view() -> Option<View> {
    let mut v: View = unsafe { core::mem::zeroed() };
    (unsafe { (api().view_get.unwrap())(&mut v) } == 0).then_some(v)
}
/// A world point on the screen (pixels, y down), or None when it is behind the camera.
pub fn to_screen(v: &View, p: [f32; 3]) -> Option<[f32; 2]> {
    let q = [p[0] - v.position[0], p[1] - v.position[1], p[2] - v.position[2]];
    let dot = |a: &[f32; 3]| q[0] * a[0] + q[1] * a[1] + q[2] * a[2];
    let z = dot(&v.forward);
    if z < 0.05 || v.height < 1.0 {
        return None;
    }
    let th = (v.fov_y * core::f32::consts::PI / 360.0).tan();
    let (x, y) = (dot(&v.right) / (z * th * v.width / v.height), dot(&v.up) / (z * th));
    Some([(x + 1.0) * 0.5 * v.width, (1.0 - y) * 0.5 * v.height])
}

pub use crate::sys::gn_hotkey_info as HotkeyInfo;
/// The hotkey as it is now (its key and name, the default, whether it waits for a key, clashes).
pub fn hotkey_info(id: &str) -> Option<HotkeyInfo> {
    let i = cstr(id);
    let mut h: HotkeyInfo = unsafe { core::mem::zeroed() };
    (unsafe { (api().hotkey_info.unwrap())(name(), i.as_ptr(), &mut h) } == 0).then_some(h)
}
/// Waits for the next key the player presses (Escape: none, Backspace: the default).
pub fn hotkey_rebind(id: &str) -> bool {
    let i = cstr(id);
    unsafe { (api().hotkey_rebind.unwrap())(name(), i.as_ptr()) == 0 }
}
/// Sets the hotkey's key (a virtual-key code, 0 none, -1 the default).
pub fn hotkey_set(id: &str, vk: i32) -> bool {
    let i = cstr(id);
    unsafe { (api().hotkey_set.unwrap())(name(), i.as_ptr(), vk) == 0 }
}
/// A movement tuning value by name (None until GridNode has verified it this session).
pub fn tuning(name: &str) -> Option<f32> {
    unsafe {
        let a = api();
        for i in 0..(a.tuning_count.unwrap())() {
            let mut t: crate::sys::gn_tuning = core::mem::zeroed();
            if (a.tuning_at.unwrap())(i, &mut t) == 0 && string(t.name) == name {
                return (t.available != 0).then_some(t.value);
            }
        }
    }
    None
}
pub fn set_tuning(name: &str, value: f32) -> bool {
    let c = cstr(name);
    unsafe { (api().tuning_set.unwrap())(c.as_ptr(), value) == 0 }
}
pub fn game_state() -> GameState {
    let mut s: GameState = unsafe { core::mem::zeroed() };
    unsafe { (api().game_state.unwrap())(&mut s) };
    s
}
pub fn set_time_of_day(seconds: f32) -> bool { unsafe { (api().time_of_day_set.unwrap())(seconds) == 0 } }
/// God mode (the game's immortality flags). Server thread.
pub fn god_mode(on: bool) -> bool { unsafe { (api().god_mode.unwrap())(on as c_int) == 0 } }
/// Moves Faith without a teleport (the camera stays free): noclip-style movement every tick.
pub fn set_position(p: [f32; 3]) -> bool { unsafe { (api().position_set.unwrap())(p.as_ptr()) == 0 } }

// ---- assets --------------------------------------------------------------------------------------
/// A loaded asset by path ("Characters/FaithCharacter"), with a reference held until dropped.
pub struct Asset {
    ptr: *mut c_void,
    pub group: u32,
}
impl Asset {
    pub fn find(path: &str) -> Option<Asset> {
        let c = cstr(path);
        let mut group = 0u32;
        let p = unsafe { (api().asset_find.unwrap())(c.as_ptr(), &mut group) };
        (!p.is_null()).then_some(Asset { ptr: p, group })
    }
    pub fn as_ptr(&self) -> *mut c_void { self.ptr }
}
impl Drop for Asset {
    fn drop(&mut self) { unsafe { (api().asset_release.unwrap())(self.ptr) } }
}

// ---- settings, console, messages ------------------------------------------------------------------
/// A settings object by class name (`"GameTimeSettings"`); look it up each time.
pub fn settings(class: &str) -> *mut c_void {
    let c = cstr(class);
    unsafe { (api().settings_find.unwrap())(c.as_ptr()) }
}
unsafe extern "C" fn console_tramp<F: FnOnce(&str)>(out: *const c_char, user: *mut c_void) {
    let f = Box::from_raw(user as *mut F);
    guard("a console callback", (), move || f(&string(out)))
}
/// Runs a console line (client thread); `f(output)` there.
pub fn console<F: FnOnce(&str) + Send + 'static>(line: &str, f: F) -> bool {
    let l = cstr(line);
    unsafe { (api().console_run.unwrap())(name(), l.as_ptr(), Some(console_tramp::<F>), keep(f)) == 0 }
}

/// What [`on_message`] passes along with the message.
pub struct MessageInfo {
    pub type_name: String,
    pub category: String,
    pub type_id: u32,
    pub category_id: u32,
    /// 0 core, 1 client, 2 server.
    pub manager: i32,
}
unsafe extern "C" fn message_tramp<F: FnMut(*const c_void, &MessageInfo)>(m: *const c_void, i: *const sys::gn_message_info, user: *mut c_void) {
    let i = &*i;
    let info = MessageInfo {
        type_name: string(i.type_name),
        category: string(i.category),
        type_id: i.type_id,
        category_id: i.category_id,
        manager: i.manager,
    };
    guard("a message callback", (), || (*(user as *mut F))(m, &info))
}
/// `f(message, info)` for a message type (`"ClientPlayerLocalSetMessage"`) or a category
/// (`"ServerPlayer"`), on the thread that sent it. The message only lives during the call.
pub fn on_message<F: FnMut(*const c_void, &MessageInfo) + Send + 'static>(type_or_category: &str, f: F) -> bool {
    let n = cstr(type_or_category);
    unsafe { (api().on_message.unwrap())(name(), n.as_ptr(), Some(message_tramp::<F>), keep(f)) == 0 }
}
