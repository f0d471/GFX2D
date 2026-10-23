// 由 tools/check/feature_build.py 生成：本构型的依赖守卫，由 engine_top 例化。
// 守卫不满足时引用未定义模块，hierarchy -check 与厂商综合会硬失败。
`ifndef GFX_ENGINE_CONFIG_GUARD_VH
`define GFX_ENGINE_CONFIG_GUARD_VH

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

`endif
