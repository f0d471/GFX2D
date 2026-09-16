// apb_probe 测试台
//
// 判据：
//   1. ID、VERSION 读值正确，SCRATCH 复位值为 0；
//   2. SCRATCH 32 位读写：全 1、全 0、两种交替码、32 个走 1 码逐一写入读回；
//   3. 16 位地址全译码：高位不同的别名地址读写都报 PSLVERROR，且不改动 SCRATCH；
//   4. 写只读寄存器报 PSLVERROR，读值不变；未映射地址读回 0 并报 PSLVERROR；
//   5. PREADY 始终为 1；复位后 SCRATCH 清零。
//
// 注错（都必须 FAIL，否则判据失效）：
//   INJECT_ADDR_TRUNC  只把 paddr 低 4 位送进 DUT，模拟顶层地址线被截断
//   INJECT_DATA_TRUNC  只把 pwdata 最低位送进 DUT，模拟顶层数据线被截断
`timescale 1ns/1ps
`default_nettype none

module tb_apb_probe;

reg clk = 1'b0;
always #5 clk = ~clk;

reg         rst     = 1'b1;
reg  [15:0] paddr   = 16'h0;
reg         psel    = 1'b0;
reg         penable = 1'b0;
reg         pwrite  = 1'b0;
reg  [31:0] pwdata  = 32'h0;
wire [31:0] prdata;
wire        pready;
wire        pslverr;

`ifdef INJECT_ADDR_TRUNC
wire [15:0] dut_paddr = {12'h000, paddr[3:0]};
`else
wire [15:0] dut_paddr = paddr;
`endif

`ifdef INJECT_DATA_TRUNC
wire [31:0] dut_pwdata = {31'h0, pwdata[0]};
`else
wire [31:0] dut_pwdata = pwdata;
`endif

apb_probe dut (
    .clk     (clk),
    .rst     (rst),
    .paddr   (dut_paddr),
    .psel    (psel),
    .penable (penable),
    .pwrite  (pwrite),
    .pwdata  (dut_pwdata),
    .prdata  (prdata),
    .pready  (pready),
    .pslverr (pslverr)
);

localparam [31:0] ID = 32'h4746_5832;

integer errors = 0;
integer checks = 0;

// 一次 APB3 传输：setup 一拍，access 一拍；access 阶段采样读数据与 PSLVERROR
task apb_xfer(input w, input [15:0] a, input [31:0] d, output [31:0] rd, output err);
begin
    @(negedge clk);
    paddr   = a;
    pwrite  = w;
    pwdata  = d;
    psel    = 1'b1;
    penable = 1'b0;
    @(negedge clk);
    penable = 1'b1;
    #1;
    rd  = prdata;
    err = pslverr;
    if (pready !== 1'b1) begin
        errors = errors + 1;
        $display("ERROR pready=%b at 0x%04h", pready, a);
    end
    @(posedge clk);
    @(negedge clk);
    psel    = 1'b0;
    penable = 1'b0;
    pwrite  = 1'b0;
end
endtask

task expect_read(input [15:0] a, input [31:0] exp_d, input exp_err);
    reg [31:0] rd;
    reg        err;
begin
    apb_xfer(1'b0, a, 32'h0, rd, err);
    checks = checks + 1;
    if (rd !== exp_d || err !== exp_err) begin
        errors = errors + 1;
        $display("ERROR read  0x%04h: data %08h err %b, expect %08h err %b", a, rd, err, exp_d, exp_err);
    end
end
endtask

task expect_write(input [15:0] a, input [31:0] d, input exp_err);
    reg [31:0] rd;
    reg        err;
begin
    apb_xfer(1'b1, a, d, rd, err);
    checks = checks + 1;
    if (err !== exp_err) begin
        errors = errors + 1;
        $display("ERROR write 0x%04h <- %08h: err %b, expect err %b", a, d, err, exp_err);
    end
end
endtask

task rw_scratch(input [31:0] p);
begin
    expect_write(16'h0008, p, 1'b0);
    expect_read (16'h0008, p, 1'b0);
end
endtask

integer k;

initial begin
    repeat (3) @(posedge clk);
    @(negedge clk);
    rst = 1'b0;

    // 1. 只读寄存器与复位值
    expect_read(16'h0000, ID,           1'b0);
    expect_read(16'h0004, 32'h00000001, 1'b0);
    expect_read(16'h0008, 32'h00000000, 1'b0);

    // 2. 32 位数据通路
    rw_scratch(32'hFFFFFFFF);
    rw_scratch(32'h00000000);
    rw_scratch(32'hA5A55A5A);
    rw_scratch(32'h5A5AA5A5);
    for (k = 0; k < 32; k = k + 1)
        rw_scratch(32'h1 << k);

    // 3. 地址全译码：别名地址不得命中
    expect_write(16'h0008, 32'h13579BDF, 1'b0);
    expect_write(16'h8008, 32'hFFFFFFFF, 1'b1);
    expect_write(16'h0108, 32'hFFFFFFFF, 1'b1);
    expect_read (16'h0008, 32'h13579BDF, 1'b0);
    expect_read (16'h8000, 32'h00000000, 1'b1);
    expect_read (16'h1004, 32'h00000000, 1'b1);

    // 4. 只读寄存器与未映射地址
    expect_write(16'h0000, 32'h00000000, 1'b1);
    expect_read (16'h0000, ID,           1'b0);
    expect_read (16'h000C, 32'h00000000, 1'b1);
    expect_read (16'hFFFF, 32'h00000000, 1'b1);

    // 5. 复位清零
    @(negedge clk);
    rst = 1'b1;
    @(negedge clk);
    rst = 1'b0;
    expect_read(16'h0008, 32'h00000000, 1'b0);

    if (errors == 0)
        $display("RESULT: PASS (%0d checks)", checks);
    else
        $display("RESULT: FAIL (%0d errors in %0d checks)", errors, checks);
    $finish;
end

endmodule

`default_nettype wire
