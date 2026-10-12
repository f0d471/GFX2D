// 前期研究探针：十步检查的三种硬件结构。
// 行为的权威是 sw/lib/gfx/check.c 的 gfx_cmd_check()；
// 本文件实现同一份检查的三种结构，仿真里三路互比加定向用例（tb_probe_check.v），
// 单独综合（syn/）登记各自的 XLR 与余量。本探针不是入库 RTL，engine_cmd.v 按研究结论重写。
//
// 结构（MODE 参数）：
//   0 = 单拍组合：九个条件并行算、按顺序取首个。检查路径是
//       「命令/表面表寄存器 → 全组合 → 结果寄存器」一拍，量的是最坏组合路径。
//   1 = 多拍状态机：每拍算一步，首错即停，一条记录固定十拍出结果。
//   2 = SURF 写入时预计算：结构谓词在写表面项时算好存成一位（每槽一位），
//       检查时第五、六步只查这 16 位；其余步骤与结构 0 相同。
//       相容性（软复位清 valid、valid 最后置位的驱动契约）：
//       预计算位随每一次表面项写按「其余两字存储值 + 本次写入字」的视图重算，
//       与存储同步更新，任意一拍都等于存储三字的现行谓词；软复位与 valid 一起清零。
//       驱动契约（更新期间 valid=0、最后写控制字置位）下三种结构对任何检查同判。
//
// 第一步「取记录」（FETCH，码 9）是取数状态机的事，不在本探针；这里只产 0~8。
// 条件顺序固定、首错停止：操作码 → 保留/未用字段 → 方程 → 源表面 → 目的表面
//   → space → format → FILL 预乘约束 → BLIT 重叠（check.c 的取记录在硬件归取数）。
`default_nettype none

// ---------------------------------------------------------------- 结构谓词
// 结构合法 ⟺ valid、base%4==0、宽高非 0、2<=stride_log2<=31、
// width*4 <= 1<<stride_log2、末像素不溢出 32 位、控制字保留位（0x7fff0ce0）为 0。
// 行偏移最大接近 2^47，不能截成 33 位；三项相加也可能越过 2^33 后让 bit 32 回到 0。
// 先查 48 位行偏移的高 16 位，再用 34 位精确加低 32 位与 base、行尾字节。
module check_struct_ok(
    input  wire [31:0] s0,   // base
    input  wire [31:0] s1,   // {height, width}
    input  wire [31:0] s2,   // 控制字：[31] valid、[15:12] format、[9:8] space、[4:0] stride_log2
    output wire        ok
);
    wire [15:0] w  = s1[15:0];
    wire [15:0] h  = s1[31:16];
    wire [4:0]  sl = s2[4:0];

    wire base_al = (s0[1:0] == 2'b00);
    wire wh_nz   = (w != 16'd0) && (h != 16'd0);
    wire sl_ok   = (sl >= 5'd2);
    // w*4 <= 1<<sl ⟺ w <= 2^(sl-2)；sl-2 >= 16 时上限超出 16 位宽，恒成立
    wire [4:0] t          = sl - 5'd2;
    wire       stride_ok  = (t >= 5'd16) || (w <= (16'd1 << t));
    // 末像素：base + (h-1)<<sl + (w-1)*4 + 3 不超过 0xFFFFFFFF
    wire [15:0] hm     = h - 16'd1;
    wire [15:0] wm     = w - 16'd1;
    wire [47:0] row_off = {32'b0, hm} << sl;
    wire [33:0] last_lo = {2'b0, s0} + {2'b0, row_off[31:0]}
                        + {16'b0, wm, 2'b11};
    wire        no_ovf = (row_off[47:32] == 16'b0) && (last_lo[33:32] == 2'b0);
    wire        ctrl_ok = ((s2 & 32'h7fff0ce0) == 32'b0);

    assign ok = s2[31] && base_al && wh_nz && sl_ok && stride_ok && no_ovf && ctrl_ok;
endmodule

// ---------------------------------------------------------------- 表面表
// 16 槽 × 3 字。写口一个（APB 寄存器页用），读口两个（源、目的，组合读）。
// PRECALC=1（结构 2）时随每次写维护 16 位预计算位，复位清零。
module check_surf16 #(
    parameter PRECALC = 0
)(
    input  wire        clk,
    input  wire        rst,
    input  wire        we,
    input  wire [3:0]  slot,
    input  wire [1:0]  wsel,
    input  wire [31:0] wdata,
    input  wire [3:0]  ra_id,
    input  wire [3:0]  rb_id,
    output wire [31:0] ra_w0, ra_w1, ra_w2,
    output wire [31:0] rb_w0, rb_w1, rb_w2,
    output wire [15:0] struct_ok
);
    reg [31:0] tab0 [0:15];
    reg [31:0] tab1 [0:15];
    reg [31:0] tab2 [0:15];

    assign ra_w0 = tab0[ra_id];
    assign ra_w1 = tab1[ra_id];
    assign ra_w2 = tab2[ra_id];
    assign rb_w0 = tab0[rb_id];
    assign rb_w1 = tab1[rb_id];
    assign rb_w2 = tab2[rb_id];

    generate
        if (PRECALC != 0) begin : g_pre
            // 逐槽预测：每槽一个 check_struct_ok，读自己那三字（常量下标），
            // 本槽被写时按「写入字替换对应存储字」的视图重算并寄存。
            // 2026-10-01：共享版（三口按下标读 + 单实例 + 向量变量下标写）会让
            // efx_map 2026.1 在层次化预综合时 EXCEPTION_ACCESS_VIOLATION，
            // 数组化 pre 与拆 generate 都绕不开；逐槽常量下标绕开，语义不变。
            reg pre [0:15];
            genvar gj;
            for (gj = 0; gj < 16; gj = gj + 1) begin : g_slot
                wire        sel = we && (slot == gj[3:0]);
                wire [31:0] w0 = (sel && wsel == 2'd0) ? wdata : tab0[gj];
                wire [31:0] w1 = (sel && wsel == 2'd1) ? wdata : tab1[gj];
                wire [31:0] w2 = (sel && wsel == 2'd2) ? wdata : tab2[gj];
                wire        p;
                check_struct_ok u_p(.s0(w0), .s1(w1), .s2(w2), .ok(p));
                always @(posedge clk) begin
                    if (rst) pre[gj] <= 1'b0;
                    else if (sel) pre[gj] <= p;
                end
            end
            assign struct_ok = {pre[15], pre[14], pre[13], pre[12], pre[11], pre[10],
                                pre[9],  pre[8],  pre[7],  pre[6],  pre[5],  pre[4],
                                pre[3],  pre[2],  pre[1],  pre[0]};
        end else begin : g_raw
            assign struct_ok = 16'b0;   // 结构 0/1 现算，不用预计算位
        end
    endgenerate

    integer i;
    always @(posedge clk) begin
        if (rst) begin
            for (i = 0; i < 16; i = i + 1) begin
                tab0[i] <= 32'b0;
                tab1[i] <= 32'b0;
                tab2[i] <= 32'b0;
            end
        end else if (we) begin
            case (wsel)
                2'd0: tab0[slot] <= wdata;
                2'd1: tab1[slot] <= wdata;
                default: tab2[slot] <= wdata;
            endcase
        end
    end

endmodule

// ---------------------------------------------------------------- 第三步
// 保留位与未用字段（命令字的字段矩阵，含 w0[15:14] 与 w6 的两句散文约束）。
// 未知操作码不会走到这一步（操作码步先行），这里不计。
module check_fields(
    input  wire [255:0] cmd,
    output wire         bad
);
    wire [7:0] op  = cmd[7:0];
    wire [3:0] eq  = cmd[11:8];
    wire       key = cmd[12];
    wire       tint = cmd[13];

    wire is_nop  = (op == 8'h00);
    wire is_fill = (op == 8'h01);
    wire is_blit = (op == 8'h02);
    wire is_clip = (op == 8'h03);
    wire is_end  = (op == 8'hff);

    wire w1_nz = (cmd[63:32]   != 32'b0);
    wire w2_nz = (cmd[95:64]   != 32'b0);
    wire w3_nz = (cmd[127:96]  != 32'b0);
    wire w4_nz = (cmd[159:128] != 32'b0);
    wire w5_nz = (cmd[191:160] != 32'b0);
    wire w6_nz = (cmd[223:192] != 32'b0);
    wire c_nz  = (cmd[31:24]   != 8'b0);
    wire src_nz = (cmd[19:16]  != 4'b0);
    wire dst_nz = (cmd[23:20]  != 4'b0);
    wire eq_nz  = (eq != 4'b0);
    wire rsvd_nz = (cmd[15:14] != 2'b0);

    wire const_ok = (eq == 4'd3);   // 仅 CONST 允许 c 非零

    // NOP/END：除 w7 外全零；FILL：key、src、w1、w5、w6，加非 CONST 的 c；
    // BLIT：w4、w6，加非 CONST 的 c，加无色键时的 w5；SET_CLIP：w2、w4、w5、w6 等。
    wire z_nop_end = eq_nz || key || src_nz || dst_nz || c_nz
                   || w1_nz || w2_nz || w3_nz || w4_nz || w5_nz;
    wire z_fill    = key || src_nz || w1_nz || w5_nz || (!const_ok && c_nz);
    wire z_blit    = w4_nz || (!const_ok && c_nz) || (!key && w5_nz);
    wire z_clip    = eq_nz || key || src_nz || dst_nz || c_nz
                   || w2_nz || w4_nz || w5_nz;

    // tint 位对所有操作码都必须为 0（check.c 的 must_zero_mask 恒含 F_TINT）
    assign bad = rsvd_nz || tint || w6_nz
              || ((is_nop || is_end) && z_nop_end)
              || (is_fill && z_fill)
              || (is_blit && z_blit)
              || (is_clip && z_clip);
endmodule

// ---------------------------------------------------------------- 探针本体
module probe_check #(
    parameter MODE = 0
)(
    input  wire        clk,
    input  wire        rst,
    // 表面表写口（APB 寄存器页）
    input  wire        surf_we,
    input  wire [3:0]  surf_slot,
    input  wire [1:0]  surf_wsel,
    input  wire [31:0] surf_wdata,
    // 待检命令
    input  wire [255:0] cmd_data,
    input  wire        cmd_valid,
    output wire        cmd_ready,
    // 结果：err_set/check_ok 是单拍脉冲，与 cmd_valid 的受理一一对应
    output reg         err_set,
    output reg  [3:0]  err_code,
    output reg         check_ok
);
    wire [7:0]  op    = cmd_data[7:0];
    wire [3:0]  eq    = cmd_data[11:8];
    wire [3:0]  srcid = cmd_data[19:16];
    wire [3:0]  dstid = cmd_data[23:20];

    wire is_fill = (op == 8'h01);
    wire is_blit = (op == 8'h02);
    wire op_ok   = (op == 8'h00) || is_fill || is_blit
                 || (op == 8'h03) || (op == 8'hff);

    wire f_bad;
    check_fields u_fields(.cmd(cmd_data), .bad(f_bad));

    wire eq_bad = (eq > 4'd5);

    // 表面表与两路结构判定：结构 0/1 现算谓词，结构 2 查预计算位。
    // 两种形态用 generate 彻底分开（2026-10-01：跨模式的常数三态选择会让
    // Efinity 2026.1 的 efx_map 在层次化预综合 probe_check(MODE=2) 时
    // EXCEPTION_ACCESS_VIOLATION 崩溃，绕开它；对仿真行为无影响）。
    wire        src_ok, dst_ok;
    wire [31:0] sa0, sa1, sa2, sb0, sb1, sb2;
    wire [15:0] struct_ok;
    generate
        if (MODE == 2) begin : g_pre_mode
            check_surf16 #(.PRECALC(1)) u_surf(
                .clk(clk), .rst(rst),
                .we(surf_we), .slot(surf_slot), .wsel(surf_wsel), .wdata(surf_wdata),
                .ra_id(srcid), .rb_id(dstid),
                .ra_w0(sa0), .ra_w1(sa1), .ra_w2(sa2),
                .rb_w0(sb0), .rb_w1(sb1), .rb_w2(sb2),
                .struct_ok(struct_ok));
            assign src_ok = struct_ok[srcid];
            assign dst_ok = struct_ok[dstid];
        end else begin : g_live_mode
            wire src_ok_w, dst_ok_w;
            check_surf16 #(.PRECALC(0)) u_surf(
                .clk(clk), .rst(rst),
                .we(surf_we), .slot(surf_slot), .wsel(surf_wsel), .wdata(surf_wdata),
                .ra_id(srcid), .rb_id(dstid),
                .ra_w0(sa0), .ra_w1(sa1), .ra_w2(sa2),
                .rb_w0(sb0), .rb_w1(sb1), .rb_w2(sb2),
                .struct_ok(struct_ok));
            check_struct_ok u_ssrc(.s0(sa0), .s1(sa1), .s2(sa2), .ok(src_ok_w));
            check_struct_ok u_sdst(.s0(sb0), .s1(sb1), .s2(sb2), .ok(dst_ok_w));
            assign src_ok = src_ok_w;
            assign dst_ok = dst_ok_w;
        end
    endgenerate

    wire src_bad = is_blit && !src_ok;                // 源表面：BLIT 才查
    wire dst_bad = ((is_fill || is_blit) && !dst_ok); // 目的表面：FILL/BLIT 都查

    // space：每步源在前
    wire sp_src_bad = is_blit && (sa2[9:8] != 2'b00);
    wire sp_dst_bad = (is_fill || is_blit) && (sb2[9:8] != 2'b00);
    wire sp_bad     = sp_src_bad || sp_dst_bad;

    // format：BLIT 方程×源格式在前，目的 XRGB 在后
    wire fmt_src_ok  = (eq == 4'd1) ? (sa2[15:12] == 4'd0 || sa2[15:12] == 4'd1)
                     : (eq == 4'd5) ? (sa2[15:12] == 4'd0 || sa2[15:12] == 4'd2)
                     :                (sa2[15:12] <= 4'd2);
    wire fmt_src_bad = is_blit && !fmt_src_ok;
    wire fmt_dst_bad = (is_fill || is_blit) && (sb2[15:12] != 4'd0);
    wire fmt_bad     = fmt_src_bad || fmt_dst_bad;

    // 第九步：FILL && ALPHA 的预乘约束（RGB 不得大于 A；w4 = cmd_data[159:128]）
    wire [7:0] a_const = cmd_data[159:152];
    wire premult_bad = is_fill && (eq == 4'd1)
                    && (cmd_data[151:144] > a_const
                     || cmd_data[143:136] > a_const
                     || cmd_data[135:128] > a_const);

    // 第十步：同表面 BLIT 的矩形重叠（未裁剪半开区间；18 位有符号足够：
    // |坐标| < 2^15、宽高 < 2^16，|和| < 2^17）
    wire [15:0] ow = cmd_data[111:96];
    wire [15:0] oh = cmd_data[127:112];
    wire [17:0] sx18 = {{2{cmd_data[47]}}, cmd_data[47:32]};
    wire [17:0] sy18 = {{2{cmd_data[63]}}, cmd_data[63:48]};
    wire [17:0] dx18 = {{2{cmd_data[79]}}, cmd_data[79:64]};
    wire [17:0] dy18 = {{2{cmd_data[95]}}, cmd_data[95:80]};
    wire        ovl_bad = is_blit && (srcid == dstid)
                       && (ow != 16'd0) && (oh != 16'd0)
                       && ($signed(sx18 + {2'b00, ow}) > $signed(dx18))
                       && ($signed(dx18 + {2'b00, ow}) > $signed(sx18))
                       && ($signed(sy18 + {2'b00, oh}) > $signed(dy18))
                       && ($signed(dy18 + {2'b00, oh}) > $signed(sy18));

    generate
        if (MODE == 1) begin : g_fsm
            // ---- 结构 1：多拍状态机，每拍一步，首错锁存，固定 S_OP..S_OV 十拍 ----
            localparam [3:0] S_IDLE = 4'd0, S_OP = 4'd1, S_FL = 4'd2, S_EQ = 4'd3,
                             S_SS = 4'd4, S_SD = 4'd5, S_SP = 4'd6, S_FM = 4'd7,
                             S_PM = 4'd8, S_OV = 4'd9;
            reg [3:0] st;
            reg       have_err;
            wire [3:0] step_err = (st == S_OP) ? (op_ok       ? 4'd0 : 4'd1)
                                : (st == S_FL) ? (f_bad       ? 4'd2 : 4'd0)
                                : (st == S_EQ) ? (eq_bad      ? 4'd3 : 4'd0)
                                : (st == S_SS) ? (src_bad     ? 4'd4 : 4'd0)
                                : (st == S_SD) ? (dst_bad     ? 4'd4 : 4'd0)
                                : (st == S_SP) ? (sp_bad      ? 4'd5 : 4'd0)
                                : (st == S_FM) ? (fmt_bad     ? 4'd6 : 4'd0)
                                : (st == S_PM) ? (premult_bad ? 4'd8 : 4'd0)
                                :                (ovl_bad     ? 4'd7 : 4'd0);
            wire step_stop = (step_err != 4'd0);
            assign cmd_ready = (st == S_IDLE);

            always @(posedge clk) begin
                if (rst) begin
                    st <= S_IDLE; have_err <= 1'b0;
                    err_set <= 1'b0; check_ok <= 1'b0; err_code <= 4'd0;
                end else begin
                    err_set  <= 1'b0;
                    check_ok <= 1'b0;
                    case (st)
                        S_IDLE: if (cmd_valid) begin
                            st <= S_OP; have_err <= 1'b0;
                        end
                        S_OP, S_FL, S_EQ, S_SS, S_SD, S_SP, S_FM, S_PM: begin
                            if (!have_err && step_stop) begin
                                have_err <= 1'b1; err_code <= step_err;
                            end
                            st <= st + 4'd1;
                        end
                        default: begin // S_OV：出结果
                            if (!have_err) begin
                                if (step_stop) begin
                                    err_code <= step_err;
                                    err_set  <= 1'b1;
                                end else begin
                                    err_code <= 4'd0;
                                    check_ok <= 1'b1;
                                end
                            end else begin
                                err_set <= 1'b1;   // 首错已锁存，这一拍给出
                            end
                            st <= S_IDLE;
                        end
                    endcase
                end
            end
        end else begin : g_comb
            // ---- 结构 0 与 2：单拍组合，按顺序取首个 ----
            wire [4:0] verdict = !op_ok      ? {1'b1, 4'd1}
                               : f_bad       ? {1'b1, 4'd2}
                               : eq_bad      ? {1'b1, 4'd3}
                               : src_bad     ? {1'b1, 4'd4}
                               : dst_bad     ? {1'b1, 4'd4}
                               : sp_bad      ? {1'b1, 4'd5}
                               : fmt_bad     ? {1'b1, 4'd6}
                               : premult_bad ? {1'b1, 4'd8}
                               : ovl_bad     ? {1'b1, 4'd7}
                               :               {1'b0, 4'd0};
            assign cmd_ready = 1'b1;

            always @(posedge clk) begin
                if (rst) begin
                    err_set <= 1'b0; check_ok <= 1'b0; err_code <= 4'd0;
                end else begin
                    err_set  <= cmd_valid && verdict[4];
                    check_ok <= cmd_valid && !verdict[4];
                    err_code <= verdict[3:0];
                end
            end
        end
    endgenerate
endmodule

`default_nettype wire
