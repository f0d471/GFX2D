// tb_engine_tex：v1-TEX（G1）的正式判据，两个层级
//
// 单元级 engine_tex（INJECT 可注错，见下）：
//   APB-LOAD    4096 字装载/逐字读回（首末地址与相邻字隔离靠逐字不同的图案）
//   APB-UNALIGN 未对齐读写回 PSLVERR 且不改 RAM
//   STR-*       读流：数据逐拍对参考、首拍延迟恒 2（bank 输出寄存器 + 输出寄存器）、
//               描述符内 valid 连续、tlast 落在末拍、buf/cidx 随流与状态带回、
//               状态在末拍之后
//   注错位（-DINJECT_TEX_VAL=n，注入构建只跑单元级，对应判据必须见红）：
//     1  valid 提前一级（数据错拍）→ STR 数据比对见红
//     2  未对齐写放行 → APB-UNALIGN 见红
//     4  状态吞掉 → STR 等状态看门狗见红
//
// 整机级 engine_top（INJECT 恒 0；软件执行器金标准，fixture 由
// tools/check/pix_cases_dump.c --tex-sprites 导出：40 颗 16×16 预乘 ALPHA，
// 前 20 颗源在片上 TEX、后 20 颗在 DDR，命令表 41 条跨两个取数批）：
//   RENDER      512 KiB 期望内存逐 128 位比对；PERF_RD_*/WR_* 与从机观测对账
//               （读事件只计真实 DDR 描述符与取数，TEX 不计入——G1 的口径）；
//               PIXEL 对软件统计；fe_cidx_err 恒 0
//   OVERLAP     取数预取在途与 TEX 像素读状态同拍的场面必须发生过（不串线）
//   ERR-*       BAND 源、TEX 目的、错对齐、越界、未登记槽的错误三元组
//   SOFT-RST    软复位排空后 TEX 内容保留：不重装载、只重登记再跑，内存仍逐字相同
//   HARD-RST    控制层复位打在渲染中途，全套重来仍正确
`timescale 1ns/1ps
`default_nettype none
`ifndef CASE
`define CASE "build/tex_sprites"
`endif

module tb_engine_tex #(
    parameter integer TEX_LATENCY = 1,
    parameter integer MOVER_INJECT = 0
);
    localparam integer CASE_WORDS = 32768;      // 512 KiB，与 --tex-sprites 的窗口一致
    // 命令表落点：512 KiB 从机窗口的顶部空白段（FB 到 0x61000、twin 到 0x66000；
    // 第一版放 0x40000 落进 FB 第 63 行，弹幕把命令表当像素写了——首跑靠
    // 批次取数的时序侥幸过关，软复位重跑就取到覆盖后的「命令表」）
    localparam [31:0] LBASE = 32'h0007_C000;
    localparam [31:0] ERRBASE = 32'h0007_CA00;  // 错误三元组用例的表落点

    reg clk = 0, rst = 1;
    always #5 clk = ~clk;

    `ifdef INJECT_TEX_VAL
    localparam integer TEX_INJ = `INJECT_TEX_VAL;
    `else
    localparam integer TEX_INJ = 0;
    `endif

    integer errors = 0;
    integer tck = 0;
    always @(posedge clk) tck = tck + 1;

    // ---------------------------------------------------------------- 共享参考
    reg [127:0] tex_ref [0:1023];               // 装载窗口的 16 KiB 内容

    // 函数：128 位参考字的第 i 个 32 位车道（write_words 的 w3w2w1w0 序）
    function [31:0] lane(input [127:0] w, input integer i);
        lane = w[32*i +: 32];
    endfunction

    // ================================================================
    // 单元级：engine_tex 独立实例
    // ================================================================
    reg          uapsel = 0, uapen = 0, uapwr = 0;
    reg [13:0]   uapaddr = 0;
    reg [31:0]   uapwdata = 0;
    wire [31:0]  uaprdata;
    wire         uapready, uapslverr;

    reg [13:0]   req_addr = 0;
    reg [11:0]   req_bytes = 0;
    reg [1:0]    req_buf = 0;
    reg [3:0]    req_cidx = 0;
    reg          req_valid = 0;
    wire         req_ready;
    wire [127:0] urs_tdata;
    wire         urs_tvalid, urs_tlast;
    wire [1:0]   urs_buf;
    wire         ustat_valid;
    reg          unit_stat_ready = 1;
    wire [3:0]   ustat_cidx;
    wire [1:0]   ustat_buf;
    wire         utex_busy;

    engine_tex #(.INJECT(TEX_INJ), .READ_LATENCY(TEX_LATENCY)) u_unit (
        .clk(clk), .rst(rst),
        .paddr(uapaddr), .psel(uapsel), .penable(uapen), .pwrite(uapwr),
        .pwdata(uapwdata), .prdata(uaprdata), .pready(uapready), .pslverr(uapslverr),
        .req_addr(req_addr), .req_bytes(req_bytes), .req_buf(req_buf),
        .req_cidx(req_cidx), .req_valid(req_valid), .req_ready(req_ready),
        .rs_tdata(urs_tdata), .rs_tvalid(urs_tvalid), .rs_tlast(urs_tlast),
        .rs_buf(urs_buf),
        .stat_valid(ustat_valid), .stat_ready(unit_stat_ready), .stat_cidx(ustat_cidx), .stat_buf(ustat_buf),
        .busy(utex_busy));

    task uapb_wr(input [13:0] a, input [31:0] d, output perr);
        begin
            @(posedge clk);
            uapsel <= 1'b1; uapen <= 1'b0; uapwr <= 1'b1; uapaddr <= a; uapwdata <= d;
            @(posedge clk);
            uapen <= 1'b1;
            @(posedge clk);
            perr = uapslverr;               // 边沿前值 = ACCESS 相的 PSLVERR
            uapsel <= 1'b0; uapen <= 1'b0; uapwr <= 1'b0;
        end
    endtask

    task uapb_rd(input [13:0] a, output [31:0] d, output perr);
        begin
            @(posedge clk);
            uapsel <= 1'b1; uapen <= 1'b0; uapwr <= 1'b0; uapaddr <= a;
            @(posedge clk);
            uapen <= 1'b1;
            @(posedge clk);
            d = uaprdata;
            perr = uapslverr;
            uapsel <= 1'b0; uapen <= 1'b0;
        end
    endtask

    // ---- 流监视：fire 后逐拍收数据、量延迟、对上下文 ----
    integer str_beats, str_lat, str_bad, str_last_seen, str_stat_ok;
    reg         mon_on = 0;
    reg [11:0]  mon_left = 0;
    reg [9:0]   mon_row = 0;
    reg [1:0]   mon_buf = 0;
    reg [3:0]   mon_cidx = 0;
    integer     mon_lat_cnt = 0;

    task unit_stream(input [13:0] addr, input [11:0] bytes,
                     input [1:0] bufid, input [3:0] cidx);
        begin
            str_beats = 0; str_lat = -1; str_bad = 0;
            str_last_seen = 0; str_stat_ok = 0;
            @(negedge clk);
            req_addr = addr; req_bytes = bytes; req_buf = bufid;
            req_cidx = cidx; req_valid = 1'b1;
            // 在受理沿采样 ready，不能等受理后已经拉低的 ready 再次变高。
            @(posedge clk);
            while (!req_ready) @(posedge clk);
            @(negedge clk);
            req_valid = 1'b0;
        end
    endtask

    always @(posedge clk) begin
        if (rst) begin
            mon_on <= 1'b0; mon_left <= 12'd0;
        end else begin
            if (req_valid && req_ready) begin
                mon_on      <= 1'b1;
                mon_left    <= req_bytes >> 4;
                mon_row     <= req_addr[13:4];
                mon_buf     <= req_buf;
                mon_cidx    <= req_cidx;
                mon_lat_cnt  = 0;
            end else if (mon_on && str_beats == 0 && !urs_tvalid) begin
                mon_lat_cnt = mon_lat_cnt + 1;      // fire 与首拍采样之间的空拍
            end
            if (mon_on && urs_tvalid) begin
                if (str_beats == 0)
                    str_lat = mon_lat_cnt;          // fire 到首拍采样的空拍数
                if (urs_tdata !== tex_ref[mon_row]) begin
                    str_bad = str_bad + 1;
                    if (str_bad <= 3)
                        $display("STR-DATA row=%0d beat=%0d got=%032x want=%032x",
                                 mon_row, str_beats, urs_tdata, tex_ref[mon_row]);
                end
                if (urs_buf !== mon_buf) begin
                    str_bad = str_bad + 1;
                    $display("STR-BUF got=%0d want=%0d", urs_buf, mon_buf);
                end
                str_beats = str_beats + 1;
                mon_left  <= mon_left - 12'd1;
                mon_row   <= mon_row + 10'd1;
                if (mon_left == 12'd1) begin
                    if (!urs_tlast) begin
                        str_bad = str_bad + 1;
                        $display("STR-LAST-MISSING");
                    end
                    str_last_seen = 1;
                    mon_on <= 1'b0;
                end else if (urs_tlast) begin
                    str_bad = str_bad + 1;
                    $display("STR-LAST-EARLY beat=%0d", str_beats);
                    mon_on <= 1'b0;
                end
            end
            if (ustat_valid) begin
                if (ustat_cidx !== mon_cidx || ustat_buf !== mon_buf) begin
                    str_bad = str_bad + 1;
                    $display("STR-STAT-CTX cidx=%0d buf=%0d want cidx=%0d buf=%0d",
                             ustat_cidx, ustat_buf, mon_cidx, mon_buf);
                end
                str_stat_ok = 1;
            end
        end
    end

    task wait_stream_done;
        integer guard;
        begin
            guard = 0;
            while ((!str_last_seen || !str_stat_ok) && guard < 20000) begin
                @(negedge clk);
                guard = guard + 1;
            end
            if (guard >= 20000) begin
                errors = errors + 1;
                $display("STR-WATCHDOG beats=%0d last=%0d stat=%0d",
                         str_beats, str_last_seen, str_stat_ok);
                if (TEX_INJ == 4) begin
                    if (errors == 1 && str_beats == 1 && str_last_seen &&
                        !str_stat_ok && str_bad == 0)
                        $display("TEX INJECT 4 STATUS_MISSING");
                    else
                        $display("TEX INJECT NOT_RED");
                    $finish;
                end
            end
        end
    endtask

    task check_stream(input [255:0] name, input integer want_beats);
        begin
            if (str_beats != want_beats || str_bad != 0 || str_lat != TEX_LATENCY) begin
                errors = errors + 1;
                $display("%0s FAIL beats=%0d/%0d bad=%0d lat=%0d",
                         name, str_beats, want_beats, str_bad, str_lat);
            end
        end
    endtask

    task unit_apb_cases;
        reg [31:0] d, want;
        reg perr;
        integer i, err0;
        begin
            err0 = errors;
            for (i = 0; i < 4096; i = i + 1) begin
                uapb_wr(i[13:0] << 2, lane(tex_ref[i >> 2], i & 3), perr);
                if (perr) begin
                    errors = errors + 1;
                    $display("APB-LOAD-ERR word=%0d", i);
                end
            end
            for (i = 0; i < 4096; i = i + 1) begin
                uapb_rd(i[13:0] << 2, d, perr);
                want = lane(tex_ref[i >> 2], i & 3);
                if (perr || d !== want) begin
                    errors = errors + 1;
                    if (errors - err0 <= 5)
                        $display("APB-RD-MISMATCH word=%0d got=%08x want=%08x perr=%b",
                                 i, d, want, perr);
                end
            end
            uapb_wr(14'h0002, 32'hDEAD_BEEF, perr);
            if (!perr) begin errors = errors + 1; $display("APB-UNALIGN-WR no pslverr"); end
            uapb_rd(14'h0002, d, perr);
            if (!perr) begin errors = errors + 1; $display("APB-UNALIGN-RD no pslverr"); end
            uapb_rd(14'h0000, d, perr);
            if (perr || d !== lane(tex_ref[0], 0)) begin
                errors = errors + 1;
                $display("APB-UNALIGN-SIDE-EFFECT got=%08x", d);
            end
            $display("UNIT APB done (errors=%0d)", errors);
        end
    endtask

    task unit_stream_cases;
        begin
            unit_stream(14'h0000, 12'd16, 2'd0, 4'd1);
            wait_stream_done; check_stream("STR-1B", 1);
            unit_stream(14'h3FC0, 12'd64, 2'd1, 4'd2);
            wait_stream_done; check_stream("STR-4B", 4);
            unit_stream(14'h0000, 12'd3072, 2'd2, 4'd3);
            wait_stream_done; check_stream("STR-MAX", 192);
            unit_stream(14'h0050, 12'd32, 2'd1, 4'd4);
            wait_stream_done;
            unit_stream(14'h1F40, 12'd16, 2'd0, 4'd5);
            wait_stream_done; check_stream("STR-CTX", 1);
            @(negedge clk); unit_stat_ready = 0;
            unit_stream(14'h0000, 12'd16, 2'd1, 4'd9);
            wait (ustat_valid);
            repeat (4) begin
                @(negedge clk);
                if (!ustat_valid || ustat_cidx != 9 || ustat_buf != 1 || !utex_busy || req_ready)
                    begin errors=errors+1; $display("STR-STATUS-HOLD FAIL"); end
            end
            unit_stat_ready = 1;
            @(negedge clk);
            $display("UNIT STR done (errors=%0d)", errors);
        end
    endtask

    // ================================================================
    // 整机级：engine_top（与 frame 台同构）
    // ================================================================
    reg psel = 0, penable = 0, pwrite = 0;
    reg [15:0] paddr = 0;
    reg [31:0] pwdata = 0;
    wire [31:0] prdata;
    wire pready, pslverr;

    wire [31:0] araddr, awaddr;
    wire [7:0] arlen, awlen;
    wire [2:0] arsize, awsize;
    wire [1:0] arburst, awburst;
    wire arvalid, arready, awvalid, awready;
    wire [127:0] rdata, wdata;
    wire [1:0] rresp, bresp;
    wire rlast, rvalid, rready, wlast, wvalid, wready, bvalid, bready;
    wire [15:0] wstrb;
    wire irq_b, fe_cidx_err;

    engine_top #(.TEX_READ_LATENCY(TEX_LATENCY), .MOVER_INJECT(MOVER_INJECT)) u_eng(
        .clk(clk), .rst_ctrl(rst), .rst_ddr_n(!rst),
        .paddr(paddr), .psel(psel), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(prdata), .pready(pready), .pslverr(pslverr),
        .m_axi_araddr(araddr), .m_axi_arlen(arlen), .m_axi_arsize(arsize),
        .m_axi_arburst(arburst), .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid), .m_axi_rready(rready),
        .m_axi_awaddr(awaddr), .m_axi_awlen(awlen), .m_axi_awsize(awsize),
        .m_axi_awburst(awburst), .m_axi_awvalid(awvalid), .m_axi_awready(awready),
        .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wlast(wlast),
        .m_axi_wvalid(wvalid), .m_axi_wready(wready),
        .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
        .irq_b(irq_b), .dbg_fe_cidx_err(fe_cidx_err));

    axi_ram_model #(.MEM_WORDS(CASE_WORDS), .STRB_EN(1), .STALL(0), .TIMED(1),
                    .RD_LAT(23), .RD_BUB(12), .WR_BUB(6), .B_LAT(1)) u_ram(
        .clk(clk), .rst_n(!rst),
        .awaddr(awaddr), .awlen(awlen), .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(wstrb), .wlast(wlast), .wvalid(wvalid),
        .wready(wready), .bresp(bresp), .bvalid(bvalid), .bready(bready),
        .araddr(araddr), .arlen(arlen), .arvalid(arvalid), .arready(arready),
        .rdata(rdata), .rresp(rresp), .rlast(rlast), .rvalid(rvalid),
        .rready(rready), .hold_aw(1'b0), .hold_ar(1'b0));

    integer obs_rd_burst = 0, obs_rd_beat = 0, obs_wr_burst = 0, obs_wr_beat = 0;
    integer overlap_pix_stat = 0, overlap_requests = 0, ddr_data_held = 0;
    reg prev_fe_stat = 0, prev_fetch_status = 0;
    reg [1:0] prev_fe_state = 0;
    always @(posedge clk) if (!rst) begin
        if (arvalid && arready) obs_rd_burst = obs_rd_burst + 1;
        if (rvalid && rready)   obs_rd_beat  = obs_rd_beat + 1;
        if (awvalid && awready) obs_wr_burst = obs_wr_burst + 1;
        if (wvalid && wready)   obs_wr_beat  = obs_wr_beat + 1;
        // 像素读状态到达时取数不应在途：取数优先 + 单读所有者（fd 举着时新像素
        // 描述符发不出、取数要等在途像素收尾）在结构上排除同拍。若有人削弱
        // 仲裁，这里会红——cmd 的显式来源位（rd_stat_fetch）是第二道保险。
        if (u_eng.rd_stat_valid && !u_eng.rd_stat_fetch && u_eng.u_cmd.fe_st != 3'd0)
            overlap_pix_stat = overlap_pix_stat + 1;
        if (u_eng.tx_busy && arvalid && arready) overlap_requests=overlap_requests+1;
        if (u_eng.tx_busy && u_eng.u_mover.core_rs_tvalid && !u_eng.u_mover.rd_ctx_cmd &&
            !u_eng.u_mover.core_rs_tready) ddr_data_held=ddr_data_held+1;
        if (u_eng.u_mover.core_rs_pix && u_eng.u_mover.tex_rs_pix)
            begin
                errors=errors+1; $display("PIXEL_STREAM_COLLISION");
                if (MOVER_INJECT == 64) begin $display("TEX MIX INJECT red"); $finish; end
            end
        if (prev_fe_state == 2'd2 && !prev_fe_stat && u_eng.u_cmd.fe_stat && !prev_fetch_status)
            begin errors=errors+1; $display("FETCH_STATUS_ATTRIBUTION"); end
        prev_fe_stat <= u_eng.u_cmd.fe_stat;
        prev_fe_state <= u_eng.u_cmd.fe_st;
        prev_fetch_status <= u_eng.rd_stat_valid && u_eng.rd_stat_fetch;
    end

    task apb_wr(input [15:0] a, input [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b1; paddr <= a; pwdata <= d;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            if (pslverr !== 1'b0) begin
                errors = errors + 1;
                $display("APB-FAIL wr %05x pslverr=%b (en=%b busy=%b pend=%b)", a, pslverr,
                         u_eng.u_regs.ctrl_en, u_eng.cmd_busy, u_eng.u_regs.soft_pending);
            end
            psel <= 1'b0; penable <= 1'b0; pwrite <= 1'b0;
        end
    endtask

    task apb_rd(input [15:0] a, output [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b0; paddr <= a;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            d = prdata;
            psel <= 1'b0; penable <= 1'b0;
        end
    endtask

    task mwrite(input [31:0] a, input [31:0] w);
        reg [127:0] old;
        integer sh;
        begin
            sh = {a[3:2], 2'b00} * 8;
            old = u_ram.mem[a[21:4]];
            u_ram.mem[a[21:4]] = (old & ~(128'hFFFF_FFFF << sh)) | ({96'd0, w} << sh);
        end
    endtask

    reg [95:0]  surf_rom [0:15];
    reg [255:0] cmd_rom [0:73];
    reg [127:0] exp_mem [0:CASE_WORDS-1];
    reg [127:0] init_mem [0:CASE_WORDS-1];
    reg [31:0]  cnt_hex [0:2];
    reg [31:0]  stats_hex [0:2];

    task load_surfaces;
        integer i;
        begin
            for (i = 0; i < 16; i = i + 1) begin
                apb_wr(16'h0040 + i*12 + 0, surf_rom[i][31:0]);
                apb_wr(16'h0040 + i*12 + 4, surf_rom[i][63:32]);
                apb_wr(16'h0040 + i*12 + 8, surf_rom[i][95:64]);
            end
        end
    endtask

    task upload_tex;
        integer i;
        reg [31:0] d;
        begin
            for (i = 0; i < 4096; i = i + 1)
                apb_wr(16'h4000 + i*4, lane(tex_ref[i >> 2], i & 3));
            for (i = 0; i < 4096; i = i + 1023) begin
                apb_rd(16'h4000 + i*4, d);
                if (d !== lane(tex_ref[i >> 2], i & 3)) begin
                    errors = errors + 1;
                    $display("TEX-UPLOAD-MISMATCH word=%0d", i);
                end
            end
        end
    endtask

    task submit(input [31:0] base, input [31:0] cnt, output [31:0] code);
        reg [31:0] s;
        integer g;
        begin
            apb_wr(16'h0020, base);
            apb_wr(16'h0024, cnt);
            apb_wr(16'h0028, 32'h1);
            g = 0; s = 32'h0;
            while (((s & 32'hC) == 32'd0 || (s & 32'h1) != 0) && g < 2000000 && !rst) begin
                apb_rd(16'h000C, s);
                g = g + 1;
            end
            if (g >= 2000000) begin
                errors = errors + 1;
                $display("SUBMIT-TIMEOUT status=%08x", s);
                code = 32'hdead;
            end else if (rst) begin
                code = 32'hffff;                // 被复位打断：调用方不检查
            end else begin
                code = s[7:4];
            end
        end
    endtask

    task load_cmds(input integer n, input [31:0] base);
        integer i, j;
        begin
            for (i = 0; i < n; i = i + 1)
                for (j = 0; j < 8; j = j + 1)
                    mwrite(base + i*32 + j*4, cmd_rom[i][j*32 +: 32]);
        end
    endtask

    task compare_mem(input [255:0] tag);
        integer i, bad, first;
        begin
            bad = 0; first = -1;
            for (i = 0; i < CASE_WORDS; i = i + 1)
                // 命令表落点（LBASE 的 73 条与 ERRBASE 的 2 条）不在期望模型里
                if ((i >= LBASE[21:4] && i < LBASE[21:4] + 150) ||
                    (i >= ERRBASE[21:4] && i < ERRBASE[21:4] + 8))
                    ;
                else if (u_ram.mem[i] !== exp_mem[i]) begin
                    if (first < 0) first = i;
                    bad = bad + 1;
                end
            if (bad != 0) begin
                errors = errors + 1;
                $display("%0s FAIL mem_bad=%0d first_word=%0d", tag, bad, first);
                $display("  got=%032h want=%032h (word %0d = byte %x)",
                         u_ram.mem[first], exp_mem[first], first, first*16);
                for (i = 0; i < CASE_WORDS; i = i + 1)
                    if (u_ram.mem[i] === exp_mem[i]) begin
                        $display("  first_match_word=%0d (byte %x)", i, i*16);
                        i = CASE_WORDS;
                    end
            end
        end
    endtask

    // ---- 错误三元组：BLIT 源1→目的0（8x2），表面按原始字改写；w7=0、索引 0 ----
    task err_probe(input [31:0] s1b, input [31:0] s1d, input [31:0] s1c,
                   input [31:0] s0b, input [31:0] s0d, input [31:0] s0c,
                   input [31:0] exp_code, input [255:0] tag);
        reg [31:0] code, eidx, etag;
        begin
            apb_wr(16'h0040 + 1*12 + 0, s1b);
            apb_wr(16'h0040 + 1*12 + 4, s1d);
            apb_wr(16'h0040 + 1*12 + 8, s1c);
            apb_wr(16'h0040 + 0*12 + 0, s0b);
            apb_wr(16'h0040 + 0*12 + 4, s0d);
            apb_wr(16'h0040 + 0*12 + 8, s0c);
            mwrite(ERRBASE + 0,  32'h0001_0102);    // w0：BLIT、ALPHA、src=1、dst=0
            mwrite(ERRBASE + 4,  32'h0000_0000);    // w1 源坐标
            mwrite(ERRBASE + 8,  32'h0000_0000);    // w2 目的坐标
            mwrite(ERRBASE + 12, 32'h0002_0008);    // w3 宽 8 高 2
            mwrite(ERRBASE + 28, 32'h0000_0000);    // w7 标签 0
            mwrite(ERRBASE + 32, 32'h0000_00FF);    // 第二条：END
            submit(ERRBASE, 2, code);
            apb_rd(16'h0018, eidx);
            apb_rd(16'h001C, etag);
            if (code !== exp_code || eidx !== 0 || etag !== 0) begin
                errors = errors + 1;
                $display("%0s FAIL code=%0d idx=%0d tag=%0d", tag, code, eidx, etag);
            end
        end
    endtask

    // ---------------------------------------------------------------- 主流程
    integer perf_rd_burst, perf_rd_beat, perf_wr_burst, perf_wr_beat, perf_pixel;
    reg [31:0] code, id, caps, eidx, etag;
    integer i;
    reg [31:0] d;
    reg perr;

    initial begin
        $readmemh({`CASE, ".tex.hex"}, tex_ref);
        if (TEX_INJ == 0) begin
            $readmemh({`CASE, ".cmd.hex"}, cmd_rom);
            $readmemh({`CASE, ".surf.hex"}, surf_rom);
            $readmemh({`CASE, ".exp.hex"}, exp_mem);
            $readmemh({`CASE, ".init.hex"}, init_mem);
            $readmemh({`CASE, ".cnt.hex"}, cnt_hex);
            $readmemh({`CASE, ".stats.hex"}, stats_hex);
        end

        // ---------------- 单元级 ----------------
        repeat (5) @(negedge clk); rst = 0;
        repeat (2) @(negedge clk);
        unit_apb_cases;
        unit_stream_cases;
        if (TEX_INJ != 0) begin
            $display("TEX INJECT %0d red=%0d", TEX_INJ, errors);
            if (errors == 0) $display("TEX INJECT NOT_RED");
            $finish;
        end
        $display("UNIT done errors=%0d", errors);

        // ---------------- 整机级：渲染 + PERF 对账 ----------------
        rst = 1;
        repeat (5) @(negedge clk); rst = 0;
        repeat (2) @(negedge clk);

        apb_rd(16'h0000, id);
        if (id !== 32'h4746_5801) begin errors = errors + 1; $display("ID-FAIL %08x", id); end
        apb_rd(16'h0004, caps);
        if (caps !== 32'h0000_044F) begin errors = errors + 1; $display("CAPS-FAIL %08x", caps); end

        for (i = 0; i < CASE_WORDS; i = i + 1) u_ram.mem[i] = init_mem[i];
        upload_tex;
        load_surfaces;
        load_cmds(cnt_hex[0], LBASE);
        apb_wr(16'h0008, 32'h1);            // CTRL.EN

        submit(LBASE, cnt_hex[0], code);
        if (code != 0) begin
            errors = errors + 1;
            $display("RENDER-ERR code=%0d", code);
        end

        apb_rd(16'h0100 + 4*1, perf_pixel);
        apb_rd(16'h0100 + 4*3, perf_rd_burst);
        apb_rd(16'h0100 + 4*5, perf_rd_beat);
        apb_rd(16'h0100 + 4*4, perf_wr_burst);
        apb_rd(16'h0100 + 4*6, perf_wr_beat);
        if (perf_rd_burst !== obs_rd_burst) begin
            errors = errors + 1;
            $display("RD-BURST-MISMATCH perf=%0d obs=%0d", perf_rd_burst, obs_rd_burst);
        end
        if (perf_rd_beat !== obs_rd_beat) begin
            errors = errors + 1;
            $display("RD-BEAT-MISMATCH perf=%0d obs=%0d", perf_rd_beat, obs_rd_beat);
        end
        if (perf_wr_burst !== obs_wr_burst || perf_wr_beat !== obs_wr_beat) begin
            errors = errors + 1;
            $display("WR-MISMATCH perf=%0d/%0d obs=%0d/%0d",
                     perf_wr_burst, perf_wr_beat, obs_wr_burst, obs_wr_beat);
        end
        if (perf_pixel !== stats_hex[0] - stats_hex[1]) begin
            errors = errors + 1;
            $display("PIXEL-MISMATCH perf=%0d exp=%0d", perf_pixel, stats_hex[0] - stats_hex[1]);
        end
        // 源 DDR 事务消失（G1）：24 颗 TEX 源的 192 行不得出现在读事务里
        // （DDR 侧：48×8 源行 + 72×8 目的行 + 5 笔取数，批次 = ≤16 条）
        if (perf_rd_burst != 384 + 576 + 1 + 5) begin
            errors = errors + 1;
            $display("RD-BURST-TOO-MANY %0d", perf_rd_burst);
        end
        if (fe_cidx_err !== 1'b0) begin
            errors = errors + 1;
            $display("FE-CIDX-ERR");
        end
        if (overlap_requests == 0 || ddr_data_held == 0 || fe_cidx_err) begin
            errors = errors + 1;
            $display("TEX_OVERLAP_NOT_EXERCISED ar=%0d hold=%0d cidx=%b",overlap_requests,ddr_data_held,fe_cidx_err);
        end else begin
            $display("TEX_OVERLAP ar=%0d hold=%0d pixel_stat_during_fetch=%0d",overlap_requests,ddr_data_held,overlap_pix_stat);
        end
        compare_mem("RENDER");
        $display("RENDER done errors=%0d", errors);

        // ---- 错误三元组 ----
        // 合法 8x2 面：dim=(2<<16)|8；ctrl=valid|stride 8|fmt|space
        err_probe(32'h0000_0000, 32'h0002_0008, 32'h8000_1108 | (2 << 8),
                  32'h0000_1000, 32'h0002_0008, 32'h8000_1008,
                  32'd5, "ERR-BAND-SRC");
        err_probe(32'h0000_0000, 32'h0002_0008, 32'h8000_1108,
                  32'h0000_0000, 32'h0002_0008, 32'h8000_1108,
                  32'd5, "ERR-TEX-DST");
        err_probe(32'h0000_0004, 32'h0002_0008, 32'h8000_1108,
                  32'h0000_1000, 32'h0002_0008, 32'h8000_1008,
                  32'd4, "ERR-TEX-ALIGN");
        err_probe(32'h0000_3000, 32'h0040_0040, 32'h8000_1108,
                  32'h0000_1000, 32'h0002_0008, 32'h8000_1008,
                  32'd4, "ERR-TEX-OOB");
        err_probe(32'h0000_0000, 32'h0002_0008, 32'h0000_1008,
                  32'h0000_1000, 32'h0002_0008, 32'h8000_1008,
                  32'd4, "ERR-NO-VALID");
        $display("ERR cases done errors=%0d", errors);

        // ---- 软复位排空：TEX 内容保留，重登记后重跑仍逐字相同 ----
        for (i = 0; i < CASE_WORDS; i = i + 1) u_ram.mem[i] = init_mem[i];
        load_cmds(cnt_hex[0], LBASE);        // 重载把命令表也清了，重写
        obs_rd_burst = 0; obs_rd_beat = 0; obs_wr_burst = 0; obs_wr_beat = 0;
        apb_wr(16'h0008, 32'h3);            // CTRL.EN | SOFT_RST（写 1 自清，排空完成才撤）
        gwait_drain;
        load_surfaces;                       // 软复位清了 valid：重登记（不重装载）
        submit(LBASE, cnt_hex[0], code);
        if (code != 0) begin
            apb_rd(16'h0018, eidx);
            apb_rd(16'h001C, etag);
            errors = errors + 1;
            $display("SOFT-RERUN-ERR code=%0d index=%0d tag=%0d", code, eidx, etag);
        end
        compare_mem("SOFT-RST");
        $display("SOFT-RST done errors=%0d", errors);

        // ---- 控制层复位打在渲染中途 ----
        fork
            begin
                submit(LBASE, cnt_hex[0], code);   // 应被打断：不检查 code
            end
            begin
                repeat (3000) @(negedge clk);
                rst = 1;
                repeat (10) @(negedge clk);
                rst = 0;
            end
        join
        repeat (5) @(negedge clk);
        for (i = 0; i < CASE_WORDS; i = i + 1) u_ram.mem[i] = init_mem[i];
        obs_rd_burst = 0; obs_rd_beat = 0; obs_wr_burst = 0; obs_wr_beat = 0;
        upload_tex;
        load_surfaces;
        load_cmds(cnt_hex[0], LBASE);
        apb_wr(16'h0008, 32'h1);
        submit(LBASE, cnt_hex[0], code);
        if (code != 0) begin
            errors = errors + 1;
            $display("HARD-RERUN-ERR code=%0d", code);
        end
        compare_mem("HARD-RST");
        $display("HARD-RST done errors=%0d", errors);

        if (errors == 0)
            $display("TEX SIM OK: unit + render + perf + err + resets");
        else
            $display("TEX SIM FAIL errors=%0d", errors);
        $finish;
    end

    task gwait_drain;
        reg [31:0] s;
        integer g;
        begin
            g = 0;
            do begin
                apb_rd(16'h000C, s);
                g = g + 1;
            end while (((s & 32'h3) != 0) && g < 100000);
            if (g >= 100000) begin
                errors = errors + 1;
                $display("DRAIN-TIMEOUT status=%08x", s);
            end
        end
    endtask

    // 看门狗
    initial begin
        #40000000;
        $display("TEX WATCHDOG (errors=%0d)", errors);
        $fatal(1, "TEX WATCHDOG");
    end
endmodule
`default_nettype wire
