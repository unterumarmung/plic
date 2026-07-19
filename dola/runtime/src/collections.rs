#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_list_length(value: DolaValue, out: *mut i64) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("out_length must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let length = with_object(
            value,
            |object| match &object.data {
                ObjectData::List(values) => Some(values.len() as i64),
                _ => None,
            },
            "list",
        )?;
        unsafe { ptr::write(out, length) };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_list_get(value: DolaValue, index: i64, out: *mut DolaValue) -> u32 {
    if let Err(code) = require_output(out) {
        return code;
    }
    if index < 0 {
        return NOT_FOUND;
    }
    sequence_get(value, index as usize, out, 2)
}

fn list_values(value: DolaValue) -> Result<(u64, Vec<OwnedValue>), u32> {
    with_object(
        value,
        |object| match &object.data {
            ObjectData::List(values) => Some((object.type_id, (**values).clone())),
            _ => None,
        },
        "list",
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_list_push(list: DolaValue, value: DolaValue, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        let (type_id, mut values) = list_values(list)?;
        values.push(borrow_value(value)?);
        Ok(write_output(
            out,
            object(type_id, ObjectData::List(Arc::new(values))),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_list_set(
    list: DolaValue,
    index: i64,
    value: DolaValue,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let (type_id, mut values) = list_values(list)?;
        if index < 0 || index as usize >= values.len() {
            return Ok(NOT_FOUND);
        }
        values[index as usize] = borrow_value(value)?;
        Ok(write_output(
            out,
            object(type_id, ObjectData::List(Arc::new(values))),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_list_remove_at(list: DolaValue, index: i64, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        let (type_id, mut values) = list_values(list)?;
        if index < 0 || index as usize >= values.len() {
            return Ok(NOT_FOUND);
        }
        values.remove(index as usize);
        Ok(write_output(
            out,
            object(type_id, ObjectData::List(Arc::new(values))),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_list_take_last(list: DolaValue, count: i64, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        if count < 0 {
            set_error("take_last count must not be negative");
            return Ok(INVALID_ARGUMENT);
        }
        let (type_id, values) = list_values(list)?;
        let start = values.len().saturating_sub(count as usize);
        Ok(write_output(
            out,
            object(
                type_id,
                ObjectData::List(Arc::new(values[start..].to_vec())),
            ),
        ))
    })
}

fn map_values(value: DolaValue) -> Result<(u64, u32, HashMap<MapKey, OwnedValue>), u32> {
    with_object(
        value,
        |object| match &object.data {
            ObjectData::Map { key_kind, entries } => {
                Some((object.type_id, *key_kind, (**entries).clone()))
            }
            _ => None,
        },
        "map",
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_map_create(type_id: u64, key_kind: u32, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        if key_kind > 1 {
            set_error("invalid map key kind");
            return Ok(INVALID_ARGUMENT);
        }
        Ok(write_output(
            out,
            object(
                type_id,
                ObjectData::Map {
                    key_kind,
                    entries: Arc::new(HashMap::new()),
                },
            ),
        ))
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_map_length(map: DolaValue, out: *mut i64) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("out_length must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let (_, _, entries) = map_values(map)?;
        unsafe { ptr::write(out, entries.len() as i64) };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_map_contains(map: DolaValue, raw_key: DolaValue, out: *mut u8) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("out_contains must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let (_, kind, entries) = map_values(map)?;
        unsafe { ptr::write(out, u8::from(entries.contains_key(&key(raw_key, kind)?))) };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_map_get(
    map: DolaValue,
    raw_key: DolaValue,
    found: *mut u8,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        if found.is_null() {
            set_error("out_found must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        require_output(out)?;
        let (_, kind, entries) = map_values(map)?;
        if let Some(value) = entries.get(&key(raw_key, kind)?) {
            unsafe { ptr::write(found, 1) };
            Ok(write_output(out, value.clone()))
        } else {
            unsafe { ptr::write(found, 0) };
            Ok(OK)
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_map_insert(
    map: DolaValue,
    raw_key: DolaValue,
    value: DolaValue,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let (type_id, kind, mut entries) = map_values(map)?;
        entries.insert(key(raw_key, kind)?, borrow_value(value)?);
        Ok(write_output(
            out,
            object(
                type_id,
                ObjectData::Map {
                    key_kind: kind,
                    entries: Arc::new(entries),
                },
            ),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_map_remove(
    map: DolaValue,
    raw_key: DolaValue,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let (type_id, kind, mut entries) = map_values(map)?;
        entries.remove(&key(raw_key, kind)?);
        Ok(write_output(
            out,
            object(
                type_id,
                ObjectData::Map {
                    key_kind: kind,
                    entries: Arc::new(entries),
                },
            ),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_map_entries(
    map: DolaValue,
    tuple_type_id: u64,
    list_type_id: u64,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let (_, _, entries) = map_values(map)?;
        let mut values = Vec::with_capacity(entries.len());
        for (key, value) in entries {
            let key = match key {
                MapKey::Int(value) => OwnedValue::Int(value),
                MapKey::String(value) => object(0, ObjectData::String(value)),
            };
            values.push(object(
                tuple_type_id,
                ObjectData::Tuple(Arc::new(vec![key, value])),
            ));
        }
        Ok(write_output(
            out,
            object(list_type_id, ObjectData::List(Arc::new(values))),
        ))
    })
}
