//! Private C ABI around the pinned Hugging Face Rust tokenizer.
//!
//! The bridge owns every returned buffer and converts every panic to an error
//! result.  It deliberately exposes no tokenizer-specific Rust type to C++.
//!
//! decode_stable implements the frozen stable-prefix contract
//! (native-token-stream-design.md "Stable Prefix Algorithms"): a full decode
//! for profiles whose output cannot be revised (null, WordPiece), a held
//! trailing byte run for ByteFallback, and a Rust-std UTF-8 prefix walk for
//! ByteLevel.  Any other decoder pipeline is rejected for stable text on
//! every call (empty ids included) while full encode/decode keep working.

use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use tokenizers::{DecoderWrapper, Tokenizer};

const JSON_LIMIT: usize = 32 * 1024 * 1024;
const TEXT_LIMIT: usize = 1024 * 1024;
const TOKEN_LIMIT: usize = 1024 * 1024;
const OUTPUT_LIMIT: usize = 16 * 1024 * 1024;

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

fn boundary(work: impl FnOnce() -> Result<Vec<u8>, String>) -> NdiTokenResult {
    match catch_unwind(AssertUnwindSafe(work)) {
        Ok(Ok(bytes)) => output(bytes, 0),
        Ok(Err(message)) => output(message.into_bytes(), 1),
        Err(_) => output(b"tokenizer panic".to_vec(), 2),
    }
}

unsafe fn borrowed<'a, T>(data: *const T, size: usize, limit: usize)
    -> Result<&'a [T], String>
{
    if size > limit || (size != 0 && data.is_null()) {
        return Err("invalid buffer range".into());
    }
    if size == 0 { Ok(&[]) } else { Ok(std::slice::from_raw_parts(data, size)) }
}

fn flag(value: u8) -> Result<bool, String> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err("invalid special token flag".into()),
    }
}

#[derive(Clone, Copy, Debug)]
enum Profile {
    Null,
    WordPiece,
    ByteFallback,
    ByteLevel,
    Unsupported,
}

/// Canonical GPT-2 byte alphabet (char -> byte), mirroring the reference
/// dependency probe's python `byte_alphabet()` and HF's `bytes_char`.
fn byte_alphabet() -> HashMap<char, u8> {
    let mut map = HashMap::with_capacity(256);
    for byte in 0u32..256 {
        let direct = (33..=126).contains(&byte)
            || (161..=172).contains(&byte)
            || (174..=255).contains(&byte);
        let value = if direct {
            byte
        } else {
            // Missing bytes in ascending order become chars 256+n.  byte 32 is
            // the 33rd missing byte -> char 288 = U+0120 'Ġ' (reference check).
            256 + (0..byte).filter(|other| {
                !(33..=126).contains(other)
                    && !(161..=172).contains(other)
                    && !(174..=255).contains(other)
            }).count() as u32
        };
        map.insert(char::from_u32(value).expect("alphabet codepoint"), byte as u8);
    }
    map
}

/// Same byte-token shape test the frozen HF ByteFallback decoder uses:
/// exactly six bytes, "<0x" + two hex digits (case-insensitive) + ">".
fn is_byte_token(content: &str) -> bool {
    content.len() == 6
        && content.starts_with("<0x")
        && content.ends_with('>')
        && u8::from_str_radix(&content[3..5], 16).is_ok()
}

/// Non-final stable text for a ByteLevel byte stream: every complete valid
/// prefix is committed, every determined-invalid subpart becomes one U+FFFD
/// (Rust-std lossy semantics), and only a trailing incomplete sequence is
/// held.  Legal U+FFFD input characters are never deleted.
fn stable_prefix(bytes: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(bytes.len());
    let mut offset = 0;
    while offset < bytes.len() {
        match std::str::from_utf8(&bytes[offset..]) {
            Ok(text) => {
                out.extend_from_slice(text.as_bytes());
                break;
            }
            Err(error) => {
                let valid = error.valid_up_to();
                out.extend_from_slice(&bytes[offset..offset + valid]);
                offset += valid;
                match error.error_len() {
                    Some(bad) => {
                        out.extend_from_slice("\u{fffd}".as_bytes());
                        offset += bad;
                    }
                    None => break, // trailing incomplete sequence stays pending
                }
            }
        }
    }
    out
}

struct Owner {
    tokenizer: Tokenizer,
    profile: Profile,
    /// Canonical GPT-2 byte alphabet (char -> byte) for ByteLevel profiles.
    byte_map: HashMap<char, u8>,
    /// Resolved id -> content with the added-vocabulary precedence the
    /// Tokenizer::decode lookup uses (simple_id_to_token before model).
    /// Present only for profiles whose stable path classifies tokens.
    content: HashMap<u32, String>,
    /// Contents that decode() filters under skip_special_tokens.  The filter
    /// is content-based (is_special_token), so a model-vocab token whose
    /// content coincides with an added special is covered as well.
    special_contents: HashSet<String>,
}

impl Owner {
    fn special(&self, id: u32) -> bool {
        self.content
            .get(&id)
            .map(|content| self.special_contents.contains(content))
            .unwrap_or(false)
    }

    /// Skip-filtered id list: the exact token list decode() would hand to the
    /// decoder for this skip flag.
    fn effective<'a>(&'a self, ids: &'a [u32], skip: bool) -> Vec<u32> {
        if !skip {
            return ids.to_vec();
        }
        ids.iter().copied().filter(|id| !self.special(*id)).collect()
    }

    fn decode(&self, ids: &[u32], skip: bool) -> Result<String, String> {
        self.tokenizer.decode(ids, skip).map_err(|e| e.to_string())
    }

    /// Stable text per the frozen profile table.  `ids` has already passed the
    /// unknown-id validation shared with the full decode path.
    fn stable_text(&self, ids: &[u32], skip: bool, final_output: bool)
        -> Result<String, String>
    {
        let text = match self.profile {
            Profile::Null | Profile::WordPiece => {
                // HF full decode: null joins filtered contents with spaces,
                // WordPiece is delegated to the real decoder.  Output after
                // position i cannot be revised by later tokens for these
                // supported shapes, so the whole prefix is stable already;
                // the `final` flag cannot change the result.
                self.decode(ids, skip)?
            }
            Profile::ByteFallback => {
                if final_output {
                    self.decode(ids, skip)?
                } else {
                    let effective = self.effective(ids, skip);
                    // Hold the last byte run that no non-byte token has
                    // closed; decode only the already-closed part.  Skipped
                    // specials vanish before run analysis, retained non-byte
                    // specials close runs - exactly the frozen boundary rule.
                    let mut run_start = effective.len();
                    while run_start > 0 {
                        let id = effective[run_start - 1];
                        let byte_token = self.content.get(&id)
                            .map(|content| is_byte_token(content))
                            .unwrap_or(false);
                        if !byte_token { break; }
                        run_start -= 1;
                    }
                    self.decode(&effective[..run_start], skip)?
                }
            }
            Profile::ByteLevel => {
                if final_output {
                    self.decode(ids, skip)?
                } else {
                    let effective = self.effective(ids, skip);
                    let mut raw = Vec::new();
                    for id in &effective {
                        let content = self.content.get(id)
                            .expect("stable path content was validated");
                        if content.chars().all(|c| self.byte_map.contains_key(&c)) {
                            for c in content.chars() {
                                raw.push(self.byte_map[&c]);
                            }
                        } else {
                            raw.extend_from_slice(content.as_bytes());
                        }
                    }
                    String::from_utf8(stable_prefix(&raw))
                        .expect("stable prefix is valid UTF-8 by construction")
                }
            }
            Profile::Unsupported => {
                // Fail closed on every stable call (empty ids included):
                // never guess a prefix-stability property for an unproven
                // decoder pipeline.  Full encode/decode stay available.
                return Err("unsupported decoder profile for stable text".into());
            }
        };
        if text.len() > OUTPUT_LIMIT {
            return Err("decoded text limit".into());
        }
        Ok(text)
    }
}

fn full_text(owner: &Owner, ids: &[u32], skip: bool) -> Result<String, String> {
    if ids.len() > TOKEN_LIMIT {
        return Err("token count limit".into());
    }
    if ids.iter().any(|id| owner.tokenizer.id_to_token(*id).is_none()) {
        return Err("unknown token id".into());
    }
    let text = owner.decode(ids, skip)?;
    if text.len() > OUTPUT_LIMIT {
        return Err("decoded text limit".into());
    }
    Ok(text)
}

fn check_ids(tokenizer: &Tokenizer, ids: &[u32]) -> Result<(), String> {
    if ids.len() > TOKEN_LIMIT {
        return Err("token count limit".into());
    }
    if ids.iter().any(|id| tokenizer.id_to_token(*id).is_none()) {
        return Err("unknown token id".into());
    }
    Ok(())
}

fn build_owner(tokenizer: Tokenizer) -> Owner {
    use Profile::*;
    let profile = match tokenizer.get_decoder() {
        None => Null,
        Some(DecoderWrapper::WordPiece(_)) => WordPiece,
        Some(DecoderWrapper::ByteFallback(_)) => ByteFallback,
        Some(DecoderWrapper::ByteLevel(_)) => ByteLevel,
        Some(_) => Unsupported,
    };
    if !matches!(profile, ByteFallback | ByteLevel) {
        return Owner {
            tokenizer,
            profile,
            byte_map: HashMap::new(),
            content: HashMap::new(),
            special_contents: HashSet::new(),
        };
    }

    // Content classification needs the exact lookup decode() performs and the
    // exact content-level special filter.  The added-vocabulary internals are
    // private, so enumerate every reachable id (model ids from the raw vocab,
    // added ids from the with-added vocab) and derive both properties from the
    // public API at create time; each call then stays stateless.
    let plain = tokenizer.get_vocab(false);
    let with_added = tokenizer.get_vocab(true);
    let mut content: HashMap<u32, String> = HashMap::with_capacity(plain.len() + 8);
    for id in plain.values().chain(with_added.values()) {
        if !content.contains_key(id) {
            if let Some(text) = tokenizer.id_to_token(*id) {
                content.insert(*id, text);
            }
        }
    }
    let mut special_contents = HashSet::new();
    for id in content.keys() {
        let text = &content[id];
        if special_contents.contains(text) {
            continue;
        }
        // Probe with the real decoder: skipped single token decodes to "" and
        // the retained one does not, iff its content is special.
        let empty = tokenizer.decode(&[*id], true).map(|t| t.is_empty()).unwrap_or(false);
        let kept = tokenizer.decode(&[*id], false).map(|t| !t.is_empty()).unwrap_or(false);
        if empty && kept {
            special_contents.insert(text.clone());
        }
    }
    Owner { tokenizer, profile, byte_map: byte_alphabet(), content, special_contents }
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_create(
    json: *const u8,
    size: usize,
    handle: *mut *mut c_void,
) -> NdiTokenResult {
    if handle.is_null() {
        return output(b"null handle output".to_vec(), 1);
    }
    *handle = std::ptr::null_mut();
    boundary(|| {
        let bytes = borrowed(json, size, JSON_LIMIT)?;
        if bytes.is_empty() {
            return Err("empty tokenizer".into());
        }
        let tokenizer = Tokenizer::from_bytes(bytes).map_err(|e| e.to_string())?;
        *handle = Box::into_raw(Box::new(build_owner(tokenizer))).cast();
        Ok(Vec::new())
    })
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_encode(
    handle: *const c_void,
    text: *const u8,
    size: usize,
    add_special: u8,
) -> NdiTokenResult {
    boundary(|| {
        if handle.is_null() {
            return Err("null tokenizer".into());
        }
        let owner = &*handle.cast::<Owner>();
        let input = std::str::from_utf8(borrowed(text, size, TEXT_LIMIT)?)
            .map_err(|e| e.to_string())?;
        let encoded = owner.tokenizer.encode(input, flag(add_special)?).map_err(|e| e.to_string())?;
        if encoded.get_ids().len() > TOKEN_LIMIT {
            return Err("token count limit".into());
        }
        Ok(encoded.get_ids().iter().flat_map(|id| id.to_le_bytes()).collect())
    })
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_decode(
    handle: *const c_void,
    ids: *const u32,
    count: usize,
    skip_special: u8,
) -> NdiTokenResult {
    boundary(|| {
        if handle.is_null() {
            return Err("null tokenizer".into());
        }
        let owner = &*handle.cast::<Owner>();
        full_text(owner, borrowed(ids, count, TOKEN_LIMIT)?, flag(skip_special)?)
            .map(|text| text.into_bytes())
    })
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_decode_stable(
    handle: *const c_void,
    ids: *const u32,
    count: usize,
    skip_special: u8,
    final_output: u8,
) -> NdiTokenResult {
    boundary(|| {
        if handle.is_null() {
            return Err("null tokenizer".into());
        }
        let owner = &*handle.cast::<Owner>();
        let ids = borrowed(ids, count, TOKEN_LIMIT)?;
        check_ids(&owner.tokenizer, ids)?;
        let skip = flag(skip_special)?;
        let final_flag = flag(final_output)?;
        owner.stable_text(ids, skip, final_flag).map(|text| text.into_bytes())
    })
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_free(data: *mut u8, size: usize) {
    if !data.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, size)));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ndi_token_destroy(handle: *mut c_void) {
    if !handle.is_null() {
        drop(Box::from_raw(handle.cast::<Owner>()));
    }
}
