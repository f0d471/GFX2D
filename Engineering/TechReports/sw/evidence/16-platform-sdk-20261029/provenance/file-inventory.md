# SW16 文件变更清单

本清单以本轮开工的未提交工作区指纹为基线，不使用 git status 或 HEAD 差异冒领其他会话的修改。
上一轮原子回写559个文件，移除25个旧路径；后续补充板测检查器与本文档。证据文件单独统计。
移动目标可能同时有接口重构；旧路径移除不代表对应功能删除。

## 一、移动/重命名（25个旧路径）

| 原路径 | 当前路径 |
|---|---|
| `Main/sw/apps/common/stg_runtime.c` | `Main/sw/ports/myriad/stg_runtime.c` |
| `Main/sw/apps/common/stg_runtime.h` | `Main/sw/ports/myriad/stg_runtime.h` |
| `Main/sw/apps/console/src/FreeRTOSConfig.h` | `Main/sw/firmware/console/FreeRTOSConfig.h` |
| `Main/sw/apps/console/src/app_game.c` | `Main/sw/ports/myriad/app_game.c` |
| `Main/sw/apps/console/src/app_r4.c` | `Main/sw/ports/myriad/app_r4.c` |
| `Main/sw/apps/console/src/appfw.c` | `Main/sw/runtime/appfw.c` |
| `Main/sw/apps/console/src/appfw.h` | `Main/sw/runtime/include/appfw.h` |
| `Main/sw/apps/console/src/console_platform.h` | `Main/sw/system/include/app_port.h` |
| `Main/sw/apps/console/src/main.c` | `Main/sw/firmware/console/main_rtos.c` |
| `Main/sw/apps/console/src/main_host.c` | `Main/sw/firmware/console/main_host.c` |
| `Main/sw/apps/game/gfx2dGame/src/frontend_game.c` | `Main/sw/ports/myriad/frontend_game.c` |
| `Main/sw/apps/game/gfx2dGame/src/frontend_game.h` | `Main/sw/ports/myriad/frontend_game.h` |
| `Main/sw/apps/game/gfx2dGame/src/game/sw6_input.c` | `Main/sw/ports/myriad/sw6_input.c` |
| `Main/sw/apps/game/gfx2dGame/src/game/sw6_report.c` | `Main/sw/ports/myriad/sw6_report.c` |
| `Main/sw/apps/game/gfx2dGame/src/gfx2d_stats.c` | `Main/sw/ports/myriad/gfx2d_stats.c` |
| `Main/sw/apps/game/gfx2dGame/src/gfx2d_stats.h` | `Main/sw/ports/myriad/gfx2d_stats.h` |
| `Main/sw/apps/game/gfx2dGame/src/main.c` | `Main/sw/firmware/legacy/main.c` |
| `Main/sw/apps/game/gfx2dGame/src/r4_render.c` | `Main/sw/ports/myriad/r4_runtime.c` |
| `Main/sw/apps/game/gfx2dGame/src/renderer.c` | `Main/sw/services/gfx/renderer.c` |
| `Main/sw/apps/game/gfx2dGame/src/renderer.h` | `Main/sw/services/gfx/renderer.h` |
| `Main/sw/apps/game/gfx2dGame/src/stg_main.c` | `Main/sw/ports/myriad/stg_main.c` |
| `Main/sw/apps/game/gfx2dGame/src/stg_main.h` | `Main/sw/ports/myriad/stg_main.h` |
| `Main/sw/apps/game/gfx2dGame/src/stg_modes.c` | `Main/sw/ports/myriad/stg_modes.c` |
| `Main/sw/apps/game/gfx2dGame/src/stg_modes.h` | `Main/sw/ports/myriad/stg_modes.h` |
| `Main/sw/bsp/board_input.c` | `Main/sw/services/input/board_input.c` |

## 真正新增的实现/构建/检查

- `Main/sw/api/input.h`
- `Main/sw/api/platform.h`
- `Main/sw/apps/console/test/test_registry.c`
- `Main/sw/apps/examples/bounce/main.c`
- `Main/sw/apps/examples/bounce/makefile`
- `Main/sw/apps/examples/pixels/main.c`
- `Main/sw/apps/examples/pixels/makefile`
- `Main/sw/bsp/include/board_input_compat.h`
- `Main/sw/lib/core/firmware_image.c`
- `Main/sw/lib/core/firmware_image.h`
- `Main/sw/lib/core/health.c`
- `Main/sw/lib/core/health.h`
- `Main/sw/lib/core/record.c`
- `Main/sw/lib/core/record.h`
- `Main/sw/ports/host/app_port.c`
- `Main/sw/ports/myriad/console_content.c`
- `Main/sw/ports/myriad/console_content.h`
- `Main/sw/ports/myriad/console_ids.h`
- `Main/sw/ports/myriad/console_views.c`
- `Main/sw/ports/myriad/console_views.h`
- `Main/sw/ports/ti60/app_port.c`
- `Main/sw/services/gfx/raster_setup_port.h`
- `Main/sw/services/gfx/render_policy.h`
- `Main/sw/services/gfx/resource_pool.c`
- `Main/sw/services/gfx/resource_pool.h`
- `Main/sw/services/platform/platform.c`
- `Main/sw/services/platform/store_host.c`
- `Main/sw/services/platform/store_target.c`
- `Main/sw/services/test/fakes/FreeRTOS.h`
- `Main/sw/services/test/fakes/bsp.h`
- `Main/sw/services/test/fakes/riscv.h`
- `Main/sw/services/test/fakes/semphr.h`
- `Main/sw/services/test/fakes/task.h`
- `Main/sw/services/test/test_driver_wait.c`
- `Main/sw/services/test/test_irq_bridge.c`
- `Main/sw/services/test/test_platform.c`
- `Main/sw/services/test/test_records.c`
- `Main/sw/services/test/test_scheduler_port.c`
- `Main/sw/services/test/test_store.c`
- `Main/tools/check/check_platform_sdk.py`
- `Main/tools/check/check_sw16_board.py`
- `Main/sw/apps/probe/sdkBoardTest/makefile`
- `Main/sw/apps/probe/sdkBoardTest/src/main.c`

## 原位置修改的实现/构建/检查

- `Main/sim/engine/frontend/test_pipeline.c`
- `Main/sw/apps/console/makefile`
- `Main/sw/apps/console/src/app_about.c`
- `Main/sw/apps/console/src/app_demo.c`
- `Main/sw/apps/console/src/apps_registry.c`
- `Main/sw/apps/console/src/shell.c`
- `Main/sw/apps/console/test/test_appfw.c`
- `Main/sw/apps/console/test/test_input.c`
- `Main/sw/apps/game/gfx2dGame/makefile`
- `Main/sw/apps/game/gfx2dGame/src/game/sw6_cfg.h`
- `Main/sw/apps/game/gfx2dGame/src/game/sw6_hud.c`
- `Main/sw/apps/game/gfx2dGame/src/game/sw6_scene_a.c`
- `Main/sw/apps/game/gfx2dGame/src/game/sw6_scene_b.c`
- `Main/sw/apps/game/gfx2dGame/src/game/sw6_scene_b.h`
- `Main/sw/apps/game/gfx2dGame/src/r4_cup.c`
- `Main/sw/apps/game/gfx2dGame/src/r4_geometry.h`
- `Main/sw/apps/game/gfx2dGame/src/r4_scene.c`
- `Main/sw/bsp/board_wait.c`
- `Main/sw/bsp/host/board.c`
- `Main/sw/bsp/include/board.h`
- `Main/sw/bsp/ti60/board.c`
- `Main/sw/bsp/ti60/gfx_engine.c`
- `Main/sw/mk/host.mk`
- `Main/sw/mk/target.mk`
- `Main/sw/rtos/rtos_irq.c`
- `Main/sw/rtos/rtos_irq.h`
- `Main/tools/board/build_pc.py`
- `Main/tools/check/check_console.py`
- `Main/tools/check/check_f4_instances.py`
- `Main/tools/check/check_f5_bin.py`
- `Main/tools/check/check_frontend.py`
- `Main/tools/check/check_g4_integration.py`
- `Main/tools/check/check_pc_wide.py`
- `Main/tools/check/check_r4.py`
- `Main/tools/check/check_sw8_stats.sh`
- `Main/tools/check/r4_checks.py`
- `Main/tools/check/research_strip_frontend.py`
- `Main/tools/check/validate.py`

本轮板测另修改 `Main/tools/check/validate.py`：登记 sw16-board，并禁止复用物理板状态。

## 文档与证据

- `Engineering/TechReports/sw/16-平台接口与应用解耦.md`
- `Engineering/TechReports/sw/README.md`
- `Engineering/Todo/README.md`
- `Engineering/Todo/sw/平台产品化板端后续.md`
- `Engineering/Todo/sw/平台接口与应用解耦.md`
- `Main/doc/README.md`
- `Main/doc/平台软件接口.md`
- `Main/doc/应用与输入契约.md`
- `Main/doc/赛题二总体设计.md`
- `Main/sw/README.md`
- `Main/sw/api/README.md`
- `Main/sw/firmware/README.md`
- `Main/sw/ports/README.md`
- `Main/sw/runtime/README.md`
- `Main/sw/services/README.md`
- `Main/tools/check/README.md`

证据目录的逐文件原始身份见 `file-inventory.json` 与各运行的 `archive-files.json`，
不把数百份日志/清单重复计作实现文件。当前文件若在回写后又被其他会话修改，JSON中
`later_modified`明确保留该事实；本轮上板验证绑定的是当前源码和本轮新构建BIN。

不属于本轮：PPA的RTL/位流提交、Deliverables报告/照片、开工前已删除的chord文件等。
