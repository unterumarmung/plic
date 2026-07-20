struct ChannelState {
    queue: VecDeque<OwnedValue>,
    closed: bool,
    receiver_open: bool,
}

struct ChannelCore {
    state: Mutex<ChannelState>,
    available: Condvar,
}

impl ChannelCore {
    fn new() -> Self {
        Self {
            state: Mutex::new(ChannelState {
                queue: VecDeque::new(),
                closed: false,
                receiver_open: true,
            }),
            available: Condvar::new(),
        }
    }

    fn close(&self) {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        state.closed = true;
        self.available.notify_all();
    }

    fn close_receiver(&self) {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        state.receiver_open = false;
        self.available.notify_all();
    }

    fn send(&self, value: OwnedValue) -> Result<(), u32> {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        if state.closed || !state.receiver_open {
            set_error("channel is closed");
            return Err(CLOSED);
        }
        state.queue.push_back(value);
        self.available.notify_one();
        Ok(())
    }

    fn receive(&self) -> Option<OwnedValue> {
        let mut state = self.state.lock().unwrap_or_else(|error| error.into_inner());
        loop {
            if let Some(value) = state.queue.pop_front() {
                return Some(value);
            }
            if state.closed {
                return None;
            }
            state = self
                .available
                .wait(state)
                .unwrap_or_else(|error| error.into_inner());
        }
    }
}

fn channel_from(value: DolaValue, sender: bool) -> Result<Arc<ChannelCore>, u32> {
    with_object(
        value,
        |object| match (&object.data, sender) {
            (ObjectData::Sender(channel), true) | (ObjectData::Receiver(channel), false) => {
                Some(Arc::clone(channel))
            }
            _ => None,
        },
        if sender { "sender" } else { "receiver" },
    )
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_channel_create(
    sender_type_id: u64,
    receiver_type_id: u64,
    out_sender: *mut DolaValue,
    out_receiver: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out_sender)?;
        require_output(out_receiver)?;
        let channel = Arc::new(ChannelCore::new());
        let sender = object(sender_type_id, ObjectData::Sender(Arc::clone(&channel)));
        let receiver = object(receiver_type_id, ObjectData::Receiver(channel));
        let sender = into_raw(sender);
        let receiver = into_raw(receiver);
        unsafe {
            ptr::write(out_sender, sender);
            ptr::write(out_receiver, receiver);
        }
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_channel_send(sender: DolaValue, value: DolaValue) -> u32 {
    status(|| {
        channel_from(sender, true)?.send(borrow_value(value)?)?;
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_channel_receive(
    receiver: DolaValue,
    out_has_value: *mut u8,
    out_value: *mut DolaValue,
) -> u32 {
    status(|| {
        if out_has_value.is_null() {
            set_error("out_has_value must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        require_output(out_value)?;
        match channel_from(receiver, false)?.receive() {
            Some(value) => {
                unsafe { ptr::write(out_has_value, 1) };
                Ok(write_output(out_value, value))
            }
            None => {
                unsafe { ptr::write(out_has_value, 0) };
                Ok(OK)
            }
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_channel_close(sender: DolaValue) -> u32 {
    status(|| {
        channel_from(sender, true)?.close();
        Ok(OK)
    })
}
