# Spec180 YOLO26n fixed input fixture

This is a repository-owned, deterministic 4×4 RGB PPM fixture. It is newly
generated for this qualification (no external image copyright or network
fetch), may be redistributed under the repository's project license, and is
resized by the declared preprocessing contract to the static 640×640 input.
The fixture bytes, preprocessing identity, and resulting oracle are bound by
the T004 manifest; do not replace it with an unrecorded local image.

- Source/revision: `spec180-fixed-fixture-v1`
- File: `fixed-fixture.ppm`
- SHA-256: `7edf1f524ef450be6ee2304b3c0b47b70c3d72610c18f2f2fa28157d7b8a113c`
- Preprocessing: RGB, float32, NCHW, values in `[0,1]`, resize to 640×640
- The fixture is for deterministic regression only; it is not a training or
  performance dataset.
