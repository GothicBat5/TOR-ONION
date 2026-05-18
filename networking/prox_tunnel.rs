use core::cell::Cell;
use core::ffi::CStr;
use core::ptr::{NonNull, addr_of, addr_of_mut};
use core::sync::atomic::Ordering;
use bun_core::scoped_log;
use bun_core::{Error, ZStr, err};
use bun_uws as uws;
use crate::http_cert_error::HTTPCertError;
use crate::http_context::HTTPSocket;
use crate::internal_state::{HTTPStage, Stage};
use crate::ssl_config::SSLConfig;
use crate::ssl_wrapper::{self, Handlers as SSLWrapperHandlers, InitError, SSLWrapper, WriteDataError};
use crate::{AlpnOffer, GenHttpContext, HTTPClient};

bun_core::declare_scope!(http_proxy_tunnel, visible);
pub type RefPtr = bun_ptr::IntrusiveRc<ProxyTunnel>;

#[inline]
pub(crate) fn raw_as_mut<'a>(ptr: *mut ProxyTunnel) -> &'a mut ProxyTunnel {
    debug_assert!(!ptr.is_null());
    unsafe { &mut *ptr }
}

type ProxyTunnelWrapper = SSLWrapper<*mut HTTPClient<'static>>;
pub use bun_uws::MaybeAnySocket as Socket;

#[derive(bun_ptr::CellRefCounted)]
pub struct ProxyTunnel {
    pub wrapper: Option<ProxyTunnelWrapper>,
    pub shutdown_err: Cell<Error>,
    pub socket: Socket,
    pub write_buffer: bun_io::StreamBuffer,
    pub did_have_handshaking_error: bool,
    pub established_with_reject_unauthorized: bool,
    pub ref_count: Cell<u32>,
}

impl Default for ProxyTunnel {
    fn default() -> Self {
        Self {
            wrapper: None,
            shutdown_err: Cell::new(err!(ConnectionClosed)),
            socket: Socket::None,
            write_buffer: bun_io::StreamBuffer::default(),
            did_have_handshaking_error: false,
            established_with_reject_unauthorized: false,
            ref_count: Cell::new(1),
        }
    }
}

impl Drop for ProxyTunnel {
    fn drop(&mut self) {
        self.socket = Socket::None;
    }
}
impl ProxyTunnel {
    #[inline]
    fn socket_of<'a>(this: NonNull<Self>) -> &'a Socket {
        unsafe { &*addr_of!((*this.as_ptr()).socket) }
    }

    #[inline]
    fn set_socket(this: NonNull<Self>, s: Socket) {
        unsafe { *addr_of_mut!((*this.as_ptr()).socket) = s };
    }

    #[inline]
    fn write_buffer_of<'a>(this: NonNull<Self>) -> &'a mut bun_io::StreamBuffer {
        // SAFETY: see [`Self::socket_of`].
        unsafe { &mut *addr_of_mut!((*this.as_ptr()).write_buffer) }
    }
    #[inline]
    fn shutdown_err_of<'a>(this: NonNull<Self>) -> &'a Cell<Error> {

        unsafe { &*addr_of!((*this.as_ptr()).shutdown_err) }
    }
    #[inline]
    fn close_from_callback(this: NonNull<Self>, err: Error) {
        Self::close_raw(this, err);
    }

    #[inline]
    fn wrapper_ssl(this: NonNull<Self>) -> Option<NonNull<bun_boringssl_sys::SSL>> {
        unsafe { (*this.as_ptr()).wrapper.as_ref().and_then(|w| w.ssl) }
    }

    #[inline]
    fn wrapper_mut<'a>(this: *mut Self) -> Option<&'a mut ProxyTunnelWrapper> {
        unsafe { (*addr_of_mut!((*this).wrapper)).as_mut() }
    }
    #[inline]
    fn ref_count_of<'a>(this: NonNull<Self>) -> &'a core::cell::Cell<u32> {
        unsafe { &*addr_of!((*this.as_ptr()).ref_count) }
    }
    #[inline]
    fn ref_scope(this: NonNull<Self>) -> bun_ptr::ScopedRef<Self> {
        unsafe { bun_ptr::ScopedRef::new(this.as_ptr()) }
    }
}
#[inline]
fn client_from_ctx<'a, 'c>(ctx: *mut HTTPClient<'c>) -> &'a mut HTTPClient<'c> {
  
    unsafe { &mut *ctx }
}
fn on_open(ctx: *mut HTTPClient) {

    let this = client_from_ctx(ctx);
    scoped_log!(http_proxy_tunnel, "ProxyTunnel onOpen");
    bun_analytics::features::http_client_proxy.fetch_add(1, Ordering::Relaxed);
    this.state.response_stage = HTTPStage::ProxyHandshake;
    this.state.request_stage = HTTPStage::ProxyHandshake;
    let Some(proxy_nn) = this.proxy_tunnel.as_ref().map(|p| p.data) else {
        return;
    };
    let _guard = ProxyTunnel::ref_scope(proxy_nn);
    if let Some(ssl_ptr) = ProxyTunnel::wrapper_ssl(proxy_nn) {
        let _hostname = this.hostname.unwrap_or(this.url.hostname);
        if bun_core::is_ip_address(_hostname) {
            crate::configure_http_client_with_alpn(
                ssl_ptr.as_ptr(),
                core::ptr::null(),
                AlpnOffer::H1,
            );
        } else {
            let temp_hostname = crate::temp_hostname();
            if _hostname.len() < temp_hostname.len() {
                temp_hostname[.._hostname.len()].copy_from_slice(_hostname);
                temp_hostname[_hostname.len()] = 0;
                crate::configure_http_client_with_alpn(ssl_ptr.as_ptr(), temp_hostname.as_ptr().cast(),
                    AlpnOffer::H1,
                );
            } else {
                let mut owned = _hostname.to_vec();
                owned.push(0);
                crate::configure_http_client_with_alpn(
                    ssl_ptr.as_ptr(),
                    owned.as_ptr().cast(),
                    AlpnOffer::H1,
                );
            }
        }
    }
}

fn on_data(ctx: *mut HTTPClient, decoded_data: &[u8]) {
    if decoded_data.is_empty() {
        return;
    }
    scoped_log!(
        http_proxy_tunnel,
        "ProxyTunnel onData decoded {}",
        decoded_data.len()
    );
    let this = client_from_ctx(ctx);
    let Some(proxy_nn) = this.proxy_tunnel.as_ref().map(|p| p.data) else {
        return;
    };
    let _guard = ProxyTunnel::ref_scope(proxy_nn);
    match this.state.response_stage {
        HTTPStage::Body => {
            scoped_log!(http_proxy_tunnel, "ProxyTunnel onData body");
            if decoded_data.is_empty() {
                return;
            }
            let report_progress = match this.handle_response_body(decoded_data, false) {
                Ok(v) => v,
                Err(err) => {
                    ProxyTunnel::close_from_callback(proxy_nn, err);
                    return;
                }
            };

            if report_progress {
                progress_update_for_proxy_socket(ctx, proxy_nn);
                return;
            }
        }
        HTTPStage::BodyChunk => {
            scoped_log!(http_proxy_tunnel, "ProxyTunnel onData body_chunk");
            if decoded_data.is_empty() {
                return;
            }
            let report_progress = match this.handle_response_body_chunked_encoding(decoded_data) {
                Ok(v) => v,
                Err(err) => {
                    ProxyTunnel::close_from_callback(proxy_nn, err);
                    return;
                }
            };

            if report_progress {
                progress_update_for_proxy_socket(ctx, proxy_nn);
                return;
            }
        }
        HTTPStage::ProxyHeaders => {
            scoped_log!(http_proxy_tunnel, "ProxyTunnel onData proxy_headers");
            match ProxyTunnel::socket_of(proxy_nn) {
                &Socket::Ssl(socket) => {
                    let hctx = &raw mut crate::http_thread().https_context;
                    this.handle_on_data_headers::<true>(decoded_data, hctx, socket);
                }
                &Socket::Tcp(socket) => {
                    let hctx = &raw mut crate::http_thread().http_context;
                    this.handle_on_data_headers::<false>(decoded_data, hctx, socket);
                }
                Socket::None => {}
            }
        }
        _ => {
            scoped_log!(http_proxy_tunnel, "ProxyTunnel onData unexpected data");
            this.state.pending_response = None;
            ProxyTunnel::close_from_callback(proxy_nn, err!(UnexpectedData));
        }
    }
}

fn on_handshake(
    ctx: *mut HTTPClient,
    handshake_success: bool,
    ssl_error: uws::us_bun_verify_error_t,
) {
    let this = client_from_ctx(ctx);
    let Some(proxy_nn) = this.proxy_tunnel.as_ref().map(|p| p.data) else {
        return;
    };
    scoped_log!(http_proxy_tunnel, "ProxyTunnel onHandshake");
    let _guard = ProxyTunnel::ref_scope(proxy_nn);
    this.state.response_stage = HTTPStage::ProxyHeaders;
    this.state.request_stage = HTTPStage::ProxyHeaders;
    this.state.request_sent_len = 0;
    let handshake_error = HTTPCertError::from_verify_error(ssl_error);
    if handshake_success {
        scoped_log!(http_proxy_tunnel, "ProxyTunnel onHandshake success");
        this.flags.did_have_handshaking_error = handshake_error.error_no != 0;
        if this.flags.reject_unauthorized {
            if this.flags.did_have_handshaking_error {
                let err = crate::get_cert_error_from_no(handshake_error.error_no);
                ProxyTunnel::close_from_callback(proxy_nn, err);
                return;
            }
            debug_assert!(unsafe { (*proxy_nn.as_ptr()).wrapper.is_some() });
            let Some(ssl_ptr) = ProxyTunnel::wrapper_ssl(proxy_nn) else {
                return;
            };

            match ProxyTunnel::socket_of(proxy_nn) {
                &Socket::Ssl(socket) => {
                    if !this.check_server_identity::<true>(
                        socket,
                        handshake_error,
                        ssl_ptr.as_ptr(),
                        false,
                    ) {
                        scoped_log!(
                            http_proxy_tunnel,
                            "ProxyTunnel onHandshake checkServerIdentity failed"
                        );
                        return;
                    }
                }
                &Socket::Tcp(socket) => {
                    if !this.check_server_identity::<false>(socket, handshake_error, ssl_ptr.as_ptr(),
                        false,
                    ) {
                        scoped_log!(
                            http_proxy_tunnel,
                            "ProxyTunnel onHandshake checkServerIdentity failed"
                        );
                        return;
                    }
                }
                Socket::None => {}
            }
        }
        match ProxyTunnel::socket_of(proxy_nn) {
            &Socket::Ssl(socket) => {
                client_from_ctx(ctx).on_writable::<true, true>(socket);
            }
            &Socket::Tcp(socket) => {
                client_from_ctx(ctx).on_writable::<true, false>(socket);
            }
            Socket::None => {}
        }
    } else {
        scoped_log!(http_proxy_tunnel, "ProxyTunnel onHandshake failed");
  
        if this.flags.did_have_handshaking_error && handshake_error.error_no != 0 {
            let err = crate::get_cert_error_from_no(handshake_error.error_no);
            ProxyTunnel::close_from_callback(proxy_nn, err);
            return;
        }
        ProxyTunnel::close_from_callback(proxy_nn, err!(ConnectionRefused));
        return;
    }
}

pub fn write_encrypted(ctx: *mut HTTPClient, encoded_data: &[u8]) {
    let Some(proxy_nn) = client_from_ctx(ctx).proxy_tunnel.as_ref().map(|p| p.data) else {
        return;
    };
    let write_buffer = ProxyTunnel::write_buffer_of(proxy_nn);.
    if write_buffer.is_not_empty() {
        if write_buffer.write(encoded_data).is_err() {
            bun_core::out_of_memory();
        }
        return;
    }
    let written = match ProxyTunnel::socket_of(proxy_nn) {
        &Socket::Ssl(socket) => socket.write(encoded_data),
        &Socket::Tcp(socket) => socket.write(encoded_data),
        Socket::None => 0,
    };
    let pending = &encoded_data[usize::try_from(written).expect("int cast")..];
    if !pending.is_empty() {
        if write_buffer.write(pending).is_err() {
            bun_core::out_of_memory();
        }
    }
}
fn on_close(ctx: *mut HTTPClient) {
    let this = client_from_ctx(ctx);
    scoped_log!(
        http_proxy_tunnel,
        "ProxyTunnel onClose {}",
        if this.proxy_tunnel.is_none() {
            "tunnel is detached"
        } else {
            "tunnel exists"
        }
    );
    let Some(proxy_nn) = this.proxy_tunnel.as_ref().map(|p| p.data) else {
        return;
    };
    let proxy_ptr = proxy_nn.as_ptr();
    {
        let rc = ProxyTunnel::ref_count_of(proxy_nn);
        rc.set(rc.get() + 1);
    }
    let in_progress = this.state.stage != Stage::Done
        && this.state.stage != Stage::Fail
        && !this.state.flags.is_redirect_pending;
    if in_progress {
        if this.state.is_chunked_encoding() {
            match this.state.chunked_decoder._state {
                4 | 5 => {
                    this.state.flags.received_last_chunk = true;
                    progress_update_for_proxy_socket(ctx, proxy_nn);
                    crate::http_thread().schedule_proxy_deref(proxy_ptr);
                    return;
                }  _ => {}
            }
        } else if this.state.content_length.is_none()
            && this.state.response_stage == HTTPStage::Body
        {
            this.state.flags.received_last_chunk = true;
            progress_update_for_proxy_socket(ctx, proxy_nn);
            crate::http_thread().schedule_proxy_deref(proxy_ptr);
            return;
        }
    }
    let err = ProxyTunnel::shutdown_err_of(proxy_nn).get();
    match ProxyTunnel::socket_of(proxy_nn) {
        &Socket::Ssl(socket) => {
            this.close_and_fail::<true>(err, socket);
        }
        &Socket::Tcp(socket) => {
            this.close_and_fail::<false>(err, socket);
        }
        Socket::None => {}
    }
    ProxyTunnel::set_socket(proxy_nn, Socket::None);
    crate::http_thread().schedule_proxy_deref(proxy_ptr);
}
fn progress_update_for_proxy_socket(ctx: *mut HTTPClient, proxy: NonNull<ProxyTunnel>) {
    match ProxyTunnel::socket_of(proxy) {
        &Socket::Ssl(socket) => {
            let hctx = &raw mut crate::http_thread().https_context;
            client_from_ctx(ctx).progress_update::<true>(hctx, socket);
        }
        &Socket::Tcp(socket) => {
            let hctx = &raw mut crate::http_thread().http_context;
            client_from_ctx(ctx).progress_update::<false>(hctx, socket);
        }
        Socket::None => {}
    }
}
impl ProxyTunnel {
    pub fn start<const IS_SSL: bool>(
        this: &mut HTTPClient,
        socket: HTTPSocket<IS_SSL>,
        ssl_options: SSLConfig,
        start_payload: &[u8],
    ) {
        let proxy_tunnel = bun_core::heap::into_raw(Box::new(ProxyTunnel::default()));
        let proxy_nn = NonNull::new(proxy_tunnel).expect("heap::into_raw is non-null");
        let proxy_tunnel_ref = raw_as_mut(proxy_tunnel);
        let custom_options = ssl_options.as_usockets_for_client_verification();
        match ProxyTunnelWrapper::init_from_options(
            custom_options,
            true,
            SSLWrapperHandlers {
                on_open,
                on_data,
                on_handshake,
                on_close,
                write: write_encrypted,
                ctx: this.as_erased_ptr().as_ptr(),
            },
        ) {
            Ok(w) => proxy_tunnel_ref.wrapper = Some(w),
            Err(e) => {
                if e == InitError::OutOfMemory {
                    bun_core::out_of_memory();
                }
                proxy_tunnel_ref.detach_and_deref();
                this.close_and_fail::<IS_SSL>(err!(ConnectionRefused), socket);
                return;
            }
        }
        this.proxy_tunnel = Some(unsafe { RefPtr::adopt_ref(proxy_nn.as_ptr()) });
        proxy_tunnel_ref.socket = Socket::from_generic::<IS_SSL>(socket);
        let wrapper = ProxyTunnel::wrapper_mut(proxy_tunnel).unwrap();
        if !start_payload.is_empty() {
            scoped_log!(http_proxy_tunnel, "proxy tunnel start with payload");
            wrapper.start_with_payload(start_payload);
        } else {
            scoped_log!(http_proxy_tunnel, "proxy tunnel start");
            wrapper.start();
        }
    }
    pub fn close(&mut self, err: Error) {
        Self::close_raw(NonNull::from(&mut *self), err);
    }
    pub fn close_raw(this: NonNull<Self>, err: Error) {
        Self::shutdown_err_of(this).set(err);
        if let Some(wrapper) = ProxyTunnel::wrapper_mut(this.as_ptr()) {
            let _ = wrapper.shutdown(true);
        }
    }

    pub fn shutdown(&mut self) {
        if let Some(wrapper) = &mut self.wrapper {
            let _ = wrapper.shutdown(true);
        }
    }

    pub fn on_writable<const IS_SSL: bool>(&mut self, socket: HTTPSocket<IS_SSL>) {
        scoped_log!(http_proxy_tunnel, "ProxyTunnel onWritable");
        let self_nn = NonNull::from(&mut *self);
        let self_ptr = self_nn.as_ptr();
        let _guard = Self::ref_scope(self_nn);
        {
            let write_buffer = ProxyTunnel::write_buffer_of(self_nn);
            let encoded_data = write_buffer.slice();
            if !encoded_data.is_empty() {
                let written = socket.write(encoded_data);
                let written = usize::try_from(written).expect("int cast");
                if written == encoded_data.len() {
                    write_buffer.reset();
                } else {
                    write_buffer.cursor += written;
                }
            }
        }

        if let Some(wrapper) = ProxyTunnel::wrapper_mut(self_ptr) {
            let _ = wrapper.flush();
        }
    }

    pub fn receive(&mut self, buf: &[u8]) {
        let self_nn = NonNull::from(&mut *self);
        let _guard = Self::ref_scope(self_nn);
        if let Some(wrapper) = ProxyTunnel::wrapper_mut(self_nn.as_ptr()) {
            wrapper.receive_data(buf);
        }
    }

    pub fn write(&mut self, buf: &[u8]) -> Result<usize, Error> {
        if let Some(wrapper) = &mut self.wrapper {
            return wrapper.write_data(buf).map_err(|e| match e {
                WriteDataError::ConnectionClosed => err!(ConnectionClosed),
                WriteDataError::WantRead => err!(WantRead),
                WriteDataError::WantWrite => err!(WantWrite),
            });
        }
        Err(err!(ConnectionClosed))
    }

    #[inline]
    pub fn detach_socket(&mut self) {
        self.socket = Socket::None;
    }

    pub fn detach_and_deref(&mut self) {
        self.detach_socket();
        unsafe { ProxyTunnel::deref(self) };
    }
    pub fn detach_owner(&mut self, client: &HTTPClient) {
        self.socket = Socket::None;
        self.did_have_handshaking_error = client.flags.did_have_handshaking_error;
        self.established_with_reject_unauthorized = self.established_with_reject_unauthorized || client.flags.reject_unauthorized;
    }
    pub fn adopt<const IS_SSL: bool>(
        &mut self,
        client: &mut HTTPClient,
        socket: HTTPSocket<IS_SSL>,) {
        scoped_log!(http_proxy_tunnel, "ProxyTunnel adopt (reusing pooled tunnel)");
        self.write_buffer.reset();
        if let Some(wrapper) = &mut self.wrapper {
            wrapper.handlers.ctx = client.as_erased_ptr().as_ptr();
        }
        self.socket = Socket::from_generic::<IS_SSL>(socket);
        client.proxy_tunnel = Some(unsafe { RefPtr::from_raw(core::ptr::from_mut(&mut *self)) });
        client.flags.proxy_tunneling = false;
        client.flags.did_have_handshaking_error = self.did_have_handshaking_error;
        client.state.request_stage = HTTPStage::ProxyHeaders;
        client.state.response_stage = HTTPStage::ProxyHeaders;
        client.state.request_sent_len = 0;
    }
}
