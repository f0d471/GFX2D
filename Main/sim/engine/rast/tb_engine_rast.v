// R2 全链测试台：engine_rlst + engine_rast + engine_band + engine_tex
// 经 engine_mover 直挂 axi_ram_model（本 sim 不做地址译码，故不经 Axi_Mux）。
//
// 用例（+define+ 选择，见 run.sh）：
//   CASE_RAND      流式回放 rast_stream.py 导出的记录流（两帧，默认共 100000 条），
//                  逐条带终态与 exp.bin 折叠期望逐像素比较（颜色+z+高位 0）
//   CASE_DIRECTED  三种跨 4KB 拆分形态（96+32/64+64/32+96，用 R1 具名向量记录）、
//                  索引越界、保留位非 0、空子表
// 注错构建复用上述用例，按各自 witness 见红（清单见 engine-manual.md）。
// 期望由 tools/check/rast_stream.py 生成（R1 同种子，前 16 条与归档向量逐位同源）。
// 完成判据与 cmd 一致：拍数收齐 + 状态脉冲（DMA 的 tlast 按 16 词突发组给，不作依据）。
`timescale 1ns/1ps
`default_nettype none

module tb_engine_rast;
    reg clk = 0;
    reg rst = 1;
    always #5 clk = ~clk;

    localparam [31:0] SUB_BASE = 32'h0010_0000;   // 16B 对齐
    localparam [31:0] TRI_BASE = 32'h0080_0000;   // 32B 对齐
    localparam integer MEM_WORDS = 2**21;
    integer errors = 0;

    // ---------------------------------------------------------------- FD 替身
    // 协议正确的描述符响应器：按 fd_bytes 从本地主存流 128b 拍（末拍 fs_last）、
    // 随机反压、描述符完成后一拍状态脉冲；主表/子表都在本地主存里。
    reg [127:0] busmem [0:1441791];   // 覆盖 SUB_BASE..TRI_BASE+12.8MB（字 = 16B）
    wire [31:0] m_fd_addr; wire [11:0] m_fd_bytes; wire [3:0] m_fd_cidx;
    wire m_fd_valid, m_fd_ready;
    wire [127:0] m_fs_data; wire m_fs_valid, m_fs_last, m_fs_ready;
    wire [127:0] rp_wd_w;            // rlst 的词流出口（直通 fs_data；勿与 fs 数据线同网，避免组合环）
    wire m_st_valid, m_st_error;
    wire        fd_fire = m_fd_valid && m_fd_ready;
    reg         s_active = 1'b0;
    reg [31:0]  s_addr;
    reg [11:0]  s_left;
    reg  [2:0]  s_dly;
    reg         s_stat_q = 1'b0;
    reg         s_err_q = 1'b0;
    wire        s_go = s_active && (s_dly == 3'd0);
    assign m_fs_data  = busmem[s_addr[31:4]];
    assign m_fs_valid = s_go;
    assign m_fs_last  = s_go && (s_left == 12'd16);
    assign m_fd_ready = !s_active && !s_stat_q;
    assign m_st_valid = s_stat_q;
    assign m_st_error = s_err_q;
    integer seedv = 9;
    always @(posedge clk) begin
        if (rst) begin
            s_active <= 1'b0;
            s_stat_q <= 1'b0;
        end else begin
            st_pulse_r <= 1'b0;
            if (s_stat_q) begin
                s_stat_q <= 1'b0;
                st_pulse_r <= 1'b1;
            end
            if (s_active) begin
                if (s_dly != 3'd0) s_dly <= s_dly - 3'd1;
                else if (m_fs_valid && m_fs_ready) begin
                    s_addr <= s_addr + 32'd16;
                    s_left <= s_left - 12'd16;
                    if (s_left == 12'd16) begin
                        s_active <= 1'b0;
                        s_stat_q <= 1'b1;
                        s_err_q  <= 1'b0;
                    end
                end
            end else if (fd_fire) begin
                s_active <= 1'b1;
                s_addr   <= m_fd_addr;
                s_left   <= m_fd_bytes;
                s_dly    <= $random(seedv) & 3'd7;
            end
        end
    end
    reg st_pulse_r = 1'b0;


    // ---------------------------------------------------------------- rlst
    reg         rlst_go = 1'b0;
    reg  [31:0] tri_base = TRI_BASE;
    reg  [31:0] sub_base = SUB_BASE;
    reg  [23:0] sub_bytes = 24'd0;
    reg  [15:0] tri_count = 16'd0;
    wire        rp_we, rp_valid, strip_req, strip_done, frame_done, rlst_error;
    wire [1:0]  rp_slot, rp_slot_v, rp_free;
    wire [4:0]  rp_wa, rp_strip, cur_strip;
    wire [15:0] rp_index, rlst_error_index;
    wire [3:0]  rlst_error_code;
    wire [31:0] rlst_error_tag;
    wire        rlst_busy;

    engine_rlst u_rlst (
        .clk(clk), .rst(rst), .cancel(1'b0), .go(rlst_go),
        .tri_base(tri_base), .sub_base(sub_base), .sub_bytes(sub_bytes), .tri_count(tri_count),
        .fd_addr(m_fd_addr), .fd_bytes(m_fd_bytes), .fd_cidx(m_fd_cidx),
        .fd_raster(), .fd_valid(m_fd_valid), .fd_ready(m_fd_ready),
        .fs_data(m_fs_data), .fs_valid(m_fs_valid), .fs_last(m_fs_last),
        .fs_ready(m_fs_ready),
        .st_valid(st_pulse_r), .st_error(m_st_error),
        .rp_we(rp_we), .rp_slot(rp_slot), .rp_wa(rp_wa), .rp_wd(rp_wd_w),
        .rp_valid(rp_valid), .rp_slot_v(rp_slot_v), .rp_index(rp_index), .rp_strip(rp_strip),
        .rp_free(rp_free),
        .strip_req(strip_req), .cur_strip(cur_strip), .strip_ack(strip_ack_r),
        .strip_done(strip_done), .frame_done(frame_done),
        .error(rlst_error), .error_code(rlst_error_code),
        .error_index(rlst_error_index), .error_tag(rlst_error_tag), .busy(rlst_busy));

    // ---------------------------------------------------------------- rast
    wire        ras_wr_valid; wire [13:0] ras_wr_pix; wire [31:0] ras_wr_data;
    wire        ras_wr_ready;
    wire        z_clear_done; reg z_clear = 1'b0;
    wire        rast_err_valid; wire [3:0] rast_err_code;
    wire [15:0] rast_err_index; wire [31:0] rast_err_tag;
    wire [31:0] dbg_retire_cnt;
    reg  [31:0] retire_base = 32'd0;
`ifdef GFX_HAS_RAST_TEX
    wire        samp_valid, samp_ready, samp_rvalid;
    wire [11:0] samp_word;
    wire [31:0] samp_rdata;
`endif

    engine_rast #(
`ifdef RAST_TEX_LAT2
        .TEX_LATENCY(2)
`else
        .TEX_LATENCY(1)
`endif
    ) u_rast (
        .clk(clk), .rst(rst), .cancel(1'b0),
        .rp_we(rp_we), .rp_slot(rp_slot), .rp_wa(rp_wa), .rp_wd(rp_wd_w),
        .rp_valid(rp_valid), .rp_slot_v(rp_slot_v), .rp_index(rp_index), .rp_strip(rp_strip),
        .rp_free(rp_free),
        .ras_wr_valid(ras_wr_valid), .ras_wr_pix(ras_wr_pix),
        .ras_wr_data(ras_wr_data), .ras_wr_ready(ras_wr_ready),
        .z_clear(z_clear), .z_clear_busy(), .z_clear_done(z_clear_done),
`ifdef GFX_HAS_RAST_TEX
        .samp_valid(samp_valid), .samp_ready(samp_ready),
        .samp_word(samp_word), .samp_rvalid(samp_rvalid), .samp_rdata(samp_rdata),
        .tex_width(13'd64), .tex_height(13'd64), .tex_slog2(4'd6),
        .ras_key(32'hff000000),
`endif
        .err_valid(rast_err_valid), .err_code(rast_err_code),
        .err_index(rast_err_index), .err_tag(rast_err_tag),
        .dbg_retire_cnt(dbg_retire_cnt));

    // ---------------------------------------------------------------- band
    engine_band #(.WIDTH_PIXELS(640), .STRIP_H(16)) u_band (
        .clk(clk), .rst(rst), .cancel(1'b0), .err_clr(1'b0),
        .start(1'b0), .start_ready(), .flush(1'b0), .flush_ready(),
        .bg_base(32'd0), .dst_base(32'd0), .bg_slog2(5'd0), .dst_slog2(5'd0),
        .strip_y0(16'd0), .frame_height(16'd480),
        .loaded(), .flushed(), .active(), .busy(), .loading(), .flushing(),
        .error(), .error_code(),
        .dma_rd_addr(), .dma_rd_bytes(), .dma_rd_valid(), .dma_rd_ready(1'b0),
        .dma_rdata(128'd0), .dma_rvalid(1'b0), .dma_rlast(1'b0), .dma_rready(),
        .dma_rd_done(1'b0), .dma_rd_error(1'b0),
        .dma_wr_addr(), .dma_wr_bytes(), .dma_wr_valid(), .dma_wr_ready(1'b0),
        .dma_wdata(), .dma_wvalid(), .dma_wready(1'b0),
        .dma_wr_done(1'b0), .dma_wr_error(1'b0), .dma_idle(1'b1),
        .rd_addr(32'd0), .rd_bytes(12'd0), .rd_buf(2'd0), .rd_cidx(4'd0),
        .rd_valid(1'b0), .rd_ready(),
        .rs_data(), .rs_valid(), .rs_last(), .rs_buf(),
        .rd_stat_valid(), .rd_stat_error(), .rd_stat_cidx(), .rd_stat_buf(),
        .rd_stat_ready(1'b0), .rd_busy(),
        .wr_addr(32'd0), .wr_bytes(12'd0), .wr_cidx(4'd0), .wr_last_row(1'b0),
        .wr_valid(1'b0), .wr_ready(),
        .ws_data(128'd0), .ws_keep(16'd0), .ws_valid(1'b0), .ws_ready(),
        .wr_stat_valid(), .wr_stat_error(), .wr_stat_cidx(), .wr_stat_last_row(),
        .wr_stat_ready(1'b0), .wr_busy(),
        .ras_lease(1'b1), .ras_wr_pix(ras_wr_pix), .ras_wr_data(ras_wr_data),
        .ras_wr_valid(ras_wr_valid), .ras_wr_ready(ras_wr_ready));

    // ---------------------------------------------------------------- tex
`ifdef GFX_HAS_RAST_TEX
    wire [13:0] tx_addr; wire [11:0] tx_bytes; wire [1:0] tx_buf; wire [3:0] tx_cidx;
    wire tx_valid; wire tx_ready; wire [127:0] tx_rs_tdata;
    wire tx_rs_tvalid, tx_rs_tlast; wire [1:0] tx_rs_buf;
    wire tx_stat_valid, tx_stat_ready; wire [3:0] tx_stat_cidx; wire [1:0] tx_stat_buf;
    wire tx_busy;
    engine_tex #(
`ifdef RAST_TEX_LAT2
        .READ_LATENCY(2)
`else
        .READ_LATENCY(1)
`endif
    ) u_tex (
        .clk(clk), .rst(rst),
        .paddr(14'd0), .psel(1'b0), .penable(1'b0), .pwrite(1'b0), .pwdata(32'd0),
        .prdata(), .pready(), .pslverr(),
        .req_addr(tx_addr), .req_bytes(tx_bytes), .req_buf(tx_buf), .req_cidx(tx_cidx),
        .req_valid(tx_valid), .req_ready(tx_ready),
        .rs_tdata(tx_rs_tdata), .rs_tvalid(tx_rs_tvalid),
        .rs_tlast(tx_rs_tlast), .rs_buf(tx_rs_buf),
        .stat_valid(tx_stat_valid), .stat_ready(tx_stat_ready),
        .stat_cidx(tx_stat_cidx), .stat_buf(tx_stat_buf), .busy(tx_busy),
        .samp_valid(samp_valid), .samp_ready(samp_ready),
        .samp_word(samp_word), .samp_rvalid(samp_rvalid), .samp_rdata(samp_rdata));
    // atlas 左上 2×2 与 R1 纹理同布局（word = v*64+u → 行 0：词 0/1，行 1：词 64/65）
    initial begin
        u_tex.g_bank[0].mem[0]  = 32'hff000000;
        u_tex.g_bank[1].mem[0]  = 32'hff80ff40;
        u_tex.g_bank[0].mem[16] = 32'hff4080ff;
        u_tex.g_bank[1].mem[16] = 32'hffffffff;
    end
`endif

    // ---------------------------------------------------------------- 条带握手：req → 清 z → ack
    reg strip_ack_r = 1'b0;
    reg clr_pend = 1'b0;
    always @(posedge clk) begin
        if (rst) begin
            strip_ack_r <= 1'b0;
            clr_pend <= 1'b0;
            z_clear <= 1'b0;
        end else begin
            z_clear <= 1'b0;
            if (strip_req && !clr_pend) begin
                clr_pend <= 1'b1;
                z_clear <= 1'b1;
            end
            if (z_clear_done && clr_pend) begin
                clr_pend <= 1'b0;
                strip_ack_r <= 1'b1;
            end
            if (strip_done) strip_ack_r <= 1'b0;
        end
    end

    // ---------------------------------------------------------------- 文件装载与比较
    reg [127:0] fbuf [0:65535];      // frame.bin
    reg [127:0] ebuf [0:327679];     // exp.bin（2 帧）
    integer errors_dma_stat = 0;

    task swap_words;                 // $fread MSB 先入 → 总线字节序（就地反序）
        input integer count;
        integer i;
        reg [127:0] w, r;
        begin
            for (i = 0; i < count; i = i + 1) begin
                w = fbuf[i];
                r[127:120]=w[7:0];    r[119:112]=w[15:8];
                r[111:104]=w[23:16];  r[103:96] =w[31:24];
                r[95:88]  =w[39:32];  r[87:80]  =w[47:40];
                r[79:72]  =w[55:48];  r[71:64]  =w[63:56];
                r[63:56]  =w[71:64];  r[55:48]  =w[79:72];
                r[47:40]  =w[87:80];  r[39:32]  =w[95:88];
                r[31:24]  =w[103:96]; r[23:16]  =w[111:104];
                r[15:8]   =w[119:112];r[7:0]    =w[127:120];
                fbuf[i] = r;
            end
        end
    endtask

    task load_main;
        input [1023:0] path;
        integer fd, n, i;
        reg [127:0] w, r;
        begin
            fd = $fopen(path, "rb");
            if (fd == 0) begin $display("MAIN OPEN FAIL %0s", path); errors = errors + 1; end
            n = $fread(busmem, fd);
            $fclose(fd);
            for (i = 0; i < n / 16; i = i + 1) begin
                w = busmem[i];   // $fread 从 0 顺序填充
                r[127:120]=w[7:0];    r[119:112]=w[15:8];
                r[111:104]=w[23:16];  r[103:96] =w[31:24];
                r[95:88]  =w[39:32];  r[87:80]  =w[47:40];
                r[79:72]  =w[55:48];  r[71:64]  =w[63:56];
                r[63:56]  =w[71:64];  r[55:48]  =w[79:72];
                r[47:40]  =w[87:80];  r[39:32]  =w[95:88];
                r[31:24]  =w[103:96]; r[23:16]  =w[111:104];
                r[15:8]   =w[119:112];r[7:0]    =w[127:120];
                busmem[(TRI_BASE >> 4) + i] = r;
            end
            $display("MAIN LOADED %0d bytes", n);
        end
    endtask

    function [15:0] get_z;
        input integer pix;
        begin
            if ((pix >> 2) < 2048) get_z = u_rast.z_big[pix & 3][pix >> 2];
            else get_z = u_rast.z_small[pix & 3][(pix >> 2) - 2048];
        end
    endfunction

    function [31:0] get_band;
        input integer pix;
        integer w;
        begin
            w = pix >> 2;
            case (pix & 3)
                0: get_band = (w < 2048) ? u_band.ram_lane[0].mem_big[w]
                                         : u_band.ram_lane[0].mem_small[w-2048];
                1: get_band = (w < 2048) ? u_band.ram_lane[1].mem_big[w]
                                         : u_band.ram_lane[1].mem_small[w-2048];
                2: get_band = (w < 2048) ? u_band.ram_lane[2].mem_big[w]
                                         : u_band.ram_lane[2].mem_small[w-2048];
                default: get_band = (w < 2048) ? u_band.ram_lane[3].mem_big[w]
                                         : u_band.ram_lane[3].mem_small[w-2048];
            endcase
        end
    endfunction

    task clear_targets;              // 定向用例的背景（z=clear，色=背景色）
        integer r, l;
        begin
            for (r = 0; r < 2048; r = r + 1) begin
                u_band.ram_lane[0].mem_big[r] = 32'h12345678;
                u_band.ram_lane[1].mem_big[r] = 32'h12345678;
                u_band.ram_lane[2].mem_big[r] = 32'h12345678;
                u_band.ram_lane[3].mem_big[r] = 32'h12345678;
            end
            for (r = 0; r < 512; r = r + 1) begin
                u_band.ram_lane[0].mem_small[r] = 32'h12345678;
                u_band.ram_lane[1].mem_small[r] = 32'h12345678;
                u_band.ram_lane[2].mem_small[r] = 32'h12345678;
                u_band.ram_lane[3].mem_small[r] = 32'h12345678;
            end
            for (l = 0; l < 4; l = l + 1) begin
                for (r = 0; r < 2048; r = r + 1) u_rast.z_big[l][r] = 16'hFFFF;
                for (r = 0; r < 512; r = r + 1) u_rast.z_small[l][r] = 16'hFFFF;
            end
        end
    endtask

    // ---------------------------------------------------------------- 主流程
        initial begin
        rst = 1;
        repeat (20) @(posedge clk);
        rst = 0;
        repeat (10) @(posedge clk);
`ifdef CASE_RAND
        begin
            reg [1023:0] mainfile, framefile, expfile;
            if (!$value$plusargs("MAIN=%s", mainfile)) mainfile = "build/frame.main.bin";
            if (!$value$plusargs("FRAME=%s", framefile)) framefile = "build/frame.frame.bin";
            if (!$value$plusargs("EXP=%s", expfile)) expfile = "build/frame.exp.bin";
            load_main(mainfile);
            replay_frame(framefile, expfile, 0);
            replay_frame(framefile, expfile, 1);
        end
`endif
`ifdef CASE_DIRECTED
        run_directed;
`endif
        if (errors == 0) $display("RAST SIM OK");
        else $display("RAST SIM FAIL errors=%0d", errors);
        $finish;
    end

    initial begin
        #400_000_000;   // 看门狗（40M 周期）
        $display("RAST WATCHDOG TIMEOUT");
        $finish;
    end

`ifdef CASE_RAND
    // 一帧：装载第 fidx 帧的子表 → go → 收尾 → 终态逐像素比较
    task replay_frame;
        input [1023:0] framefile;
        input [1023:0] expfile;
        input integer fidx;
        integer fd, n, i, s, k;
        reg [127:0] wv, r;
        reg [31:0] magic, fbase, rec_off, sublen, expw;
        reg [31:0] ec;
        reg [15:0] ez;
        reg [31:0] got;
        reg [15:0] gz;
        reg bad;
        begin
            fd = $fopen(framefile, "rb");
            if (fd == 0) begin $display("FRAME OPEN FAIL"); errors = errors + 1; end
            n = $fread(fbuf, fd);
            $fclose(fd);
            swap_words((n + 15) / 16);
            magic = fbuf[0][31:0];
            if (magic !== 32'h52325354) begin
                $display("BAD MAGIC %08x", magic); errors = errors + 1;
            end
            fbase = 2 + fidx * 8;   // 文件头 2 词 + 帧头 8 词（offset+sublen+30 段偏移）
            rec_off = fbuf[fbase][31:0];
            sublen  = fbuf[fbase][63:32];
            for (i = 0; i < ((sublen + 15) >> 4); i = i + 1)
                busmem[(SUB_BASE >> 4) + i] = fbuf[fbase + 8 + i];
            $display("SUBW0=%h SUBW1=%h sublen=%0d", busmem[(SUB_BASE>>4)], busmem[(SUB_BASE>>4)+1], sublen);
            tri_base = TRI_BASE + (rec_off << 7);
            sub_base = SUB_BASE;
            sub_bytes = sublen[23:0];
            tri_count = 16'hFFFF;
            retire_base = dbg_retire_cnt;
            @(posedge clk); rlst_go <= 1'b1;
            @(posedge clk); rlst_go <= 1'b0;
            wait (frame_done === 1'b1);
            @(posedge clk);
            if (rlst_error) begin
                $display("FRAME%0d RLST ERROR code=%0d idx=%04x", fidx, rlst_error_code, rlst_error_index);
                errors = errors + 1;
            end
            // 期望：exp.bin 帧 fidx 的 30×5120 字
            fd = $fopen(expfile, "rb");
            n = $fread(ebuf, fd);
            $fclose(fd);
            for (i = 0; i < n / 16; i = i + 1) begin
                wv = ebuf[i];
                r[127:120]=wv[7:0];    r[119:112]=wv[15:8];
                r[111:104]=wv[23:16];  r[103:96] =wv[31:24];
                r[95:88]  =wv[39:32];  r[87:80]  =wv[47:40];
                r[79:72]  =wv[55:48];  r[71:64]  =wv[63:56];
                r[63:56]  =wv[71:64];  r[55:48]  =wv[79:72];
                r[47:40]  =wv[87:80];  r[39:32]  =wv[95:88];
                r[31:24]  =wv[103:96]; r[23:16]  =wv[111:104];
                r[15:8]   =wv[119:112];r[7:0]    =wv[127:120];
                ebuf[i] = r;
            end
            expw = fidx * 30 * 5120;
            bad = 1'b0;
            for (s = 0; s < 30; s = s + 1) begin
                for (k = 0; k < 10240; k = k + 1) begin
                    i = expw + s * 5120 + (k >> 1);
                    if (k[0] == 1'b0) begin
                        ec = ebuf[i][31:0]; ez = ebuf[i][63:32];
                    end else begin
                        ec = ebuf[i][95:64]; ez = ebuf[i][127:96];
                    end
                    got = get_band(k);
                    gz = get_z(k);
                    if (got[23:0] !== ec[23:0] || got[31:24] !== 8'd0 || gz !== ez) begin
                        if (!bad) begin
                            $display("MISMATCH f%0d strip=%0d pix=%0d got=%08x/%04x want=%06x/%04x",
                                     fidx, s, k, got, gz, ec[23:0], ez);
                            bad = 1'b1;
                        end
                        errors = errors + 1;
                    end
                end
            end
            if (!bad) $display("FRAME%0d COMPARE PASS retires=%0d", fidx, dbg_retire_cnt - retire_base);
        end
    endtask
`endif

    // ---------------------------------------------------------------- 定向用例
    // R1 具名向量 top_left_right 的 128B 记录（(0,0)(4,0)(0,4)，z=100，顶点色）
    function [127:0] rec_word;
        input integer i;
        begin
            case (i)
                0: rec_word = 128'h02000000000000000004000000000000;
                1: rec_word = 128'h00FCFFFF00FCFFFF0010000000040000;
                2: rec_word = 128'h00000000000000006400000000000000;
                3: rec_word = 128'h00000000000000000000000000000000;
                4: rec_word = 128'h00000000000000000000040000000400;
                5: rec_word = 128'h204080FF204080FF204080FF00000000;
                default: rec_word = 128'd0;
            endcase
        end
    endfunction

`ifdef CASE_DIRECTED
    task run_directed;
        begin
            directed_split(32'd4000);   // 96+32
            directed_split(32'd4032);   // 64+64
            directed_split(32'd4064);   // 32+96
            directed_bad_index;
            directed_rsvd;
            directed_empty;
        end
    endtask

    task directed_split;
        input [31:0] off;
        integer i;
        reg bad;
        begin
            clear_targets;
            for (i = 0; i < 8; i = i + 1)
                busmem[((TRI_BASE + off) >> 4) + i] = rec_word(i);
            busmem[(SUB_BASE >> 4)] = 128'd1;         // strip0 count=1 rsvd=0 idx0=0
            busmem[(SUB_BASE >> 4) + 1] = 128'd0;
            tri_base = TRI_BASE + off;
            sub_base = SUB_BASE;
            sub_bytes = 24'd8;
            tri_count = 16'd1;
            @(posedge clk); rlst_go <= 1'b1;
            @(posedge clk); rlst_go <= 1'b0;
            wait (frame_done === 1'b1);
            @(posedge clk);
            bad = 1'b0;
            if (rlst_error) begin $display("DIR+%0d RLST ERROR %0d", off, rlst_error_code); bad = 1; end
            for (i = 0; i < 16; i = i + 1) begin
                if (((i & 3) + (i >> 2)) < 4) begin
                    if (get_band(i) !== 32'h00804020 || get_z(i) !== 16'd100) bad = 1;
                end else begin
                    if (get_band(i) !== 32'h12345678 || get_z(i) !== 16'hFFFF) bad = 1;
                end
            end
            if (bad) begin
                $display("DIR+%0d CROSS-PAGE FAIL", off); errors = errors + 1;
            end else $display("DIR+%0d CROSS-PAGE PASS", off);
        end
    endtask

    task directed_bad_index;    // index ≥ tri_count → BAD_INDEX(13)
        begin
            clear_targets;
            busmem[(SUB_BASE >> 4)] = 128'd1;
            busmem[(SUB_BASE >> 4) + 1] = 128'd0;
            tri_base = TRI_BASE;
            sub_bytes = 24'd8;
            tri_count = 16'd0;
            @(posedge clk); rlst_go <= 1'b1;
            @(posedge clk); rlst_go <= 1'b0;
            wait (frame_done === 1'b1);
            @(posedge clk);
            if (rlst_error && rlst_error_code == 4'd13 && rlst_error_index == 16'd0)
                $display("DIR BAD_INDEX PASS");
            else begin
                $display("DIR BAD_INDEX FAIL code=%0d", rlst_error_code); errors = errors + 1;
            end
        end
    endtask

    task directed_rsvd;         // 子表保留位非 0 → BAD_INDEX
        begin
            clear_targets;
            busmem[(SUB_BASE >> 4)] = 128'h0000_0000_0001_0001; // count=1 rsvd=1
            busmem[(SUB_BASE >> 4) + 1] = 128'd0;
            tri_count = 16'd4;
            sub_base = SUB_BASE;
            sub_bytes = 24'd8;
            @(posedge clk); rlst_go <= 1'b1;
            @(posedge clk); rlst_go <= 1'b0;
            wait (frame_done === 1'b1);
            @(posedge clk);
            if (rlst_error && rlst_error_code == 4'd13) $display("DIR RSVD PASS");
            else begin $display("DIR RSVD FAIL"); errors = errors + 1; end
        end
    endtask

    task directed_empty;        // 空 subtable → 立即 frame_done 无错
        begin
            tri_base = TRI_BASE;
            sub_base = SUB_BASE;
            sub_bytes = 24'd0;
            tri_count = 16'd0;
            @(posedge clk); rlst_go <= 1'b1;
            @(posedge clk); rlst_go <= 1'b0;
            wait (frame_done === 1'b1);
            @(posedge clk);
            if (!rlst_error) $display("DIR EMPTY PASS");
            else begin $display("DIR EMPTY FAIL"); errors = errors + 1; end
        end
    endtask
`endif
endmodule
`default_nettype wire
