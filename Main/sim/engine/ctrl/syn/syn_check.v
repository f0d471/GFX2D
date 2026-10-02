// B5 前期研究探针的单独综合封装：三种结构各一个顶层（B4 syn 的同一套做法）。
// 全部输入由 256 位 LFSR 驱动（不接常数，B3 验收踩过：常数会让综合把逻辑折掉、余量虚高），
// 全部输出折进观测寄存器 obs，RAM/逻辑不会被剪。约束 100 MHz（core_clk）。
`timescale 1ns/1ps
`default_nettype none
module syn_check_comb(input wire clk, input wire rst, output reg [127:0] obs);
    wire [255:0] lfsr; wire es, co, rdy; wire [3:0] ec;
    check_lfsr u_l(.clk(clk), .rst(rst), .obs(obs), .lfsr(lfsr));
    probe_check #(.MODE(0)) u_dut(
        .clk(clk), .rst(rst),
        .surf_we(lfsr[0]), .surf_slot(lfsr[4:1]), .surf_wsel(lfsr[6:5]), .surf_wdata(lfsr[38:7]),
        .cmd_data(lfsr), .cmd_valid(lfsr[39]), .cmd_ready(rdy),
        .err_set(es), .err_code(ec), .check_ok(co));
    always @(posedge clk) begin
        if (rst) obs <= 128'b0;
        else obs <= obs ^ {ec, rdy, co, es, lfsr[46:7]};
    end
endmodule

module syn_check_fsm(input wire clk, input wire rst, output reg [127:0] obs);
    wire [255:0] lfsr; wire es, co, rdy; wire [3:0] ec;
    check_lfsr u_l(.clk(clk), .rst(rst), .obs(obs), .lfsr(lfsr));
    probe_check #(.MODE(1)) u_dut(
        .clk(clk), .rst(rst),
        .surf_we(lfsr[0]), .surf_slot(lfsr[4:1]), .surf_wsel(lfsr[6:5]), .surf_wdata(lfsr[38:7]),
        .cmd_data(lfsr), .cmd_valid(lfsr[39]), .cmd_ready(rdy),
        .err_set(es), .err_code(ec), .check_ok(co));
    always @(posedge clk) begin
        if (rst) obs <= 128'b0;
        else obs <= obs ^ {ec, rdy, co, es, lfsr[46:7]};
    end
endmodule

module syn_check_pre(input wire clk, input wire rst, output reg [127:0] obs);
    wire [255:0] lfsr; wire es, co, rdy; wire [3:0] ec;
    check_lfsr u_l(.clk(clk), .rst(rst), .obs(obs), .lfsr(lfsr));
    probe_check #(.MODE(2)) u_dut(
        .clk(clk), .rst(rst),
        .surf_we(lfsr[0]), .surf_slot(lfsr[4:1]), .surf_wsel(lfsr[6:5]), .surf_wdata(lfsr[38:7]),
        .cmd_data(lfsr), .cmd_valid(lfsr[39]), .cmd_ready(rdy),
        .err_set(es), .err_code(ec), .check_ok(co));
    always @(posedge clk) begin
        if (rst) obs <= 128'b0;
        else obs <= obs ^ {ec, rdy, co, es, lfsr[46:7]};
    end
endmodule

// 256 位 LFSR，带 obs 回馈，防止任何一路输入被常数折叠
module check_lfsr(
    input  wire        clk,
    input  wire        rst,
    input  wire [127:0] obs,
    output wire [255:0] lfsr
);
    reg [255:0] r;
    assign lfsr = r;
    always @(posedge clk) begin
        if (rst) r <= 256'h0123456789ABCDEF_FEDCBA9876543210_00000000DEADBEEF;
        else r <= {r[254:0], r[255] ^ r[253] ^ r[251] ^ r[246] ^ obs[0]};
    end
endmodule
`default_nettype wire
