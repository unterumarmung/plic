pub type DolaTaskEntry =
    unsafe extern "C" fn(*mut DolaContext, *const DolaValue, *mut DolaValue) -> u32;

enum TaskOutcome {
    Pending,
    Complete(Result<OwnedValue, String>),
}

struct TaskCore {
    outcome: Mutex<TaskOutcome>,
    complete: Condvar,
}

impl TaskCore {
    fn new() -> Self {
        Self {
            outcome: Mutex::new(TaskOutcome::Pending),
            complete: Condvar::new(),
        }
    }

    fn publish(&self, value: Result<OwnedValue, String>) {
        let mut outcome = self
            .outcome
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        *outcome = TaskOutcome::Complete(value);
        self.complete.notify_all();
    }

    fn join(&self) -> Result<OwnedValue, String> {
        let mut outcome = self
            .outcome
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        loop {
            if let TaskOutcome::Complete(value) = &*outcome {
                return value.clone();
            }
            outcome = self
                .complete
                .wait(outcome)
                .unwrap_or_else(|error| error.into_inner());
        }
    }
}

fn task_from(value: DolaValue) -> Result<Arc<TaskCore>, u32> {
    match borrow_value(value)? {
        OwnedValue::Object(object) => match &object.data {
            ObjectData::Task(task) => Ok(Arc::clone(task)),
            ObjectData::Tuple(_) => {
                set_error("expected task value, received tuple");
                Err(INVALID_ARGUMENT)
            }
            _ => {
                set_error("expected task value, received another object kind");
                Err(INVALID_ARGUMENT)
            }
        },
        _ => {
            set_error(format!("expected task value, received tag {}", value.tag));
            Err(INVALID_ARGUMENT)
        }
    }
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_context_create(
    runtime: *mut DolaRuntime,
    out_context: *mut *mut DolaContext,
) -> u32 {
    status(|| {
        if runtime.is_null() || out_context.is_null() {
            set_error("runtime and context output must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let runtime = unsafe { &*runtime };
        let context = Box::new(DolaContext {
            runtime: Arc::clone(&runtime.core),
            panic_message: Mutex::new(None),
        });
        unsafe { ptr::write(out_context, Box::into_raw(context)) };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_context_destroy(context: *mut DolaContext) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !context.is_null() {
            unsafe { drop(Box::from_raw(context)) };
        }
    }));
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_context_set_panic(context: *mut DolaContext, message: DolaValue) -> u32 {
    status(|| {
        if context.is_null() {
            set_error("execution context must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let message = string_value(message)?;
        *unsafe { &*context }
            .panic_message
            .lock()
            .unwrap_or_else(|error| error.into_inner()) = Some(message);
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_context_report_panic(context: *mut DolaContext) -> u32 {
    status(|| {
        if context.is_null() {
            set_error("execution context must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        let message = unsafe { &*context }
            .panic_message
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .clone()
            .unwrap_or_else(|| "task panicked".to_string());
        let _ = writeln!(io::stderr().lock(), "panic: {message}");
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen_context_set_panic(
    context: *mut DolaContext,
    message: *const DolaValue,
) -> u32 {
    if message.is_null() {
        set_error("panic message must not be null");
        return INVALID_ARGUMENT;
    }
    dola_rt_context_set_panic(context, unsafe { ptr::read(message) })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_task_spawn(
    context: *mut DolaContext,
    entry: Option<DolaTaskEntry>,
    environment: DolaValue,
    task_type_id: u64,
    out_task: *mut DolaValue,
) -> u32 {
    status(|| {
        if context.is_null() || entry.is_none() {
            set_error("task context and entry must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        require_output(out_task)?;
        let context = unsafe { &*context };
        let runtime = Arc::clone(&context.runtime);
        let task_runtime = Arc::clone(&runtime);
        let environment = borrow_value(environment)?;
        let task = Arc::new(TaskCore::new());
        let worker_task = Arc::clone(&task);
        runtime.pool.submit(Box::new(move || {
            let mut task_context = Box::new(DolaContext {
                runtime: task_runtime,
                panic_message: Mutex::new(None),
            });
            let environment = into_raw(environment);
            let mut output = into_raw(OwnedValue::Unit);
            let code = unsafe {
                entry.expect("entry was validated")(
                    &raw mut *task_context,
                    &raw const environment,
                    &raw mut output,
                )
            };
            dola_rt_value_release(environment);
            let outcome = if code == OK {
                take_value(output).map_err(|_| "task returned an invalid value".to_string())
            } else {
                let message = task_context
                    .panic_message
                    .lock()
                    .unwrap_or_else(|error| error.into_inner())
                    .clone()
                    .unwrap_or_else(|| "task panicked".to_string());
                Err(message)
            };
            worker_task.publish(outcome);
        }))?;
        Ok(write_output(
            out_task,
            object(task_type_id, ObjectData::Task(task)),
        ))
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen_task_spawn(
    context: *mut DolaContext,
    entry: Option<DolaTaskEntry>,
    environment: *const DolaValue,
    task_type_id: u64,
    out_task: *mut DolaValue,
) -> u32 {
    if environment.is_null() {
        set_error("task environment must not be null");
        return INVALID_ARGUMENT;
    }
    let environment = unsafe { ptr::read(environment) };
    let code = dola_rt_task_spawn(context, entry, environment, task_type_id, out_task);
    if code != OK {
        let message = LAST_ERROR.with(|slot| slot.borrow().to_string_lossy().into_owned());
        let _ = writeln!(io::stderr().lock(), "Dola runtime panic: {message}");
        std::process::abort();
    }
    code
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_codegen_tuple_get(
    tuple: *const DolaValue,
    index: usize,
    out_value: *mut DolaValue,
) -> u32 {
    if tuple.is_null() {
        set_error("task environment must not be null");
        return INVALID_ARGUMENT;
    }
    dola_rt_tuple_get(unsafe { ptr::read(tuple) }, index, out_value)
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_task_join(
    task: DolaValue,
    out_panicked: *mut u8,
    out_value: *mut DolaValue,
) -> u32 {
    status(|| {
        if out_panicked.is_null() {
            set_error("out_panicked must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        require_output(out_value)?;
        match task_from(task)?.join() {
            Ok(value) => {
                unsafe { ptr::write(out_panicked, 0) };
                Ok(write_output(out_value, value))
            }
            Err(message) => {
                unsafe { ptr::write(out_panicked, 1) };
                Ok(write_output(
                    out_value,
                    object(0, ObjectData::String(message)),
                ))
            }
        }
    })
}
