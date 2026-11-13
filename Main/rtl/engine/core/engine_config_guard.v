// 由 tools/check/feature_build.py 生成：依赖守卫，由 engine_top 例化一次。
// 依赖不满足时例化一个不存在的模块，仿真、hierarchy -check 与厂商综合都会硬失败，
// 报错里带违规边的名字。内容只随 feature_config.py 的依赖表变化，与构型无关。
`timescale 1ns/1ps
`default_nettype none
`include "engine_feature.vh"

module gfx_config_guard();

`ifdef GFX_HAS_PB
  `ifndef GFX_HAS_BAND_HW
    GFX_CONFIG_ERROR_PB_REQUIRES_BAND_HW u_config_error_pb_requires_band_hw();
  `endif
`endif
`ifdef GFX_HAS_PB
  `ifndef GFX_HAS_INST
    GFX_CONFIG_ERROR_PB_REQUIRES_INST u_config_error_pb_requires_inst();
  `endif
`endif
`ifdef GFX_HAS_STRIP
  `ifndef GFX_HAS_BAND_HW
    GFX_CONFIG_ERROR_STRIP_REQUIRES_BAND_HW u_config_error_strip_requires_band_hw();
  `endif
`endif
`ifdef GFX_HAS_RAST
  `ifndef GFX_HAS_STRIP
    GFX_CONFIG_ERROR_RAST_REQUIRES_STRIP u_config_error_rast_requires_strip();
  `endif
`endif
`ifdef GFX_HAS_RAST_TEX
  `ifndef GFX_HAS_RAST
    GFX_CONFIG_ERROR_RAST_TEX_REQUIRES_RAST u_config_error_rast_tex_requires_rast();
  `endif
`endif
`ifdef GFX_HAS_RAST_TEX
  `ifndef GFX_HAS_TEX
    GFX_CONFIG_ERROR_RAST_TEX_REQUIRES_TEX u_config_error_rast_tex_requires_tex();
  `endif
`endif
`ifdef GFX_DELIVERY
  `ifndef GFX_ALPHA
    GFX_CONFIG_ERROR_DELIVERY_REQUIRES_ALPHA u_config_error_delivery_requires_alpha();
  `endif
`endif
`ifdef GFX_DELIVERY
  `ifndef GFX_KEY
    GFX_CONFIG_ERROR_DELIVERY_REQUIRES_KEY u_config_error_delivery_requires_key();
  `endif
`endif
`ifdef GFX_HAS_PB
  `ifdef GFX_HAS_RAST
    GFX_CONFIG_ERROR_PB_EXCLUDES_RAST u_config_error_pb_rast();
  `endif
`endif

endmodule

`default_nettype wire
