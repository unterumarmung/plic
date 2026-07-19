use runtime_lib::*;
use std::ffi::CStr;
use std::io::{Read, Write};
use std::mem::MaybeUninit;
use std::net::TcpListener;
use std::sync::{LazyLock, Mutex, MutexGuard};
use std::thread::JoinHandle;

const OK: u32 = 0;
const NOT_FOUND: u32 = 2;
const INVALID_ARGUMENT: u32 = 3;
const TRANSPORT_MAGIC: &[u8; 8] = b"DOLA\0\0\0\x03";

static RUNTIME_TEST_LOCK: LazyLock<Mutex<()>> = LazyLock::new(|| Mutex::new(()));

fn runtime_test_lock() -> MutexGuard<'static, ()> {
    RUNTIME_TEST_LOCK
        .lock()
        .expect("runtime test lock must not be poisoned")
}

fn make_string(value: &str) -> DolaValue {
    let mut result = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_string_create(value.as_ptr(), value.len(), result.as_mut_ptr()),
        OK
    );
    unsafe { result.assume_init() }
}

fn handshake(send_type: u64, receive_type: u64) -> Vec<u8> {
    let mut bytes = Vec::from(TRANSPORT_MAGIC.as_slice());
    bytes.extend_from_slice(&send_type.to_be_bytes());
    bytes.extend_from_slice(&receive_type.to_be_bytes());
    bytes
}

fn raw_transport_server(response: Vec<u8>) -> (DolaValue, JoinHandle<()>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("loopback listener must bind");
    let address = make_string(
        &listener
            .local_addr()
            .expect("listener must have a local address")
            .to_string(),
    );
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().expect("transport client must connect");
        let mut client_handshake = [0_u8; 24];
        stream
            .read_exact(&mut client_handshake)
            .expect("transport client must send its handshake");
        stream
            .write_all(&response)
            .expect("test transport response must be written");
    });
    (address, server)
}

fn last_error() -> String {
    let message = dola_rt_last_error_message();
    assert!(!message.is_null());
    unsafe { CStr::from_ptr(message) }
        .to_string_lossy()
        .into_owned()
}

#[test]
fn reports_abi_version_and_validates_runtime_outputs() {
    let _lock = runtime_test_lock();
    assert_eq!(dola_rt_abi_version(), 4);
    assert_eq!(dola_rt_create(std::ptr::null_mut()), INVALID_ARGUMENT);

    let mut runtime = std::ptr::null_mut();
    assert_eq!(dola_rt_create(&raw mut runtime), OK);
    assert!(!runtime.is_null());
    dola_rt_destroy(runtime);
    dola_rt_destroy(std::ptr::null_mut());
}

#[test]
fn immutable_list_updates_preserve_the_source() {
    let _lock = runtime_test_lock();
    let baseline = dola_rt_live_object_count();
    let mut list = MaybeUninit::uninit();
    assert_eq!(dola_rt_list_create(7, list.as_mut_ptr()), OK);
    let list = unsafe { list.assume_init() };

    let mut updated = MaybeUninit::uninit();
    let placeholder = DolaValue {
        tag: 2,
        reserved: 0,
        bits: 42,
    };
    assert_eq!(
        dola_rt_list_push(list, placeholder, updated.as_mut_ptr()),
        OK
    );
    let updated = unsafe { updated.assume_init() };

    let mut original_length = -1;
    let mut updated_length = -1;
    assert_eq!(dola_rt_list_length(list, &raw mut original_length), OK);
    assert_eq!(dola_rt_list_length(updated, &raw mut updated_length), OK);
    assert_eq!((original_length, updated_length), (0, 1));

    let mut missing = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_list_get(updated, -1, missing.as_mut_ptr()),
        NOT_FOUND
    );
    dola_rt_value_release(updated);
    dola_rt_value_release(list);
    assert_eq!(dola_rt_live_object_count(), baseline);
}

#[test]
fn concurrent_retain_and_read_is_safe() {
    let _lock = runtime_test_lock();
    let mut string = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_string_create(b"shared".as_ptr(), 6, string.as_mut_ptr()),
        OK
    );
    let string = unsafe { string.assume_init() };
    let mut workers = Vec::new();
    for _ in 0..4 {
        let shared = string;
        workers.push(std::thread::spawn(move || {
            for _ in 0..500 {
                dola_rt_value_retain(shared);
                let mut length = 0;
                assert_eq!(dola_rt_string_length(shared, &raw mut length), OK);
                assert_eq!(length, 6);
                dola_rt_value_release(shared);
            }
        }));
    }
    for worker in workers {
        worker.join().expect("runtime worker must not panic");
    }
    dola_rt_value_release(string);
}

#[test]
fn transport_rejects_incompatible_handshake_types() {
    let _lock = runtime_test_lock();
    let (address, server) = raw_transport_server(handshake(99, 98));
    let mut connection = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_transport_connect(address, 11, 22, 33, connection.as_mut_ptr()),
        INVALID_ARGUMENT
    );
    assert!(last_error().contains("incompatible message types"));
    dola_rt_value_release(address);
    server.join().expect("transport server must finish");
}

#[test]
fn transport_rejects_malformed_message_pack_frames() {
    let _lock = runtime_test_lock();
    let mut response = handshake(22, 11);
    response.extend_from_slice(&1_u32.to_be_bytes());
    response.push(0xc1); // Reserved by MessagePack and therefore never a Dola value.
    let (address, server) = raw_transport_server(response);
    let mut connection = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_transport_connect(address, 11, 22, 33, connection.as_mut_ptr()),
        OK
    );
    let connection = unsafe { connection.assume_init() };
    let mut has_value = 1;
    let mut value = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_transport_receive(connection, &raw mut has_value, value.as_mut_ptr()),
        INVALID_ARGUMENT
    );
    assert!(last_error().contains("unsupported MessagePack marker"));
    dola_rt_value_release(connection);
    dola_rt_value_release(address);
    server.join().expect("transport server must finish");
}

#[test]
fn transport_rejects_oversized_frames_before_allocating_payload() {
    let _lock = runtime_test_lock();
    let mut response = handshake(22, 11);
    response.extend_from_slice(&(64_u32 * 1024 + 1).to_be_bytes());
    let (address, server) = raw_transport_server(response);
    let mut connection = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_transport_connect(address, 11, 22, 33, connection.as_mut_ptr()),
        OK
    );
    let connection = unsafe { connection.assume_init() };
    let mut has_value = 1;
    let mut value = MaybeUninit::uninit();
    assert_eq!(
        dola_rt_transport_receive(connection, &raw mut has_value, value.as_mut_ptr()),
        INVALID_ARGUMENT
    );
    assert!(last_error().contains("exceeds the maximum length"));
    dola_rt_value_release(connection);
    dola_rt_value_release(address);
    server.join().expect("transport server must finish");
}
