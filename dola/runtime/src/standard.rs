fn io_status(error: &io::Error) -> u32 {
    use io::ErrorKind;
    match error.kind() {
        ErrorKind::NotFound => NOT_FOUND,
        ErrorKind::PermissionDenied => INVALID_ARGUMENT,
        _ => INTERNAL_ERROR,
    }
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_read_line(out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        let mut line = String::new();
        if let Err(error) = io::stdin().read_line(&mut line) {
            set_error(error.to_string());
            return Ok(io_status(&error));
        }
        if line.ends_with('\n') {
            line.pop();
            if line.ends_with('\r') {
                line.pop();
            }
        }
        Ok(write_output(out, object(0, ObjectData::String(line))))
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_read_text(path: DolaValue, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        let path = string_value(path)?;
        match std::fs::read_to_string(path) {
            Ok(contents) => Ok(write_output(out, object(0, ObjectData::String(contents)))),
            Err(error) => {
                set_error(error.to_string());
                Ok(io_status(&error))
            }
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_write_text(path: DolaValue, contents: DolaValue) -> u32 {
    status(|| {
        let path = string_value(path)?;
        let contents = string_value(contents)?;
        match std::fs::write(path, contents) {
            Ok(()) => Ok(OK),
            Err(error) => {
                set_error(error.to_string());
                Ok(io_status(&error))
            }
        }
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_unix_seconds(out: *mut i64) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("unix-seconds output must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let seconds = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(internal_error)?
            .as_secs();
        let seconds = i64::try_from(seconds).map_err(internal_error)?;
        unsafe { ptr::write(out, seconds) };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_sleep_ms(milliseconds: i64) -> u32 {
    status(|| {
        if milliseconds < 0 {
            set_error("sleep duration must not be negative");
            return Ok(INVALID_ARGUMENT);
        }
        std::thread::sleep(std::time::Duration::from_millis(
            u64::try_from(milliseconds).map_err(internal_error)?,
        ));
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_string_split_once(
    value: DolaValue,
    delimiter: DolaValue,
    tuple_type_id: u64,
    out_found: *mut u8,
    out_value: *mut DolaValue,
) -> u32 {
    status(|| {
        if out_found.is_null() {
            set_error("split result flag must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        require_output(out_value)?;
        let value = string_value(value)?;
        let delimiter = string_value(delimiter)?;
        if delimiter.is_empty() {
            set_error("split delimiter must not be empty");
            return Ok(INVALID_ARGUMENT);
        }
        if let Some((left, right)) = value.split_once(&delimiter) {
            let tuple = object(
                tuple_type_id,
                ObjectData::Tuple(Arc::new(vec![
                    object(0, ObjectData::String(left.to_string())),
                    object(0, ObjectData::String(right.to_string())),
                ])),
            );
            unsafe { ptr::write(out_found, 1) };
            Ok(write_output(out_value, tuple))
        } else {
            unsafe { ptr::write(out_found, 0) };
            Ok(OK)
        }
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_parse_int(value: DolaValue, out: *mut i64) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("integer output must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let value = string_value(value)?;
        match value.parse::<i64>() {
            Ok(parsed) => {
                unsafe { ptr::write(out, parsed) };
                Ok(OK)
            }
            Err(error) => {
                set_error(error.to_string());
                Ok(INVALID_ARGUMENT)
            }
        }
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_string_concat(
    left: DolaValue,
    right: DolaValue,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let left = string_value(left)?;
        let right = string_value(right)?;
        let mut result = String::with_capacity(left.len().saturating_add(right.len()));
        result.push_str(&left);
        result.push_str(&right);
        Ok(write_output(out, object(0, ObjectData::String(result))))
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_int_to_string(value: i64, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        Ok(write_output(
            out,
            object(0, ObjectData::String(value.to_string())),
        ))
    })
}
