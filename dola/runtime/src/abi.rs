use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::ffi::{CString, c_char};
use std::io::{self, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::slice;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};

const OK: u32 = 0;
const CLOSED: u32 = 1;
const NOT_FOUND: u32 = 2;
const INVALID_ARGUMENT: u32 = 3;
const PANIC: u32 = 5;
const INTERNAL_ERROR: u32 = 6;

const UNIT: u32 = 0;
const BOOL: u32 = 1;
const INT: u32 = 2;
const FLOAT: u32 = 3;
const OBJECT: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DolaValue {
    pub tag: u32,
    pub reserved: u32,
    pub bits: u64,
}

#[repr(C)]
pub struct DolaRuntime {
    core: Arc<RuntimeCore>,
}

#[repr(C)]
pub struct DolaContext {
    runtime: Arc<RuntimeCore>,
    panic_message: Mutex<Option<String>>,
}

#[derive(Clone)]
enum OwnedValue {
    Unit,
    Bool(bool),
    Int(i64),
    Float(f64),
    Object(Arc<Object>),
}

struct Object {
    type_id: u64,
    data: ObjectData,
}

static LIVE_OBJECTS: AtomicUsize = AtomicUsize::new(0);

impl Drop for Object {
    fn drop(&mut self) {
        match &self.data {
            ObjectData::Sender(channel) => channel.close(),
            ObjectData::Receiver(channel) => channel.close_receiver(),
            ObjectData::Listener(listener) => {
                listener
                    .listener
                    .lock()
                    .unwrap_or_else(|error| error.into_inner())
                    .take();
            }
            ObjectData::Connection(connection) => connection.close(),
            _ => {}
        }
        LIVE_OBJECTS.fetch_sub(1, Ordering::Relaxed);
    }
}

enum ObjectData {
    String(String),
    Tuple(Arc<Vec<OwnedValue>>),
    Record(Arc<Vec<OwnedValue>>),
    Enum {
        variant: u32,
        payloads: Arc<Vec<OwnedValue>>,
    },
    List(Arc<Vec<OwnedValue>>),
    Map {
        key_kind: u32,
        entries: Arc<HashMap<MapKey, OwnedValue>>,
    },
    Sender(Arc<ChannelCore>),
    Receiver(Arc<ChannelCore>),
    Task(Arc<TaskCore>),
    Listener(Arc<ListenerCore>),
    Connection(Arc<ConnectionCore>),
}

#[derive(Clone, Eq, Hash, PartialEq)]
enum MapKey {
    Int(i64),
    String(String),
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}
