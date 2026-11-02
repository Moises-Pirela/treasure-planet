# Third-party code

| Directory | What | Version | Licence |
| --- | --- | --- | --- |
| `gl/` | Khronos OpenGL core profile headers (`glcorearb.h`, `khrplatform.h`) | as vendored by clockwork-c | MIT, in each file's header |
| `vulkan/` | Khronos Vulkan headers | 1.3.283 | Apache-2.0 / MIT, see `vulkan/LICENSE.md` |
| `volk/` | Vulkan function loader | 283 | MIT, see `volk/LICENSE.md` |

The Vulkan headers and volk were copied from `override-engine/thirdparty`, and only the headers this project includes were kept.
