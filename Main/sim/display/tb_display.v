// display_top（扫描输出）仿真：取数、跨域、像素对齐、双缓冲切换与自愈
//
// 判据来自 Engineering/Todo/hw/扫描输出.md 2.3 节。用 -D 选用例：
//   CASE_IMAGE / CASE_SWAP / CASE_INFLIGHT / CASE_LEVEL / CASE_STARVE / CASE_SNAPSHOT
//   FULL_FRAME     整帧 640x480（默认小帧 640x8，逐像素比对跑得动）
//   CLK_PLUS / CLK_MINUS   像素时钟偏 ±0.5%，结论不能依赖固定相位
//   INJECT_VAL=n   注错（位定义见 rtl/display 两个子模块的端口说明）：
//                  1 不排空、2 不清空、4 立刻切换、8 两笔在途、16 D=0、32 相位 psel==3、64 拍内顺序反
//
// 从机用 ../axi_probe/axi_ram_model.v：带随机反压与读地址队列。它的存储是 4096 拍（64KB），
// 按 (addr>>4)%4096 取模，所以两个帧缓冲不能用 4MB 的实际间隔（会重叠），本仿真取
// 0x0200_0000 与 0x0200_8000：前者落在 0..2047 拍、后者落在 2048..4095 拍，互不覆盖。
// 期望值直接按地址从模型存储里取（含取模），所以整帧用例里图案每 16 行重复一次——
// 地址正确性另由 AR 监视器逐笔核对（偏移必须依次 0,4096,...，到 V_ACT 回绕）。
//
// starve 用例用从机的 hold_ar 停住地址通道（模型没有单独的 R 通道 hold 口）：
// 效果一样——取数停了、FIFO 被弹空、画面出黑、下一场恢复。
`timescale 1ns/1ps

module tb_display;

`ifndef INJECT_VAL
`define INJECT_VAL 0
`endif

`ifdef FULL_FRAME
    localparam V_ACT = 480;
`else
    localparam V_ACT = 8;
`endif

    localparam H_ACT = 640, H_FP = 16, H_SYNC = 96, H_BP = 48;
    localparam V_FP  = 10, V_SYNC = 2, V_BP = 33;

    localparam [31:0] BASE0 = 32'h0200_0000;   // 帧缓冲 0（也落在模型的 0..2047 拍）
    localparam [31:0] BASE1 = 32'h0200_8000;   // 帧缓冲 1（落在模型的 2048..4095 拍）

    localparam integer INJECT       = `INJECT_VAL;
    localparam integer LINE_BEATS   = 160;
    localparam integer FIELD_BEATS  = V_ACT * LINE_BEATS;
    localparam integer MEM_WORDS    = 4096;

    // ---- 时钟 ----
    reg clk_core = 1'b0;
    reg clk_pix  = 1'b0;
    always #5.0 clk_core = ~clk_core;
`ifdef CLK_PLUS
    always #19.7424 clk_pix = ~clk_pix;      // 39.4848ns，+0.5%
`elsif CLK_MINUS
    always #19.9404 clk_pix = ~clk_pix;      // 39.8808ns，-0.5%
`else
    always #19.8415 clk_pix = ~clk_pix;      // 39.683ns
`endif

    // ---- 复位 ----
    reg rst_core  = 1'b1;
    reg rst_pix_n = 1'b0;

    // ---- APB ----
    reg  [11:0] paddr   = 12'h0;
    reg         psel    = 1'b0;
    reg         penable = 1'b0;
    reg         pwrite  = 1'b0;
    reg  [31:0] pwdata  = 32'h0;
    wire [31:0] prdata;
    wire        pready, pslverr;

    // ---- AXI ----
    wire [31:0]  ar_addr;
    wire [7:0]   ar_len;
    wire [2:0]   ar_size;
    wire [1:0]   ar_burst, ar_lock;
    wire [7:0]   ar_id;
    wire         ar_valid, ar_ready;
    wire [127:0] r_data;
    wire [1:0]   r_resp;
    wire         r_last, r_valid, r_ready;
    reg          hold_ar = 1'b0;

    // ---- 像素输出 ----
    wire [23:0] vid_rgb;
    wire        vid_hs, vid_vs, vid_de;
    wire        irq;

    display_top #(
        .H_ACTIVE   (H_ACT),
        .H_FP       (H_FP),
        .H_SYNC     (H_SYNC),
        .H_BP       (H_BP),
        .V_ACTIVE   (V_ACT),
        .V_FP       (V_FP),
        .V_SYNC     (V_SYNC),
        .V_BP       (V_BP),
        .SYNC_POL   (1'b0),
        .FRONT_BASE_DEF (BASE0),
        .BACK_BASE_DEF  (BASE1),
        .LINE_BEATS (LINE_BEATS),
        .DEPTH      (512),
        .INJECT     (INJECT)
    ) dut (
        .clk_core (clk_core),
        .rst_core (rst_core),
        .clk_pixel(clk_pix),
        .rst_pixel_n (rst_pix_n),
        .paddr (paddr), .psel (psel), .penable (penable), .pwrite (pwrite),
        .pwdata (pwdata), .prdata (prdata), .pready (pready), .pslverr (pslverr),
        .ar_addr (ar_addr), .ar_len (ar_len), .ar_size (ar_size), .ar_burst (ar_burst),
        .ar_lock (ar_lock), .ar_id (ar_id), .ar_valid (ar_valid), .ar_ready (ar_ready),
        .r_id (8'h0), .r_data (r_data), .r_last (r_last), .r_resp (r_resp),
        .r_valid (r_valid), .r_ready (r_ready),
        .irq (irq),
        .vid_rgb (vid_rgb), .vid_hs (vid_hs), .vid_vs (vid_vs), .vid_de (vid_de)
    );

    axi_ram_model #(
        .MAX_LEN (255),
        .SEED    (1),
        .STALL   (3),
        .ADDR_STALL_MAX (0),
        .RD_QUEUE (1)
    ) slave (
        .clk (clk_core), .rst_n (~rst_core),
        .awaddr (32'h0), .awlen (8'h0), .awvalid (1'b0), .awready (),
        .wdata (128'h0), .wlast (1'b0), .wvalid (1'b0), .wready (),
        .bresp (), .bvalid (), .bready (1'b1),
        .araddr (ar_addr), .arlen (ar_len), .arvalid (ar_valid), .arready (ar_ready),
        .rdata (r_data), .rresp (r_resp), .rlast (r_last), .rvalid (r_valid),
        .rready (r_ready),
        .hold_aw (1'b0), .hold_ar (hold_ar)
    );

    // ------------------------------------------------------------------
    // 模型存储：按拍号造图案，四个像素的 B 通道不同（抓拍内顺序）
    // 0..2047 拍给帧缓冲 0，2048..4095 给帧缓冲 1，两段图案不同（抓混场）
    // ------------------------------------------------------------------
    integer i;
    initial begin
        for (i = 0; i < MEM_WORDS; i = i + 1) begin
            slave.mem[i][31:0]   = {8'h00, i[15:8], i[7:0], 8'h00};
            slave.mem[i][63:32]  = {8'h00, i[15:8], i[7:0], 8'h01};
            slave.mem[i][95:64]  = {8'h00, i[15:8], i[7:0], 8'h02};
            slave.mem[i][127:96] = {8'h00, i[15:8], i[7:0], 8'h03};
            if (i >= 2048) begin
                slave.mem[i][31:0]   = {8'h00, ~i[15:8], i[7:0], 8'h10};
                slave.mem[i][63:32]  = {8'h00, ~i[15:8], i[7:0], 8'h11};
                slave.mem[i][95:64]  = {8'h00, ~i[15:8], i[7:0], 8'h12};
                slave.mem[i][127:96] = {8'h00, ~i[15:8], i[7:0], 8'h13};
            end
        end
    end

    // ------------------------------------------------------------------
    // APB
    // ------------------------------------------------------------------
    localparam [11:0] A_CTRL = 12'h008, A_FRONT = 12'h00C, A_BACK = 12'h010;
    localparam [11:0] A_SWAP = 12'h014, A_TXN = 12'h024, A_LINE = 12'h028;
    localparam [11:0] A_BEAT = 12'h02C, A_STATUS = 12'h038, A_IRQ_EN = 12'h0D0;
    localparam [11:0] A_IRQ_ST = 12'h0D4, A_FRCNT = 12'h0D8, A_SWCNT = 12'h0DC;
    localparam [11:0] A_SNAPFR = 12'h0E0, A_SNAPMIN = 12'h0E4, A_ZERO = 12'h0E8;
    localparam [11:0] A_UNDER = 12'h0EC, A_FBEAT = 12'h0F0, A_ENDLV = 12'h0F4;

    task apb_write(input [11:0] a, input [31:0] d);
        begin
            @(negedge clk_core);
            paddr = a; pwdata = d; pwrite = 1'b1; psel = 1'b1; penable = 1'b0;
            @(negedge clk_core);
            penable = 1'b1;
            @(negedge clk_core);
            psel = 1'b0; penable = 1'b0; pwrite = 1'b0;
        end
    endtask

    task apb_read(input [11:0] a, output [31:0] d);
        begin
            @(negedge clk_core);
            paddr = a; pwrite = 1'b0; psel = 1'b1; penable = 1'b0;
            @(negedge clk_core);
            penable = 1'b1;
            @(negedge clk_core);
            d = prdata;
            psel = 1'b0; penable = 1'b0;
        end
    endtask

    // ------------------------------------------------------------------
    // 场计数与逐像素检查（像素域）
    // ------------------------------------------------------------------
    event   vs_fall;
    event   de_rise;
    reg     de_dbg_d = 1'b0;
    always @(posedge clk_pix) begin
        de_dbg_d <= vid_de;
        if (vid_de && !de_dbg_d) -> de_rise;
    end

    integer fs_count = 0;              // 已见到的场起始数
    integer fld_pix_err  [0:63];
    integer fld_line_err [0:63];
    integer cur_pix_err = 0;
    integer cur_line_err = 0;
    integer x = 0, y = 0;
    reg     chk_on = 0;
    reg [31:0] base_cur = BASE0;       // 本场该扫的基址（TB 的模型，切换在场起始生效）
    reg [31:0] base_next = BASE1;
    reg        swap_pending = 0;       // 激励写 SWAP_REQ 后置起，场起始时换 base_cur

    reg        vs_d = 1'b1;
    reg [127:0] w128;
    reg [31:0]  exp_px;

    function [11:0] mem_idx(input [31:0] b, input integer xx, input integer yy);
        begin
            mem_idx = (((b + yy*4096 + (xx/4)*16) >> 4) % MEM_WORDS);
        end
    endfunction

    always @(posedge clk_pix) begin
        if (rst_pix_n) begin
            if (vs_d && !vid_vs) begin             // 场起始
                if (fs_count < 64) begin
                    fld_pix_err[fs_count]  = cur_pix_err;
                    fld_line_err[fs_count] = cur_line_err;
                end
                fs_count = fs_count + 1;
                cur_pix_err = 0;
                cur_line_err = 0;
                x = 0; y = 0;
                if (swap_pending) begin
                    base_cur = base_next;
                    swap_pending = 0;
                end
                -> vs_fall;
            end
            if (vid_de) begin
                if (chk_on) begin
                    w128 = slave.mem[mem_idx(base_cur, x, y)];
                    exp_px = w128[{x[1:0], 5'b0} +: 32];
                    if (vid_rgb !== exp_px) begin
                        cur_pix_err = cur_pix_err + 1;
                        if (cur_pix_err <= 3)
                            $display("ERROR image x=%0d y=%0d got %h expect %h (t=%0t)",
                                     x, y, vid_rgb, exp_px, $time);
                    end
                end
                x = x + 1;
            end else if (x != 0) begin
                if (x != H_ACT) begin
                    cur_line_err = cur_line_err + 1;
                    if (cur_line_err <= 3)
                        $display("ERROR line width x=%0d expect %0d (t=%0t)", x, H_ACT, $time);
                end
                x = 0;
                y = y + 1;
            end
        end
        vs_d <= vid_vs;
    end

    task wait_fields(input integer n);
        integer k;
        begin
            for (k = 0; k < n; k = k + 1) @(vs_fall);
        end
    endtask

    // ---- 调试：每场一个快照（正式判据不看它，用来定位取数/水位的形状）----
    integer dbg_ar = 0, dbg_r = 0, dbg_ar_prev = 0, dbg_r_prev = 0;
    integer dbg_z_prev = 0, dbg_u_prev = 0, dbg_b_prev = 0;
    integer dbg_min_rd = 1024;
    // 与 DUT 内部那个 min_level 平行的独立观测：在像素域直接采 FIFO 读侧水位
    always @(posedge clk_pix)
        if (vid_de && dut.u_out.src_eff && (dut.u_fifo.rd_level < dbg_min_rd))
            dbg_min_rd = dut.u_fifo.rd_level;

`ifdef TRACE_LINE
    reg dbg_de_d = 1'b0;
    integer dbg_line = 0;
    always @(posedge clk_pix) begin
        dbg_de_d <= vid_de;
        if (vid_de && !dbg_de_d && dut.u_out.src_eff) begin
            $display("LINE %0d t=%0t rd=%0d wr=%0d row=%0d infl=%b",
                     dbg_line, $time, dut.u_fifo.rd_level, dut.u_fifo.wr_level,
                     dut.u_fetch.row, dut.u_fetch.in_flight);
            dbg_line = dbg_line + 1;
        end
        if (vs_d && !vid_vs) dbg_line = 0;
    end
`endif
    always @(posedge clk_core) begin
        if (ar_valid && ar_ready) dbg_ar = dbg_ar + 1;
        if (r_valid && r_ready)   dbg_r  = dbg_r + 1;
    end
    always @(vs_fall) begin
        $display("FS %0d wr_lvl=%0d rd_lvl=%0d in_flight=%b row=%0d state=%0d empty=%b ar+=%0d r+=%0d rst=%b rstsync=%b frst=%b blc=%0d rcnt=%0d can=%b arv=%b",
                 fs_count, dut.u_fifo.wr_level, dut.u_fifo.rd_level,
                 dut.u_fetch.in_flight, dut.u_fetch.row, dut.u_fetch.state,
                 dut.u_fifo.rd_empty,
                 dbg_ar - dbg_ar_prev, dbg_r - dbg_r_prev,
                 dut.u_fifo.u_fifo.rst_busy, dut.u_fifo.rst_busy_sync,
                 dut.u_fetch.fifo_rst, dut.u_fetch.busy_low_cnt, dut.u_fetch.rst_cnt,
                 dut.u_fetch.can_issue, dut.u_fetch.ar_valid);
        $display("   SNAP tog=%b busy=%b acksync=%b fsync=%b fack=%b",
                 dut.u_out.snap_toggle_r, dut.u_out.snap_busy, dut.u_out.ack_sync,
                 dut.u_fetch.snap_sync, dut.u_fetch.snap_ack);
        $display("   OUT min=%0d z+=%0d u+=%0d beats+=%0d tb_min_rd=%0d",
                 dut.u_out.min_level,
                 dut.u_out.zero_hits - dbg_z_prev,
                 dut.u_out.underflow_beats - dbg_u_prev,
                 dut.u_out.frame_beats - dbg_b_prev,
                 dbg_min_rd);
        dbg_z_prev = dut.u_out.zero_hits;
        dbg_u_prev = dut.u_out.underflow_beats;
        dbg_b_prev = dut.u_out.frame_beats;
        dbg_min_rd = 1024;
        dbg_ar_prev = dbg_ar;
        dbg_r_prev  = dbg_r;
    end

    // ------------------------------------------------------------------
    // AXI 监视：在途笔数、突发长度、行地址序列
    // ------------------------------------------------------------------
    integer inflight = 0;
    integer ar_row = 0;
    integer axi_over_err = 0, axi_len_err = 0, axi_addr_err = 0;

    always @(posedge clk_core) begin
        if (ar_valid && ar_ready) begin
            if (inflight >= 1) begin
                axi_over_err = axi_over_err + 1;
                if (axi_over_err <= 3)
                    $display("ERROR inflight > 1 at AR (t=%0t)", $time);
            end
            inflight = inflight + 1;
            if (ar_len !== 8'd159) begin
                axi_len_err = axi_len_err + 1;
                $display("ERROR arlen got %0d expect 159 (t=%0t)", ar_len, $time);
            end
            if (ar_addr !== (base_cur + ar_row*4096)) begin
                axi_addr_err = axi_addr_err + 1;
                if (axi_addr_err <= 3)
                    $display("ERROR araddr got %h expect %h (row %0d, t=%0t)",
                             ar_addr, base_cur + ar_row*4096, ar_row, $time);
            end
            ar_row = (ar_row + 1) % V_ACT;
        end
        if (r_valid && r_ready && r_last)
            inflight = inflight - 1;
        if (vs_fall_edge_core) begin
            ar_row = 0;
        end
    end

    // 场起始在 core 域的影子（两拍同步后用来重置行序号；不参与判据）
    reg [1:0] fs_sync_core = 2'b00;
    reg       fs_toggle_seen = 1'b0;
    reg [1:0] fs_sync_pix = 2'b00;
    always @(posedge clk_pix) fs_sync_pix <= {fs_sync_pix[0], dut.u_out.fs_toggle};
    wire fs_edge_pix = fs_sync_pix[1] ^ fs_sync_pix[0];
    always @(posedge clk_core) fs_sync_core <= {fs_sync_core[0], fs_sync_pix[1]};
    wire vs_fall_edge_core = (fs_sync_core[1] ^ fs_sync_core[0]);

    // ------------------------------------------------------------------
    // 激励与判据
    // ------------------------------------------------------------------
    integer errors = 0;
    integer checks = 0;
    integer k;
    reg [31:0] rd;

    // D7 的不变量（2026-09-21 复核补）：复位 FIFO 的那一拍，既不能有在途读，
    // 也不能有已经拉起、还没被接走的 AR。少了这一条，「同一拍既 fs_event 又发行」
    // 这类竞态会绕过排空，让在途那笔的拍落进新场的队头——正是 D7 要防的事，
    // 而逐像素比对只在恰好撞上时才会红，判据不该靠运气。
    integer drain_viol = 0;
    always @(posedge clk_core) begin
        if (!rst_core && dut.u_fetch.fifo_rst &&
            (dut.u_fetch.in_flight || dut.u_fetch.ar_valid)) begin
            if (drain_viol == 0)
                $display("ERROR drain-invariant: t=%0t fifo_rst=1 但 in_flight=%b ar_valid=%b",
                         $time, dut.u_fetch.in_flight, dut.u_fetch.ar_valid);
            drain_viol = drain_viol + 1;
        end
    end

    task chk(input [8*48-1:0] what, input integer got, input integer lo, input integer hi);
        begin
            checks = checks + 1;
            // ^got === 1'bx 这一条不能省：读回 X 时 got<lo 与 got>hi 都是假，判据会静默通过
            if ((^got === 1'bx) || (got < lo) || (got > hi)) begin
                errors = errors + 1;
                $display("ERROR %0s: got %0d expect %0d..%0d", what, got, lo, hi);
            end else
                $display("ok %0s: %0d", what, got);
        end
    endtask

    integer clean_from = 5;      // 这一下标起的场必须逐像素全对（配置生效后）
    integer check_fields = 3;

    initial begin
`ifdef DUMP
        $dumpfile("tb_display.vcd");
        $dumpvars(0, tb_display);
`endif

        rst_core = 1'b1;
        rst_pix_n = 1'b0;
        repeat (20) @(posedge clk_core);
        rst_core = 1'b0;
        repeat (20) @(posedge clk_pix);
        rst_pix_n = 1'b1;
        $display("CASE: V_ACT=%0d INJECT=%0d", V_ACT, INJECT);

        wait_fields(2);
        apb_write(A_BACK, BASE1);
        apb_write(A_CTRL, 32'h3);          // EN=1, SRC=1
        wait_fields(2);                    // 让 EN/SRC 在场起始生效并预取
        chk_on = 1;

`ifdef TRACE_FSM
        for (k = 0; k < 400; k = k + 1) begin
            @(posedge clk_core);
            $display("TR t=%0t st=%0d frst=%b rst=%b rsync=%b blc=%0d rcnt=%0d row=%0d infl=%b arv=%b",
                     $time, dut.u_fetch.state, dut.u_fetch.fifo_rst,
                     dut.u_fifo.u_fifo.rst_busy, dut.u_fifo.rst_busy_sync,
                     dut.u_fetch.busy_low_cnt, dut.u_fetch.rst_cnt,
                     dut.u_fetch.row, dut.u_fetch.in_flight, dut.u_fetch.ar_valid);
        end
`endif

`ifdef CASE_SWAP
        // 第 1 个被检查场的中途请求切换，下一场必须整场来自帧缓冲 1
        begin : swap_case
            integer sw_field;
            wait_fields(3);
            repeat (H_ACT/2) @(posedge clk_pix);   // 等到这个场的场中
            apb_write(A_SWAP, 32'h1);
            sw_field = fs_count;                   // 这一场结束时记到的下标
            swap_pending = 1;                      // TB 模型：下一个场起始换基址
            base_next = BASE1;
            wait_fields(5);
            // 切换后的三个场每一场都不许混：像素比对已经按 base_cur 逐像素查过
            for (k = sw_field + 1; k <= sw_field + 3; k = k + 1)
                chk("swap field pixels", fld_pix_err[k], 0, 0);
            apb_read(A_SWCNT, rd);
            chk("SWAP_COUNT", rd, 1, 1);
            apb_read(A_FRONT, rd);
            chk("FRONT_BASE after swap", rd, BASE1, BASE1);
        end
`elsif CASE_LEVEL
        // 连续若干场：最低水位不低于一行、触底与欠载为 0
        begin : level_case
            integer min_seen = 1024;
            for (k = 0; k < 6; k = k + 1) begin
                wait_fields(1);
                apb_read(A_SNAPMIN, rd);
                if (rd < min_seen) min_seen = rd;
            end
            chk("min level over fields", min_seen, 160, 512);
            apb_read(A_ZERO, rd);   chk("FIFO_ZERO_HITS", rd, 0, 0);
            apb_read(A_UNDER, rd);  chk("FIFO_UNDERFLOW_BEATS", rd, 0, 0);
            for (k = clean_from; k < clean_from + 3; k = k + 1)
                chk("level field pixels", fld_pix_err[k], 0, 0);
        end
`elsif CASE_STARVE
        // 停住取数一段时间：这一场出错、欠载计数非零；下一场逐像素恢复
        begin : starve_case
            integer starve_idx;
            // 停住取数要停在场消隐之后的可见区里：消隐有 1.43ms，在那边停多久都能被预取补回来。
            // 一直停到场起始才放开——这样取数侧是「还欠着账」进场的，D7 的第一步与第二步
            // 才有实活干（正常场里最后一行早就取完，场起始时取数侧是闲着的，
            // 所以 inj_no_drain 只能跟这个用例配对，跟 image 配对是测不出来的）。
            wait_fields(1);
            @(de_rise);                     // 等可见区真的开始
            starve_idx = fs_count;          // 这一场结束时记到的下标
            hold_ar = 1'b1;
            wait_fields(1);                 // 一直停到下一个场起始
            hold_ar = 1'b0;
            wait_fields(4);
            chk("starve field has errors", (fld_pix_err[starve_idx] > 0) ? 1 : 0, 1, 1);
            apb_read(A_UNDER, rd);
            chk("FIFO_UNDERFLOW_BEATS > 0", (rd > 0) ? 1 : 0, 1, 1);
            wait_fields(2);
            // 自愈的判据：欠载那一场的**下一场**必须逐像素全对
            chk("field after starve pixels", fld_pix_err[starve_idx + 1], 0, 0);
            chk("recovered field pixels", fld_pix_err[starve_idx + 2], 0, 0);

            // CTRL.CLEAR 要能把像素侧的累计值也清掉（2026-09-21 上板补的判据）。
            // 这两个计数器只靠像素域复位清，而 core 复位碰不到像素域：
            // reset halt 之后 src_eff 还留着上一次的 1，一整场空读就把它们灌满，
            // 软件清不掉的话「恒为 0」这条判据永远过不了。上板实测过 77266 拍。
            apb_write(A_CTRL, 32'h7);       // EN=1 SRC=1 CLEAR=1
            wait_fields(1);
            apb_read(A_ZERO,  rd); chk("CLEAR 清掉 ZERO_HITS", rd, 0, 0);
            apb_read(A_UNDER, rd); chk("CLEAR 清掉 UNDERFLOW_BEATS", rd, 0, 0);
            apb_write(A_CTRL, 32'h3);
        end
`elsif CASE_SNAPSHOT
        begin : snap_case
            integer prev, now, bad;
            bad = 0;
            apb_read(A_SNAPFR, prev);
            for (k = 0; k < 20; k = k + 1) begin
                wait_fields(1);
                apb_read(A_SNAPFR, now);
                if ((now - prev) != 1) bad = bad + 1;
                prev = now;
            end
            chk("snapshot frame +1 every field", bad, 0, 0);
        end
`else
        // CASE_IMAGE（默认）与 CASE_INFLIGHT 共用：连续若干场逐像素全对
        wait_fields(6);
        for (k = clean_from; k < clean_from + 3; k = k + 1) begin
            chk("image field pixels", fld_pix_err[k], 0, 0);
            chk("image field line widths", fld_line_err[k], 0, 0);
        end
        apb_read(A_FBEAT, rd);  chk("FRAME_BEATS", rd, FIELD_BEATS, FIELD_BEATS);
        apb_read(A_ENDLV, rd);  chk("SNAP_END_LEVEL", rd, 0, 0);
        apb_read(A_ZERO, rd);   chk("FIFO_ZERO_HITS", rd, 0, 0);
        apb_read(A_UNDER, rd);  chk("FIFO_UNDERFLOW_BEATS", rd, 0, 0);
        apb_read(A_SNAPMIN, rd);chk("SNAP_MIN_LEVEL", rd, 160, 512);
        apb_read(A_FRONT, rd);  chk("FRONT_BASE", rd, BASE0, BASE0);
`endif

        // 与判据无关的公共检查
        chk("in-flight never > 1", axi_over_err, 0, 0);
        chk("arlen always 159", axi_len_err, 0, 0);
        chk("row address sequence", axi_addr_err, 0, 0);
        chk("drain before fifo reset", drain_viol, 0, 0);
        apb_read(A_STATUS, rd);
        $display("STATUS=%h FRAME_COUNT=%0d", rd, fs_count);

        if (errors == 0)
            $display("RESULT: PASS checks=%0d fields=%0d", checks, fs_count);
        else
            $display("RESULT: FAIL errors=%0d checks=%0d fields=%0d", errors, checks, fs_count);
        $finish;
    end

    // 看门狗：小帧一场 1.68ms，整帧 16.7ms，留足
`ifdef FULL_FRAME
    initial begin
        #400_000_000;
        $display("RESULT: FAIL timeout at t=%0t", $time);
        $finish;
    end
`else
    initial begin
        #120_000_000;
        $display("RESULT: FAIL timeout at t=%0t", $time);
        $finish;
    end
`endif

endmodule
