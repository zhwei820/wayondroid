//! The rootless engine's GPU server: guest gfxstream-vk connects to it
//! over kumquat's socket, and it renders through the host's Vulkan.
//!
//! Usage: vine_kumquat GPU_SOCKET GRALLOC_SOCKET [AHB_SOCKET]
//!
//! The guest's gralloc allocates its buffers on GRALLOC_SOCKET (see the
//! wayondroid patch to the kumquat server). AHB_SOCKET, an abstract
//! SOCK_SEQPACKET name, hands the app a resource's AHardwareBuffer so it
//! can show guest frames without copying them: the app sends a u32
//! resource id, gets a u32 status (0: ok) and then the buffer, in
//! AHardwareBuffer_sendHandleToUnixSocket's format.

use std::ffi::CStr;
use std::os::raw::c_int;

use kumquat_virtio::kumquat::KumquatBuilder;

#[repr(C)]
struct NativeHandle {
    version: c_int,
    num_fds: c_int,
    num_ints: c_int,
    data: [c_int; 0],
}

type GetNativeHandle = unsafe extern "C" fn(*const libc::c_void) -> *const NativeHandle;
type SendHandle = unsafe extern "C" fn(*const libc::c_void, c_int) -> c_int;
type Release = unsafe extern "C" fn(*mut libc::c_void);

extern "C" {
    // libgfxstream_backend (gfxstream patch 02): a ColorBuffer's AHB, acquired
    fn tawc_gfxstream_lookup_ahb(handle: u32) -> *mut libc::c_void;
}

fn nativewindow(name: &CStr) -> *mut libc::c_void {
    static LIB: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    let lib = *LIB.get_or_init(|| unsafe {
        let lib = libc::dlopen(c"libnativewindow.so".as_ptr(), libc::RTLD_NOW);
        if lib.is_null() {
            eprintln!("vine_kumquat: dlopen libnativewindow.so: {}", CStr::from_ptr(libc::dlerror()).to_string_lossy());
        }
        lib as usize
    });
    if lib == 0 {
        return std::ptr::null_mut();
    }
    unsafe { libc::dlsym(lib as *mut libc::c_void, name.as_ptr()) }
}

fn get_native_handle() -> Option<GetNativeHandle> {
    let sym = nativewindow(c"AHardwareBuffer_getNativeHandle");
    (!sym.is_null()).then(|| unsafe { std::mem::transmute::<*mut libc::c_void, GetNativeHandle>(sym) })
}

/// Host-visible memory is an AHB; the guest maps its gralloc dmabuf.
fn ahb_export(ahb: i64) -> Result<i32, ()> {
    let get = get_native_handle().ok_or(())?;
    let nh = unsafe { get(ahb as *const libc::c_void) };
    if nh.is_null() || unsafe { (*nh).num_fds } < 1 {
        return Err(());
    }
    let fd = unsafe { *(*nh).data.as_ptr() };
    let dup = unsafe { libc::dup(fd) };
    if dup < 0 { Err(()) } else { Ok(dup) }
}

/// Answers one app connection on the AHB socket until it closes
fn serve_ahb(fd: c_int, send: SendHandle, release: Release) {
    loop {
        let mut id = [0u8; 4];
        let n = unsafe { libc::recv(fd, id.as_mut_ptr().cast(), 4, 0) };
        if n != 4 {
            break;
        }
        let ahb = unsafe { tawc_gfxstream_lookup_ahb(u32::from_le_bytes(id)) };
        let status: u32 = if ahb.is_null() { 1 } else { 0 };
        let sent = unsafe { libc::send(fd, status.to_le_bytes().as_ptr().cast(), 4, libc::MSG_NOSIGNAL) } == 4
            && (ahb.is_null() || unsafe { send(ahb, fd) } == 0);
        if !ahb.is_null() {
            unsafe { release(ahb) };
        }
        if !sent {
            break;
        }
    }
    unsafe { libc::close(fd) };
}

fn listen_ahb(name: &str) {
    let send = nativewindow(c"AHardwareBuffer_sendHandleToUnixSocket");
    let release = nativewindow(c"AHardwareBuffer_release");
    if send.is_null() || release.is_null() {
        eprintln!("vine_kumquat: no AHardwareBuffer socket API, frames go through the CPU");
        return;
    }
    let send = unsafe { std::mem::transmute::<*mut libc::c_void, SendHandle>(send) };
    let release = unsafe { std::mem::transmute::<*mut libc::c_void, Release>(release) };
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0) };
    let mut sa: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    sa.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let bytes = name.as_bytes();
    if fd < 0 || bytes.len() + 1 > sa.sun_path.len() {
        eprintln!("vine_kumquat: cannot make the AHB socket");
        return;
    }
    // Abstract: a leading NUL, no terminator
    for (i, b) in bytes.iter().enumerate() {
        sa.sun_path[i + 1] = *b as libc::c_char;
    }
    let len = (std::mem::size_of::<libc::sa_family_t>() + 1 + bytes.len()) as libc::socklen_t;
    if unsafe { libc::bind(fd, (&sa as *const libc::sockaddr_un).cast(), len) } != 0
        || unsafe { libc::listen(fd, 4) } != 0
    {
        eprintln!("vine_kumquat: AHB socket {}: {}", name, std::io::Error::last_os_error());
        unsafe { libc::close(fd) };
        return;
    }
    std::thread::spawn(move || loop {
        let c = unsafe { libc::accept4(fd, std::ptr::null_mut(), std::ptr::null_mut(), libc::SOCK_CLOEXEC) };
        if c >= 0 {
            std::thread::spawn(move || serve_ahb(c, send, release));
        } else if std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
            eprintln!("vine_kumquat: AHB socket accept: {}", std::io::Error::last_os_error());
            return;
        }
    });
}

/// kumquat and rutabaga log through `log`; send it to our log file
struct StderrLog;

impl log::Log for StderrLog {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Warn
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            eprintln!("vine_kumquat: {} {}", r.level(), r.args());
        }
    }
    fn flush(&self) {}
}

static LOGGER: StderrLog = StderrLog;

/// SIGSEGV/SIGBUS/SIGILL: the fault and the frame-pointer chain, each pc
/// as library+offset, to stderr (vine_kumquat.log), then the default
/// death. The vendor drops this app's logcat past a budget, tombstone
/// lines included, so a crash would otherwise leave nothing to read.
const CRASH_SIGNALS: [c_int; 3] = [libc::SIGSEGV, libc::SIGBUS, libc::SIGILL];

fn install_crash_report() {
    // SAFETY: a plain sigaction with a handler that only reads its context.
    // SA_ONSTACK: bionic gives every thread a signal stack, so a stack
    // overflow still reports.
    unsafe {
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = crash_report as *const () as usize;
        sa.sa_flags = libc::SA_SIGINFO | libc::SA_RESETHAND | libc::SA_ONSTACK;
        libc::sigemptyset(&mut sa.sa_mask);
        for sig in CRASH_SIGNALS {
            libc::sigaction(sig, &sa, std::ptr::null_mut());
        }
    }
    // A library loaded later (the host's Vulkan driver) may take these
    // signals over and exit quietly: name it, and take them back
    std::thread::spawn(|| loop {
        std::thread::sleep(std::time::Duration::from_millis(100));
        for sig in CRASH_SIGNALS {
            // SAFETY: querying and resetting a handler
            unsafe {
                let mut old: libc::sigaction = std::mem::zeroed();
                libc::sigaction(sig, std::ptr::null(), &mut old);
                if old.sa_sigaction != crash_report as *const () as usize {
                    let mut l = Line::new();
                    l.str(b"vine_kumquat: signal ").hex(sig as u64).str(b" handler replaced by");
                    l.emit();
                    report_frame(old.sa_sigaction as u64);
                    let mut sa: libc::sigaction = std::mem::zeroed();
                    sa.sa_sigaction = crash_report as *const () as usize;
                    sa.sa_flags = libc::SA_SIGINFO | libc::SA_RESETHAND | libc::SA_ONSTACK;
                    libc::sigemptyset(&mut sa.sa_mask);
                    libc::sigaction(sig, &sa, std::ptr::null_mut());
                }
            }
        }
    });
}

struct Line {
    buf: [u8; 512],
    len: usize,
}

impl Line {
    fn new() -> Self {
        Line { buf: [0; 512], len: 0 }
    }
    fn str(&mut self, s: &[u8]) -> &mut Self {
        for &b in s {
            if self.len < self.buf.len() - 1 {
                self.buf[self.len] = b;
                self.len += 1;
            }
        }
        self
    }
    fn hex(&mut self, v: u64) -> &mut Self {
        let mut digits = [0u8; 16];
        let mut n = 0;
        let mut v = v;
        loop {
            digits[n] = b"0123456789abcdef"[(v & 15) as usize];
            n += 1;
            v >>= 4;
            if v == 0 {
                break;
            }
        }
        self.str(b"0x");
        while n > 0 {
            n -= 1;
            self.str(&digits[n..n + 1]);
        }
        self
    }
    fn emit(&mut self) {
        self.str(b"\n");
        // SAFETY: writing our own buffer to stderr
        unsafe { libc::write(2, self.buf.as_ptr() as *const libc::c_void, self.len) };
        self.len = 0;
    }
}

fn report_frame(pc: u64) {
    let mut l = Line::new();
    l.str(b"  ").hex(pc);
    // SAFETY: dladdr fills `info` with pointers into the loader's tables
    unsafe {
        let mut info: libc::Dl_info = std::mem::zeroed();
        if libc::dladdr(pc as *const libc::c_void, &mut info) != 0 && !info.dli_fname.is_null() {
            let lib = CStr::from_ptr(info.dli_fname).to_bytes();
            let base = lib.iter().rposition(|&c| c == b'/').map_or(0, |i| i + 1);
            l.str(b" ").str(&lib[base..]).str(b"+").hex(pc - info.dli_fbase as u64);
            if !info.dli_sname.is_null() {
                l.str(b" ").str(CStr::from_ptr(info.dli_sname).to_bytes())
                    .str(b"+").hex(pc - info.dli_saddr as u64);
            }
        }
    }
    l.emit();
}

extern "C" fn crash_report(sig: c_int, _info: *mut libc::siginfo_t, ctx: *mut libc::c_void) {
    // Return addresses may carry a PAC signature above the 48-bit VA
    const VA: u64 = 0xffff_ffff_ffff;
    // SAFETY: the kernel passes a valid ucontext_t; a bad frame pointer
    // faults again, which SA_RESETHAND turns into the default death
    unsafe {
        let mc = &(*(ctx as *const libc::ucontext_t)).uc_mcontext;
        Line::new()
            .str(b"vine_kumquat: signal ").hex(sig as u64)
            .str(b" fault ").hex(mc.fault_address)
            .str(b" tid ").hex(libc::gettid() as u64)
            .emit();
        report_frame(mc.pc);
        report_frame(mc.regs[30] & VA);
        let mut fp = mc.regs[29];
        for _ in 0..40 {
            if fp == 0 || fp % 8 != 0 {
                break;
            }
            let next = *(fp as *const u64);
            let ret = *((fp + 8) as *const u64) & VA;
            if ret == 0 {
                break;
            }
            report_frame(ret);
            if next <= fp {
                break;
            }
            fp = next;
        }
    }
}

fn main() {
    let _ = log::set_logger(&LOGGER).map(|()| log::set_max_level(log::LevelFilter::Warn));
    install_crash_report();
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 && args.len() != 4 {
        eprintln!("usage: vine_kumquat GPU_SOCKET GRALLOC_SOCKET [AHB_SOCKET]");
        std::process::exit(2);
    }
    let socket = args[1].clone();
    let _ = kumquat_virtio::rutabaga_gfx::set_ahb_export_callback(ahb_export);
    let _ = std::fs::remove_file(&socket);
    let mut kumquat = match KumquatBuilder::new()
        .set_capset_names("gfxstream-vulkan".into())
        // Needs /dev/udmabuf, which apps may not open; FATAL when it fails
        .set_renderer_features("VulkanAllocateHostVisibleAsUdmabuf:disabled".into())
        .set_gpu_socket(Some(socket.clone()))
        .set_gralloc_socket(Some(args[2].clone()))
        .build()
    {
        Ok(k) => k,
        Err(e) => {
            eprintln!("vine_kumquat: {:?}", e);
            std::process::exit(1);
        }
    };
    if let Some(name) = args.get(3).filter(|n| !n.is_empty()) {
        listen_ahb(name);
    }
    eprintln!("vine_kumquat: listening on {}", socket);
    loop {
        if let Err(e) = kumquat.run() {
            eprintln!("vine_kumquat: {:?}", e);
            std::process::exit(1);
        }
    }
}
