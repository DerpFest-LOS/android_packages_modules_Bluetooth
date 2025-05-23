//! Shared data-types and utility methods go here.

pub mod address;
mod ffi;
pub mod shared_box;
pub mod shared_mutex;
pub mod uuid;

use std::{pin::Pin, rc::Rc, thread};

use cxx::UniquePtr;

use crate::{
    gatt::ffi::{AttTransportImpl, GattCallbacksImpl},
    GlobalModuleRegistry, MainThreadTxMessage, GLOBAL_MODULE_REGISTRY,
};

use self::ffi::{future_ready, Future, GattServerCallbacks};

fn start(
    gatt_server_callbacks: UniquePtr<GattServerCallbacks>,
    on_started: Pin<&'static mut Future>,
) {
    thread::spawn(move || {
        GlobalModuleRegistry::start(
            Rc::new(GattCallbacksImpl(gatt_server_callbacks)),
            Rc::new(AttTransportImpl()),
            || {
                future_ready(on_started);
            },
        );
    });
}

fn stop() {
    let _ = GLOBAL_MODULE_REGISTRY
        .try_lock()
        .unwrap()
        .as_ref()
        .map(|registry| registry.task_tx.send(MainThreadTxMessage::Stop));
}
