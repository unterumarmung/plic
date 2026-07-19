type RuntimeJob = Box<dyn FnOnce() + Send + 'static>;

struct PoolState {
    queue: VecDeque<RuntimeJob>,
    shutdown: bool,
    workers: usize,
    idle: usize,
}

struct PoolShared {
    state: Mutex<PoolState>,
    ready: Condvar,
    handles: Mutex<Vec<std::thread::JoinHandle<()>>>,
    minimum: usize,
}

struct ThreadPool {
    shared: Arc<PoolShared>,
}

impl ThreadPool {
    fn new() -> Self {
        let minimum = 1;
        let shared = Arc::new(PoolShared {
            state: Mutex::new(PoolState {
                queue: VecDeque::new(),
                shutdown: false,
                workers: 0,
                idle: 0,
            }),
            ready: Condvar::new(),
            handles: Mutex::new(Vec::new()),
            minimum,
        });
        for _ in 0..minimum {
            Self::spawn_worker(&shared);
        }
        Self { shared }
    }

    fn spawn_worker(shared: &Arc<PoolShared>) {
        {
            let mut state = shared
                .state
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            if state.shutdown {
                return;
            }
            state.workers += 1;
        }
        let worker_shared = Arc::clone(shared);
        let handle = std::thread::spawn(move || Self::worker(worker_shared));
        shared
            .handles
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .push(handle);
    }

    fn worker(shared: Arc<PoolShared>) {
        loop {
            let job = {
                let mut state = shared
                    .state
                    .lock()
                    .unwrap_or_else(|error| error.into_inner());
                state.idle += 1;
                loop {
                    if let Some(job) = state.queue.pop_front() {
                        state.idle -= 1;
                        break Some(job);
                    }
                    if state.shutdown {
                        state.idle -= 1;
                        state.workers -= 1;
                        break None;
                    }
                    let (next, timeout) = shared
                        .ready
                        .wait_timeout(state, std::time::Duration::from_secs(1))
                        .unwrap_or_else(|error| error.into_inner());
                    state = next;
                    if timeout.timed_out() && state.workers > shared.minimum {
                        state.idle -= 1;
                        state.workers -= 1;
                        break None;
                    }
                }
            };
            let Some(job) = job else { return };
            let _ = catch_unwind(AssertUnwindSafe(job));
        }
    }

    fn submit(&self, job: RuntimeJob) -> Result<(), u32> {
        let should_grow = {
            let mut state = self
                .shared
                .state
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            if state.shutdown {
                set_error("runtime scheduler is shutting down");
                return Err(CLOSED);
            }
            state.queue.push_back(job);
            let grow = state.idle == 0;
            self.shared.ready.notify_one();
            grow
        };
        if should_grow {
            Self::spawn_worker(&self.shared);
        }
        Ok(())
    }

    fn shutdown(&self) {
        {
            let mut state = self
                .shared
                .state
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            state.shutdown = true;
            self.shared.ready.notify_all();
        }
        let handles = std::mem::take(
            &mut *self
                .shared
                .handles
                .lock()
                .unwrap_or_else(|error| error.into_inner()),
        );
        for handle in handles {
            let _ = handle.join();
        }
    }
}

struct RuntimeCore {
    pool: ThreadPool,
}

impl RuntimeCore {
    fn new() -> Self {
        Self {
            pool: ThreadPool::new(),
        }
    }
}
