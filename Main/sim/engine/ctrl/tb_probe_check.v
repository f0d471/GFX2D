// 前期研究探针的测试台：三种结构（组合 / 状态机 / 预计算）实现同一份十步检查。
// 判据三路：
//   1 定向用例：期望值按 sw/lib/gfx/check.c 的 gfx_cmd_check() 逐条手推（注释给出依据），
//     三种结构的结果都必须等于期望；
//   2 随机互比：随机表面表与命令 3000 组，三种结构的错误码两两相同（随机组不算期望）；
//   3 预计算位等价：结构 2 的 16 位预计算位在每一拍都等于存储三字的现行谓词
//     （谓词用 probe_check 里的 check_struct_ok 现算，不复制逻辑文本）。
// 权威语义的 159 条向量对账归 engine_top 的正式单元仿真，本台只证三种结构同判。
// 命令字位号备忘：w0=[31:0]（op[7:0]、eq[11:8]、key[12]、tint[13]、rsvd[15:14]、
//   src[19:16]、dst[23:20]、c[31:24]），w1=[63:32]，w2=[95:64]，w3=[127:96]，
//   w4=[159:128]，w5=[191:160]，w6=[223:192]，w7=[255:224]。
`timescale 1ns/1ps
`default_nettype none
module tb_probe_check;
    reg clk = 1'b0;
    reg rst = 1'b1;
    always #5 clk = ~clk;

    // ---- 公共激励（三种结构同一份，表面表内容保持一致）----
    reg          surf_we = 1'b0;
    reg  [3:0]   surf_slot = 4'b0;
    reg  [1:0]   surf_wsel = 2'b0;
    reg  [31:0]  surf_wdata = 32'b0;
    reg  [255:0] cmd_data = 256'b0;
    reg          cmd_valid = 1'b0;

    wire rdy0, rdy1, rdy2;
    wire       es0, es1, es2, co0, co1, co2;
    wire [3:0] ec0, ec1, ec2;

    probe_check #(.MODE(0)) u0(.clk(clk), .rst(rst),
        .surf_we(surf_we), .surf_slot(surf_slot), .surf_wsel(surf_wsel), .surf_wdata(surf_wdata),
        .cmd_data(cmd_data), .cmd_valid(cmd_valid), .cmd_ready(rdy0),
        .err_set(es0), .err_code(ec0), .check_ok(co0));
    probe_check #(.MODE(1)) u1(.clk(clk), .rst(rst),
        .surf_we(surf_we), .surf_slot(surf_slot), .surf_wsel(surf_wsel), .surf_wdata(surf_wdata),
        .cmd_data(cmd_data), .cmd_valid(cmd_valid), .cmd_ready(rdy1),
        .err_set(es1), .err_code(ec1), .check_ok(co1));
    probe_check #(.MODE(2)) u2(.clk(clk), .rst(rst),
        .surf_we(surf_we), .surf_slot(surf_slot), .surf_wsel(surf_wsel), .surf_wdata(surf_wdata),
        .cmd_data(cmd_data), .cmd_valid(cmd_valid), .cmd_ready(rdy2),
        .err_set(es2), .err_code(ec2), .check_ok(co2));

    // ---- 结果监视：每个结构每次提交后的第一个脉冲即其结果 ----
    reg       done0 = 1'b0, done1 = 1'b0, done2 = 1'b0;
    reg [3:0] res0 = 4'b0, res1 = 4'b0, res2 = 4'b0;
    always @(posedge clk) begin
        if (!rst) begin
            if ((es0 || co0) && !done0) begin done0 <= 1'b1; res0 <= es0 ? ec0 : 4'd0; end
            if ((es1 || co1) && !done1) begin done1 <= 1'b1; res1 <= es1 ? ec1 : 4'd0; end
            if ((es2 || co2) && !done2) begin done2 <= 1'b1; res2 <= es2 ? ec2 : 4'd0; end
        end
    end

    integer errors = 0;
    integer checks = 0;
    integer dir_cases = 0;

    // ---- 判据 3：结构 2 的预计算位逐拍等价于存储谓词 ----
    genvar gi;
    generate
        for (gi = 0; gi < 16; gi = gi + 1) begin : g_eq
            wire [31:0] s0 = u2.g_pre_mode.u_surf.tab0[gi];
            wire [31:0] s1 = u2.g_pre_mode.u_surf.tab1[gi];
            wire [31:0] s2 = u2.g_pre_mode.u_surf.tab2[gi];
            wire        live;
            check_struct_ok u_live(.s0(s0), .s1(s1), .s2(s2), .ok(live));
            always @(posedge clk) begin
                if (!rst && u2.g_pre_mode.u_surf.g_pre.pre[gi] !== live) begin
                    errors = errors + 1;
                    if (errors < 20)
                        $display("PRECALC_DRIFT slot=%0d t=%0t pre=%b live=%b",
                                 gi, $time, u2.g_pre_mode.u_surf.g_pre.pre[gi], live);
                end
            end
        end
    endgenerate

    // ---- 激励任务 ----
    task automatic step; begin @(posedge clk); end endtask

    task automatic set_surf(input [3:0] slot, input [31:0] w0, input [31:0] w1, input [31:0] w2);
        begin
            surf_we = 1'b1; surf_slot = slot;
            surf_wsel = 2'd0; surf_wdata = w0; step();
            surf_wsel = 2'd1; surf_wdata = w1; step();
            surf_wsel = 2'd2; surf_wdata = w2; step();
            surf_we = 1'b0; step();
        end
    endtask

    // 提交一条命令，等三种结构都出结果。exp != 4'hF 时逐结构比期望（定向组）；
    // 无论定向还是随机，三路结果必须互同（随机组的唯一判据）。
    task automatic submit(input [255:0] cmd, input [3:0] exp, input [127:0] name);
        begin
            done0 = 1'b0; done1 = 1'b0; done2 = 1'b0;
            cmd_data = cmd; step(); cmd_valid = 1'b1; step(); cmd_valid = 1'b0;
            wait (done0 && done1 && done2);
            #1;
            checks = checks + 1;
            if (res0 !== res1 || res1 !== res2) begin
                errors = errors + 1;
                $display("DISAGREE %0s: comb=%0d fsm=%0d pre=%0d", name, res0, res1, res2);
            end
            if (exp != 4'hF && (res0 !== exp || res1 !== exp || res2 !== exp)) begin
                errors = errors + 1;
                $display("CASE-FAIL %0s: exp=%0d comb=%0d fsm=%0d pre=%0d",
                         name, exp, res0, res1, res2);
            end
            step();
        end
    endtask

    // 命令打包：w0 = {c, dst, src, rsvd=0, tint=0, key, eq, op}；w7 标签固定 0xA5
    function [255:0] pack(
        input [7:0]  op, input [3:0] eq, input key,
        input [3:0]  src, input [3:0] dst, input [7:0] c,
        input [31:0] w1, input [31:0] w2, input [31:0] w3,
        input [31:0] w4, input [31:0] w5);
        pack = {32'h0000_00a5, 32'b0, w5, w4, w3, w2, w1,
                {c, dst, src, 2'b00, 1'b0, key, eq, op}};
    endfunction

    // 表面控制字打包：{valid, 15'b0, fmt[15:12], rsvd[11:10]=0, space[9:8], rsvd[7:5]=0, sl}
    function [31:0] ctrl(input valid, input [3:0] fmt, input [1:0] space, input [4:0] sl);
        ctrl = {valid, 15'b0, fmt, 2'b00, space, 3'b000, sl};
    endfunction

    task automatic rnd32(inout [31:0] lfsr);
        begin
            lfsr = {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
        end
    endtask

    integer k;
    reg [31:0] rw = 32'hDEAD_BEEF;
    reg [255:0] rc;
    reg [7:0]   op_r;

    initial begin
        rst = 1'b1;
        repeat (4) step();
        rst = 1'b0;
        step();

        // ---------------- 表面表登记 ----------------
        set_surf(4'd0, 32'h0000_1000, 32'h01E0_0280, ctrl(1'b1, 4'd0, 2'd0, 5'd12)); // 640×480 XRGB 合法
        set_surf(4'd1, 32'h0000_2000, 32'h0010_0010, ctrl(1'b1, 4'd0, 2'd0, 5'd6));  // 16×16 XRGB 合法
        set_surf(4'd2, 32'h0000_1000, 32'h01E0_0280, ctrl(1'b0, 4'd0, 2'd0, 5'd12)); // 同槽 0，未登记
        set_surf(4'd3, 32'h0000_1002, 32'h0010_0010, ctrl(1'b1, 4'd0, 2'd0, 5'd6));  // base 未对齐
        set_surf(4'd4, 32'h0000_2000, 32'h0010_0000, ctrl(1'b1, 4'd0, 2'd0, 5'd6));  // width=0
        set_surf(4'd5, 32'h0000_2000, 32'h0010_0010, ctrl(1'b1, 4'd0, 2'd0, 5'd1));  // sl=1
        set_surf(4'd6, 32'h0000_2000, 32'h0010_0280, ctrl(1'b1, 4'd0, 2'd0, 5'd7));  // 640×4 > 2^7
        set_surf(4'd7, 32'hFFFF_F000, 32'h0008_0280, ctrl(1'b1, 4'd0, 2'd0, 5'd12)); // 末像素溢出
        set_surf(4'd8,  32'h0000_2000, 32'h0010_0010, ctrl(1'b1, 4'd0, 2'd0, 5'd6) | 32'h0000_0020); // rsvd[7:5]
        set_surf(4'd9,  32'h0000_2000, 32'h0010_0010, ctrl(1'b1, 4'd0, 2'd0, 5'd6) | 32'h0000_0400); // rsvd[11:10]
        set_surf(4'd10, 32'h0000_2000, 32'h0010_0010, ctrl(1'b1, 4'd0, 2'd0, 5'd6) | 32'h0001_0000); // rsvd[30:16]
        set_surf(4'd11, 32'h0000_3000, 32'h0010_0010, ctrl(1'b1, 4'd1, 2'd0, 5'd6)); // 预乘 ARGB
        set_surf(4'd12, 32'h0000_4000, 32'h0010_0010, ctrl(1'b1, 4'd2, 2'd0, 5'd6)); // 直通 ARGB
        set_surf(4'd13, 32'h0000_5000, 32'h0010_0010, ctrl(1'b1, 4'd0, 2'd1, 5'd6)); // space=TEX
        set_surf(4'd14, 32'h0000_5000, 32'h0010_0010, ctrl(1'b0, 4'd0, 2'd1, 5'd6)); // 未登记+TEX

        // ---------------- 定向用例（期望按 check.c 手推）----------------
        // 期望码：0 OK / 1 BAD_OPCODE / 2 BAD_FIELD / 3 BAD_EQUATION / 4 BAD_SURFACE
        //         / 5 BAD_SPACE / 6 BAD_FORMAT / 7 OVERLAP / 8 BAD_PREMULT
        submit(pack(8'h00, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'b0, 32'b0, 32'b0),
               4'd0, "nop_ok");
        submit(pack(8'hFF, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'b0, 32'b0, 32'b0),
               4'd0, "end_ok");
        submit(pack(8'h05, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'b0, 32'b0, 32'b0),
               4'd1, "opcode_unk");
        submit(pack(8'h04, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'b0, 32'b0, 32'b0),
               4'd1, "opcode_band");
        submit(pack(8'h01, 4'd0, 1'b1, 4'd0, 4'd0, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd2, "fill_key_bit");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'b0, 32'b0, 32'b0)
               | (256'd1 << 13), 4'd2, "fill_tint_bit");            // w0[13]
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'b0, 32'b0, 32'b0)
               | (256'd3 << 14), 4'd2, "fill_rsvd_bit");            // w0[15:14]
        submit(pack(8'h01, 4'd0, 1'b0, 4'd3, 4'd0, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd2, "fill_src_nz");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'h0001_0001, 32'h000A_000A,
                    32'h0010_0010, 32'h00FF_8040, 32'b0), 4'd2, "fill_w1_nz");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'h0000_00FF), 4'd2, "fill_w5_nz");
        submit(pack(8'h01, 4'd3, 1'b0, 4'd0, 4'd0, 8'hAB, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd0, "fill_const_ok");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd0, 8'hAB, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd2, "fill_copy_c");
        submit(pack(8'h01, 4'd6, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd3, "fill_eq6");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd2, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_unreg");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd3, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_algn");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd4, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_w0");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd5, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_sl1");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd6, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_strd");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd7, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_ovf");
        // 大 stride 的高位乘积与三项相加可超过 33 位；期望按 check.c 的 uint64_t 式复核。
        set_surf(4'd15, 32'h0000_0000, 32'h0005_0001, ctrl(1'b1, 4'd0, 2'd0, 5'd31));
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd15, 8'h00, 32'b0, 32'h0001_0001, 32'b0,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_ovf_high_stride");
        set_surf(4'd15, 32'h8000_0000, 32'h0004_0001, ctrl(1'b1, 4'd0, 2'd0, 5'd31));
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd15, 8'h00, 32'b0, 32'h0001_0001, 32'b0,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_ovf_sum_wrap");
        set_surf(4'd15, 32'hffff_fffc, 32'h0001_0001, ctrl(1'b1, 4'd0, 2'd0, 5'd31));
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd15, 8'h00, 32'b0, 32'h0001_0001, 32'b0,
                    32'h00FF_8040, 32'b0), 4'd0, "fill_dst_boundary_ok");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd8, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_r5");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd9, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_r10");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd10, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_dst_r16");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd11, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd6, "fill_dst_fmt");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd13, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd5, "fill_dst_tex");
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd14, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd4, "fill_sfc_before_space");  // 表面先于 space
        submit(pack(8'h01, 4'd0, 1'b1, 4'd0, 4'd2, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd2, "fill_field_before_sfc");  // 字段先于表面
        submit(pack(8'h01, 4'd6, 1'b0, 4'd0, 4'd0, 8'h00, 32'h0001_0001, 32'h000A_000A,
                    32'h0010_0010, 32'h00FF_8040, 32'b0), 4'd2, "fill_field_before_eq"); // 字段先于方程
        submit(pack(8'h01, 4'd6, 1'b0, 4'd0, 4'd2, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h00FF_8040, 32'b0), 4'd3, "fill_eq_before_sfc");     // 方程先于表面
        // 零面积仍走表面与格式步（spec §4.3）
        submit(pack(8'h01, 4'd0, 1'b0, 4'd0, 4'd11, 8'h00, 32'b0, 32'b0, 32'b0,
                    32'h00FF_8040, 32'b0), 4'd6, "fill_zero_fmt");
        submit(pack(8'h01, 4'd1, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'b0,
                    32'h4080_4040, 32'b0), 4'd8, "fill_zero_premult"); // R=80 > A=40 → BAD_PREMULT
        submit(pack(8'h01, 4'd1, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h4080_4040, 32'b0), 4'd8, "fill_premult_bad");
        submit(pack(8'h01, 4'd1, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'h000A_000A, 32'h0010_0010,
                    32'h807F_4040, 32'b0), 4'd0, "fill_premult_ok");
        // BLIT：方程×源格式（spec §2 末三行）
        submit(pack(8'h02, 4'd0, 1'b0, 4'd0, 4'd1, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd0, "blit_copy_ok");
        submit(pack(8'h02, 4'd1, 1'b0, 4'd1, 4'd0, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd0, "blit_alpha_xrgb");
        submit(pack(8'h02, 4'd1, 1'b0, 4'd11, 4'd0, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd0, "blit_alpha_pm");
        submit(pack(8'h02, 4'd1, 1'b0, 4'd12, 4'd0, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd6, "blit_alpha_st");
        submit(pack(8'h02, 4'd5, 1'b0, 4'd11, 4'd0, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd6, "blit_alst_pm");
        submit(pack(8'h02, 4'd5, 1'b0, 4'd12, 4'd0, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd0, "blit_alst_st");
        submit(pack(8'h02, 4'd2, 1'b0, 4'd0, 4'd1, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd0, "blit_add");
        submit(pack(8'h02, 4'd0, 1'b0, 4'd0, 4'd1, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'h0000_0001, 32'b0), 4'd2, "blit_w4_nz");
        submit(pack(8'h02, 4'd0, 1'b0, 4'd0, 4'd1, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'h0000_00FF), 4'd2, "blit_w5_nokey");
        submit(pack(8'h02, 4'd0, 1'b1, 4'd0, 4'd1, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'h0000_00FF), 4'd0, "blit_key_ok");
        // 重叠（同表面、未裁剪半开区间、有符号）
        submit(pack(8'h02, 4'd0, 1'b0, 4'd1, 4'd1, 8'h00, 32'h0000_0000, 32'h0008_0000,
                    32'h0010_0010, 32'b0, 32'b0), 4'd7, "blit_overlap");
        submit(pack(8'h02, 4'd0, 1'b0, 4'd1, 4'd1, 8'h00, 32'h0000_0000, 32'h0010_0000,
                    32'h0010_0010, 32'b0, 32'b0), 4'd0, "blit_no_overlap");  // 相接不相交
        submit(pack(8'h02, 4'd0, 1'b0, 4'd1, 4'd1, 8'h00, 32'h0000_0000, 32'b0,
                    32'b0, 32'b0, 32'b0), 4'd0, "blit_zero_overlap");        // 零面积跳过重叠步
        submit(pack(8'h02, 4'd0, 1'b0, 4'd1, 4'd1, 8'h00, {16'hFFF0, 16'h0000},
                    {16'hFFF8, 16'h0000}, 32'h0010_0010, 32'b0, 32'b0),
               4'd7, "blit_overlap_neg");
        // prio-src-format-vs-dst-space：源格式非法 + 目的 space 非 DDR → BAD_SPACE
        submit(pack(8'h02, 4'd1, 1'b0, 4'd12, 4'd13, 8'h00, 32'b0, 32'h0010_0010,
                    32'h0010_0010, 32'b0, 32'b0), 4'd5, "blit_fmt_vs_space");
        // SET_CLIP
        submit(pack(8'h03, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0, 32'h0280_01E0,
                    32'b0, 32'b0), 4'd0, "clip_ok");
        submit(pack(8'h03, 4'd0, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'h0000_0001,
                    32'b0, 32'b0, 32'b0), 4'd2, "clip_w2_nz");
        submit(pack(8'h03, 4'd1, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0,
                    32'b0, 32'b0, 32'b0), 4'd2, "clip_eq");
        // END 的 eq 非 0 在字段步报 BAD_FIELD（BAD_EQUATION 只对 FILL/BLIT 可达）
        submit(pack(8'hFF, 4'd1, 1'b0, 4'd0, 4'd0, 8'h00, 32'b0, 32'b0,
                    32'b0, 32'b0, 32'b0), 4'd2, "end_eq_field");

        dir_cases = checks;
        if (errors == 0)
            $display("directed: %0d cases x3 structures, all as expected", dir_cases);

        // ---------------- 随机互比：3000 组 ----------------
        for (k = 0; k < 3000; k = k + 1) begin
            // 随机表面项：一半按参数构造（对齐 base、1..640×1..480、sl 6..14、fmt 0..2），
            // 一半全随机（三种结构必须同判，对不对不管）
            rnd32(rw); surf_slot = rw[3:0];
            if (rw[8]) begin
                rnd32(rw); surf_wdata = {rw[31:2], 2'b00};
                surf_we = 1'b1; surf_wsel = 2'd0; step();
                rnd32(rw); surf_wdata = {(rw[31:16] % 11'd480) + 16'd1, (rw[15:0] % 11'd640) + 16'd1};
                surf_wsel = 2'd1; step();
                rnd32(rw);
                surf_wdata = {rw[0], 15'b0, 2'b00, rw[11:10], 2'b00, rw[13:12], 3'b000,
                              (rw[18:14] % 5'd9) + 5'd6};
                surf_wsel = 2'd2; step();
            end else begin
                rnd32(rw); surf_wdata = rw;
                surf_we = 1'b1; surf_wsel = 2'd0; step();
                rnd32(rw); surf_wdata = rw; surf_wsel = 2'd1; step();
                rnd32(rw); surf_wdata = rw; surf_wsel = 2'd2; step();
            end
            surf_we = 1'b0; step();

            // 随机命令：op 偏向五种合法值，字段偏零
            rnd32(rw);
            case (rw[3:0] % 6)
                4'd0:    op_r = 8'h00;
                4'd1:    op_r = 8'h01;
                4'd2:    op_r = 8'h02;
                4'd3:    op_r = 8'h03;
                4'd4:    op_r = 8'hFF;
                default: op_r = {4'b0000, rw[7:4]};
            endcase
            rc = pack(op_r, {1'b0, rw[10:8]} % 7, rw[11],
                      rw[15:12], rw[19:16], rw[12] ? 8'hAB : 8'h00,
                      rw[20] ? 32'h0001_0001 : 32'b0,
                      rw[21] ? {rw[31:16], rw[15:0]} : 32'h000A_000A,
                      rw[22] ? {rw[31:16], rw[15:0]} : 32'h0010_0010,
                      rw[23] ? {rw[31:24] | 8'h80, rw[23:0]} : 32'h00FF_8040,
                      rw[24] ? 32'h0000_00FF : 32'b0);
            if (rw[25]) rc[223:192] = rw;      // 随机 w6
            if (rw[26]) rc[15:14]   = rw[1:0]; // 随机保留位
            if (rw[27]) rc[13]      = 1'b1;    // 随机 tint 位
            submit(rc, 4'hF, "random");
        end

        $display("directed=%0d random=%0d total=%0d errors=%0d",
                 dir_cases, checks - dir_cases, checks, errors);
        if (errors == 0)
            $display("RESULT: PASS");
        else
            $display("RESULT: FAIL (%0d errors)", errors);
        $finish;
    end

    // 超时看门狗
    initial begin
        #10_000_000;
        $display("RESULT: FAIL (timeout)");
        $finish;
    end
endmodule
`default_nettype wire
