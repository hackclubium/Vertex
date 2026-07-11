use arti_client::{TorClient, TorClientConfig};
use rustls::pki_types::ServerName;
use rustls::{ClientConfig, RootCertStore};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uchar};
use std::ptr;
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio_rustls::TlsConnector;
use url::Url;

#[repr(C)]
pub struct VertexArtiResponse {
    pub success: c_int,
    pub status: c_int,
    pub content_type: *mut c_char,
    pub final_url: *mut c_char,
    pub error: *mut c_char,
    pub body: *mut c_uchar,
    pub body_len: usize,
}

fn c_string(s: impl AsRef<str>) -> *mut c_char {
    CString::new(s.as_ref()).unwrap_or_else(|_| CString::new("invalid string").unwrap()).into_raw()
}

fn response_error(msg: impl AsRef<str>) -> *mut VertexArtiResponse {
    Box::into_raw(Box::new(VertexArtiResponse {
        success: 0,
        status: 0,
        content_type: c_string(""),
        final_url: c_string(""),
        error: c_string(msg),
        body: ptr::null_mut(),
        body_len: 0,
    }))
}

async fn read_http_response<S>(stream: &mut S, max_bytes: usize) -> Result<(i32, String, Vec<u8>), String>
where
    S: AsyncReadExt + Unpin,
{
    let mut buf = Vec::new();
    let mut chunk = [0u8; 8192];
    loop {
        let n = stream.read(&mut chunk).await.map_err(|e| e.to_string())?;
        if n == 0 { break; }
        buf.extend_from_slice(&chunk[..n]);
        if buf.len() > max_bytes + 256 * 1024 { return Err("response exceeds size limit".into()); }
    }
    let header_end = buf.windows(4).position(|w| w == b"\r\n\r\n").ok_or("missing response headers")?;
    let header = String::from_utf8_lossy(&buf[..header_end]);
    let mut lines = header.lines();
    let status_line = lines.next().ok_or("missing status line")?;
    let status = status_line.split_whitespace().nth(1).and_then(|s| s.parse::<i32>().ok()).unwrap_or(0);
    let mut content_type = String::new();
    for line in lines {
        if let Some((name, value)) = line.split_once(':') {
            if name.eq_ignore_ascii_case("content-type") { content_type = value.trim().to_string(); }
        }
    }
    let body = buf[(header_end + 4)..].to_vec();
    Ok((status, content_type, body))
}

async fn fetch_inner(url: &str, max_bytes: usize) -> Result<VertexArtiResponse, String> {
    let parsed = Url::parse(url).map_err(|e| e.to_string())?;
    let scheme = parsed.scheme();
    if scheme != "http" && scheme != "https" { return Err("unsupported scheme".into()); }
    let host = parsed.host_str().ok_or("missing host")?.to_string();
    if !host.ends_with(".onion") { return Err("embedded Arti only handles .onion hosts".into()); }
    let port = parsed.port_or_known_default().ok_or("missing port")?;
    let path = if parsed.path().is_empty() { "/" } else { parsed.path() };
    let path_query = match parsed.query() {
        Some(q) => format!("{}?{}", path, q),
        None => path.to_string(),
    };
    let req = format!(
        "GET {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: Vertex/0.1 Arti\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
        path_query, host
    );

    let mut config = TorClientConfig::builder();
    config.address_filter().allow_onion_addrs(true);
    let client = TorClient::create_bootstrapped(config.build().map_err(|e| e.to_string())?).await.map_err(|e| e.to_string())?;
    let stream = client.connect((host.as_str(), port)).await.map_err(|e| e.to_string())?;

    let (status, content_type, body) = if scheme == "https" {
        let mut roots = RootCertStore::empty();
        roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        let config = ClientConfig::builder().with_root_certificates(roots).with_no_client_auth();
        let connector = TlsConnector::from(Arc::new(config));
        let server_name = ServerName::try_from(host.clone()).map_err(|e| e.to_string())?;
        let mut tls = connector.connect(server_name, stream).await.map_err(|e| e.to_string())?;
        tls.write_all(req.as_bytes()).await.map_err(|e| e.to_string())?;
        read_http_response(&mut tls, max_bytes).await?
    } else {
        let mut plain = stream;
        plain.write_all(req.as_bytes()).await.map_err(|e| e.to_string())?;
        read_http_response(&mut plain, max_bytes).await?
    };

    let mut body = body;
    if body.len() > max_bytes { body.truncate(max_bytes); }
    let body_len = body.len();
    let mut boxed = body.into_boxed_slice();
    let body_ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    Ok(VertexArtiResponse {
        success: if status < 400 { 1 } else { 0 },
        status,
        content_type: c_string(content_type),
        final_url: c_string(url),
        error: c_string(""),
        body: body_ptr,
        body_len,
    })
}

#[no_mangle]
pub extern "C" fn vertex_arti_fetch(url: *const c_char, max_bytes: usize) -> *mut VertexArtiResponse {
    if url.is_null() { return response_error("null url"); }
    let url = unsafe { CStr::from_ptr(url) }.to_string_lossy().to_string();
    let rt = match tokio::runtime::Runtime::new() {
        Ok(rt) => rt,
        Err(e) => return response_error(e.to_string()),
    };
    match rt.block_on(fetch_inner(&url, max_bytes)) {
        Ok(resp) => Box::into_raw(Box::new(resp)),
        Err(e) => response_error(e),
    }
}

#[no_mangle]
pub extern "C" fn vertex_arti_free_response(resp: *mut VertexArtiResponse) {
    if resp.is_null() { return; }
    let resp = unsafe { Box::from_raw(resp) };
    unsafe {
        if !resp.content_type.is_null() { let _ = CString::from_raw(resp.content_type); }
        if !resp.final_url.is_null() { let _ = CString::from_raw(resp.final_url); }
        if !resp.error.is_null() { let _ = CString::from_raw(resp.error); }
        if !resp.body.is_null() { let _ = Vec::from_raw_parts(resp.body, resp.body_len, resp.body_len); }
    }
}
