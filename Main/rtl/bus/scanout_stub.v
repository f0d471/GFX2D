// 扫描输出替身（实验 E5 的 2.3 节）
//
// 引擎与扫描输出都还不存在，但它们在总线上的正常负载可以用替身先量出来。
// 本模块是扫描输出在 AXI 上的替身：按行节拍对 DDR3 发一笔 160 拍（128 位，640 像素）的读，
// 并记录三类时间，用于 2.3 节的板上量测与「直方图里没有等两笔引擎事务的样本」这条判据。
//
// 它只发起读，写通道（AW/W/B）由顶层接成不发起。
// 每个方向在途事务不超过 1：上一笔的 rlast 握手完成之前不发下一笔 arvalid（E5 2.2 节措施一）。
//
// 挂在 SoC 的 APB Slave 0 下，独占窗口 0xF810_E000~0xF810_EFFF（CPU 地址），
// 顶层按 paddr[15:12]==4'hE 译码，窗口内 12 位偏移：
//
//   偏移    名称            读写  复位值        内容
//   0x000   ID              只读  32'h5343_414E  ASCII "SCAN"
//   0x004   VERSION         只读  1             寄存器接口版本
//   0x008   CTRL            读写  0             bit0 ENABLE，bit1 CLEAR（写 1 清全部统计）
//   0x00C   BASE            读写  0x0200_0000   读的起始地址（帧缓冲 0，见设计文档 2.1 节分区）
//   0x010   LINE_PERIOD     读写  3175          行周期，单位 core_clk 周期（31.7 微秒）
//   0x014   FIELD_LINES     读写  525           场行数
//   0x018   MAX_FIRST_WAIT  只读  0             AR 握手到首个 R 拍的最大周期数
//   0x01C   MAX_FULL_TXN    只读  0             AR 握手到 rlast 的最大周期数
//   0x020   MAX_R_GAP       只读  0             相邻 R 拍之间的最大周期数
//   0x024   TXN_COUNT       只读  0             完成的读事务数
//   0x028   LINE_COUNT      只读  0             场内的行序号（0~FIELD_LINES-1）
//   0x02C   R_BEAT_COUNT    只读  0             收到的 R 拍数
//   0x030   SKIP_COUNT      只读  0             该发行时上一笔还没完成而跳过的行数
//   0x034   RESPERR_COUNT   只读  0             非 OKAY 读响应计数
//   0x038   STATUS          只读  0             bit0 BUSY，bit1 AR_PENDING
//   0x03C   HIST_FIRST[12]  只读  0             首拍等待的直方图
//   0x06C   HIST_FULL[12]   只读  0             整笔时间的直方图
//   0x09C   HIST_GAP[12]    只读  0             R 拍间隙的直方图
//
// 直方图分桶：桶 k 计 [2^k, 2^(k+1)-1]，桶 11 计 >=2048；0 与 1 同落桶 0。
// 计数约定：时间量的单位都是 core_clk 周期，从 AR 握手那一拍算起；
// 握手后下一拍就返回数据的记 0。R 拍间隙按相邻两次 rvalid 握手之间的周期数记。
`timescale 1ns/1ps
`default_nettype none

module scanout_stub #(
    parameter [31:0] ID      = 32'h5343_414E,
    parameter [31:0] VERSION = 32'h0000_0001
)(
    input  wire        clk,
    input  wire        rst,        // 同步复位，高有效
    // APB 从口，paddr 只取窗口内偏移
    input  wire [11:0] paddr,
    input  wire        psel,
    input  wire        penable,
    input  wire        pwrite,
    input  wire [31:0] pwdata,
    output reg  [31:0] prdata,
    output wire        pready,
    output wire        pslverr,

    // AXI 读主机口（接 Axi_Mux 的 S0；写通道由顶层接成不发起）
    output reg  [31:0]  ar_addr,
    output wire [7:0]   ar_len,
    output wire [2:0]   ar_size,
    output wire [1:0]   ar_burst,
    output wire [1:0]   ar_lock,
    output wire [7:0]   ar_id,
    output reg          ar_valid,
    input  wire         ar_ready,

    input  wire [7:0]   r_id,
    input  wire [127:0] r_data,
    input  wire         r_last,
    input  wire [1:0]   r_resp,
    input  wire         r_valid,
    output wire         r_ready
);

localparam [11:0] ADDR_ID        = 12'h000;
localparam [11:0] ADDR_VERSION   = 12'h004;
localparam [11:0] ADDR_CTRL      = 12'h008;
localparam [11:0] ADDR_BASE      = 12'h00C;
localparam [11:0] ADDR_LINE_PER  = 12'h010;
localparam [11:0] ADDR_FIELD     = 12'h014;
localparam [11:0] ADDR_MAX_FIRST = 12'h018;
localparam [11:0] ADDR_MAX_FULL  = 12'h01C;
localparam [11:0] ADDR_MAX_GAP   = 12'h020;
localparam [11:0] ADDR_TXN       = 12'h024;
localparam [11:0] ADDR_LINE_CNT  = 12'h028;
localparam [11:0] ADDR_BEAT_CNT  = 12'h02C;
localparam [11:0] ADDR_SKIP      = 12'h030;
localparam [11:0] ADDR_RESPERR   = 12'h034;
localparam [11:0] ADDR_STATUS    = 12'h038;
localparam [11:0] ADDR_HIST_FIRST= 12'h03C;   // 12 个
localparam [11:0] ADDR_HIST_FULL = 12'h06C;   // 12 个
localparam [11:0] ADDR_HIST_GAP  = 12'h09C;   // 12 个

// 一行 640 像素 XRGB8888 = 2560 字节
localparam [31:0] LINE_BYTES = 32'd2560;

// 配置寄存器
reg        cfg_enable;
reg [31:0] cfg_base;
reg [31:0] cfg_line_period;
reg [31:0] cfg_field_lines;

// 统计寄存器（每个寄存器只在一个 always 块里赋值）
reg [31:0] stat_max_first;
reg [31:0] stat_max_full;
reg [31:0] stat_max_gap;
reg [31:0] stat_resperr;
reg [31:0] hist_first [0:11];
reg [31:0] hist_full  [0:11];
reg [31:0] hist_gap   [0:11];

reg [31:0] stat_txn;
reg [31:0] stat_beat;

reg [31:0] stat_line;
reg [31:0] stat_skip;

// 运行状态
reg [31:0] line_cnt;     // 行内周期计数 0~LINE_PERIOD-1
reg [31:0] row_idx;      // 场内行序号 0~FIELD_LINES-1
reg [31:0] row_addr;     // 本行的读起始地址
reg [31:0] base_d;       // cfg_base 的上一拍值，用于检测改基址
reg        busy;         // 一笔读已发行、还没到 rlast
reg        got_first;    // 本笔已收到首个 R 拍
reg [31:0] c_first;      // 首拍等待
reg [31:0] c_full;       // 整笔时间
reg [31:0] c_gap;        // R 拍间隙

integer i;

wire access = psel & penable;

wire wr_legal = (paddr == ADDR_CTRL) | (paddr == ADDR_BASE) |
                (paddr == ADDR_LINE_PER) | (paddr == ADDR_FIELD);
wire stat_clear = access & pwrite & (paddr == ADDR_CTRL) & pwdata[1];

wire [31:0] field_lines = (cfg_field_lines == 32'h0) ? 32'd1 : cfg_field_lines;

// 直方图分桶
function [3:0] bucket;
    input [31:0] v;
    begin
        if      (v >= 32'd2048) bucket = 4'd11;
        else if (v >= 32'd1024) bucket = 4'd10;
        else if (v >= 32'd512)  bucket = 4'd9;
        else if (v >= 32'd256)  bucket = 4'd8;
        else if (v >= 32'd128)  bucket = 4'd7;
        else if (v >= 32'd64)   bucket = 4'd6;
        else if (v >= 32'd32)   bucket = 4'd5;
        else if (v >= 32'd16)   bucket = 4'd4;
        else if (v >= 32'd8)    bucket = 4'd3;
        else if (v >= 32'd4)    bucket = 4'd2;
        else if (v >= 32'd2)    bucket = 4'd1;
        else                    bucket = 4'd0;
    end
endfunction

// ---------------------------------------------------------------------------
// 读通路（access 阶段组合输出）
// ---------------------------------------------------------------------------
reg hit;

always @(*) begin
    hit = 1'b1;
    if (paddr == ADDR_ID)             prdata = ID;
    else if (paddr == ADDR_VERSION)   prdata = VERSION;
    else if (paddr == ADDR_CTRL)      prdata = {31'h0, cfg_enable};
    else if (paddr == ADDR_BASE)      prdata = cfg_base;
    else if (paddr == ADDR_LINE_PER)  prdata = cfg_line_period;
    else if (paddr == ADDR_FIELD)     prdata = cfg_field_lines;
    else if (paddr == ADDR_MAX_FIRST) prdata = stat_max_first;
    else if (paddr == ADDR_MAX_FULL)  prdata = stat_max_full;
    else if (paddr == ADDR_MAX_GAP)   prdata = stat_max_gap;
    else if (paddr == ADDR_TXN)       prdata = stat_txn;
    else if (paddr == ADDR_LINE_CNT)  prdata = stat_line;
    else if (paddr == ADDR_BEAT_CNT)  prdata = stat_beat;
    else if (paddr == ADDR_SKIP)      prdata = stat_skip;
    else if (paddr == ADDR_RESPERR)   prdata = stat_resperr;
    else if (paddr == ADDR_STATUS)    prdata = {30'h0, ar_valid, busy};
    else if ((paddr >= ADDR_HIST_FIRST) && (paddr < ADDR_HIST_FIRST + 12*4))
        prdata = hist_first[(paddr - ADDR_HIST_FIRST) >> 2];
    else if ((paddr >= ADDR_HIST_FULL) && (paddr < ADDR_HIST_FULL + 12*4))
        prdata = hist_full[(paddr - ADDR_HIST_FULL) >> 2];
    else if ((paddr >= ADDR_HIST_GAP) && (paddr < ADDR_HIST_GAP + 12*4))
        prdata = hist_gap[(paddr - ADDR_HIST_GAP) >> 2];
    else begin
        prdata = 32'h0;
        hit    = 1'b0;
    end
end

assign pready  = 1'b1;
assign pslverr = access & (pwrite ? ~wr_legal : ~hit);

// ---------------------------------------------------------------------------
// AXI 读
// ---------------------------------------------------------------------------
assign ar_len   = 8'd159;      // 160 拍
assign ar_size  = 3'b100;      // 16 字节 = 128 位
assign ar_burst = 2'b01;       // INCR
assign ar_lock  = 2'b00;
assign ar_id    = 8'h00;
assign r_ready  = 1'b1;        // 替身不做像素域反压，只量总线侧时间

wire line_tick = (line_cnt == 32'h0);
wire ar_fire   = ar_valid & ar_ready;
wire r_fire    = r_valid & r_ready;
wire txn_end   = r_fire & r_last;

// 每次测量的分桶结果（组合），供统计块使用
wire [11:0] dec_first = 12'b1 << bucket(c_first);
wire [11:0] dec_gap   = 12'b1 << bucket(c_gap);
wire [11:0] dec_full  = 12'b1 << bucket(c_full);

// ---------------------------------------------------------------------------
// 主体：行节拍、发行、计时
// ---------------------------------------------------------------------------
always @(posedge clk) begin
    if (rst) begin
        ar_valid <= 1'b0;
        ar_addr  <= 32'h0;
        busy     <= 1'b0;
        got_first<= 1'b0;
        c_first  <= 32'h0;
        c_full   <= 32'h0;
        c_gap    <= 32'h0;
        line_cnt <= 32'h0;
        row_idx  <= 32'h0;
        row_addr <= 32'h0;
        base_d   <= 32'h0;
    end else begin
        base_d <= cfg_base;

        // 行内计数
        if (cfg_line_period == 32'h0)
            line_cnt <= 32'h0;
        else if (line_cnt + 32'h1 >= cfg_line_period)
            line_cnt <= 32'h0;
        else
            line_cnt <= line_cnt + 32'h1;

        // 行节拍：发行本行读，并把行地址推进一行
        if (line_tick) begin
            if (cfg_enable & ~ar_valid & ~busy) begin
                ar_valid <= 1'b1;
                ar_addr  <= row_addr;
            end
            if (row_idx + 32'h1 >= field_lines) begin
                row_idx  <= 32'h0;
                row_addr <= cfg_base;
            end else begin
                row_idx  <= row_idx + 32'h1;
                row_addr <= row_addr + LINE_BYTES;
            end
        end

        // 基址被改写：下一拍把行地址拉回新基址
        if (cfg_base != base_d) begin
            row_idx  <= 32'h0;
            row_addr <= cfg_base;
        end

        if (ar_fire) begin
            ar_valid  <= 1'b0;
            busy      <= 1'b1;
            got_first <= 1'b0;
            c_first   <= 32'h0;
            c_full    <= 32'h0;
            c_gap     <= 32'h0;
        end

        // 计时
        if (busy) begin
            c_full <= c_full + 32'h1;
            if (~got_first)
                c_first <= c_first + 32'h1;
            else
                c_gap <= c_gap + 32'h1;
        end

        if (r_fire) begin
            if (~got_first) begin
                got_first <= 1'b1;
                c_gap     <= 32'h0;
            end else
                c_gap <= 32'h0;

            if (r_last)
                busy <= 1'b0;
        end
    end
end

// ---------------------------------------------------------------------------
// 统计：最大值与直方图（一次测量在这里落账）
// ---------------------------------------------------------------------------
always @(posedge clk) begin
    if (rst | stat_clear) begin
        stat_max_first <= 32'h0;
        stat_max_full  <= 32'h0;
        stat_max_gap   <= 32'h0;
        stat_resperr   <= 32'h0;
        for (i = 0; i < 12; i = i + 1) begin
            hist_first[i] <= 32'h0;
            hist_full[i]  <= 32'h0;
            hist_gap[i]   <= 32'h0;
        end
    end else begin
        if (r_fire) begin
            if (r_resp != 2'b00)
                stat_resperr <= stat_resperr + 32'h1;

            if (~got_first) begin
                // 首个 R 拍：首拍等待
                if (c_first > stat_max_first) stat_max_first <= c_first;
                for (i = 0; i < 12; i = i + 1)
                    if (dec_first[i]) hist_first[i] <= hist_first[i] + 32'h1;
            end else begin
                // 后续 R 拍：与前一拍之间的间隙
                if (c_gap > stat_max_gap) stat_max_gap <= c_gap;
                for (i = 0; i < 12; i = i + 1)
                    if (dec_gap[i]) hist_gap[i] <= hist_gap[i] + 32'h1;
            end

            if (r_last) begin
                // 整笔时间
                if (c_full > stat_max_full) stat_max_full <= c_full;
                for (i = 0; i < 12; i = i + 1)
                    if (dec_full[i]) hist_full[i] <= hist_full[i] + 32'h1;
            end
        end
    end
end

// ---------------------------------------------------------------------------
// 计数：事务数、R 拍数、行序号、跳过行
// ---------------------------------------------------------------------------
always @(posedge clk) begin
    if (rst | stat_clear) begin
        stat_txn  <= 32'h0;
        stat_beat <= 32'h0;
        stat_line <= 32'h0;
        stat_skip <= 32'h0;
    end else begin
        if (txn_end)
            stat_txn <= stat_txn + 32'h1;
        if (r_fire)
            stat_beat <= stat_beat + 32'h1;

        if (line_tick & cfg_enable) begin
            if (row_idx + 32'h1 >= field_lines)
                stat_line <= 32'h0;
            else
                stat_line <= stat_line + 32'h1;
            if (ar_valid | busy)
                stat_skip <= stat_skip + 32'h1;
        end
    end
end

// ---------------------------------------------------------------------------
// 配置寄存器写
// ---------------------------------------------------------------------------
always @(posedge clk) begin
    if (rst) begin
        cfg_enable      <= 1'b0;
        cfg_base        <= 32'h0200_0000;
        cfg_line_period <= 32'd3175;
        cfg_field_lines <= 32'd525;
    end else if (access & pwrite) begin
        case (paddr)
            ADDR_CTRL:      cfg_enable      <= pwdata[0];
            ADDR_BASE:      cfg_base        <= pwdata;
            ADDR_LINE_PER:  cfg_line_period <= pwdata;
            ADDR_FIELD:     cfg_field_lines <= pwdata;
            default: ;
        endcase
    end
end

endmodule

`default_nettype wire
