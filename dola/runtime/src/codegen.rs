fn enum_value(type_id: u64, variant: u32, payloads: Vec<OwnedValue>) -> OwnedValue {
    object(
        type_id,
        ObjectData::Enum {
            variant,
            payloads: Arc::new(payloads),
        },
    )
}

fn last_error_string() -> OwnedValue {
    let message = LAST_ERROR.with(|slot| slot.borrow().to_string_lossy().into_owned());
    object(0, ObjectData::String(message))
}

fn result_ok(type_id: u64, value: OwnedValue) -> OwnedValue {
    enum_value(type_id, 0, vec![value])
}

fn result_error(type_id: u64, variant: u32, message: bool) -> OwnedValue {
    let payload = if message {
        vec![last_error_string()]
    } else {
        Vec::new()
    };
    enum_value(type_id, 1, vec![enum_value(0, variant, payload)])
}

fn transport_result_error(type_id: u64, code: u32) -> OwnedValue {
    let message = LAST_ERROR.with(|slot| slot.borrow().to_string_lossy().into_owned());
    let variant = if message.contains("address already in use") {
        0
    } else if code == CLOSED || message.contains("closed") {
        1
    } else if message.contains("reset") || message.contains("broken pipe") {
        2
    } else if message.contains("incompatible message types")
        || message.contains("invalid Dola transport handshake")
    {
        3
    } else if message.contains("maximum frame length")
        || message.contains("maximum length")
        || message.contains("too large")
    {
        5
    } else if message.contains("MessagePack")
        || message.contains("transport value type")
        || message.contains("trailing data")
    {
        4
    } else {
        6
    };
    result_error(type_id, variant, variant == 6)
}

fn integer_input(input: DolaValue) -> Result<i64, u32> {
    match borrow_value(input)? {
        OwnedValue::Int(value) => Ok(value),
        _ => {
            set_error("codegen input must be an integer");
            Err(INVALID_ARGUMENT)
        }
    }
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments, clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen(
    operation: u32,
    result_type_id: u64,
    metadata0: i64,
    _metadata1: i64,
    inputs: *const DolaValue,
    input_count: usize,
    out: *mut DolaValue,
) -> u32 {
    let code = status(|| {
        if out.is_null() {
            set_error("codegen output must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let raw = unsafe { raw_values(inputs, input_count)? };
        let operation = RuntimeOperation::try_from(operation).map_err(|()| {
            set_error(format!("unknown codegen operation {operation}"));
            INVALID_ARGUMENT
        })?;
        let input = |index: usize| {
            raw.get(index).copied().ok_or_else(|| {
                set_error("missing codegen input");
                INVALID_ARGUMENT
            })
        };
        let mut temporary = into_raw(OwnedValue::Unit);
        let result = match operation {
            RuntimeOperation::TupleCreate => object(
                result_type_id,
                ObjectData::Tuple(Arc::new(owned_slice(inputs, input_count)?)),
            ),
            RuntimeOperation::TupleGet => {
                let code = dola_rt_tuple_get(input(0)?, metadata0 as usize, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::RecordCreate => object(
                result_type_id,
                ObjectData::Record(Arc::new(owned_slice(inputs, input_count)?)),
            ),
            RuntimeOperation::RecordGet => {
                let code = dola_rt_record_get(input(0)?, metadata0 as usize, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::RecordWith => {
                let code = dola_rt_record_with(
                    input(0)?,
                    metadata0 as usize,
                    input(1)?,
                    &raw mut temporary,
                );
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::EnumCreate => enum_value(
                result_type_id,
                metadata0 as u32,
                owned_slice(inputs, input_count)?,
            ),
            RuntimeOperation::EnumTag => {
                let mut tag = 0;
                let code = dola_rt_enum_tag(input(0)?, &raw mut tag);
                if code != OK {
                    return Ok(code);
                }
                OwnedValue::Int(i64::from(tag))
            }
            RuntimeOperation::EnumPayload => {
                let code = dola_rt_enum_payload(input(0)?, metadata0 as usize, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::ListCreate => {
                object(result_type_id, ObjectData::List(Arc::new(Vec::new())))
            }
            RuntimeOperation::ListLength | RuntimeOperation::ListIsEmpty => {
                let mut length = 0;
                let code = dola_rt_list_length(input(0)?, &raw mut length);
                if code != OK {
                    return Ok(code);
                }
                if operation == RuntimeOperation::ListIsEmpty {
                    OwnedValue::Bool(length == 0)
                } else {
                    OwnedValue::Int(length)
                }
            }
            RuntimeOperation::ListGet | RuntimeOperation::ListGetUnchecked => {
                let code = dola_rt_list_get(
                    input(0)?,
                    match borrow_value(input(1)?)? {
                        OwnedValue::Int(value) => value,
                        _ => return Ok(INVALID_ARGUMENT),
                    },
                    &raw mut temporary,
                );
                if operation == RuntimeOperation::ListGetUnchecked {
                    if code != OK {
                        return Ok(code);
                    }
                    take_value(temporary)?
                } else if code == NOT_FOUND {
                    enum_value(result_type_id, 0, vec![])
                } else if code == OK {
                    enum_value(result_type_id, 1, vec![take_value(temporary)?])
                } else {
                    return Ok(code);
                }
            }
            RuntimeOperation::ListPush => {
                let code = dola_rt_list_push(input(0)?, input(1)?, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::ListSet | RuntimeOperation::ListRemoveAt => {
                let index = match borrow_value(input(1)?)? {
                    OwnedValue::Int(value) => value,
                    _ => return Ok(INVALID_ARGUMENT),
                };
                let code = if operation == RuntimeOperation::ListSet {
                    dola_rt_list_set(input(0)?, index, input(2)?, &raw mut temporary)
                } else {
                    dola_rt_list_remove_at(input(0)?, index, &raw mut temporary)
                };
                if code == OK {
                    enum_value(result_type_id, 0, vec![take_value(temporary)?])
                } else if code == NOT_FOUND {
                    enum_value(result_type_id, 1, vec![enum_value(0, 0, vec![])])
                } else {
                    return Ok(code);
                }
            }
            RuntimeOperation::ListTakeLast => {
                let count = match borrow_value(input(1)?)? {
                    OwnedValue::Int(value) => value,
                    _ => return Ok(INVALID_ARGUMENT),
                };
                let code = dola_rt_list_take_last(input(0)?, count, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::MapCreate => object(
                result_type_id,
                ObjectData::Map {
                    key_kind: metadata0 as u32,
                    entries: Arc::new(HashMap::new()),
                },
            ),
            RuntimeOperation::MapLength | RuntimeOperation::MapIsEmpty => {
                let mut length = 0;
                let code = dola_rt_map_length(input(0)?, &raw mut length);
                if code != OK {
                    return Ok(code);
                }
                if operation == RuntimeOperation::MapIsEmpty {
                    OwnedValue::Bool(length == 0)
                } else {
                    OwnedValue::Int(length)
                }
            }
            RuntimeOperation::MapContains => {
                let mut value = 0;
                let code = dola_rt_map_contains(input(0)?, input(1)?, &raw mut value);
                if code != OK {
                    return Ok(code);
                }
                OwnedValue::Bool(value != 0)
            }
            RuntimeOperation::MapGet => {
                let mut found = 0;
                let code =
                    dola_rt_map_get(input(0)?, input(1)?, &raw mut found, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                if found == 0 {
                    enum_value(result_type_id, 0, vec![])
                } else {
                    enum_value(result_type_id, 1, vec![take_value(temporary)?])
                }
            }
            RuntimeOperation::MapInsert => {
                let code = dola_rt_map_insert(input(0)?, input(1)?, input(2)?, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::MapRemove => {
                let code = dola_rt_map_remove(input(0)?, input(1)?, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::MapEntries => {
                let code = dola_rt_map_entries(
                    input(0)?,
                    metadata0 as u64,
                    result_type_id,
                    &raw mut temporary,
                );
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::StringLength => {
                let mut length = 0;
                let code = dola_rt_string_length(input(0)?, &raw mut length);
                if code != OK {
                    return Ok(code);
                }
                OwnedValue::Int(length)
            }
            RuntimeOperation::StringSplitOnce => {
                let mut found = 0;
                let code = dola_rt_string_split_once(
                    input(0)?,
                    input(1)?,
                    metadata0 as u64,
                    &raw mut found,
                    &raw mut temporary,
                );
                if code != OK {
                    return Ok(code);
                }
                if found == 0 {
                    enum_value(result_type_id, 0, vec![])
                } else {
                    enum_value(result_type_id, 1, vec![take_value(temporary)?])
                }
            }
            RuntimeOperation::ParseInt => {
                let mut parsed = 0;
                let code = dola_rt_parse_int(input(0)?, &raw mut parsed);
                if code == OK {
                    result_ok(result_type_id, OwnedValue::Int(parsed))
                } else if code == INVALID_ARGUMENT {
                    let message =
                        LAST_ERROR.with(|slot| slot.borrow().to_string_lossy().into_owned());
                    result_error(
                        result_type_id,
                        u32::from(message.contains("too large") || message.contains("too small")),
                        false,
                    )
                } else {
                    return Ok(code);
                }
            }
            RuntimeOperation::ReadLine | RuntimeOperation::ReadText => {
                let code = if operation == RuntimeOperation::ReadLine {
                    dola_rt_read_line(&raw mut temporary)
                } else {
                    dola_rt_read_text(input(0)?, &raw mut temporary)
                };
                if code == OK {
                    result_ok(result_type_id, take_value(temporary)?)
                } else {
                    let variant = if code == NOT_FOUND { 0 } else { 3 };
                    result_error(result_type_id, variant, variant == 3)
                }
            }
            RuntimeOperation::WriteText => {
                let code = dola_rt_write_text(input(0)?, input(1)?);
                if code == OK {
                    result_ok(result_type_id, OwnedValue::Unit)
                } else {
                    let variant = if code == NOT_FOUND { 0 } else { 3 };
                    result_error(result_type_id, variant, variant == 3)
                }
            }
            RuntimeOperation::UnixSeconds => {
                let mut seconds = 0;
                let code = dola_rt_unix_seconds(&raw mut seconds);
                if code != OK {
                    return Ok(code);
                }
                OwnedValue::Int(seconds)
            }
            RuntimeOperation::SleepMilliseconds => {
                let code = dola_rt_sleep_ms(integer_input(input(0)?)?);
                if code != OK {
                    return Ok(code);
                }
                OwnedValue::Unit
            }
            RuntimeOperation::ChannelCreate => {
                let code = dola_rt_channel_create(0, 0, &raw mut temporary, out);
                if code != OK {
                    return Ok(code);
                }
                let receiver = take_value(unsafe { ptr::read(out) })?;
                let sender = take_value(temporary)?;
                object(
                    result_type_id,
                    ObjectData::Tuple(Arc::new(vec![sender, receiver])),
                )
            }
            RuntimeOperation::ChannelSend => {
                let code = dola_rt_channel_send(input(0)?, input(1)?);
                if code == OK {
                    result_ok(result_type_id, OwnedValue::Unit)
                } else if code == CLOSED {
                    result_error(result_type_id, 0, false)
                } else {
                    return Ok(code);
                }
            }
            RuntimeOperation::ChannelClose => {
                let code = dola_rt_channel_close(input(0)?);
                if code != OK {
                    return Ok(code);
                }
                OwnedValue::Unit
            }
            RuntimeOperation::ChannelReceive => {
                let mut found = 0;
                let code = dola_rt_channel_receive(input(0)?, &raw mut found, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                if found == 0 {
                    enum_value(result_type_id, 0, vec![])
                } else {
                    enum_value(result_type_id, 1, vec![take_value(temporary)?])
                }
            }
            RuntimeOperation::TaskJoin => {
                let mut panicked = 0;
                let code = dola_rt_task_join(input(0)?, &raw mut panicked, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                if panicked == 0 {
                    result_ok(result_type_id, take_value(temporary)?)
                } else {
                    enum_value(
                        result_type_id,
                        1,
                        vec![enum_value(0, 0, vec![take_value(temporary)?])],
                    )
                }
            }
            RuntimeOperation::TransportListen | RuntimeOperation::TransportConnect => {
                let first = integer_input(input(1)?)? as u64;
                let second = integer_input(input(2)?)? as u64;
                let code = if operation == RuntimeOperation::TransportListen {
                    dola_rt_transport_listen(input(0)?, first, second, 0, &raw mut temporary)
                } else {
                    // A source Connection[Incoming, Outgoing] receives the first
                    // type and sends the second; the runtime ABI names these in
                    // wire direction order.
                    dola_rt_transport_connect(input(0)?, second, first, 0, &raw mut temporary)
                };
                if code == OK {
                    result_ok(result_type_id, take_value(temporary)?)
                } else {
                    transport_result_error(result_type_id, code)
                }
            }
            RuntimeOperation::TransportAccept | RuntimeOperation::TransportLocalAddress => {
                let code = if operation == RuntimeOperation::TransportAccept {
                    dola_rt_transport_accept(input(0)?, 0, &raw mut temporary)
                } else {
                    dola_rt_transport_local_address(input(0)?, &raw mut temporary)
                };
                if code == OK {
                    result_ok(result_type_id, take_value(temporary)?)
                } else {
                    transport_result_error(result_type_id, code)
                }
            }
            RuntimeOperation::TransportClose => {
                let code = dola_rt_transport_close(input(0)?);
                if code != OK {
                    return Ok(code);
                }
                OwnedValue::Unit
            }
            RuntimeOperation::TransportSend => {
                let code = dola_rt_transport_send(input(0)?, input(1)?);
                if code == OK {
                    result_ok(result_type_id, OwnedValue::Unit)
                } else {
                    transport_result_error(result_type_id, code)
                }
            }
            RuntimeOperation::TransportReceive => {
                let mut found = 0;
                let code = dola_rt_transport_receive(input(0)?, &raw mut found, &raw mut temporary);
                if code == OK {
                    let option = if found == 0 {
                        enum_value(0, 0, vec![])
                    } else {
                        enum_value(0, 1, vec![take_value(temporary)?])
                    };
                    result_ok(result_type_id, option)
                } else {
                    transport_result_error(result_type_id, code)
                }
            }
            RuntimeOperation::Panic => {
                let message = string_value(input(0)?)?;
                let _ = writeln!(io::stderr().lock(), "panic: {message}");
                std::process::abort();
            }
            RuntimeOperation::StringConcat => {
                let code = dola_rt_string_concat(input(0)?, input(1)?, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
            RuntimeOperation::IntToString => {
                let code = dola_rt_int_to_string(integer_input(input(0)?)?, &raw mut temporary);
                if code != OK {
                    return Ok(code);
                }
                take_value(temporary)?
            }
        };
        Ok(write_output(out, result))
    });
    if code != OK {
        let message = LAST_ERROR.with(|slot| slot.borrow().to_string_lossy().into_owned());
        let _ = writeln!(io::stderr().lock(), "Dola runtime panic: {message}");
        std::process::abort();
    }
    code
}

#[unsafe(no_mangle)]
// Compiler-generated callers pass a validated address to an ABI-sized value.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_result_main(result: *const DolaValue) -> i32 {
    let code = catch_unwind(AssertUnwindSafe(|| -> Result<i32, u32> {
        if result.is_null() {
            set_error("Result main pointer is null");
            return Err(INVALID_ARGUMENT);
        }
        let result = unsafe { ptr::read(result) };
        let value = borrow_value(result)?;
        dola_rt_value_release(result);
        match value {
            OwnedValue::Object(object) => match &object.data {
                ObjectData::Enum { variant: 0, .. } => Ok(0),
                ObjectData::Enum {
                    variant: 1,
                    payloads,
                } => {
                    if let Some(OwnedValue::Object(message)) = payloads.first()
                        && let ObjectData::String(text) = &message.data
                    {
                        let _ = writeln!(io::stderr().lock(), "{text}");
                        Ok(1)
                    } else {
                        let _ = writeln!(
                            io::stderr().lock(),
                            "Result main Err payload is not a string"
                        );
                        set_error("Result main error payload is not a string");
                        Ok(1)
                    }
                }
                _ => {
                    let _ = writeln!(
                        io::stderr().lock(),
                        "Result main returned the wrong object kind"
                    );
                    set_error("Result main returned the wrong object kind");
                    Ok(1)
                }
            },
            _ => {
                let _ = writeln!(
                    io::stderr().lock(),
                    "Result main returned a primitive value"
                );
                set_error("Result main returned a primitive value");
                Ok(1)
            }
        }
    }));
    match code {
        Ok(Ok(value)) => value,
        Ok(Err(_)) => {
            let _ = writeln!(
                io::stderr().lock(),
                "Result main contained an invalid value"
            );
            1
        }
        Err(_) => {
            let _ = writeln!(io::stderr().lock(), "Result main handling panicked");
            1
        }
    }
}

#[unsafe(no_mangle)]
// Compiler-generated callers pass a validated address to an ABI-sized value.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen_retain(value: *const DolaValue) {
    if !value.is_null() {
        dola_rt_value_retain(unsafe { ptr::read(value) });
    }
}

#[unsafe(no_mangle)]
// Compiler-generated callers pass a validated address to an ABI-sized value.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen_release(value: *const DolaValue) {
    if !value.is_null() {
        dola_rt_value_release(unsafe { ptr::read(value) });
    }
}

#[unsafe(no_mangle)]
// Compiler-generated callers pass validated addresses and the output is checked below.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen_equal(
    left: *const DolaValue,
    right: *const DolaValue,
    out: *mut u8,
) -> u32 {
    if left.is_null() || right.is_null() {
        set_error("codegen equality value pointer is null");
        return INVALID_ARGUMENT;
    }
    dola_rt_value_equal(unsafe { ptr::read(left) }, unsafe { ptr::read(right) }, out)
}

#[unsafe(no_mangle)]
// Compiler-generated callers pass a validated address to an ABI-sized value.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen_print(value: *const DolaValue, newline: u8) -> u32 {
    if value.is_null() {
        set_error("codegen print value pointer is null");
        return INVALID_ARGUMENT;
    }
    let value = unsafe { ptr::read(value) };
    if newline == 0 {
        dola_rt_print_value(value)
    } else {
        dola_rt_println_value(value)
    }
}
