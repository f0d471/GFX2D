// tb_probe_pix：研究探针 probe_pix.v 的测试台
//
// 参考模型与 sw/lib/gfx/exec_sw.c 逐条对齐（blend_px 与 r255 照抄）：
//   a = XRGB ? 255 : srcw[31:24]；三通道 r255(s*fs + d*fd)；输出高字节恒 0；
//   BLIT 色键比较源的原始存储字，命中则该像素不写；FILL 无色键。
//
// 五类用例：
//   sweep  npx 1..25 × ls 0..3 × ld 0..3 共 400 行（BLIT），方程、sfmt、色键、半号滚动；
//          每行随机填源、目的行缓冲（色键字取真实源像素字，保证有命中）
//   fill   FILL 的六个方程（源 = w4 常量、不读源块；sfmt 按方程：ALPHA 用 PM、ALPHA_STRAIGHT 用 ST，其余 XRGB）
//   seg    写流拆两段的用例：tlast 恰在段 1 末拍与段 2 末拍，段间写流不断
//   long   长行 253、640 像素各四行（640 像素、非零相位就是 161 拍的最长行）
//   注错   见 run.sh 的注错表，必须见红
//
// 写流的下游有两种接法：
//   默认      ws_tready 恒 1（只看「下游 ready 时每拍一拍」）
//   +REALWR   接真写核 rtl/vendor/verilog-axi/axi_dma_wr.v（参数与 engine_mover 相同）
//             与板上实测校准的定时从机（axi_ram_model TIMED=1）；测试台按段发写描述符
//             （段 1 结束在 4KB 边界上），ws_tready 由写核给。写核每个描述符首拍必停一拍，
//             输出 FIFO 半满时也停（2026-10-01 复核补的判据）
//
// 整体检查（每行都查）：
//   逐拍逐槽（按握手计拍）：行内非色键像素逐位相等且「不写」位为 0；行外/色键命中「不写」位为 1；
//   拍数 = ceil((ld+npx)/4)（与几何层 wr_total 同式）；
//   写流无气泡：首个 valid 到末拍握手之间 tvalid 不落（停顿时 tvalid 保持，判据「两行在片上之后写流不断拍」）；
//   tlast 只出现在段末拍。另统计停顿拍数：段首拍的停顿与段中的停顿分开数。
`timescale 1ns/1ps
`default_nettype none

// INJECT 值由 run.sh 以 -DINJECT_VAL=N 传入（照 sim/engine/geom 的惯例）
`ifndef INJECT_VAL
`define INJECT_VAL 0
`endif

module tb_probe_pix;
    reg clk = 1'b0, rst = 1'b1;
    always #5 clk = ~clk;

    reg         start;
    reg  [1:0]  half, sfmt, lead_src, lead_dst;
    reg         is_blit, key_en, read_dst;
    reg  [2:0]  eq;
    reg  [31:0] key_w5, w4;
    reg  [7:0]  c;
    reg  [10:0] npx;
    reg  [7:0]  seg1_beats, seg2_beats;
    reg         lb_we, lb_src_sel;
    reg  [8:0]  lb_waddr;
    reg  [127:0] lb_wdata;
    wire [127:0] ws_tdata;
    wire [15:0]  ws_tkeep;
    wire         ws_tlast, ws_tvalid, busy;
    wire [1:0]   dbg_off, dbg_st;
    wire         ws_tready;

`ifdef REALWR
    // 真写核 + 定时从机。写描述符由 run_row 按段装进 wd_*，下面的进程逐个握手送出。
    reg  [31:0] wd_addr = 32'd0, wd_addr2 = 32'd0;
    reg  [11:0] wd_len = 12'd0, wd_len2 = 12'd0;
    reg         wd_valid = 1'b0;
    integer     wd_left = 0;
    wire        wd_ready;
    integer     wd_sent = 0, wd_done = 0, wd_err = 0;
    wire        st_valid;
    wire [3:0]  st_err;
    always @(posedge clk) begin
        if (wd_valid && wd_ready) begin
            wd_sent = wd_sent + 1;
            if (wd_left > 1) begin
                wd_addr <= wd_addr2; wd_len <= wd_len2; wd_left <= wd_left - 1;
            end else begin
                wd_valid <= 1'b0; wd_left <= 0;
            end
        end
        if (st_valid) begin
            wd_done = wd_done + 1;
            if (st_err != 4'd0) wd_err = wd_err + 1;
        end
    end

    wire [31:0]  awaddr;
    wire [7:0]   awlen;
    wire         awvalid, awready, wlast, wvalid, wready, bvalid, bready;
    wire [127:0] wdata;
    wire [15:0]  wstrb;
    wire [1:0]   bresp;
    axi_dma_wr #(
        .AXI_DATA_WIDTH(128), .AXI_ADDR_WIDTH(32), .AXI_ID_WIDTH(4),
        .AXI_MAX_BURST_LEN(192),
        .AXIS_DATA_WIDTH(128), .AXIS_KEEP_ENABLE(1), .AXIS_LAST_ENABLE(1),
        .AXIS_ID_ENABLE(0), .AXIS_DEST_ENABLE(0), .AXIS_USER_ENABLE(0),
        .LEN_WIDTH(12), .TAG_WIDTH(4), .ENABLE_SG(0), .ENABLE_UNALIGNED(0)
    ) u_wr (
        .clk(clk), .rst(rst),
        .s_axis_write_desc_addr(wd_addr), .s_axis_write_desc_len(wd_len),
        .s_axis_write_desc_tag(4'd0),
        .s_axis_write_desc_valid(wd_valid), .s_axis_write_desc_ready(wd_ready),
        .m_axis_write_desc_status_len(), .m_axis_write_desc_status_tag(),
        .m_axis_write_desc_status_id(), .m_axis_write_desc_status_dest(),
        .m_axis_write_desc_status_user(),
        .m_axis_write_desc_status_error(st_err),
        .m_axis_write_desc_status_valid(st_valid),
        .s_axis_write_data_tdata(ws_tdata), .s_axis_write_data_tkeep(ws_tkeep),
        .s_axis_write_data_tvalid(ws_tvalid), .s_axis_write_data_tready(ws_tready),
        .s_axis_write_data_tlast(ws_tlast),
        .s_axis_write_data_tid(8'd0), .s_axis_write_data_tdest(8'd0),
        .s_axis_write_data_tuser(1'b0),
        .m_axi_awid(), .m_axi_awaddr(awaddr), .m_axi_awlen(awlen),
        .m_axi_awsize(), .m_axi_awburst(),
        .m_axi_awlock(), .m_axi_awcache(), .m_axi_awprot(),
        .m_axi_awvalid(awvalid), .m_axi_awready(awready),
        .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wlast(wlast),
        .m_axi_wvalid(wvalid), .m_axi_wready(wready),
        .m_axi_bid(4'd0), .m_axi_bresp(bresp), .m_axi_bvalid(bvalid),
        .m_axi_bready(bready),
        .enable(1'b1), .abort(1'b0)
    );

    axi_ram_model #(
        .STALL(0), .STRB_EN(1), .TIMED(1), .RD_LAT(23), .RD_BUB(12), .WR_BUB(6), .B_LAT(1)
    ) u_slave (
        .clk(clk), .rst_n(!rst),
        .awaddr(awaddr), .awlen(awlen), .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(wstrb), .wlast(wlast), .wvalid(wvalid), .wready(wready),
        .bresp(bresp), .bvalid(bvalid), .bready(bready),
        .araddr(32'd0), .arlen(8'd0), .arvalid(1'b0), .arready(),
        .rdata(), .rresp(), .rlast(), .rvalid(), .rready(1'b1),
        .hold_aw(1'b0), .hold_ar(1'b0)
    );
`else
    assign ws_tready = 1'b1;
`endif

    probe_pix_path #(.INJECT(`INJECT_VAL)) dut (
        .clk(clk), .rst(rst),
        .start(start), .half(half), .is_blit(is_blit), .eq(eq),
        .key_en(key_en), .key_w5(key_w5), .w4(w4), .c(c), .sfmt(sfmt),
        .read_dst(read_dst), .npx(npx), .lead_src(lead_src), .lead_dst(lead_dst),
        .seg1_beats(seg1_beats), .seg2_beats(seg2_beats),
        .lb_we(lb_we), .lb_src_sel(lb_src_sel), .lb_waddr(lb_waddr), .lb_wdata(lb_wdata),
        .ws_tdata(ws_tdata), .ws_tkeep(ws_tkeep), .ws_tlast(ws_tlast),
        .ws_tvalid(ws_tvalid), .ws_tready(ws_tready),
        .busy(busy), .dbg_off(dbg_off), .dbg_st(dbg_st));

    // INJECT 由 -DINJECT_VAL 传入，见文件头

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

    // ---------------------------------------------------------------- 行内存与期望
    reg [31:0] src_mem [0:1023];        // 半内绝对槽（源行缓冲，含行外槽）
    reg [31:0] dst_mem [0:1023];
    reg [31:0] exp_word [0:255][0:3];   // 期望写流：拍 × 槽
    reg        exp_nw   [0:255][0:3];   // 期望「不写」
    reg [31:0] f_key, f_w4;
    reg [7:0]  f_c;
    reg [2:0]  f_eq;
    reg [1:0]  f_sfmt;
    reg        f_is_blit, f_key_en, f_read_dst;
    reg [10:0] f_npx;
    reg [1:0]  f_ls, f_ld;
    reg [7:0]  f_seg1, f_seg2;

    integer bad, checked, rows_run;
    integer gap_err, len_err, last_err, keep_err;
    integer stall_head, stall_mid, rows_mid;   // 段首拍的停顿、段中的停顿、段中停过的行数
    integer seed = 32'h2026_1001;

    // 填期望：按像素 j 算期望字与不写位
    task build_expect;
        integer j, t, slot, beats_i;
        reg [31:0] srcw, dstw, outw;
        reg hit, oob;
        begin
            beats_i = (f_ld + f_npx + 3) / 4;
            for (t = 0; t < 256; t = t + 1)
                for (slot = 0; slot < 4; slot = slot + 1) begin
                    exp_nw[t][slot] = 1'b1;
                    exp_word[t][slot] = 32'hFFFFFFFF;
                end
            for (t = 0; t < beats_i; t = t + 1)
                for (slot = 0; slot < 4; slot = slot + 1) begin
                    j = 4 * t + slot - f_ld;
                    oob = (j < 0) || (j >= f_npx);
                    if (f_is_blit) srcw = src_mem[f_ls + j];
                    else           srcw = f_w4;
                    dstw = dst_mem[f_ld + j];
                    hit = f_is_blit && f_key_en && (srcw == f_key);
                    if (oob || hit) begin
                        exp_nw[t][slot] = 1'b1;       // 数据不查
                    end else begin
                        exp_nw[t][slot] = 1'b0;
                        outw = blend_px(srcw, dstw, f_eq, f_c, f_sfmt);
                        exp_word[t][slot] = outw;
                    end
                end
        end
    endtask

    // 预填行缓冲：源、目的各 src/dst_beats 拍。
    // 激励一律 negedge 设置、posedge 被采样：posedge 唤醒的同一时刻改写
    // lb_src_sel/lb_wdata 会与 RAM 的采样构成竞态（RAM 采到旧 sel + 新数据，
    // 目的数据写进源缓冲）——这是首版 preload 的真实错因。
    task preload;
        integer m, sb, db, slot;
        begin
            sb = (f_ls + f_npx + 3) / 4;
            db = (f_ld + f_npx + 3) / 4;
            for (m = 0; m < sb; m = m + 1) begin
                @(negedge clk);
                lb_we = 1'b1; lb_src_sel = 1'b0;
                lb_waddr = {half[0], m[7:0]};
                for (slot = 0; slot < 4; slot = slot + 1)
                    lb_wdata[slot*32 +: 32] = src_mem[4*m + slot];
            end
            for (m = 0; m < db; m = m + 1) begin
                @(negedge clk);
                lb_we = 1'b1; lb_src_sel = 1'b1;
                lb_waddr = {half[0], m[7:0]};
                for (slot = 0; slot < 4; slot = slot + 1)
                    lb_wdata[slot*32 +: 32] = dst_mem[4*m + slot];
            end
            @(negedge clk);
            lb_we = 1'b0;
        end
    endtask

    // 跑一行：start、收写流、逐拍逐槽比对 + 整体检查
    task run_row;
        integer t, slot, beats_i, got, seen_valid, gap_here, last_cnt, i, mid_here;
        integer row_h0, row_m0, row_cyc, t_first;
        reg [127:0] dw;
        begin
            beats_i = (f_ld + f_npx + 3) / 4;
            build_expect;
            preload;
`ifdef REALWR
            // 写描述符：不拆段的行写在 0x1000 起（最长 2576 字节，不跨 4KB）；
            // 拆段的行让段 1 结束在 0x2000、段 2 从 0x2000 起（与几何层的 4KB 拆段同形）
            @(negedge clk);
            if (f_seg2 == 0) begin
                wd_addr = 32'h0000_1000; wd_len = beats_i * 16; wd_left = 1;
            end else begin
                wd_addr = 32'h0000_2000 - f_seg1 * 16; wd_len = f_seg1 * 16;
                wd_addr2 = 32'h0000_2000;              wd_len2 = f_seg2 * 16;
                wd_left = 2;
            end
            wd_valid = 1'b1;
`endif
            @(negedge clk);
            // start 一拍：negedge 设置、negedge 清除，跨过一个完整的 posedge，
            // 避免「posedge 唤醒后清 0 与 DUT 同沿采样」的竞态（调试单行时踩过）
            start = 1'b1;
            is_blit = f_is_blit; eq = f_eq; key_en = f_key_en; key_w5 = f_key;
            w4 = f_w4; c = f_c; sfmt = f_sfmt; read_dst = f_read_dst;
            npx = f_npx; lead_src = f_ls; lead_dst = f_ld;
            seg1_beats = f_seg1; seg2_beats = f_seg2;
            @(negedge clk);
            start = 1'b0;
            // 收写流（超时 4000 拍），按握手计拍
            got = 0; seen_valid = 0; gap_here = 0; last_cnt = 0; mid_here = 0;
            row_h0 = stall_head; row_m0 = stall_mid; row_cyc = 0; t_first = 0;
            for (i = 0; i < 4000; i = i + 1) begin
                @(posedge clk);
                if (ws_tvalid && !ws_tready) begin
                    // 停顿：tvalid 保持，数据这一拍不算。段首拍（got == 0 或 == seg1）与段中分开数
                    if (seen_valid == 0) t_first = i;
                    seen_valid = 1;
                    if (got == 0 || (f_seg2 != 0 && got == f_seg1)) stall_head = stall_head + 1;
                    else begin stall_mid = stall_mid + 1; mid_here = 1; end
                end else if (ws_tvalid) begin
                    if (seen_valid == 0) t_first = i;
                    seen_valid = 1;
                    if (got == beats_i - 1) row_cyc = i - t_first + 1;
                    dw = ws_tdata;
                    // 逐槽比对
                    for (slot = 0; slot < 4; slot = slot + 1) begin
                        checked = checked + 1;
                        if (exp_nw[got][slot]) begin
                            if (dw[slot*32 + 31] != 1'b1) begin
                                bad = bad + 1;
                                if (bad <= 20)
                                    $display("  MISMATCH npx=%0d ls=%0d ld=%0d beat=%0d slot=%0d should be nowrite, data %08x",
                                             f_npx, f_ls, f_ld, got, slot, dw[slot*32 +: 32]);
                            end
                        end else begin
                            if (dw[slot*32 +: 32] !== exp_word[got][slot] || dw[slot*32 + 31] != 1'b0) begin
                                bad = bad + 1;
                                if (bad <= 20)
                                    $display("  MISMATCH npx=%0d ls=%0d ld=%0d eq=%0d beat=%0d slot=%0d got %08x want %08x | srcj=%08x dstj=%08x c=%0d sfmt=%0d | DUT rcur0=%08x rprev0=%08x px1=%08x px2=%08x rdst2=%08x rkey=%08x rkeyen=%b",
                                             f_npx, f_ls, f_ld, f_eq, got, slot,
                                             dw[slot*32 +: 32], exp_word[got][slot],
                                             (4*got+slot-f_ld >= 0 && 4*got+slot-f_ld < f_npx) ? src_mem[f_ls+4*got+slot-f_ld] : 32'hBAD0BAD0,
                                             dst_mem[f_ld+4*got+slot-f_ld], f_c, f_sfmt,
                                             dut.r_cur[31:0], dut.r_prev[31:0],
                                             dut.s1, dut.s2, dut.r_dst2[95:64],
                                             dut.r_key, dut.r_key_en, dut.u_lbs.raddr,
                                             dut.u_lbs.mem[256], dut.u_lbd.mem[256]);
                            end
                        end
                    end
                    if (ws_tkeep !== 16'hFFFF) begin
                        keep_err = keep_err + 1; bad = bad + 1;
                    end
                    if (ws_tlast) begin
                        last_cnt = last_cnt + 1;
                        if (f_seg2 != 0) begin
                            if (got != f_seg1 - 1 && got != f_seg1 + f_seg2 - 1) begin
                                last_err = last_err + 1; bad = bad + 1;
                                $display("  TLAST-POS npx=%0d at beat %0d (segs %0d+%0d)", f_npx, got, f_seg1, f_seg2);
                            end
                        end else begin
                            if (got != beats_i - 1) begin
                                last_err = last_err + 1; bad = bad + 1;
                                $display("  TLAST-POS npx=%0d at beat %0d want %0d", f_npx, got, beats_i - 1);
                            end
                        end
                    end
                    got = got + 1;
                    if (got > beats_i) i = 4000;        // 超收，断
                end else if (seen_valid != 0 && got < beats_i) begin
                    // 第一个 valid 之后、收满之前不许断
                    gap_here = 1;
                end
                if (got >= beats_i && (f_seg2 == 0 || last_cnt >= 2)) i = 4000;
            end
            if (got != beats_i) begin
                len_err = len_err + 1; bad = bad + 1;
                $display("  BEATS npx=%0d ls=%0d ld=%0d got %0d want %0d", f_npx, f_ls, f_ld, got, beats_i);
            end
            if (gap_here != 0) begin
                gap_err = gap_err + 1; bad = bad + 1;
                $display("  GAP npx=%0d ls=%0d ld=%0d tvalid dropped mid-row", f_npx, f_ls, f_ld);
            end
            if (mid_here != 0) rows_mid = rows_mid + 1;
`ifdef REALWR
            // 长行、拆段行逐行打印；其余行只在「段首停顿拍数 ≠ 段数」时打印
            if (f_npx >= 100 || f_seg2 != 0 || (stall_head - row_h0) != 1)
                $display("  row npx=%0d ls=%0d ld=%0d beats=%0d segs=%0d+%0d: stall head %0d, mid %0d, first valid to last handshake %0d cycles",
                         f_npx, f_ls, f_ld, beats_i, f_seg1, f_seg2,
                         stall_head - row_h0, stall_mid - row_m0, row_cyc);
`endif
            rows_run = rows_run + 1;
            // 等回到 IDLE：busy=running 在排空之前就落了，要看状态机本身
            i = 0;
            while (dut.pst != 2'd0 && i < 100) begin @(posedge clk); i = i + 1; end
        end
    endtask

    // 填一行随机内容；色键字取真实源像素字（保证命中）
    task gen_row;
        input integer mode;               // 0 BLIT、1 FILL
        input [2:0] eqf;
        input [1:0] sfmtf;
        input keyf;
        integer slot, sb;
        reg [31:0] rnd;
        begin
            f_is_blit = (mode == 0);
            f_eq = eqf; f_sfmt = sfmtf; f_key_en = keyf;
            rnd = $random(seed);
            f_c = rnd[7:0];
            f_w4 = {(rnd[7:0] | 8'h80), $random(seed) & 32'h00FFFFFF};
            f_read_dst = (eqf != 3'd0);                 // 非 COPY 才读目的
            sb = (f_ls + f_npx + 3) / 4;
            // 源字：随机 alpha（含 0 与 255 的极端值）+ 随机 24 位色
            for (slot = 0; slot < sb * 4 + 8; slot = slot + 1) begin
                rnd = $random(seed);
                src_mem[slot] = {rnd[7:0], $random(seed) & 32'h00FFFFFF};
            end
            // 目的字：高字节恒 0（XRGB 帧缓冲的实际内容）
            for (slot = 0; slot < 1024; slot = slot + 1)
                dst_mem[slot] = {8'h00, $random(seed) & 32'h00FFFFFF};
            f_key = src_mem[f_ls + (f_npx / 2)];        // 取行中真实字作色键
            f_seg2 = 0;
            f_seg1 = (f_ld + f_npx + 3) / 4;
        end
    endtask

    integer n, ls_i, ld_i, k, i;
    initial begin
        bad = 0; checked = 0; rows_run = 0;
        gap_err = 0; len_err = 0; last_err = 0; keep_err = 0;
        stall_head = 0; stall_mid = 0; rows_mid = 0;
        start = 1'b0; lb_we = 1'b0; lb_src_sel = 1'b0; lb_waddr = 9'd0; lb_wdata = 128'd0;

        repeat (3) @(posedge clk);
        rst = 1'b0;
        repeat (2) @(posedge clk);

        // ---------------- sweep：npx 1..25 × ls × ld（BLIT），方程/格式/色键/半号滚动
        for (n = 1; n <= 25; n = n + 1)
            for (ls_i = 0; ls_i < 4; ls_i = ls_i + 1)
                for (ld_i = 0; ld_i < 4; ld_i = ld_i + 1) begin
                    f_npx = n; f_ls = ls_i[1:0]; f_ld = ld_i[1:0];
                    half = ((n + ls_i + ld_i) % 2) ? 2'd1 : 2'd0;
                    gen_row(0, (n + ls_i + ld_i) % 6, (n + ls_i) % 3, ((n + ld_i) % 3) != 0);
                    run_row;
                end
        $display("sweep done: %0d rows, %0d slots checked", rows_run, checked);

        // ---------------- fill：六个方程；源 = w4 常量，不读源块；FILL 无色键
        rows_run = 0;
        for (k = 0; k < 6; k = k + 1) begin
            f_npx = 3 + k * 5;                 // 3,8,13,18,23,28：各种奇偶尾
            f_ls = 2'd0;                       // FILL 无源行，ls 无意义
            f_ld = (k * 3) % 4;                // 0,3,2,1,0,3：四种相位都覆盖
            half = k[0];
            // spec：FILL 的 sfmt 由方程决定——ALPHA 用 PM、ALPHA_STRAIGHT 用 ST，其余 XRGB（规范条款 §4.2，exec_sw.c 337~339 行）
            gen_row(1, k[2:0], (k == 1) ? 2'd1 : (k == 5) ? 2'd2 : 2'd0, 1'b0);
            run_row;
        end
        $display("fill done: %0d rows", rows_run);

        // ---------------- seg：拆两段，tlast 两处、段间不断
        rows_run = 0;
        for (k = 0; k < 4; k = k + 1) begin
            f_npx = 21 + k * 3;
            f_ls = (k + 1) % 4; f_ld = (k + 2) % 4;
            half = k[0];
            gen_row(0, k[2:0] + ((k == 3) ? 1 : 0), k % 3, (k % 2) != 0);
            f_seg1 = (f_ld + f_npx + 3) / 4 / 2 + 1;
            f_seg2 = (f_ld + f_npx + 3) / 4 - f_seg1;
            run_row;
        end
        $display("seg done: %0d rows", rows_run);

        // ---------------- long：253 与 640 像素各四行，相位轮换（640 像素、ld≠0 是 161 拍）
        rows_run = 0;
        for (k = 0; k < 8; k = k + 1) begin
            f_npx = (k < 4) ? 253 : 640;
            f_ls = (k * 3 + 1) % 4; f_ld = k % 4;
            half = k[0];
            gen_row(0, k % 6, k % 3, k[0]);
            run_row;
        end
        $display("long done: %0d rows", rows_run);

`ifdef REALWR
        // 等写核把最后一个描述符的状态交回来
        i = 0;
        while ((wd_done < wd_sent || wd_valid) && i < 20000) begin @(posedge clk); i = i + 1; end
        if (wd_done != wd_sent || wd_err != 0) begin
            bad = bad + 1;
            $display("  DESC sent=%0d done=%0d err=%0d", wd_sent, wd_done, wd_err);
        end
        $display("realwr: %0d descriptors, stall cycles: head %0d, mid %0d (rows with mid stalls %0d)",
                 wd_sent, stall_head, stall_mid, rows_mid);
`else
        $display("const-ready: stall cycles: head %0d, mid %0d", stall_head, stall_mid);
`endif

        if (`INJECT_VAL == 0) begin
            if (bad == 0)
                $display("RESULT: PASS (%0d slots checked)", checked);
            else
                $display("RESULT: FAIL (%0d mismatches, gap=%0d len=%0d last=%0d keep=%0d)",
                         bad, gap_err, len_err, last_err, keep_err);
        end else begin
            // 注错构建：有红才算对（run.sh 判「RESULT: FAIL」）
            if (bad > 0)
                $display("RESULT: FAIL (inject %0d: %0d mismatches, as expected)", `INJECT_VAL, bad);
            else
                $display("RESULT: NOT_RED (inject %0d: no mismatch caught)", `INJECT_VAL);
        end
        $finish;
    end
endmodule

`default_nettype wire
