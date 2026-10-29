#ifndef GFX_RENDER_POLICY_H
#define GFX_RENDER_POLICY_H
#ifndef SW9_PERF
#define SW9_PERF 0
#endif
#ifndef GFX_RENDER_PIPELINE
#define GFX_RENDER_PIPELINE SW9_PERF
#endif
#ifndef GFX_FRAME_DIAGNOSTIC
#define GFX_FRAME_DIAGNOSTIC (!SW9_PERF)
#endif
#if GFX_RENDER_PIPELINE && GFX_FRAME_DIAGNOSTIC
#error "Asynchronous rendering and per-frame CRC cannot share an unfinished frame"
#endif
#endif
