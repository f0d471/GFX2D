`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_top.v
//
// DESCRIPTION: 渲染引擎对外唯一入口：寄存器层 → 命令层 → 几何层 → 计算阵列 →
//   搬运层，外加计算核心、TEX 装载窗口与 PB 前端。APB 第 0 页落寄存器表，
//   第 4~7 页落 TEX；AXI 口接 Axi_Mux 的 S1，中断 B 接 SoC userInterruptB。
//
// NOTE:
//   1. 控制层挂 rst_ctrl，搬运层挂 ~rst_ddr_n；控制层复位或软复位都经 drain_req 排空搬运层
//   2. bin_mode=1 时 DOORBELL 走 PB 前端（engine_pb），否则走原命令层；pb_active 按门铃锁存
//   3. PERF 读事件按 rd_tex 过滤，只计真实 DDR 描述符与命令取数，TEX 读不计入
//   4. 子模块注错由 `ifdef INJ_xxx 选择 compile 输入，本层不透传参数
//==============================================================================================//
`default_nettype none
`include "engine_regs_offsets.vh"

module engine_top #(
    parameter integer STRIP_H = 16,
    parameter integer TEX_READ_LATENCY = 1
)(
    input  wire        clk,           // core_clk，100 MHz，与 SoC、DDR3 控制器同域
    input  wire        rst_ctrl,      // 控制层复位（soc_system_reset），同步、高有效
    input  wire        rst_ddr_n,     // 搬运层复位源（顶层接 ddr_rstn，低有效）

    // APB 从口（第 0 页寄存器 + 第 4~7 页 TEX；psel 已在顶层按两处页门控）
    input  wire [15:0] paddr,
    input  wire        psel,
    input  wire        penable,
    input  wire        pwrite,
    input  wire [31:0] pwdata,
    output wire [31:0] prdata,
    output wire        pready,
    output wire        pslverr,

    // AXI 主口（接 Axi_Mux 的 S1）
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

    // 中断 B（电平 = IRQ_STAT & IRQ_EN；命令表完成与错误）
    output wire        irq_b,

    // 仿真观测（综合悬空）
    output wire        dbg_fe_cidx_err   // 命令层取数流上下文一致性监视
);

    // 内部连线
    wire        mover_rst = ~rst_ddr_n;
    wire        soft_pending;
    wire        drain_done;
    wire        kick;
    wire [31:0] list_base;
    wire [31:0] list_count;
    wire band_perf_rd, band_perf_wr;
    wire band_mode, band_enable, band_start, band_flush, band_abort;
    wire [31:0] lstrips, strip_log2, band_surfs, band_bg_base, band_dst_base;
    wire [4:0]  band_bg_slog2, band_dst_slog2;
    wire [15:0] band_y0, band_frame_height;
    wire band_start_ready, band_flush_ready, band_loaded, band_flushed, band_busy, band_error;
    wire [3:0]  band_error_code;
    wire        cmd_busy;
    wire        err_set, done_set, commit, suppress_writes;
    wire [3:0]  err_code;
    wire [31:0] err_index, err_tag, err_instance;
    wire        perf_instance, fd_instance, fs_instance, rd_stat_instance;

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
    wire        tmo_evt, tmo_dir;

    // PB 前端与旧命令层两套通路，按 pb_active 选择
    wire bin_mode, pb_busy, pb_building, pb_abort, pb_done, pb_failed, pb_command_go;
    reg  pb_active;
    wire [31:0] pb_base, pb_bytes, draw_cap, ref_cap, pb_error_index, pb_error_instance, pb_error_tag;
    wire [3:0]  pb_error_code, pb_error_stage, pb_src_id, pb_dst_id;
    wire        pb_perf_cmd, pb_perf_instance, pb_rd_beat, pb_wr_beat;
    wire [1:0]  pb_ref_add;
    wire [255:0] pb_command_packet;
    wire [63:0]  pb_command_clip;
    wire [31:0] pb_fd_addr, pw_addr;
    wire [11:0] pb_fd_bytes, pw_bytes;
    wire [3:0]  pb_fd_cidx;
    wire        pb_fd_valid, pb_fd_instance, pb_fs_ready, pw_valid, pw_ready, pw_data_valid, pw_data_ready, pw_status, pw_error;
    wire [127:0] pw_data;
    wire [31:0]  old_fd_addr;
    wire [11:0]  old_fd_bytes;
    wire [3:0]   old_fd_cidx, old_surf_ra_id, old_surf_rb_id;
    wire         old_fd_valid, old_fd_instance, old_fs_tready;
    wire         old_band_enable, old_band_start, old_band_flush, old_band_abort;
    wire [31:0]  old_band_bg_base, old_band_dst_base, pb_bg_base, pb_target_base;
    wire [4:0]   old_band_bg_slog2, old_band_dst_slog2, pb_bg_slog2, pb_target_slog2;
    wire [15:0]  old_band_y0, old_band_frame_height, pb_y0, pb_height;
    wire         pb_band_start, pb_band_flush;

    // 后端选择随门铃锁存；控制层复位或软复位回旧路径
    always @(posedge clk) begin
        if (rst_ctrl || soft_pending) pb_active <= 0;
        else if (kick) pb_active <= bin_mode;
    end
    assign fd_addr = pb_active ? pb_fd_addr : old_fd_addr;
    assign fd_bytes = pb_active ? pb_fd_bytes : old_fd_bytes;
    assign fd_cidx = pb_active ? pb_fd_cidx : old_fd_cidx;
    assign fd_instance = pb_active ? pb_fd_instance : old_fd_instance;
    assign fd_valid = pb_active ? pb_fd_valid : old_fd_valid;
    assign fs_tready = pb_active ? pb_fs_ready : old_fs_tready;
    assign surf_ra_id = pb_active ? pb_src_id : old_surf_ra_id;
    assign surf_rb_id = pb_active ? pb_dst_id : old_surf_rb_id;
    assign band_enable = pb_active ? pb_busy : old_band_enable;
    assign band_start = pb_active ? pb_band_start : old_band_start;
    assign band_flush = pb_active ? pb_band_flush : old_band_flush;
    assign band_abort = pb_active ? pb_abort : old_band_abort;
    assign band_bg_base = pb_active ? pb_bg_base : old_band_bg_base;
    assign band_dst_base = pb_active ? pb_target_base : old_band_dst_base;
    assign band_bg_slog2 = pb_active ? pb_bg_slog2 : old_band_bg_slog2;
    assign band_dst_slog2 = pb_active ? pb_target_slog2 : old_band_dst_slog2;
    assign band_y0 = pb_active ? pb_y0 : old_band_y0;
    assign band_frame_height = pb_active ? pb_height : old_band_frame_height;

    engine_pb #(.STRIP_H(STRIP_H)) u_pb (
        .clk(clk), .rst(rst_ctrl || soft_pending), .go(kick && bin_mode), .band_mode(band_mode),
        .list_base(list_base), .list_count(list_count),
        .pb_base(pb_base), .pb_bytes(pb_bytes), .draw_cap(draw_cap), .ref_cap(ref_cap),
        .lstrips(lstrips), .strip_log2(strip_log2), .band_surfs(band_surfs),
        .busy(pb_busy), .building(pb_building), .abort(pb_abort), .done(pb_done), .failed(pb_failed),
        .error_code(pb_error_code), .error_stage(pb_error_stage),
        .error_index(pb_error_index), .error_instance(pb_error_instance), .error_tag(pb_error_tag),
        .perf_cmd(pb_perf_cmd), .perf_instance(pb_perf_instance), .ref_add(pb_ref_add),
        .src_id(pb_src_id), .dst_id(pb_dst_id),
        .src_base(surf_ra_w0), .src_dim(surf_ra_w1), .src_ctrl(surf_ra_w2),
        .dst_base(surf_rb_w0), .dst_dim(surf_rb_w1), .dst_ctrl(surf_rb_w2),
        .band_start(pb_band_start), .band_flush(pb_band_flush),
        .bg_base(pb_bg_base), .target_base(pb_target_base),
        .bg_slog2(pb_bg_slog2), .target_slog2(pb_target_slog2),
        .band_y0(pb_y0), .frame_height(pb_height),
        .band_start_ready(band_start_ready), .band_flush_ready(band_flush_ready),
        .band_loaded(band_loaded), .band_flushed(band_flushed),
        .band_busy(band_busy), .band_error(band_error), .band_error_code(band_error_code),
        .drain_done(drain_done), .tmo_evt(tmo_evt), .tmo_write(tmo_dir),
        .command_go(pb_command_go), .command_packet(pb_command_packet), .command_clip(pb_command_clip),
        .command_busy(cmd_busy), .command_done(done_set), .command_error(err_set), .command_error_code(err_code),
        .fd_addr(pb_fd_addr), .fd_bytes(pb_fd_bytes), .fd_cidx(pb_fd_cidx),
        .fd_instance(pb_fd_instance), .fd_valid(pb_fd_valid), .fd_ready(fd_ready && pb_active),
        .fs_data(fs_tdata), .fs_valid(fs_tvalid && pb_active), .fs_last(fs_tlast),
        .fs_instance(fs_instance), .fs_cidx(fs_cidx), .fs_ready(pb_fs_ready),
        .rd_status(rd_stat_valid && rd_stat_fetch && pb_active), .rd_error(rd_stat_err),
        .rd_instance(rd_stat_instance), .rd_cidx(rd_stat_cidx),
        .pw_addr(pw_addr), .pw_bytes(pw_bytes), .pw_valid(pw_valid), .pw_ready(pw_ready),
        .pw_data(pw_data), .pw_data_valid(pw_data_valid),
        .pw_data_ready(pw_data_ready), .pw_status(pw_status), .pw_error(pw_error));

    // TEX 装载窗口（第 4~7 页）与片上读流
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
    wire        g_rd_band, g_wr_band;
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
    // 读错误行及失败命令的后续写流仍可走完描述符，但不得改 DDR：
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

    // PERF 汇聚（I8）：读事件只计真实 DDR 描述符与命令取数，TEX 读不计入
    wire [3:0] perf_key_lanes_w;
    wire       perf_key_valid_w;
`ifdef INJ_STRIP_PERF
    wire perf_rdb = (g_rd_valid && g_rd_ready && !g_rd_tex)
`else
    wire perf_rdb = (g_rd_valid && g_rd_ready && !g_rd_tex && !g_rd_band)
`endif || (fd_valid && fd_ready) ||
                    band_perf_rd;
    wire [11:0] perf_rdb_beats = (fd_valid && fd_ready) ? {4'b0, fd_bytes[11:4]}
                                                        : band_perf_rd ? 12'd160 : g_rd_beats;
    wire perf_wrb = (pw_valid && pw_ready) || (g_wr_valid && g_wr_ready && !g_wr_band) ||
                    band_perf_wr;
    wire perf_wait = (m_axi_arvalid && !m_axi_arready)
                  || (m_axi_rvalid  && !m_axi_rready)
                  || (m_axi_awvalid && !m_axi_awready)
                  || (m_axi_wvalid  && !m_axi_wready)
                  || (m_axi_bvalid  && !m_axi_bready);

    // APB 分选：第 0 页寄存器、第 4~7 页 TEX；psel 已含两处页门控
    wire apb_hit_regs = (paddr[15:12] == 4'h0);
    wire apb_hit_tex  = (paddr[15:14] == 2'b01);
    assign prdata  = apb_hit_regs ? regs_prdata : tex_prdata;
    assign pready  = apb_hit_regs ? regs_pready : tex_pready;
    assign pslverr = apb_hit_regs ? regs_pslverr : tex_pslverr;

    // 寄存器层
    engine_regs #(.STRIP_H(STRIP_H)) u_regs (
        .clk(clk), .rst(rst_ctrl),
        .paddr(paddr[11:0]), .psel(psel && apb_hit_regs), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(regs_prdata), .pready(regs_pready), .pslverr(regs_pslverr),
        .cmd_busy(cmd_busy || pb_busy), .pb_busy(pb_busy),
        .err_set(pb_active ? (pb_done && pb_failed) : err_set),
        .err_code(pb_active ? pb_error_code : err_code),
        .err_index(pb_active ? pb_error_index : err_index),
        .err_tag(pb_active ? pb_error_tag : err_tag),
        .err_instance(pb_active ? pb_error_instance : err_instance),
        .err_stage(pb_active ? pb_error_stage : 4'd0),
        .perf_instance(pb_active ? pb_perf_instance : perf_instance),
        .done_set(pb_active ? (pb_done && !pb_failed) : done_set),
        .kick(kick), .list_base(list_base), .list_count(list_count),
        .band_mode(band_mode), .bin_mode(bin_mode),
        .pb_base(pb_base), .pb_bytes(pb_bytes), .draw_cap(draw_cap), .ref_cap(ref_cap),
        .lstrips(lstrips), .strip_log2(strip_log2), .band_surfs(band_surfs),
        .soft_rst_done(drain_done), .soft_rst_pending(soft_pending),
        .irq_b(irq_b),
        .perf_cmd(pb_active ? pb_perf_cmd : commit),
        .perf_refs(pb_active ? pb_ref_add : 2'd0),
        .perf_bin(pb_building), .perf_real_mode(pb_active),
        .perf_axi_rd(m_axi_rvalid && m_axi_rready), .perf_axi_wr(m_axi_wvalid && m_axi_wready),
        .perf_pb_rd(pb_active && pb_rd_beat), .perf_pb_wr(pb_active && pb_wr_beat),
        .perf_pixel_en(sb_valid && sb_ready), .perf_pixel_npx(sb_npx),
        .perf_key_en(perf_key_valid_w), .perf_key_lanes(perf_key_lanes_w),
        .perf_rdb(perf_rdb), .perf_rdb_beats(perf_rdb_beats),
        .perf_wrb(perf_wrb),
        .perf_wrb_beats((pw_valid && pw_ready) ? {4'd0, pw_bytes[11:4]} : (band_perf_wr ? 12'd160 : g_wr_beats)),
        .perf_wait(perf_wait), .perf_busy(cmd_busy || pb_busy),
        .surf_ra_id(surf_ra_id), .surf_rb_id(surf_rb_id),
        .surf_ra_w0(surf_ra_w0), .surf_ra_w1(surf_ra_w1), .surf_ra_w2(surf_ra_w2),
        .surf_rb_w0(surf_rb_w0), .surf_rb_w1(surf_rb_w1), .surf_rb_w2(surf_rb_w2)
    );

    // 命令层
    engine_cmd #(.STRIP_H(STRIP_H)) u_cmd (
        .clk(clk), .rst(rst_ctrl),
        .kick((kick && !bin_mode) || pb_command_go),
        .external_mode(pb_command_go), .external_packet(pb_command_packet), .external_clip(pb_command_clip),
        .list_base(list_base), .list_count(list_count),
        .band_mode(band_mode && !pb_active), .lstrips(lstrips), .strip_log2(strip_log2), .band_surfs(band_surfs),
        .soft_rst(soft_pending || pb_abort), .preserve_clip(pb_abort && !soft_pending),
        .band_enable(old_band_enable), .band_start(old_band_start), .band_flush(old_band_flush), .band_abort(old_band_abort),
        .band_bg_base(old_band_bg_base), .band_dst_base(old_band_dst_base),
        .band_bg_slog2(old_band_bg_slog2), .band_dst_slog2(old_band_dst_slog2),
        .band_y0(old_band_y0), .band_frame_height(old_band_frame_height),
        .band_start_ready(band_start_ready), .band_flush_ready(band_flush_ready),
        .band_loaded(band_loaded), .band_flushed(band_flushed),
        .band_busy(band_busy), .band_error(band_error), .band_error_code(band_error_code),
        .drain_done(drain_done),
        .busy(cmd_busy),
        .err_set(err_set), .err_code(err_code),
        .err_index(err_index), .err_tag(err_tag), .err_instance(err_instance), .perf_instance(perf_instance),
        .done_set(done_set), .commit(commit),
        .suppress_writes(suppress_writes),
        .surf_ra_id(old_surf_ra_id), .surf_rb_id(old_surf_rb_id),
        .surf_ra_w0(surf_ra_w0), .surf_ra_w1(surf_ra_w1), .surf_ra_w2(surf_ra_w2),
        .surf_rb_w0(surf_rb_w0), .surf_rb_w1(surf_rb_w1), .surf_rb_w2(surf_rb_w2),
        .fd_addr(old_fd_addr), .fd_bytes(old_fd_bytes), .fd_cidx(old_fd_cidx),
        .fd_instance(old_fd_instance), .fs_instance(fs_instance), .rd_stat_instance(rd_stat_instance),
        .fd_valid(old_fd_valid), .fd_ready(fd_ready && !pb_active),
        .fs_tdata(fs_tdata), .fs_tlast(fs_tlast), .fs_tvalid(fs_tvalid && !pb_active),
        .fs_tready(old_fs_tready), .fs_cidx(fs_cidx),
        .rd_stat_valid(rd_stat_valid), .rd_stat_fetch(rd_stat_fetch),
        .rd_stat_err(rd_stat_err), .rd_stat_cidx(rd_stat_cidx),
        .wr_stat_valid(wr_stat_valid), .wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx), .wr_stat_last_row(wr_stat_last_row),
        .tmo_evt(tmo_evt), .tmo_instance(fs_instance && !tmo_dir),
        .start(geom_start),
        .i1_op(i1_op), .i1_eq(i1_eq), .i1_key_en(i1_key_en),
        .i1_key(i1_key), .i1_w4(i1_w4), .i1_c(i1_c), .i1_sfmt(i1_sfmt),
        .i1_src_base(i1_src_base), .i1_src_tex(i1_src_tex),
        .i1_src_w(i1_src_w), .i1_src_h(i1_src_h), .i1_src_slog2(i1_src_slog2),
        .i1_dst_base(i1_dst_base), .i1_dst_w(i1_dst_w), .i1_dst_h(i1_dst_h), .i1_dst_slog2(i1_dst_slog2),
        .i1_src_x(i1_src_x), .i1_src_y(i1_src_y), .i1_dst_x(i1_dst_x), .i1_dst_y(i1_dst_y),
        .i1_w(i1_w), .i1_h(i1_h),
        .i1_clip_x0(i1_clip_x0), .i1_clip_y0(i1_clip_y0), .i1_clip_x1(i1_clip_x1), .i1_clip_y1(i1_clip_y1),
        .i1_cidx(i1_cidx),
        .geom_busy(geom_busy), .geom_cmd_done(geom_cmd_done), .geom_done_cidx(geom_done_cidx),
        .fe_cidx_err(dbg_fe_cidx_err)
    );

    // 几何层
    engine_geom #(.MAX_BEATS(192), .STRIP_H(STRIP_H)) u_geom (
        .clk(clk), .rst(rst_ctrl || pb_abort),
        .start(geom_start),
        .op(i1_op), .eq(i1_eq), .key_en(i1_key_en),
        .key_w5(i1_key), .const_w4(i1_w4), .c(i1_c), .sfmt(i1_sfmt),
        .src_base(i1_src_base), .src_w(i1_src_w), .src_h(i1_src_h),
        .src_slog2(i1_src_slog2), .src_tex(i1_src_tex),
        .band_enable(band_enable), .band_y0(band_y0),
        .dst_base(i1_dst_base), .dst_w(i1_dst_w), .dst_h(i1_dst_h), .dst_slog2(i1_dst_slog2),
        .src_x(i1_src_x), .src_y(i1_src_y), .dst_x(i1_dst_x), .dst_y(i1_dst_y),
        .w(i1_w), .h(i1_h),
        .clip_x0(i1_clip_x0), .clip_y0(i1_clip_y0), .clip_x1(i1_clip_x1), .clip_y1(i1_clip_y1),
        .cidx(i1_cidx),
        .row_free(row_free),
        .busy(geom_busy), .cmd_done(geom_cmd_done), .done_cidx(geom_done_cidx),
        .rd_addr(g_rd_addr), .rd_beats(g_rd_beats), .rd_buf(g_rd_buf), .rd_tex(g_rd_tex),
        .rd_band(g_rd_band), .wr_band(g_wr_band),
        .rd_cidx(g_rd_cidx), .rd_valid(g_rd_valid), .rd_ready(g_rd_ready),
        .wr_addr(g_wr_addr), .wr_beats(g_wr_beats), .wr_cidx(g_wr_cidx),
        .wr_last_row(g_wr_last_row), .wr_valid(g_wr_valid), .wr_ready(g_wr_ready),
        .sb_row(sb_row), .sb_py(sb_py), .sb_npx(sb_npx), .sb_beats(sb_beats),
        .sb_lead_dst(sb_lead_dst), .sb_lead_src(sb_lead_src),
        .sb_op(sb_op), .sb_eq(sb_eq), .sb_key_en(sb_key_en), .sb_key(sb_key),
        .sb_w4(sb_w4), .sb_c(sb_c), .sb_sfmt(sb_sfmt), .sb_read_dst(sb_read_dst),
        .sb_valid(sb_valid), .sb_ready(sb_ready)
    );

    // 计算阵列与计算核心
    engine_pix u_pix (
        .clk(clk), .rst(rst_ctrl || pb_abort),
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

    // 搬运层（AXI 事务层）
    engine_mover #(.AXI_MAX_BURST_LEN(192), .TMO_WIDTH(13), .ENABLE_BAND(1), .BAND_STRIP_H(STRIP_H)) u_mover (
        .clk(clk), .rst(mover_rst),
        .pw_addr(pw_addr), .pw_bytes(pw_bytes), .pw_valid(pw_valid), .pw_ready(pw_ready),
        .pw_data(pw_data), .pw_data_valid(pw_data_valid), .pw_data_ready(pw_data_ready),
        .pw_status(pw_status), .pw_error(pw_error),
        .pb_rd_beat(pb_rd_beat), .pb_wr_beat(pb_wr_beat),
        .band_perf_rd(band_perf_rd), .band_perf_wr(band_perf_wr),
        .rd_addr(g_rd_addr), .rd_bytes(g_rd_bytes), .rd_buf(g_rd_buf),
        .rd_cidx(g_rd_cidx), .rd_tex(g_rd_tex),
        .rd_band(g_rd_band), .wr_band(g_wr_band),
        // LOAD/EXEC/FLUSH 三相的条带级控制
        .band_start(band_start), .band_flush(band_flush),
        .band_start_ready(band_start_ready), .band_flush_ready(band_flush_ready),
        .band_bg_base(band_bg_base), .band_dst_base(band_dst_base),
        .band_bg_slog2(band_bg_slog2), .band_dst_slog2(band_dst_slog2),
        .band_y0(band_y0), .band_frame_height(band_frame_height),
        .band_loaded(band_loaded), .band_flushed(band_flushed), .band_active(), .band_busy(band_busy),
        .band_error(band_error), .band_error_code(band_error_code),
        .rd_valid(g_rd_valid), .rd_ready(g_rd_ready),
        .tx_addr(tx_addr), .tx_bytes(tx_bytes), .tx_buf(tx_buf), .tx_cidx(tx_cidx),
        .tx_valid(tx_valid), .tx_ready(tx_ready),
        .tx_rs_tdata(tx_rs_tdata), .tx_rs_tvalid(tx_rs_tvalid),
        .tx_rs_tlast(tx_rs_tlast), .tx_rs_buf(tx_rs_buf),
        .tx_stat_valid(tx_stat_valid), .tx_stat_ready(tx_stat_ready), .tx_stat_cidx(tx_stat_cidx),
        .tx_stat_buf(tx_stat_buf), .tx_busy(tx_busy),
        .fd_addr(fd_addr), .fd_bytes(fd_bytes), .fd_cidx(fd_cidx),
        .fd_instance(fd_instance), .fs_instance(fs_instance), .rd_stat_instance(rd_stat_instance),
        .fd_valid(fd_valid), .fd_ready(fd_ready),
        .rs_tdata(rs_tdata), .rs_tkeep(rs_tkeep), .rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid), .rs_tready(rs_tready), .rs_buf(rs_buf),
        .fs_tdata(fs_tdata), .fs_tkeep(), .fs_tlast(fs_tlast), .fs_tvalid(fs_tvalid),
        .fs_tready(fs_tready), .fs_cidx(fs_cidx),
        .wr_addr(g_wr_addr), .wr_bytes(g_wr_bytes), .wr_cidx(g_wr_cidx),
        .wr_last_row(g_wr_last_row), .wr_valid(g_wr_valid), .wr_ready(g_wr_ready),
        .ws_tdata(ws_data_safe), .ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid), .ws_tready(ws_tready),
        .rd_stat_valid(rd_stat_valid), .rd_stat_err(rd_stat_err), .rd_stat_fetch(rd_stat_fetch),
        .rd_stat_cidx(rd_stat_cidx), .rd_stat_buf(),
        .wr_stat_valid(wr_stat_valid), .wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx), .wr_stat_last_row(wr_stat_last_row),
        .drain_req(rst_ctrl || soft_pending || band_abort),
        .drain_busy(), .drain_done(drain_done),
        .tmo_limit(EC_TMO_RESET),
        .err_clr(kick),
        .tmo_evt(tmo_evt), .tmo_dir(tmo_dir),
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

    // TEX 装载窗口：第 4~7 页的 16 KiB scratchpad
    assign tex_psel = psel && apb_hit_tex;
    engine_tex #(.READ_LATENCY(TEX_READ_LATENCY)) u_tex (
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
