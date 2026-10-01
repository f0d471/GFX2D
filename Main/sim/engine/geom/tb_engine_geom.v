// tb_engine_geom：引擎 P1 B3 几何层的单元仿真
//
// 被测是 rtl/engine/engine_geom.v 整块。测试台按 I1 喂命令，收 I2 的描述符与 I5 的边带，
// 与独立模型（Main/tools/check/geom_model.py --dump-records / --dump-directed）给的
// 期望值逐条比。期望值不读 RTL，也不在本文件里重算——本文件只做「驱动 + 比对」。
//
// 判据（B3 条目 2.4 节）：
//   1 定向用例 31 条：基址不按 4KB 对齐、基址只按 4 字节对齐（相位不等于 x 对 4 的余数）、
//     行跨 4KB（拆两段）、源与目的相位 4×4、宽度对 4 取余 4 种、源矩形越出源表面、零面积
//   2 已有输入：159 条向量与十一整帧用例里几何层真正见到的全部命令
//   3 规矩 1 与避让：每个描述符 16 字节对齐、不跨 4KB、不超过 192 拍；D7 三种情形没有读描述符
//   4 注错：漏掉第五项求交、跨 4KB 不拆、ALPHA 误开读目的避让、COPY 多发读目的、
//     行开始之前不看 row_free，各自必须见红
//   另加两个握手用例：+STALL（三个方向各压出反压）与 +RFSTALL（B4 行缓冲替身的排空
//     拉长到 23 拍，几何层必须真的停下来等），判据都是「序列与恒 ready 时逐条相同」。
//
// 每条记录的消费循环只在**握手真正发生的那一拍之后**才前进到期望的下一条：
// 「比较 → 等一拍 → 前进」的写法在 ready 被压住时会与 RTL 脱节（描述符还压在
// DUT 的 valid 上没被收走，测试台已经去比下一条了）。这一条是逼出来的，见第 03 章第四节。
//
// 消费循环只看它正在等的那个通道，别的通道的 ready 照样是高的——多发的描述符会被
// 悄悄收走。所以另有一个与消费循环无关的**握手监视器**：逐拍数三个通道的握手与
// cmd_done 脉冲，每条命令结束时与消费循环收下的条数比（PROTO / DONEERR），
// 并查三个 valid 不同时为高。还有一个 **B4 行缓冲替身**给 row_free，
// 查每一行都开在空着的那一半上（BUFERR）。这两样是 2026-10-01 复核时补的：
// 原来的测试台放过了「COPY 多发读目的」「非空命令也回 cmd_done」「行推进不等 row_free」三种错。
//
// 跑法见同目录 run.sh：vvp -n build/geom.vvp +REC=<记录文件> [+STALL] [+RFSTALL]
`timescale 1ns/1ps
`default_nettype none
`ifndef INJECT_VAL
`define INJECT_VAL 0
`endif

module tb_engine_geom;
    // ---- 记录格式的字段下标，与 tools/check/geom_model.py 的 REC_* 表一一对应 ----
    localparam C_CIDX = 1,  C_OP = 2,   C_EQ = 3,   C_FLAGS = 4, C_NROWS = 5,
               C_Y0 = 6,    C_X0 = 7,   C_Y1 = 8,   C_X1 = 9,
               C_KEYEN = 10, C_SFMT = 11, C_C = 12,
               C_SRC_BASE = 13, C_SRC_W = 14, C_SRC_H = 15, C_SRC_SLOG2 = 16,
               C_DST_BASE = 17, C_DST_W = 18, C_DST_H = 19, C_DST_SLOG2 = 20,
               C_SRC_X = 21, C_SRC_Y = 22, C_DST_X = 23, C_DST_Y = 24,
               C_W = 25, C_H = 26,
               C_CX0 = 27, C_CY0 = 28, C_CX1 = 29, C_CY1 = 30, C_KEY = 31, C_W4 = 32;
    localparam C_N = 33;
    localparam R_CIDX = 1, R_ROW = 2, R_DIR = 3, R_BUF = 4, R_ADDR = 5, R_BEATS = 6;
    localparam R_N = 7;
    localparam W_CIDX = 1, W_ROW = 2, W_ADDR = 3, W_BEATS = 4, W_LAST = 5;
    localparam W_N = 6;
    localparam S_CIDX = 1, S_ROW = 2, S_PY = 3, S_NPX = 4, S_LEADD = 5, S_LEADS = 6,
               S_BEATS = 7;
    localparam S_N = 8;

    localparam FLAG_RSRC = 1, FLAG_RDST = 2, FLAG_EMPTY = 4;

    reg clk = 1'b0;
    always #5 clk = ~clk;

    integer bad = 0, total = 0, cmds = 0, nrec = 0;
    integer rule1 = 0, mism = 0, dnerr = 0, proto = 0, buferr = 0;

    task chk;
        input cond;
        input [1023:0] what;
        begin
            total = total + 1;
            if (!cond) begin
                bad = bad + 1;
                $display("  FAIL %0s", what);
            end
        end
    endtask

    task failm;
        input [1023:0] what;
        begin
            mism = mism + 1;
            bad = bad + 1;
            if (mism <= 20) $display("  MISMATCH %0s", what);
        end
    endtask

    // ---------------------------------------------------------------- 记录文件
    reg [31:0] mem [0:131071];
    reg [1023:0] recfile;
    integer p, i;

    // ---------------------------------------------------------------- DUT
    reg         start = 1'b0;
    reg  [7:0]  op = 8'd0;
    reg  [3:0]  eq = 4'd0;
    reg         key_en = 1'b0;
    reg  [31:0] key_w5 = 32'd0, const_w4 = 32'd0;
    reg  [7:0]  c = 8'd0;
    reg  [3:0]  sfmt = 4'd0;
    reg  [31:0] src_base = 32'd0, dst_base = 32'd0;
    reg  [15:0] src_w = 16'd0, src_h = 16'd0, dst_w = 16'd0, dst_h = 16'd0;
    reg  [4:0]  src_slog2 = 5'd0, dst_slog2 = 5'd0;
    reg  signed [15:0] src_x = 16'sd0, src_y = 16'sd0, dst_x = 16'sd0, dst_y = 16'sd0;
    reg  [15:0] w = 16'd0, h = 16'd0;
    reg  signed [15:0] clip_x0 = 16'sd0, clip_y0 = 16'sd0,
                       clip_x1 = 16'sd639, clip_y1 = 16'sd479;
    reg  [3:0]  cidx = 4'd0;
    reg  [1:0]  row_free;
    reg         rst;

    wire        busy, cmd_done;
    wire [3:0]  done_cidx;
    wire [31:0] rd_addr, wr_addr;
    wire [11:0] rd_beats, wr_beats;
    wire [1:0]  rd_buf;
    wire [3:0]  rd_cidx, wr_cidx;
    wire        rd_valid, wr_valid, wr_last_row;
    wire [15:0] sb_row, sb_py;
    wire [10:0] sb_npx;
    wire [7:0]  sb_beats;
    wire [1:0]  sb_lead_dst, sb_lead_src;
    wire [7:0]  sb_op;
    wire [3:0]  sb_eq;
    wire        sb_key_en, sb_read_dst, sb_valid;
    wire [31:0] sb_key, sb_w4;
    wire [7:0]  sb_c;
    wire [3:0]  sb_sfmt;

    // 消费端：默认恒 ready（搬运层每个方向一笔在途，反压由它自己的仿真覆盖；
    // 这里量的是本层的序列正确性与吞吐）。+STALL=1 时给三个方向各压出反压，
    // 判据是「序列与恒 ready 时逐条相同」——证明三个方向的握手各自独立、不丢描述符。
    reg rd_ready, wr_ready, sb_ready;
    reg stall_en = 1'b0;
    reg rf_stall = 1'b0;
    reg [3:0] pcnt = 4'd0;
    always @(posedge clk) if (rst) pcnt <= 4'd0; else pcnt <= pcnt + 4'd1;
    always @* begin
        rd_ready = !stall_en || (pcnt[2:0] != 3'd0);      // 每 8 拍压 1 拍
        wr_ready = !stall_en || (pcnt[2:0] != 3'd3);      // 与读错开
        sb_ready = !stall_en || (pcnt[2:0] != 3'd5);
    end

    // ---------------------------------------------------------------- B4 行缓冲替身（D41）
    // 两半各一位占用。第 h 半在它的边带握手那一拍占上；这一行的写描述符拍数收齐
    // （累计到边带的 sb_beats）之后，再过 drain 拍放掉——模拟写流从行缓冲里读完。
    // row_free = ~占用。默认 drain = 0；+RFSTALL 时 23 拍，比一行的几何层开销长，
    // 下一行与下一条命令的第 0 行都必须真的停下来等。
    // 判据（BUFERR）：边带握手那一拍它那一半必须是空的；读描述符的 rd_buf[0] 必须是
    // 当前这一行的那一半。
    reg  [1:0] occ;
    reg  [1:0] dpend;
    integer    dcnt0, dcnt1;
    integer    drain_len = 0;
    integer    rem_beats;
    reg        cur_h;
    always @* row_free = ~occ;
    always @(posedge clk) begin
        if (rst) begin
            occ <= 2'b00; dpend <= 2'b00; dcnt0 <= 0; dcnt1 <= 0;
            rem_beats <= 0; cur_h <= 1'b0;
        end else begin
            if (dpend[0]) begin
                if (dcnt0 == 0) begin occ[0] <= 1'b0; dpend[0] <= 1'b0; end
                else dcnt0 <= dcnt0 - 1;
            end
            if (dpend[1]) begin
                if (dcnt1 == 0) begin occ[1] <= 1'b0; dpend[1] <= 1'b0; end
                else dcnt1 <= dcnt1 - 1;
            end
            if (sb_valid && sb_ready) begin
                if (occ[sb_row[0]]) begin
                    buferr = buferr + 1;
                    bad = bad + 1;
                    if (buferr <= 20)
                        $display("  BUFERR row %0d started on busy half %0d", sb_row, sb_row[0]);
                end
                occ[sb_row[0]] <= 1'b1;
                cur_h <= sb_row[0];
                rem_beats <= sb_beats;
            end
            if (rd_valid && rd_ready && (rd_buf[0] !== cur_h)) begin
                buferr = buferr + 1;
                bad = bad + 1;
                if (buferr <= 20)
                    $display("  BUFERR rd_buf half %0d but current row is on half %0d", rd_buf[0], cur_h);
            end
            if (wr_valid && wr_ready) begin
                if (rem_beats - $signed({1'b0, wr_beats}) <= 0) begin
                    if (cur_h) begin dpend[1] <= 1'b1; dcnt1 <= drain_len; end
                    else       begin dpend[0] <= 1'b1; dcnt0 <= drain_len; end
                end
                rem_beats <= rem_beats - $signed({1'b0, wr_beats});
            end
        end
    end

    // ---------------------------------------------------------------- 握手监视器
    // 与消费循环无关：逐拍数三个通道的握手与 cmd_done 脉冲（hs_*），消费循环每收下
    // 一条期望记录记一笔（ex_*）；每条命令结束时两边必须相等。
    integer hs_sb = 0, hs_rd = 0, hs_wr = 0, hs_done = 0;
    integer ex_sb = 0, ex_rd = 0, ex_wr = 0, ex_done = 0;
    always @(posedge clk) begin
        if (!rst) begin
            if (sb_valid && sb_ready) hs_sb = hs_sb + 1;
            if (rd_valid && rd_ready) hs_rd = hs_rd + 1;
            if (wr_valid && wr_ready) hs_wr = hs_wr + 1;
            if (cmd_done)             hs_done = hs_done + 1;
            if ((sb_valid + rd_valid + wr_valid) > 1) begin
                proto = proto + 1;
                bad = bad + 1;
                if (proto <= 20) $display("  PROTO two valids at once (sb %0d rd %0d wr %0d)",
                                          sb_valid, rd_valid, wr_valid);
            end
        end
    end

    task check_counts;
        begin
            total = total + 1;
            if (hs_sb != ex_sb || hs_rd != ex_rd || hs_wr != ex_wr) begin
                proto = proto + 1;
                bad = bad + 1;
                if (proto <= 20)
                    $display("  PROTO handshake count cidx %0d: sb %0d/%0d rd %0d/%0d wr %0d/%0d (seen/expected)",
                             cidx, hs_sb, ex_sb, hs_rd, ex_rd, hs_wr, ex_wr);
                hs_sb = ex_sb; hs_rd = ex_rd; hs_wr = ex_wr;      // 只报一次，后面的命令重新对齐
            end
            total = total + 1;
            if (hs_done != ex_done) begin
                dnerr = dnerr + 1;
                bad = bad + 1;
                $display("  DONEERR cmd_done pulses %0d, expected %0d (cidx %0d)",
                         hs_done, ex_done, cidx);
                hs_done = ex_done;
            end
        end
    endtask

    engine_geom #(.MAX_BEATS(192), .INJECT(`INJECT_VAL)) dut (
        .clk(clk), .rst(rst),
        .start(start), .op(op), .eq(eq), .key_en(key_en), .key_w5(key_w5),
        .const_w4(const_w4), .c(c), .sfmt(sfmt),
        .src_base(src_base), .src_w(src_w), .src_h(src_h), .src_slog2(src_slog2),
        .dst_base(dst_base), .dst_w(dst_w), .dst_h(dst_h), .dst_slog2(dst_slog2),
        .src_x(src_x), .src_y(src_y), .dst_x(dst_x), .dst_y(dst_y),
        .w(w), .h(h),
        .clip_x0(clip_x0), .clip_y0(clip_y0), .clip_x1(clip_x1), .clip_y1(clip_y1),
        .cidx(cidx), .row_free(row_free),
        .busy(busy), .cmd_done(cmd_done), .done_cidx(done_cidx),
        .rd_addr(rd_addr), .rd_beats(rd_beats), .rd_buf(rd_buf),
        .rd_cidx(rd_cidx), .rd_valid(rd_valid), .rd_ready(rd_ready),
        .wr_addr(wr_addr), .wr_beats(wr_beats), .wr_cidx(wr_cidx),
        .wr_last_row(wr_last_row), .wr_valid(wr_valid), .wr_ready(wr_ready),
        .sb_row(sb_row), .sb_py(sb_py), .sb_npx(sb_npx), .sb_beats(sb_beats),
        .sb_lead_dst(sb_lead_dst), .sb_lead_src(sb_lead_src),
        .sb_op(sb_op), .sb_eq(sb_eq), .sb_key_en(sb_key_en), .sb_key(sb_key),
        .sb_w4(sb_w4), .sb_c(sb_c), .sb_sfmt(sb_sfmt), .sb_read_dst(sb_read_dst),
        .sb_valid(sb_valid), .sb_ready(sb_ready)
    );

    integer nrow_seen, nrows_exp, hdr;
    integer cyc, cmp, done_this, wr_seen, rd_src_seen, rd_dst_seen;
    integer nrow_total = 0;
    time t_start, t_end;

    // D3 规矩 1：每个描述符都要过。违例直接计入 bad：原来只计 rule1、不计 bad，
    // 规矩 1 单独违例时 RESULT 仍是 PASS，只靠 run.sh 事后 grep 兜底（2026-10-01 复核）。
    task check_rule1;
        input is_wr;
        input [31:0] a;
        input [11:0] b;
        begin
            if ((a & 32'hF) != 32'd0) begin
                rule1 = rule1 + 1; bad = bad + 1;
                $display("  RULE1 %0s addr %08X not 16-byte aligned", is_wr ? "wr" : "rd", a);
            end
            if (({20'b0, a[11:0]} + {20'b0, b} * 32'd16) > 32'h1000) begin
                rule1 = rule1 + 1; bad = bad + 1;
                $display("  RULE1 addr %08X len %0d beats crosses 4KB", a, b);
            end
            if (b > 12'd192) begin
                rule1 = rule1 + 1; bad = bad + 1;
                $display("  RULE1 %0d beats over 192", b);
            end
            if (b == 12'd0) begin
                rule1 = rule1 + 1; bad = bad + 1;
                $display("  RULE1 zero-beat descriptor");
            end
        end
    endtask

    // ---------------------------------------------------------------- 一条命令
    task run_one;
        begin
            if (mem[p] != 32'd0) begin
                failm("expect type-0 header");
                $finish;
            end
            cidx      = mem[p + C_CIDX][3:0];
            op        = mem[p + C_OP][7:0];
            eq        = mem[p + C_EQ][3:0];
            key_en    = mem[p + C_KEYEN][0];
            sfmt      = mem[p + C_SFMT][3:0];
            c         = mem[p + C_C][7:0];
            src_base  = mem[p + C_SRC_BASE];
            src_w     = mem[p + C_SRC_W][15:0];
            src_h     = mem[p + C_SRC_H][15:0];
            src_slog2 = mem[p + C_SRC_SLOG2][4:0];
            dst_base  = mem[p + C_DST_BASE];
            dst_w     = mem[p + C_DST_W][15:0];
            dst_h     = mem[p + C_DST_H][15:0];
            dst_slog2 = mem[p + C_DST_SLOG2][4:0];
            src_x     = mem[p + C_SRC_X][15:0];
            src_y     = mem[p + C_SRC_Y][15:0];
            dst_x     = mem[p + C_DST_X][15:0];
            dst_y     = mem[p + C_DST_Y][15:0];
            w         = mem[p + C_W][15:0];
            h         = mem[p + C_H][15:0];
            clip_x0   = mem[p + C_CX0][15:0];
            clip_y0   = mem[p + C_CY0][15:0];
            clip_x1   = mem[p + C_CX1][15:0];
            clip_y1   = mem[p + C_CY1][15:0];
            key_w5    = mem[p + C_KEY];
            const_w4  = mem[p + C_W4];
            nrows_exp = mem[p + C_NROWS];
            hdr = p;
            p = p + C_N;
            cmds = cmds + 1;

            @(negedge clk);
            start = 1'b1;
            @(negedge clk);
            start = 1'b0;

            if ((mem[hdr + C_FLAGS] & FLAG_EMPTY) != 0) begin
                // 零面积命令：不回描述符，只回一拍带命令索引的完成脉冲（D38）
                cyc = 0;
                while (!cmd_done && cyc < 256) begin
                    @(posedge clk);
                    #1;
                    cyc = cyc + 1;
                end
                ex_done = ex_done + 1;
                if (!cmd_done) begin
                    dnerr = dnerr + 1;
                    bad = bad + 1;
                    $display("  DONEERR empty command gave no cmd_done (cidx %0d)", cidx);
                end else begin
                    if (done_cidx !== cidx) begin
                        dnerr = dnerr + 1;
                        bad = bad + 1;
                        $display("  DONEERR cmd_done cidx %0d != %0d", done_cidx, cidx);
                    end
                    @(posedge clk);
                    #1;
                end
                check_counts;
            end else begin
                nrow_seen = 0;
                wr_seen = 0;
                rd_src_seen = 0;
                rd_dst_seen = 0;
                while ((mem[p] == 32'd3) || (mem[p] == 32'd2) || (mem[p] == 32'd1)) begin
                    nrec = nrec + 1;
                    cmp = 0;
                    done_this = 0;
                    cyc = 0;
                    if (mem[p] == 32'd3) begin
                        while (!done_this && cyc < 8192) begin
                            if (sb_valid) begin
                                if (!cmp) begin
                                    if (sb_row !== mem[p + S_ROW][15:0] ||
                                        sb_py  !== mem[p + S_PY][15:0]  ||
                                        sb_npx !== mem[p + S_NPX][10:0] ||
                                        sb_beats !== mem[p + S_BEATS][7:0] ||
                                        sb_lead_dst !== mem[p + S_LEADD][1:0] ||
                                        sb_lead_src !== mem[p + S_LEADS][1:0]) begin
                                        failm("sideband row/py/npx/beats/lead mismatch");
                                        $display("        RTL row=%0d py=%0d npx=%0d beats=%0d lead=%0d/%0d",
                                                 sb_row, sb_py, sb_npx, sb_beats,
                                                 sb_lead_dst, sb_lead_src);
                                        $display("        EXP row=%0d py=%0d npx=%0d beats=%0d lead=%0d/%0d",
                                                 mem[p+S_ROW], mem[p+S_PY], mem[p+S_NPX],
                                                 mem[p+S_BEATS], mem[p+S_LEADD], mem[p+S_LEADS]);
                                    end
                                    if (sb_op !== op || sb_eq !== eq || sb_key_en !== key_en ||
                                        sb_key !== key_w5 || sb_w4 !== const_w4 ||
                                        sb_c !== c || sb_sfmt !== sfmt)
                                        failm("sideband blend params vs I1");
                                    if (sb_read_dst !== ((mem[hdr + C_FLAGS] & FLAG_RDST) != 0))
                                        failm("sideband read-dst vs D7");
                                    nrow_seen = nrow_seen + 1;
                                    cmp = 1;
                                end
                                if (sb_ready) done_this = 1;
                            end
                            if (!done_this) begin @(posedge clk); #1; cyc = cyc + 1; end
                        end
                        if (!done_this) failm("no sideband");
                        else ex_sb = ex_sb + 1;
                        @(posedge clk); #1;
                        p = p + S_N;
                    end else if (mem[p] == 32'd2) begin
                        while (!done_this && cyc < 8192) begin
                            if (rd_valid) begin
                                if (!cmp) begin
                                    check_rule1(1'b0, rd_addr, rd_beats);
                                    if (rd_cidx !== mem[p + R_CIDX][3:0] ||
                                        rd_buf  !== mem[p + R_BUF][1:0]  ||
                                        rd_addr !== mem[p + R_ADDR]      ||
                                        rd_beats !== mem[p + R_BEATS][11:0]) begin
                                        failm("read descriptor addr/beats/buf mismatch");
                                        $display("        RTL cidx=%0d buf=%0d addr=%08X beats=%0d",
                                                 rd_cidx, rd_buf, rd_addr, rd_beats);
                                        $display("        EXP cidx=%0d buf=%0d addr=%08X beats=%0d",
                                                 mem[p+R_CIDX], mem[p+R_BUF],
                                                 mem[p+R_ADDR], mem[p+R_BEATS]);
                                    end
                                    if (mem[p + R_DIR] == 32'd0) rd_src_seen = rd_src_seen + 1;
                                    else                        rd_dst_seen = rd_dst_seen + 1;
                                    cmp = 1;
                                end
                                if (rd_ready) done_this = 1;
                            end
                            if (!done_this) begin @(posedge clk); #1; cyc = cyc + 1; end
                        end
                        if (!done_this) failm("no read descriptor");
                        else ex_rd = ex_rd + 1;
                        @(posedge clk); #1;
                        p = p + R_N;
                    end else begin
                        while (!done_this && cyc < 8192) begin
                            if (wr_valid) begin
                                if (!cmp) begin
                                    check_rule1(1'b1, wr_addr, wr_beats);
                                    if (wr_cidx !== mem[p + W_CIDX][3:0] ||
                                        wr_addr !== mem[p + W_ADDR]      ||
                                        wr_beats !== mem[p + W_BEATS][11:0] ||
                                        wr_last_row !== mem[p + W_LAST][0]) begin
                                        failm("write descriptor addr/beats/last mismatch");
                                        $display("        RTL cidx=%0d addr=%08X beats=%0d last=%0d",
                                                 wr_cidx, wr_addr, wr_beats, wr_last_row);
                                        $display("        EXP cidx=%0d addr=%08X beats=%0d last=%0d",
                                                 mem[p+W_CIDX], mem[p+W_ADDR],
                                                 mem[p+W_BEATS], mem[p+W_LAST]);
                                    end
                                    wr_seen = wr_seen + 1;
                                    cmp = 1;
                                end
                                if (wr_ready) done_this = 1;
                            end
                            if (!done_this) begin @(posedge clk); #1; cyc = cyc + 1; end
                        end
                        if (!done_this) failm("no write descriptor");
                        else ex_wr = ex_wr + 1;
                        @(posedge clk); #1;
                        p = p + W_N;
                    end
                end

                total = total + 1;
                if (nrow_seen != nrows_exp) begin
                    failm("sideband count vs nrows");
                    $display("        RTL %0d rows, EXP %0d rows", nrow_seen, nrows_exp);
                end
                nrow_total = nrow_total + nrow_seen;
                // D7：读源的有无与操作对应、读目的的有无与方程对应
                total = total + 1;
                if ((rd_src_seen > 0) !== ((mem[hdr + C_FLAGS] & FLAG_RSRC) != 0)) begin
                    failm("read-source presence vs D7");
                end
                total = total + 1;
                if ((rd_dst_seen > 0) !== ((mem[hdr + C_FLAGS] & FLAG_RDST) != 0)) begin
                    failm("read-dest presence vs D7");
                end
                total = total + 1;
                if (wr_seen == 0) begin
                    failm("no write descriptor at all");
                end
                // 命令发完之后 busy 应当落下来
                cyc = 0;
                while (busy && cyc < 256) begin @(posedge clk); #1; cyc = cyc + 1; end
                if (busy) failm("busy stuck high after last descriptor");
                // 命令发完之后的握手条数与 cmd_done 脉冲：多发的描述符、非空命令的 cmd_done
                check_counts;
            end
        end
    endtask

    initial begin
        if (!$value$plusargs("REC=%s", recfile)) begin
            $display("RESULT: FAIL (no +REC=<record file>)");
            $finish;
        end
        for (i = 0; i < 131072; i = i + 1) mem[i] = 32'hFFFFFFFF;
        $readmemh(recfile, mem);

        $display("engine_geom unit sim: rec=%0s INJECT=%0d", recfile, `INJECT_VAL);
        if ($test$plusargs("STALL"))   begin stall_en = 1'b1; $display("  (+STALL)"); end
        if ($test$plusargs("RFSTALL")) begin
            rf_stall = 1'b1; drain_len = 23; $display("  (+RFSTALL: row buffer drain 23 cycles)");
        end

        rst = 1'b1;
        repeat (4) @(posedge clk);
        rst = 1'b0;
        @(negedge clk);
        t_start = $time;

        p = 0;
        while (mem[p] !== 32'h0000000F) begin
            if (mem[p] === 32'hFFFFFFFF) begin
                failm("record file has no F terminator");
                $finish;
            end
            run_one;
        end

        $display("");
        t_end = $time;
        $display("  commands %0d, expected records %0d", cmds, nrec);
        $display("  rows %0d, cycles %0d, cycles/row %0d.%02d",
                 nrow_total, (t_end - t_start) / 10,
                 ((t_end - t_start) / 10) / (nrow_total ? nrow_total : 1),
                 (((t_end - t_start) / 10) * 100 / (nrow_total ? nrow_total : 1)) % 100);
        $display("  rule1 violations %0d, mismatches %0d, done-pulse errors %0d, protocol %0d, buffer %0d",
                 rule1, mism, dnerr, proto, buferr);
        $display("  handshakes sb %0d rd %0d wr %0d, cmd_done %0d", hs_sb, hs_rd, hs_wr, hs_done);
        // bad 数的是出错的次数（一条比对可能报多处），total 数的是整体检查的项数，
        // 两者不是分子与分母（原来写成「bad of total」，注错时出现过「193 of 120」）
        if (bad == 0) $display("RESULT: PASS (%0d checks)", total);
        else          $display("RESULT: FAIL (%0d errors; %0d checks run)", bad, total);
        $finish;
    end

    initial begin
        #200000000;
        $display("RESULT: FAIL (timeout)");
        $finish;
    end

`ifdef DUMP
    initial begin
        $dumpfile("build/engine_geom.vcd");
        $dumpvars(0, tb_engine_geom);
    end
`endif
endmodule

`default_nettype wire
