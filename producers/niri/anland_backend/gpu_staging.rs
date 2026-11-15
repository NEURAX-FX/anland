use std::ffi::CString;
use std::io;
use std::os::fd::{OwnedFd, RawFd};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

use anyhow::Context as _;
use smithay::backend::allocator::dmabuf::Dmabuf;
use smithay::backend::egl::fence::EGLFence;
use smithay::backend::renderer::gles::{GlesRenderer, GlesTarget};
use smithay::backend::renderer::Bind;

pub fn queue_release_wait(renderer: &mut GlesRenderer, fd: Option<OwnedFd>) -> anyhow::Result<Option<EGLFence>> {
    let Some(fd) = fd else { return Ok(None) };
    // Do not use Renderer::wait: its fallback blocks the compositor on the CPU.
    renderer.with_context(|_| ()).context("make Anland EGL context current")?;
    let display = renderer.egl_context().display();
    let fence = EGLFence::import(display, fd).context("import Android SYNC_FD")?;
    fence.wait(display).context("queue Android release on the GPU")?;
    Ok(Some(fence))
}

/// This is a private ICD hook, not an EGL or Vulkan extension. Resolve it from
/// the ICD already loaded by Zink; loading a second driver copy would not affect
/// the EGL renderer's imports.
pub struct ImportPolicy {
    library: *mut libc::c_void,
    scope: unsafe extern "C" fn(libc::c_int) -> libc::c_int,
}

impl ImportPolicy {
    pub fn load(path: &Path) -> anyhow::Result<Self> {
        anyhow::ensure!(path.is_absolute(), "ANLAND_PANVK_ICD must be absolute");
        let path = CString::new(path.as_os_str().as_bytes())?;
        let library = unsafe { libc::dlopen(path.as_ptr(), libc::RTLD_NOLOAD | libc::RTLD_NOW) };
        anyhow::ensure!(!library.is_null(), "configured PanVK ICD is not loaded by EGL");
        let symbol = unsafe { libc::dlsym(library, c"panvk_kbase_scoped_gpu_uncached_import".as_ptr()) };
        if symbol.is_null() {
            unsafe { libc::dlclose(library) };
            anyhow::bail!("loaded PanVK ICD has no scoped staging import");
        }
        // SAFETY: the private ICD header defines the exact C ABI of this export.
        let scope = unsafe { std::mem::transmute::<*mut libc::c_void, unsafe extern "C" fn(libc::c_int) -> libc::c_int>(symbol) };
        Ok(Self { library, scope })
    }

    pub fn bind<'a>(
        &self, renderer: &mut GlesRenderer, dmabuf: &'a mut Dmabuf, fd: RawFd,
    ) -> anyhow::Result<GlesTarget<'a>> {
        let scope = self.begin(fd).context("begin scoped PanVK import")?;
        let bound = renderer.bind(dmabuf);
        let observed = scope.finish();
        let target = bound.context("bind staging DMA-BUF")?;
        anyhow::ensure!(observed, "PanVK did not confirm a GPU-uncached staging import");
        Ok(target)
    }

    pub fn begin(&self, fd: RawFd) -> io::Result<ImportScope<'_>> {
        let rc = unsafe { (self.scope)(fd) };
        if rc < 0 {
            return Err(io::Error::from_raw_os_error(-rc));
        }
        Ok(ImportScope(Some(self)))
    }
}

impl Drop for ImportPolicy {
    fn drop(&mut self) {
        unsafe { libc::dlclose(self.library) };
    }
}

/// End the thread-local scope on every bind result, including an unwind.
pub struct ImportScope<'a>(Option<&'a ImportPolicy>);

impl ImportScope<'_> {
    pub fn finish(mut self) -> bool {
        self.end() == 1
    }

    fn end(&mut self) -> libc::c_int {
        self.0.take().map_or(0, |policy| unsafe { (policy.scope)(-1) })
    }
}

impl Drop for ImportScope<'_> {
    fn drop(&mut self) {
        self.end();
    }
}
