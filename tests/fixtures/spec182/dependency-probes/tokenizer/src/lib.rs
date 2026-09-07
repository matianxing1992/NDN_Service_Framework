//! T001-only C ABI feasibility probe. Production integration belongs to T007.
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use tokenizers::Tokenizer;

#[repr(C)]
pub struct NdiTokenResult {
    data: *mut u8,
    size: usize,
    code: i32,
}

fn output(bytes: Vec<u8>, code: i32) -> NdiTokenResult {
    if bytes.is_empty() {
        return NdiTokenResult { data: std::ptr::null_mut(), size: 0, code };
    }
    let mut owned = bytes.into_boxed_slice();
    let result = NdiTokenResult { data: owned.as_mut_ptr(), size: owned.len(), code };
    std::mem::forget(owned);
    result
}

// Result conversion is outside the algorithm; no Rust panic crosses the ABI.
fn boundary(work: impl FnOnce() -> Result<Vec<u8>, String>) -> NdiTokenResult {
    match catch_unwind(AssertUnwindSafe(work)) {
        Ok(Ok(bytes)) => output(bytes, 0),
        Ok(Err(message)) => output(message.into_bytes(), 1),
        Err(_) => output(b"tokenizer panic".to_vec(), 2),
    }
}

unsafe fn borrowed<'a, T>(data: *const T, size: usize, limit: usize) -> Result<&'a [T], String> {
    if size > limit || (size != 0 && data.is_null()) {
        return Err("invalid buffer range".into());
    }
    if size == 0 { Ok(&[]) } else { Ok(std::slice::from_raw_parts(data, size)) }
}

fn flag(value: u8) -> Result<bool, String> {
    match value { 0 => Ok(false), 1 => Ok(true), _ => Err("invalid special token flag".into()) }
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_create(json: *const u8, size: usize, handle: *mut *mut c_void) -> NdiTokenResult {
    if handle.is_null() { return output(b"null handle output".to_vec(), 1); }
    *handle = std::ptr::null_mut();
    boundary(|| {
        let bytes = borrowed(json, size, 32 * 1024 * 1024)?;
        if bytes.is_empty() { return Err("empty tokenizer".into()); }
        let tokenizer = Tokenizer::from_bytes(bytes).map_err(|e| e.to_string())?;
        *handle = Box::into_raw(Box::new(tokenizer)).cast();
        Ok(Vec::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_encode(handle: *const c_void, text: *const u8, size: usize, add_special: u8) -> NdiTokenResult {
    boundary(|| {
        if handle.is_null() { return Err("null tokenizer".into()); }
        let tokenizer = &*handle.cast::<Tokenizer>();
        let input = std::str::from_utf8(borrowed(text, size, 1024 * 1024)?).map_err(|e| e.to_string())?;
        let encoded = tokenizer.encode(input, flag(add_special)?).map_err(|e| e.to_string())?;
        if encoded.get_ids().len() > 1024 * 1024 { return Err("token count limit".into()); }
        Ok(encoded.get_ids().iter().flat_map(|id| id.to_le_bytes()).collect())
    })
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_decode(handle: *const c_void, ids: *const u32, count: usize, skip_special: u8) -> NdiTokenResult {
    boundary(|| {
        if handle.is_null() { return Err("null tokenizer".into()); }
        let tokenizer = &*handle.cast::<Tokenizer>();
        let ids = borrowed(ids, count, 1024 * 1024)?;
        let skip = flag(skip_special)?;
        if ids.iter().any(|id| tokenizer.id_to_token(*id).is_none()) { return Err("unknown token id".into()); }
        let text = tokenizer.decode(ids, skip).map_err(|e| e.to_string())?;
        if text.len() > 16 * 1024 * 1024 { return Err("decoded text limit".into()); }
        Ok(text.into_bytes())
    })
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_free(data: *mut u8, size: usize) {
    if !data.is_null() { drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, size))); }
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_destroy(handle: *mut c_void) {
    if !handle.is_null() { drop(Box::from_raw(handle.cast::<Tokenizer>())); }
}
