// B1~B4 真实通路：几何层、计算阵列、搬运层与两个 DMA 核，接随机反压的 AXI RAM。
// 初始内存、表面表、命令表与期望内存由 tools/check/pix_cases_dump.c 经软件执行器导出。
// 测试台照解码之后的 I1 逐条提交，等上一条的写回全部完成（末行写状态）再提交下一条；
// 跑完把 512 KiB 存储逐 128 位对期望内存。另查写流一行之内不断拍、读流恒 ready、
// 没有总线错误与超时、WLAST 位置、行数对账。
//
// -DNOWAIT：几何层一空闲（busy 落下）就提交下一条，不等上一条的写回。B1~B4 里没有
// 跨命令的写后读保护：上一条命令最后一行的写还在路上，下一条命令就可能读到它的旧值。
// --hazard 那组用例在这种提交方式下必须见红（run_formal.sh 的 path_hazard_nowait）。
// -DCASE="build/xxx"：用例文件的前缀，默认 build/directed。
// -DTIMED：从机换成 E4 校准的定时模式（RD_LAT=23 RD_BUB=12 WR_BUB=6 B_LAT=1、不随机反压），
// 配 --sprites 那组用例量两种提交方式的总周期数（PERF 一行，从第一条提交到最后一条写回完成）。
`timescale 1ns/1ps
`default_nettype none
`ifndef INJ_VAL
`define INJ_VAL 0
`endif
`ifndef CASE
`define CASE "build/directed"
`endif
module tb_engine_path;
    reg clk=0, rst=1; always #5 clk=~clk;
    reg start=0;
    reg [7:0] op=0;
    reg [3:0] eq=0, sfmt=0, cidx=0;
    reg key_en=0;
    reg [31:0] key_w5=0, const_w4=0;
    reg [7:0] c=0;
    reg [31:0] src_base=0,dst_base=0;
    reg [15:0] src_w=0,src_h=0,dst_w=0,dst_h=0;
    reg [4:0] src_slog2=0,dst_slog2=0;
    reg signed [15:0] src_x=0,src_y=0,dst_x=0,dst_y=0;
    reg [15:0] width=0,height=0;
    wire geom_busy,cmd_done; wire [3:0] done_cidx;
    wire [1:0] row_free;
    wire [31:0] rd_addr,wr_addr;
    wire [11:0] rd_beats,wr_beats;
    wire [11:0] rd_bytes=rd_beats<<4,wr_bytes=wr_beats<<4;
    wire [1:0] rd_buf; wire [3:0] rd_cidx,wr_cidx;
    wire rd_valid,rd_ready,wr_valid,wr_ready,wr_last_row;
    wire rd_tex;
    wire [15:0] sb_row,sb_py;
    wire [10:0] sb_npx;wire [7:0] sb_beats,sb_op,sb_c;
    wire [1:0] sb_lead_dst,sb_lead_src;
    wire [3:0] sb_eq,sb_sfmt;
    wire sb_key_en,sb_read_dst,sb_valid,sb_ready;
    wire [31:0] sb_key,sb_w4;

    engine_geom u_geom(
        .clk(clk),.rst(rst),.start(start),.op(op),.eq(eq),
        .key_en(key_en),.key_w5(key_w5),.const_w4(const_w4),.c(c),
        .sfmt(sfmt),.src_base(src_base),.src_w(src_w),.src_h(src_h),
        .src_slog2(src_slog2),.src_tex(1'b0),.dst_base(dst_base),.dst_w(dst_w),
        .dst_h(dst_h),.dst_slog2(dst_slog2),.src_x(src_x),.src_y(src_y),
        .dst_x(dst_x),.dst_y(dst_y),.w(width),.h(height),
        .clip_x0(16'sd0),.clip_y0(16'sd0),
        .clip_x1(16'sd639),.clip_y1(16'sd479),.cidx(cidx),
        .row_free(row_free),.busy(geom_busy),.cmd_done(cmd_done),
        .done_cidx(done_cidx),
        .rd_addr(rd_addr),.rd_beats(rd_beats),.rd_buf(rd_buf),.rd_tex(rd_tex),
        .rd_cidx(rd_cidx),.rd_valid(rd_valid),.rd_ready(rd_ready),
        .wr_addr(wr_addr),.wr_beats(wr_beats),.wr_cidx(wr_cidx),
        .wr_last_row(wr_last_row),.wr_valid(wr_valid),.wr_ready(wr_ready),
        .sb_row(sb_row),.sb_py(sb_py),.sb_npx(sb_npx),.sb_beats(sb_beats),
        .sb_lead_dst(sb_lead_dst),.sb_lead_src(sb_lead_src),
        .sb_op(sb_op),.sb_eq(sb_eq),.sb_key_en(sb_key_en),
        .sb_key(sb_key),.sb_w4(sb_w4),.sb_c(sb_c),.sb_sfmt(sb_sfmt),
        .sb_read_dst(sb_read_dst),.sb_valid(sb_valid),.sb_ready(sb_ready));

    wire [127:0] rs_tdata,ws_tdata;
    wire [15:0] rs_tkeep,ws_tkeep;
    wire rs_tlast,rs_tvalid,rs_tready,ws_tvalid,ws_tready;
    wire [1:0] rs_buf;
    engine_pix #(.INJECT(`INJ_VAL)) u_pix(
        .clk(clk),.rst(rst),.sb_row(sb_row),.sb_npx(sb_npx),
        .sb_beats(sb_beats),.sb_lead_dst(sb_lead_dst),
        .sb_lead_src(sb_lead_src),.sb_op(sb_op),.sb_eq(sb_eq),
        .sb_key_en(sb_key_en),.sb_key(sb_key),.sb_w4(sb_w4),
        .sb_c(sb_c),.sb_sfmt(sb_sfmt),.sb_read_dst(sb_read_dst),
        .sb_valid(sb_valid),.sb_ready(sb_ready),.row_free(row_free),
        .rs_tdata(rs_tdata),.rs_tkeep(rs_tkeep),.rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid),.rs_tready(rs_tready),.rs_buf(rs_buf),
        .ws_tdata(ws_tdata),.ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid),.ws_tready(ws_tready));

    wire rd_stat_valid,rd_stat_err,wr_stat_valid,wr_stat_err,wr_stat_last_row;
    wire [3:0] rd_stat_cidx,wr_stat_cidx;
    wire [1:0] rd_stat_buf;
    wire drain_busy,drain_done,tmo_evt,tmo_dir;
    wire [31:0] araddr,awaddr;
    wire [7:0] arlen,awlen;
    wire [2:0] arsize,awsize;
    wire [1:0] arburst,awburst;
    wire arvalid,arready,awvalid,awready;
    wire [127:0] rdata,wdata;
    wire [1:0] rresp,bresp;
    wire rlast,rvalid,rready,wlast,wvalid,wready,bvalid,bready;
    wire [15:0] wstrb;
    engine_mover u_mover(
        .clk(clk),.rst(rst),
        .rd_addr(rd_addr),.rd_bytes(rd_bytes),.rd_buf(rd_buf),.rd_tex(rd_tex),
        .rd_cidx(rd_cidx),.rd_valid(rd_valid),.rd_ready(rd_ready),
        .tx_addr(),.tx_bytes(),.tx_buf(),.tx_cidx(),.tx_valid(),
        .tx_ready(1'b1),.tx_busy(1'b0),
        .tx_rs_tdata(128'd0),.tx_rs_tvalid(1'b0),.tx_rs_tlast(1'b0),.tx_rs_buf(2'd0),
        .tx_stat_valid(1'b0),.tx_stat_cidx(4'd0),.tx_stat_buf(2'd0),
        .fd_addr(32'd0),.fd_bytes(12'd0),.fd_cidx(4'd0),
        .fd_valid(1'b0),.fd_ready(),
        .rs_tdata(rs_tdata),.rs_tkeep(rs_tkeep),.rs_tlast(rs_tlast),
        .rs_tvalid(rs_tvalid),.rs_tready(rs_tready),.rs_buf(rs_buf),
        .fs_tdata(),.fs_tkeep(),.fs_tlast(),.fs_tvalid(),
        .fs_tready(1'b1),.fs_cidx(),
        .wr_addr(wr_addr),.wr_bytes(wr_bytes),.wr_cidx(wr_cidx),
        .wr_last_row(wr_last_row),.wr_valid(wr_valid),.wr_ready(wr_ready),
        .ws_tdata(ws_tdata),.ws_tkeep(ws_tkeep),
        .ws_tvalid(ws_tvalid),.ws_tready(ws_tready),
        .rd_stat_valid(rd_stat_valid),.rd_stat_err(rd_stat_err),
        .rd_stat_fetch(),
        .rd_stat_cidx(rd_stat_cidx),.rd_stat_buf(rd_stat_buf),
        .wr_stat_valid(wr_stat_valid),.wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx),.wr_stat_last_row(wr_stat_last_row),
        .drain_req(1'b0),.drain_busy(drain_busy),.drain_done(drain_done),
        .tmo_limit(13'd4096),.err_clr(1'b0),.tmo_evt(tmo_evt),
        .tmo_dir(tmo_dir),
        .m_axi_araddr(araddr),.m_axi_arlen(arlen),.m_axi_arsize(arsize),
        .m_axi_arburst(arburst),.m_axi_arvalid(arvalid),.m_axi_arready(arready),
        .m_axi_rdata(rdata),.m_axi_rresp(rresp),.m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid),.m_axi_rready(rready),
        .m_axi_awaddr(awaddr),.m_axi_awlen(awlen),.m_axi_awsize(awsize),
        .m_axi_awburst(awburst),.m_axi_awvalid(awvalid),.m_axi_awready(awready),
        .m_axi_wdata(wdata),.m_axi_wstrb(wstrb),.m_axi_wlast(wlast),
        .m_axi_wvalid(wvalid),.m_axi_wready(wready),.m_axi_bresp(bresp),
        .m_axi_bvalid(bvalid),.m_axi_bready(bready));

`ifdef TIMED
    axi_ram_model #(.MEM_WORDS(32768),.STRB_EN(1),.STALL(0),.TIMED(1),
                    .RD_LAT(23),.RD_BUB(12),.WR_BUB(6),.B_LAT(1)) u_ram(
`else
    axi_ram_model #(.MEM_WORDS(32768),.STRB_EN(1),.STALL(3),
                    .ADDR_STALL_MAX(3)) u_ram(
`endif
        .clk(clk),.rst_n(!rst),
        .awaddr(awaddr),.awlen(awlen),.awvalid(awvalid),.awready(awready),
        .wdata(wdata),.wstrb(wstrb),.wlast(wlast),.wvalid(wvalid),
        .wready(wready),.bresp(bresp),.bvalid(bvalid),.bready(bready),
        .araddr(araddr),.arlen(arlen),.arvalid(arvalid),.arready(arready),
        .rdata(rdata),.rresp(rresp),.rlast(rlast),.rvalid(rvalid),
        .rready(rready),.hold_aw(1'b0),.hold_ar(1'b0));

    reg [255:0] cmd_rom [0:63];
    reg [95:0] surf_rom [0:15];
    reg [127:0] expected [0:32767];
    reg [31:0] words [0:7];
    reg [95:0] ss,ds;
    integer done_count=0,desc_count=0,read_count=0,write_count=0;
    integer errors=0,stalls=0;
    integer row_beats [0:4095];
    integer row_in=0,row_out=0,remaining=0;
    always @(posedge clk) if(!rst) begin
        if(sb_valid && sb_ready) begin
            row_beats[row_in]=sb_beats;
            row_in=row_in+1;
        end
        if(ws_tvalid) begin
            if(remaining==0) begin
                if(row_out>=row_in) begin
                    errors=errors+1;$display("UNEXPECTED_WRITE_ROW");
                end else begin remaining=row_beats[row_out];row_out=row_out+1;end
            end
            if(ws_tready && remaining>0) remaining=remaining-1;
        end else if(remaining!=0) begin
            errors=errors+1;$display("WRITE_STREAM_GAP");
        end
        if(rd_stat_valid) begin
            read_count=read_count+1;
            if(rd_stat_err) begin errors=errors+1;$display("READ_ERROR");end
        end
        if(wr_stat_valid) begin
            write_count=write_count+1;
            if(wr_stat_last_row) done_count=done_count+1;
            if(wr_stat_err) begin errors=errors+1;$display("WRITE_ERROR");end
        end
        if(rs_tvalid && !rs_tready) begin errors=errors+1;$display("I3_BACKPRESSURE");end
        if(ws_tvalid && !ws_tready) stalls=stalls+1;
        if(tmo_evt) begin errors=errors+1;$display("TIMEOUT dir=%b",tmo_dir);end
        if(wr_valid && wr_ready) desc_count=desc_count+1;
    end

    task submit;
        input integer index;
        integer j,srcid,dstid;
        reg [31:0] w0;
        begin
            for(j=0;j<8;j=j+1) words[j]=cmd_rom[index][j*32 +:32];
            w0=words[0];srcid=w0[19:16];dstid=w0[23:20];
            ss=surf_rom[srcid];ds=surf_rom[dstid];
            @(negedge clk);
            op=w0[7:0];eq=w0[11:8];key_en=w0[12];c=w0[31:24];
            key_w5=words[5];const_w4=words[4];
            sfmt=ss[79:76];
            src_base=ss[31:0];src_w=ss[47:32];src_h=ss[63:48];
            src_slog2=ss[68:64];
            dst_base=ds[31:0];dst_w=ds[47:32];dst_h=ds[63:48];
            dst_slog2=ds[68:64];
            src_x=words[1][15:0];src_y=words[1][31:16];
            dst_x=words[2][15:0];dst_y=words[2][31:16];
            width=words[3][15:0];height=words[3][31:16];
            cidx=index[3:0];start=1;
            @(negedge clk);start=0;
        end
    endtask

    integer ci,cy,mi,mem_bad,expected_done,t_start;
    integer cycle=0;
    always @(posedge clk) cycle=cycle+1;
    initial begin
        $readmemh({`CASE,".cmd.hex"},cmd_rom);
        $readmemh({`CASE,".surf.hex"},surf_rom);
        $readmemh({`CASE,".exp.hex"},expected);
        #1 $readmemh({`CASE,".init.hex"},u_ram.mem);
        repeat(5) @(negedge clk);rst=0;
        expected_done=0;
        t_start=cycle;
        for(ci=0;ci<64;ci=ci+1) begin
            if(cmd_rom[ci][7:0]==8'hff) ci=64;
            else begin
                submit(ci);
                expected_done=expected_done+1;
                cy=0;
`ifdef NOWAIT
                while(geom_busy && cy<30000) begin @(negedge clk);cy=cy+1; end
`else
                while(done_count<expected_done && cy<30000) begin
                    @(negedge clk);cy=cy+1;
                end
`endif
                if(cy==30000) begin errors=errors+1;$display("CMD_TIMEOUT index=%0d",ci);ci=64;end
            end
        end
        cy=0;
        while(done_count<expected_done && cy<60000) begin @(negedge clk);cy=cy+1; end
        $display("PERF cycles=%0d commands=%0d",cycle-t_start,done_count);
        repeat(20) @(negedge clk);
        if(row_free!==2'b11) begin errors=errors+1;$display("ROWS_NOT_FREE %b",row_free);end
        if(row_in!=row_out || remaining!=0) begin
            errors=errors+1;$display("ROW_COUNT in=%0d out=%0d remaining=%0d",row_in,row_out,remaining);
        end
        if(u_ram.wlast_err!=0) begin errors=errors+1;$display("WLAST_ERROR %0d",u_ram.wlast_err);end
        mem_bad=0;
        for(mi=0;mi<32768;mi=mi+1) if(u_ram.mem[mi]!==expected[mi]) begin
            mem_bad=mem_bad+1;
            if(mem_bad<=20) $display("MEM_MISMATCH addr=%08x got=%032x want=%032x",mi*16,u_ram.mem[mi],expected[mi]);
        end
        errors=errors+mem_bad;
        $display("PATH stats commands=%0d read=%0d write=%0d stalls=%0d mem_bad=%0d errors=%0d",
                 done_count,read_count,write_count,stalls,mem_bad,errors);
        if(errors==0 && done_count==expected_done && read_count>0 && write_count>0)
            $display("RESULT: PASS (B1-B4, software executor golden)");
        else $display("RESULT: FAIL (B1-B4 integration)");
        $finish;
    end
endmodule
`default_nettype wire
