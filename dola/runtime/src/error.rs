fn set_error(message: impl AsRef<str>) {
    let clean = message.as_ref().replace('\0', "\\0");
    LAST_ERROR.with(|slot| *slot.borrow_mut() = CString::new(clean).unwrap());
}

fn status(action: impl FnOnce() -> Result<u32, u32>) -> u32 {
    match catch_unwind(AssertUnwindSafe(action)) {
        Ok(Ok(value)) => value,
        Ok(Err(code)) => code,
        Err(_) => {
            set_error("runtime panic");
            PANIC
        }
    }
}

fn internal_error(message: impl ToString) -> u32 {
    set_error(message.to_string());
    INTERNAL_ERROR
}

unsafe fn bytes<'a>(data: *const u8, length: usize) -> Result<&'a [u8], u32> {
    if data.is_null() && length != 0 {
        set_error("null byte pointer with nonzero length");
        return Err(INVALID_ARGUMENT);
    }
    Ok(if length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, length) }
    })
}

unsafe fn raw_values<'a>(data: *const DolaValue, length: usize) -> Result<&'a [DolaValue], u32> {
    if data.is_null() && length != 0 {
        set_error("null value pointer with nonzero length");
        return Err(INVALID_ARGUMENT);
    }
    Ok(if length == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, length) }
    })
}
