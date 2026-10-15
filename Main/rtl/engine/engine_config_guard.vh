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
`ifdef GFX_DELIVERY
  `ifndef GFX_HAS_ALPHA
    GFX_CONFIG_ERROR_DELIVERY_REQUIRES_ALPHA u_config_error_delivery_requires_alpha();
  `endif
`endif
`ifdef GFX_DELIVERY
  `ifndef GFX_HAS_KEY
    GFX_CONFIG_ERROR_DELIVERY_REQUIRES_KEY u_config_error_delivery_requires_key();
  `endif
`endif

`endif
