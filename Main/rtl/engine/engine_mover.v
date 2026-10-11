`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_mover.v
//
// DESCRIPTION: 引擎唯一的 AXI 主机（接 Axi_Mux S1）：两个 verilog-axi 核（源码不改）
//   加读侧仲裁、单笔在途门控、复位排空、超时监视与 W 通道带内掩码，
//   另含 BAND 条带数据面与 TEX 片上读的接入。
//
// NOTE:
//   1. 每方向最多一笔在途；读方向完成 = 状态已捕获且流 tlast 已握手，写方向等状态
//   2. 每个写描述符最后一拍必须带 tlast（核在流未收完时会丢数据）；正常写流与排空哑拍同一式子
//   3. 排空一次请求只完成一次：drain_req 举着的每一拍都不收新提交
//   4. 超时只报告不放弃；「握手」两侧都算（本层与 AXI），T 复位值 4096 拍
//   5. 带内掩码在核外按像素 X 字节最高位清 wstrb——写核中间拍不保留 tkeep，不能塞进流
//   6. 命令取数优先于像素读；TEX/BAND 在 DDR 读收尾后受理，返回流保持单所有者
//   7. 注错开关见 engine-manual.md
//==============================================================================================//
`default_nettype none

module engine_mover #(
    parameter integer AXI_MAX_BURST_LEN = 192,
    // 超时门限 T 的位宽：控制层给的 tmo_limit 最宽这么多位，4096 要 13 位
    parameter integer TMO_WIDTH = 13,
    parameter integer ENABLE_BAND = 0,
    parameter integer BAND_STRIP_H = 16
)(
    input  wire                  clk,
    input  wire                  rst,          // 搬运层复位，顶层接 ~ddr_rstn

    // PB 前端的写工作区通路与 PERF 拍计数
    input  wire [31:0]           pw_addr,
    input  wire [11:0]           pw_bytes,
    input  wire                  pw_valid,
    output wire                  pw_ready,
    input  wire [127:0]          pw_data,
    input  wire                  pw_data_valid,
    output wire                  pw_data_ready,
    output wire                  pw_status,
    output wire                  pw_error,
    output wire                  pb_rd_beat,
    output wire                  pb_wr_beat,
    output wire                  band_perf_rd,
    output wire                  band_perf_wr,

    // I2：像素读描述符（几何层 → 搬运层）
    input  wire [31:0]           rd_addr,      // 字节地址，16 字节对齐
    input  wire [11:0]           rd_bytes,     // 字节数，16 的倍数
    input  wire [1:0]            rd_buf,       // 落到哪块行缓冲
    input  wire [3:0]            rd_cidx,      // 命令索引，错误归属
    input  wire                  rd_tex,       // 源在片上 TEX（G1）：描述符入口选路
    input  wire                  rd_band,      // G2：目的在 BAND；ENABLE_BAND=0 时忽略
    input  wire                  rd_valid,
    output wire                  rd_ready,

    // TEX 片上读（engine_tex）：描述符出口与流/状态入口
    output wire [13:0]           tx_addr,      // TEX 窗口内字节地址，16 字节对齐
    output wire [11:0]           tx_bytes,
    output wire [1:0]            tx_buf,
    output wire [3:0]            tx_cidx,
    output wire                  tx_valid,
    input  wire                  tx_ready,
    input  wire [127:0]          tx_rs_tdata,  // 本地读流（消费者恒 ready，无 ready 口）
    input  wire                  tx_rs_tvalid,
    input  wire                  tx_rs_tlast,
    input  wire [1:0]            tx_rs_buf,
    input  wire                  tx_stat_valid, // 保持到 ready，恒无错
    output wire                  tx_stat_ready,
    input  wire [3:0]            tx_stat_cidx,
    input  wire [1:0]            tx_stat_buf,
    input  wire                  tx_busy,       // TEX 描述符在途（受理起到状态脉冲止）

    // I7：命令取数描述符（控制层 → 搬运层），优先于像素读
    input  wire                  fd_instance,
    output wire                  fs_instance,
    output wire                  rd_stat_instance,
    input  wire [31:0]           fd_addr,
    input  wire [11:0]           fd_bytes,
    input  wire [3:0]            fd_cidx,     // 描述符上下文低 4 位；批量取数时取批首，不是每条记录的索引
    input  wire                  fd_valid,
    output wire                  fd_ready,

    // I3：读流（搬运层 → 计算阵列）
    output wire [127:0]          rs_tdata,     // 像素读流，进行缓冲
    output wire [15:0]           rs_tkeep,
    output wire                  rs_tlast,
    output wire                  rs_tvalid,
    input  wire                  rs_tready,    // 消费者恒 ready
    output wire [1:0]            rs_buf,       // 本拍数据的目标缓冲号
    output wire [127:0]          fs_tdata,     // 命令取数流，进命令缓冲
    output wire [15:0]           fs_tkeep,
    output wire                  fs_tlast,
    output wire                  fs_tvalid,
    input  wire                  fs_tready,
    output wire [3:0]            fs_cidx,      // 回显 fd_cidx；批量取数须按拍序在 B5 重建记录索引

    // I2：写描述符（几何层 → 搬运层）
    input  wire [31:0]           wr_addr,
    input  wire [11:0]           wr_bytes,
    input  wire [3:0]            wr_cidx,
    input  wire                  wr_last_row,  // 本命令的最后一行
    input  wire                  wr_band,      // G2：像素结果写入 BAND
    input  wire                  wr_valid,
    output wire                  wr_ready,

    // I4：写流（计算阵列 → 搬运层）
    input  wire [127:0]          ws_tdata,     // 每拍 4 像素，X 字节最高位 = 1 表示不写
    input  wire [15:0]           ws_tkeep,     // 写流没有 tlast 口：本层按描述符拍数打
    input  wire                  ws_tvalid,
    output wire                  ws_tready,

    // BAND 条带面控制口（G2 直接控制，G3 接帧级循环）
    input  wire                  band_start,
    input  wire                  band_flush,
    output wire                  band_start_ready,
    output wire                  band_flush_ready,
    input  wire [31:0]           band_bg_base,
    input  wire [31:0]           band_dst_base,
    input  wire [4:0]            band_bg_slog2,
    input  wire [4:0]            band_dst_slog2,
    input  wire [15:0]           band_y0,
    input  wire [15:0]           band_frame_height,
    output wire                  band_loaded,
    output wire                  band_flushed,
    output wire                  band_active,
    output wire                  band_busy,
    output wire                  band_error,
    output wire [3:0]            band_error_code,

    // I7：状态（搬运层 → 控制层）
    output wire                  rd_stat_valid, // 一拍脉冲，负载同拍有效
    output wire                  rd_stat_err,   // 1 = 读回 rresp 非 OKAY
    output wire                  rd_stat_fetch, // 显式来源：1 = 命令取数（G1，控制层不再按 FE 状态猜）
    output wire [3:0]            rd_stat_cidx,
    output wire [1:0]            rd_stat_buf,
    output wire                  wr_stat_valid,
    output wire                  wr_stat_err,   // 1 = 写响应非 OKAY
    output wire [3:0]            wr_stat_cidx,
    output wire                  wr_stat_last_row,

    // I7：排空与超时
    input  wire                  drain_req,     // 电平请求
    output wire                  drain_busy,
    output wire                  drain_done,    // 一拍脉冲
    input  wire [TMO_WIDTH-1:0]  tmo_limit,     // 超时门限 T，复位值 4096
    input  wire                  err_clr,       // 控制层清粘滞错误，超时计时一并重起
    output wire                  tmo_evt,       // 一拍脉冲
    output wire                  tmo_dir,       // 脉冲那一拍：0 读、1 写

    // AXI 主口（接 Axi_Mux S1）
    output wire [31:0]           m_axi_araddr,
    output wire [7:0]            m_axi_arlen,
    output wire [2:0]            m_axi_arsize,
    output wire [1:0]            m_axi_arburst,
    output wire                  m_axi_arvalid,
    input  wire                  m_axi_arready,
    input  wire [127:0]          m_axi_rdata,
    input  wire [1:0]            m_axi_rresp,
    input  wire                  m_axi_rlast,
    input  wire                  m_axi_rvalid,
    output wire                  m_axi_rready,

    output wire [31:0]           m_axi_awaddr,
    output wire [7:0]            m_axi_awlen,
    output wire [2:0]            m_axi_awsize,
    output wire [1:0]            m_axi_awburst,
    output wire                  m_axi_awvalid,
    input  wire                  m_axi_awready,
    output wire [127:0]          m_axi_wdata,
    output wire [15:0]           m_axi_wstrb,
    output wire                  m_axi_wlast,
    output wire                  m_axi_wvalid,
    input  wire                  m_axi_wready,
    input  wire [1:0]            m_axi_bresp,
    input  wire                  m_axi_bvalid,
    output wire                  m_axi_bready
);

    // 上下文寄存器：每方向一笔在途，各一组就够；不用核的 4 位 tag
    reg         rd_ctx_instance;
    reg         rd_ctx_cmd;      // 在途读描述符来源：0 像素读、1 FD；命令/实例由 rd_ctx_instance 区分
    reg         rd_ctx_bg, wr_ctx_flush;
    reg         rd_busy, wr_busy;
    wire        drain_hold;
    reg  [1:0]  rd_ctx_buf;
    reg  [3:0]  rd_ctx_cidx;
    reg  [3:0]  wr_ctx_cidx;
    reg         wr_ctx_last_row;

    // BAND 条带面的连接线（ENABLE_BAND=0 时全接 0）
    wire rd_is_band = ENABLE_BAND ? rd_band : 1'b0;
    wire wr_is_band = ENABLE_BAND ? wr_band : 1'b0;
    wire b_start_ready, b_flush_ready, b_loading, b_flushing, b_rbusy, b_wbusy;
    wire b_start_fire = band_start && band_start_ready;
    wire b_flush_fire = band_flush && band_flush_ready;
    wire b_xfer = b_loading || b_flushing || (band_busy && !band_active);
    wire b_bus_idle = !rd_busy && !wr_busy && !tx_busy;
    assign band_start_ready = b_start_ready && b_bus_idle && !drain_hold;
    assign band_flush_ready = b_flush_ready && b_bus_idle && !drain_hold;
    wire [31:0]  b_rd_addr, b_wr_addr;
    wire [11:0]  b_rd_bytes, b_wr_bytes;
    wire         b_rd_valid, b_rd_ready, b_wr_valid, b_wr_ready;
    wire [127:0] b_wdata, b_rsdata;
    wire         b_wvalid, b_wready, b_rsvalid, b_rslast, b_rstat, b_rerr, b_wstat, b_werr, b_wlast;
    wire [1:0]   b_rsbuf, b_rstatbuf;
    wire [3:0]   b_rcidx, b_wcidx;
    wire         b_rready, b_wready_desc, b_wsready, b_rfire, b_wfire, b_rstat_ready, b_wstat_ready;
    generate if(ENABLE_BAND!=0) begin : g_band
        engine_band #(.STRIP_H(BAND_STRIP_H)) u_band(
            .clk(clk),.rst(rst),.cancel(drain_hold),.err_clr(err_clr),
            .start(b_start_fire),.start_ready(b_start_ready),.flush(b_flush_fire),.flush_ready(b_flush_ready),
            .bg_base(band_bg_base),.dst_base(band_dst_base),
            .bg_slog2(band_bg_slog2),.dst_slog2(band_dst_slog2),
            .strip_y0(band_y0),.frame_height(band_frame_height),
            .loaded(band_loaded),.flushed(band_flushed),.active(band_active),.busy(band_busy),
            .loading(b_loading),.flushing(b_flushing),.error(band_error),.error_code(band_error_code),
            .dma_rd_addr(b_rd_addr),.dma_rd_bytes(b_rd_bytes),.dma_rd_valid(b_rd_valid),.dma_rd_ready(b_rd_ready),
            .dma_rdata(core_rs_tdata),.dma_rvalid(core_rs_tvalid && rd_ctx_bg),.dma_rlast(core_rs_tlast),.dma_rready(),
            .dma_rd_done(rd_stat_v && rd_ctx_bg),.dma_rd_error(rd_err_r),
            .dma_wr_addr(b_wr_addr),.dma_wr_bytes(b_wr_bytes),.dma_wr_valid(b_wr_valid),.dma_wr_ready(b_wr_ready),
            .dma_wdata(b_wdata),.dma_wvalid(b_wvalid),.dma_wready(b_wready),
            .dma_wr_done(wr_stat_v && wr_ctx_flush),.dma_wr_error(wr_err_r),.dma_idle(b_bus_idle),
            .rd_addr(rd_addr),.rd_bytes(rd_bytes),.rd_buf(rd_buf),.rd_cidx(rd_cidx),.rd_valid(b_rfire),.rd_ready(b_rready),
            .rs_data(b_rsdata),.rs_valid(b_rsvalid),.rs_last(b_rslast),.rs_buf(b_rsbuf),
            .rd_stat_valid(b_rstat),.rd_stat_error(b_rerr),.rd_stat_cidx(b_rcidx),.rd_stat_buf(b_rstatbuf),
            .rd_stat_ready(b_rstat_ready),.rd_busy(b_rbusy),
            .wr_addr(wr_addr),.wr_bytes(wr_bytes),.wr_cidx(wr_cidx),.wr_last_row(wr_last_row),
            .wr_valid(b_wfire),.wr_ready(b_wready_desc),.ws_data(ws_tdata),.ws_keep(ws_tkeep),
            .ws_valid(ws_tvalid && b_wbusy && !drain_hold),.ws_ready(b_wsready),
            .wr_stat_valid(b_wstat),.wr_stat_error(b_werr),.wr_stat_cidx(b_wcidx),.wr_stat_last_row(b_wlast),
            .wr_stat_ready(b_wstat_ready),.wr_busy(b_wbusy));
    end else begin : g_no_band
        assign b_start_ready=0;assign b_flush_ready=0;
        assign band_loaded=0;assign band_flushed=0;assign band_active=0;assign band_busy=0;
        assign band_error=0;assign band_error_code=0;assign b_loading=0;assign b_flushing=0;
        assign b_rbusy=0;assign b_wbusy=0;assign b_rd_addr=0;assign b_wr_addr=0;
        assign b_rd_bytes=0;assign b_wr_bytes=0;assign b_rd_valid=0;assign b_wr_valid=0;
        assign b_wdata=0;assign b_wvalid=0;assign b_rsdata=0;assign b_rsvalid=0;assign b_rslast=0;
        assign b_rstat=0;assign b_rerr=0;assign b_wstat=0;assign b_werr=0;assign b_wlast=0;
        assign b_rsbuf=0;assign b_rstatbuf=0;assign b_rcidx=0;assign b_wcidx=0;
        assign b_rready=0;assign b_wready_desc=0;assign b_wsready=0;
    end endgenerate

    // 复位排空状态机
    reg draining;
    reg drain_served;        // 这一次请求已经完成过，等 drain_req 撤下才清
`ifdef INJ_DRAIN_LEVEL
    wire drain_enter     = drain_req && !draining;     // 注错：drain_done 之后仍按电平重进
`else
    wire drain_enter     = drain_req && !draining && !drain_served;
`endif

    always @(posedge clk) begin
        if (rst)                                   draining <= 1'b0;
        else if (drain_enter)                      draining <= 1'b1;
        else if (draining && !rd_busy && !wr_busy && !tx_busy && !band_busy)
                                                   draining <= 1'b0;
    end

    always @(posedge clk) begin
        if (rst || !drain_req)                     drain_served <= 1'b0;
        else if (drain_done)                       drain_served <= 1'b1;
    end

    // 挡新提交：排空态里，以及请求还举着的每一拍
`ifdef INJ_DRAIN_LEVEL
    assign      drain_hold = draining;                 // 注错：只在排空态里挡提交
`else
    assign      drain_hold = draining || drain_req;
`endif

`ifdef INJ_DRAIN_BYPASS
    // 注错：排空期不帮忙收/补，直接等两个方向自己空
    wire        rd_drain_help = 1'b0;
    wire        wr_drain_help = 1'b0;
`else
    wire        rd_drain_help = draining && rd_busy;
    wire        wr_drain_help = draining && wr_busy;
`endif

    assign      drain_busy = draining;
    assign      drain_done = draining && !rd_busy && !wr_busy && !tx_busy && !band_busy;

    // 读侧：仲裁与门控
    wire        rd_core_ready;   // 读核描述符口 ready
    reg         rd_status_seen;
    reg         rd_stream_done;
    reg         rd_err_r;
    reg         rd_stat_v;

    // 门控条件：状态脉冲已捕获且读流 tlast 已握手
`ifdef INJ_NO_GATE_RD
    wire        rd_gate_done = 1'b1;                   // 注错：去掉读门控
`else
    wire        rd_gate_done = (rd_status_seen && rd_stream_done);
`endif
    // 排空期间挡新提交。TEX 流在途时可预发 DDR 请求，但 I3 保持单流所有者。
    wire        rd_block     = (rd_busy && !rd_gate_done) || drain_hold || b_xfer || b_start_fire || b_flush_fire;
    wire        rd_open      = !rd_block;

    // 仲裁：命令取数优先于像素读；像素读按 rd_tex/rd_band 在描述符入口选路。
    // TEX/BAND 在 DDR 读流收尾后受理；返回流待数据与状态收尾后才交给 I3，
    wire        rd_pick_bg    = b_rd_valid;
    wire        rd_pick_fd    = !rd_pick_bg && fd_valid;
    wire        rd_pick_tex   = !fd_valid && rd_valid && rd_tex && !rd_is_band;
    wire        bg_open       = !drain_hold && (!rd_busy || rd_gate_done);
    wire        rd_fire       = rd_core_ready && (rd_pick_bg ? bg_open :
                               (rd_open && (fd_valid || (rd_valid && !rd_tex && !rd_is_band))));
    wire        tx_fire       = tx_ready && rd_open && rd_pick_tex && !rd_busy && !b_rbusy;
    assign      b_rfire       = b_rready && rd_open && !fd_valid && rd_valid && rd_is_band && !rd_busy && !tx_busy;
    assign      b_rd_ready    = rd_core_ready && bg_open;
    assign band_perf_rd = b_rd_valid && b_rd_ready;
    assign band_perf_wr = b_wr_valid && b_wr_ready;
    assign      fd_ready      = rd_core_ready && rd_open;
    assign      rd_ready      = rd_open && !fd_valid &&
                                (rd_is_band ? (b_rready && !rd_busy && !tx_busy) :
                                 (rd_tex ? (tx_ready && !rd_busy && !b_rbusy) : rd_core_ready));

    wire [31:0] dsc_addr      = rd_pick_bg ? b_rd_addr : (rd_pick_fd ? fd_addr : rd_addr);
    wire [11:0] dsc_bytes     = rd_pick_bg ? b_rd_bytes : (rd_pick_fd ? fd_bytes : rd_bytes);

    // TEX 描述符出口：地址截到窗口内使用（越界是 cmd 检查的责任）
    assign      tx_addr       = rd_addr[13:0];
    assign      tx_bytes      = rd_bytes;
    assign      tx_buf        = rd_buf;
    assign      tx_cidx       = rd_cidx;
    assign      tx_valid      = tx_fire;

    // I3 的源流先于目的流，命令流另走 fs；上下文各带各的。
    wire [127:0] core_rs_tdata;
    wire [15:0]  core_rs_tkeep;
    wire         core_rs_tlast, core_rs_tvalid, core_rs_tready;
`ifdef INJ_TX_NOWAIT
    wire        tex_data_blocks = 1'b0;                 // 注错：去掉目的流等待
`else
    wire        tex_data_blocks = tx_busy;
`endif
`ifdef INJ_BAND_FLOW_WAIT
    wire        band_data_blocks=1'b0;
`else
    wire        band_data_blocks=b_rbusy;
`endif
    wire        tx_data_blocks = tex_data_blocks || band_data_blocks;
    wire        core_rs_pix   = core_rs_tvalid && !rd_ctx_cmd && !rd_ctx_bg && !rd_drain_help && !tx_data_blocks;
    wire        tex_rs_pix    = tx_rs_tvalid && !drain_hold;
    wire        band_rs_pix   = b_rsvalid && !drain_hold;
    assign      core_rs_tready = rd_drain_help ? 1'b1
                               : (rd_ctx_bg ? 1'b1 : (rd_ctx_cmd ? fs_tready : (rs_tready && !tx_data_blocks)));
    assign      rs_tvalid      = core_rs_pix || tex_rs_pix || band_rs_pix;
    assign      fs_tvalid      = core_rs_tvalid &&  rd_ctx_cmd && !rd_drain_help;
    assign      rs_tdata       = band_rs_pix ? b_rsdata : (tex_rs_pix ? tx_rs_tdata : core_rs_tdata);
    assign      rs_tkeep       = (tex_rs_pix || band_rs_pix) ? 16'hFFFF : core_rs_tkeep;
    assign      rs_tlast       = band_rs_pix ? b_rslast : (tex_rs_pix ? tx_rs_tlast : core_rs_tlast);
    assign      rs_buf         = band_rs_pix ? b_rsbuf : (tex_rs_pix ? tx_rs_buf : rd_ctx_buf);
    assign      fs_tdata       = core_rs_tdata;
    assign      fs_tkeep       = core_rs_tkeep;
    assign      fs_tlast       = core_rs_tlast;
    assign      fs_cidx        = rd_ctx_cidx;

    wire        rs_fire        = core_rs_tvalid && core_rs_tready;
    wire        rs_last_fire   = rs_fire && core_rs_tlast;

    wire        rd_status_valid;
    wire [3:0]  rd_status_tag;
    wire [3:0]  rd_status_err;

    always @(posedge clk) begin
        if (rst) begin
            rd_busy        <= 1'b0;
            rd_status_seen <= 1'b0;
            rd_stream_done <= 1'b0;
            rd_err_r       <= 1'b0;
            rd_stat_v      <= 1'b0;
            rd_ctx_cmd     <= 1'b0;
            rd_ctx_instance<=0;
            rd_ctx_bg      <= 1'b0;
            rd_ctx_buf     <= 2'd0;
            rd_ctx_cidx    <= 4'd0;
        end else begin
            if (rd_fire) begin
                rd_busy        <= 1'b1;
                rd_status_seen <= 1'b0;
                rd_stream_done <= 1'b0;
                rd_ctx_cmd     <= rd_pick_fd;
                rd_ctx_instance<=rd_pick_fd && (fd_instance === 1'b1);
                rd_ctx_bg      <= rd_pick_bg;
                rd_ctx_buf     <= rd_pick_fd ? 2'd0 : rd_buf;
                rd_ctx_cidx    <= rd_pick_fd ? fd_cidx : rd_cidx;
            end else if (rd_busy && rd_gate_done) begin
                rd_busy <= 1'b0;
            end

            if (rd_status_valid) begin
                rd_status_seen <= 1'b1;
                rd_err_r       <= (rd_status_err != 4'd0);
            end
            if (rs_last_fire) rd_stream_done <= 1'b1;

            // 状态脉冲延后一拍出，负载与 valid 对齐（核的状态口没有 tready）
            rd_stat_v <= rd_status_valid;
        end
    end

    // 状态：读核/取数的状态（延后一拍）与 TEX 的状态合并成一路。
    // 单所有者保证两者不同拍；TEX 本地读没有错误来源，err 恒 0。
    wire legacy_rd_stat=rd_stat_v && !rd_ctx_bg;
    assign rd_stat_valid = legacy_rd_stat || tx_stat_valid || b_rstat;
    assign rd_stat_err   = legacy_rd_stat ? rd_err_r : (tx_stat_valid ? 1'b0 : b_rerr);
    assign rd_stat_cidx  = legacy_rd_stat ? rd_ctx_cidx : (tx_stat_valid ? tx_stat_cidx : b_rcidx);
    assign rd_stat_buf   = legacy_rd_stat ? rd_ctx_buf : (tx_stat_valid ? tx_stat_buf : b_rstatbuf);
    assign rd_stat_fetch = legacy_rd_stat && rd_ctx_cmd;
`ifdef INJ_INST_OWNER
    assign fs_instance=1'b0;
    assign rd_stat_instance=1'b0;
`else
    assign fs_instance=rd_ctx_instance;
    assign rd_stat_instance=legacy_rd_stat && rd_ctx_instance;
`endif
    // DDR 状态无反压口；本地状态遇到同拍 DDR 状态时保留到下一拍。
    assign tx_stat_ready = !legacy_rd_stat;
    assign b_rstat_ready = !legacy_rd_stat && !tx_stat_valid;

    // 写侧：门控与排空补拍
    wire        wr_core_ready;   // 写核描述符口 ready
    wire        wr_data_ready;   // 写核写流 ready
    reg         wr_status_seen;
    reg         wr_err_r;
    reg         wr_stat_v;
    reg  [11:0] wr_beats_total;
    reg  [11:0] wr_beats_sent;
    reg [11:0] wr_bus_beats, raw_stop_after;
    reg        raw_stop_valid;
    reg        wr_ctx_pb;
    assign pb_rd_beat = m_axi_rvalid && m_axi_rready && rd_ctx_cmd;
    assign pb_wr_beat = m_axi_wvalid && m_axi_wready && wr_ctx_pb;
    wire pb_request = (pw_valid === 1'b1);

`ifdef INJ_NO_GATE_WR
    wire        wr_gate_done = 1'b1;                   // 注错：去掉写门控
`else
    wire        wr_gate_done = wr_status_seen;
`endif
    wire        wr_block     = (wr_busy && !wr_gate_done) || drain_hold || b_xfer || b_start_fire || b_flush_fire || b_wbusy;
    wire        wr_open      = !wr_block;
    wire        flush_open   = !drain_hold && (!wr_busy || wr_gate_done);
    wire        wr_fire      = wr_core_ready && (b_wr_valid ? flush_open :
                               (wr_open && (pb_request || (wr_valid && !wr_is_band))));
    assign pw_ready=wr_core_ready && wr_open && !b_wr_valid;
    assign      b_wr_ready   = wr_core_ready && flush_open;
    assign      b_wfire      = wr_valid && wr_is_band && b_wready_desc && !wr_busy && !drain_hold && !b_start_fire && !b_flush_fire;
    assign      wr_ready     = wr_is_band ? (b_wready_desc && !wr_busy && !drain_hold && !b_start_fire && !b_flush_fire)
                                         : (wr_core_ready && wr_open && !pb_request);
    wire [31:0] write_desc_addr=b_wr_valid ? b_wr_addr : pb_request?pw_addr:wr_addr;
    wire [11:0] write_desc_bytes=b_wr_valid ? b_wr_bytes : pb_request?pw_bytes:wr_bytes;

    wire [3:0]   wr_status_tag;
    wire [11:0]  wr_status_len;
    wire [3:0]   wr_status_err;
    wire         wr_status_valid;
    wire [127:0] core_wdata;
    wire [15:0]  core_wstrb;

    // 排空补拍：欠多少拍补多少拍，像素标记位全打「不写」；
    // tlast 正常写流与排空同一个式子：剩下的最后一拍
    wire [11:0]  wr_beats_left  = wr_beats_total - wr_beats_sent;
    wire         ws_core_tvalid = wr_drain_help ? (wr_beats_left != 12'd0) :
                                  (wr_ctx_flush ? b_wvalid : (wr_ctx_pb ? pw_data_valid : (ws_tvalid && !b_wbusy)));
`ifdef INJ_NO_TLAST
    // 注错：正常写流不打 tlast，只有排空哑拍打
    wire         ws_core_tlast  = wr_drain_help ? (wr_beats_left == 12'd1) : 1'b0;
`else
    wire         ws_core_tlast  = (wr_beats_left == 12'd1);
`endif
    wire [127:0] ws_core_tdata  = wr_drain_help ? {4{32'h8000_0000}} :
                                  (wr_ctx_flush ? b_wdata : (wr_ctx_pb ? pw_data : ws_tdata));
    // 写核整拍路径不保留输入 keep；BAND 取消掩码必须在 AXI 出口按前缀施加。
    wire [15:0]  ws_core_tkeep  = wr_drain_help ? 16'hFFFF :
                                  ((wr_ctx_flush || wr_ctx_pb) ? 16'hFFFF : ws_tkeep);
    assign       ws_tready      = drain_hold ? 1'b0 : (b_wbusy ? b_wsready : ((wr_ctx_flush || wr_ctx_pb) ? 1'b0 : wr_data_ready));
    assign pw_data_ready=wr_ctx_pb && !wr_drain_help && wr_data_ready;
    assign pw_status=wr_stat_v && wr_ctx_pb;
    assign pw_error=wr_err_r;
    assign       b_wready       = wr_ctx_flush && !wr_drain_help && wr_data_ready;

    wire         ws_core_fire   = ws_core_tvalid && wr_data_ready;
    always @(posedge clk) begin
        if(rst) begin wr_bus_beats<=0;raw_stop_after<=0;raw_stop_valid<=0;end
        else begin
            if(wr_fire) begin wr_bus_beats<=0;raw_stop_valid<=0;end
            else if(m_axi_wvalid && m_axi_wready) wr_bus_beats<=wr_bus_beats+1'b1;
            if(drain_enter && wr_busy && (wr_ctx_flush || wr_ctx_pb)) begin
                raw_stop_valid<=1;raw_stop_after<=wr_beats_sent;
            end
        end
    end
`ifdef INJ_BAND_DRAIN_MASK
    wire raw_drain_kill=1'b0;
`else
    wire raw_drain_kill=(wr_ctx_flush || wr_ctx_pb) && raw_stop_valid && wr_bus_beats>=raw_stop_after;
`endif

    always @(posedge clk) begin
        if (rst) begin
            wr_busy        <= 1'b0;
            wr_status_seen <= 1'b0;
            wr_err_r       <= 1'b0;
            wr_stat_v      <= 1'b0;
            wr_beats_total <= 12'd0;
            wr_beats_sent  <= 12'd0;
            wr_ctx_cidx    <= 4'd0;
            wr_ctx_last_row<= 1'b0;
            wr_ctx_flush   <= 1'b0;
            wr_ctx_pb<=0;
        end else begin
            if (wr_fire) begin
                wr_busy         <= 1'b1;
                wr_status_seen  <= 1'b0;
                wr_beats_total  <= write_desc_bytes >> 4;
                wr_beats_sent   <= 12'd0;
                wr_ctx_cidx     <= wr_cidx;
                wr_ctx_last_row <= wr_last_row;
                wr_ctx_flush    <= b_wr_valid;
                wr_ctx_pb<=!b_wr_valid && pb_request;
            end else if (wr_busy && wr_gate_done) begin
                wr_busy <= 1'b0;
            end

            if (ws_core_fire) wr_beats_sent <= wr_beats_sent + 12'd1;

            if (wr_status_valid) begin
                wr_status_seen <= 1'b1;
                wr_err_r       <= (wr_status_err != 4'd0);
            end
            wr_stat_v <= wr_status_valid;
        end
    end

    wire legacy_wr_stat=wr_stat_v && !wr_ctx_flush && !wr_ctx_pb;
    assign wr_stat_valid    = legacy_wr_stat || b_wstat;
    assign wr_stat_err      = legacy_wr_stat ? wr_err_r : b_werr;
    assign wr_stat_cidx     = legacy_wr_stat ? wr_ctx_cidx : b_wcidx;
    assign wr_stat_last_row = legacy_wr_stat ? wr_ctx_last_row : b_wlast;
    assign b_wstat_ready    = !legacy_wr_stat;

    // 超时：只报告、不放弃
    reg  [TMO_WIDTH-1:0] rd_tmo, wr_tmo;
    reg                  rd_tmo_rep, wr_tmo_rep;

    // 「握手」两侧都算：本层一侧与 AXI 一侧；TEX 的描述符/流/状态算读方向（G1）
    wire rd_axi_hs = (m_axi_arvalid && m_axi_arready) || (m_axi_rvalid && m_axi_rready);
    wire wr_axi_hs = (m_axi_awvalid && m_axi_awready) || (m_axi_wvalid && m_axi_wready)
                  || (m_axi_bvalid && m_axi_bready);
    wire rd_hs     = rd_fire || tx_fire || rs_fire || tx_rs_tvalid
                   || rd_status_valid || tx_stat_valid || rd_axi_hs || b_rfire || b_rsvalid || b_rstat;
    wire wr_hs     = wr_fire || ws_core_fire || wr_status_valid || wr_axi_hs || b_wfire ||
                     (ws_tvalid && b_wsready && b_wbusy) || b_wstat;
    wire any_read_busy=rd_busy || tx_busy || b_rbusy || b_loading;
    wire any_write_busy=wr_busy || b_wbusy || b_flushing;

    wire rd_over   = any_read_busy && (rd_tmo >= tmo_limit);
    wire wr_over   = any_write_busy && (wr_tmo >= tmo_limit);
    wire rd_over_evt = rd_over && !rd_tmo_rep;
    wire wr_over_evt = wr_over && !wr_tmo_rep;

    always @(posedge clk) begin
        if (rst || err_clr || !any_read_busy || rd_hs) begin
            rd_tmo <= {TMO_WIDTH{1'b0}};
        end else if (!rd_over) begin
            rd_tmo <= rd_tmo + 1'b1;
        end
    end

    always @(posedge clk) begin
        if (rst || err_clr || !any_write_busy || wr_hs) begin
            wr_tmo <= {TMO_WIDTH{1'b0}};
        end else if (!wr_over) begin
            wr_tmo <= wr_tmo + 1'b1;
        end
    end

    always @(posedge clk) begin
        if (rst || err_clr || !any_read_busy)     rd_tmo_rep <= 1'b0;
        else if (rd_over)                                 rd_tmo_rep <= 1'b1;
    end

    always @(posedge clk) begin
        if (rst || err_clr || !any_write_busy)     wr_tmo_rep <= 1'b0;
        else if (wr_over)                   wr_tmo_rep <= 1'b1;
    end

    assign tmo_evt = rd_over_evt || wr_over_evt;
    assign tmo_dir = wr_over_evt;

    // 两个核（源码不改）
    axi_dma_rd #(
        .AXI_DATA_WIDTH(128), .AXI_ADDR_WIDTH(32), .AXI_ID_WIDTH(4),
        .AXI_MAX_BURST_LEN(AXI_MAX_BURST_LEN),
        .AXIS_DATA_WIDTH(128), .AXIS_KEEP_ENABLE(1), .AXIS_LAST_ENABLE(1),
        .AXIS_ID_ENABLE(0), .AXIS_DEST_ENABLE(0), .AXIS_USER_ENABLE(0),
        .LEN_WIDTH(12), .TAG_WIDTH(4), .ENABLE_SG(0), .ENABLE_UNALIGNED(0)
    ) u_rd (
        .clk(clk), .rst(rst),
        .s_axis_read_desc_addr(dsc_addr), .s_axis_read_desc_len(dsc_bytes),
        .s_axis_read_desc_tag(4'd0),
        .s_axis_read_desc_id(8'd0), .s_axis_read_desc_dest(8'd0), .s_axis_read_desc_user(1'b0),
        .s_axis_read_desc_valid(rd_fire), .s_axis_read_desc_ready(rd_core_ready),
        .m_axis_read_desc_status_tag(rd_status_tag),
        .m_axis_read_desc_status_error(rd_status_err),
        .m_axis_read_desc_status_valid(rd_status_valid),
        .m_axis_read_data_tdata(core_rs_tdata), .m_axis_read_data_tkeep(core_rs_tkeep),
        .m_axis_read_data_tvalid(core_rs_tvalid), .m_axis_read_data_tready(core_rs_tready),
        .m_axis_read_data_tlast(core_rs_tlast),
        .m_axis_read_data_tid(), .m_axis_read_data_tdest(), .m_axis_read_data_tuser(),
        .m_axi_arid(), .m_axi_araddr(m_axi_araddr), .m_axi_arlen(m_axi_arlen),
        .m_axi_arsize(m_axi_arsize), .m_axi_arburst(m_axi_arburst),
        .m_axi_arlock(), .m_axi_arcache(), .m_axi_arprot(),
        .m_axi_arvalid(m_axi_arvalid), .m_axi_arready(m_axi_arready),
        .m_axi_rid(4'd0), .m_axi_rdata(m_axi_rdata), .m_axi_rresp(m_axi_rresp),
        .m_axi_rlast(m_axi_rlast), .m_axi_rvalid(m_axi_rvalid), .m_axi_rready(m_axi_rready),
        .enable(1'b1)
    );

    axi_dma_wr #(
        .AXI_DATA_WIDTH(128), .AXI_ADDR_WIDTH(32), .AXI_ID_WIDTH(4),
        .AXI_MAX_BURST_LEN(AXI_MAX_BURST_LEN),
        .AXIS_DATA_WIDTH(128), .AXIS_KEEP_ENABLE(1), .AXIS_LAST_ENABLE(1),
        .AXIS_ID_ENABLE(0), .AXIS_DEST_ENABLE(0), .AXIS_USER_ENABLE(0),
        .LEN_WIDTH(12), .TAG_WIDTH(4), .ENABLE_SG(0), .ENABLE_UNALIGNED(0)
    ) u_wr (
        .clk(clk), .rst(rst),
        .s_axis_write_desc_addr(write_desc_addr), .s_axis_write_desc_len(write_desc_bytes),
        .s_axis_write_desc_tag(4'd0),
        .s_axis_write_desc_valid(wr_fire), .s_axis_write_desc_ready(wr_core_ready),
        .m_axis_write_desc_status_len(wr_status_len), .m_axis_write_desc_status_tag(wr_status_tag),
        .m_axis_write_desc_status_id(), .m_axis_write_desc_status_dest(),
        .m_axis_write_desc_status_user(),
        .m_axis_write_desc_status_error(wr_status_err),
        .m_axis_write_desc_status_valid(wr_status_valid),
        .s_axis_write_data_tdata(ws_core_tdata), .s_axis_write_data_tkeep(ws_core_tkeep),
        .s_axis_write_data_tvalid(ws_core_tvalid), .s_axis_write_data_tready(wr_data_ready),
        .s_axis_write_data_tlast(ws_core_tlast),
        .s_axis_write_data_tid(8'd0), .s_axis_write_data_tdest(8'd0),
        .s_axis_write_data_tuser(1'b0),
        .m_axi_awid(), .m_axi_awaddr(m_axi_awaddr), .m_axi_awlen(m_axi_awlen),
        .m_axi_awsize(m_axi_awsize), .m_axi_awburst(m_axi_awburst),
        .m_axi_awlock(), .m_axi_awcache(), .m_axi_awprot(),
        .m_axi_awvalid(m_axi_awvalid), .m_axi_awready(m_axi_awready),
        .m_axi_wdata(core_wdata), .m_axi_wstrb(core_wstrb), .m_axi_wlast(m_axi_wlast),
        .m_axi_wvalid(m_axi_wvalid), .m_axi_wready(m_axi_wready),
        .m_axi_bid(4'd0), .m_axi_bresp(m_axi_bresp), .m_axi_bvalid(m_axi_bvalid),
        .m_axi_bready(m_axi_bready),
        .enable(1'b1), .abort(1'b0)
    );

    // W 通道带内掩码
    genvar gp;
    generate
    for (gp = 0; gp < 4; gp = gp + 1) begin : g_wrap
`ifdef INJ_MASK_BYPASS
        // 注错：带内掩码旁路，wstrb 与 X 字节直通上总线
        wire kill = 1'b0;
        assign m_axi_wstrb[4*gp +: 4]     = core_wstrb[4*gp +: 4];
        assign m_axi_wdata[32*gp +: 24]   = core_wdata[32*gp +: 24];
        assign m_axi_wdata[32*gp+24 +: 8] = core_wdata[32*gp+24 +: 8];
`else
`ifdef INJ_BAND_RAW_MASK
        wire kill = core_wdata[32*gp + 31];
`else
        wire kill = raw_drain_kill || (!(wr_ctx_flush || wr_ctx_pb) && core_wdata[32*gp + 31]);
`endif
        assign m_axi_wstrb[4*gp +: 4]     = core_wstrb[4*gp +: 4] & {4{~kill}};
        assign m_axi_wdata[32*gp +: 24]   = core_wdata[32*gp +: 24];
`ifdef INJ_BAND_RAW_MASK
        assign m_axi_wdata[32*gp+24 +: 8] = 8'h00;
`else
        assign m_axi_wdata[32*gp+24 +: 8] = (wr_ctx_flush || wr_ctx_pb) ? core_wdata[32*gp+24 +:8] : 8'h00;
`endif
`endif
    end
    endgenerate

endmodule

`default_nettype wire
