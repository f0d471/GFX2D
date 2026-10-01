// B4 入库 RTL 的乒乓与流水测试台：驱动与检查分开，行与行之间不等。
//
// tb_engine_pix.v 一行一行地跑（发边带 → 喂读流 → 收完写流 → 等这一半放出来），
// 两半从来不同时有货，排空、行队列、半的放出时机都看不到。这个测试台按 I5 契约里
// 允许的最快节奏驱动：一行的边带与读流喂完就发下一行，只在 row_free 说那一半占着时等。
// 期望值按行算好排进一个队列，检查端在写流上按握手逐拍出队比对，与驱动端互不等待。
//
// 命令结构照几何层：每条命令 1~5 行，行号从 0 起，半号 = 行号最低位（D41、D49），
// 所以奇数行命令之后的第 0 行要等第 0 半放出来，偶数行命令之后两半交替。
// 写流下游三种：0 恒 ready、1 每 7 拍停 1 拍、2 成串停顿（偶尔一停 20~275 拍）。
// 成串停顿下像素通路读得慢、另一半的读入照常，半要是放早了，下一行的读入会盖掉
// 正在读出的那一行。
//
// 每拍查：逐槽数据与「不写」标记、tkeep 恒全 1、停顿时数据与 valid 保持、一行首拍到
// 末拍之间 tvalid 不落；sb_ready 在 row_free 说那一半占着时必须为 0；rs_tready 恒 1。
// 结束时期望队列取空、两半都放出来、没有多余的拍。
`timescale 1ns/1ps
`default_nettype none
`ifndef INJ_VAL
`define INJ_VAL 0
`endif
`ifndef READY_MODE
`define READY_MODE 2
`endif
`ifndef N_CMDS
`define N_CMDS 300
`endif
module tb_engine_pix_pipe;
    reg clk = 0, rst = 1;
    always #5 clk = ~clk;

    reg  [15:0]  sb_row = 0;
    reg  [10:0]  sb_npx = 0;
    reg  [7:0]   sb_beats = 0, sb_op = 0, sb_c = 0;
    reg  [1:0]   sb_ld = 0, sb_ls = 0;
    reg  [3:0]   sb_eq = 0, sb_sfmt = 0;
    reg          sb_key_en = 0, sb_read_dst = 0, sb_valid = 0;
    reg  [31:0]  sb_key = 0, sb_w4 = 0;
    wire         sb_ready;
    wire [1:0]   row_free;
    reg  [127:0] rs_tdata = 0;
    reg          rs_tvalid = 0;
    reg  [1:0]   rs_buf = 0;
    wire         rs_tready;
    wire [127:0] ws_tdata;
    wire [15:0]  ws_tkeep;
    wire         ws_tvalid;
    reg          ws_tready = 1;

    engine_pix #(.INJECT(`INJ_VAL)) dut (
        .clk(clk), .rst(rst), .sb_row(sb_row), .sb_npx(sb_npx), .sb_beats(sb_beats),
        .sb_lead_dst(sb_ld), .sb_lead_src(sb_ls), .sb_op(sb_op), .sb_eq(sb_eq),
        .sb_key_en(sb_key_en), .sb_key(sb_key), .sb_w4(sb_w4), .sb_c(sb_c),
        .sb_sfmt(sb_sfmt), .sb_read_dst(sb_read_dst), .sb_valid(sb_valid),
        .sb_ready(sb_ready), .row_free(row_free),
        .rs_tdata(rs_tdata), .rs_tkeep(16'hffff), .rs_tlast(1'b0),
        .rs_tvalid(rs_tvalid), .rs_tready(rs_tready), .rs_buf(rs_buf),
        .ws_tdata(ws_tdata), .ws_tkeep(ws_tkeep), .ws_tvalid(ws_tvalid),
        .ws_tready(ws_tready));

    // ---------------------------------------------------------------- 参考模型（照 exec_sw.c）
    function [7:0] r255;
        input [17:0] x;
        reg [17:0] t;
        reg [8:0]  q;
        begin
            t = x + 18'd128;
            q = (t + (t >> 8)) >> 8;
            r255 = (q > 9'd255) ? 8'd255 : q[7:0];
        end
    endfunction

    function [31:0] blend_px;
        input [31:0] srcw, dstw;
        input [2:0]  eqf;
        input [7:0]  cf;
        input [1:0]  sfmtf;
        reg [7:0] a, s, d, fs, fd;
        reg [31:0] out;
        integer i;
        begin
            a = (sfmtf == 2'd0) ? 8'd255 : srcw[31:24];
            out = 32'd0;
            for (i = 0; i < 3; i = i + 1) begin
                s = srcw >> (8 * i);
                d = dstw >> (8 * i);
                case (eqf)
                3'd0: begin fs = 8'd255; fd = 8'd0;        end
                3'd1: begin fs = 8'd255; fd = 8'd255 - a;  end
                3'd2: begin fs = 8'd255; fd = 8'd255;      end
                3'd3: begin fs = cf;      fd = 8'd255 - cf; end
                3'd4: begin fs = d;       fd = 8'd0;        end
                default: begin fs = a;   fd = 8'd255 - a;  end
                endcase
                out[i*8 +: 8] = r255({10'd0, s} * {10'd0, fs} + {10'd0, d} * {10'd0, fd});
            end
            blend_px = out & 32'h00FFFFFF;
        end
    endfunction

    // FILL 的常量源字按方程解释（规范条款 §4.2）
    function [1:0] fill_fmt;
        input [2:0] eqf;
        fill_fmt = (eqf == 3'd1) ? 2'd1 : (eqf == 3'd5) ? 2'd2 : 2'd0;
    endfunction

    // ---------------------------------------------------------------- 写流下游
    integer seed_rdy = 32'h5eed_0b4b, burst = 0;
    always @(negedge clk) begin
        case (`READY_MODE)
        0: ws_tready = 1'b1;
        1: ws_tready = ($time / 10) % 7 != 0;
        default: begin
            if (burst > 0) begin
                burst = burst - 1; ws_tready = 1'b0;
            end else if (($random(seed_rdy) & 63) == 0) begin
                burst = 20 + ($random(seed_rdy) & 255); ws_tready = 1'b0;
            end else
                ws_tready = ($random(seed_rdy) & 3) != 0;
        end
        endcase
    end

    // ---------------------------------------------------------------- 期望队列
    reg [127:0] e_data [0:262143];
    reg [3:0]   e_nw   [0:262143];
    reg         e_last [0:262143];
    integer e_wr = 0, e_rd = 0;

    // ---------------------------------------------------------------- 检查端
    integer bad = 0, checked = 0, beats_out = 0, stall_cyc = 0, gap_cyc = 0;
    integer proto_bad = 0, overlap_cyc = 0;
    reg in_row = 0, holding = 0;
    reg [127:0] held;
    integer slot;
    always @(posedge clk) if (!rst) begin
        if (holding && (ws_tdata !== held || ws_tvalid !== 1'b1)) begin
            bad = bad + 1; if (bad < 20) $display("STALL_DATA_CHANGED at %0t", $time);
        end
        holding = ws_tvalid && !ws_tready; held = ws_tdata;
        if (ws_tvalid && !ws_tready) stall_cyc = stall_cyc + 1;
        if (ws_tvalid && rs_tvalid) overlap_cyc = overlap_cyc + 1;
        if (ws_tvalid && ws_tready) begin
            if (e_rd >= e_wr) begin
                bad = bad + 1; if (bad < 20) $display("EXTRA_BEAT at %0t", $time);
            end else begin
                for (slot = 0; slot < 4; slot = slot + 1) begin
                    checked = checked + 1;
                    if (e_nw[e_rd][slot]) begin
                        if (ws_tdata[slot*32+31] !== 1'b1) begin
                            bad = bad + 1;
                            if (bad < 20) $display("MASK_MISMATCH beat=%0d slot=%0d", e_rd, slot);
                        end
                    end else if (ws_tdata[slot*32 +: 32] !== e_data[e_rd][slot*32 +: 32]) begin
                        bad = bad + 1;
                        if (bad < 20) $display("PIX_MISMATCH beat=%0d slot=%0d got=%08x exp=%08x",
                                               e_rd, slot, ws_tdata[slot*32 +: 32], e_data[e_rd][slot*32 +: 32]);
                    end
                end
                if (ws_tkeep !== 16'hffff) begin bad = bad + 1; $display("KEEP_MISMATCH"); end
                in_row = !e_last[e_rd];
                e_rd = e_rd + 1;
                beats_out = beats_out + 1;
            end
        end else if (!ws_tvalid && in_row) begin
            gap_cyc = gap_cyc + 1;
            bad = bad + 1; if (bad < 20) $display("WRITE_STREAM_GAP at %0t", $time);
        end
        if (!row_free[sb_row[0]] && sb_ready) begin
            proto_bad = proto_bad + 1; bad = bad + 1;
            if (proto_bad < 5) $display("SB_READY_ON_OCCUPIED_HALF at %0t", $time);
        end
        if (rs_tvalid && rs_tready !== 1'b1) begin bad = bad + 1; $display("I3_NOT_READY"); end
    end

    // ---------------------------------------------------------------- 驱动端
    integer seed = 32'h2026_1001;
    reg [31:0] srcm [0:1023];
    reg [31:0] dstm [0:1023];
    integer rows = 0, cmds = 0, waits = 0, long_rows = 0;

    task push_row;
        input integer npx, ls, ld, is_blit, eqf, sfmtf, key_en, read_dst;
        input [31:0] key, w4;
        input [7:0] cf;
        integer beats, k, t, j;
        reg [31:0] sw, dw;
        reg [127:0] d;
        reg [3:0] nw;
        begin
            beats = (ld + npx + 3) / 4;
            for (k = 0; k < beats; k = k + 1) begin
                d = 128'd0; nw = 4'd0;
                for (t = 0; t < 4; t = t + 1) begin
                    j = 4 * k + t - ld;
                    sw = is_blit ? srcm[ls + j] : w4;
                    dw = dstm[ld + j];
                    if (j < 0 || j >= npx || (is_blit && key_en && sw == key))
                        nw[t] = 1'b1;
                    else
                        d[t*32 +: 32] = blend_px(sw, dw, eqf[2:0], cf,
                                                 is_blit ? sfmtf[1:0] : fill_fmt(eqf[2:0]));
                end
                e_data[e_wr] = d; e_nw[e_wr] = nw; e_last[e_wr] = (k == beats - 1);
                e_wr = e_wr + 1;
            end
        end
    endtask

    task feed;
        input integer dir, half, beats;
        integer m, s;
        begin
            m = 0;
            while (m < beats) begin
                @(negedge clk);
                if (($random(seed) & 7) == 0) rs_tvalid = 1'b0;     // 读流偶尔断一拍
                else begin
                    rs_tvalid = 1'b1; rs_buf = {dir[0], half[0]};
                    for (s = 0; s < 4; s = s + 1)
                        rs_tdata[s*32 +: 32] = dir ? dstm[4*m + s] : srcm[4*m + s];
                    m = m + 1;
                end
            end
            @(negedge clk); rs_tvalid = 1'b0;
        end
    endtask

    task one_row;
        input integer row_i;
        integer npx, ls, ld, is_blit, eqf, sfmtf, key_en, read_dst, i, r, h, sb, guard;
        reg [31:0] key, w4;
        reg [7:0] cf;
        begin
            h = row_i % 2;
            r = $random(seed);
            if ((r & 31) == 0) begin npx = ((r >> 5) & 1) ? 640 : 253; long_rows = long_rows + 1; end
            else npx = 1 + ((r >> 5) & 63) % 40;
            ls = (r >> 12) & 3; ld = (r >> 14) & 3;
            is_blit = ((r >> 16) & 7) != 0;
            eqf = ((r >> 19) & 15) % 6;
            sfmtf = ((r >> 23) & 3) % 3;
            if (!is_blit) ls = 0;
            read_dst = (eqf != 0);
            cf = $random(seed);
            for (i = 0; i < 1024; i = i + 1) begin
                srcm[i] = $random(seed);
                dstm[i] = $random(seed) & 32'h00FFFFFF;
            end
            key_en = is_blit ? ((r >> 25) & 1) : ((r >> 25) & 1);    // FILL 也会带色键位：被测必须不看
            key = srcm[ls + npx / 2];
            if (npx >= 3) srcm[ls + npx / 2 + 1] = {~key[31:24], key[23:0]};   // 近似字必须照写
            w4 = $random(seed);
            if (!is_blit) key = w4;
            if (!is_blit && (eqf == 1 || eqf == 5)) sfmtf = 0;     // FILL 的 sfmt 偏给 XRGB
            // 等这一半空出来（D41）：驱动端照几何层先查 row_free 再发边带
            @(negedge clk);
            sb_row = row_i[15:0];
            #1;                                   // sb_ready 是 sb_row 的组合函数，等它更新
            guard = 0;
            if (!(row_free[h] && sb_ready)) waits = waits + 1;
            while (!(row_free[h] && sb_ready) && guard < 20000) begin
                @(negedge clk); guard = guard + 1;
            end
            if (guard >= 20000) begin                // 这一半放不出来：后面的行都会卡住，就此收尾
                bad = bad + 1; $display("ROW_NEVER_FREE row=%0d half=%0d", rows, h);
                report;
            end
            sb_npx = npx; sb_beats = (ld + npx + 3) / 4; sb_ld = ld; sb_ls = ls;
            sb_op = is_blit ? 8'd2 : 8'd1; sb_eq = eqf; sb_sfmt = sfmtf;
            sb_key_en = key_en; sb_key = key; sb_w4 = w4; sb_c = cf; sb_read_dst = read_dst;
            sb_valid = 1'b1;
            push_row(npx, ls, ld, is_blit, eqf, sfmtf, key_en, read_dst, key, w4, cf);
            @(negedge clk); sb_valid = 1'b0;
            if (row_free[h] !== 1'b0) begin bad = bad + 1; $display("ROW_NOT_RESERVED"); end
            sb = (ls + npx + 3) / 4;
            if (is_blit) feed(0, h, sb);
            if (read_dst) feed(1, h, (ld + npx + 3) / 4);
            rows = rows + 1;
        end
    endtask

    task report;
        begin
            $display("pipe: cmds=%0d rows=%0d long=%0d beats=%0d slots=%0d waits=%0d stall=%0d overlap=%0d gap=%0d proto=%0d",
                     cmds, rows, long_rows, beats_out, checked, waits, stall_cyc, overlap_cyc, gap_cyc, proto_bad);
            if (`INJ_VAL == 0) begin
                if (bad == 0) $display("RESULT: PASS (%0d rows, %0d slots checked)", rows, checked);
                else $display("RESULT: FAIL (%0d mismatches)", bad);
            end else if (bad > 0) $display("RESULT: FAIL (inject %0d: %0d mismatches, as expected)", `INJ_VAL, bad);
            else $display("RESULT: NOT_RED (inject %0d)", `INJ_VAL);
            $finish;
        end
    endtask

    integer c, nr, ri, guard2;
    initial begin
        repeat (3) @(negedge clk); rst = 0;
        for (c = 0; c < `N_CMDS; c = c + 1) begin
            nr = 1 + (($random(seed) & 255) % 5);
            for (ri = 0; ri < nr; ri = ri + 1) one_row(ri);
            cmds = cmds + 1;
        end
        guard2 = 0;
        while (e_rd < e_wr && guard2 < 200000) begin @(negedge clk); guard2 = guard2 + 1; end
        repeat (40) @(negedge clk);
        if (e_rd != e_wr) begin bad = bad + 1; $display("MISSING_BEATS %0d of %0d", e_wr - e_rd, e_wr); end
        if (row_free !== 2'b11) begin bad = bad + 1; $display("ROWS_NOT_FREE %b", row_free); end
        report;
    end
endmodule
`default_nettype wire
