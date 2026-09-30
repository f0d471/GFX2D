// 从机定时模式（sim/axi_probe/axi_ram_model.v 的 TIMED=1，D13）的回归
//
// 定时模式按前期研究校准到 E4 板上数的那一版从机逐拍复刻（B2 条目第 4 节第 10 问），
// 引擎的性能预估要用它。三条判据，期望值都是在校准用的那一版从机上用本测试台量出来的：
//   1. 连续写 16 拍、WR_BUB=6：W 通道只在第 6、12 拍握手之后各空一拍，共 2 拍空拍
//      （进写数据态的第一拍就能收，不算空拍）。
//   2. 主机在第 6 拍握手之后自己停一拍（真实写核的输出 FIFO 空了就会这样）：必须照常写完。
//      入库第一版按「已收拍数是 WR_BUB 的倍数」拉低 wready，这一拍没有握手，拍数不变，
//      wready 就一直是 0，写通道卡死。
//   3. 第一个 R 握手在 AR 握手之后第 RD_LAT+2 拍：等 RD_LAT 拍，一拍拉起 rvalid，一拍握手。
//      入库第一版多一拍（地址先入队、下一拍才出队，这一拍没有算进 RD_LAT）。
`timescale 1ns/1ps
`default_nettype none

module tb_slave_timed;
    localparam integer RD_LAT = 23;
    localparam integer WR_BUB = 6;

    reg clk = 0;
    always #5 clk = ~clk;
    reg rst_n = 0;

    reg  [31:0]  awaddr = 32'h0, araddr = 32'h0;
    reg  [7:0]   awlen = 8'd15, arlen = 8'd3;
    reg          awvalid = 0, arvalid = 0;
    wire         awready, arready;
    reg  [127:0] wdata = 128'h0;
    reg          wlast = 0, wvalid = 0;
    wire         wready;
    wire [1:0]   bresp, rresp;
    wire         bvalid, rvalid, rlast;
    wire [127:0] rdata;

    axi_ram_model #(
        .STALL(0), .STRB_EN(1), .TIMED(1), .RD_LAT(RD_LAT), .RD_BUB(12), .WR_BUB(WR_BUB), .B_LAT(1)
    ) u_slave (
        .clk(clk), .rst_n(rst_n),
        .awaddr(awaddr), .awlen(awlen), .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(16'hFFFF), .wlast(wlast), .wvalid(wvalid), .wready(wready),
        .bresp(bresp), .bvalid(bvalid), .bready(1'b1),
        .araddr(araddr), .arlen(arlen), .arvalid(arvalid), .arready(arready),
        .rdata(rdata), .rresp(rresp), .rlast(rlast), .rvalid(rvalid), .rready(1'b1),
        .hold_aw(1'b0), .hold_ar(1'b0)
    );

    integer errors = 0;
    integer cyc = 0;
    always @(posedge clk) cyc <= cyc + 1;

    // 写一笔 16 拍；pause_after 非负时，在那么多拍握手之后主机自己停一拍
    integer sent, gaps, t0, t1;
    reg     paused;
    task write16;
        input integer pause_after;
        begin
            @(negedge clk); awvalid = 1;
            while (!(awvalid && awready)) @(negedge clk);
            @(negedge clk); awvalid = 0;
            sent = 0; gaps = 0; paused = 0; t0 = cyc;
            while (sent < 16 && (cyc - t0) < 2000) begin
                wvalid = !(sent == pause_after && !paused);
                if (sent == pause_after) paused = 1;
                wlast  = (sent == 15);
                @(posedge clk);
                if (wvalid && wready) sent = sent + 1;
                else if (wvalid)      gaps = gaps + 1;
                @(negedge clk);
            end
            wvalid = 0; wlast = 0;
            while (!bvalid && (cyc - t0) < 2000) @(negedge clk);
            @(negedge clk);
        end
    endtask

    // 读延迟在时钟沿上记：AR 握手那一拍与第一个 R 握手那一拍（rready 恒 1）
    integer t_ar = -1, t_r = -1;
    always @(posedge clk) begin
        if (arvalid && arready && t_ar < 0) t_ar <= cyc;
        if (rvalid && t_ar >= 0 && t_r < 0) t_r <= cyc;
    end

    initial begin
        repeat (3) @(posedge clk);
        rst_n = 1;

        // 1. 连续写：空拍数
        write16(-1);
        if (sent != 16) begin
            $display("ERR 连续写只写了 %0d 拍", sent); errors = errors + 1;
        end else if (gaps != 2) begin
            $display("ERR 连续写 16 拍、WR_BUB=%0d 应有 2 拍空拍，实际 %0d", WR_BUB, gaps); errors = errors + 1;
        end else
            $display("  continuous write: 16 beats, %0d bubbles", gaps);

        // 2. 主机在第 WR_BUB 拍之后停一拍
        write16(WR_BUB);
        if (sent != 16) begin
            $display("ERR 主机在第 %0d 拍之后停一拍，写通道卡死（只写了 %0d 拍）", WR_BUB, sent); errors = errors + 1;
        end else
            $display("  paused write: 16 beats done");

        // 3. 读首拍延迟
        @(negedge clk); arvalid = 1;
        while (!(arvalid && arready)) @(negedge clk);
        @(negedge clk); arvalid = 0;
        while (t_r < 0 && cyc < 5000) @(negedge clk);
        if (t_r - t_ar != RD_LAT + 2) begin
            $display("ERR 读首拍握手离地址握手 %0d 拍，应为 RD_LAT+2=%0d", t_r - t_ar, RD_LAT + 2); errors = errors + 1;
        end else
            $display("  read first beat: %0d cycles after AR handshake", t_r - t_ar);
        while (!(rvalid && rlast)) @(negedge clk);
        @(negedge clk);

        if (errors == 0) $display("RESULT: PASS");
        else             $display("RESULT: FAIL (%0d errors)", errors);
        $finish;
    end

    initial begin
        #200000;
        $display("RESULT: FAIL (看门狗)");
        $finish;
    end
endmodule

`default_nettype wire
