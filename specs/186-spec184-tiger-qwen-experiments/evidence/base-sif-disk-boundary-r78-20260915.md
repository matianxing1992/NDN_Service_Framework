# Spec186 base SIF disk boundary — r78

The r78 local-first build passed source/definition preflight and Apptainer
1.5.3 startup, then failed while fully unpacking the 3,088,780,800-byte
SquashFS base. `unsquashfs` stopped near 70% while reading the CUDA
`libcufft.so.11.2.1.3` payload. The mounted image and targeted CUDA reads were
valid; the host had only about 12 GiB free while the checked-in `build/` tree
occupied about 8 GiB.

No final or partial SIF was emitted. The preflight now requires at least 16 GiB
free on the base image filesystem, or four times the compressed base size when
that is larger, before Apptainer extraction begins.
