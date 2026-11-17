Synthetic blue 160x120, 30 fps, four-second H.264/MP4 input for native capture/decoder tests.
No camera, network, model or application video is required. Hex is decoded into an owned
temporary file by the fixture and removed after the pipelines stop.

SHA-256 of decoded bytes: `0d784cd980a755de413fc388a1a7145dcc399451bd3101f9557cc11d3b1b21a0`.

Generation: `ffmpeg -f lavfi -i color=c=blue:s=160x120:r=30 -t 4 -c:v libx264
-pix_fmt yuv420p -movflags +faststart sample.mp4`. The frozen bytes are the test input;
FFmpeg is not a test-time dependency.
