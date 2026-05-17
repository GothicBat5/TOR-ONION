use core::ffi::c_void;
use core::ptr::NonNull;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;
use bun_collections::ArrayHashMap;
use bun_core::{self, Output};
use bun_threading::{Mutex, UnboundedQueue};
use bun_uws as uws;

use crate::async_http::{ACTIVE_REQUESTS_COUNT, MAX_SIMULTANEOUS_REQUESTS};
use crate::http_context::ActiveSocketExt;
use crate::proxy_tunnel::ProxyTunnel;
use crate::ssl_config::{self, SSLConfig};
use crate::{AsyncHttp, HTTPContext, HttpClient, InitError, NewHttpContext, h2, h3};

bun_core::declare_scope!(HTTPThread, hidden); // threadlog
bun_core::declare_scope!(HTTPThread_log, visible); // log
struct SslContextCacheEntry {
    ctx: NonNull<NewHttpContext<true>>,
    last_used_ns: u64,
    config_ref: ssl_config::SharedPtr,
}

impl SslContextCacheEntry {
    #[inline]
    fn ctx_mut<'a>(&self) -> &'a mut NewHttpContext<true> {
        unsafe { &mut *self.ctx.as_ptr() }
    }

    fn release(self) {

        unsafe { NewHttpContext::<true>::deref(self.ctx.as_ptr()) };
    }
}
const SSL_CONTEXT_CACHE_MAX_SIZE: usize = 60;
const SSL_CONTEXT_CACHE_TTL_NS: u64 = 30 * (60 * 1_000_000_000); 

static CUSTOM_SSL_CONTEXT_MAP: bun_core::RacyCell<
    Option<ArrayHashMap<*const SSLConfig, SslContextCacheEntry>>, > = bun_core::RacyCell::new(None);

fn custom_ssl_context_map() -> &'static mut ArrayHashMap<*const SSLConfig, SslContextCacheEntry> {
    unsafe { (*CUSTOM_SSL_CONTEXT_MAP.get()).get_or_insert_with(ArrayHashMap::new) }
}

use bun_event_loop::MiniEventLoop as mini_event_loop;
use bun_event_loop::MiniEventLoop::MiniEventLoop;

pub struct HttpThread {
    pub loop_: *const MiniEventLoop<'static>,
    pub uws_loop: *mut uws::Loop,
    pub http_context: NewHttpContext<false>,
    pub https_context: NewHttpContext<true>, lazy_https_init: Option<InitOpts>,
    pub queued_tasks: Queue,
    pub deferred_tasks: Vec<NonNull<AsyncHttp<'static>>>,
    pub has_pending_queued_abort: bool,
    pub queued_shutdowns: Vec<ShutdownMessage>,
    pub queued_writes: Vec<WriteMessage>,
    pub queued_response_body_drains: Vec<DrainMessage>,
    pub queued_shutdowns_lock: Mutex,
    pub queued_writes_lock: Mutex,
    pub queued_response_body_drains_lock: Mutex,
    pub queued_threadlocal_proxy_derefs: Vec<*mut ProxyTunnel>,
    pub has_awoken: AtomicBool,
    pub timer: Instant,
    pub lazy_libdeflater: Option<Box<LibdeflateState>>,
    pub lazy_request_body_buffer: Option<Box<HeapRequestBodyBuffer>>,
}

impl HttpThread {
    fn new() -> Self {
        Self {
            loop_: core::ptr::null(),
            uws_loop: core::ptr::null_mut(),
            http_context: NewHttpContext::<false> {
                ref_count: Cell::new(1),
                pending_sockets: bun_collections::HiveArray::init(),
                group: uws::SocketGroup::default(),
                secure: None,
                active_h2_sessions: Vec::new(),
                pending_h2_connects: Vec::new(),
            },
            https_context: NewHttpContext::<true> {
                ref_count: Cell::new(1),
                pending_sockets: bun_collections::HiveArray::init(),
                group: uws::SocketGroup::default(),
                secure: None,
                active_h2_sessions: Vec::new(),
                pending_h2_connects: Vec::new(),
            },
            lazy_https_init: None,
            queued_tasks: Queue::new(),
            deferred_tasks: Vec::new(),
            has_pending_queued_abort: false,
            queued_shutdowns: Vec::new(),
            queued_writes: Vec::new(),
            queued_response_body_drains: Vec::new(),
            queued_shutdowns_lock: Mutex::new(),
            queued_writes_lock: Mutex::new(),
            queued_response_body_drains_lock: Mutex::new(),
            queued_threadlocal_proxy_derefs: Vec::new(),
            has_awoken: AtomicBool::new(false),
            timer: Instant::now(),
            lazy_libdeflater: None,
            lazy_request_body_buffer: None,
        }
    }
}

pub struct HeapRequestBodyBuffer {
    pub buffer: [u8; 512 * 1024],
    pub cursor: usize,
}

impl HeapRequestBodyBuffer {
    pub fn init() -> Box<Self> {
        Box::new(HeapRequestBodyBuffer {
            buffer: [0u8; 512 * 1024],
            cursor: 0,
        })
    }

    pub fn put(mut self: Box<Self>) {
        let thread = crate::http_thread_mut();
        if thread.lazy_request_body_buffer.is_none() {
            self.cursor = 0; // .reset()
            thread.lazy_request_body_buffer = Some(self);
        } else {
            drop(self);
        }
    }
}

pub enum RequestBodyBuffer {
    Heap(Option<Box<HeapRequestBodyBuffer>>),
    Stack(Box<[u8; REQUEST_BODY_SEND_STACK_BUFFER_SIZE]>),
}

impl Drop for RequestBodyBuffer {
    fn drop(&mut self) {
        if let Self::Heap(heap) = self {
            if let Some(h) = heap.take() {
                h.put();
            }
        }
    }
}

impl RequestBodyBuffer {
    pub fn allocated_slice(&mut self) -> &mut [u8] {
        match self {
            Self::Heap(heap) => &mut heap.as_mut().unwrap().buffer,
            Self::Stack(stack) => &mut stack[..],
        }
    }

    pub fn to_array_list(&mut self) -> Vec<u8> {
        let mut arraylist = Vec::with_capacity(self.allocated_slice().len());
        arraylist.clear();
        arraylist
    }
}

pub struct WriteMessage {
    pub async_http_id: u32,
    pub kind: WriteMessageType,
}

#[repr(u8)] // Zig: enum(u2)
#[derive(Copy, Clone, PartialEq, Eq)]
pub enum WriteMessageType {
    Data = 0,
    End = 1,
}

pub struct DrainMessage {
    pub async_http_id: u32,
}

pub struct ShutdownMessage {
    pub async_http_id: u32,
}

pub struct LibdeflateState {
    pub decompressor: *mut bun_libdeflate_sys::libdeflate::Decompressor,
    pub shared_buffer: [u8; 512 * 1024],
}

impl LibdeflateState {
    #[inline]
    pub fn decompressor_mut<'a>(&self) -> &'a mut bun_libdeflate_sys::libdeflate::Decompressor {
        unsafe { &mut *self.decompressor }
    }
}

pub const REQUEST_BODY_SEND_STACK_BUFFER_SIZE: usize = 32 * 1024;

pub type Queue = UnboundedQueue<AsyncHttp<'static>>;
#[derive(Clone)]
pub struct InitOpts {
    pub ca: Vec<*const c_void>, 
    pub abs_ca_file_name: &'static [u8],
    pub for_install: bool,

    pub on_init_error: fn(err: InitError, opts: &InitOpts) -> !,
}

unsafe impl Send for InitOpts {}

impl Default for InitOpts {
    fn default() -> Self {
        Self {
            ca: Vec::new(),
            abs_ca_file_name: b"",
            for_install: false,
            on_init_error: on_init_error_noop,
        }
    }
}

fn on_init_error_noop(err: InitError, opts: &InitOpts) -> ! {
    match err {
        InitError::LoadCAFile => {

            let path = unsafe {
                bun_core::ZStr::from_raw(opts.abs_ca_file_name.as_ptr(), opts.abs_ca_file_name.len(),
                )
            };
            if !bun_sys::exists_z(path) {
                Output::err("HTTPThread", "failed to find CA file: '{}'", (bstr::BStr::new(opts.abs_ca_file_name),),);
            } else {
                Output::err("HTTPThread", "failed to load CA file: '{}'", (bstr::BStr::new(opts.abs_ca_file_name),),);
            }
        }
        InitError::InvalidCAFile => {
            Output::err("HTTPThread", "the CA file is invalid: '{}'", (bstr::BStr::new(opts.abs_ca_file_name),),);
        }
        InitError::InvalidCA => {
            Output::err("HTTPThread", "the provided CA is invalid", ());
        }
        InitError::FailedToOpenSocket => {
            bun_core::err_generic!("failed to start HTTP client thread");
        }
    }
    bun_core::Global::crash();
}

impl HttpThread {
    #[inline]
    pub fn uws_loop(&self) -> *mut uws::Loop {
        self.uws_loop
    }
    #[inline]
    fn uws_loop_mut<'a>(&self) -> &'a mut uws::Loop {
        // SAFETY: see INVARIANT above.
        unsafe { &mut *self.uws_loop }
    }

    #[inline]
    fn timer_read(&self) -> u64 {
        u64::try_from(self.timer.elapsed().as_nanos()).expect("int cast")
    }

    #[inline]
    pub fn get_request_body_send_buffer(&mut self, estimated_size: usize) -> RequestBodyBuffer {
        if estimated_size >= REQUEST_BODY_SEND_STACK_BUFFER_SIZE {
            if self.lazy_request_body_buffer.is_none() {
                bun_core::scoped_log!(HTTPThread_log, "Allocating HeapRequestBodyBuffer due to {} bytes request body", estimated_size);
                return RequestBodyBuffer::Heap(Some(HeapRequestBodyBuffer::init()));
            }

            return RequestBodyBuffer::Heap(self.lazy_request_body_buffer.take());
        }
        RequestBodyBuffer::Stack(Box::new([0u8; REQUEST_BODY_SEND_STACK_BUFFER_SIZE]))
    }
    pub fn deflater(&mut self) -> &mut LibdeflateState {
        if self.lazy_libdeflater.is_none() {
            let decompressor = bun_libdeflate_sys::libdeflate::Decompressor::alloc();
            if decompressor.is_null() {
                bun_core::out_of_memory();
            }
            self.lazy_libdeflater = Some(Box::new(LibdeflateState {
                decompressor, shared_buffer: [0u8; 512 * 1024],}));
        }

        self.lazy_libdeflater.as_deref_mut().unwrap()
    }

    pub fn context<const IS_SSL: bool>(&mut self) -> &mut NewHttpContext<IS_SSL> {
        if IS_SSL {
            unsafe { &mut *(&raw mut self.https_context).cast::<NewHttpContext<IS_SSL>>() }
        } else {
            unsafe { &mut *(&raw mut self.http_context).cast::<NewHttpContext<IS_SSL>>() }
        }
    }
    #[inline]
    fn ensure_https_context_init(&mut self) {
        if let Some(opts) = self.lazy_https_init.take() {
            self.init_https_context_cold(opts);
        }
    }

    #[cold]
    fn init_https_context_cold(&mut self, opts: InitOpts) {
        if let Err(err) = self.https_context.init_with_thread_opts(&opts) {
            (opts.on_init_error)(err, &opts);
        }
    }
    pub fn connect<const IS_SSL: bool>(
        &mut self,
        client: &mut HttpClient,
    ) -> Result<Option<crate::HTTPSocket<IS_SSL>>, bun_core::Error>
    {
        if IS_SSL z
            self.ensure_https_context_init();
        }
        let unix_path = bun_ptr::RawSlice::new(client.unix_socket_path.slice());
        if !unix_path.is_empty() {
            return self.context::<IS_SSL>().connect_socket(client, unix_path.slice());
        }

        if IS_SSL {
            'custom_ctx: {
                let Some(tls) = client.tls_props.clone() else {
                    break 'custom_ctx;
                };
                if !tls.get().requires_custom_request_ctx {
                    break 'custom_ctx;
                }
                let requested_config: *const SSLConfig = tls.get();
                self.evict_stale_ssl_contexts();
                if let Some(entry) = custom_ssl_context_map().get_mut(&requested_config) {
                    entry.last_used_ns = self.timer_read();
                    client.set_custom_ssl_ctx(entry.ctx);
                    let ctx = entry.ctx_mut();
                    return if let Some(url) = client.http_proxy.clone() {
                        ctx.connect(client, url.hostname, url.get_port_auto())
                    } else {
                        let (hn, pt) = (client.url.hostname, client.url.get_port_auto());
                        ctx.connect(client, hn, pt)
                    }
                    .map(|o| o.map(|s| s.cast_ssl::<IS_SSL>()));
                }
                let custom_context = bun_core::heap::release(Box::new(NewHttpContext::<true> {
                    ref_count: Cell::new(1),
                    pending_sockets: bun_collections::HiveArray::init(),
                    group: uws::SocketGroup::default(),
                    secure: None,
                    active_h2_sessions: Vec::new(),
                    pending_h2_connects: Vec::new(),
                }));
                if let Err(err) = custom_context.init_with_client_config(client) {
                    drop(unsafe {
                        bun_core::heap::take(std::ptr::from_mut::<NewHttpContext<true>>(
                            custom_context,
                        ))
                    });
                    return Err(match err {
                        InitError::FailedToOpenSocket
                        | InitError::InvalidCA
                        | InitError::InvalidCAFile
                        | InitError::LoadCAFile => bun_core::err!("FailedToOpenSocket"),
                    });
                }

                let now = self.timer_read();
                let ctx_nn = NonNull::from(&mut *custom_context);
                let _ = custom_ssl_context_map().put(requested_config, SslContextCacheEntry {
                  ctx: ctx_nn, last_used_ns: now, config_ref: tls,},);
                if custom_ssl_context_map().count() > SSL_CONTEXT_CACHE_MAX_SIZE {
                    evict_oldest_ssl_context();
                }
                client.set_custom_ssl_ctx(ctx_nn);
                let result = if let Some(url) = client.http_proxy.clone() {
                    if (url.protocol.is_empty() || url.protocol == b"https" || url.protocol == b"http")
                    {
                        custom_context.connect(client, url.hostname, url.get_port_auto())
                    } else {
                        return Err(bun_core::err!("UnsupportedProxyProtocol"));
                    }
                } else {
                    let (hn, pt) = (client.url.hostname, client.url.get_port_auto());
                    custom_context.connect(client, hn, pt)
                };
                return result.map(|o| o.map(|s| s.cast_ssl::<IS_SSL>()));
            }
        }
        if let Some(url) = client.http_proxy.clone() {
            if !url.href.is_empty() {
                if url.protocol.is_empty() || url.protocol == b"https" || url.protocol == b"http" {
                    return self.context::<IS_SSL>().connect(
                        client, url.hostname, url.get_port_auto(),);
                }
                return Err(bun_core::err!("UnsupportedProxyProtocol"));
            }
        }
        let (hn, pt) = (client.url.hostname, client.url.get_port_auto());
        self.context::<IS_SSL>().connect(client, hn, pt)
    }
    fn evict_stale_ssl_contexts(&mut self) {
        let now = self.timer_read();
        let map = custom_ssl_context_map();
        let mut i: usize = 0;
        while i < map.count() {
            let entry_last_used = map.values()[i].last_used_ns;
            if now.saturating_sub(entry_last_used) > SSL_CONTEXT_CACHE_TTL_NS {
                let (_k, entry) = map.swap_remove_at(i);
                entry.release();
            } else {
                i += 1;
            }
        }
    }

    fn abort_pending_h2_waiter(&mut self, async_http_id: u32) -> bool {
        if self.https_context.abort_pending_h2_waiter(async_http_id) {
            return true;
        }
        for entry in custom_ssl_context_map().values_mut() {
            if entry.ctx_mut().abort_pending_h2_waiter(async_http_id) {
                return true;
            }
        }
        false
    }

    fn drain_queued_shutdowns(&mut self) {
        loop {
            let queued_shutdowns = {
                let _guard = self.queued_shutdowns_lock.lock_guard();
                core::mem::take(&mut self.queued_shutdowns)
            };

            for http in &queued_shutdowns {
                let tracker = abort_tracker();
                let found_idx = tracker.keys().iter().position(|&k| k == http.async_http_id);
                if let Some(idx) = found_idx {
                    let (_k, socket_ptr) = tracker.swap_remove_at(idx);
                    match socket_ptr {
                        uws::AnySocket::SocketTls(socket) => {
                            let tagged = HTTPContext::<true>::get_tagged_from_socket(socket);
                            if let Some(client) = tagged.client_mut() {
                                client.close_and_abort::<true>(socket);
                                continue;
                            }
                            if let Some(session) = tagged.session_mut() {
                                session.abort_by_http_id(http.async_http_id);
                                continue;
                            }
                            socket.close(uws::CloseKind::Failure);
                        }
                        uws::AnySocket::SocketTcp(socket) => {
                            let tagged = HTTPContext::<false>::get_tagged_from_socket(socket);
                            if let Some(client) = tagged.client_mut() {
                                client.close_and_abort::<false>(socket);
                                continue;
                            }
                            if let Some(session) = tagged.session_mut() {
                                session.abort_by_http_id(http.async_http_id);
                                continue;
                            }
                            socket.close(uws::CloseKind::Failure);
                        }
                    }
                } else {
                    if self.abort_pending_h2_waiter(http.async_http_id) {
                        continue;
                    }
                    if h3::ClientContext::abort_by_http_id(http.async_http_id) {
                        continue;
                    }
                    self.has_pending_queued_abort = true;
                }
            }
            let len = queued_shutdowns.len();
            drop(queued_shutdowns);
            if len == 0 {
                break;
            }
            bun_core::scoped_log!(HTTPThread, "drained {} queued shutdowns", len);
        }
    }

    fn drain_queued_writes(&mut self) {
        loop {
            let queued_writes = {
                let _guard = self.queued_writes_lock.lock_guard();
                core::mem::take(&mut self.queued_writes)
            };
            for write in &queued_writes {
                let message = write.kind;
                let ended = message == WriteMessageType::End;

                if let Some(socket_ptr) = abort_tracker().get(&write.async_http_id) {
                    match *socket_ptr {
                        uws::AnySocket::SocketTls(socket) => {
                            if socket.is_closed() || socket.is_shutdown() {
                                continue;
                            }
                            let tagged = HTTPContext::<true>::get_tagged_from_socket(socket);
                            if let Some(client) = tagged.client_mut() {
                                if let crate::HTTPRequestBody::Stream(stream) = &mut client.state.original_request_body
                                {
                                    stream.ended = ended;
                                    client.flush_stream::<true>(socket);
                                }
                            }
                            if let Some(session) = tagged.session_mut() {
                                session.stream_body_by_http_id(write.async_http_id, ended);
                            }
                        }
                        uws::AnySocket::SocketTcp(socket) => {
                            if socket.is_closed() || socket.is_shutdown() {
                                continue;
                            }
                            let tagged = HTTPContext::<false>::get_tagged_from_socket(socket);
                            if let Some(client) = tagged.client_mut() {
                                if let crate::HTTPRequestBody::Stream(stream) = &mut client.state.original_request_body
                                {
                                    stream.ended = ended;
                                    client.flush_stream::<false>(socket);
                                }
                            }
                            if let Some(session) = tagged.session_mut() {
                                session.stream_body_by_http_id(write.async_http_id, ended);
                            }
                        }
                    }
                } else {
                    h3::ClientContext::stream_body_by_http_id(write.async_http_id, ended);
                }
            }
            let len = queued_writes.len();
            drop(queued_writes);
            if len == 0 {
                break;
            }
            bun_core::scoped_log!(HTTPThread, "drained {} queued writes", len);
        }
    }

    fn drain_queued_http_response_body_drains(&mut self) {
        loop {
            let queued_response_body_drains = {
                let _guard = self.queued_response_body_drains_lock.lock_guard();
                core::mem::take(&mut self.queued_response_body_drains)
            };

            for drain in &queued_response_body_drains {
                if let Some(socket_ptr) = abort_tracker().get(&drain.async_http_id) {
                    match *socket_ptr {
                        uws::AnySocket::SocketTls(socket) => {
                            let tagged = HTTPContext::<true>::get_tagged_from_socket(socket);
                            if let Some(client) = tagged.client_mut() {
                                client.drain_response_body::<true>(socket);
                            }
                            if let Some(session) = tagged.session_mut() {
                                session.drain_response_body_by_http_id(drain.async_http_id);
                            }
                        }
                        uws::AnySocket::SocketTcp(socket) => {
                            let tagged = HTTPContext::<false>::get_tagged_from_socket(socket);
                            if let Some(client) = tagged.client_mut() {
                                client.drain_response_body::<false>(socket);
                            }
                            if let Some(session) = tagged.session_mut() {
                                session.drain_response_body_by_http_id(drain.async_http_id);
                            }
                        }
                    }
                }
            }
            let len = queued_response_body_drains.len();
            drop(queued_response_body_drains);
            if len == 0 {
                break;
            }
            bun_core::scoped_log!(HTTPThread, "drained {} queued drains", len);
        }
    }

    pub fn drain_events(&mut self) {
        self.drain_queued_http_response_body_drains();
        self.drain_queued_writes();
        self.drain_queued_shutdowns();
        h3::PendingConnect::drain_resolved();

        for http in self.queued_threadlocal_proxy_derefs.drain(..) {
            unsafe { ProxyTunnel::deref(http) };
        }
        let mut count: usize = 0;
        let mut active = ACTIVE_REQUESTS_COUNT.load(Ordering::Relaxed);
        let max = MAX_SIMULTANEOUS_REQUESTS.load(Ordering::Relaxed);
        if active >= max && !self.has_pending_queued_abort {
            return;
        }
        self.has_pending_queued_abort = false;
        {
            let pending = core::mem::take(&mut self.deferred_tasks);
            for http in pending {
                let aborted = bun_ptr::ParentRef::from(http).client..get(crate::signals::Field::Aborted);
                if aborted || active < max {
                    start_queued_task(http.as_ptr());
                    if cfg!(debug_assertions) {
                        count += 1;
                    }
                    active = ACTIVE_REQUESTS_COUNT.load(Ordering::Relaxed);
                } else {
                    self.deferred_tasks.push(http);
                }
            }
        }

        loop {
            let Some(http) = NonNull::new(self.queued_tasks.pop()) else {
                break;
            };
            let aborted = bun_ptr::ParentRef::from(http).client.signals.get(crate::signals::Field::Aborted);
            if !aborted && active >= max {
                self.deferred_tasks.push(http);
                continue;
            }
            start_queued_task(http.as_ptr());
            if cfg!(debug_assertions) {
                count += 1;
            }
            active = ACTIVE_REQUESTS_COUNT.load(Ordering::Relaxed);
        }

        if cfg!(debug_assertions) && count > 0 {
            bun_core::scoped_log!(HTTPThread_log, "Processed {} tasks\n", count);
        }
    }

    pub fn schedule_response_body_drain(&mut self, async_http_id: u32) {
        {
            let _guard = self.queued_response_body_drains_lock.lock_guard();
            self.queued_response_body_drains.push(DrainMessage { async_http_id });
        }
        self.wakeup();
    }

    pub fn schedule_shutdown(&mut self, http: &AsyncHttp) {
        bun_core::scoped_log!(HTTPThread, "scheduleShutdown {}", http.async_http_id);
        {
            let _guard = self.queued_shutdowns_lock.lock_guard();
            self.queued_shutdowns.push(ShutdownMessage {
                async_http_id: http.async_http_id,
            });
        }
        self.wakeup();
    }

    pub fn schedule_request_write(&mut self, http: &AsyncHttp, kind: WriteMessageType) {
        {
            let _guard = self.queued_writes_lock.lock_guard();
            self.queued_writes.push(WriteMessage {
                async_http_id: http.async_http_id,
                kind,
            });
        }
        self.wakeup();
    }

    pub fn schedule_proxy_deref(&mut self, proxy: *mut ProxyTunnel) {
        self.queued_threadlocal_proxy_derefs.push(proxy);
        self.wakeup();
    }

    pub fn wakeup(&self) {
        if self.has_awoken.load(Ordering::Acquire) {
            unsafe { uws::us_wakeup_loop(self.uws_loop) };
        }
    }
    pub fn schedule(batch: bun_threading::thread_pool::Batch) {
        if batch.len == 0 {
            return;
        }
        assert!(
            crate::HTTP_THREAD_INIT.load(Ordering::Acquire),
            "HTTPThread::schedule() called before HTTPThread::init()"
        );
        let this = unsafe {
            bun_ptr::ParentRef::<Self>::from_raw((*crate::HTTP_THREAD.get_unchecked()).as_mut_ptr())
        };
        {
            let mut batch_ = batch;
            while let Some(task) = batch_.pop() {
                let http: *mut AsyncHttp =
                    unsafe { bun_core::from_field_ptr!(AsyncHttp, task, task.as_ptr()) };
                this.queued_tasks.push(http);
            }
        }
        this.wakeup();
    }
}
fn evict_oldest_ssl_context() {
    let map = custom_ssl_context_map();
    if map.count() == 0 {
        return;
    }
    let mut oldest_idx: usize = 0;
    let mut oldest_time: u64 = u64::MAX;
    for (i, entry) in map.values().iter().enumerate() {
        if entry.last_used_ns < oldest_time {
            oldest_time = entry.last_used_ns;
            oldest_idx = i;
        }
    }
    let (_k, entry) = map.swap_remove_at(oldest_idx);
    entry.release();
}
fn start_queued_task(http: *mut AsyncHttp) {
    let cloned = crate::ThreadlocalAsyncHttp::new(unsafe { core::ptr::read(http) });
    let cloned = bun_core::heap::release(cloned);
    cloned.async_http.real = NonNull::new(http);
    cloned.async_http.next.clear();
    cloned.async_http.task.node.next = core::ptr::null_mut();
    cloned.async_http.on_start();
}

#[inline]
fn abort_tracker() -> &'static mut ArrayHashMap<u32, uws::AnySocket> {
    crate::abort_tracker()
}
use core::cell::Cell;
mod _event_loop_draft {
    use super::*;
    use bun_core::Global;
    use std::sync::Once;
    static INIT_ONCE: Once = Once::new();
    static HTTP_THREAD_HANDLE: std::sync::OnceLock<std::thread::JoinHandle<()>> = std::sync::OnceLock::new();
    pub(super) fn init(opts: &InitOpts) {
        INIT_ONCE.call_once(|| init_once(opts));
    }

    fn init_once(opts: &InitOpts) {
        unsafe {
            (*crate::HTTP_THREAD.get()).write(HttpThread::new());
        }
        crate::HTTP_THREAD_INIT.store(true, core::sync::atomic::Ordering::Release);
        bun_libdeflate_sys::libdeflate::load();
        let opts_copy = opts.clone();
        let thread = std::thread::Builder::new()
            .stack_size(bun_threading::thread_pool::DEFAULT_THREAD_STACK_SIZE as usize)
            .spawn(move || on_start(opts_copy));
        match thread {
            Ok(t) => {
                let _ = HTTP_THREAD_HANDLE.set(t);
            }
            Err(err) => Output::panic(format_args!("Failed to start HTTP Client thread: {}", err)),
        }
    }
    pub(super) fn on_start(opts: InitOpts) {
        Output::Source::configure_named_thread(bun_core::zstr!("HTTP Client"));
        let raw: u64 = bun_core::env_var::BUN_CONFIG_HTTP_IDLE_TIMEOUT.get().unwrap_or(300).min(239 * 60);
        crate::IDLE_TIMEOUT_SECONDS.store(
            (if raw > 240 {
                raw.div_ceil(60) * 60
            } else {
                raw
            }) as core::ffi::c_uint,
            core::sync::atomic::Ordering::Relaxed,
        );
        let loop_ = mini_event_loop::init_global(None, None);
        let uws_loop = bun_ptr::ParentRef::from(NonNull::new(loop_)
        .expect("init_global returns the thread-local singleton"),).loop_ptr();
        #[cfg(windows)]
        {
            if bun_sys::windows::getenv_w(bun_core::w!("SystemRoot\0")).is_none() {
                Output::err_generic(
                    "The %SystemRoot% environment variable is not set. Bun needs this set in order for network requests to work.",
                    (),);Global::crash();
            }
        }
        let thread = crate::http_thread_mut();
        thread.loop_ = loop_;
        thread.uws_loop = uws_loop;
        thread.http_context.init();
        if !opts.abs_ca_file_name.is_empty() || !opts.ca.is_empty() {
            if let Err(err) = thread.https_context.init_with_thread_opts(&opts) {
                (opts.on_init_error)(err, &opts);
            }
        } else {
            thread.lazy_https_init = Some(opts);
        }
        thread.has_awoken.store(true, Ordering::Release);
        thread.process_events();
    }

    impl HttpThread {
        fn process_events(&mut self) -> ! {
            let uws_loop = self.uws_loop_mut();
            #[cfg(unix)]
            {
                uws_loop.num_polls = uws_loop.num_polls.max(2);
            }
            #[cfg(windows)]
            {
                uws_loop.inc();
            }
            loop {
                self.drain_events();
                Output::flush();
                let uws_loop = self.uws_loop_mut();
                uws_loop.inc();
                uws_loop.tick();
                uws_loop.dec();
                if cfg!(debug_assertions) {
                    Output::flush();
                }
            }
        }
    }
}
pub fn init(opts: &InitOpts) {
    _event_loop_draft::init(opts)
}
// ported from: src/http/HTTPThread.zig
