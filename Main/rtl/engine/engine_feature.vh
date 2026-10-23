// 由 tools/check/feature_build.py 生成，是特性宏的唯一定义处，不要手改。
// 构型 0000000-111-d    CAPS 复位值 0x0000040F
// 关闭的机制不定义对应宏。禁止用 -DGFX_HAS_x=0 表示关闭：
// 存在式宏即使值为 0，`ifdef 仍然成立，实测会进入启用分支。
`ifndef GFX_ENGINE_FEATURE_VH
`define GFX_ENGINE_FEATURE_VH

// ---- 本构型启用的机制 ----
`define GFX_EQ_EXT
`define GFX_ALPHA
`define GFX_KEY
`define GFX_DELIVERY

`endif
