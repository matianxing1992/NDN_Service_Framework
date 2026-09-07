# JSON for Modern C++

Unmodified single header and MIT license from nlohmann/json v3.11.3:

- https://github.com/nlohmann/json/tree/v3.11.3
- json.hpp SHA-256: `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`
- LICENSE.MIT SHA-256: `86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14`

Used for native typed JSON values, parsing and string escaping. No shared library,
Python runtime or build-time network resolution. Canonical float formatting is
owned by NativeCanonicalJson.hpp to match the frozen Python JSON identity bytes.
Keep this header out of the installed public DTO headers; include it only in
native JSON implementation helpers and their tests.
