// engine_top：渲染引擎对外的唯一入口（引擎 P1 B5 的顶层）
//
// B5 把六层接成一台引擎：寄存器层（APB 第 0 页）→ 命令层 → 几何层 → 计算阵列 →
// 搬运层（AXI 主口），外加计算核心。B6 把本模块接进 SoC 顶层（APB 第 0 页的
// psel 在顶层按第 0 页及第 4～7 页门控；AXI 口接 Axi_Mux 的 S1；
// 中断 B 接 SoC 的 userInterruptB）。
// G1（引擎 P3，2026-10-07）：顶层收完整的 16 位 Slave0 页内偏移并分选——
// paddr[15:12]==0 落寄存器表（engine_regs 继续只收页内 12 位地址），
// paddr[15:14]==01 落 TEX 装载窗口（engine_tex，第 4~7 页，16 KiB）。
// TEX 的描述符出口接搬运层，读流与状态并回搬运层；PERF 的读事件按显式
// rd_tex 过滤，只计真实 DDR 描述符与命令取数。
//
// 复位（D17）：控制层（寄存器、命令、几何、计算阵列）挂 rst_ctrl（soc_system_reset），
// 搬运层挂 rst_ddr_n 的取反（与 Axi_Mux 同源，只在上电与 PLL 失锁时复位）。
// 控制层复位或软复位都要排空搬运层：本模块把「rst_ctrl 电平 OR 寄存器层的软复位
// 请求」作为 drain_req 送给搬运层——复位持续期间 drain_req 就是复位电平本身
//（mover 对此有「一次请求只完成一次」的约定）；软复位请求在排空完成（drain_done）
// 之后由寄存器层撤下。
//
// PERF 事件（I8，engine_regs 计数，判据与对账在 B6）：
//   CMD      命令层每完成一条通过检查的命令（不含 END 与出错命令）
//   PIXEL    几何层每行边带握手时按 sb_npx 计（五者求交之后的像素，源越界不计）
//   KEY_SKIP 计算阵列第 7 拍的「色键命中且在本行窗口内」槽位（本批新引出的观测口）
//   RD_*     几何层读描述符与命令取数描述符的握手（PERF_RD_* 计实际总线事务）
//   WR_*     几何层写描述符的握手
//   WAIT_DDR AXI 五个通道任一 valid 未握手满一拍计一
//   BUSY     引擎忙（受理起到收尾止）
//
// 设计依据：B5 条目、TechReports engine 第 05 章；层间接口见总览 2.4 节。
`timescale 1ns/1ps
`default_nettype none
`include "engine_regs_offsets.vh"

module engine_top #(
    parameter integer INJECT = 0,     // 仿真注错口，透传给命令层；综合时常量
    parameter integer TEX_READ_LATENCY = 1,
    parameter integer MOVER_INJECT = 0
)(
    input  wire        clk,           // core_clk，100 MHz，与 SoC、DDR3 控制器同域
    input  wire        rst_ctrl,      // 控制层复位（soc_system_reset），同步、高有效
    input  wire        rst_ddr_n,     // 搬运层复位源（顶层接 ddr_rstn，低有效）

    // ---- APB 从口（第 0 页寄存器 + 第 4~7 页 TEX；psel 已在顶层按两处页门控）----
    input  wire [15:0] paddr,
    input  wire        psel,
    input  wire        penable,
    input  wire        pwrite,
    input  wire [31:0] pwdata,
    output wire [31:0] prdata,
    output wire        pready,
    output wire        pslverr,

    // ---- AXI 主口（接 Axi_Mux 的 S1）----
    output wire [31:0] m_axi_araddr,
    output wire [7:0]  m_axi_arlen,
    output wire [2:0]  m_axi_arsize,
    output wire [1:0]  m_axi_arburst,
    output wire        m_axi_arvalid,
    input  wire        m_axi_arready,
    input  wire [127:0] m_axi_rdata,
    input  wire [1:0]  m_axi_rresp,
    input  wire        m_axi_rlast,
    input  wire        m_axi_rvalid,
    output wire        m_axi_rready,

    output wire [31:0] m_axi_awaddr,
    output wire [7:0]  m_axi_awlen,
    output wire [2:0]  m_axi_awsize,
    output wire [1:0]  m_axi_awburst,
    output wire        m_axi_awvalid,
    input  wire        m_axi_awready,
    output wire [127:0] m_axi_wdata,
    output wire [15:0] m_axi_wstrb,
    output wire        m_axi_wlast,
    output wire        m_axi_wvalid,
    input  wire        m_axi_wready,
    input  wire [1:0]  m_axi_bresp,
    input  wire        m_axi_bvalid,
    output wire        m_axi_bready,

    // ---- 中断 B（电平 = IRQ_STAT & IRQ_EN；命令表完成与错误）----
    output wire        irq_b,

    // ---- 仿真观测（综合悬空）----
    output wire        dbg_fe_cidx_err   // 命令层取数流上下文一致性监视
);
    // =====================================================================
    // 内部连线
    // =====================================================================
    wire        mover_rst = ~rst_ddr_n;
    wire        soft_pending;
    wire        drain_done;
    wire        kick;
    wire [31:0] list_base;
    wire [31:0] list_count;
    wire        cmd_busy;
    wire        err_set, done_set, commit, suppress_writes;
    wire [3:0]  err_code;
    wire [31:0] err_index, err_tag;

    // 搬运层（I7）
    wire [31:0] fd_addr;
    wire [11:0] fd_bytes;
    wire [3:0]  fd_cidx;
    wire        fd_valid, fd_ready;
    wire [127:0] fs_tdata;
    wire        fs_tlast, fs_tvalid, fs_tready;
    wire [3:0]  fs_cidx;
    wire        rd_stat_valid, rd_stat_err;
    wire        rd_stat_fetch;
    wire [3:0]  rd_stat_cidx;
    wire        wr_stat_valid, wr_stat_err, wr_stat_last_row;
    wire [3:0]  wr_stat_cidx;
    wire        tmo_evt;

    // TEX 装载窗口（G1，第 4~7 页）与片上读流
    wire [31:0] regs_prdata;
    wire        regs_pready, regs_pslverr;
    wire        tex_psel;
    wire [31:0] tex_prdata;
    wire        tex_pready, tex_pslverr;
    wire [13:0] tx_addr;
    wire [11:0] tx_bytes;
    wire [1:0]  tx_buf;
    wire [3:0]  tx_cidx;
    wire        tx_valid, tx_ready;
    wire [127:0] tx_rs_tdata;
    wire        tx_rs_tvalid, tx_rs_tlast;
    wire [1:0]  tx_rs_buf;
    wire        tx_stat_valid;
    wire        tx_stat_ready;
    wire [3:0]  tx_stat_cidx;
    wire [1:0]  tx_stat_buf;
    wire        tx_busy;

    // 几何层（I1/I2/I5）
    wire        geom_start;
    wire [7:0]  i1_op;
    wire [3:0]  i1_eq;
    wire        i1_key_en;
    wire [31:0] i1_key, i1_w4;
    wire [7:0]  i1_c;
    wire [3:0]  i1_sfmt;
    wire [31:0] i1_src_base;
    wire        i1_src_tex;
    wire [15:0] i1_src_w, i1_src_h;
    wire [4:0]  i1_src_slog2;
    wire [31:0] i1_dst_base;
    wire [15:0] i1_dst_w, i1_dst_h;
    wire [4:0]  i1_dst_slog2;
    wire signed [15:0] i1_src_x, i1_src_y, i1_dst_x, i1_dst_y;
    wire [15:0] i1_w, i1_h;
    wire signed [15:0] i1_clip_x0, i1_clip_y0, i1_clip_x1, i1_clip_y1;
    wire [3:0]  i1_cidx;
    wire        geom_busy, geom_cmd_done;
    wire [3:0]  geom_done_cidx;
    wire [31:0] g_rd_addr, g_wr_addr;
    wire [11:0] g_rd_beats, g_wr_beats;
    wire [11:0] g_rd_bytes = g_rd_beats << 4;
    wire [11:0] g_wr_bytes = g_wr_beats << 4;
    wire [1:0]  g_rd_buf;
    wire        g_rd_tex;
    wire [3:0]  g_rd_cidx, g_wr_cidx;
    wire        g_rd_valid, g_rd_ready, g_wr_valid, g_wr_ready, g_wr_last_row;
    wire [15:0] sb_row, sb_py;
    wire [10:0] sb_npx;
    wire [7:0]  sb_beats, sb_op, sb_c;
    wire [1:0]  sb_lead_dst, sb_lead_src;
    wire [3:0]  sb_eq, sb_sfmt;
    wire        sb_key_en, sb_read_dst, sb_valid, sb_ready;
    wire [31:0] sb_key, sb_w4;
    wire [1:0]  row_free;

    // 计算阵列（I3/I4 + I8）
    wire [127:0] rs_tdata, ws_tdata;
    // D19：读错误行及失败命令的后续写流仍可走完描述符，但不得改 DDR。
    // engine_mover 按每个像素 X 字节的最高位清掉对应 WSTRB。
    wire [127:0] ws_data_safe = suppress_writes
        ? (ws_tdata | 128'h80000000_80000000_80000000_80000000)
        : ws_tdata;
    wire [15:0]  rs_tkeep, ws_tkeep;
    wire         rs_tlast, rs_tvalid, rs_tready;
    wire [1:0]   rs_buf;
    wire         ws_tvalid, ws_tready;
    wire [3:0]   key_lanes;
    wire         key_valid;

    // 表面表读口（regs 组合读，cmd 检查用）
    wire [3:0]  surf_ra_id, surf_rb_id;
    wire [31:0] surf_ra_w0, surf_ra_w1, surf_ra_w2;
    wire [31:0] surf_rb_w0, surf_rb_w1, surf_rb_w2;

    // PERF 汇聚（I8）。G1：读事件只计真实 DDR 描述符与命令取数，TEX 读不计入。
    wire [3:0] perf_key_lanes_w;
    wire       perf_key_valid_w;
    wire perf_rdb = (g_rd_valid && g_rd_ready && !g_rd_tex) || (fd_valid && fd_ready);
    wire [11:0] perf_rdb_beats = (fd_valid && fd_ready) ? {4'b0, fd_bytes[11:4]}
                                                        : g_rd_beats;
    wire perf_wrb = (g_wr_valid && g_wr_ready);
    wire perf_wait = (m_axi_arvalid && !m_axi_arready)
                  || (m_axi_rvalid  && !m_axi_rready)
                  || (m_axi_awvalid && !m_axi_awready)
                  || (m_axi_wvalid  && !m_axi_wready)
                  || (m_axi_bvalid  && !m_axi_bready);

    // =====================================================================
    // APB 分选（G1）：第 0 页寄存器、第 4~7 页 TEX；psel 已含两处页门控
    // =====================================================================
    wire apb_hit_regs = (paddr[15:12] == 4'h0);
    wire apb_hit_tex  = (paddr[15:14] == 2'b01);
    assign prdata  = apb_hit_regs ? regs_prdata : tex_prdata;
    assign pready  = apb_hit_regs ? regs_pready : tex_pready;
    assign pslverr = apb_hit_regs ? regs_pslverr : tex_pslverr;

    // =====================================================================
    // 寄存器层
    // =====================================================================
    engine_regs u_regs (
        .clk(clk), .rst(rst_ctrl),
        .paddr(paddr[11:0]), .psel(psel && apb_hit_regs), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(regs_prdata), .pready(regs_pready), .pslverr(regs_pslverr),
        .cmd_busy(cmd_busy),
        .err_set(err_set), .err_code(err_code), .err_index(err_index), .err_tag(err_tag),
        .done_set(done_set),
        .kick(kick), .list_base(list_base), .list_count(list_count),
        .soft_rst_done(drain_done), .soft_rst_pending(soft_pending),
        .irq_b(irq_b),
        .perf_cmd(commit),
        .perf_pixel_en(sb_valid && sb_ready), .perf_pixel_npx(sb_npx),
        .perf_key_en(perf_key_valid_w), .perf_key_lanes(perf_key_lanes_w),
        .perf_rdb(perf_rdb), .perf_rdb_beats(perf_rdb_beats),
        .perf_wrb(perf_wrb), .perf_wrb_beats(g_wr_beats),
        .perf_wait(perf_wait), .perf_busy(cmd_busy),
        .surf_ra_id(surf_ra_id), .surf_rb_id(surf_rb_id),
        .surf_ra_w0(surf_ra_w0), .surf_ra_w1(surf_ra_w1), .surf_ra_w2(surf_ra_w2),
        .surf_rb_w0(surf_rb_w0), .surf_rb_w1(surf_rb_w1), .surf_rb_w2(surf_rb_w2)
    );

    // =====================================================================
    // 命令层
    // =====================================================================
    engine_cmd #(.INJECT(INJECT)) u_cmd (
        .clk(clk), .rst(rst_ctrl),
        .kick(kick), .list_base(list_base), .list_count(list_count),
        .soft_rst(soft_pending),
        .busy(cmd_busy),
        .err_set(err_set), .err_code(err_code),
        .err_index(err_index), .err_tag(err_tag),
        .done_set(done_set), .commit(commit),
        .suppress_writes(suppress_writes),
        .surf_ra_id(surf_ra_id), .surf_rb_id(surf_rb_id),
        .surf_ra_w0(surf_ra_w0), .surf_ra_w1(surf_ra_w1),
        .surf_ra_w2(surf_ra_w2),
        .surf_rb_w0(surf_rb_w0), .surf_rb_w1(surf_rb_w1),
        .surf_rb_w2(surf_rb_w2),
        .fd_addr(fd_addr), .fd_bytes(fd_bytes), .fd_cidx(fd_cidx),
        .fd_valid(fd_valid), .fd_ready(fd_ready),
        .fs_tdata(fs_tdata), .fs_tlast(fs_tlast), .fs_tvalid(fs_tvalid),
        .fs_tready(fs_tready), .fs_cidx(fs_cidx),
        .rd_stat_valid(rd_stat_valid), .rd_stat_fetch(rd_stat_fetch),
        .rd_stat_err(rd_stat_err),
        .rd_stat_cidx(rd_stat_cidx),
        .wr_stat_valid(wr_stat_valid), .wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx), .wr_stat_last_row(wr_stat_last_row),
        .tmo_evt(tmo_evt),
        .start(geom_start),
        .i1_op(i1_op), .i1_eq(i1_eq), .i1_key_en(i1_key_en),
        .i1_key(i1_key), .i1_w4(i1_w4), .i1_c(i1_c), .i1_sfmt(i1_sfmt),
        .i1_src_base(i1_src_base), .i1_src_tex(i1_src_tex),
        .i1_src_w(i1_src_w), .i1_src_h(i1_src_h),
        .i1_src_slog2(i1_src_slog2),
        .i1_dst_base(i1_dst_base), .i1_dst_w(i1_dst_w), .i1_dst_h(i1_dst_h),
        .i1_dst_slog2(i1_dst_slog2),
        .i1_src_x(i1_src_x), .i1_src_y(i1_src_y),
        .i1_dst_x(i1_dst_x), .i1_dst_y(i1_dst_y),
        .i1_w(i1_w), .i1_h(i1_h),
        .i1_clip_x0(i1_clip_x0), .i1_clip_y0(i1_clip_y0),
        .i1_clip_x1(i1_clip_x1), .i1_clip_y1(i1_clip_y1),
        .i1_cidx(i1_cidx),
        .geom_busy(geom_busy), .geom_cmd_done(geom_cmd_done),
        .geom_done_cidx(geom_done_cidx),
        .fe_cidx_err(dbg_fe_cidx_err)
    );

    // =====================================================================
    // 几何层
    // =====================================================================
    engine_geom #(.MAX_BEATS(192), .INJECT(0)) u_geom (
        .clk(clk), .rst(rst_ctrl),
        .start(geom_start),
        .op(i1_op), .eq(i1_eq), .key_en(i1_key_en),
        .key_w5(i1_key), .const_w4(i1_w4), .c(i1_c), .sfmt(i1_sfmt),
        .src_base(i1_src_base), .src_w(i1_src_w), .src_h(i1_src_h),
        .src_slog2(i1_src_slog2), .src_tex(i1_src_tex),
        .dst_base(i1_dst_base), .dst_w(i1_dst_w), .dst_h(i1_dst_h),
        .dst_slog2(i1_dst_slog2),
        .src_x(i1_src_x), .src_y(i1_src_y), .dst_x(i1_dst_x), .dst_y(i1_dst_y),
        .w(i1_w), .h(i1_h),
        .clip_x0(i1_clip_x0), .clip_y0(i1_clip_y0),
        .clip_x1(i1_clip_x1), .clip_y1(i1_clip_y1),
        .cidx(i1_cidx),
        .row_free(row_free),
        .busy(geom_busy), .cmd_done(geom_cmd_done), .done_cidx(geom_done_cidx),
        .rd_addr(g_rd_addr), .rd_beats(g_rd_beats), .rd_buf(g_rd_buf),
        .rd_tex(g_rd_tex),
        .rd_cidx(g_rd_cidx), .rd_valid(g_rd_valid), .rd_ready(g_rd_ready),
        .wr_addr(g_wr_addr), .wr_beats(g_wr_beats), .wr_cidx(g_wr_cidx),
        .wr_last_row(g_wr_last_row), .wr_valid(g_wr_valid), .wr_ready(g_wr_ready),
        .sb_row(sb_row), .sb_py(sb_py), .sb_npx(sb_npx), .sb_beats(sb_beats),
        .sb_lead_dst(sb_lead_dst), .sb_lead_src(sb_lead_src),
        .sb_op(sb_op), .sb_eq(sb_eq), .sb_key_en(sb_key_en), .sb_key(sb_key),
        .sb_w4(sb_w4), .sb_c(sb_c), .sb_sfmt(sb_sfmt), .sb_read_dst(sb_read_dst),
        .sb_valid(sb_valid), .sb_ready(sb_ready)
    );

    // =====================================================================
    // 计算阵列与计算核心
    // =====================================================================
    engine_pix #(.INJECT(0)) u_pix (
        .clk(clk), .rst(rst_ctrl),
        .sb_row(sb_row), .sb_npx(sb_npx), .sb_beats(sb_beats),
        .sb_lead_dst(sb_lead_dst), .sb_lead_src(sb_lead_src),
        .sb_op(sb_op), .sb_eq(sb_eq), .sb_key_en(sb_key_en), .sb_key(sb_key),
        .sb_w4(sb_w4), .sb_c(sb_c), .sb_sfmt(sb_sfmt), .sb_read_dst(sb_read_dst),
        .sb_valid(sb_valid), .sb_ready(sb_ready),
        .row_free(row_free),
        .rs_tdata(rs_tdata), .rs_tkeep(rs_tkeep), .rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid), .rs_tready(rs_tready), .rs_buf(rs_buf),
        .ws_tdata(ws_tdata), .ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid), .ws_tready(ws_tready),
        .perf_key_lanes(perf_key_lanes_w), .perf_key_valid(perf_key_valid_w)
    );

    // =====================================================================
    // 搬运层（AXI 事务层）
    // =====================================================================
    engine_mover #(.AXI_MAX_BURST_LEN(192), .TMO_WIDTH(13), .INJECT(MOVER_INJECT)) u_mover (
        .clk(clk), .rst(mover_rst),
        .rd_addr(g_rd_addr), .rd_bytes(g_rd_bytes), .rd_buf(g_rd_buf),
        .rd_cidx(g_rd_cidx), .rd_tex(g_rd_tex),
        .rd_valid(g_rd_valid), .rd_ready(g_rd_ready),
        .tx_addr(tx_addr), .tx_bytes(tx_bytes), .tx_buf(tx_buf), .tx_cidx(tx_cidx),
        .tx_valid(tx_valid), .tx_ready(tx_ready),
        .tx_rs_tdata(tx_rs_tdata), .tx_rs_tvalid(tx_rs_tvalid),
        .tx_rs_tlast(tx_rs_tlast), .tx_rs_buf(tx_rs_buf),
        .tx_stat_valid(tx_stat_valid), .tx_stat_ready(tx_stat_ready), .tx_stat_cidx(tx_stat_cidx),
        .tx_stat_buf(tx_stat_buf), .tx_busy(tx_busy),
        .fd_addr(fd_addr), .fd_bytes(fd_bytes), .fd_cidx(fd_cidx),
        .fd_valid(fd_valid), .fd_ready(fd_ready),
        .rs_tdata(rs_tdata), .rs_tkeep(rs_tkeep), .rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid), .rs_tready(rs_tready), .rs_buf(rs_buf),
        .fs_tdata(fs_tdata), .fs_tkeep(), .fs_tlast(fs_tlast), .fs_tvalid(fs_tvalid),
        .fs_tready(fs_tready), .fs_cidx(fs_cidx),
        .wr_addr(g_wr_addr), .wr_bytes(g_wr_bytes), .wr_cidx(g_wr_cidx),
        .wr_last_row(g_wr_last_row), .wr_valid(g_wr_valid), .wr_ready(g_wr_ready),
        .ws_tdata(ws_data_safe), .ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid), .ws_tready(ws_tready),
        .rd_stat_valid(rd_stat_valid), .rd_stat_err(rd_stat_err),
        .rd_stat_fetch(rd_stat_fetch),
        .rd_stat_cidx(rd_stat_cidx), .rd_stat_buf(),
        .wr_stat_valid(wr_stat_valid), .wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx), .wr_stat_last_row(wr_stat_last_row),
        .drain_req(rst_ctrl || soft_pending),
        .drain_busy(), .drain_done(drain_done),
        .tmo_limit(EC_TMO_RESET),
        .err_clr(kick),
        .tmo_evt(tmo_evt), .tmo_dir(),
        .m_axi_araddr(m_axi_araddr), .m_axi_arlen(m_axi_arlen),
        .m_axi_arsize(m_axi_arsize), .m_axi_arburst(m_axi_arburst),
        .m_axi_arvalid(m_axi_arvalid), .m_axi_arready(m_axi_arready),
        .m_axi_rdata(m_axi_rdata), .m_axi_rresp(m_axi_rresp),
        .m_axi_rlast(m_axi_rlast), .m_axi_rvalid(m_axi_rvalid), .m_axi_rready(m_axi_rready),
        .m_axi_awaddr(m_axi_awaddr), .m_axi_awlen(m_axi_awlen),
        .m_axi_awsize(m_axi_awsize), .m_axi_awburst(m_axi_awburst),
        .m_axi_awvalid(m_axi_awvalid), .m_axi_awready(m_axi_awready),
        .m_axi_wdata(m_axi_wdata), .m_axi_wstrb(m_axi_wstrb), .m_axi_wlast(m_axi_wlast),
        .m_axi_wvalid(m_axi_wvalid), .m_axi_wready(m_axi_wready),
        .m_axi_bresp(m_axi_bresp), .m_axi_bvalid(m_axi_bvalid), .m_axi_bready(m_axi_bready)
    );

    // =====================================================================
    // TEX 装载窗口（G1，D57）：第 4~7 页的 16 KiB scratchpad
    // =====================================================================
    assign tex_psel = psel && apb_hit_tex;
    engine_tex #(.INJECT(0), .READ_LATENCY(TEX_READ_LATENCY)) u_tex (
        .clk(clk), .rst(rst_ctrl),
        .paddr(paddr[13:0]), .psel(tex_psel), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(tex_prdata), .pready(tex_pready), .pslverr(tex_pslverr),
        .req_addr(tx_addr), .req_bytes(tx_bytes), .req_buf(tx_buf), .req_cidx(tx_cidx),
        .req_valid(tx_valid), .req_ready(tx_ready),
        .rs_tdata(tx_rs_tdata), .rs_tvalid(tx_rs_tvalid),
        .rs_tlast(tx_rs_tlast), .rs_buf(tx_rs_buf),
        .stat_valid(tx_stat_valid), .stat_ready(tx_stat_ready), .stat_cidx(tx_stat_cidx),
        .stat_buf(tx_stat_buf), .busy(tx_busy)
    );

endmodule
`default_nettype wire
