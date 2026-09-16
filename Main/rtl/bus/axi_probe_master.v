// AXI 探针主机：由 APB 配置，按固定参数连续发出 AXI4 读或写事务，测量 DDR3 控制器的
// 突发上限、每笔事务的固定开销与持续吞吐（实验 E4），也用作 CPU 之外的第二个 DDR 观测者。
//
// 挂在 APB Slave 0 的 0x1000~0x1FFF（CPU 地址 0xF810_1000 起），寄存器偏移相对 0x1000：
//
//   偏移   名称      读写  内容
//   0x00   CTRL      写    bit0 START：按当前配置开始一轮；bit1 ABORT：立即回到空闲（会撤销 valid，只用于救场）
//   0x04   STATUS    只读  bit0 BUSY；bit1 DONE；bit2 TIMEOUT；bit3 RESP_ERR（本轮出现过非 OKAY 响应）；
//                          bit4 LAST_ERR（读突发的 rlast 位置不对）
//   0x08   ADDR      读写  第一笔事务的起始地址，应为 16 的倍数
//   0x0C   LEN       读写  [7:0] 送到 awlen/arlen，每笔事务 LEN+1 拍，每拍 128 位
//   0x10   COUNT     读写  本轮事务数
//   0x14   MODE      读写  bit0 WRITE（1 写 0 读）；bit1 INC（每笔之后地址加 (LEN+1)*16）；bit2 CHECK（读时比对图样）
//   0x18   SEED      读写  数据图样种子
//   0x1C   TIMEOUT   读写  连续这么多个周期没有任何握手即判超时，复位值 0x0010_0000
//   0x20   TX_DONE   只读  已完成的事务数
//   0x24   CYCLES    只读  从 START 到最后一笔完成的周期数
//   0x28   LAT_SUM   只读  每笔事务从地址 valid 拉高到第一个读数据拍（读）或写响应（写）的周期数之和
//   0x2C   ERRS      只读  [15:0] 非 OKAY 响应计数；[31:16] 读比对失配的拍数
//   0x30   RDATA0    只读  最近一笔读事务第一拍的 [31:0]
//   0x34   RDATA1    只读  最近一笔读事务第一拍的 [63:32]
//   0x38   ARB       读写  bit0 送 Axi_Mux 的 Cfg_Arb_Type；bit[2:1] 送 Cfg_Cnl_Mask，复位值 3'b110
//   0x3C   ID        只读  32'h4158_4950（ASCII "AXIP"）
//   其余           读回 0，读写都置 PSLVERROR
//
// 数据图样：地址为 a 的那一拍，v = SEED + a[31:0]，拍数据 = {v^A5A5A5A5, v^5A5A5A5A, ~v, v}。
// AXI 是小端，CPU 在地址 a 读到的 32 位字就是 v，在 a+4 读到 ~v，便于 CPU 侧核对。
//
// 超时之后保持 valid 不撤销（AXI 规定 valid 拉高后必须等到握手），只停止计数并置 TIMEOUT。
// 此时 Axi_Mux 仍被本主机占着，CPU 也访问不了 DDR，只能 ABORT 或重新加载比特流。
`timescale 1ns/1ps
`default_nettype none

module axi_probe_master (
    input  wire         clk,
    input  wire         rst,            // 同步复位，高有效

    // APB 从口，paddr 只取窗口内偏移
    input  wire [11:0]  paddr,
    input  wire         psel,
    input  wire         penable,
    input  wire         pwrite,
    input  wire [31:0]  pwdata,
    output reg  [31:0]  prdata,
    output wire         pready,
    output wire         pslverr,

    // 给 Axi_Mux 的配置
    output wire         arb_type,
    output wire [1:0]   arb_mask,

    // AXI4 主机口
    output reg  [31:0]  awaddr,
    output wire [7:0]   awlen,
    output wire [2:0]   awsize,
    output wire [1:0]   awburst,
    output reg          awvalid,
    input  wire         awready,

    output wire [127:0] wdata,
    output wire [15:0]  wstrb,
    output wire         wlast,
    output reg          wvalid,
    input  wire         wready,

    input  wire [1:0]   bresp,
    input  wire         bvalid,
    output wire         bready,

    output reg  [31:0]  araddr,
    output wire [7:0]   arlen,
    output wire [2:0]   arsize,
    output wire [1:0]   arburst,
    output reg          arvalid,
    input  wire         arready,

    input  wire [127:0] rdata,
    input  wire [1:0]   rresp,
    input  wire         rlast,
    input  wire         rvalid,
    output wire         rready
);

localparam [31:0] ID_VALUE = 32'h4158_4950;

// ---------------------------------------------------------------------------
// 配置寄存器
// ---------------------------------------------------------------------------
reg [31:0] cfg_addr;
reg [7:0]  cfg_len;
reg [31:0] cfg_count;
reg        cfg_write, cfg_inc, cfg_check;
reg [31:0] cfg_seed;
reg [31:0] cfg_timeout;
reg [2:0]  cfg_arb;

// 状态与结果
reg        st_done, st_timeout, st_resp_err, st_last_err;
reg [31:0] r_tx_done, r_cycles, r_lat_sum;
reg [15:0] r_resp_errs, r_mismatch;
reg [31:0] r_rdata0, r_rdata1;

localparam [2:0] S_IDLE = 3'd0,
                 S_NEXT = 3'd1,
                 S_WR   = 3'd2,
                 S_RD   = 3'd3,
                 S_HANG = 3'd4;
reg [2:0] state;

wire busy = (state != S_IDLE) && (state != S_HANG);

// ---------------------------------------------------------------------------
// APB
// ---------------------------------------------------------------------------
wire access = psel & penable;
reg  rd_hit;
reg  wr_hit;

always @(*) begin
    rd_hit = 1'b1;
    case (paddr)
        12'h004: prdata = {27'h0, st_last_err, st_resp_err, st_timeout, st_done, busy};
        12'h008: prdata = cfg_addr;
        12'h00C: prdata = {24'h0, cfg_len};
        12'h010: prdata = cfg_count;
        12'h014: prdata = {29'h0, cfg_check, cfg_inc, cfg_write};
        12'h018: prdata = cfg_seed;
        12'h01C: prdata = cfg_timeout;
        12'h020: prdata = r_tx_done;
        12'h024: prdata = r_cycles;
        12'h028: prdata = r_lat_sum;
        12'h02C: prdata = {r_mismatch, r_resp_errs};
        12'h030: prdata = r_rdata0;
        12'h034: prdata = r_rdata1;
        12'h038: prdata = {29'h0, cfg_arb};
        12'h03C: prdata = ID_VALUE;
        default: begin
            prdata = 32'h0;
            rd_hit = 1'b0;
        end
    endcase
end

always @(*) begin
    case (paddr)
        12'h000, 12'h008, 12'h00C, 12'h010, 12'h014,
        12'h018, 12'h01C, 12'h038: wr_hit = 1'b1;
        default:                   wr_hit = 1'b0;
    endcase
end

assign pready  = 1'b1;
assign pslverr = access & (pwrite ? ~wr_hit : ~rd_hit);

wire apb_wr    = access & pwrite & wr_hit;
wire cmd_start = apb_wr & (paddr == 12'h000) & pwdata[0];
wire cmd_abort = apb_wr & (paddr == 12'h000) & pwdata[1];

// 配置只在空闲时可写，避免一轮进行中参数被改
always @(posedge clk) begin
    if (rst) begin
        cfg_addr    <= 32'h0;
        cfg_len     <= 8'h0;
        cfg_count   <= 32'h0;
        cfg_write   <= 1'b0;
        cfg_inc     <= 1'b0;
        cfg_check   <= 1'b0;
        cfg_seed    <= 32'h0;
        cfg_timeout <= 32'h0010_0000;
        cfg_arb     <= 3'b110;
    end else if (apb_wr) begin
        case (paddr)
            12'h008: if (!busy) cfg_addr    <= pwdata;
            12'h00C: if (!busy) cfg_len     <= pwdata[7:0];
            12'h010: if (!busy) cfg_count   <= pwdata;
            12'h014: if (!busy) {cfg_check, cfg_inc, cfg_write} <= pwdata[2:0];
            12'h018: if (!busy) cfg_seed    <= pwdata;
            12'h01C: if (!busy) cfg_timeout <= pwdata;
            12'h038:            cfg_arb     <= pwdata[2:0];
            default: ;
        endcase
    end
end

assign arb_type = cfg_arb[0];
assign arb_mask = cfg_arb[2:1];

// ---------------------------------------------------------------------------
// AXI 固定字段
// ---------------------------------------------------------------------------
assign awlen   = cfg_len;
assign arlen   = cfg_len;
assign awsize  = 3'd4;          // 16 字节一拍
assign arsize  = 3'd4;
assign awburst = 2'b01;         // INCR
assign arburst = 2'b01;
assign wstrb   = 16'hFFFF;
assign bready  = 1'b1;
assign rready  = 1'b1;

// ---------------------------------------------------------------------------
// 数据图样
// ---------------------------------------------------------------------------
function [127:0] pattern;
    input [31:0] seed;
    input [31:0] a;
    reg   [31:0] v;
    begin
        v = seed + a;
        pattern = {v ^ 32'hA5A5_A5A5, v ^ 32'h5A5A_5A5A, ~v, v};
    end
endfunction

reg  [31:0] beat_addr;          // 当前拍的地址
reg  [7:0]  beat_idx;           // 当前事务内的拍序号
reg  [31:0] tx_left;
reg  [31:0] cur_addr;           // 当前事务起始地址
reg  [31:0] t_start;            // 当前事务地址 valid 拉高时的 r_cycles
reg         aw_done, w_done;
reg         first_rbeat;
reg  [31:0] wdog;

assign wdata = pattern(cfg_seed, beat_addr);
assign wlast = (beat_idx == cfg_len);

wire aw_fire = awvalid & awready;
wire w_fire  = wvalid & wready;
wire b_fire  = bvalid & bready;
wire ar_fire = arvalid & arready;
wire r_fire  = rvalid & rready;
wire any_hs  = aw_fire | w_fire | b_fire | ar_fire | r_fire;

wire [31:0] step = ({24'h0, cfg_len} + 32'd1) << 4;

// 读比对打一拍，128 位比较不和 rvalid 挤在同一级
reg         chk_valid;
reg [127:0] chk_got, chk_exp;

always @(posedge clk) begin
    if (rst | cmd_abort) begin
        state       <= S_IDLE;
        awvalid     <= 1'b0;
        wvalid      <= 1'b0;
        arvalid     <= 1'b0;
        chk_valid   <= 1'b0;
        if (rst) begin
            st_done     <= 1'b0;
            st_timeout  <= 1'b0;
            st_resp_err <= 1'b0;
            st_last_err <= 1'b0;
            r_tx_done   <= 32'h0;
            r_cycles    <= 32'h0;
            r_lat_sum   <= 32'h0;
            r_resp_errs <= 16'h0;
            r_mismatch  <= 16'h0;
            r_rdata0    <= 32'h0;
            r_rdata1    <= 32'h0;
        end
    end else begin
        // 读比对的第二级
        chk_valid <= 1'b0;
        if (chk_valid && (chk_got != chk_exp) && (r_mismatch != 16'hFFFF))
            r_mismatch <= r_mismatch + 16'd1;

        if (busy)
            r_cycles <= r_cycles + 32'd1;

        if (busy && (state == S_WR || state == S_RD)) begin
            if (any_hs)
                wdog <= 32'h0;
            else if (wdog == cfg_timeout) begin
                st_timeout <= 1'b1;
                st_done    <= 1'b1;
                state      <= S_HANG;
            end else
                wdog <= wdog + 32'd1;
        end

        case (state)
        S_IDLE, S_HANG: begin
            if (cmd_start && state == S_IDLE) begin
                st_done     <= 1'b0;
                st_timeout  <= 1'b0;
                st_resp_err <= 1'b0;
                st_last_err <= 1'b0;
                r_tx_done   <= 32'h0;
                r_cycles    <= 32'h0;
                r_lat_sum   <= 32'h0;
                r_resp_errs <= 16'h0;
                r_mismatch  <= 16'h0;
                tx_left     <= cfg_count;
                cur_addr    <= cfg_addr;
                state       <= S_NEXT;
            end
        end

        S_NEXT: begin
            wdog     <= 32'h0;
            beat_idx <= 8'h0;
            beat_addr <= cur_addr;
            t_start  <= r_cycles;
            if (tx_left == 32'h0) begin
                st_done <= 1'b1;
                state   <= S_IDLE;
            end else if (cfg_write) begin
                awaddr  <= cur_addr;
                awvalid <= 1'b1;
                wvalid  <= 1'b1;
                aw_done <= 1'b0;
                w_done  <= 1'b0;
                state   <= S_WR;
            end else begin
                araddr      <= cur_addr;
                arvalid     <= 1'b1;
                first_rbeat <= 1'b1;
                state       <= S_RD;
            end
        end

        S_WR: begin
            if (aw_fire) begin
                awvalid <= 1'b0;
                aw_done <= 1'b1;
            end
            if (w_fire) begin
                if (wlast) begin
                    wvalid <= 1'b0;
                    w_done <= 1'b1;
                end else begin
                    beat_idx  <= beat_idx + 8'd1;
                    beat_addr <= beat_addr + 32'd16;
                end
            end
            if (b_fire) begin
                if (bresp != 2'b00) begin
                    st_resp_err <= 1'b1;
                    if (r_resp_errs != 16'hFFFF)
                        r_resp_errs <= r_resp_errs + 16'd1;
                end
                r_lat_sum <= r_lat_sum + (r_cycles - t_start);
                r_tx_done <= r_tx_done + 32'd1;
                tx_left   <= tx_left - 32'd1;
                if (cfg_inc)
                    cur_addr <= cur_addr + step;
                state <= S_NEXT;
            end
        end

        S_RD: begin
            if (ar_fire)
                arvalid <= 1'b0;
            if (r_fire) begin
                if (first_rbeat) begin
                    first_rbeat <= 1'b0;
                    r_lat_sum   <= r_lat_sum + (r_cycles - t_start);
                    r_rdata0    <= rdata[31:0];
                    r_rdata1    <= rdata[63:32];
                end
                if (rresp != 2'b00) begin
                    st_resp_err <= 1'b1;
                    if (r_resp_errs != 16'hFFFF)
                        r_resp_errs <= r_resp_errs + 16'd1;
                end
                if (cfg_check) begin
                    chk_valid <= 1'b1;
                    chk_got   <= rdata;
                    chk_exp   <= pattern(cfg_seed, beat_addr);
                end
                beat_idx  <= beat_idx + 8'd1;
                beat_addr <= beat_addr + 32'd16;
                if (rlast != (beat_idx == cfg_len))
                    st_last_err <= 1'b1;
                if (rlast) begin
                    r_tx_done <= r_tx_done + 32'd1;
                    tx_left   <= tx_left - 32'd1;
                    if (cfg_inc)
                        cur_addr <= cur_addr + step;
                    state <= S_NEXT;
                end
            end
        end

        default: state <= S_IDLE;
        endcase
    end
end

endmodule

`default_nettype wire
