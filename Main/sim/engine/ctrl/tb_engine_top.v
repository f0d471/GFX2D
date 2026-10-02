// B5 的单元仿真测试台：engine_top（B1~B5 全部）经 APB 提交，从机是补齐后的
// axi_ram_model（4 MiB、wstrb、运行时 SLVERR 窗口）。判据（B5 条目 2.4 节）：
//
//   1 十步检查：159 条一致性向量逐条喂进引擎，错误三元组与 spec 第 02 章逐条相同；
//     命令表放在专用地址，可读记录之后由从机回 SLVERR（vec_to_sim 转换）。
//   2 取数的边界：FETCH 落在批中间（回退后定位到那一条）、首批/下一批不可取、
//     LIST_BASE 非 32 字节对齐、批跨 4KB 拆两笔、32 位地址末端的 END 优先。
//   3 总线错误与超时（端到端）：BUS_READ/BUS_WRITE 各一例（三元组、中断 B、
//     失败命令之后的命令无像素副作用）；从机迟到超过门限报 TIMEOUT，
//     迟到响应到达后引擎回空闲、能再次提交。
//   4 软复位：排空完成后 valid 全清、裁剪框恢复全游戏区、之后能再次提交。
//   5 跨命令写后读与首错顺序：别名表面的先写后读逐字相同；空命令/NOP/SET_CLIP
//     不死锁；前一条 BUS_WRITE 先于后一条 BAD_FIELD。
//   6 PERF_CMD/PIXEL/KEY_SKIP 的精确对账与读拍、CAPS、APB 拒绝语义。
//
// 注错构建（run.sh 另编两次）：INJECT=1 对调 space/format、INJECT=2 关掉逐条回退，
// 各自必须让至少一条向量见红（+vectors_only 只跑向量段并反转判据）。
// +vec_from=N 跳过前 N 条向量（调试用）。
//
// 地址约定：模型存储 4 MiB、按 (addr>>4) 取模映射。命令表区 0x0020_0000 起每表
// 1 KiB；向量与定向用例的像素区在低 64 KiB；0xFFFFFFE0 的读落在字 0x3FFFE
//（(0xFFFFFFE0>>4) mod 0x40000，地址 0x0003_FFE0），测试台按别名地址写。
// XRGB 目的的 X 字节恒为 0，所有内存期望只对比低 24 位。
`timescale 1ns/1ps
`default_nettype none
module tb_engine_top #(
    parameter integer INJECT = 0
);
    reg clk = 1'b0;
    reg rst = 1'b1;
    always #5 clk = ~clk;

    // APB
    reg         psel = 1'b0, penable = 1'b0, pwrite = 1'b0;
    reg [11:0]  paddr = 12'd0;
    reg [31:0]  pwdata = 32'd0;
    wire [31:0] prdata;
    wire        pready, pslverr;

    // AXI
    wire [31:0] araddr;  wire [7:0] arlen;
    wire        arvalid, arready;
    wire [127:0] rdata;  wire [1:0] rresp;
    wire        rlast, rvalid;
    wire        rready;                  // 引擎主口的输出
    wire [31:0] awaddr;  wire [7:0] awlen;
    wire        awvalid, awready;
    wire [127:0] wdata;   wire [15:0] wstrb;
    wire        wlast, wvalid, wready;
    wire [1:0]  bresp;    wire bvalid;
    wire        bready;
    reg         ar_hold = 1'b0, aw_hold = 1'b0;

    wire irq_b;
    wire fe_cidx_err;

    engine_top #(.INJECT(INJECT)) dut (
        .clk(clk), .rst_ctrl(rst), .rst_ddr_n(~rst),
        .paddr(paddr), .psel(psel), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(prdata), .pready(pready), .pslverr(pslverr),
        .m_axi_araddr(araddr), .m_axi_arlen(arlen), .m_axi_arsize(), .m_axi_arburst(),
        .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid), .m_axi_rready(rready),
        .m_axi_awaddr(awaddr), .m_axi_awlen(awlen), .m_axi_arsize(), .m_axi_awburst(),
        .m_axi_awvalid(awvalid), .m_axi_awready(awready),
        .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wlast(wlast),
        .m_axi_wvalid(wvalid), .m_axi_wready(wready),
        .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
        .irq_b(irq_b), .dbg_fe_cidx_err(fe_cidx_err)
    );

    axi_ram_model #(
        .MEM_WORDS(262144),
        .STRB_EN(1)
    ) u_ram (
        .clk(clk), .rst_n(~rst),
        .awaddr(awaddr), .awlen(awlen), .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(wstrb), .wlast(wlast), .wvalid(wvalid), .wready(wready),
        .bresp(bresp), .bvalid(bvalid), .bready(bready),
        .araddr(araddr), .arlen(arlen), .arvalid(arvalid), .arready(arready),
        .rdata(rdata), .rresp(rresp), .rlast(rlast), .rvalid(rvalid), .rready(rready),
        .hold_aw(aw_hold), .hold_ar(ar_hold)
    );

    localparam [11:0] R_CTRL  = 12'h008, R_STATUS = 12'h00C, R_IRQ_EN = 12'h010,
                      R_IRST  = 12'h014, R_EIDX   = 12'h018, R_ETAG  = 12'h01C,
                      R_LBASE = 12'h020, R_LCNT   = 12'h024, R_DOOR  = 12'h028,
                      R_SURF  = 12'h040, R_PERF   = 12'h100;
    localparam [31:0] ST_BUSY = 32'h1, ST_DRAIN = 32'h2, ST_DONE = 32'h4,
                      ST_ERR  = 32'h8;

    localparam [31:0] VBASE = 32'h0020_0000;   // 向量命令表区（每表 1 KiB 跨距）
    localparam [31:0] LBASE = 32'h0020_4000;   // 定向用例命令表区
    localparam [31:0] WORK  = 32'h0001_0000;   // 定向用例像素区

    integer errors   = 0;
    integer checks   = 0;
    integer vec_fail = 0;
    integer vec_total = 0;
    integer guard_g;

    task step; begin @(posedge clk); end endtask

    // ---------------------------------------------------------------- APB BFM
    task apb_wr(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b1; paddr <= a; pwdata <= d;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            checks = checks + 1;
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
            checks = checks + 1;
            if (pslverr !== 1'b0) begin errors = errors + 1;
                $display("APB-FAIL 读 %03x: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0;
        end
    endtask

    task apb_wr_reject(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b1; paddr <= a; pwdata <= d;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            checks = checks + 1;
            if (pslverr !== 1'b1) begin errors = errors + 1;
                $display("APB-FAIL 写 %03x 应拒绝: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0; pwrite <= 1'b0;
        end
    endtask

    task apb_rd_reject(input [11:0] a);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b0; paddr <= a;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            checks = checks + 1;
            if (pslverr !== 1'b1) begin errors = errors + 1;
                $display("APB-FAIL 读 %03x 应拒绝: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0;
        end
    endtask

    task put_surf3(input integer slot, input [31:0] w0, input [31:0] w1, input [31:0] w2);
        begin
            apb_wr(12'h040 + slot*12 + 0, w0);
            apb_wr(12'h040 + slot*12 + 4, w1);
            apb_wr(12'h040 + slot*12 + 8, w2);
        end
    endtask

    task surf16(input integer slot, input [31:0] base);
        begin
            put_surf3(slot, base, 32'h0010_0010, 32'h8000_0006);
        end
    endtask

    // 软复位（带 EN 写），顺带撤掉 SLVERR 窗口
    task soft_reset;
        reg [31:0] s;
        integer g;
        begin
            u_ram.err_lo_r = 32'hffff_ffff;
            u_ram.err_hi_r = 32'h0000_0000;
            apb_wr(R_CTRL, 32'h3);              // EN | SOFT_RST
            g = 0;
            apb_rd(R_STATUS, s);
            while (s[1] == 1'b1 && g < 1000) begin
                apb_rd(R_STATUS, s);
                g = g + 1;
            end
        end
    endtask

    // 提交一张表并等结束（DONE/ERR 置位且引擎回空闲）
    task submit(input [31:0] base, input [31:0] cnt, output [31:0] code,
                output [31:0] eidx, output [31:0] etag);
        reg [31:0] s;
        integer g;
        begin
            apb_wr(R_LBASE, base);
            apb_wr(R_LCNT, {16'd0, cnt});
            apb_wr(R_DOOR, 32'h1);
            g = 0;
            s = 32'h0;
            while (((s & (ST_DONE | ST_ERR)) == 32'd0 || (s & ST_BUSY) != 0)
                   && g < 400000) begin
                apb_rd(R_STATUS, s);
                g = g + 1;
            end
            if (g >= 400000) begin
                errors = errors + 1;
                $display("TIMEOUT-WAIT base=%08x status=%08x", base, s);
                code = 32'hdead; eidx = 32'hdead; etag = 32'hdead;
            end else begin
                code = s[7:4];
                apb_rd(R_EIDX, eidx);
                apb_rd(R_ETAG, etag);
            end
        end
    endtask

    // 从机存储的 32 位车道（地址按模型映射折算：字 = (a>>4) mod 2^18）
    function [31:0] mread(input [31:0] a);
        begin
            mread = u_ram.mem[a[21:4]] >> ({a[3:2], 2'b00} * 8);
        end
    endfunction
    task mwrite(input [31:0] a, input [31:0] w);
        reg [127:0] old;
        reg [6:0]   sh;
        begin
            sh = {a[3:2], 2'b00} * 8;
            old = u_ram.mem[a[21:4]];
            u_ram.mem[a[21:4]] = (old & ~(128'hFFFF_FFFF << sh)) | ({96'd0, w} << sh);
        end
    endtask

    task submit_check(input [31:0] base, input [31:0] cnt,
                      input [31:0] exp_code, input [31:0] exp_idx, input [31:0] exp_tag,
                      input [127:0] name);
        reg [31:0] c, i, t;
        begin
            checks = checks + 1;
            submit(base, cnt, c, i, t);
            if (c !== exp_code || (exp_code != 0 && (i !== exp_idx || t !== exp_tag))) begin
                errors = errors + 1;
                $display("CASE-FAIL %0s: code=%0d(idx=%0d tag=%08x) exp=%0d(idx=%0d tag=%08x)",
                         name, c, i, t, exp_code, exp_idx, exp_tag);
            end
        end
    endtask

    task check_mem(input [31:0] a, input [31:0] exp, input [127:0] name);
        begin
            checks = checks + 1;
            if (mread(a) !== exp) begin
                errors = errors + 1;
                $display("MEM-FAIL %0s: [%08x] got=%08x exp=%08x", name, a, mread(a), exp);
            end
        end
    endtask

    // =====================================================================
    // 向量段：先整文件解析进数组，再逐向量执行
    // =====================================================================
    localparam integer MAXREC = 8192;
    reg [8*8-1:0] rec_tag  [0:MAXREC-1];
    integer       rec_i1   [0:MAXREC-1];
    integer       rec_i2   [0:MAXREC-1];
    integer       rec_i3   [0:MAXREC-1];
    integer       rec_i4   [0:MAXREC-1];
    integer       rec_i5   [0:MAXREC-1];
    integer       rec_i6   [0:MAXREC-1];
    integer       rec_i7   [0:MAXREC-1];
    integer       rec_i8   [0:MAXREC-1];
    integer       rec_i9   [0:MAXREC-1];
    integer       rec_i10  [0:MAXREC-1];
    integer       rec_i11  [0:MAXREC-1];
    integer       nrec;
    integer vhead [0:255];
    integer nvec;

    task parse_vectors;
        integer fd, r, k;
        reg [1023:0] tok;
        begin
            for (r = 0; r < MAXREC; r = r + 1) rec_tag[r] = 8'h0;
            fd = $fopen("vec_stream.txt", "r");
            if (fd == 0) begin
                $display("RESULT: FAIL (vec_stream.txt 打不开)");
                $finish;
            end
            r = 0; nvec = 0;
            k = $fscanf(fd, "%s", tok);
            while (k == 1 && r < MAXREC) begin
                if (tok == "V") begin
                    vhead[nvec] = r;
                    nvec = nvec + 1;
                    rec_tag[r] = "V";
                    k = $fscanf(fd, "%d %d %d %d %d %x %d %x %x %x %x",
                                rec_i1[r], rec_i2[r], rec_i3[r], rec_i4[r], rec_i5[r],
                                rec_i6[r], rec_i7[r], rec_i8[r], rec_i9[r], rec_i10[r],
                                rec_i11[r]);
                    r = r + 1;
                end else if (tok == "S") begin
                    rec_tag[r] = "S";
                    k = $fscanf(fd, "%d %x %x %x %x",
                                rec_i1[r], rec_i2[r], rec_i3[r], rec_i4[r], rec_i5[r]);
                    r = r + 1;
                end else if (tok == "L") begin
                    rec_tag[r] = "L";
                    k = $fscanf(fd, "%x", rec_i1[r]);
                    r = r + 1;
                end else if (tok == "C") begin
                    rec_tag[r] = "C";
                    k = $fscanf(fd, "%x %x %x %x %x %x %x %x",
                                rec_i1[r], rec_i2[r], rec_i3[r], rec_i4[r],
                                rec_i5[r], rec_i6[r], rec_i7[r], rec_i8[r]);
                    r = r + 1;
                end else if (tok == "M") begin
                    rec_tag[r] = "M";
                    k = $fscanf(fd, "%d %x %x", rec_i1[r], rec_i2[r], rec_i3[r]);
                    r = r + 1;
                end
                k = $fscanf(fd, "%s", tok);
            end
            $fclose(fd);
            nrec = r;
        end
    endtask

    task run_vectors;
        integer v, r, end_r, l, j, ncmd;
        integer recs_start [0:7];
        integer readable;
        reg [31:0] cmds [0:8191];
        reg [15:0] cnts [0:7];
        reg [31:0] base_l, c, ei, et;
        integer v_lists, v_code, v_errlist, v_erridx, v_mem;
        reg [31:0] v_tag;
        integer nexp;
        reg [31:0] exp_a [0:2047];
        reg [31:0] exp_w [0:2047];
        reg [31:0] t_sw, t_sh, t_sc;
        integer v_from;
        begin
            v_from = 0;
            if ($value$plusargs("vec_from=%d", v_from)) ;
            for (v = 0; v < nvec; v = v + 1) begin
                r = vhead[v];
                end_r = (v + 1 < nvec) ? vhead[v+1] : nrec;
                if (v >= v_from) begin
                    v_lists = rec_i1[r]; v_code = rec_i3[r];
                    v_errlist = rec_i4[r]; v_erridx = rec_i5[r];
                    v_tag = rec_i6[r]; v_mem = rec_i7[r];
                    vec_total = vec_total + 1;

                    soft_reset;
                    l = 0; ncmd = 0; nexp = 0;
                    for (r = r + 1; r < end_r; r = r + 1) begin
                        if (rec_tag[r] == "S") begin
                            t_sw = rec_i3[r]; t_sh = rec_i4[r]; t_sc = rec_i5[r];
                            put_surf3(rec_i1[r], rec_i2[r], {t_sh[15:0], t_sw[15:0]}, t_sc);
                        end else if (rec_tag[r] == "L") begin
                            cnts[l] = rec_i1[r];
                            recs_start[l] = ncmd >> 3;
                            l = l + 1;
                        end else if (rec_tag[r] == "C") begin
                            cmds[ncmd+0] = rec_i1[r]; cmds[ncmd+1] = rec_i2[r];
                            cmds[ncmd+2] = rec_i3[r]; cmds[ncmd+3] = rec_i4[r];
                            cmds[ncmd+4] = rec_i5[r]; cmds[ncmd+5] = rec_i6[r];
                            cmds[ncmd+6] = rec_i7[r]; cmds[ncmd+7] = rec_i8[r];
                            ncmd = ncmd + 8;
                        end else if (rec_tag[r] == "M") begin
                            if (rec_i1[r] == 0) mwrite(rec_i2[r], rec_i3[r]);
                            else begin
                                exp_a[nexp] = rec_i2[r]; exp_w[nexp] = rec_i3[r];
                                nexp = nexp + 1;
                            end
                        end
                    end

                    base_l = VBASE;
                    for (l = 0; l < v_lists; l = l + 1) begin
                        for (j = 0; j < cnts[l] * 8; j = j + 1)
                            mwrite(base_l + j * 4, cmds[recs_start[l] * 8 + j]);
                        // FETCH 截断：可读记录 = [cmd] 条数（emit 的 reads 是「尝试
                        // 读取数」含失败那一次，不能当可读数用）
                        readable = ((l + 1 < v_lists) ? recs_start[l + 1] : (ncmd >> 3))
                                 - recs_start[l];
                        u_ram.err_lo_r = base_l + readable * 32;
                        u_ram.err_hi_r = base_l + 32'h3ff;
                        submit(base_l, cnts[l], c, ei, et);
                        checks = checks + 1;
                        if (v_errlist == l) begin
                            if (c !== v_code
                                || (v_code != 0 && (ei !== v_erridx || et !== v_tag))) begin
                                errors = errors + 1;
                                vec_fail = vec_fail + 1;
                                if (vec_fail < 14)
                                    $display("VEC-FAIL #%0d: code=%0d idx=%0d tag=%08x exp=%0d/%0d/%08x",
                                             vec_total, c, ei, et, v_code, v_erridx, v_tag);
                            end
                        end else if (c !== 32'd0) begin
                            errors = errors + 1;
                            vec_fail = vec_fail + 1;
                            $display("VEC-FAIL #%0d list%0d 应成功实得 code=%0d",
                                     vec_total, l, c);
                        end
                        base_l = base_l + 32'h400;
                    end

                    if (v_mem != 0) begin
                        for (j = 0; j < nexp; j = j + 1) begin
                            checks = checks + 1;
                            if (mread(exp_a[j]) !== exp_w[j]) begin
                                errors = errors + 1;
                                vec_fail = vec_fail + 1;
                                if (vec_fail < 14)
                                    $display("VEC-MEM #%0d: [%08x] got=%08x exp=%08x",
                                             vec_total, exp_a[j], mread(exp_a[j]), exp_w[j]);
                            end
                        end
                    end
                end
            end
        end
    endtask

    // =====================================================================
    // 定向段（D1~D16）
    // =====================================================================
    task directed_all;
        reg [31:0] s;
        reg [31:0] perf0, perf1, perf2;
        integer j;
        reg [127:0] tname;

        begin
            // ---- D1 跨 4KB 的批拆分：批首 0xFE0（1 条 + 15 条两笔），全部可读
            tname = "d1_split_4k";
            soft_reset;
            surf16(0, WORK);
            for (j = 0; j < 15; j = j + 1) begin
                mwrite(LBASE + 32'hFE0 + j*32 +  0, 32'h0000_0003);    // SET_CLIP
                mwrite(LBASE + 32'hFE0 + j*32 +  4, 32'h0000_0000);
                mwrite(LBASE + 32'hFE0 + j*32 + 12, 32'h0280_01E0);    // 640×480
            end
            mwrite(LBASE + 32'hFE0 + 15*32 +  0, 32'h0000_0001);       // FILL COPY dst0
            mwrite(LBASE + 32'hFE0 + 15*32 + 12, 32'h0001_0001);
            mwrite(LBASE + 32'hFE0 + 15*32 + 16, 32'h00AB_CD12);
            mwrite(WORK, 32'h0000_0000);
            apb_wr(R_CTRL, 32'h1);
            submit_check(LBASE + 32'hFE0, 16'd16, 32'd0, 0, 0, tname);
            check_mem(WORK, 32'h00AB_CD12, tname);

            // ---- D2 LIST_BASE 非 32 字节对齐：FETCH（索引 0、TAG 0）
            tname = "d2_misaligned";
            soft_reset;
            apb_wr(R_CTRL, 32'h1);
            mwrite(LBASE, 32'h0000_0000);
            submit_check(LBASE + 32'h10, 16'd1, 32'd9, 32'd0, 32'd0, tname);

            // ---- D3a 32 位地址末端：第 0 条 END（别名字 0x3FFFE，地址 0x3FFFE0）
            tname = "d3_end_first";
            soft_reset;
            apb_wr(R_CTRL, 32'h1);
            mwrite(32'h003F_FFE0, 32'h0000_00FF);
            mwrite(32'h003F_FFFC, 32'h0000_00A5);
            submit_check(32'hFFFF_FFE0, 16'd2, 32'd0, 0, 0, tname);

            // ---- D3b 第 0 条 FILL、第 1 条不可取 → FETCH 索引 1（第 0 条已执行）
            tname = "d3b_unfetchable1";
            soft_reset;
            surf16(0, WORK + 32'h0100);
            mwrite(32'h003F_FFE0, 32'h0000_0001);
            mwrite(32'h003F_FFE8, 32'h0000_0000);
            mwrite(32'h003F_FFEC, 32'h0001_0001);
            mwrite(32'h003F_FFF0, 32'h0011_2233);
            mwrite(WORK + 32'h0100, 32'h0000_0000);
            submit_check(32'hFFFF_FFE0, 16'd2, 32'd9, 32'd1, 32'd0, tname);
            check_mem(WORK + 32'h0100, 32'h0011_2233, tname);

            // ---- D4a 索引 ≥16 的 FETCH：20 条、可读截在 17 → ERROR_INDEX=17
            tname = "d4a_fetch17";
            soft_reset;
            put_surf3(0, WORK + 32'h0200, 32'h0020_0010, 32'h8000_0006); // 16×32, stride 64
            for (j = 0; j < 20; j = j + 1) begin
                mwrite(LBASE + 32'h400 + j*32 +  0, 32'h0000_0001);
                mwrite(LBASE + 32'h400 + j*32 +  8, j << 16);           // dst (0,j)
                mwrite(LBASE + 32'h400 + j*32 + 12, 32'h0001_0001);
                mwrite(LBASE + 32'h400 + j*32 + 16, 32'h0100_0000 + j);
                mwrite(WORK + 32'h0200 + j*64, 32'h0000_0000);
            end
            u_ram.err_lo_r = LBASE + 32'h400 + 17*32;
            u_ram.err_hi_r = LBASE + 32'h400 + 32'h3ff;
            submit_check(LBASE + 32'h400, 16'd20, 32'd9, 32'd17, 32'd0, tname);
            // XRGB 目的：X 字节恒 0，写入值是颜色的低 24 位
            for (j = 0; j < 17; j = j + 1)
                check_mem(WORK + 32'h0200 + j*64, 32'h0000_0000 + j, tname);
            check_mem(WORK + 32'h0200 + 17*64, 32'h0000_0000, tname);

            // ---- D4b 索引 ≥16 的 BUS_WRITE：第 17 条的目的表面落在 SLVERR 区
            tname = "d4b_bus_write17";
            soft_reset;
            put_surf3(0, 32'h0031_0000, 32'h0020_0010, 32'h8000_0006);
            put_surf3(1, 32'h0030_0000, 32'h0020_0010, 32'h8000_0006); // 高 32 行
            for (j = 0; j < 20; j = j + 1) begin
                mwrite(LBASE + 32'h800 + j*32 +  0,
                       32'h0000_0001 | ((j == 17) ? 32'h0010_0000 : 32'h0000_0000));
                mwrite(LBASE + 32'h800 + j*32 +  8, j << 16);
                mwrite(LBASE + 32'h800 + j*32 + 12, 32'h0001_0001);
                mwrite(LBASE + 32'h800 + j*32 + 16, 32'h0200_0000 + j);
                mwrite(LBASE + 32'h800 + j*32 + 28,
                       (j == 17) ? 32'h0000_0011 : 32'h0000_0000);
            end
            u_ram.err_lo_r = 32'h0030_0000;
            u_ram.err_hi_r = 32'h0030_0fff;
            submit_check(LBASE + 32'h800, 16'd20, 32'd11, 32'd17, 32'h0000_0011, tname);

            // ---- D5 BUS_READ 端到端：源像素在 SLVERR 区；后续命令无副作用
            tname = "d5_bus_read";
            soft_reset;
            surf16(0, 32'h0030_0000);
            surf16(1, WORK + 32'h0300);
            surf16(2, WORK + 32'h0400);
            u_ram.err_lo_r = 32'h0030_0000;
            u_ram.err_hi_r = 32'h0030_0fff;
            mwrite(LBASE + 32'hC00 +  0, 32'h0010_0002);   // BLIT COPY src0 dst1
            mwrite(LBASE + 32'hC00 + 12, 32'h0010_0010);
            mwrite(LBASE + 32'hC00 + 28, 32'h0000_00C0);
            mwrite(LBASE + 32'hC00 + 32, 32'h0000_0001);   // FILL dst0
            mwrite(LBASE + 32'hC00 + 44, 32'h0001_0001);
            mwrite(LBASE + 32'hC00 + 48, 32'h0055_6677);
            mwrite(WORK + 32'h0300, 32'h0012_3456);
            mwrite(WORK + 32'h0340, 32'h0065_4321);
            mwrite(WORK + 32'h0400, 32'h0000_0000);
            apb_wr(R_IRQ_EN, 32'h2);
            submit_check(LBASE + 32'hC00, 16'd2, 32'd10, 32'd0, 32'h0000_00C0, tname);
            checks = checks + 1;
            if (irq_b !== 1'b1) begin
                errors = errors + 1;
                $display("IRQ-FAIL %0s: irq_b=%b", tname, irq_b);
            end
            apb_wr(R_IRST, 32'h2);
            apb_rd(R_IRST, s);
            checks = checks + 1;
            if (irq_b !== 1'b0 || s[1] !== 1'b0) begin
                errors = errors + 1;
                $display("IRQ-FAIL %0s 清不掉: irq_b=%b stat=%08x", tname, irq_b, s);
            end
            check_mem(WORK + 32'h0300, 32'h0012_3456, tname); // 读错的行不得写回
            check_mem(WORK + 32'h0340, 32'h0065_4321, tname); // 错误后的行也不得写回
            check_mem(WORK + 32'h0400, 32'h0000_0000, tname);
            apb_wr(R_IRQ_EN, 32'h0);

            // ---- D6 BUS_WRITE 端到端：目的在 SLVERR 区
            tname = "d6_bus_write";
            soft_reset;
            surf16(0, WORK + 32'h0500);
            surf16(1, 32'h0030_0000);
            for (j = 0; j < 16; j = j + 1) mwrite(WORK + 32'h0500 + j*4, 32'h1234_5600 + j);
            u_ram.err_lo_r = 32'h0030_0000;
            u_ram.err_hi_r = 32'h0030_0fff;
            mwrite(LBASE + 32'h1000 +  0, 32'h0010_0002);
            mwrite(LBASE + 32'h1000 + 12, 32'h0010_0010);
            mwrite(LBASE + 32'h1000 + 28, 32'h0000_00D1);
            submit_check(LBASE + 32'h1000, 16'd1, 32'd11, 32'd0, 32'h0000_00D1, tname);

            // ---- D7 超时：取数方向被按住超过 T=4096 拍；放行后回空闲、可再提交
            tname = "d7_timeout";
            soft_reset;
            surf16(0, WORK + 32'h0600);
            mwrite(WORK + 32'h0600, 32'h0A0B_0C0D);
            mwrite(LBASE + 32'h1400 +  0, 32'h0000_0001);
            mwrite(LBASE + 32'h1400 + 12, 32'h0001_0001);
            mwrite(LBASE + 32'h1400 + 16, 32'h0A0B_0C0D);
            apb_wr(R_LBASE, LBASE + 32'h1400);
            apb_wr(R_LCNT, 32'd1);
            apb_wr(R_DOOR, 32'h1);
            guard_g = 0;
            while (dut.u_cmd.fd_valid !== 1'b1 && guard_g < 20000) begin
                step;
                guard_g = guard_g + 1;
            end
            ar_hold = 1'b1;
            repeat (4300) step;
            ar_hold = 1'b0;
            guard_g = 0;
            apb_rd(R_STATUS, s);
            while (((s & (ST_DONE | ST_ERR)) == 32'd0) && guard_g < 100000) begin
                apb_rd(R_STATUS, s);
                guard_g = guard_g + 1;
            end
            checks = checks + 1;
            if (s[7:4] !== 4'd12) begin
                errors = errors + 1;
                $display("CASE-FAIL %0s: code=%0d exp=12 (status=%08x)", tname, s[7:4], s);
            end
            checks = checks + 1;
            apb_rd(R_STATUS, s);
            if (s[0] !== 1'b0) begin
                errors = errors + 1;
                $display("CASE-FAIL %0s: 引擎没有回到空闲 (status=%08x)", tname, s);
            end
            submit_check(LBASE + 32'h1400, 16'd1, 32'd0, 0, 0, tname);
            check_mem(WORK + 32'h0600, 32'h000B_0C0D, tname);   // XRGB：X 字节恒 0

            // ---- D8 软复位：清 valid、裁剪框恢复全游戏区、之后能再次提交
            tname = "d8_soft_reset";
            soft_reset;
            surf16(0, WORK + 32'h0700);
            mwrite(LBASE + 32'h1800 +  0, 32'h0000_0003);      // SET_CLIP 8×8
            mwrite(LBASE + 32'h1800 +  4, 32'h0000_0000);
            mwrite(LBASE + 32'h1800 + 12, 32'h0008_0008);
            apb_wr(R_CTRL, 32'h1);
            submit_check(LBASE + 32'h1800, 16'd1, 32'd0, 0, 0, tname);
            put_surf3(1, WORK, 32'h0010_0010, 32'h8000_0006);
            soft_reset;
            apb_rd(12'h040 + 1*12 + 8, s);
            checks = checks + 1;
            if (s[31] !== 1'b0) begin
                errors = errors + 1;
                $display("CASE-FAIL %0s: valid 没清 (ctrl=%08x)", tname, s);
            end
            surf16(0, WORK + 32'h0700);
            mwrite(LBASE + 32'h1840 +  0, 32'h0000_0001);      // FILL 16×16 @ (0,0)
            mwrite(LBASE + 32'h1840 + 12, 32'h0010_0010);
            mwrite(LBASE + 32'h1840 + 16, 32'h0777_7777);
            for (j = 0; j < 16; j = j + 1) mwrite(WORK + 32'h0700 + j*64, 32'h0000_0000);
            submit_check(LBASE + 32'h1840, 16'd1, 32'd0, 0, 0, tname);
            // 裁剪框已复位为全游戏区：16 行全部写入（若还是 8×8 就只有 8 行）；
            // XRGB 目的 X 字节恒 0
            for (j = 0; j < 16; j = j + 1)
                check_mem(WORK + 32'h0700 + j*64, 32'h0077_7777, tname);

            // ---- D9 跨命令写后读：FILL 写 A，紧接 BLIT 从别名表面 B 读同一块
            tname = "d9_hazard_war";
            soft_reset;
            put_surf3(0, WORK + 32'h0800, 32'h0010_0010, 32'h8000_0006);  // A
            put_surf3(1, WORK + 32'h0800, 32'h0010_0010, 32'h8000_0006);  // B = A 别名
            put_surf3(2, WORK + 32'h0900, 32'h0010_0010, 32'h8000_0006);  // C
            mwrite(LBASE + 32'h1880 +  0, 32'h0000_0001);      // FILL A 全部
            mwrite(LBASE + 32'h1880 + 12, 32'h0010_0010);
            mwrite(LBASE + 32'h1880 + 16, 32'h0F1E_2D3C);
            mwrite(LBASE + 32'h1880 + 32, 32'h0021_0002);      // BLIT src=B dst=C
            mwrite(LBASE + 32'h1880 + 36, 32'h0000_0000);
            mwrite(LBASE + 32'h1880 + 40, 32'h0000_0000);
            mwrite(LBASE + 32'h1880 + 44, 32'h0010_0010);
            for (j = 0; j < 16; j = j + 1) mwrite(WORK + 32'h0900 + j*4, 32'h0000_0000);
            apb_wr(R_CTRL, 32'h1);
            submit_check(LBASE + 32'h1880, 16'd2, 32'd0, 0, 0, tname);
            // XRGB 目的：X 字节恒 0
            for (j = 0; j < 16; j = j + 1)
                check_mem(WORK + 32'h0900 + j*4, 32'h001E_2D3C, tname);

            // ---- D10 首错顺序：前一条 BUS_WRITE 先于后一条 BAD_FIELD
            tname = "d10_err_before_field";
            soft_reset;
            surf16(0, 32'h0030_0000);
            surf16(1, WORK + 32'h0A00);
            u_ram.err_lo_r = 32'h0030_0000; u_ram.err_hi_r = 32'h0030_0fff;
            mwrite(LBASE + 32'h1900 +  0, 32'h0000_0001);      // FILL dst0（BUS_WRITE）
            mwrite(LBASE + 32'h1900 + 12, 32'h0001_0001);
            mwrite(LBASE + 32'h1900 + 16, 32'h0042_0000);
            mwrite(LBASE + 32'h1900 + 28, 32'h0000_0042);
            mwrite(LBASE + 32'h1900 + 32, 32'h0000_0101);      // FILL + key 位 → BAD_FIELD
            submit_check(LBASE + 32'h1900, 16'd2, 32'd11, 32'd0, 32'h0000_0042, tname);

            // ---- D11 零面积/NOP/SET_CLIP 混排不死锁
            tname = "d11_no_deadlock";
            soft_reset;
            surf16(0, WORK + 32'h0B00);
            mwrite(LBASE + 32'h1980 +  0, 32'h0000_0001);      // FILL h=0（零面积）
            mwrite(LBASE + 32'h1980 + 12, 32'h0004_0000);
            mwrite(LBASE + 32'h1980 + 16, 32'h0AAAAAAA);
            mwrite(LBASE + 32'h1980 + 32, 32'h0000_0000);      // NOP
            mwrite(LBASE + 32'h1980 + 64, 32'h0000_0003);      // SET_CLIP 全游戏区
            mwrite(LBASE + 32'h1980 + 68, 32'h0000_0000);
            mwrite(LBASE + 32'h1980 + 76, 32'h0280_01E0);
            mwrite(LBASE + 32'h1980 + 96, 32'h0000_0001);      // FILL 正常
            mwrite(LBASE + 32'h1980 + 108, 32'h0001_0001);
            mwrite(LBASE + 32'h1980 + 112, 32'h0055_5555);
            mwrite(WORK + 32'h0B00, 32'h0000_0000);
            apb_wr(R_CTRL, 32'h1);
            submit_check(LBASE + 32'h1980, 16'd4, 32'd0, 0, 0, tname);
            check_mem(WORK + 32'h0B00, 32'h0055_5555, tname);  // XRGB：X 字节恒 0

            // ---- D12 PERF 对账抽样：CMD/PIXEL/KEY_SKIP 在已知负载上精确
            tname = "d12_perf";
            soft_reset;
            surf16(0, WORK + 32'h0C00);
            surf16(1, WORK + 32'h0D00);
            for (j = 0; j < 16; j = j + 1) mwrite(WORK + 32'h0C00 + j*4, 32'h0055_5555);
            for (j = 0; j < 8; j = j + 1)  mwrite(WORK + 32'h0C00 + j*4, 32'h0000_1111);
            mwrite(LBASE + 32'h1A00 +  0, 32'h0010_1002);      // BLIT key src0 dst1
            mwrite(LBASE + 32'h1A00 + 12, 32'h0001_0010);      // 16×1：单行 16 像素
            mwrite(LBASE + 32'h1A00 + 20, 32'h0000_1111);      // 色键字（位在 w0[12]，已置）
            apb_wr(R_CTRL, 32'h5);                             // EN | CNT_CLR
            submit_check(LBASE + 32'h1A00, 16'd1, 32'd0, 0, 0, tname);
            apb_rd(R_PERF + 4*0, perf0);
            apb_rd(R_PERF + 4*1, perf1);
            apb_rd(R_PERF + 4*2, perf2);
            checks = checks + 3;
            if (perf0 !== 32'd1) begin errors = errors + 1;
                $display("PERF-FAIL CMD=%0d exp=1", perf0); end
            if (perf1 !== 32'd16) begin errors = errors + 1;
                $display("PERF-FAIL PIXEL=%0d exp=16", perf1); end  // 16×1 = 16 像素
            if (perf2 !== 32'd8) begin errors = errors + 1;
                $display("PERF-FAIL KEY_SKIP=%0d exp=8", perf2); end
            // 色键命中的 8 个像素不写，后 8 个照常 COPY
            for (j = 0; j < 8; j = j + 1)
                check_mem(WORK + 32'h0D00 + j*4, 32'h0000_0000, tname);
            for (j = 8; j < 16; j = j + 1)
                check_mem(WORK + 32'h0D00 + j*4, 32'h0055_5555, tname);

            // ---- D13 16 条 NOP：一笔 32 拍取数，RD_BEAT 不得截成 4 位
            tname = "d13_fetch_beats";
            soft_reset;
            for (j = 0; j < 128; j = j + 1)
                mwrite(LBASE + 32'h1C00 + j*4, 32'd0);
            apb_wr(R_CTRL, 32'h5);                    // EN | CNT_CLR
            submit_check(LBASE + 32'h1C00, 16'd16, 32'd0, 0, 0, tname);
            apb_rd(R_PERF + 4*3, perf0);
            apb_rd(R_PERF + 4*5, perf1);
            checks = checks + 2;
            if (perf0 !== 32'd1) begin errors = errors + 1;
                $display("PERF-FAIL RD_BURST=%0d exp=1", perf0); end
            if (perf1 !== 32'd32) begin errors = errors + 1;
                $display("PERF-FAIL RD_BEAT=%0d exp=32", perf1); end

            // ---- D14 LIST_COUNT 是整字寄存器，高 16 位不得静默丢失
            tname = "d14_count_width";
            soft_reset;
            for (j = 0; j < 128; j = j + 1)
                mwrite(LBASE + 32'h1E00 + j*4, 32'd0);
            mwrite(LBASE + 32'h1E00, 32'h0000_007f); // 第 0 条非法操作码
            apb_wr(R_CTRL, 32'h1);
            submit_check(LBASE + 32'h1E00, 32'h0001_0000, 32'd1, 0, 0, tname);
            apb_rd(R_LCNT, perf0);
            checks = checks + 1;
            if (perf0 !== 32'h0001_0000) begin errors = errors + 1;
                $display("CASE-FAIL LIST_COUNT=%08x exp=00010000", perf0); end

            // ---- D15 逐条回退时超时：ERR 只能在取数排空后与 BUSY=0 同时可见
            tname = "d15_fallback_timeout";
            soft_reset;
            for (j = 0; j < 16; j = j + 1)
                mwrite(LBASE + 32'h2000 + j*4, 32'd0);
            u_ram.err_lo_r = LBASE + 32'h2020;
            u_ram.err_hi_r = LBASE + 32'h203f; // 批量读取第 1 条出错，回退第 0 条可读
            apb_wr(R_CTRL, 32'h1);
            apb_wr(R_LBASE, LBASE + 32'h2000);
            apb_wr(R_LCNT, 32'd2);
            apb_wr(R_DOOR, 32'h1);
            guard_g = 0;
            while (dut.u_cmd.st !== 4'd9 && guard_g < 20000) begin
                @(negedge clk);
                guard_g = guard_g + 1;
            end
            checks = checks + 1;
            if (guard_g >= 20000) begin errors = errors + 1;
                $display("CASE-FAIL %0s: 未进入逐条回退", tname); end
            ar_hold = 1'b1;
            repeat (4300) step;
            apb_rd(R_STATUS, s);
            checks = checks + 1;
            if (s[0] !== 1'b1 || s[3] !== 1'b0) begin errors = errors + 1;
                $display("CASE-FAIL %0s: 排空前提前报错 status=%08x", tname, s); end
            ar_hold = 1'b0;
            guard_g = 0;
            while (((s & (ST_DONE | ST_ERR)) == 32'd0) && guard_g < 100000) begin
                apb_rd(R_STATUS, s);
                guard_g = guard_g + 1;
            end
            checks = checks + 1;
            if (s[7:4] !== 4'd12 || s[3] !== 1'b1 || s[0] !== 1'b0) begin
                errors = errors + 1;
                $display("CASE-FAIL %0s: 收尾 status=%08x exp=TIMEOUT且空闲", tname, s); end

            // ---- D16 APB 错误响应须在 ACCESS 完成边沿可见
            soft_reset;
            apb_wr(R_CTRL, 32'd0);
            apb_wr_reject(R_DOOR, 32'd1); // EN=0
            apb_wr_reject(12'hffc, 32'h1); // 未映射
            apb_wr_reject(12'h000, 32'h1); // ID 只读
            apb_rd_reject(R_DOOR);          // DOORBELL 只写
            apb_rd_reject(12'hffc);         // 未映射
        end
    endtask

    // fe_cidx_err 一致性监视
    always @(posedge clk) begin
        if (!rst && fe_cidx_err === 1'b1) begin
            errors = errors + 1;
            $display("CIDX-ERR: 取数流上下文低 4 位与期望不符 t=%0t", $time);
        end
    end

    // 主流程
    reg [31:0] id_rd, caps_rd;
    initial begin
        rst = 1'b1;
        repeat (8) step();
        rst = 1'b0;
        repeat (4) step();

        apb_rd(12'h000, id_rd);
        checks = checks + 1;
        if (id_rd !== 32'h4746_5801) begin
            errors = errors + 1;
            $display("ID-FAIL got=%08x", id_rd);
        end
        apb_rd(12'h004, caps_rd);
        checks = checks + 1;
        if (caps_rd !== 32'h0000_040f) begin
            errors = errors + 1;
            $display("CAPS-FAIL got=%08x exp=0000040f", caps_rd);
        end

        parse_vectors;
        if (!$test$plusargs("directed_only")) run_vectors;
        if (!$test$plusargs("vectors_only")) directed_all;

        $display("checks=%0d errors=%0d vectors=%0d vec_fail=%0d",
                 checks, errors, vec_total, vec_fail);
        if (errors == 0)
            $display("RESULT: PASS");
        else
            $display("RESULT: FAIL (%0d errors)", errors);
        $finish;
    end

    // 超时看门狗
    initial begin
        #600_000_000;
        $display("RESULT: FAIL (全局超时)");
        $finish;
    end
endmodule
`default_nettype wire
