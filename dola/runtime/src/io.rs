#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_abi_version() -> u32 {
    4
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_live_object_count() -> usize {
    LIVE_OBJECTS.load(Ordering::Relaxed)
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_create(out_runtime: *mut *mut DolaRuntime) -> u32 {
    status(|| {
        if out_runtime.is_null() {
            set_error("out_runtime must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        unsafe {
            ptr::write(
                out_runtime,
                Box::into_raw(Box::new(DolaRuntime {
                    core: Arc::new(RuntimeCore::new()),
                })),
            )
        };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_destroy(runtime: *mut DolaRuntime) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !runtime.is_null() {
            let runtime = unsafe { Box::from_raw(runtime) };
            runtime.core.pool.shutdown();
            drop(runtime);
        }
    }));
}

fn write_bytes(data: *const u8, length: usize, stream: u8) -> u32 {
    let data = match unsafe { bytes(data, length) } {
        Ok(data) => data,
        Err(code) => return code,
    };
    status(|| {
        std::str::from_utf8(data).map_err(|_| internal_error("output is not valid UTF-8"))?;
        if stream == 2 {
            let mut output = io::stderr().lock();
            output.write_all(data).map_err(internal_error)?;
            output.write_all(b"\n").map_err(internal_error)?;
            output.flush().map_err(internal_error)?;
        } else {
            let mut output = io::stdout().lock();
            output.write_all(data).map_err(internal_error)?;
            if stream == 1 {
                output.write_all(b"\n").map_err(internal_error)?;
            }
            output.flush().map_err(internal_error)?;
        }
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_print(data: *const u8, length: usize) -> u32 {
    write_bytes(data, length, 0)
}
#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_println(data: *const u8, length: usize) -> u32 {
    write_bytes(data, length, 1)
}

#[unsafe(no_mangle)]
// The C ABI validates the pointer/length pair before constructing the slice.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_panic(data: *const u8, length: usize) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let data = unsafe { bytes(data, length) }.unwrap_or(b"runtime panic");
        let _ = writeln!(io::stderr(), "panic: {}", String::from_utf8_lossy(data));
    }));
    std::process::abort();
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_value_retain(value: DolaValue) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if value.tag == OBJECT && value.bits != 0 {
            unsafe { Arc::increment_strong_count(value.bits as *const Object) };
        }
    }));
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_value_release(value: DolaValue) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if value.tag == OBJECT && value.bits != 0 {
            unsafe { drop(Arc::from_raw(value.bits as *const Object)) };
        }
    }));
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_value_equal(left: DolaValue, right: DolaValue, out: *mut u8) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("out_equal must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let value = equal(&borrow_value(left)?, &borrow_value(right)?);
        unsafe { ptr::write(out, u8::from(value)) };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
// The C ABI validates all raw pointers and reports invalid arguments as status codes.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_string_create(
    data: *const u8,
    length: usize,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let data = unsafe { bytes(data, length) }?;
        let value = std::str::from_utf8(data).map_err(|_| {
            set_error("string is not valid UTF-8");
            INVALID_ARGUMENT
        })?;
        Ok(write_output(
            out,
            object(0, ObjectData::String(value.to_string())),
        ))
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_string_length(value: DolaValue, out: *mut i64) -> u32 {
    status(|| {
        if out.is_null() {
            set_error("out_length must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let length = string_value(value)?.len() as i64;
        unsafe { ptr::write(out, length) };
        Ok(OK)
    })
}

fn print_owned_string(value: DolaValue, stream: u8) -> u32 {
    match string_value(value) {
        Ok(value) => write_bytes(value.as_ptr(), value.len(), stream),
        Err(code) => code,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_print_value(value: DolaValue) -> u32 {
    print_owned_string(value, 0)
}
#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_println_value(value: DolaValue) -> u32 {
    print_owned_string(value, 1)
}
#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_eprintln_value(value: DolaValue) -> u32 {
    print_owned_string(value, 2)
}
