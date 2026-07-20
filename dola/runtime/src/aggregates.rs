fn create_sequence(
    type_id: u64,
    values: *const DolaValue,
    length: usize,
    out: *mut DolaValue,
    kind: u8,
) -> u32 {
    status(|| {
        require_output(out)?;
        let values = Arc::new(owned_slice(values, length)?);
        let data = match kind {
            0 => ObjectData::Tuple(values),
            1 => ObjectData::Record(values),
            _ => ObjectData::List(values),
        };
        Ok(write_output(out, object(type_id, data)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_tuple_create(
    type_id: u64,
    values: *const DolaValue,
    length: usize,
    out: *mut DolaValue,
) -> u32 {
    create_sequence(type_id, values, length, out, 0)
}
#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_record_create(
    type_id: u64,
    values: *const DolaValue,
    length: usize,
    out: *mut DolaValue,
) -> u32 {
    create_sequence(type_id, values, length, out, 1)
}
#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_list_create(type_id: u64, out: *mut DolaValue) -> u32 {
    create_sequence(type_id, ptr::null(), 0, out, 2)
}

fn sequence_get(value: DolaValue, index: usize, out: *mut DolaValue, kind: u8) -> u32 {
    status(|| {
        require_output(out)?;
        let OwnedValue::Object(object) = borrow_value(value)? else {
            set_error("sequence value is not an object");
            return Ok(INVALID_ARGUMENT);
        };
        let values = match (&object.data, kind) {
            (ObjectData::Tuple(values), 0)
            | (ObjectData::Record(values), 1)
            | (ObjectData::List(values), 2) => values,
            _ => {
                set_error("sequence object has the wrong kind");
                return Ok(INVALID_ARGUMENT);
            }
        };
        match values.get(index).cloned() {
            Some(found) => Ok(write_output(out, found)),
            None => Ok(NOT_FOUND),
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_tuple_get(value: DolaValue, index: usize, out: *mut DolaValue) -> u32 {
    sequence_get(value, index, out, 0)
}
#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_record_get(value: DolaValue, index: usize, out: *mut DolaValue) -> u32 {
    sequence_get(value, index, out, 1)
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_record_with(
    value: DolaValue,
    index: usize,
    replacement: DolaValue,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let (type_id, mut fields) = with_object(
            value,
            |object| match &object.data {
                ObjectData::Record(values) => Some((object.type_id, (**values).clone())),
                _ => None,
            },
            "record",
        )?;
        if index >= fields.len() {
            return Ok(NOT_FOUND);
        }
        fields[index] = borrow_value(replacement)?;
        Ok(write_output(
            out,
            object(type_id, ObjectData::Record(Arc::new(fields))),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_enum_create(
    type_id: u64,
    variant: u32,
    payloads: *const DolaValue,
    length: usize,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let payloads = Arc::new(owned_slice(payloads, length)?);
        Ok(write_output(
            out,
            object(type_id, ObjectData::Enum { variant, payloads }),
        ))
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_enum_tag(value: DolaValue, out: *mut u32) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("out_variant must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let variant = with_object(
            value,
            |object| match &object.data {
                ObjectData::Enum { variant, .. } => Some(*variant),
                _ => None,
            },
            "enum",
        )?;
        unsafe { ptr::write(out, variant) };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_enum_payload(value: DolaValue, index: usize, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        let payload = with_object(
            value,
            |object| match &object.data {
                ObjectData::Enum { payloads, .. } => payloads.get(index).cloned(),
                _ => None,
            },
            "enum",
        )?;
        Ok(write_output(out, payload))
    })
}
