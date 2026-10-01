// tb_probe_geom：几何层前期研究的逐拍探针测试台
//
// 回答 B3 条目 2.2 节第 1 问「产出速率要多快」：把行迭代探针接一个恒 ready 的消费者，
// 量每行、每条命令花几拍，与 engine 第 02 章 3.8 节量到的搬运层每行周期数比。
// 同时把 D3 规矩 1（16 字节对齐、不跨 4KB、不超过 192 拍）与 D7 当作判据，
// 三条注错各自必须见红。
//
// 跑法见同目录 run.sh（WSL 里的 iverilog）。
`timescale 1ns/1ps
`default_nettype none

module tb_probe_geom;
    reg clk = 1'b0;
    reg rst = 1'b1;
    always #5 clk = ~clk;

    integer bad = 0;
    integer total = 0;

    task chk;
        input cond;
        input [1023:0] what;             // 中文一个字 3 字节，256 位只装得下十来个字
        begin
            total = total + 1;
            if (!cond) begin
                bad = bad + 1;
                $display("  FAIL %0s", what);
            end
        end
    endtask

    // ---------------------------------------------------------------- DUT
    reg         start;
    reg  [31:0] dst_row0, src_row0, dst_stride, src_stride;
    reg  [15:0] nrows;
    reg  [10:0] npx;
    reg         rsrc, rdst, last_cmd;
    reg  [3:0]  cidx;
    wire [31:0] rd_addr, wr_addr;
    wire [11:0] rd_beats, wr_beats;
    wire [1:0]  rd_buf;
    wire        rd_valid, wr_valid, wr_last_row;
    wire [10:0] sb_npx;
    wire [1:0]  sb_lead_dst, sb_lead_src;
    wire [7:0]  sb_beats;
    wire        sb_valid, busy, done;

    // 恒 ready 的消费者：量的是几何层自己的吞吐，不含反压
    reg rd_ready = 1'b1, wr_ready = 1'b1, sb_ready = 1'b1;

    integer n_rd, n_wr, n_sb, cyc, rules_bad, wr_last_cnt;

    probe_geom_rowiter #(.MAX_BEATS(192), .INJECT(0)) dut (
        .clk(clk), .rst(rst), .start(start),
        .dst_row0(dst_row0), .src_row0(src_row0),
        .dst_stride(dst_stride), .src_stride(src_stride),
        .nrows(nrows), .npx(npx), .rsrc(rsrc), .rdst(rdst),
        .last_cmd(last_cmd), .cidx(cidx),
        .rd_addr(rd_addr), .rd_beats(rd_beats), .rd_buf(rd_buf),
        .rd_valid(rd_valid), .rd_ready(rd_ready),
        .wr_addr(wr_addr), .wr_beats(wr_beats), .wr_last_row(wr_last_row),
        .wr_valid(wr_valid), .wr_ready(wr_ready),
        .sb_npx(sb_npx), .sb_lead_dst(sb_lead_dst), .sb_lead_src(sb_lead_src),
        .sb_beats(sb_beats), .sb_valid(sb_valid), .sb_ready(sb_ready),
        .busy(busy), .done(done)
    );

    // 规矩 1：收到的每一笔描述符都要过
    task check_desc;
        input is_wr;
        input [31:0] a;
        input [11:0] b;
        input [255:0] what;
        begin
            if ((a & 32'hF) != 32'd0) begin
                rules_bad = rules_bad + 1;
                $display("  RULE1 %0s 地址 0x%08X 不是 16 字节对齐", what, a);
            end
            if (({20'b0, a[11:0]} + {20'b0, b} * 32'd16) > 32'h1000) begin
                rules_bad = rules_bad + 1;
                $display("  RULE1 %0s 0x%08X 起 %0d 拍跨 4KB", what, a, b);
            end
            if (b > 12'd192) begin
                rules_bad = rules_bad + 1;
                $display("  RULE1 %0s %0d 拍超过 192", what, b);
            end
            if (b == 12'd0) begin
                rules_bad = rules_bad + 1;
                $display("  RULE1 %0s 拍数为 0", what);
            end
            if (is_wr) n_wr = n_wr + 1; else n_rd = n_rd + 1;
        end
    endtask

    always @(posedge clk) begin
        if (!rst) begin
            if (rd_valid && rd_ready) check_desc(1'b0, rd_addr, rd_beats, "读");
            if (wr_valid && wr_ready) begin
                check_desc(1'b1, wr_addr, wr_beats, "写");
                if (wr_last_row) wr_last_cnt = wr_last_cnt + 1;
            end
            if (sb_valid && sb_ready) n_sb = n_sb + 1;
        end
    end

    // ---------------------------------------------------------------- 跑一条命令
    task run_cmd;
        input [31:0] d0, s0, dstr, sstr;
        input [15:0] nr;
        input [10:0] npxi;
        input        rs, rd;
        input        lastc;
        output integer cycles;
        begin
            @(negedge clk);
            dst_row0 = d0; src_row0 = s0; dst_stride = dstr; src_stride = sstr;
            nrows = nr; npx = npxi; rsrc = rs; rdst = rd; last_cmd = lastc; cidx = 4'd1;
            n_rd = 0; n_wr = 0; n_sb = 0; wr_last_cnt = 0;
            start = 1'b1;
            @(negedge clk);
            start = 1'b0;
            cycles = 0;
            while (!done && cycles < 100000) begin
                @(posedge clk);
                #1;
                cycles = cycles + 1;
            end
            @(negedge clk);
        end
    endtask

    // ---------------------------------------------------------------- D7 的判定
    // 正常版给前三条命令出 rsrc/rdst；注错版（ALPHA 也按 COPY 避让）的输出在注错 3 里接进行迭代。
    reg  [7:0] d7_op;
    reg  [3:0] d7_eq;
    wire       n7_rsrc, n7_rdst, j7_rsrc, j7_rdst;
    probe_geom_d7 #(.INJECT(0)) u_d7 (.op(d7_op), .eq(d7_eq), .rsrc(n7_rsrc), .rdst(n7_rdst));
    probe_geom_d7 #(.INJECT(1)) u_d7_inj (.op(d7_op), .eq(d7_eq), .rsrc(j7_rsrc), .rdst(j7_rdst));

    // ---------------------------------------------------------------- 五者求交
    reg signed [31:0] ix0, iy0, ix1, iy1;
    wire signed [31:0] wx0, wy0, wx1, wy1;
    wire               wempty;
    wire signed [31:0] bx0, by0, bx1, by1;
    wire               bempty;

    probe_geom_intersect #(.INJECT(0)) u_i0 (
        .dst_x(32'sd300), .dst_y(32'sd100), .w(32'd100), .h(32'd100),
        .src_x(-32'sd10), .src_y(-32'sd10),
        .src_surf_w(32'd64), .src_surf_h(32'd64),
        .dst_surf_w(32'd640), .dst_surf_h(32'd480),
        .clip_x0(32'sd0), .clip_y0(32'sd0), .clip_x1(32'sd639), .clip_y1(32'sd479),
        .is_blit(1'b1),
        .x0(wx0), .y0(wy0), .x1(wx1), .y1(wy1), .empty(wempty)
    );
    probe_geom_intersect #(.INJECT(1)) u_i1 (
        .dst_x(32'sd300), .dst_y(32'sd100), .w(32'd100), .h(32'd100),
        .src_x(-32'sd10), .src_y(-32'sd10),
        .src_surf_w(32'd64), .src_surf_h(32'd64),
        .dst_surf_w(32'd640), .dst_surf_h(32'd480),
        .clip_x0(32'sd0), .clip_y0(32'sd0), .clip_x1(32'sd639), .clip_y1(32'sd479),
        .is_blit(1'b1),
        .x0(bx0), .y0(by0), .x1(bx1), .y1(by1), .empty(bempty)
    );

    // ---------------------------------------------------------------- 注错版行迭代
    wire [31:0] j_wr_addr;
    wire [11:0] j_wr_beats;
    wire        j_wr_valid;
    probe_geom_rowiter #(.MAX_BEATS(192), .INJECT(1)) u_inj (
        .clk(clk), .rst(rst), .start(start),
        .dst_row0(dst_row0), .src_row0(src_row0),
        .dst_stride(dst_stride), .src_stride(src_stride),
        .nrows(nrows), .npx(npx), .rsrc(rsrc), .rdst(rdst),
        .last_cmd(last_cmd), .cidx(cidx),
        .rd_addr(), .rd_beats(), .rd_buf(), .rd_valid(), .rd_ready(1'b1),
        .wr_addr(j_wr_addr), .wr_beats(j_wr_beats), .wr_last_row(),
        .wr_valid(j_wr_valid), .wr_ready(1'b1),
        .sb_npx(), .sb_lead_dst(), .sb_lead_src(), .sb_beats(),
        .sb_valid(), .sb_ready(1'b1), .busy(), .done()
    );
    integer inj_rules_bad = 0;
    always @(posedge clk) begin
        if (!rst && j_wr_valid) begin
            if (({20'b0, j_wr_addr[11:0]} + {20'b0, j_wr_beats} * 32'd16) > 32'h1000)
                inj_rules_bad = inj_rules_bad + 1;
        end
    end

    // ---------------------------------------------------------------- 主流程
    integer c_fill, c_copy, c_alpha, c_full, c_cross, c_lead, c_inj7;
    integer rows;
    integer exp_rd;
    integer i;
    integer d7_bad, d7_inj_diff;
    integer op_i, eq_i;

    initial begin
        $display("probe_geom：几何层前期研究的逐拍探针");
        $display("");

        rst = 1'b1; start = 1'b0;
        dst_row0 = 0; src_row0 = 0; dst_stride = 32'h1000; src_stride = 32'h100;
        nrows = 16; npx = 11'd16; rsrc = 0; rdst = 0; last_cmd = 1; cidx = 0;
        rules_bad = 0; inj_rules_bad = 0;
        d7_op = 8'd1; d7_eq = 4'd0;
        repeat (4) @(posedge clk);
        rst = 1'b0;
        @(negedge clk);

        // ---- 0 D7 的判定表：FILL 不读源、BLIT 读源；COPY 不读目的，其余五个方程读目的
        d7_bad = 0; d7_inj_diff = 0;
        for (op_i = 1; op_i <= 2; op_i = op_i + 1) begin
            for (eq_i = 0; eq_i <= 5; eq_i = eq_i + 1) begin
                d7_op = op_i; d7_eq = eq_i;
                #1;
                if (n7_rsrc !== (op_i == 2) || n7_rdst !== (eq_i != 0)) begin
                    d7_bad = d7_bad + 1;
                    $display("  D7 op=%0d eq=%0d 给出 读源=%b 读目的=%b", op_i, eq_i, n7_rsrc, n7_rdst);
                end
                if (j7_rsrc !== n7_rsrc || j7_rdst !== n7_rdst) d7_inj_diff = d7_inj_diff + 1;
            end
        end
        chk(d7_bad == 0, "D7 判定表 2 种操作 x 6 个方程全对");
        $display("  D7 判定表         : 12 项，不符 %0d", d7_bad);
        @(negedge clk);

        // ---- 1 FILL COPY 16x16：写只有一笔，最快的行（搬运层 15.0 周期/行）
        d7_op = 8'd1; d7_eq = 4'd0; #1;
        run_cmd(32'h00101000, 32'h00200000, 32'h1000, 32'h100, 16'd16, 11'd16,
                n7_rsrc, n7_rdst, 1'b1, c_fill);
        $display("  FILL  COPY 16x16  : 命令 %0d 拍，%0d 行，每行 %0d.%02d 拍；写 %0d 读 %0d 边带 %0d",
                 c_fill, 16, c_fill / 16, (c_fill * 100 / 16) % 100, n_wr, n_rd, n_sb);
        chk(n_rd == 0, "FILL COPY 不该有读描述符");
        chk(n_sb == 16, "FILL COPY 每行一条边带");
        c_fill = c_fill;

        // ---- 2 COPY BLIT 16x16：读源一笔（搬运层 34.3 周期/行）
        d7_op = 8'd2; d7_eq = 4'd0; #1;
        run_cmd(32'h00101000, 32'h00200000, 32'h1000, 32'h100, 16'd16, 11'd16,
                n7_rsrc, n7_rdst, 1'b1, c_copy);
        $display("  COPY  BLIT 16x16  : 命令 %0d 拍，%0d 行，每行 %0d.%02d 拍；写 %0d 读 %0d 边带 %0d",
                 c_copy, 16, c_copy / 16, (c_copy * 100 / 16) % 100, n_wr, n_rd, n_sb);
        chk(n_rd == 16, "COPY BLIT 每行一笔读源");
        chk(n_wr == 16, "COPY BLIT 每行一笔写");

        // ---- 3 ALPHA BLIT 16x16：读源加读目的（搬运层 68.3 周期/行）
        d7_op = 8'd2; d7_eq = 4'd1; #1;
        run_cmd(32'h00101000, 32'h00200000, 32'h1000, 32'h100, 16'd16, 11'd16,
                n7_rsrc, n7_rdst, 1'b1, c_alpha);
        $display("  ALPHA BLIT 16x16  : 命令 %0d 拍，%0d 行，每行 %0d.%02d 拍；写 %0d 读 %0d 边带 %0d",
                 c_alpha, 16, c_alpha / 16, (c_alpha * 100 / 16) % 100, n_wr, n_rd, n_sb);
        chk(n_rd == 32, "ALPHA BLIT 每行两笔读");

        // ---- 4 整屏 FILL：一行 400 像素 = 100 拍（搬运层 126.0 周期/行）
        run_cmd(32'h00100000, 32'h00200000, 32'h1000, 32'h100, 16'd480, 11'd400,
                1'b0, 1'b0, 1'b1, c_full);
        $display("  整屏 FILL 400px   : 命令 %0d 拍，%0d 行，每行 %0d.%02d 拍；写 %0d 读 %0d 边带 %0d",
                 c_full, 480, c_full / 480, (c_full * 100 / 480) % 100, n_wr, n_rd, n_sb);
        chk(n_wr == 480, "整屏 FILL 每行一笔写（100 拍不拆）");

        // ---- 5 行跨 4KB：起始 0x100F00，一行 2560 字节
        run_cmd(32'h00100F00, 32'h00200000, 32'h1000, 32'h100, 16'd8, 11'd640,
                1'b0, 1'b0, 1'b1, c_cross);
        $display("  跨 4KB 一行 640px : 命令 %0d 拍，%0d 行，每行 %0d.%02d 拍；写 %0d 读 %0d 边带 %0d",
                 c_cross, 8, c_cross / 8, (c_cross * 100 / 8) % 100, n_wr, n_rd, n_sb);
        chk(n_wr == 16, "跨 4KB 的行必须拆成两笔写");
        chk(sb_beats == 8'd160, "跨 4KB 行的边带拍数是两段之和 160（16+144）");

        // ---- 6 基址只按 4 字节对齐：拍内相位随地址，不是 x 对 4 的余数
        run_cmd(32'h00100404, 32'h00200000, 32'h1000, 32'h100, 16'd4, 11'd29,
                1'b0, 1'b0, 1'b1, c_lead);
        $display("  base%%16=4 相位     : 命令 %0d 拍，%0d 行，每行 %0d.%02d 拍；写 %0d 读 %0d 边带 %0d",
                 c_lead, 4, c_lead / 4, (c_lead * 100 / 4) % 100, n_wr, n_rd, n_sb);
        chk(sb_lead_dst == 2'd1, "行首 0x100404 的拍内相位是 1");

        // ---- 判据：规矩 1 全程违例 0
        $display("");
        chk(rules_bad == 0, "D3 规矩 1 全程违例 0");

        // ---- 注错 1：跨 4KB 不拆，规矩 1 必须见红
        $display("");
        $display("注错用例（期望见红）：");
        rst = 1'b1; repeat (2) @(posedge clk); rst = 1'b0; @(negedge clk);
        run_cmd(32'h00100F00, 32'h00200000, 32'h1000, 32'h100, 16'd8, 11'd640,
                1'b0, 1'b0, 1'b1, c_cross);
        chk(inj_rules_bad > 0, "注错「跨 4KB 不拆」必须触发规矩 1 违例");
        $display("    跨 4KB 不拆：违例 %0d 个周期", inj_rules_bad);

        // ---- 注错 2：漏掉第五项求交，矩形必须与正确版不同
        chk(!(wx0 == bx0 && wy0 == by0 && wx1 == bx1 && wy1 == by1),
            "注错「漏掉第五项求交」必须改掉矩形");
        $display("    正确版矩形 (%0d,%0d)-(%0d,%0d)，漏第五项 (%0d,%0d)-(%0d,%0d)",
                 wx0, wy0, wx1, wy1, bx0, by0, bx1, by1);

        // ---- 注错 3：D7 的 ALPHA 误开读目的避让。注错版 probe_geom_d7 的输出接进行迭代，
        //      重跑第 3 条 ALPHA BLIT，同一条判据「每行两笔读」必须见红
        d7_op = 8'd2; d7_eq = 4'd1; #1;
        run_cmd(32'h00101000, 32'h00200000, 32'h1000, 32'h100, 16'd16, 11'd16,
                j7_rsrc, j7_rdst, 1'b1, c_inj7);
        chk(n_rd != 32, "注错「ALPHA 误开读目的避让」必须让「ALPHA BLIT 每行两笔读」见红");
        $display("    ALPHA 误开读目的避让：读描述符 %0d 笔（正确应为 32），判定表里与正确版不同 %0d 项",
                 n_rd, d7_inj_diff);

        $display("");
        if (bad == 0) $display("RESULT: PASS (%0d/%0d)", total, total);
        else          $display("RESULT: FAIL (%0d/%0d 项不符)", bad, total);
        $finish;
    end

    initial begin
        #2000000;
        $display("RESULT: FAIL (timeout)");
        $finish;
    end
`ifdef DUMP
    initial begin
        $dumpfile("build/probe_geom.vcd");
        $dumpvars(0, tb_probe_geom);
    end
`endif
endmodule

`default_nettype wire
