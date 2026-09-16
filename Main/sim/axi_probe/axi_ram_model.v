// 测试台用 AXI4 从机：128 位数据、INCR 突发，带随机反压与响应延迟
//
// 存储 4096 拍（65536 字节），地址按 (addr >> 4) % 4096 取模。
// 突发长度超过 MAX_LEN 时仍按协议走完整个突发，但回 SLVERR，读回的数据为 0。
//
// 注错宏（由 run.sh 传入）：
//   INJECT_FLIP  地址 0x0000_0100 那一拍写入时翻转最低位，读比对必须失配
//   INJECT_HANG  永不发出写响应，主机必须超时
`timescale 1ns/1ps
`default_nettype none

module axi_ram_model #(
    parameter integer MAX_LEN = 255,
    parameter integer SEED    = 1,
    parameter integer STALL   = 3      // 每个 ready/valid 拉低的概率约 STALL/8
)(
    input  wire         clk,
    input  wire         rst_n,

    input  wire [31:0]  awaddr,
    input  wire [7:0]   awlen,
    input  wire         awvalid,
    output reg          awready,
    input  wire [127:0] wdata,
    input  wire         wlast,
    input  wire         wvalid,
    output reg          wready,
    output reg  [1:0]   bresp,
    output reg          bvalid,
    input  wire         bready,

    input  wire [31:0]  araddr,
    input  wire [7:0]   arlen,
    input  wire         arvalid,
    output reg          arready,
    output reg  [127:0] rdata,
    output reg  [1:0]   rresp,
    output reg          rlast,
    output reg          rvalid,
    input  wire         rready
);

reg [127:0] mem [0:4095];
integer seed_r = SEED;

function stall;
    input dummy;
    begin
        stall = (($random(seed_r) & 7) < STALL);
    end
endfunction

integer i;
initial begin
    for (i = 0; i < 4096; i = i + 1)
        mem[i] = 128'h0;
end

// ---------------- 写 ----------------
localparam W_IDLE = 2'd0, W_DATA = 2'd1, W_RESP = 2'd2;
reg [1:0]  wst;
reg [31:0] wa;
reg [7:0]  wlen, wcnt;
reg        werr;

always @(posedge clk) begin
    if (!rst_n) begin
        wst     <= W_IDLE;
        awready <= 1'b0;
        wready  <= 1'b0;
        bvalid  <= 1'b0;
        bresp   <= 2'b00;
    end else begin
        case (wst)
        W_IDLE: begin
            awready <= !stall(0);
            if (awvalid && awready) begin
                awready <= 1'b0;
                wa      <= awaddr;
                wlen    <= awlen;
                wcnt    <= 8'd0;
                werr    <= (awlen > MAX_LEN);
                wst     <= W_DATA;
            end
        end
        W_DATA: begin
            wready <= !stall(0);
            if (wvalid && wready) begin
                if (!werr)
                    mem[(wa >> 4) % 4096] <= wdata
`ifdef INJECT_FLIP
                        ^ ((wa == 32'h0000_0100) ? 128'h1 : 128'h0)
`endif
                        ;
                wa   <= wa + 32'd16;
                wcnt <= wcnt + 8'd1;
                if (wlast != (wcnt == wlen))
                    $display("MODEL: wlast mismatch at beat %0d", wcnt);
                if (wlast) begin
                    wready <= 1'b0;
                    wst    <= W_RESP;
                end
            end
        end
        W_RESP: begin
`ifndef INJECT_HANG
            if (!bvalid && !stall(0)) begin
                bvalid <= 1'b1;
                bresp  <= werr ? 2'b10 : 2'b00;
            end
`endif
            if (bvalid && bready) begin
                bvalid <= 1'b0;
                wst    <= W_IDLE;
            end
        end
        default: wst <= W_IDLE;
        endcase
    end
end

// ---------------- 读 ----------------
localparam R_IDLE = 2'd0, R_DATA = 2'd1;
reg [1:0]  rst_;
reg [31:0] ra;
reg [7:0]  rlen, rcnt;
reg        rerr;

always @(posedge clk) begin
    if (!rst_n) begin
        rst_    <= R_IDLE;
        arready <= 1'b0;
        rvalid  <= 1'b0;
        rlast   <= 1'b0;
        rresp   <= 2'b00;
    end else begin
        case (rst_)
        R_IDLE: begin
            arready <= !stall(0);
            if (arvalid && arready) begin
                arready <= 1'b0;
                ra      <= araddr;
                rlen    <= arlen;
                rcnt    <= 8'd0;
                rerr    <= (arlen > MAX_LEN);
                rst_    <= R_DATA;
            end
        end
        R_DATA: begin
            if (!rvalid || rready) begin
                if (rvalid && rlast) begin
                    rvalid <= 1'b0;
                    rst_   <= R_IDLE;
                end else if (!stall(0)) begin
                    rvalid <= 1'b1;
                    rdata  <= rerr ? 128'h0 : mem[(ra >> 4) % 4096];
                    rresp  <= rerr ? 2'b10 : 2'b00;
                    rlast  <= (rcnt == rlen);
                    ra     <= ra + 32'd16;
                    rcnt   <= rcnt + 8'd1;
                end else
                    rvalid <= 1'b0;
            end
        end
        default: rst_ <= R_IDLE;
        endcase
    end
end

endmodule

`default_nettype wire
