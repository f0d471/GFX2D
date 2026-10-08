// B4 formal RTL unit test: I5 sidebands + I3 read stream + I4 write stream.
`timescale 1ns/1ps
`default_nettype none
// 注错开关以 -DINJ_xxx 直接传给被测 RTL（预处理器级宏）；INJ_VAL 只为打印，
// 名字与 run_formal.sh 的用例一一对应。
`ifdef INJ_ROT_OFF
`define INJ_VAL 1
`elsif INJ_SKIP_KEY
`define INJ_VAL 2
`elsif INJ_DROP_MASK
`define INJ_VAL 4
`elsif INJ_EN_NOTREADY
`define INJ_VAL 8
`elsif INJ_CORE_NOEN
`define INJ_VAL 16
`else
`define INJ_VAL 0
`endif
module tb_engine_pix;
    reg clk=0, rst=1; always #5 clk=~clk;
    reg [1:0] half, sfmt, lead_src, lead_dst;
    reg is_blit, key_en, read_dst;
    reg [2:0] eq; reg [31:0] key_w5,w4; reg [7:0] c;
    reg [10:0] npx; reg [7:0] seg1_beats,seg2_beats;
    reg sb_valid=0; wire sb_ready; wire [1:0] row_free;
    reg [127:0] rs_tdata=0; reg [15:0] rs_tkeep=16'hffff;
    reg rs_tlast=0,rs_tvalid=0; reg [1:0] rs_buf=0; wire rs_tready;
    wire [127:0] ws_tdata; wire [15:0] ws_tkeep; wire ws_tvalid;
    reg [31:0] cycle_count=0; always @(posedge clk) cycle_count<=cycle_count+1;
`ifdef STALL
    wire ws_tready = (cycle_count % 7 != 0);
`else
    wire ws_tready = 1'b1;
`endif
    engine_pix dut (
        .clk(clk),.rst(rst),.sb_row({15'd0,half[0]}),.sb_npx(npx),
        .sb_beats(seg1_beats+seg2_beats),.sb_lead_dst(lead_dst),
        .sb_lead_src(lead_src),.sb_op(is_blit?8'd2:8'd1),
        .sb_eq({1'b0,eq}),.sb_key_en(key_en),.sb_key(key_w5),
        .sb_w4(w4),.sb_c(c),.sb_sfmt({2'b00,sfmt}),
        .sb_read_dst(read_dst),.sb_valid(sb_valid),.sb_ready(sb_ready),
        .row_free(row_free),.rs_tdata(rs_tdata),.rs_tkeep(rs_tkeep),
        .rs_tlast(rs_tlast),.rs_tvalid(rs_tvalid),.rs_tready(rs_tready),
        .rs_buf(rs_buf),.ws_tdata(ws_tdata),.ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid),.ws_tready(ws_tready));
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

    // FILL 的常量源字按方程解释（规范条款 §4.2，exec_sw.c 同款）：
    // ALPHA 按预乘、ALPHA_STRAIGHT 按直通，alpha 都取 w4[31:24]；其余方程不看 alpha
    function [1:0] fill_fmt;
        input [2:0] eqf;
        fill_fmt = (eqf == 3'd1) ? 2'd1 : (eqf == 3'd5) ? 2'd2 : 2'd0;
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
                        outw = blend_px(srcw, dstw, f_eq, f_c,
                                        f_is_blit ? f_sfmt : fill_fmt(f_eq));
                        exp_word[t][slot] = outw;
                    end
                end
        end
    endtask

    task feed_read;
        input bit destination;
        input integer beats;
        integer m,slot;
        begin
            for(m=0;m<beats;m=m+1) begin
                @(negedge clk);
                rs_tvalid=1'b1; rs_buf={destination,half[0]};
                rs_tlast=(m==beats-1)||(f_seg2!=0 && m==beats/2-1);
                for(slot=0;slot<4;slot=slot+1)
                    rs_tdata[slot*32 +: 32]=destination?dst_mem[4*m+slot]:src_mem[4*m+slot];
                if(rs_tready!==1'b1) begin bad=bad+1;$display("I3_NOT_READY");end
            end
            @(negedge clk); rs_tvalid=0; rs_tlast=0;
        end
    endtask

    task run_row;
        integer i,slot,got,beats_i,sbeats,dbeats,seen,gap,stall;
        reg [127:0] dw,held; reg holding;
        begin
            beats_i=(f_ld+f_npx+3)/4;
            build_expect;
            @(negedge clk);
            if(row_free[half[0]]!==1'b1 || sb_ready!==1'b1) begin
                bad=bad+1;$display("ROW_NOT_FREE");
            end
            is_blit=f_is_blit; eq=f_eq; key_en=f_key_en; key_w5=f_key;
            w4=f_w4;c=f_c;sfmt=f_sfmt;read_dst=f_read_dst;
            npx=f_npx;lead_src=f_ls;lead_dst=f_ld;
            seg1_beats=f_seg1;seg2_beats=f_seg2;sb_valid=1;
            @(negedge clk);sb_valid=0;
            if(row_free[half[0]]!==1'b0) begin bad=bad+1;$display("ROW_NOT_RESERVED");end
            sbeats=(f_ls+f_npx+3)/4;dbeats=beats_i;
            if(f_is_blit) feed_read(1'b0,sbeats);
            if(f_read_dst) feed_read(1'b1,dbeats);
            got=0;seen=0;gap=0;stall=0;holding=0;
            for(i=0;i<5000;i=i+1) begin
                @(posedge clk);
                if(holding && (ws_tdata!==held || ws_tvalid!==1'b1)) begin
                    bad=bad+1;if(bad<20)$display("STALL_DATA_CHANGED");
                end
                holding=ws_tvalid && !ws_tready;held=ws_tdata;
                if(ws_tvalid && !ws_tready) begin seen=1;stall=stall+1;end
                else if(ws_tvalid) begin
                    seen=1;dw=ws_tdata;
                    if(got>=beats_i) begin bad=bad+1;$display("EXTRA_BEAT"); end
                    else for(slot=0;slot<4;slot=slot+1) begin
                        checked=checked+1;
                        if(exp_nw[got][slot]) begin
                            if(dw[slot*32+31]!==1'b1) begin
                                bad=bad+1;if(bad<20)$display("MASK_MISMATCH n=%0d ls=%0d ld=%0d beat=%0d slot=%0d",f_npx,f_ls,f_ld,got,slot);
                            end
                        end else if(dw[slot*32 +:32]!==exp_word[got][slot]) begin
                            bad=bad+1;if(bad<20)$display("PIX_MISMATCH n=%0d ls=%0d ld=%0d eq=%0d beat=%0d slot=%0d got=%08x exp=%08x",f_npx,f_ls,f_ld,f_eq,got,slot,dw[slot*32 +:32],exp_word[got][slot]);
                        end
                    end
                    if(ws_tkeep!==16'hffff) begin bad=bad+1;$display("KEEP_MISMATCH");end
                    got=got+1;
                end else if(seen && got<beats_i) gap=1;
                if(got>=beats_i) i=5000;
            end
            if(got!=beats_i || gap) begin bad=bad+1;$display("BEATS_OR_GAP n=%0d got=%0d want=%0d gap=%0d",f_npx,got,beats_i,gap);end
            i=0;while(row_free[half[0]]!==1'b1 && i<100) begin @(negedge clk);i=i+1;end
            if(i>=100) begin bad=bad+1;$display("ROW_NOT_RELEASED");end
            rows_run=rows_run+1;
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
            f_read_dst = (eqf != 3'd0);                 // D37：非 COPY 才读目的
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
            if (f_npx >= 3)                             // 近似字：只有高字节不同，不能跳过
                src_mem[f_ls + (f_npx / 2) + 1] = {~f_key[31:24], f_key[23:0]};
            if (mode == 1) f_key = f_w4;                // FILL：色键字等于常量源字
            f_seg2 = 0;
            f_seg1 = (f_ld + f_npx + 3) / 4;
        end
    endtask

    integer n,ls_i,ld_i,k;
    initial begin
        bad=0;checked=0;rows_run=0;
        gap_err=0;len_err=0;last_err=0;keep_err=0;
        stall_head=0;stall_mid=0;rows_mid=0;
        half=0;sfmt=0;lead_src=0;lead_dst=0;is_blit=0;key_en=0;
        read_dst=0;eq=0;key_w5=0;w4=0;c=0;npx=0;seg1_beats=0;seg2_beats=0;
        repeat(3) @(negedge clk);rst=0;
        for(n=1;n<=25;n=n+1)
            for(ls_i=0;ls_i<4;ls_i=ls_i+1)
                for(ld_i=0;ld_i<4;ld_i=ld_i+1) begin
                    f_npx=n;f_ls=ls_i;f_ld=ld_i;half=(n+ls_i+ld_i)%2;
                    gen_row(0,(n+ls_i+ld_i)%6,(n+ls_i)%3,((n+ld_i)%3)!=0);
                    run_row;
                end
        $display("sweep done: %0d rows",rows_run);
        for(k=0;k<6;k=k+1) begin
            f_npx=3+k*5;f_ls=0;f_ld=(k*3)%4;half=k%2;
            // 送给被测的 sfmt 故意不按方程给（I1 上 FILL 没有源表面）：ALPHA 与
            // ALPHA_STRAIGHT 偏给 XRGB，被测必须不看它。色键位打开、色键字等于常量：
            // FILL 没有色键（spec 第 02 章条款 3），被测必须照写
            gen_row(1,k,(k==1||k==5)?0:k%3,1);run_row;
        end
        for(k=0;k<4;k=k+1) begin
            f_npx=21+k*3;f_ls=(k+1)%4;f_ld=(k+2)%4;half=k%2;
            gen_row(0,k+((k==3)?1:0),k%3,(k%2)!=0);
            f_seg1=(f_ld+f_npx+3)/4/2+1;
            f_seg2=(f_ld+f_npx+3)/4-f_seg1;run_row;
        end
        for(k=0;k<8;k=k+1) begin
            f_npx=(k<4)?253:640;f_ls=(k*3+1)%4;f_ld=k%4;half=k%2;
            gen_row(0,k%6,k%3,k[0]);run_row;
        end
        if(`INJ_VAL==0) begin
            if(bad==0)$display("RESULT: PASS (%0d rows, %0d slots checked)",rows_run,checked);
            else $display("RESULT: FAIL (%0d mismatches)",bad);
        end else if(bad>0)$display("RESULT: FAIL (inject %0d: %0d mismatches, as expected)",`INJ_VAL,bad);
        else $display("RESULT: NOT_RED (inject %0d)",`INJ_VAL);
        $finish;
    end
endmodule
`default_nettype wire
