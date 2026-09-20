// display_fifo 的水位与复位语义判据（D4 的开工前提）
//
// 这一条把 Engineering/Todo/hw/扫描输出.md 的前期研究（2026-09-20）变成库内判据。
// 被测的是 rtl/display/display_fifo.v（里面是厂商标本 efx_fifo_wrapper）。
// 判据来自那份文件 2.2 节的「FIFO 语义」表：
//   1. 读延迟恰为 1 个 rd 周期（rd_en 在沿 N 采样且 empty=0 → 数据在沿 N+1）
//   2. 写入 → 读侧水位：滞后 3~4 个 rd 沿（读侧偏低）
//   3. 读出 → 写侧水位：滞后 4~8 个 wr 沿（写侧偏高）
//   4. 复位后两侧水位归零且 empty=1（两域一起复位）
//   5. 连写连读 160 拍顺序逐拍相同
//
// 注错：INJECT_NO_RESET —— 不驱动 wr_rst，判据 4 必须 FAIL。
// 口径：所有电平都在时钟的 negedge 采样。verilog 里 posedge 上的监视块读到的还是
// NBA 更新前的旧值，直接在那里判水位会把「第 3 个沿更新」看成「第 4 个沿」。
`timescale 1ns/1ps

module tb_fifo_semantics;

`ifdef INJECT_NO_RESET
    localparam RST_DISABLE = 1'b1;
`else
    localparam RST_DISABLE = 1'b0;
`endif

    // 板上名义值：core_clk 10ns、像素时钟 39.683ns
    localparam real WR_HALF = 5.0;
    localparam real RD_HALF = 19.8415;

    reg wr_clk = 1'b0;
    reg rd_clk = 1'b0;
    always #WR_HALF wr_clk = ~wr_clk;
    always #RD_HALF rd_clk = ~rd_clk;

    reg          wr_rst  = 1'b1;
    reg          wr_en   = 1'b0;
    reg  [127:0] wr_data = 128'h0;
    reg          rd_en   = 1'b0;

    wire [127:0] rd_data;
    wire         rd_valid, rd_empty, rd_underflow, wr_full, wr_overflow, rst_busy_sync;
    wire [9:0]   wr_level, rd_level;

    display_fifo #(
        .DEPTH      (512),
        .DATA_WIDTH (128)
    ) dut (
        .wr_clk        (wr_clk),
        .wr_rst        (wr_rst),
        .wr_en         (wr_en),
        .wr_data       (wr_data),
        .wr_level      (wr_level),
        .wr_full       (wr_full),
        .wr_overflow   (wr_overflow),
        .rst_busy_sync (rst_busy_sync),
        .rd_clk        (rd_clk),
        .rd_en         (rd_en),
        .rd_data       (rd_data),
        .rd_valid      (rd_valid),
        .rd_level      (rd_level),
        .rd_empty      (rd_empty),
        .rd_underflow  (rd_underflow)
    );

    integer errors = 0;
    integer checks = 0;
    integer i;

    task check(input [8*56-1:0] what, input integer got, input integer lo, input integer hi);
        begin
            checks = checks + 1;
            if (got < lo || got > hi) begin
                errors = errors + 1;
                $display("ERROR %0s: got %0d, expect %0d..%0d (t=%0t)", what, got, lo, hi, $time);
            end else begin
                $display("ok %0s: %0d (expect %0d..%0d)", what, got, lo, hi);
            end
        end
    endtask

    // ---- 图案用 128 位寄存器自增：整数位选 i[127:0] 在 iverilog 里取到 x，
    //      而 x !== x 为真，逐拍比对会静默通过（2026-09-20 踩到过）。
    reg [127:0] wpat = 128'h0;

    reg  [127:0] exp_q [0:255];          // 期望读出的序列
    integer      n_exp = 0;
    reg  [127:0] got_q [0:255];
    integer      n_got = 0;

    // 每拍收集读出的数据（rd_valid 与 rd_data 同拍）
    always @(posedge rd_clk)
        if (rd_valid) begin
            got_q[n_got] = rd_data;
            n_got = n_got + 1;
        end

    // 复位窗口里不写（FIFO 自己也拦不住，见 display_fifo.v 文件头第 2 条）
    wire wr_active = wr_en & ~wr_rst;

    // ---- 判据 2：写入 → 读侧水位的滞后（单位：rd 沿）----
    task write_one_and_measure;
        integer n;
        begin
            @(negedge wr_clk); wr_en = 1'b1; wpat = wpat + 128'h1; wr_data = wpat;
            @(posedge wr_clk);                       // 写沿
            @(negedge wr_clk); wr_en = 1'b0;
            n = 0;
            while (rd_level == 10'd0 && n < 24) begin
                @(posedge rd_clk); n = n + 1;
                @(negedge rd_clk);
            end
            check("write -> rd_level lag (rd edges)", n, 3, 4);
        end
    endtask

    // ---- 判据 3：读出 → 写侧水位的滞后（单位：wr 沿）----
    task read_one_and_measure;
        integer m;
        integer lat;
        reg [127:0] exp;
        begin
            exp = rd_data;                            // 上一拍已经拿到的值
            exp = 128'h0;                             // 占位，下面按 buf 取
            exp = exp_q[0];
            @(negedge rd_clk); rd_en = 1'b1;
            @(posedge rd_clk);                        // 读沿：这一拍弹出
            @(negedge rd_clk); rd_en = 1'b0;
            m = 0;
            while (wr_level != 10'd0 && m < 32) begin
                @(posedge wr_clk); m = m + 1;
                @(negedge wr_clk);
            end
            check("read -> wr_level lag (wr edges)", m, 4, 8);
        end
    endtask

    initial begin
`ifdef DUMP
        $dumpfile("tb_fifo_semantics.vcd");
        $dumpvars(0, tb_fifo_semantics);
`endif

        if (RST_DISABLE)
            $display("INJECT_NO_RESET: 跳过场起始那一次 a_rst_i，判据 4 必须 FAIL");

        // ---- 上电复位：至少 2 个 core_clk 周期，之后等 rst_busy_sync 落下 ----
        // 上电复位任何情况下都要做：注错只跳过判据 4 的那一次场起始复位，
        // 否则 FIFO 从 X 开始，什么判据都测不出来（第一版就是这么写的，判据不尖锐）。
        wr_rst = 1'b1;
        repeat (8) @(negedge wr_clk);
        wr_rst = 1'b0;
        i = 0;
        while (rst_busy_sync && i < 64) begin
            @(posedge wr_clk); i = i + 1;
        end
        $display("rst_busy_sync 落下用了 %0d 个 wr 周期", i);
        repeat (4) @(posedge wr_clk);
        check("post-poweron wr_level", wr_level, 0, 0);
        check("post-poweron rd_level", rd_level, 0, 0);
        check("post-poweron empty",    rd_empty, 1, 1);

        // ---- 判据 1 + 2：单拍写，测滞后 ----
        write_one_and_measure();
        check("rd_level after 1 write", rd_level, 1, 1);
        check("wr_level after 1 write", wr_level, 1, 1);

        // ---- 判据 1 + 3：单拍读，测读延迟与写侧滞后 ----
        // 读延迟：rd_en 在 negedge 拉高，下一沿采样，再下一沿数据有效。
        n_got = 0;
        @(negedge rd_clk); rd_en = 1'b1;
        @(posedge rd_clk);                                  // N：采样 rd_en
        check("rd_valid at N (must be 0)", rd_valid, 0, 0);
        @(posedge rd_clk);                                  // N+1：数据有效
        check("rd_valid at N+1", rd_valid, 1, 1);
        check("rd_data at N+1 low word", rd_data[31:0], 32'h0000_0001, 32'h0000_0001);
        @(negedge rd_clk); rd_en = 1'b0;
        check("rd_level after 1 read", rd_level, 0, 0);
        // 写侧水位随后跟上（等够时间再判）
        repeat (16) @(posedge wr_clk);
        check("wr_level after 1 read", wr_level, 0, 0);

        // ---- 判据 5：连写连读 160 拍，顺序逐拍相同 ----
        for (i = 0; i < 160; i = i + 1) begin
            @(negedge wr_clk); wr_en = 1'b1; wpat = wpat + 128'h1; wr_data = wpat;
            exp_q[i] = wpat;
        end
        @(negedge wr_clk); wr_en = 1'b0;
        n_exp = 160;
        repeat (16) @(posedge rd_clk);
        check("rd_level after 160 writes", rd_level, 160, 160);
        n_got = 0;
        for (i = 0; i < 160; i = i + 1) begin
            @(negedge rd_clk); rd_en = 1'b1;
        end
        @(negedge rd_clk); rd_en = 1'b0;
        repeat (8) @(posedge rd_clk);
        check("160-beat read count", n_got, 160, 160);
        checks = checks + 1;
        i = 0;
        while (i < 160) begin
            if (got_q[i] !== exp_q[i]) begin
                errors = errors + 1;
                if (errors <= 6)
                    $display("ERROR order at %0d: got %h expect %h", i, got_q[i], exp_q[i]);
            end
            i = i + 1;
        end

        // ---- 判据 4：复位把两侧都清掉（注错时这一条必须 FAIL）----
        for (i = 0; i < 100; i = i + 1) begin
            @(negedge wr_clk); wr_en = 1'b1; wpat = wpat + 128'h1; wr_data = wpat;
        end
        @(negedge wr_clk); wr_en = 1'b0;
        repeat (16) @(posedge rd_clk);
        $display("复位前：wr_level=%0d rd_level=%0d", wr_level, rd_level);
        checks = checks + 1;
        if (wr_level == 10'd0) begin
            errors = errors + 1;
            $display("ERROR 复位前 FIFO 是空的，测不出复位");
        end
        if (!RST_DISABLE) begin
            @(negedge wr_clk); wr_rst = 1'b1;
            repeat (4) @(negedge wr_clk);           // 保持 4 个 core_clk 周期（实测 1 个够）
            wr_rst = 1'b0;
            i = 0;
            while (rst_busy_sync && i < 64) begin
                @(posedge wr_clk); i = i + 1;
            end
            repeat (4) @(posedge wr_clk);           // 订正的那 4 拍余量
            repeat (4) @(posedge rd_clk);
        end else begin
            repeat (16) @(posedge rd_clk);          // 注错：什么都不做，只等
        end
        check("post-reset wr_level", wr_level, 0, 0);
        check("post-reset rd_level", rd_level, 0, 0);
        check("post-reset empty",    rd_empty, 1, 1);

        // ---- 复位后还能正常收发 ----
        n_got = 0;
        for (i = 0; i < 4; i = i + 1) begin
            @(negedge wr_clk); wr_en = 1'b1; wpat = wpat + 128'h1; wr_data = wpat;
            exp_q[i] = wpat;
        end
        @(negedge wr_clk); wr_en = 1'b0;
        repeat (16) @(posedge rd_clk);
        for (i = 0; i < 4; i = i + 1) begin
            @(negedge rd_clk); rd_en = 1'b1;
        end
        @(negedge rd_clk); rd_en = 1'b0;
        repeat (8) @(posedge rd_clk);
        check("post-reset read count", n_got, 4, 4);
        i = 0;
        while (i < 4) begin
            checks = checks + 1;
            if (got_q[i] !== exp_q[i]) begin
                errors = errors + 1;
                $display("ERROR post-reset order at %0d: got %h expect %h", i, got_q[i], exp_q[i]);
            end
            i = i + 1;
        end

        if (errors == 0)
            $display("RESULT: PASS checks=%0d", checks);
        else
            $display("RESULT: FAIL errors=%0d checks=%0d", errors, checks);
        $finish;
    end

    initial begin
        #2_000_000;
        $display("RESULT: FAIL timeout at t=%0t", $time);
        $finish;
    end

endmodule
