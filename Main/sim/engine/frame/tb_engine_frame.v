// B6 整帧仿真：engine_top（B1~B5 六层整机）经 APB 提交整帧命令表，从机是
// 带随机反压的 axi_ram_model。初始内存、表面表、命令表与期望内存由
// tools/check/pix_cases_dump.c --frame（SW3 十一个整帧金标准）与
// tools/check/sw6_frames_dump.c（SW6 场景 A 的两帧）导出。
//
// 判据（B6 条目 2.4 节）：
//   整帧       期望内存逐 128 位相同；帧缓冲 CRC 由 run.sh 的 python 按
//              sw 第 03 章 3.6 节口径另算，与 golden.txt 比对（11 帧金标准）
//   计数器对账 PERF_PIXEL == 软件执行器 pixel_total − src_oob（stats.hex 前两行）；
//              PERF_KEY_SKIP == key_skip；PERF_RD_BEAT/WR_BEAT == 从机观测的读写拍数；
//              PERF_RD_BURST/WR_BURST == 从机观测的读写事务数（命令取数计入读侧）
//   性能       +TIMED 定时从机（RD_LAT=23 RD_BUB=12 WR_BUB=6 B_LAT=1）下跑弹幕用例，
//              PERF_BUSY / 16 = 每颗周期数，与 engine 第 02 章 3.8 节模型 1092~1124
//              与 B4 实测 1104.3 对照；本台把通过线放宽到 [900, 1400] 防粗大回归，
//              精确对账在章正文里做
//   注错       ERRWIN=1 时从机把帧缓冲区间设成 SLVERR 窗口，第一条写命令必须以
//              BUS_WRITE 报错收场（总线错误经整机通路端到端浮出），本台在
//              ERRWIN!=0 时要求 errors>0 且打印 RESULT: FAIL（期望见红）。
//              ctrl 套件的 INJECT=1/2 是检查优先级与回退的结构注错，帧全合法时
//              本来就不触发，帧级注错要用总线错误
//
// -DCASE="build/xxx"：用例文件前缀（.init/.cmd/.surf/.exp/.cnt/.stats 六件）。
// 命令表写进从机存储的 LBASE（0x0024_0000，32 字节对齐，在用例窗口之外），
// 两张表的用例按 cnt.hex 拆成两次提交（SET_CLIP 状态跨提交保持，与软件执行器一致）。
`timescale 1ns/1ps
`default_nettype none
`ifndef CASE
`define CASE "build/frames/frame0"
`endif

module tb_engine_frame #(
    parameter integer ERRWIN = 0       // 1：把用例的帧缓冲区间设成从机 SLVERR 窗口（注错）
)();
    localparam integer CASE_WORDS = 135168;     // 0x210000 / 16，整帧用例的设备窗口
    localparam integer MEM_WORDS  = 262144;     // 从机 4 MiB，命令表放在用例窗口之外
    localparam [31:0] LBASE = 32'h0024_0000;    // 32 字节对齐，init/exp 不覆盖这一段

    reg clk=0, rst=1; always #5 clk=~clk;
    reg psel=0, penable=0, pwrite=0;
    reg [11:0] paddr=0;
    reg [31:0] pwdata=0;
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
    wire irq_b;

    engine_top u_eng(
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
        .irq_b(irq_b), .dbg_fe_cidx_err());

`ifdef TIMED
    axi_ram_model #(.MEM_WORDS(MEM_WORDS), .STRB_EN(1), .STALL(0), .TIMED(1),
                    .RD_LAT(23), .RD_BUB(12), .WR_BUB(6), .B_LAT(1),
                    .ERR_LO(ERRWIN ? 32'h0000_1000 : 32'hFFFF_FFFF),
                    .ERR_HI(ERRWIN ? 32'h001E_1000 : 32'h0000_0000)) u_ram(
`else
    axi_ram_model #(.MEM_WORDS(MEM_WORDS), .STRB_EN(1), .STALL(3),
                    .ADDR_STALL_MAX(3),
                    .ERR_LO(ERRWIN ? 32'h0000_1000 : 32'hFFFF_FFFF),
                    .ERR_HI(ERRWIN ? 32'h001E_1000 : 32'h0000_0000)) u_ram(
`endif
        .clk(clk), .rst_n(!rst),
        .awaddr(awaddr), .awlen(awlen), .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(wstrb), .wlast(wlast), .wvalid(wvalid),
        .wready(wready), .bresp(bresp), .bvalid(bvalid), .bready(bready),
        .araddr(araddr), .arlen(arlen), .arvalid(arvalid), .arready(arready),
        .rdata(rdata), .rresp(rresp), .rlast(rlast), .rvalid(rvalid),
        .rready(rready), .hold_aw(1'b0), .hold_ar(1'b0));

    // ---- 从机侧的总线观测：事务数与拍数（计数器对账的独立参照） ----
    integer obs_rd_burst=0, obs_rd_beat=0, obs_wr_burst=0, obs_wr_beat=0;
    integer bus_err=0;
    always @(posedge clk) if(!rst) begin
        if (arvalid && arready) obs_rd_burst = obs_rd_burst + 1;
        if (rvalid && rready)   obs_rd_beat  = obs_rd_beat + 1;
        if (awvalid && awready) obs_wr_burst = obs_wr_burst + 1;
        if (wvalid && wready)   obs_wr_beat  = obs_wr_beat + 1;
        if ((rvalid && rready && rresp != 2'b00) ||
            (bvalid && bready && bresp != 2'b00)) begin
            bus_err = bus_err + 1;
            $display("BUS-ERR rresp=%b bresp=%b", rresp, bresp);
        end
    end

    // ---------------------------------------------------------------- APB BFM
    integer errors=0;

    task apb_wr(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b1; paddr <= a; pwdata <= d;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            if (pslverr !== 1'b0) begin errors = errors + 1;
                $display("APB-FAIL 写 %03x: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0; pwrite <= 1'b0;
        end
    endtask

    task apb_rd(input [11:0] a, output [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b0; paddr <= a;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            d = prdata;
            if (pslverr !== 1'b0) begin errors = errors + 1;
                $display("APB-FAIL 读 %03x: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0;
        end
    endtask

    // 提交一张表并等结束（D54：DONE/ERR 置位即静止）
    task submit(input [31:0] base, input [31:0] cnt, output [31:0] code);
        reg [31:0] s;
        integer g;
        begin
            apb_wr(12'h020, base);
            apb_wr(12'h024, {16'd0, cnt[15:0]});
            apb_wr(12'h028, 32'h1);
            g = 0; s = 32'h0;
            while (((s & 32'hC) == 32'd0 || (s & 32'h1) != 0) && g < 2000000) begin
                apb_rd(12'h00C, s);
                g = g + 1;
            end
            if (g >= 2000000) begin
                errors = errors + 1;
                $display("TIMEOUT-WAIT base=%08x status=%08x", base, s);
                code = 32'hdead;
            end else begin
                code = s[7:4];
            end
        end
    endtask

    // 从机存储的 32 位车道（字 = (a>>4)，低 4 位选字节_lane）
    task mwrite(input [31:0] a, input [31:0] w);
        reg [127:0] old;
        integer     sh;
        begin
            sh = {a[3:2], 2'b00} * 8;
            old = u_ram.mem[a[21:4]];
            u_ram.mem[a[21:4]] = (old & ~(128'hFFFF_FFFF << sh)) | ({96'd0, w} << sh);
        end
    endtask

    // ---------------------------------------------------------------- 用例
    reg [255:0] cmd_rom [0:4095];
    reg [95:0]  surf_rom [0:15];
    reg [127:0] exp_mem [0:CASE_WORDS-1];
    reg [31:0]  cnt_hex [0:2];      // n1、n2、用例窗口（128 位字数）
    reg [31:0]  stats_hex [0:2];

    integer perf_rd_burst, perf_rd_beat, perf_wr_burst, perf_wr_beat;
    integer perf_cmd, perf_pixel, perf_key;
    integer i, j, k, mem_bad, total_cnt;
    reg [31:0] code, code2, id, caps;
    integer t0, t_submit;

    initial begin
        $readmemh({`CASE, ".cmd.hex"}, cmd_rom);
        $readmemh({`CASE, ".surf.hex"}, surf_rom);
        $readmemh({`CASE, ".exp.hex"}, exp_mem);
        $readmemh({`CASE, ".cnt.hex"}, cnt_hex);
        $readmemh({`CASE, ".stats.hex"}, stats_hex);
        #1 $readmemh({`CASE, ".init.hex"}, u_ram.mem);

        repeat(5) @(negedge clk); rst=0;
        repeat(2) @(negedge clk);

        // ID 与 CAPS 在位（D51：ID="GFX"<<8|1；CAPS 复位值 0x0000040F）
        apb_rd(12'h000, id);
        if (id !== 32'h4746_5801) begin
            errors = errors + 1; $display("ID-FAIL %08x", id);
        end
        apb_rd(12'h004, caps);
        if (caps !== 32'h0000_040F) begin
            errors = errors + 1; $display("CAPS-FAIL %08x", caps);
        end

        // 表面表 16 槽：+0 base、+4 dim、+8 ctrl（surf.hex 的 96 位字是 ctrl|dim|base）
        for (i = 0; i < 16; i = i + 1) begin
            apb_wr(12'h040 + i*12 + 0, surf_rom[i][31:0]);
            apb_wr(12'h040 + i*12 + 4, surf_rom[i][63:32]);
            apb_wr(12'h040 + i*12 + 8, surf_rom[i][95:64]);
        end

        // 命令表写进从机存储（两张表接连存放）
        total_cnt = cnt_hex[0] + cnt_hex[1];
        for (i = 0; i < total_cnt; i = i + 1)
            for (j = 0; j < 8; j = j + 1)
                mwrite(LBASE + i*32 + j*4, cmd_rom[i][j*32 +: 32]);

        apb_wr(12'h008, 32'h1);     // CTRL.EN，软复位不用（上电即净态）

        t0 = $time;
        submit(LBASE, cnt_hex[0], code);
        if (cnt_hex[1] != 0) begin
            // 第二张表的第一条地址：LBASE + n1*32（cnt 单位是记录）
            submit(LBASE + (cnt_hex[0] << 5), cnt_hex[1], code2);
        end else code2 = 32'h0;
        t_submit = ($time - t0) / 10;   // 周期数（10ns 一拍）

        // ---- 计数器对账（perf 寄存器在静止后读，D20/D54） ----
        apb_rd(12'h100 + 4*0, perf_cmd);
        apb_rd(12'h100 + 4*1, perf_pixel);
        apb_rd(12'h100 + 4*2, perf_key);
        apb_rd(12'h100 + 4*3, perf_rd_burst);
        apb_rd(12'h100 + 4*4, perf_wr_burst);
        apb_rd(12'h100 + 4*5, perf_rd_beat);
        apb_rd(12'h100 + 4*6, perf_wr_beat);

        if (code !== 32'h0) begin
            errors = errors + 1; $display("LIST1-ERR code=%0d", code);
        end
        if (cnt_hex[1] != 0 && code2 !== 32'h0) begin
            errors = errors + 1; $display("LIST2-ERR code=%0d", code2);
        end
        if (bus_err != 0) begin errors = errors + 1; $display("BUS-ERR %0d", bus_err); end
        if (u_ram.wlast_err != 0) begin
            errors = errors + 1; $display("WLAST-ERR %0d", u_ram.wlast_err);
        end

        // PIXEL == pixel_total − src_oob（源越界的像素在求交时被裁掉，硬件见不到）
        if (perf_pixel !== stats_hex[0] - stats_hex[1]) begin
            errors = errors + 1;
            $display("PERF-PIXEL got=%0d want=%0d (pixels=%0d src_oob=%0d)",
                     perf_pixel, stats_hex[0] - stats_hex[1], stats_hex[0], stats_hex[1]);
        end
        if (perf_key !== stats_hex[2]) begin
            errors = errors + 1;
            $display("PERF-KEY got=%0d want=%0d", perf_key, stats_hex[2]);
        end
        if (perf_rd_beat !== obs_rd_beat) begin
            errors = errors + 1;
            $display("PERF-RDBEAT reg=%0d obs=%0d", perf_rd_beat, obs_rd_beat);
        end
        if (perf_wr_beat !== obs_wr_beat) begin
            errors = errors + 1;
            $display("PERF-WRBEAT reg=%0d obs=%0d", perf_wr_beat, obs_wr_beat);
        end
        if (perf_rd_burst !== obs_rd_burst) begin
            errors = errors + 1;
            $display("PERF-RDBURST reg=%0d obs=%0d", perf_rd_burst, obs_rd_burst);
        end
        if (perf_wr_burst !== obs_wr_burst) begin
            errors = errors + 1;
            $display("PERF-WRBURST reg=%0d obs=%0d", perf_wr_burst, obs_wr_burst);
        end

        // ---- 期望内存逐 128 位（比对只盖本用例导出的窗口；命令表在窗口外，不参与比对） ----
        mem_bad = 0;
        for (k = 0; k < cnt_hex[2]; k = k + 1)
            if (u_ram.mem[k] !== exp_mem[k]) begin
                mem_bad = mem_bad + 1;
                if (mem_bad <= 8) $display("MEM addr=%08x got=%032h want=%032h",
                                           k*16, u_ram.mem[k], exp_mem[k]);
            end
        errors = errors + mem_bad;

        // 帧缓冲导出给 run.sh 的 python 算 CRC（sw 第 03 章 3.6 节口径）
        $writememh({`CASE, ".got.hex"}, u_ram.mem);

        $display("FRAME cycles=%0d rd_burst=%0d rd_beat=%0d wr_burst=%0d wr_beat=%0d pixel=%0d key=%0d cmd=%0d mem_bad=%0d",
                 t_submit, perf_rd_burst, perf_rd_beat, perf_wr_burst, perf_wr_beat,
                 perf_pixel, perf_key, perf_cmd, mem_bad);

`ifdef TIMED
`ifdef SPRITES
        // 弹幕用例（16 颗 16×16 ALPHA）：PERF_BUSY 逐颗对账；通过线放宽到
        // [900, 1400]（模型 1092~1124、B4 实测 1104.3），精确对账在章正文。
        begin
            integer busy;
            integer per;
            apb_rd(12'h100 + 4*8, busy);
            per = busy / 16;
            $display("SPRITES busy=%0d per_bullet=%0d", busy, per);
            if (per < 900 || per > 1400) begin
                errors = errors + 1;
                $display("PERF-SPRITES per_bullet=%0d 超出 [900,1400]", per);
            end
        end
`endif
`endif

        if (ERRWIN != 0) begin
            // 仅有内存不符不足以证明 BUS_WRITE 链路；同时确认总线错误、
            // 错误三元组里的 BUS_WRITE(11) 与目标帧缓冲的实际失配。
            if (code === 32'd11 && bus_err > 0 && mem_bad > 0)
                $display("RESULT: FAIL (BUS_WRITE confirmed)");
            else begin
                errors = errors + 1;
                $display("RESULT: INJECT-FAIL (code=%0d bus_err=%0d mem_bad=%0d)",
                         code, bus_err, mem_bad);
            end
        end else begin
            if (errors == 0) $display("RESULT: PASS (frame, software executor golden)");
            else $display("RESULT: FAIL (frame)");
        end
        $finish;
    end
endmodule
`default_nettype wire
