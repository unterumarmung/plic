const TRANSPORT_MAGIC: &[u8; 8] = b"DOLA\0\0\0\x03";
const MAX_FRAME_LENGTH: usize = 2 * 1024 * 1024;

struct ListenerCore {
    listener: Mutex<Option<TcpListener>>,
    request_type: u64,
    response_type: u64,
}

struct ConnectionCore {
    reader: Mutex<Option<TcpStream>>,
    writer: Mutex<Option<TcpStream>>,
    send_type: u64,
    receive_type: u64,
}

impl ConnectionCore {
    fn new(stream: TcpStream, send_type: u64, receive_type: u64) -> Result<Self, u32> {
        let writer = stream.try_clone().map_err(transport_error)?;
        Ok(Self {
            reader: Mutex::new(Some(stream)),
            writer: Mutex::new(Some(writer)),
            send_type,
            receive_type,
        })
    }

    fn close(&self) {
        for stream in [&self.reader, &self.writer] {
            if let Some(stream) = stream
                .lock()
                .unwrap_or_else(|error| error.into_inner())
                .take()
            {
                let _ = stream.shutdown(Shutdown::Both);
            }
        }
    }
}

fn transport_error(error: impl std::fmt::Display) -> u32 {
    set_error(error.to_string());
    INTERNAL_ERROR
}

fn listener_from(value: DolaValue) -> Result<Arc<ListenerCore>, u32> {
    with_object(
        value,
        |object| match &object.data {
            ObjectData::Listener(listener) => Some(Arc::clone(listener)),
            _ => None,
        },
        "listener",
    )
}

fn connection_from(value: DolaValue) -> Result<Arc<ConnectionCore>, u32> {
    with_object(
        value,
        |object| match &object.data {
            ObjectData::Connection(connection) => Some(Arc::clone(connection)),
            _ => None,
        },
        "connection",
    )
}

fn write_handshake(stream: &mut TcpStream, send_type: u64, receive_type: u64) -> io::Result<()> {
    stream.write_all(TRANSPORT_MAGIC)?;
    stream.write_all(&send_type.to_be_bytes())?;
    stream.write_all(&receive_type.to_be_bytes())?;
    stream.flush()
}

fn read_handshake(stream: &mut TcpStream) -> io::Result<(u64, u64)> {
    use std::io::Read;
    let mut magic = [0_u8; 8];
    stream.read_exact(&mut magic)?;
    if &magic != TRANSPORT_MAGIC {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid Dola transport handshake",
        ));
    }
    let mut first = [0_u8; 8];
    let mut second = [0_u8; 8];
    stream.read_exact(&mut first)?;
    stream.read_exact(&mut second)?;
    Ok((u64::from_be_bytes(first), u64::from_be_bytes(second)))
}

fn encode_unsigned(output: &mut Vec<u8>, value: u64) {
    output.push(0xcf);
    output.extend_from_slice(&value.to_be_bytes());
}

fn encode_signed(output: &mut Vec<u8>, value: i64) {
    output.push(0xd3);
    output.extend_from_slice(&value.to_be_bytes());
}

fn encode_array(output: &mut Vec<u8>, length: usize) -> Result<(), u32> {
    let length = u32::try_from(length).map_err(|_| {
        set_error("transport value has too many elements");
        INVALID_ARGUMENT
    })?;
    output.push(0xdd);
    output.extend_from_slice(&length.to_be_bytes());
    Ok(())
}

fn encode_string(output: &mut Vec<u8>, value: &str) -> Result<(), u32> {
    let length = u32::try_from(value.len()).map_err(|_| {
        set_error("transport string is too large");
        INVALID_ARGUMENT
    })?;
    output.push(0xdb);
    output.extend_from_slice(&length.to_be_bytes());
    output.extend_from_slice(value.as_bytes());
    Ok(())
}

fn encode_value(output: &mut Vec<u8>, value: &OwnedValue) -> Result<(), u32> {
    match value {
        OwnedValue::Unit => output.push(0xc0),
        OwnedValue::Bool(false) => output.push(0xc2),
        OwnedValue::Bool(true) => output.push(0xc3),
        OwnedValue::Int(value) => encode_signed(output, *value),
        OwnedValue::Float(value) => {
            output.push(0xcb);
            output.extend_from_slice(&value.to_bits().to_be_bytes());
        }
        OwnedValue::Object(object) => {
            let (kind, values): (u64, Vec<OwnedValue>) = match &object.data {
                ObjectData::String(value) => {
                    encode_array(output, 3)?;
                    encode_unsigned(output, 5);
                    encode_unsigned(output, object.type_id);
                    encode_string(output, value)?;
                    return Ok(());
                }
                ObjectData::Tuple(values) => (6, values.as_ref().clone()),
                ObjectData::Record(values) => (7, values.as_ref().clone()),
                ObjectData::Enum { variant, payloads } => {
                    encode_array(output, 4)?;
                    encode_unsigned(output, 8);
                    encode_unsigned(output, object.type_id);
                    encode_unsigned(output, u64::from(*variant));
                    encode_array(output, payloads.len())?;
                    for value in payloads.iter() {
                        encode_value(output, value)?;
                    }
                    return Ok(());
                }
                ObjectData::List(values) => (9, values.as_ref().clone()),
                ObjectData::Map { key_kind, entries } => {
                    encode_array(output, 4)?;
                    encode_unsigned(output, 10);
                    encode_unsigned(output, object.type_id);
                    encode_unsigned(output, u64::from(*key_kind));
                    encode_array(output, entries.len())?;
                    for (key, value) in entries.iter() {
                        encode_array(output, 2)?;
                        match key {
                            MapKey::Int(value) => encode_signed(output, *value),
                            MapKey::String(value) => encode_string(output, value)?,
                        }
                        encode_value(output, value)?;
                    }
                    return Ok(());
                }
                ObjectData::Sender(_)
                | ObjectData::Receiver(_)
                | ObjectData::Task(_)
                | ObjectData::Listener(_)
                | ObjectData::Connection(_) => {
                    set_error("runtime resources cannot be serialized");
                    return Err(INVALID_ARGUMENT);
                }
            };
            encode_array(output, 3)?;
            encode_unsigned(output, kind);
            encode_unsigned(output, object.type_id);
            encode_array(output, values.len())?;
            for value in &values {
                encode_value(output, value)?;
            }
        }
    }
    Ok(())
}

struct Decoder<'a> {
    input: &'a [u8],
    position: usize,
}

impl<'a> Decoder<'a> {
    fn byte(&mut self) -> Result<u8, u32> {
        let byte = self.input.get(self.position).copied().ok_or_else(|| {
            set_error("truncated MessagePack value");
            INVALID_ARGUMENT
        })?;
        self.position += 1;
        Ok(byte)
    }

    fn bytes<const N: usize>(&mut self) -> Result<[u8; N], u32> {
        let end = self.position.checked_add(N).ok_or(INVALID_ARGUMENT)?;
        let bytes = self.input.get(self.position..end).ok_or_else(|| {
            set_error("truncated MessagePack value");
            INVALID_ARGUMENT
        })?;
        self.position = end;
        bytes.try_into().map_err(|_| INVALID_ARGUMENT)
    }

    fn unsigned(&mut self) -> Result<u64, u32> {
        if self.byte()? != 0xcf {
            set_error("expected MessagePack unsigned integer");
            return Err(INVALID_ARGUMENT);
        }
        Ok(u64::from_be_bytes(self.bytes()?))
    }

    fn array(&mut self) -> Result<usize, u32> {
        if self.byte()? != 0xdd {
            set_error("expected MessagePack array");
            return Err(INVALID_ARGUMENT);
        }
        Ok(u32::from_be_bytes(self.bytes()?) as usize)
    }

    fn string(&mut self) -> Result<String, u32> {
        if self.byte()? != 0xdb {
            set_error("expected MessagePack string");
            return Err(INVALID_ARGUMENT);
        }
        let length = u32::from_be_bytes(self.bytes()?) as usize;
        let end = self.position.checked_add(length).ok_or(INVALID_ARGUMENT)?;
        let bytes = self.input.get(self.position..end).ok_or_else(|| {
            set_error("truncated MessagePack string");
            INVALID_ARGUMENT
        })?;
        self.position = end;
        std::str::from_utf8(bytes).map(str::to_owned).map_err(|_| {
            set_error("MessagePack string is not UTF-8");
            INVALID_ARGUMENT
        })
    }

    fn value(&mut self) -> Result<OwnedValue, u32> {
        let marker = *self.input.get(self.position).ok_or(INVALID_ARGUMENT)?;
        match marker {
            0xc0 => {
                self.position += 1;
                Ok(OwnedValue::Unit)
            }
            0xc2 | 0xc3 => {
                self.position += 1;
                Ok(OwnedValue::Bool(marker == 0xc3))
            }
            0xd3 => {
                self.position += 1;
                Ok(OwnedValue::Int(i64::from_be_bytes(self.bytes()?)))
            }
            0xcb => {
                self.position += 1;
                Ok(OwnedValue::Float(f64::from_bits(u64::from_be_bytes(
                    self.bytes()?,
                ))))
            }
            0xdd => self.object(),
            _ => {
                set_error("unsupported MessagePack marker");
                Err(INVALID_ARGUMENT)
            }
        }
    }

    fn object(&mut self) -> Result<OwnedValue, u32> {
        let envelope = self.array()?;
        if envelope < 3 {
            set_error("invalid MessagePack object envelope");
            return Err(INVALID_ARGUMENT);
        }
        let kind = self.unsigned()?;
        let type_id = self.unsigned()?;
        let data = match kind {
            5 => ObjectData::String(self.string()?),
            6 | 7 | 9 => {
                let length = self.array()?;
                let mut values = Vec::with_capacity(length);
                for _ in 0..length {
                    values.push(self.value()?);
                }
                match kind {
                    6 => ObjectData::Tuple(Arc::new(values)),
                    7 => ObjectData::Record(Arc::new(values)),
                    _ => ObjectData::List(Arc::new(values)),
                }
            }
            8 => {
                let variant = u32::try_from(self.unsigned()?).map_err(|_| INVALID_ARGUMENT)?;
                let length = self.array()?;
                let mut payloads = Vec::with_capacity(length);
                for _ in 0..length {
                    payloads.push(self.value()?);
                }
                ObjectData::Enum {
                    variant,
                    payloads: Arc::new(payloads),
                }
            }
            10 => {
                let key_kind = u32::try_from(self.unsigned()?).map_err(|_| INVALID_ARGUMENT)?;
                let length = self.array()?;
                let mut entries = HashMap::with_capacity(length);
                for _ in 0..length {
                    if self.array()? != 2 {
                        return Err(INVALID_ARGUMENT);
                    }
                    let key = if key_kind == 0 {
                        if self.byte()? != 0xd3 {
                            return Err(INVALID_ARGUMENT);
                        }
                        MapKey::Int(i64::from_be_bytes(self.bytes()?))
                    } else {
                        MapKey::String(self.string()?)
                    };
                    entries.insert(key, self.value()?);
                }
                ObjectData::Map {
                    key_kind,
                    entries: Arc::new(entries),
                }
            }
            _ => {
                set_error("unknown MessagePack Dola object kind");
                return Err(INVALID_ARGUMENT);
            }
        };
        Ok(object(type_id, data))
    }
}

fn send_frame(connection: &ConnectionCore, value: OwnedValue) -> Result<(), u32> {
    if let OwnedValue::Object(object) = &value
        && !matches!(object.data, ObjectData::String(_))
        && object.type_id != connection.send_type
    {
        set_error("transport value type does not match the connection");
        return Err(INVALID_ARGUMENT);
    }
    let mut payload = Vec::new();
    encode_value(&mut payload, &value)?;
    if payload.len() > MAX_FRAME_LENGTH {
        set_error("transport message exceeds the maximum frame length");
        return Err(INVALID_ARGUMENT);
    }
    let length = u32::try_from(payload.len()).map_err(|_| INVALID_ARGUMENT)?;
    let mut stream = connection
        .writer
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    let stream = stream.as_mut().ok_or_else(|| {
        set_error("connection is closed");
        CLOSED
    })?;
    stream
        .write_all(&length.to_be_bytes())
        .and_then(|()| stream.write_all(&payload))
        .and_then(|()| stream.flush())
        .map_err(transport_error)
}

fn receive_frame(connection: &ConnectionCore) -> Result<Option<OwnedValue>, u32> {
    use std::io::Read;
    let mut stream = connection
        .reader
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    let stream = stream.as_mut().ok_or_else(|| {
        set_error("connection is closed");
        CLOSED
    })?;
    let mut length = [0_u8; 4];
    match stream.read_exact(&mut length) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(transport_error(error)),
    }
    let length = u32::from_be_bytes(length) as usize;
    if length > MAX_FRAME_LENGTH {
        set_error("transport frame exceeds the maximum length");
        return Err(INVALID_ARGUMENT);
    }
    let mut payload = vec![0_u8; length];
    stream.read_exact(&mut payload).map_err(transport_error)?;
    let mut decoder = Decoder {
        input: &payload,
        position: 0,
    };
    let value = decoder.value()?;
    if decoder.position != payload.len() {
        set_error("transport frame contains trailing data");
        return Err(INVALID_ARGUMENT);
    }
    if let OwnedValue::Object(object) = &value
        && !matches!(object.data, ObjectData::String(_))
        && object.type_id != connection.receive_type
    {
        set_error("received transport value has an incompatible type");
        return Err(INVALID_ARGUMENT);
    }
    Ok(Some(value))
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_transport_listen(
    address: DolaValue,
    request_type: u64,
    response_type: u64,
    listener_type: u64,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let address = string_value(address)?;
        let listener = TcpListener::bind(address).map_err(transport_error)?;
        Ok(write_output(
            out,
            object(
                listener_type,
                ObjectData::Listener(Arc::new(ListenerCore {
                    listener: Mutex::new(Some(listener)),
                    request_type,
                    response_type,
                })),
            ),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_transport_local_address(listener: DolaValue, out: *mut DolaValue) -> u32 {
    status(|| {
        require_output(out)?;
        let listener = listener_from(listener)?;
        let guard = listener
            .listener
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let address = guard
            .as_ref()
            .ok_or_else(|| {
                set_error("listener is closed");
                CLOSED
            })?
            .local_addr()
            .map_err(transport_error)?
            .to_string();
        Ok(write_output(out, object(0, ObjectData::String(address))))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_transport_connect(
    address: DolaValue,
    send_type: u64,
    receive_type: u64,
    connection_type: u64,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let address = string_value(address)?;
        let mut stream = TcpStream::connect(address).map_err(transport_error)?;
        write_handshake(&mut stream, send_type, receive_type).map_err(transport_error)?;
        let (peer_send, peer_receive) = read_handshake(&mut stream).map_err(transport_error)?;
        if peer_send != receive_type || peer_receive != send_type {
            set_error("transport peer uses incompatible message types");
            return Ok(INVALID_ARGUMENT);
        }
        Ok(write_output(
            out,
            object(
                connection_type,
                ObjectData::Connection(Arc::new(ConnectionCore::new(
                    stream,
                    send_type,
                    receive_type,
                )?)),
            ),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_transport_accept(
    listener: DolaValue,
    connection_type: u64,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        require_output(out)?;
        let listener = listener_from(listener)?;
        let (mut stream, _) = listener
            .listener
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .as_ref()
            .ok_or_else(|| {
                set_error("listener is closed");
                CLOSED
            })?
            .accept()
            .map_err(transport_error)?;
        let (peer_send, peer_receive) = read_handshake(&mut stream).map_err(transport_error)?;
        write_handshake(&mut stream, listener.response_type, listener.request_type)
            .map_err(transport_error)?;
        if peer_send != listener.request_type || peer_receive != listener.response_type {
            set_error("transport peer uses incompatible message types");
            return Ok(INVALID_ARGUMENT);
        }
        Ok(write_output(
            out,
            object(
                connection_type,
                ObjectData::Connection(Arc::new(ConnectionCore::new(
                    stream,
                    listener.response_type,
                    listener.request_type,
                )?)),
            ),
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_transport_send(connection: DolaValue, value: DolaValue) -> u32 {
    status(|| {
        let connection = connection_from(connection)?;
        send_frame(&connection, borrow_value(value)?)?;
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn dola_rt_transport_receive(
    connection: DolaValue,
    out_has_value: *mut u8,
    out: *mut DolaValue,
) -> u32 {
    status(|| {
        if out_has_value.is_null() {
            set_error("transport receive flag must not be null");
            return Ok(INVALID_ARGUMENT);
        }
        require_output(out)?;
        let connection = connection_from(connection)?;
        match receive_frame(&connection)? {
            Some(value) => {
                unsafe { ptr::write(out_has_value, 1) };
                Ok(write_output(out, value))
            }
            None => {
                unsafe { ptr::write(out_has_value, 0) };
                Ok(OK)
            }
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn dola_rt_transport_close(value: DolaValue) -> u32 {
    status(|| {
        if let Ok(listener) = listener_from(value) {
            listener
                .listener
                .lock()
                .unwrap_or_else(|error| error.into_inner())
                .take();
            return Ok(OK);
        }
        let connection = connection_from(value)?;
        connection.close();
        Ok(OK)
    })
}
