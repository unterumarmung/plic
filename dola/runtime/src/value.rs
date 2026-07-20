fn borrow_value(value: DolaValue) -> Result<OwnedValue, u32> {
    match value.tag {
        UNIT => Ok(OwnedValue::Unit),
        BOOL => Ok(OwnedValue::Bool(value.bits != 0)),
        INT => Ok(OwnedValue::Int(value.bits as i64)),
        FLOAT => Ok(OwnedValue::Float(f64::from_bits(value.bits))),
        OBJECT if value.bits != 0 => {
            let pointer = value.bits as *const Object;
            unsafe { Arc::increment_strong_count(pointer) };
            Ok(OwnedValue::Object(unsafe { Arc::from_raw(pointer) }))
        }
        OBJECT => {
            set_error("null object value");
            Err(INVALID_ARGUMENT)
        }
        _ => {
            set_error("invalid value tag");
            Err(INVALID_ARGUMENT)
        }
    }
}

fn take_value(value: DolaValue) -> Result<OwnedValue, u32> {
    match value.tag {
        UNIT => Ok(OwnedValue::Unit),
        BOOL => Ok(OwnedValue::Bool(value.bits != 0)),
        INT => Ok(OwnedValue::Int(value.bits as i64)),
        FLOAT => Ok(OwnedValue::Float(f64::from_bits(value.bits))),
        OBJECT if value.bits != 0 => Ok(OwnedValue::Object(unsafe {
            Arc::from_raw(value.bits as *const Object)
        })),
        _ => {
            set_error("invalid owned value");
            Err(INVALID_ARGUMENT)
        }
    }
}

fn into_raw(value: OwnedValue) -> DolaValue {
    match value {
        OwnedValue::Unit => DolaValue {
            tag: UNIT,
            reserved: 0,
            bits: 0,
        },
        OwnedValue::Bool(value) => DolaValue {
            tag: BOOL,
            reserved: 0,
            bits: u64::from(value),
        },
        OwnedValue::Int(value) => DolaValue {
            tag: INT,
            reserved: 0,
            bits: value as u64,
        },
        OwnedValue::Float(value) => DolaValue {
            tag: FLOAT,
            reserved: 0,
            bits: value.to_bits(),
        },
        OwnedValue::Object(value) => DolaValue {
            tag: OBJECT,
            reserved: 0,
            bits: Arc::into_raw(value) as u64,
        },
    }
}

fn object(type_id: u64, data: ObjectData) -> OwnedValue {
    LIVE_OBJECTS.fetch_add(1, Ordering::Relaxed);
    OwnedValue::Object(Arc::new(Object { type_id, data }))
}

fn write_output(out: *mut DolaValue, value: OwnedValue) -> u32 {
    if out.is_null() {
        set_error("output value must not be null");
        return INVALID_ARGUMENT;
    }
    unsafe { ptr::write(out, into_raw(value)) };
    OK
}

fn require_output(out: *mut DolaValue) -> Result<(), u32> {
    if out.is_null() {
        set_error("output value must not be null");
        return Err(INVALID_ARGUMENT);
    }
    Ok(())
}

fn with_object<T>(
    value: DolaValue,
    access: impl FnOnce(&Object) -> Option<T>,
    expected: &str,
) -> Result<T, u32> {
    match borrow_value(value)? {
        OwnedValue::Object(value) => access(&value).ok_or_else(|| {
            set_error(format!("expected {expected} value"));
            INVALID_ARGUMENT
        }),
        _ => {
            set_error(format!("expected {expected} value"));
            Err(INVALID_ARGUMENT)
        }
    }
}

fn owned_slice(values: *const DolaValue, length: usize) -> Result<Vec<OwnedValue>, u32> {
    let values = unsafe { raw_values(values, length) }?;
    values.iter().copied().map(borrow_value).collect()
}

fn equal(left: &OwnedValue, right: &OwnedValue) -> bool {
    match (left, right) {
        (OwnedValue::Unit, OwnedValue::Unit) => true,
        (OwnedValue::Bool(left), OwnedValue::Bool(right)) => left == right,
        (OwnedValue::Int(left), OwnedValue::Int(right)) => left == right,
        (OwnedValue::Float(left), OwnedValue::Float(right)) => left == right,
        (OwnedValue::Object(left), OwnedValue::Object(right)) => object_equal(left, right),
        _ => false,
    }
}

fn object_equal(left: &Object, right: &Object) -> bool {
    if left.type_id != right.type_id {
        return false;
    }
    match (&left.data, &right.data) {
        (ObjectData::String(left), ObjectData::String(right)) => left == right,
        (ObjectData::Tuple(left), ObjectData::Tuple(right))
        | (ObjectData::Record(left), ObjectData::Record(right))
        | (ObjectData::List(left), ObjectData::List(right)) => {
            left.len() == right.len() && left.iter().zip(right.iter()).all(|(l, r)| equal(l, r))
        }
        (
            ObjectData::Enum {
                variant: lv,
                payloads: lp,
            },
            ObjectData::Enum {
                variant: rv,
                payloads: rp,
            },
        ) => lv == rv && lp.len() == rp.len() && lp.iter().zip(rp.iter()).all(|(l, r)| equal(l, r)),
        (
            ObjectData::Map {
                key_kind: lk,
                entries: le,
            },
            ObjectData::Map {
                key_kind: rk,
                entries: re,
            },
        ) => {
            lk == rk
                && le.len() == re.len()
                && le
                    .iter()
                    .all(|(key, value)| re.get(key).is_some_and(|other| equal(value, other)))
        }
        (ObjectData::Sender(_), ObjectData::Sender(_))
        | (ObjectData::Receiver(_), ObjectData::Receiver(_))
        | (ObjectData::Task(_), ObjectData::Task(_)) => false,
        _ => false,
    }
}

fn key(value: DolaValue, kind: u32) -> Result<MapKey, u32> {
    match (kind, borrow_value(value)?) {
        (0, OwnedValue::Int(value)) => Ok(MapKey::Int(value)),
        (1, OwnedValue::Object(value)) => match &value.data {
            ObjectData::String(value) => Ok(MapKey::String(value.clone())),
            _ => {
                set_error("map key must be `String`");
                Err(INVALID_ARGUMENT)
            }
        },
        (0, _) => {
            set_error("map key must be `Int`");
            Err(INVALID_ARGUMENT)
        }
        (1, _) => {
            set_error("map key must be `String`");
            Err(INVALID_ARGUMENT)
        }
        _ => {
            set_error("invalid map key kind");
            Err(INVALID_ARGUMENT)
        }
    }
}

fn string_value(value: DolaValue) -> Result<String, u32> {
    with_object(
        value,
        |object| match &object.data {
            ObjectData::String(value) => Some(value.clone()),
            _ => None,
        },
        "string",
    )
}
